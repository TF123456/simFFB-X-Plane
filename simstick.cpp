#include "stdafx.h"
#include "simstick.h"
#include <math.h>
#include <iostream>
#include <chrono>
#include <algorithm>

#define OPTFILENAME _T("opt.dat") // File name where settings are saved
#define MAGICNUMBER 145011981     // Bumped: added rollGain field

LPDIRECTINPUT8 g_pDI = NULL;
LPDIRECTINPUTDEVICE8 g_pFFDevice = NULL;

LPDIRECTINPUTEFFECT g_pEffectSpring = NULL;
LPDIRECTINPUTEFFECT g_pEffectDamper = NULL;
LPDIRECTINPUTEFFECT g_pEffectFricti = NULL;

LPDIRECTINPUTEFFECT g_pEffectSpring2 = NULL;
LPDIRECTINPUTEFFECT g_pEffectDamper2 = NULL;
LPDIRECTINPUTEFFECT g_pEffectFricti2 = NULL;

LPDIRECTINPUTEFFECT g_pEffectShaker = NULL;
LPDIRECTINPUTEFFECT g_pEffectBump = NULL;
LPDIRECTINPUTEFFECT g_pEffectRoll = NULL;

float g_xplaneSpeedScale = 1.0f;
static INT g_xplaneAeroY = 0; // IAS-scaled elevator deflection, combined with g_nYForce in spring
static INT g_xplaneAeroX = 0; // IAS-scaled aileron deflection

DWORD g_dwNumForceFeedbackAxis = 0;
INT g_nXForce;
INT g_nYForce;
BYTE g_bBoton[MAXBUTTONS];
DIJOYSTATE2 g_js;
BYTE g_bSpring = 1; // Wether there is spring force or not
int g_iNsticks = 0;
int g_iFF = -1;
BYTE old_btrimToggle_state = 0x00; // Keeps track of old trim toggle state
bool is_centered = true;           // Keeps track of joystick centering status to prevent unnecessary recentering
time_t time_last = INT_MAX;        // Used for debouncing trim toggle
sticks g_Sticks[MAXSTICKS];
stoptions g_Opt = {
    0,            // joystick
    0,            // joystick POV hat for progressive trimmer
    0,            // trim button (hold)
    0,            // trim button (toggle)
    0,            // trim button (center)
    5000,         // spring force
    3000,         // dampering level (force trim on)
    2000,         // friction level (force trim on)
    0,            // spring force 2
    5000,         // dampering level (force trim off)
    0,            // friction level (force trim off)
    0,            // key for reinitializing dinput
    {0, 0, 0, 0}, // modifier keys for init dinput key - CTRL/ALT/SHIFT/WIN
    0,            // key for cycling trim
    {0, 0, 0, 0}, // modifier keys for cycle trim key - CTRL/ALT/SHIFT/WIN
    0,            // x-coordinate of last window position
    0,            // y-coordinate of last window position
    true,         // swap axes
    1,            // no trim - only instant - only progressive - both
    1,            // spring force status
    "127.0.0.1",  // xplaneIP
    49000,        // xplanePort
    20,           // shakerGain
    20,           // bumpGain
    13,           // rollGain
    0,            // xplaneMode
};

// Private functions
BOOL CALLBACK EnumFFDevicesCallback(const DIDEVICEINSTANCE *pInst, VOID *pContext);
BOOL CALLBACK EnumAxesCallback(const DIDEVICEOBJECTINSTANCE *pdidoi, VOID *pContext);
HRESULT poll(LPDIRECTINPUTDEVICE8 Device, DIJOYSTATE2 *js);
HRESULT SetDeviceForcesXY();
HRESULT SetDeviceSpring();
HRESULT SetDeviceConditions();
void Botones(); // Poll buttons states
BOOL LoadOptionsFromFile();
BOOL SaveOptionsToFile();

HRESULT poll(LPDIRECTINPUTDEVICE8 Device, DIJOYSTATE2 *js)
{
    HRESULT hr;

    if (Device == NULL)
    {
        return S_OK;
    }

    // Poll the device to read the current state
    hr = Device->Poll();
    if (FAILED(hr))
    {
        // DInput is telling us that the input stream has been
        // interrupted. We aren't tracking any state between polls, so
        // we don't have any special reset that needs to be done. We
        // just re-acquire and try again.
        hr = Device->Acquire();
        while (hr == DIERR_INPUTLOST)
        {
            hr = Device->Acquire();
        }

        // If we encounter a fatal error, return failure.
        if ((hr == DIERR_INVALIDPARAM) || (hr == DIERR_NOTINITIALIZED))
        {
            return E_FAIL;
        }

        // If another application has control of this device, return successfully.
        // We'll just have to wait our turn to use the joystick.
        if (hr == DIERR_OTHERAPPHASPRIO)
        {
            return S_OK;
        }
    }

    // Get the input's device state
    if (FAILED(hr = Device->GetDeviceState(sizeof(DIJOYSTATE2), js)))
    {
        return hr; // The device should have been acquired during the Poll()
    }
    Botones();
    return S_OK;
}

//-----------------------------------------------------------------------------
// Name: InitDirectInput()
// Desc: Initialize the DirectInput variables.
//-----------------------------------------------------------------------------
HRESULT InitDirectInput(HWND hCon)
{
    DIPROPDWORD dipdw;
    HRESULT hr;

    // Register with the DirectInput subsystem and get a pointer
    // to a IDirectInput interface we can use.
    printf("[DI] DirectInput8Create...\n");
    if (FAILED(hr = DirectInput8Create(GetModuleHandle(NULL), DIRECTINPUT_VERSION,
                                       IID_IDirectInput8, (VOID **)&g_pDI, NULL)))
    {
        printf("[DI] DirectInput8Create FAILED hr=0x%08X\n", hr);
        return hr;
    }
    printf("[DI] DirectInput8Create OK\n");

    // Look for a force feedback device we can use
    printf("[DI] EnumDevices...\n");
    if (FAILED(hr = g_pDI->EnumDevices(DI8DEVCLASS_GAMECTRL,
                                       EnumFFDevicesCallback, NULL,
                                       DIEDFL_ATTACHEDONLY /*| DIEDFL_FORCEFEEDBACK*/)))
    {
        printf("[DI] EnumDevices FAILED hr=0x%08X\n", hr);
        return hr;
    }
    printf("[DI] EnumDevices done: g_iNsticks=%d, g_iFF=%d, g_pFFDevice=%p\n", g_iNsticks, g_iFF, g_pFFDevice);

    if (-1 == g_iNsticks)
    {
        printf("[DI] No joystick found\n");
        MessageBox(NULL, _T("No Joystick found."),
                   _T("FFConst"), MB_ICONERROR | MB_OK);
        return S_OK;
    }
    if (g_pFFDevice == NULL)
    {
        printf("[DI] No FFB joystick found\n");
        MessageBox(NULL, _T("No FFB Joystick found."),
                   _T("FFConst"), MB_ICONERROR | MB_OK);
        return S_OK;
    }
    printf("[DI] FFB device: stick index %d\n", g_iFF);

    // Have to load options again since we finally polled for devices and have the list
    LoadOptionsFromFile();

    // Set the data format to "simple joystick" - a predefined data format. A
    // data format specifies which controls on a device we are interested in,
    // and how they should be reported.
    //
    // This tells DirectInput that we will be passing a DIJOYSTATE structure to
    // IDirectInputDevice8::GetDeviceState(). Even though we won't actually do
    // it in this sample. But setting the data format is important so that the
    // DIJOFS_* values work properly.
    for (int i = 0; i < g_iNsticks; i++)
    {
        hr = g_Sticks[i].dev->SetDataFormat(&c_dfDIJoystick2);
        printf("[DI] stick[%d] SetDataFormat hr=0x%08X\n", i, hr);
        hr = g_Sticks[i].dev->SetCooperativeLevel(hCon, DISCL_EXCLUSIVE | DISCL_BACKGROUND);
        printf("[DI] stick[%d] SetCooperativeLevel hr=0x%08X\n", i, hr);
    }

    // Since we will be playing force feedback effects, we should disable the
    // auto-centering spring.
    dipdw.diph.dwSize = sizeof(DIPROPDWORD);
    dipdw.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    dipdw.diph.dwObj = 0;
    dipdw.diph.dwHow = DIPH_DEVICE;
    dipdw.dwData = FALSE;

    if (FAILED(hr = g_pFFDevice->SetProperty(DIPROP_AUTOCENTER, &dipdw.diph)))
    {
        printf("[DI] SetProperty(AUTOCENTER) FAILED hr=0x%08X\n", hr);
        return hr;
    }
    printf("[DI] SetProperty(AUTOCENTER) OK\n");

    // Enumerate and count the axes of the joystick
    if (FAILED(hr = g_pFFDevice->EnumObjects(EnumAxesCallback,
                                             (VOID *)&g_dwNumForceFeedbackAxis, DIDFT_AXIS)))
    {
        printf("[DI] EnumObjects(axes) FAILED hr=0x%08X\n", hr);
        return hr;
    }
    printf("[DI] FFB axes found: %d\n", g_dwNumForceFeedbackAxis);

    // This simple sample only supports one or two axis joysticks
    if (g_dwNumForceFeedbackAxis > 2)
        g_dwNumForceFeedbackAxis = 2;

    DWORD rgdwAxes[2] = {DIJOFS_X, DIJOFS_Y};
    LONG rglDirection[2] = {0, 0};
    int ax1, ax2;
    DICONDITION condition[2];
    ZeroMemory(&condition, sizeof(condition));

    if (g_Opt.swap)
    {
        ax1 = 0;
        ax2 = 1;
    }
    else
    {
        ax1 = 1;
        ax2 = 0;
    }

    DIEFFECT eff;
    ZeroMemory(&eff, sizeof(eff));
    eff.dwSize = sizeof(DIEFFECT);
    eff.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
    eff.dwDuration = INFINITE;
    eff.dwSamplePeriod = 0;
    eff.dwGain = DI_FFNOMINALMAX;
    eff.dwTriggerButton = DIEB_NOTRIGGER;
    eff.dwTriggerRepeatInterval = 0;
    eff.cAxes = g_dwNumForceFeedbackAxis;
    eff.rgdwAxes = rgdwAxes;
    eff.rglDirection = rglDirection;
    eff.lpEnvelope = 0;
    eff.cbTypeSpecificParams = sizeof(condition);
    eff.lpvTypeSpecificParams = &condition;
    eff.dwStartDelay = 0;

    // Create damper effect
    condition[ax1].lOffset = 0;
    condition[ax1].dwNegativeSaturation = g_Opt.damper;
    condition[ax1].dwPositiveSaturation = g_Opt.damper;
    condition[ax1].lDeadBand = 0;
    condition[ax1].lNegativeCoefficient = g_Opt.damper;
    condition[ax1].lPositiveCoefficient = g_Opt.damper;
    condition[ax2].lOffset = 0;
    condition[ax2].dwNegativeSaturation = g_Opt.damper;
    condition[ax2].dwPositiveSaturation = g_Opt.damper;
    condition[ax2].lDeadBand = 0;
    condition[ax2].lNegativeCoefficient = g_Opt.damper;
    condition[ax2].lPositiveCoefficient = g_Opt.damper;

    if (FAILED(hr = g_pFFDevice->CreateEffect(GUID_Damper, &eff, &g_pEffectDamper, NULL)))
    {
        printf("[DI] CreateEffect(Damper) FAILED hr=0x%08X\n", hr);
        return hr;
    }
    printf("[DI] CreateEffect(Damper) OK\n");
    if (g_pEffectDamper)
        g_pEffectDamper->Start(1, 0);

    // Create damper effect 2
    condition[ax1].lOffset = 0;
    condition[ax1].dwNegativeSaturation = g_Opt.damper2;
    condition[ax1].dwPositiveSaturation = g_Opt.damper2;
    condition[ax1].lDeadBand = 0;
    condition[ax1].lNegativeCoefficient = g_Opt.damper2;
    condition[ax1].lPositiveCoefficient = g_Opt.damper2;
    condition[ax2].lOffset = 0;
    condition[ax2].dwNegativeSaturation = g_Opt.damper2;
    condition[ax2].dwPositiveSaturation = g_Opt.damper2;
    condition[ax2].lDeadBand = 0;
    condition[ax2].lNegativeCoefficient = g_Opt.damper2;
    condition[ax2].lPositiveCoefficient = g_Opt.damper2;

    if (FAILED(hr = g_pFFDevice->CreateEffect(GUID_Damper, &eff, &g_pEffectDamper2, NULL)))
    {
        printf("[DI] CreateEffect(Damper2) FAILED hr=0x%08X\n", hr);
        return hr;
    }
    printf("[DI] CreateEffect(Damper2) OK\n");

    // Create Friction effect
    condition[ax1].dwNegativeSaturation = g_Opt.friction;
    condition[ax1].dwPositiveSaturation = g_Opt.friction;
    condition[ax1].lNegativeCoefficient = g_Opt.friction;
    condition[ax1].lPositiveCoefficient = g_Opt.friction;
    condition[ax2].dwNegativeSaturation = g_Opt.friction;
    condition[ax2].dwPositiveSaturation = g_Opt.friction;
    condition[ax2].lNegativeCoefficient = g_Opt.friction;
    condition[ax2].lPositiveCoefficient = g_Opt.friction;
    // eff.dwGain = g_Opt.friction;
    if (FAILED(hr = g_pFFDevice->CreateEffect(GUID_Friction, &eff, &g_pEffectFricti, NULL)))
    {
        printf("[DI] CreateEffect(Friction) FAILED hr=0x%08X\n", hr);
        return hr;
    }
    printf("[DI] CreateEffect(Friction) OK\n");
    if (g_pEffectFricti)
        g_pEffectFricti->Start(1, 0);

    // Create Friction effect 2
    condition[ax1].dwNegativeSaturation = g_Opt.friction2;
    condition[ax1].dwPositiveSaturation = g_Opt.friction2;
    condition[ax1].lNegativeCoefficient = g_Opt.friction2;
    condition[ax1].lPositiveCoefficient = g_Opt.friction2;
    condition[ax2].dwNegativeSaturation = g_Opt.friction2;
    condition[ax2].dwPositiveSaturation = g_Opt.friction2;
    condition[ax2].lNegativeCoefficient = g_Opt.friction2;
    condition[ax2].lPositiveCoefficient = g_Opt.friction2;
    // eff.dwGain = g_Opt.friction;
    if (FAILED(hr = g_pFFDevice->CreateEffect(GUID_Friction, &eff, &g_pEffectFricti2, NULL)))
    {
        printf("[DI] CreateEffect(Friction2) FAILED hr=0x%08X\n", hr);
        return hr;
    }
    printf("[DI] CreateEffect(Friction2) OK\n");

    // Create Spring effect
    condition[ax1].dwNegativeSaturation = g_Opt.spring;
    condition[ax1].dwPositiveSaturation = g_Opt.spring;
    condition[ax1].lNegativeCoefficient = g_Opt.spring;
    condition[ax1].lPositiveCoefficient = g_Opt.spring;
    condition[ax2].dwNegativeSaturation = g_Opt.spring;
    condition[ax2].dwPositiveSaturation = g_Opt.spring;
    condition[ax2].lNegativeCoefficient = g_Opt.spring;
    condition[ax2].lPositiveCoefficient = g_Opt.spring;
    // eff.dwGain = g_Opt.spring;
    if (FAILED(hr = g_pFFDevice->CreateEffect(GUID_Spring, &eff, &g_pEffectSpring, NULL)))
    {
        printf("[DI] CreateEffect(Spring) FAILED hr=0x%08X\n", hr);
        return hr;
    }
    printf("[DI] CreateEffect(Spring) OK\n");

    // Create Spring Effect 2
    condition[ax1].dwNegativeSaturation = g_Opt.spring2;
    condition[ax1].dwPositiveSaturation = g_Opt.spring2;
    condition[ax1].lNegativeCoefficient = g_Opt.spring2;
    condition[ax1].lPositiveCoefficient = g_Opt.spring2;
    condition[ax2].dwNegativeSaturation = g_Opt.spring2;
    condition[ax2].dwPositiveSaturation = g_Opt.spring2;
    condition[ax2].lNegativeCoefficient = g_Opt.spring2;
    condition[ax2].lPositiveCoefficient = g_Opt.spring2;

    if (FAILED(hr = g_pFFDevice->CreateEffect(GUID_Spring, &eff, &g_pEffectSpring2, NULL)))
    {
        printf("[DI] CreateEffect(Spring2) FAILED hr=0x%08X\n", hr);
        return hr;
    }
    printf("[DI] CreateEffect(Spring2) OK\n");

    if (NULL == g_pEffectSpring)
        return E_FAIL;
    if (NULL == g_pEffectSpring2)
        return E_FAIL;
    g_pEffectSpring->Start(1, 0);

    // Shaker effect: sine wave at 20 Hz, magnitude=0 (off until stall warning)
    DIPERIODIC per;
    ZeroMemory(&per, sizeof(per));
    per.dwMagnitude = 0;
    per.lOffset = 0;
    per.dwPhase = 0;
    per.dwPeriod = 50000; // microseconds = 20 Hz

    ZeroMemory(&eff, sizeof(eff));
    eff.dwSize = sizeof(DIEFFECT);
    eff.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
    eff.dwDuration = INFINITE;
    eff.dwSamplePeriod = 0;
    eff.dwGain = DI_FFNOMINALMAX;
    eff.dwTriggerButton = DIEB_NOTRIGGER;
    eff.dwTriggerRepeatInterval = 0;
    eff.cAxes = g_dwNumForceFeedbackAxis;
    eff.rgdwAxes = rgdwAxes;
    eff.rglDirection = rglDirection;
    eff.lpEnvelope = 0;
    eff.cbTypeSpecificParams = sizeof(DIPERIODIC);
    eff.lpvTypeSpecificParams = &per;
    eff.dwStartDelay = 0;
    g_pFFDevice->CreateEffect(GUID_Sine, &eff, &g_pEffectShaker, NULL);
    printf("[DI] CreateEffect(Shaker/Sine) g_pEffectShaker=%p\n", g_pEffectShaker);

    // Bump effect: sine wave at 8 Hz, magnitude=0 (off until on ground)
    per.dwPeriod = 125000; // microseconds = 8 Hz
    g_pFFDevice->CreateEffect(GUID_Sine, &eff, &g_pEffectBump, NULL);
    printf("[DI] CreateEffect(Bump/Sine) g_pEffectBump=%p\n", g_pEffectBump);

    // Roll effect: sine wave, magnitude=0, period set dynamically from ground speed
    per.dwPeriod = 200000; // default 5 Hz placeholder
    g_pFFDevice->CreateEffect(GUID_Sine, &eff, &g_pEffectRoll, NULL);
    printf("[DI] CreateEffect(Roll/Sine) g_pEffectRoll=%p\n", g_pEffectRoll);

    printf("[DI] InitDirectInput complete\n");
    return S_OK;
}

//-----------------------------------------------------------------------------
// Name: EnumAxesCallback()
// Desc: Callback function for enumerating the axes on a joystick and counting
//       each force feedback enabled axis
//-----------------------------------------------------------------------------
BOOL CALLBACK EnumAxesCallback(const DIDEVICEOBJECTINSTANCE *pdidoi,
                               VOID *pContext)
{
    DWORD *pdwNumForceFeedbackAxis = (DWORD *)pContext;

    if ((pdidoi->dwFlags & DIDOI_FFACTUATOR) != 0)
        (*pdwNumForceFeedbackAxis)++;

    return DIENUM_CONTINUE;
}

//-----------------------------------------------------------------------------
// Name: EnumFFDevicesCallback()
// Desc: Called once for each enumerated force feedback device. If we find
//       one, create a device interface on it so we can play with it.
//-----------------------------------------------------------------------------
BOOL CALLBACK EnumFFDevicesCallback(const DIDEVICEINSTANCE *pInst,
                                    VOID *pContext)
{
    LPDIRECTINPUTDEVICE8 pDevice;
    HRESULT hr;

    printf("[DI] EnumFFDevicesCallback: '%ls' guidFFDriver.Data1=0x%08X\n",
           pInst->tszInstanceName, pInst->guidFFDriver.Data1);

    // Obtain an interface to the enumerated force feedback device.
    hr = g_pDI->CreateDevice(pInst->guidInstance, &pDevice, NULL);

    // If it failed, then we can't use this device for some
    // bizarre reason.  (Maybe the user unplugged it while we
    // were in the middle of enumerating it.)  So continue enumerating
    if (FAILED(hr))
    {
        printf("[DI]   CreateDevice FAILED hr=0x%08X, skipping\n", hr);
        return DIENUM_CONTINUE;
    }

    // We successfully created an IDirectInputDevice8.
    _tcscpy_s(g_Sticks[g_iNsticks].name, MAX_PATH, pInst->tszInstanceName);
    g_Sticks[g_iNsticks].dev = pDevice;
    if (pInst->guidFFDriver.Data1 != NULL && g_pFFDevice == NULL &&
        _tcsstr(pInst->tszInstanceName, _T("vJoy")) == NULL)
    {
        g_iFF = g_iNsticks;
        g_pFFDevice = pDevice;
        printf("[DI]   -> assigned as FFB device (stick index %d)\n", g_iNsticks);
    }
    else if (pInst->guidFFDriver.Data1 != NULL)
    {
        printf("[DI]   -> has FFB driver but skipping (vJoy or FFB device already assigned)\n");
    }
    g_iNsticks++;
    if (32 == g_iNsticks)
        return DIENUM_STOP;
    return DIENUM_CONTINUE;
}

//-----------------------------------------------------------------------------
// Name: FreeDirectInput()
// Desc: Initialize the DirectInput variables.
//-----------------------------------------------------------------------------
VOID FreeDirectInput()
{
    // Unacquire the device one last time just in case
    // the app tried to exit while the device is still acquired.
    for (int i = 0; i < g_iNsticks; i++)
        if (g_Sticks[i].dev)
            g_Sticks[i].dev->Unacquire();

    // Release any DirectInput objects.
    SAFE_RELEASE(g_pEffectSpring);
    SAFE_RELEASE(g_pEffectDamper);
    SAFE_RELEASE(g_pEffectFricti);
    SAFE_RELEASE(g_pEffectSpring2);
    SAFE_RELEASE(g_pEffectDamper2);
    SAFE_RELEASE(g_pEffectFricti2);
    SAFE_RELEASE(g_pEffectShaker);
    SAFE_RELEASE(g_pEffectBump);
    SAFE_RELEASE(g_pEffectRoll);

    for (int i = 0; i < g_iNsticks; i++)
        SAFE_RELEASE(g_Sticks[i].dev);
    // SAFE_RELEASE( g_pFFDevice );
    SAFE_RELEASE(g_pDI);

    g_iNsticks = 0;
    g_iFF = -1;
    g_nXForce = 0;
    g_nYForce = 0;

    SaveOptionsToFile();
}

//-----------------------------------------------------------------------------
// Name: SetDeviceForcesXY()
// Desc: Apply the X and Y forces to the effect we prepared.
//-----------------------------------------------------------------------------
HRESULT SetDeviceForcesXY() // This function is part of the original sample but not used in this application
{
    // Modifying an effect is basically the same as creating a new one, except
    // you need only specify the parameters you are modifying
    LONG rglDirection[2] = {0, 0};

    DICONSTANTFORCE cf;

    if (g_dwNumForceFeedbackAxis == 1)
    {
        // If only one force feedback axis, then apply only one direction and
        // keep the direction at zero
        cf.lMagnitude = g_nXForce;
        rglDirection[0] = 0;
    }
    else
    {
        // If two force feedback axis, then apply magnitude from both directions
        rglDirection[0] = g_nXForce;
        rglDirection[1] = g_nYForce;
        cf.lMagnitude = (DWORD)sqrt((double)g_nXForce * (double)g_nXForce +
                                    (double)g_nYForce * (double)g_nYForce);
    }

    DIEFFECT eff;
    ZeroMemory(&eff, sizeof(eff));
    eff.dwSize = sizeof(DIEFFECT);
    eff.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
    eff.cAxes = g_dwNumForceFeedbackAxis;
    eff.rglDirection = rglDirection;
    eff.lpEnvelope = 0;
    eff.cbTypeSpecificParams = sizeof(DICONSTANTFORCE);
    eff.lpvTypeSpecificParams = &cf;
    eff.dwStartDelay = 0;

    // Now set the new parameters and start the effect immediately.
    return g_pEffectSpring->SetParameters(&eff, DIEP_DIRECTION |
                                                    DIEP_TYPESPECIFICPARAMS |
                                                    DIEP_START);
}

HRESULT SetDeviceConditions()
{
    LONG rglDirection[2] = {0, 0};
    DWORD rgdwAxes[2] = {DIJOFS_X, DIJOFS_Y};
    int ax1, ax2;
    DICONDITION condition[2];
    ZeroMemory(&condition, sizeof(condition));

    if (g_Opt.swap)
    {
        ax1 = 0;
        ax2 = 1;
    }
    else
    {
        ax1 = 1;
        ax2 = 0;
    }
    SetDeviceSpring();

    DIEFFECT eff;
    ZeroMemory(&eff, sizeof(eff));
    eff.dwSize = sizeof(DIEFFECT);
    eff.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
    eff.dwDuration = INFINITE;
    eff.dwSamplePeriod = 0;
    eff.dwGain = DI_FFNOMINALMAX;
    eff.dwTriggerButton = DIEB_NOTRIGGER;
    eff.dwTriggerRepeatInterval = 0;
    eff.cAxes = g_dwNumForceFeedbackAxis;
    eff.rgdwAxes = rgdwAxes;
    eff.rglDirection = rglDirection;
    eff.lpEnvelope = 0;
    eff.cbTypeSpecificParams = sizeof(condition);
    eff.lpvTypeSpecificParams = condition;
    eff.dwStartDelay = 0;

    // Friction
    condition[ax1].lOffset = 0;
    condition[ax1].dwNegativeSaturation = g_Opt.friction;
    condition[ax1].dwPositiveSaturation = g_Opt.friction;
    condition[ax1].lDeadBand = 0;
    condition[ax1].lNegativeCoefficient = g_Opt.friction;
    condition[ax1].lPositiveCoefficient = g_Opt.friction;
    condition[ax2].lOffset = 0;
    condition[ax2].dwNegativeSaturation = g_Opt.friction;
    condition[ax2].dwPositiveSaturation = g_Opt.friction;
    condition[ax2].lDeadBand = 0;
    condition[ax2].lNegativeCoefficient = g_Opt.friction;
    condition[ax2].lPositiveCoefficient = g_Opt.friction;

    if (g_pEffectFricti)
        g_pEffectFricti->SetParameters(&eff, DIEP_TYPESPECIFICPARAMS);

    // Friction 2
    condition[ax1].lOffset = 0;
    condition[ax1].dwNegativeSaturation = g_Opt.friction2;
    condition[ax1].dwPositiveSaturation = g_Opt.friction2;
    condition[ax1].lDeadBand = 0;
    condition[ax1].lNegativeCoefficient = g_Opt.friction2;
    condition[ax1].lPositiveCoefficient = g_Opt.friction2;
    condition[ax2].lOffset = 0;
    condition[ax2].dwNegativeSaturation = g_Opt.friction2;
    condition[ax2].dwPositiveSaturation = g_Opt.friction2;
    condition[ax2].lDeadBand = 0;
    condition[ax2].lNegativeCoefficient = g_Opt.friction2;
    condition[ax2].lPositiveCoefficient = g_Opt.friction2;

    if (g_pEffectFricti2)
        g_pEffectFricti2->SetParameters(&eff, DIEP_TYPESPECIFICPARAMS);

    // Damper 1
    condition[ax1].lOffset = 0;
    condition[ax1].dwNegativeSaturation = g_Opt.damper;
    condition[ax1].dwPositiveSaturation = g_Opt.damper;
    condition[ax1].lDeadBand = 0;
    condition[ax1].lNegativeCoefficient = g_Opt.damper;
    condition[ax1].lPositiveCoefficient = g_Opt.damper;
    condition[ax2].lOffset = 0;
    condition[ax2].dwNegativeSaturation = g_Opt.damper;
    condition[ax2].dwPositiveSaturation = g_Opt.damper;
    condition[ax2].lDeadBand = 0;
    condition[ax2].lNegativeCoefficient = g_Opt.damper;
    condition[ax2].lPositiveCoefficient = g_Opt.damper;

    if (g_pEffectDamper)
        g_pEffectDamper->SetParameters(&eff, DIEP_TYPESPECIFICPARAMS);

    // Damper 2
    condition[ax1].lOffset = 0;
    condition[ax1].dwNegativeSaturation = g_Opt.damper2;
    condition[ax1].dwPositiveSaturation = g_Opt.damper2;
    condition[ax1].lDeadBand = 0;
    condition[ax1].lNegativeCoefficient = g_Opt.damper2;
    condition[ax1].lPositiveCoefficient = g_Opt.damper2;
    condition[ax2].lOffset = 0;
    condition[ax2].dwNegativeSaturation = g_Opt.damper2;
    condition[ax2].dwPositiveSaturation = g_Opt.damper2;
    condition[ax2].lDeadBand = 0;
    condition[ax2].lNegativeCoefficient = g_Opt.damper2;
    condition[ax2].lPositiveCoefficient = g_Opt.damper2;

    if (g_pEffectDamper2)
        g_pEffectDamper2->SetParameters(&eff, DIEP_TYPESPECIFICPARAMS);

    return 0;
}

HRESULT SetDeviceSpring()
{
    LONG rglDirection[2] = {0, 0};
    DWORD rgdwAxes[2] = {DIJOFS_X, DIJOFS_Y};
    int ax1, ax2;

    DICONDITION condition[2];
    ZeroMemory(condition, sizeof(condition));

    if (g_Opt.swap)
    {
        ax1 = 0;
        ax2 = 1;
    }
    else
    {
        ax1 = 1;
        ax2 = 0;
    }

    DIEFFECT eff;
    ZeroMemory(&eff, sizeof(eff));
    eff.dwSize = sizeof(DIEFFECT);
    eff.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
    eff.dwDuration = INFINITE;
    eff.dwSamplePeriod = 0;
    eff.dwGain = DI_FFNOMINALMAX;
    eff.dwTriggerButton = DIEB_NOTRIGGER;
    eff.dwTriggerRepeatInterval = 0;
    eff.cAxes = g_dwNumForceFeedbackAxis;
    eff.rgdwAxes = rgdwAxes;
    eff.rglDirection = rglDirection;
    eff.lpEnvelope = 0;
    eff.cbTypeSpecificParams = sizeof(condition);
    eff.lpvTypeSpecificParams = condition;
    eff.dwStartDelay = 0;

    int scaledSpring = g_Opt.spring;

    // In X-Plane mode: lOffset = aero component (IAS-scaled deflection) + internal trim (g_nYForce).
    // In normal mode: lOffset = internal trim only.
    INT lOY, lOX;
    if (g_Opt.xplaneMode)
    {
        INT scaledTrimY = (INT)(g_nYForce * 0.5f * g_xplaneSpeedScale * g_xplaneSpeedScale);
        INT scaledTrimX = (INT)(g_nXForce * 0.5f * g_xplaneSpeedScale * g_xplaneSpeedScale);
        lOY = max(-(INT)DI_FFNOMINALMAX, min(g_xplaneAeroY + 2.0f * scaledTrimY, (INT)DI_FFNOMINALMAX));
        INT lOX_raw = max(-(INT)DI_FFNOMINALMAX, min(g_xplaneAeroX + scaledTrimX, (INT)DI_FFNOMINALMAX));
        lOX = g_Opt.swap ? -lOX_raw : lOX_raw;
        printf("lOY: %d, lOX: %d          \r", lOY, lOX);
    }
    else
    {
        lOY = g_nYForce;
        lOX = g_Opt.swap ? -g_nXForce : g_nXForce;
    }

    if (g_Opt.spring == 0)
    { // Fixes problematic behavior with G940
        condition[ax1].lOffset = lOY;
        condition[ax1].dwNegativeSaturation = 100;
        condition[ax1].dwPositiveSaturation = 100;
        condition[ax1].lDeadBand = 0;
        condition[ax1].lNegativeCoefficient = 100;
        condition[ax1].lPositiveCoefficient = 100;

        condition[ax2].lOffset = lOX;
        condition[ax2].dwNegativeSaturation = 100;
        condition[ax2].dwPositiveSaturation = 100;
        condition[ax2].lDeadBand = 0;
        condition[ax2].lNegativeCoefficient = 100;
        condition[ax2].lPositiveCoefficient = 100;
    }
    else
    {
        condition[ax1].lOffset = lOY;
        condition[ax1].dwNegativeSaturation = scaledSpring;
        condition[ax1].dwPositiveSaturation = scaledSpring;
        condition[ax1].lDeadBand = 0;
        condition[ax1].lNegativeCoefficient = scaledSpring;
        condition[ax1].lPositiveCoefficient = scaledSpring;

        condition[ax2].lOffset = lOX;
        condition[ax2].dwNegativeSaturation = scaledSpring;
        condition[ax2].dwPositiveSaturation = scaledSpring;
        condition[ax2].lDeadBand = 0;
        condition[ax2].lNegativeCoefficient = scaledSpring;
        condition[ax2].lPositiveCoefficient = scaledSpring;
    }

    if (g_pEffectSpring)
        g_pEffectSpring->SetParameters(&eff, DIEP_TYPESPECIFICPARAMS);

    // Spring 2
    if (g_Opt.spring2 == 0)
    { // Fixes problematic behavior with G940
        condition[ax1].lOffset = lOY;
        condition[ax1].dwNegativeSaturation = 100;
        condition[ax1].dwPositiveSaturation = 100;
        condition[ax1].lDeadBand = 0;
        condition[ax1].lNegativeCoefficient = 100;
        condition[ax1].lPositiveCoefficient = 100;

        condition[ax2].lOffset = lOX;
        condition[ax2].dwNegativeSaturation = 100;
        condition[ax2].dwPositiveSaturation = 100;
        condition[ax2].lDeadBand = 0;
        condition[ax2].lNegativeCoefficient = 100;
        condition[ax2].lPositiveCoefficient = 100;
    }
    else
    {
        condition[ax1].lOffset = lOY;
        condition[ax1].dwNegativeSaturation = g_Opt.spring2;
        condition[ax1].dwPositiveSaturation = g_Opt.spring2;
        condition[ax1].lDeadBand = 0;
        condition[ax1].lNegativeCoefficient = g_Opt.spring2;
        condition[ax1].lPositiveCoefficient = g_Opt.spring2;

        condition[ax2].lOffset = lOX;
        condition[ax2].dwNegativeSaturation = g_Opt.spring2;
        condition[ax2].dwPositiveSaturation = g_Opt.spring2;
        condition[ax2].lDeadBand = 0;
        condition[ax2].lNegativeCoefficient = g_Opt.spring2;
        condition[ax2].lPositiveCoefficient = g_Opt.spring2;
    }

    if (g_pEffectSpring2)
        g_pEffectSpring2->SetParameters(&eff, DIEP_TYPESPECIFICPARAMS);

    // Set force profile
    if (g_Opt.g_bSpring == 1)
    {
        if (g_pEffectSpring2)
            g_pEffectSpring2->Stop();
        if (g_pEffectSpring)
            g_pEffectSpring->Start(1, 0);
        if (g_pEffectDamper2)
            g_pEffectDamper2->Stop();
        if (g_pEffectDamper)
            g_pEffectDamper->Start(1, 0);
        if (g_pEffectFricti2)
            g_pEffectFricti2->Stop();
        if (g_pEffectFricti)
            g_pEffectFricti->Start(1, 0);
    }
    else
    {
        g_pEffectSpring2->SetParameters(&eff, DIEP_TYPESPECIFICPARAMS);
        if (g_pEffectSpring)
            g_pEffectSpring->Stop();
        if (g_pEffectSpring2)
            g_pEffectSpring2->Start(1, 0);
        if (g_pEffectDamper)
            g_pEffectDamper->Stop();
        if (g_pEffectDamper2)
            g_pEffectDamper2->Start(1, 0);
        if (g_pEffectFricti)
            g_pEffectFricti->Stop();
        if (g_pEffectFricti2)
            g_pEffectFricti2->Start(1, 0);
    }
    return 0;
}

void Botones()
{
    using namespace std::chrono;

    // Handle force trim toggle
    int i = g_Opt.btrimToggle;
    BYTE new_btrimToggle_state = g_js.rgbButtons[i];
    // debounce
    auto ms = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    if (new_btrimToggle_state == 0x80 && old_btrimToggle_state == 0x00 && ms - time_last > 200)
    {
        g_bBoton[i] = (g_bBoton[i] == DOWN) ? UP : DOWN;
        time_last = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    }
    old_btrimToggle_state = new_btrimToggle_state;

    // handle other buttons
    for (int i = 0; i < MAXBUTTONS; i++)
    {
        if (i != g_Opt.btrimToggle)
        {
            if (g_js.rgbButtons[i] == 0x80)
            {
                g_bBoton[i] = DOWN;
            }
            else
            {
                if (g_bBoton[i] == DOWN)
                {
                    g_bBoton[i] = RELEASED;
                }
                else
                {
                    g_bBoton[i] = UP;
                }
            }
        }
    }
}

HRESULT Adquirir()
{
    HRESULT hr;
    for (int i = 0; i < g_iNsticks; i++)
    {
        hr = g_Sticks[i].dev->Acquire();
        printf("[DI] Acquire stick[%d] ('%ls') hr=0x%08X\n", i, g_Sticks[i].name, hr);
    }
    return 0;
}

void StartEffects()
{
    Sleep(100);
    if (g_pEffectSpring)
        g_pEffectSpring->Start(1, 0);
    if (g_pEffectDamper)
        g_pEffectDamper->Start(1, 0);
    if (g_pEffectFricti)
        g_pEffectFricti->Start(1, 0);
}

void StopEffects()
{
    Sleep(100);
    if (g_pEffectSpring)
        g_pEffectSpring->Stop();
    if (g_pEffectDamper)
        g_pEffectDamper->Stop();
    if (g_pEffectFricti)
        g_pEffectFricti->Stop();
    if (g_pEffectSpring2)
        g_pEffectSpring2->Stop();
    if (g_pEffectDamper2)
        g_pEffectDamper2->Stop();
    if (g_pEffectFricti2)
        g_pEffectFricti2->Stop();
}

void JoystickStuffIT() // IT -> instantaneous trimming
{
    if (FAILED(poll(g_Sticks[g_Opt.jtrim].dev, &g_js)))
        return;

    // Handle force trim toggle
    if (g_bBoton[g_Opt.btrimToggle] == DOWN && (g_Opt.g_bSpring))
    { // Switch to spring force 2 if force trim toggle button was pressed
        g_Opt.g_bSpring = 0;
        is_centered = false;
        SetDeviceSpring();
    }
    else if (g_bBoton[g_Opt.btrimToggle] == UP && !is_centered)
    { // Update spring center if trim toggle button pressed & joystick un-centered
        g_Opt.g_bSpring = 1;
        InstantTrim();
        is_centered = true;
    }

    // Handle center trim
    if (g_bBoton[g_Opt.btrimCenter] == DOWN && (g_Opt.g_bSpring))
    { // Trim center
        CenterTrim();
    }

    // Handle hold trim
    if ((g_bBoton[g_Opt.btrimHold] == DOWN) && (g_Opt.g_bSpring))
    { // Switch to spring force 2 if trim button is down
        g_Opt.g_bSpring = 0;
        SetDeviceSpring();
    }
    else if (g_bBoton[g_Opt.btrimHold] == RELEASED)
    { // Update spring center if trim button is released
        g_Opt.g_bSpring = 1;
        InstantTrim();
    }
}

void ApplyArrowKeyTrim()
{
    if (0 == g_Opt.g_bSpring)
        return;
    bool any = false;
    if (GetAsyncKeyState(VK_UP) & 0x8000)
    {
        SUBLIM(g_nYForce, TRIMSTEP, -DI_FFNOMINALMAX);
        any = true;
    }
    if (GetAsyncKeyState(VK_DOWN) & 0x8000)
    {
        ADDLIM(g_nYForce, TRIMSTEP, DI_FFNOMINALMAX);
        any = true;
    }
    if (GetAsyncKeyState(VK_RIGHT) & 0x8000)
    {
        ADDLIM(g_nXForce, TRIMSTEP, DI_FFNOMINALMAX);
        any = true;
    }
    if (GetAsyncKeyState(VK_LEFT) & 0x8000)
    {
        SUBLIM(g_nXForce, TRIMSTEP, -DI_FFNOMINALMAX);
        any = true;
    }
    if (any)
        SetDeviceSpring();
}

void JoystickStuffPT() // PT -> progressive trimming
{

    if (0 == g_Opt.g_bSpring)
        return; // Do nothing if the trimming button is down
    if (FAILED(poll(g_Sticks[g_Opt.jPOV].dev, &g_js)))
        return;
    switch (g_js.rgdwPOV[0])
    {       // To do: add diagonals for simultaneous directions
    case 0: // Direction away from the user
        SUBLIM(g_nYForce, TRIMSTEP, -DI_FFNOMINALMAX);
        break;
    case 18000: // Direction TO THE USER
        ADDLIM(g_nYForce, TRIMSTEP, DI_FFNOMINALMAX);
        break;
    case 9000: // Direction right
        ADDLIM(g_nXForce, TRIMSTEP, DI_FFNOMINALMAX);
        break;
    case 27000: // Direction left
        SUBLIM(g_nXForce, TRIMSTEP, -DI_FFNOMINALMAX);
        break;
    case 4500:
        SUBLIM(g_nYForce, TRIMSTEP, -DI_FFNOMINALMAX);
        ADDLIM(g_nXForce, TRIMSTEP, DI_FFNOMINALMAX);
        break;
    case 13500:
        ADDLIM(g_nYForce, TRIMSTEP, DI_FFNOMINALMAX);
        ADDLIM(g_nXForce, TRIMSTEP, DI_FFNOMINALMAX);
        break;
    case 22500:
        ADDLIM(g_nYForce, TRIMSTEP, DI_FFNOMINALMAX);
        SUBLIM(g_nXForce, TRIMSTEP, -DI_FFNOMINALMAX);
        break;
    case 31500:
        SUBLIM(g_nYForce, TRIMSTEP, -DI_FFNOMINALMAX);
        SUBLIM(g_nXForce, TRIMSTEP, -DI_FFNOMINALMAX);
        break;
    }
    SetDeviceSpring();
}

void InstantTrim()
{
    if (FAILED(poll(g_pFFDevice, &g_js)))
        return;

    // g_nXForce = ((double)g_js.lX / 65535) * 20000 - 10000;  //Range has to be mapped from -65535,65535 to -10000,10000
    // g_nYForce = ((double)g_js.lY / 65535) * 20000 - 10000;
    g_nXForce = (INT)((((double)(g_js.lX) / 65535.0) * 20000.0) - 10000.0); // Range has to be mapped from -65535,65535 to -10000,10000
    g_nYForce = (INT)((((double)(g_js.lY) / 65535.0) * 20000.0) - 10000.0);
    SetDeviceSpring();
}

// Center joystick and turn on spring
void CenterTrim()
{
    if (FAILED(poll(g_pFFDevice, &g_js)))
        return;

    while (g_nXForce != 0)
    {
        g_nXForce = (INT)(g_nXForce / CENTER_DAMP_COEFF);
        g_nYForce = (INT)(g_nYForce / CENTER_DAMP_COEFF);
        SetDeviceSpring();
    }
    g_nYForce = 0;
    SetDeviceSpring();
}

int JoysticksNumber()
{
    return g_iNsticks;
}

LPCTSTR JoystickName(int i)
{
    if (i >= g_iNsticks)
        return NULL;
    return g_Sticks[i].name;
}

void SetTrimmer(int j, int k, int b, int b2, int b3)
{
    if (j < g_iNsticks)
    {
        g_Opt.jtrim = j;
        g_Opt.jPOV = k;
    }

    if (b < MAXBUTTONS)
    {
        g_Opt.btrimHold = b;
        g_Opt.btrimToggle = b2;
        g_Opt.btrimCenter = b3;
    }
}

void GetTrimmer(int &j, int &k, int &b, int &b2, int &b3)
{
    j = g_Opt.jtrim;
    k = g_Opt.jPOV;
    b = g_Opt.btrimHold;
    b2 = g_Opt.btrimToggle;
    b3 = g_Opt.btrimCenter;
}

void SetJtOptions(stoptions *so)
{
    SetTrimmer(so->jtrim, so->jPOV, so->btrimHold, so->btrimToggle, so->btrimCenter);
    g_Opt.damper = so->damper * 100;
    g_Opt.damper2 = so->damper2 * 100;
    g_Opt.friction = so->friction * 100;
    g_Opt.friction2 = so->friction2 * 100;
    g_Opt.spring = so->spring * 100;
    g_Opt.spring2 = so->spring2 * 100;
    g_Opt.iKey = so->iKey;
    g_Opt.ctKey = so->ctKey;
    g_Opt.windowX = so->windowX;
    g_Opt.windowY = so->windowY;
    g_Opt.swap = so->swap;
    g_Opt.trimmode = so->trimmode;
    g_Opt.g_bSpring = so->g_bSpring;

    for (int i = 0; i < 4; i++)
    {
        g_Opt.iKeyMod[i] = so->iKeyMod[i];
        g_Opt.ctKeyMod[i] = so->ctKeyMod[i];
    }

    strncpy_s(g_Opt.xplaneIP, sizeof(g_Opt.xplaneIP), so->xplaneIP, _TRUNCATE);
    g_Opt.xplanePort = so->xplanePort;
    g_Opt.shakerGain = so->shakerGain;
    g_Opt.bumpGain = so->bumpGain;
    g_Opt.rollGain = so->rollGain;
    g_Opt.xplaneMode = so->xplaneMode;

    SetDeviceConditions();
}

void GetJtOptions(stoptions *so)
{
    GetTrimmer(so->jtrim, so->jPOV, so->btrimHold, so->btrimToggle, so->btrimCenter);
    so->spring = g_Opt.spring / 100;
    so->spring2 = g_Opt.spring2 / 100;
    so->damper = g_Opt.damper / 100;
    so->damper2 = g_Opt.damper2 / 100;
    so->friction = g_Opt.friction / 100;
    so->friction2 = g_Opt.friction2 / 100;
    so->iKey = g_Opt.iKey;
    so->ctKey = g_Opt.ctKey;
    so->windowX = g_Opt.windowX;
    so->windowY = g_Opt.windowY;
    so->swap = g_Opt.swap;
    so->trimmode = g_Opt.trimmode;
    so->g_bSpring = g_Opt.g_bSpring;

    for (int i = 0; i < 4; i++)
    {
        so->iKeyMod[i] = g_Opt.iKeyMod[i];
        so->ctKeyMod[i] = g_Opt.ctKeyMod[i];
    }

    strncpy_s(so->xplaneIP, sizeof(so->xplaneIP), g_Opt.xplaneIP, _TRUNCATE);
    so->xplanePort = g_Opt.xplanePort;
    so->shakerGain = g_Opt.shakerGain;
    so->bumpGain = g_Opt.bumpGain;
    so->rollGain = g_Opt.rollGain;
    so->xplaneMode = g_Opt.xplaneMode;
}

BOOL LoadOptionsFromFile()
{
    HANDLE fhnd;
    stoptions tmpopt;
    DWORD r;
    unsigned long mn;

    fhnd = CreateFile(OPTFILENAME, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (INVALID_HANDLE_VALUE == fhnd)
    {
        return false;
    }
    ReadFile(fhnd, &mn, sizeof(mn), &r, NULL);
    if (mn != MAGICNUMBER)
    {
        CloseHandle(fhnd);
        return false;
    }
    ReadFile(fhnd, &tmpopt, sizeof(stoptions), &r, NULL);
    // Always want Spring Force 1 on startup
    tmpopt.g_bSpring = 1;
    SetJtOptions(&tmpopt);
    CloseHandle(fhnd);
    return true;
}

BOOL SaveOptionsToFile()
{
    HANDLE fhnd;
    DWORD w;
    unsigned long mn = MAGICNUMBER;
    stoptions so;

    fhnd = CreateFile(OPTFILENAME, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (INVALID_HANDLE_VALUE == fhnd)
    {
        return false;
    }
    WriteFile(fhnd, &mn, sizeof(mn), &w, NULL);
    GetJtOptions(&so);
    WriteFile(fhnd, &so, sizeof(stoptions), &w, NULL);
    CloseHandle(fhnd);
    return true;
}

//-----------------------------------------------------------------------------
// X-Plane FFB functions
//-----------------------------------------------------------------------------

void ApplyXPlaneTelemetry(const XPlaneTelemetry &t)
{
    // Aerodynamic stick force = elv_deflection × IAS × constant.
    // elv1_def is in degrees; assume ±30° max — adjust MAX_DEF_DEG if the aircraft differs.
    // Sign check: positive elv1_def should produce aft stick force (pulling back).
    // Negate elv_norm / ail_norm below if the direction feels inverted on your aircraft.
    const float MAX_DEF_DEG = 10.0f;
    const float IAS_REF = 150.0f; // knots at which aero load reaches full DI scale

    float elv_norm = max(-1.0f, min(t.elv_deflection / MAX_DEF_DEG, 1.0f));
    float ail_norm = max(-1.0f, min(-t.ail_deflection / MAX_DEF_DEG, 1.0f));

    g_xplaneSpeedScale = min(t.indicated_airspeed / IAS_REF, 1.0f);

    // Aero spring offset; g_nYForce/g_nXForce (internal trim, adjusted by POV hat) are
    // subtracted in SetDeviceSpring: lOffset = g_xplaneAeroY + g_nYForce.
    // When the user has trimmed to exactly counteract the aero load, lOffset == 0.
    // g_xplaneAeroY = (INT)( elv_norm * g_xplaneSpeedScale * DI_FFNOMINALMAX);
    const float k = 0.5f; // Empirical constant to keep max force in check; adjust as needed for your aircraft.
    g_xplaneAeroY = (INT)(elv_norm * k * g_xplaneSpeedScale * g_xplaneSpeedScale * DI_FFNOMINALMAX);
    if (g_xplaneAeroY > DI_FFNOMINALMAX)
        g_xplaneAeroY = DI_FFNOMINALMAX;
    else if (g_xplaneAeroY < -DI_FFNOMINALMAX)
        g_xplaneAeroY = -DI_FFNOMINALMAX;

    // printf("elv_norm=%.3f  ail_norm=%.3f  ias=%.1f  scale=%.3f g_xplaneAeroY=%d\n",
    //        elv_norm, ail_norm, t.indicated_airspeed, g_xplaneSpeedScale, g_xplaneAeroY);
    g_xplaneAeroX = (INT)(ail_norm * k * g_xplaneSpeedScale * g_xplaneSpeedScale * DI_FFNOMINALMAX);
    if (g_xplaneAeroX > DI_FFNOMINALMAX)
        g_xplaneAeroX = DI_FFNOMINALMAX;
    else if (g_xplaneAeroX < -DI_FFNOMINALMAX)
        g_xplaneAeroX = -DI_FFNOMINALMAX;

    // printf("elv_norm=%.3f  ail_norm=%.3f  scalespeed=%.3f g_xplaneAeroY=%d g_xplaneAeroX=%d\n",
    //        elv_norm, ail_norm, g_xplaneSpeedScale, g_xplaneAeroY, g_xplaneAeroX);

    SetDeviceSpring();

    // printf("tire_vrt_def  [0]=%.4f  [1]=%.4f  [2]=%.4f\n",
    //        t.tire_vrt_def[0], t.tire_vrt_def[1], t.tire_vrt_def[2]);

    SetShakerActive(t.stall_warning > 0.5f, g_Opt.shakerGain);

    {
        static float s_prevDef[3] = {};
        static bool s_wasOnGround = false;
        static float s_prevFpm = 0.0f;
        static int s_bumpFrames = 0;
        static int s_bumpGain = 0;

        const float BUMP_THRESHOLD = 0.01f;
        const int BUMP_DURATION = 1;

        bool onGround = t.on_ground > 0.5f;

        // Landing impact: on_ground false→true; use previous frame's fpm magnitude
        if (onGround && !s_wasOnGround)
        {
            float impactFpm = fabsf(s_prevFpm);
            int landingGain = (int)(impactFpm / 500.0f * g_Opt.bumpGain * 4);
            if (landingGain > 0)
            {
                s_bumpFrames = BUMP_DURATION;
                s_bumpGain = landingGain;
            }
        }
        else
        {
            // Runway roughness: bump on sudden strut deflection change
            float maxDiff = 0.0f;
            for (int i = 0; i < 3; i++)
            {
                float prev = s_prevDef[i];
                s_prevDef[i] = t.tire_vrt_def[i];
                float diff = fabsf(t.tire_vrt_def[i] - prev);
                if (diff > maxDiff)
                    maxDiff = diff;
            }
            if (maxDiff > BUMP_THRESHOLD)
            {
                s_bumpFrames = BUMP_DURATION;
                s_bumpGain = (int)(maxDiff / BUMP_THRESHOLD * g_Opt.bumpGain);
            }
        }

        SetBumpActive(s_bumpFrames > 0, s_bumpGain);
        if (s_bumpFrames > 0)
            s_bumpFrames--;

        SetRollActive(onGround, t.ground_speed_ms);

        s_prevFpm = t.vvi_fpm;
        s_wasOnGround = onGround;
    }
}

void GetXPlaneForces(INT &aeroY, INT &aeroX, INT &trimY, INT &trimX, INT &effTrimX, INT &effTrimY)
{
    aeroY = g_xplaneAeroY;
    aeroX = g_xplaneAeroX;
    trimY = g_nYForce;
    trimX = g_nXForce;
    effTrimY = (INT)(g_nYForce * 0.5f * g_xplaneSpeedScale * g_xplaneSpeedScale);
    effTrimX = (INT)(g_nXForce * 0.5f * g_xplaneSpeedScale * g_xplaneSpeedScale);
}

void ResetTrim()
{
    g_nYForce = 0;
    g_nXForce = 0;
    SetDeviceSpring();
}

void SetShakerActive(bool on, int gain)
{
    if (!g_pEffectShaker)
        return;

    DIPERIODIC per;
    ZeroMemory(&per, sizeof(per));
    per.dwMagnitude = on ? (DWORD)(gain * 100) : 0;
    per.lOffset = 0;
    per.dwPhase = 0;
    per.dwPeriod = 50000; // 20 Hz

    DIEFFECT eff;
    ZeroMemory(&eff, sizeof(eff));
    eff.dwSize = sizeof(DIEFFECT);
    eff.cbTypeSpecificParams = sizeof(DIPERIODIC);
    eff.lpvTypeSpecificParams = &per;

    g_pEffectShaker->SetParameters(&eff, DIEP_TYPESPECIFICPARAMS);

    if (on)
        g_pEffectShaker->Start(1, 0);
    else
        g_pEffectShaker->Stop();
}

void SetBumpActive(bool on, int gain)
{
    if (!g_pEffectBump)
        return;

    static bool s_wasOn = false;

    // On each new bump trigger, randomise the direction so consecutive bumps
    // feel distinct rather than always shaking along the same axis.
    if (on && !s_wasOn)
    {
        float angle = ((float)rand() / RAND_MAX) * 2.0f * 3.14159265f;
        LONG rglDir[2] = {(LONG)(cosf(angle) * 10000), (LONG)(sinf(angle) * 10000)};
        DWORD rgdwAxes[2] = {DIJOFS_X, DIJOFS_Y};

        DIEFFECT dirEff;
        ZeroMemory(&dirEff, sizeof(dirEff));
        dirEff.dwSize = sizeof(DIEFFECT);
        dirEff.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
        dirEff.cAxes = g_dwNumForceFeedbackAxis;
        dirEff.rgdwAxes = rgdwAxes;
        dirEff.rglDirection = rglDir;
        g_pEffectBump->SetParameters(&dirEff, DIEP_DIRECTION);
    }
    s_wasOn = on;

    DIPERIODIC per;
    ZeroMemory(&per, sizeof(per));
    per.dwMagnitude = on ? (DWORD)(gain * 30) : 0;
    per.lOffset = 0;
    per.dwPhase = 0;
    per.dwPeriod = 125000; // 8 Hz

    DIEFFECT eff;
    ZeroMemory(&eff, sizeof(eff));
    eff.dwSize = sizeof(DIEFFECT);
    eff.cbTypeSpecificParams = sizeof(DIPERIODIC);
    eff.lpvTypeSpecificParams = &per;

    g_pEffectBump->SetParameters(&eff, DIEP_TYPESPECIFICPARAMS);

    if (on)
        g_pEffectBump->Start(1, 0);
    else
        g_pEffectBump->Stop();
}

void SetRollActive(bool on, float groundspeed_ms)
{
    if (!g_pEffectRoll)
        return;

    DIPERIODIC per;
    ZeroMemory(&per, sizeof(per));

    if (on && groundspeed_ms > 0.1f)
    {
        // Higher ground speed → shorter period (higher frequency)
        DWORD period_us = (DWORD)(2000000.0f / groundspeed_ms);
        period_us = max(10000u, min(500000u, period_us));
        per.dwMagnitude = (DWORD)(g_Opt.rollGain * 100);
        per.dwPeriod = period_us;
    }
    // else magnitude and period stay 0

    DIEFFECT eff;
    ZeroMemory(&eff, sizeof(eff));
    eff.dwSize = sizeof(DIEFFECT);
    eff.cbTypeSpecificParams = sizeof(DIPERIODIC);
    eff.lpvTypeSpecificParams = &per;

    g_pEffectRoll->SetParameters(&eff, DIEP_TYPESPECIFICPARAMS);

    if (on && groundspeed_ms > 0.1f)
        g_pEffectRoll->Start(1, 0);
    else
        g_pEffectRoll->Stop();
}