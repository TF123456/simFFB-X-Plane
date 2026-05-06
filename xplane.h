#pragma once

struct XPlaneTelemetry {
    float elevator_trim;       // sim/cockpit2/controls/elevator_trim  (-1..1)
    float aileron_trim;        // sim/cockpit2/controls/aileron_trim   (-1..1)
    float indicated_airspeed;  // sim/flightmodel/position/indicated_airspeed (kt)
    float stall_warning;       // sim/cockpit2/annunciators/stall_warning (0/1)
    float g_normal;            // sim/flightmodel/forces/g_nrml
    float on_ground;           // sim/flightmodel/failures/onground_any
    float elv_deflection;      // sim/flightmodel2/wing/elevator1_deg[8] (degrees)
    float ail_deflection;      // sim/flightmodel2/wing/aileron1_deg[0] (degrees)
    float tire_vrt_def[3];     // sim/flightmodel/parts/tire_vrt_def_veh[0..2] (metres)
    float vvi_fpm;             // sim/cockpit2/gauges/indicators/vvi_fpm_pilot (ft/min)
    float ground_speed_ms;     // sim/flightmodel/position/groundspeed (m/s)
    bool  connected;
};

void StartXPlaneListener(const char* xplaneIP, int xplanePort);
void StopXPlaneListener();
void GetTelemetry(XPlaneTelemetry* out);
