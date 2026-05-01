#include "stdafx.h"
#include "xplane.h"
#include <process.h>
#include <string.h>
#include <stdio.h>

#pragma comment(lib, "Ws2_32.lib")

static FILE* g_log = nullptr;
static void xlog(const char* fmt, ...)
{
    if (!g_log) return;
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fflush(g_log);
}

// RREF subscription packet sent to X-Plane
#pragma pack(push, 1)
struct RREFSub {
    char  header[5];   // "RREF\0"
    int   freq;
    int   refid;
    char  dref[400];
};

// Each value entry in an RREF response packet (after the 5-byte header)
struct RREFVal {
    int   refid;
    float value;
};
#pragma pack(pop)

// Datarefs to subscribe to
struct DrefDef { int id; int freq; const char* name; };
static const DrefDef k_drefs[] = {
    { 0, 30, "sim/cockpit2/controls/elevator_trim" },
    { 1, 30, "sim/cockpit2/controls/aileron_trim" },
    { 2, 30, "sim/flightmodel/position/indicated_airspeed" },
    { 3, 30, "sim/cockpit2/annunciators/stall_warning" },
    { 4, 30, "sim/flightmodel/forces/g_nrml" },
    { 5, 30, "sim/flightmodel/failures/onground_any" },
    { 6, 30, "sim/flightmodel2/wing/elevator1_deg[8]" },
    { 7, 30, "sim/flightmodel2/wing/aileron1_deg[0]" },
    { 8, 30, "sim/flightmodel/parts/tire_vrt_def_veh[0]" },
    { 9, 30, "sim/flightmodel/parts/tire_vrt_def_veh[1]" },
    {10, 30, "sim/flightmodel/parts/tire_vrt_def_veh[2]" },
    {11, 30, "sim/cockpit2/gauges/indicators/vvi_fpm_pilot" },
};
static const int k_ndefs = 12;

static SOCKET           g_sock       = INVALID_SOCKET;
static HANDLE           g_thread     = NULL;
static volatile bool    g_running    = false;
static CRITICAL_SECTION g_tele_cs;
static XPlaneTelemetry  g_tele       = {};
static DWORD            g_lastPacket = 0;
static sockaddr_in      g_xpAddr     = {};

static void SendSubscriptions(int freq)
{
    RREFSub pkt;
    memset(&pkt, 0, sizeof(pkt));
    memcpy(pkt.header, "RREF\0", 5);
    pkt.freq = freq;

    for (int i = 0; i < k_ndefs; i++) {
        pkt.refid = k_drefs[i].id;
        strncpy_s(pkt.dref, sizeof(pkt.dref), k_drefs[i].name, _TRUNCATE);
        int r = sendto(g_sock, (const char*)&pkt, sizeof(pkt), 0,
                       (sockaddr*)&g_xpAddr, sizeof(g_xpAddr));
        xlog("sendto dref=%s freq=%d -> %d (err=%d)\n",
             k_drefs[i].name, freq, r, WSAGetLastError());
        memset(pkt.dref, 0, sizeof(pkt.dref));
    }
}

static unsigned __stdcall ListenerThread(void*)
{
    char buf[4096];
    sockaddr_in from;
    int fromLen = sizeof(from);

    while (g_running) {
        fromLen = sizeof(from);
        int n = recvfrom(g_sock, buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
        if (n < 0) { xlog("recvfrom err=%d\n", WSAGetLastError()); continue; }
        xlog("recvfrom n=%d header=%.4s\n", n, buf);
        if (n < 5) continue;
        if (memcmp(buf, "RREF", 4) != 0) continue;
        xlog("RREF accepted n=%d 5thbyte=%d nvals=%d\n", n, (int)(unsigned char)buf[4], (n-5)/(int)sizeof(RREFVal));

        int nvals = (n - 5) / sizeof(RREFVal);
        const RREFVal* vals = reinterpret_cast<const RREFVal*>(buf + 5);

        EnterCriticalSection(&g_tele_cs);
        for (int i = 0; i < nvals; i++) {
            switch (vals[i].refid) {
                case 0: g_tele.elevator_trim      = vals[i].value; break;
                case 1: g_tele.aileron_trim        = vals[i].value; break;
                case 2: g_tele.indicated_airspeed  = vals[i].value; break;
                case 3: g_tele.stall_warning       = vals[i].value; break;
                case 4: g_tele.g_normal            = vals[i].value; break;
                case 5: g_tele.on_ground           = vals[i].value; break;
                case 6: g_tele.elv_deflection      = vals[i].value; break;
                case 7: g_tele.ail_deflection      = vals[i].value; break;
                case 8: g_tele.tire_vrt_def[0]     = vals[i].value; break;
                case 9: g_tele.tire_vrt_def[1]     = vals[i].value; break;
                case 10: g_tele.tire_vrt_def[2]    = vals[i].value; break;
                case 11: g_tele.vvi_fpm            = vals[i].value; break;
            }
        }
        g_tele.connected = true;
        g_lastPacket = GetTickCount();
        LeaveCriticalSection(&g_tele_cs);
    }
    return 0;
}

void StartXPlaneListener(const char* xplaneIP, int xplanePort)
{
    StopXPlaneListener();

    fopen_s(&g_log, "xplane_debug.log", "w");
    xlog("StartXPlaneListener ip=%s port=%d\n", xplaneIP, xplanePort);

    WSADATA wsd;
    int wsa = WSAStartup(MAKEWORD(2, 2), &wsd);
    xlog("WSAStartup=%d\n", wsa);

    InitializeCriticalSection(&g_tele_cs);
    memset(&g_tele, 0, sizeof(g_tele));
    g_lastPacket = 0;

    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    xlog("socket=%d (INVALID=%d)\n", (int)g_sock, (int)INVALID_SOCKET);
    if (g_sock == INVALID_SOCKET) { xlog("socket failed\n"); return; }

    // Bind to any local port; OS picks the ephemeral port
    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = 0;
    int br = bind(g_sock, (sockaddr*)&local, sizeof(local));
    // Log the port the OS assigned
    int llen = sizeof(local);
    getsockname(g_sock, (sockaddr*)&local, &llen);
    xlog("bind=%d local_port=%d\n", br, ntohs(local.sin_port));

    // Set receive timeout so the thread can check g_running periodically
    DWORD timeout = 1000;
    setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

    memset(&g_xpAddr, 0, sizeof(g_xpAddr));
    g_xpAddr.sin_family = AF_INET;
    g_xpAddr.sin_port   = htons((u_short)xplanePort);
    inet_pton(AF_INET, xplaneIP, &g_xpAddr.sin_addr);

    g_running = true;
    g_thread  = (HANDLE)_beginthreadex(NULL, 0, ListenerThread, NULL, 0, NULL);

    SendSubscriptions(30);
}

void StopXPlaneListener()
{
    if (!g_running && g_sock == INVALID_SOCKET) return;

    g_running = false;

    if (g_sock != INVALID_SOCKET) {
        SendSubscriptions(0);   // unsubscribe
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
    }

    if (g_thread) {
        WaitForSingleObject(g_thread, 2000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }

    DeleteCriticalSection(&g_tele_cs);
    WSACleanup();
}

void GetTelemetry(XPlaneTelemetry* out)
{
    EnterCriticalSection(&g_tele_cs);

    // Mark disconnected if no packet in 3 s
    if (g_tele.connected && GetTickCount() - g_lastPacket > 3000)
        g_tele.connected = false;

    *out = g_tele;
    LeaveCriticalSection(&g_tele_cs);
}
