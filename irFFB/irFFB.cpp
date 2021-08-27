/*
Copyright (c) 2016 NLP

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "irFFB.h"
#include "Settings.h"
#include "public.h"
#include "vjoyinterface.h"
#include "SharedFileOut.h"
#include <math.h>

#define MAX_LOADSTRING 100

#define STATUS_CONNECTED_PART 0
#define STATUS_ONTRACK_PART 1
#define STATUS_CAR_PART 2

extern HANDLE hDataValidEvent;

// Globals
HINSTANCE hInst;
WCHAR szTitle[MAX_LOADSTRING];
WCHAR szWindowClass[MAX_LOADSTRING];
NOTIFYICONDATA niData;

HANDLE globalMutex;

HANDLE debugHnd = INVALID_HANDLE_VALUE;
wchar_t debugLastMsg[512];
LONG debugRepeat = 0;

LPDIRECTINPUT8 pDI = nullptr;
LPDIRECTINPUTDEVICE8 ffdevice = nullptr;
LPDIRECTINPUTEFFECT effect = nullptr;

CRITICAL_SECTION effectCrit;

DIJOYSTATE joyState;
DWORD axes[1] = { DIJOFS_X };
LONG  dir[1]  = { 0 };
DIPERIODIC pforce;
DIEFFECT   dieff;

LogiLedData logiLedData;
DIEFFESCAPE logiEscape;

Settings settings;

float firc6[] = {
    0.1295867f, 0.2311436f, 0.2582509f, 0.1923936f, 0.1156718f, 0.0729534f
};
float firc12[] = {
    0.0322661f, 0.0696877f, 0.0967984f, 0.1243019f, 0.1317534f, 0.1388793f,
    0.1129315f, 0.0844297f, 0.0699100f, 0.0567884f, 0.0430215f, 0.0392321f
};

char car[MAX_CAR_NAME];
understeerCoefs usteerCoefs[] = {
    { "audir8gt3",          52.0f, 78.0f  },
    { "ferrari488gt3",      46.0f, 54.0f  },
    { "ferrari488gte",      44.0f, 46.0f  },
    { "formularenault20",   34.5f, 96.0f  },
    { "lotus79",            27.8f, 104.0f },
    { "mercedesamggt3",     37.5f, 82.0f  },
    { "mx5 mx52016",        36.0f, 96.0f  },
    { "porsche911cup",      46.0f, 88.0f  },
    { "porsche991rsr",      42.0f, 72.0f  },
    { "rt2000",             25.0f, 86.0f  }
};

int force = 0;
volatile float suspForce = 0.0f; 
volatile float yawForce = 0.0f;
__declspec(align(16)) volatile float suspForceST[DIRECT_INTERP_SAMPLES];
bool onTrack = false, stopped = true, deviceChangePending = false, logiWheel = false;

volatile int ffbMag = 0;
volatile bool nearStops = false;

int numButtons = 0, numPov = 0, vjButtons = 0, vjPov = 0;
UINT samples, clippedSamples;

HANDLE wheelEvent = CreateEvent(nullptr, false, false, L"WheelEvent");
HANDLE ffbEvent   = CreateEvent(nullptr, false, false, L"FFBEvent");

HWND mainWnd, textWnd, statusWnd;

LARGE_INTEGER freq;

int vjDev = 1;
FFB_DATA ffbPacket;

template <typename T, unsigned S>
inline unsigned arraysize(const T(&v)[S])
{
    return S;
}


struct SMElement
{
    HANDLE hMapFile;
    unsigned char* mapFileBuffer;
};

SMElement m_graphics;
SMElement m_physics;
SMElement m_static;

void initPhysics()
{
    TCHAR szName[] = TEXT("Local\\acpmf_physics");
    m_physics.hMapFile = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(SPageFilePhysics), szName);
    if (!m_physics.hMapFile)
    {
        MessageBoxA(GetActiveWindow(), "CreateFileMapping failed", "ACCS", MB_OK);
    }
    m_physics.mapFileBuffer = (unsigned char*)MapViewOfFile(m_physics.hMapFile, FILE_MAP_READ, 0, 0, sizeof(SPageFilePhysics));
    if (!m_physics.mapFileBuffer)
    {
        MessageBoxA(GetActiveWindow(), "MapViewOfFile failed", "ACCS", MB_OK);
    }
}

void initGraphics()
{
    TCHAR szName[] = TEXT("Local\\acpmf_graphics");
    m_graphics.hMapFile = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(SPageFileGraphic), szName);
    if (!m_graphics.hMapFile)
    {
        MessageBoxA(GetActiveWindow(), "CreateFileMapping failed", "ACCS", MB_OK);
    }
    m_graphics.mapFileBuffer = (unsigned char*)MapViewOfFile(m_graphics.hMapFile, FILE_MAP_READ, 0, 0, sizeof(SPageFileGraphic));
    if (!m_graphics.mapFileBuffer)
    {
        MessageBoxA(GetActiveWindow(), "MapViewOfFile failed", "ACCS", MB_OK);
    }
}

void initStatic()
{
    TCHAR szName[] = TEXT("Local\\acpmf_static");
    m_static.hMapFile = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(SPageFileStatic), szName);
    if (!m_static.hMapFile)
    {
        MessageBoxA(GetActiveWindow(), "CreateFileMapping failed", "ACCS", MB_OK);
    }
    m_static.mapFileBuffer = (unsigned char*)MapViewOfFile(m_static.hMapFile, FILE_MAP_READ, 0, 0, sizeof(SPageFileStatic));
    if (!m_static.mapFileBuffer)
    {
        MessageBoxA(GetActiveWindow(), "MapViewOfFile failed", "ACCS", MB_OK);
    }
}

void dismiss(SMElement element)
{
    UnmapViewOfFile(element.mapFileBuffer);
    CloseHandle(element.hMapFile);
}

// Thread that reads the wheel, writes to vJoy and updates the DI effect
DWORD WINAPI readWheelThread(LPVOID lParam) {

    UNREFERENCED_PARAMETER(lParam);

    HRESULT res;
    JOYSTICK_POSITION vjData;
    DWORD *hats[] = { &vjData.bHats, &vjData.bHatsEx1, &vjData.bHatsEx2, &vjData.bHatsEx3 };
    ResetVJD(vjDev);
    LONG lastX;
    LARGE_INTEGER lastTime, time, elapsed;
    float vel[DIRECT_INTERP_SAMPLES] = { 0.0f }, fd[4] = { 0.0f };
    int velIdx = 0, vi = 0, fdIdx = 0;
    float d = 0.0f;

    lastTime.QuadPart = 0;

    while (true) {

        DWORD signaled = WaitForSingleObject(wheelEvent, 1);

        if (!ffdevice)
            continue;

        if (signaled == WAIT_OBJECT_0) {

            res = ffdevice->GetDeviceState(sizeof(joyState), &joyState);
            if (res != DI_OK) {
                debug(L"GetDeviceState returned: 0x%x, requesting reacquire", res);
                reacquireDIDevice();
                continue;
            }

            vjData.wAxisX    = joyState.lX;
            vjData.wAxisY    = joyState.lY;
            vjData.wAxisZ    = joyState.lZ;
            vjData.wAxisXRot = joyState.lRx;
            vjData.wAxisYRot = joyState.lRy;
            vjData.wAxisZRot = joyState.lRz;

            if (vjButtons > 0)
                for (int i = 0; i < numButtons; i++) {
                    if (joyState.rgbButtons[i])
                        vjData.lButtons |= 1 << i;
                    else
                        vjData.lButtons &= ~(1 << i);
                }

            // This could be wrong, untested..
            if (vjPov > 0)
                for (int i = 0; i < numPov && i < 4; i++)
                    *hats[i] = joyState.rgdwPOV[i];

            UpdateVJD(vjDev, (PVOID)&vjData);

            if (effect == nullptr)
                continue;

            if (settings.getDampingFactor() != 0.0f || nearStops) {

                QueryPerformanceCounter(&time);

                if (lastTime.QuadPart != 0) {
                    elapsed.QuadPart = (time.QuadPart - lastTime.QuadPart) * 1000000;
                    elapsed.QuadPart /= freq.QuadPart;
                    vel[velIdx] = (float)(joyState.lX - lastX) / elapsed.QuadPart;
                }

                lastTime.QuadPart = time.QuadPart;
                lastX = joyState.lX;

                vi = velIdx;

                if (++velIdx > DIRECT_INTERP_SAMPLES - 1)
                    velIdx = 0;

                fd[fdIdx] = vel[vi++] * firc6[0];
                for (int i = 1; i < DIRECT_INTERP_SAMPLES; i++) {
                    if (vi > DIRECT_INTERP_SAMPLES - 1)
                        vi = 0;
                    fd[fdIdx] += vel[vi++] * firc6[i];
                }

                if (++fdIdx > 3)
                    fdIdx = 0;

                d = (fd[0] + fd[1] + fd[2] + fd[3]) / 4.0f;

                if (nearStops)
                    d *= DAMPING_MULTIPLIER_STOPS;
                else
                    d *= DAMPING_MULTIPLIER * settings.getDampingFactor();

            }
            else
                d = 0.0f;

        }

        pforce.lOffset = ffbMag;
        //pforce.lOffset += (int)d;

        if (pforce.lOffset > DI_MAX)
            pforce.lOffset = DI_MAX;
        else if (pforce.lOffset < -DI_MAX)
            pforce.lOffset = -DI_MAX;

        EnterCriticalSection(&effectCrit);

        if (effect == nullptr) {
            LeaveCriticalSection(&effectCrit);
            continue;
        }

        HRESULT hr = effect->SetParameters(&dieff, DIEP_TYPESPECIFICPARAMS | DIEP_NORESTART);
        if (hr != DI_OK) {
            debug(L"SetParameters returned 0x%x, requesting reacquire", hr);
            reacquireDIDevice();
        }

        LeaveCriticalSection(&effectCrit);

    }

}

void resetForces() {
    debug(L"Resetting forces");
    suspForce = 0;
    yawForce = 0;
    for (int i = 0; i < DIRECT_INTERP_SAMPLES; i++) {
        suspForceST[i] = 0;
    }
    force = 0;
    setFFB(0);
}

boolean getCarName() {
    return false;
}

float getCarRedline() {

    return 8000.0f;

}

void clippingReport() {

    float clippedPerCent = samples > 0 ? (float)clippedSamples * 100.0f / samples : 0.0f;
    text(L"%.02f%% of samples were clipped", clippedPerCent);
    if (clippedPerCent > 2.5f)
        text(L"Consider increasing max force to reduce clipping");
    samples = clippedSamples = 0;

}

void logiRpmLed(float* rpm, float redline) {

    logiLedData.rpmData.rpm = *rpm / (redline * 0.90f);
    logiLedData.rpmData.rpmFirstLed = 0.65f;
    logiLedData.rpmData.rpmRedLine = 1.0f;

    ffdevice->Escape(&logiEscape);

}

// Calculate FFB samples for the direct modes
DWORD WINAPI directFFBThread(LPVOID lParam) {

    UNREFERENCED_PARAMETER(lParam);
    int16_t mag;

    float s;
    int r;
    float lastSuspForce = 0, lastYawForce = 0;
    float yaw = 0.0f;
    float gLat = 0.0f;
    float wheelSlip = 0.0f;
    float wheelSlipTH = 0.95f;

    float maxValue = 10000.0f;

    constexpr auto PI = 3.14159265;

    while (true) {

        // Signalled when force has been updated
        WaitForSingleObject(ffbEvent, INFINITE);
        
        if (((ffbPacket.data[0] & 0xF0) >> 4) != vjDev)
            continue;
        
        mag = (ffbPacket.data[3] << 8) + ffbPacket.data[2];
        force = mag;

        s = (float)force / maxValue;

        SPageFilePhysics* pfPhys = (SPageFilePhysics*)m_physics.mapFileBuffer;
        SPageFileGraphic* pfGfx = (SPageFileGraphic*)m_graphics.mapFileBuffer;

        if (onTrack && pfGfx->isInPit) {
            debug(L"No longer on track");
            onTrack = false;
            setOnTrackStatus(onTrack);
            resetForces();
            clippingReport();
        }
        else if (!onTrack && !pfGfx->isInPit) {
            debug(L"Now on track");
            onTrack = true;
            setOnTrackStatus(onTrack);
        }

        yaw = 0.0f;
        gLat = 0.0f;

        float bumpsFactor = settings.getBumpsFactor();
        float sopFactor = settings.getSopFactor() / 100.0f;
        float gLatFactor = settings.getGLatFactor() / 100.0f;
        float sopOffset = settings.getSopOffset() / 10.0f;
        float wheelSlipFactor = settings.getUndersteerFactor() / 100.0f;

        if (pfPhys->speedKmh > 2.0f) {

            if (pfPhys->speedKmh > 5.0f) {

                if (wheelSlipFactor > 0.0f)
                {
                    float ws = (pfPhys->wheelSlip[0] + pfPhys->wheelSlip[1]) / 2.0f;
                    if (ws > wheelSlipTH)
                        ws = ws - wheelSlipTH;
                    else
                        ws = 0.0f;
                    wheelSlip = (2.0f * wheelSlipFactor) / (1.0f + expf(-10.0f * ws)) - wheelSlipFactor;
                }
                
                if (sopFactor > 0.0f)
                {
                    float r = (2 * pfPhys->localVelocity[0]) / pfPhys->localVelocity[2];
                    if (pfPhys->localVelocity[2] < 0.0f)
                        r = -r;

                    yaw = (2.0f * sopFactor) / (1.0f + expf(-sopOffset / sopFactor * r)) - sopFactor;

                    float sgn = csignf(1.0f, yaw);
                    yaw = minf(fabsf(yaw), fabsf(pfPhys->accG[0] / 1.0f));
                    yaw = csignf(yaw, sgn);
                }

                if (gLatFactor > 0.0f)
                {
                    float r = -pfPhys->accG[0] / 2.6f;
                    if (pfPhys->localVelocity[2] < 0.0f)
                        r = -r;

                    gLat = (2.0f * gLatFactor) / (1.0f + expf(-2.3f * r)) - gLatFactor;
                }

            }

            stopped = false;

        }
        else
            stopped = true;

        float forces = (yaw + gLat) * settings.getScaleFactor() + s;

        float sign = csignf(1.0f, forces);

        float gLong = -pfPhys->accG[2];

        gLong = ((1.2f * gLatFactor) / (1.0f + expf(-2.3f * (gLong - 0.7f))) - 0.2f * gLatFactor) / 2.0f;

        forces = fabsf(forces);
        float sig = 2.0f / (1.0f + powf(2.718f, -15.0f * forces)) - 1.0f;
        forces = sign * (((1.0f - sig) * forces) + (sig * (powf(forces, 1.0f / (1.0f + gLong)))));
        forces *= 1.0f - 0.5f * wheelSlip;

        r = (int)(forces * maxValue);

        setFFB(r);
    }

    return 0;

}


void deviceChange() {

    debug(L"Device change notification");
    if (!onTrack) {
        debug(L"Not on track, processing device change");
        deviceChangePending = false;
        enumDirectInput();
        if (!settings.isFfbDevicePresent())
            releaseDirectInput();
    }
    else {
        debug(L"Deferring device change processing whilst on track");
        deviceChangePending = true;
    }

}

DWORD getDeviceVidPid(LPDIRECTINPUTDEVICE8 dev) {

    DIPROPDWORD dipdw;
    dipdw.diph.dwSize = sizeof(DIPROPDWORD);
    dipdw.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    dipdw.diph.dwObj = 0;
    dipdw.diph.dwHow = DIPH_DEVICE;

    if (dev == nullptr)
        return 0;

    if (!SUCCEEDED(dev->GetProperty(DIPROP_VIDPID, &dipdw.diph)))
        return 0;

    return dipdw.dwData;

}

void minimise() {
    debug(L"Minimising window");
    Shell_NotifyIcon(NIM_ADD, &niData);
    ShowWindow(mainWnd, SW_HIDE);
}

void restore() {
    debug(L"Restoring window");
    Shell_NotifyIcon(NIM_DELETE, &niData);
    ShowWindow(mainWnd, SW_SHOW);
    BringWindowToTop(mainWnd);
}

int APIENTRY wWinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR    lpCmdLine,
    _In_ int       nCmdShow
) {

    UNREFERENCED_PARAMETER(hPrevInstance);

    globalMutex = CreateMutex(NULL, false, L"Global\\irFFB_Mutex");

    if (GetLastError() == ERROR_ALREADY_EXISTS)
        exit(0);

    initPhysics();
    initGraphics();
    initStatic();

    INITCOMMONCONTROLSEX ccEx;

    char *data = nullptr;
    bool irConnected = false;
    MSG msg;

    ccEx.dwICC = ICC_WIN95_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    ccEx.dwSize = sizeof(ccEx);
    InitCommonControlsEx(&ccEx);

    HACCEL hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_IRFFB));

    LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
    LoadStringW(hInstance, IDC_IRFFB, szWindowClass, MAX_LOADSTRING);
    MyRegisterClass(hInstance);

    // Setup DI FFB effect
    pforce.dwMagnitude = 0;
    pforce.dwPeriod = INFINITE;
    pforce.dwPhase = 0;
    pforce.lOffset = 0;

    ZeroMemory(&dieff, sizeof(dieff));
    dieff.dwSize = sizeof(DIEFFECT);
    dieff.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
    dieff.dwDuration = INFINITE;
    dieff.dwSamplePeriod = 0;
    dieff.dwGain = DI_FFNOMINALMAX;
    dieff.dwTriggerButton = DIEB_NOTRIGGER;
    dieff.dwTriggerRepeatInterval = 0;
    dieff.cAxes = 1;
    dieff.rgdwAxes = axes;
    dieff.rglDirection = dir;
    dieff.lpEnvelope = NULL;
    dieff.cbTypeSpecificParams = sizeof(DIPERIODIC);
    dieff.lpvTypeSpecificParams = &pforce;
    dieff.dwStartDelay = 0;

    ZeroMemory(&logiLedData, sizeof(logiLedData));
    logiLedData.size = sizeof(logiLedData);
    logiLedData.version = 1;
    ZeroMemory(&logiEscape, sizeof(logiEscape));
    logiEscape.dwSize = sizeof(DIEFFESCAPE);
    logiEscape.dwCommand = 0;
    logiEscape.lpvInBuffer = &logiLedData;
    logiEscape.cbInBuffer = sizeof(logiLedData);

    InitializeCriticalSection(&effectCrit);

    if (!InitInstance(hInstance, nCmdShow))
        return FALSE;

    memset(car, 0, sizeof(car));
    setCarStatus(car);
    setConnectedStatus(false);
    setOnTrackStatus(false);
    settings.readGenericSettings();
    settings.readRegSettings(car);

    if (settings.getStartMinimised())
        minimise();
    else
        restore();

    enumDirectInput();

    QueryPerformanceFrequency(&freq);

    initVJD();
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

    SetThreadPriority(
        CreateThread(NULL, 0, readWheelThread, NULL, 0, NULL), THREAD_PRIORITY_HIGHEST
    );
    SetThreadPriority(
        CreateThread(NULL, 0, directFFBThread, NULL, 0, NULL), THREAD_PRIORITY_HIGHEST
    );

    debug(L"Init complete, entering mainloop");

    setConnectedStatus(true);

    setCarStatus(nullptr);

    int bRet;
    while (TRUE) {

        while ((bRet = GetMessage(&msg, NULL, 0, 0)) != 0)
        {
            if (bRet == -1)
            {
                // handle the error and possibly exit
            }
            else
            {
                if (msg.message == WM_QUIT)
                    DestroyWindow(mainWnd);
                if (mainWnd == (HWND)NULL ||
                    !IsDialogMessage(mainWnd, &msg) &&
                    !TranslateAccelerator(msg.hwnd, hAccelTable,
                        &msg))
                {
                    TranslateMessage(&msg);
                    DispatchMessage(&msg);
                }
            }
        }
    }

    return (int)msg.wParam;

}

ATOM MyRegisterClass(HINSTANCE hInstance) {

    WNDCLASSEXW wcex;

    wcex.cbSize = sizeof(WNDCLASSEX);

    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.cbClsExtra = 0;
    wcex.cbWndExtra = 0;
    wcex.hInstance = hInstance;
    wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_IRFFB));
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszMenuName = MAKEINTRESOURCEW(IDC_IRFFB);
    wcex.lpszClassName = szWindowClass;
    wcex.hIconSm = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));

    return RegisterClassExW(&wcex);

}

LRESULT CALLBACK EditWndProc(HWND wnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR subId, DWORD_PTR rData) {

    if (msg == WM_CHAR) {

        wchar_t buf[8];

        if (subId == EDIT_FLOAT) {

            if (GetWindowTextW(wnd, buf, 8) && StrChrIW(buf, L'.') && wParam == '.')
                return 0;

            if (
                !(
                    (wParam >= L'0' && wParam <= L'9') ||
                    wParam == L'.' ||
                    wParam == VK_RETURN ||
                    wParam == VK_DELETE ||
                    wParam == VK_BACK
                )
            )
                return 0;

            LRESULT ret = DefSubclassProc(wnd, msg, wParam, lParam);

            wchar_t *end;
            float val = 0.0f;

            GetWindowText(wnd, buf, 8);
            val = wcstof(buf, &end);
            if (end - buf == wcslen(buf))
                SendMessage(GetParent(wnd), WM_EDIT_VALUE, reinterpret_cast<WPARAM &>(val), (LPARAM)wnd);

            return ret;

        }
        else {

            if (
                !(
                    (wParam >= L'0' && wParam <= L'9') ||
                    wParam == VK_RETURN ||
                    wParam == VK_DELETE ||
                    wParam == VK_BACK
                )
            )
                return 0;

            LRESULT ret = DefSubclassProc(wnd, msg, wParam, lParam);
            GetWindowText(wnd, buf, 8);
            int val = _wtoi(buf);
            SendMessage(GetParent(wnd), WM_EDIT_VALUE, (WPARAM)val, (LPARAM)wnd);
            return ret;

        }

    }

    return DefSubclassProc(wnd, msg, wParam, lParam);

}

HWND combo(HWND parent, wchar_t *name, int x, int y) {

    CreateWindowW(
        L"STATIC", name,
        WS_CHILD | WS_VISIBLE,
        x, y, 300, 20, parent, NULL, hInst, NULL
    );
    return 
        CreateWindow(
            L"COMBOBOX", nullptr,
            CBS_DROPDOWNLIST | WS_CHILD | WS_VISIBLE | WS_OVERLAPPED | WS_TABSTOP,
            x + 12, y + 26, 300, 240, parent, nullptr, hInst, nullptr
        );

}

sWins_t *slider(HWND parent, wchar_t *name, int x, int y, wchar_t *start, wchar_t *end, bool floatData) {

    sWins_t *wins = (sWins_t *)malloc(sizeof(sWins_t));

    wins->label = CreateWindowW(
        L"STATIC", name,
        WS_CHILD | WS_VISIBLE,
        x, y, 300, 20, parent, NULL, hInst, NULL
    );

    wins->value = CreateWindowW(
        L"EDIT", L"", 
        WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_CENTER,
        x + 210, y, 50, 20, parent, NULL, hInst, NULL
    );

    SetWindowSubclass(wins->value, EditWndProc, floatData ? 1 : 0, 0);

    SendMessage(wins->value, EM_SETLIMITTEXT, 5, 0);

    wins->trackbar = CreateWindowExW(
        0, TRACKBAR_CLASS, name,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_TOOLTIPS | TBS_TRANSPARENTBKGND,
        x + 40, y + 26, 240, 30,
        parent, NULL, hInst, NULL
    );

    HWND buddyLeft = CreateWindowEx(
        0, L"STATIC", start,
        SS_LEFT | WS_CHILD | WS_VISIBLE,
        0, 0, 40, 20, parent, NULL, hInst, NULL
    );
    SendMessage(wins->trackbar, TBM_SETBUDDY, (WPARAM)TRUE, (LPARAM)buddyLeft);

    HWND buddyRight = CreateWindowEx(
        0, L"STATIC", end,
        SS_RIGHT | WS_CHILD | WS_VISIBLE,
        0, 0, 52, 20, parent, NULL, hInst, NULL
    );
    SendMessage(wins->trackbar, TBM_SETBUDDY, (WPARAM)FALSE, (LPARAM)buddyRight);

    return wins;

}

HWND checkbox(HWND parent, wchar_t *name, int x, int y) {

    return 
        CreateWindowEx(
            0, L"BUTTON", name,
            BS_CHECKBOX | BS_MULTILINE | WS_CHILD | WS_TABSTOP | WS_VISIBLE,
            x, y, 360, 58, parent, nullptr, hInst, nullptr
        );

}

BOOL InitInstance(HINSTANCE hInstance, int nCmdShow) {

    //DEV_BROADCAST_DEVICEINTERFACE devFilter;

    hInst = hInstance;

    mainWnd = CreateWindowW(
        szWindowClass, szTitle,
        WS_OVERLAPPEDWINDOW ^ WS_THICKFRAME,
        CW_USEDEFAULT, CW_USEDEFAULT, 864, 500,
        NULL, NULL, hInst, NULL
    );

    if (!mainWnd)
        return FALSE;
    
    memset(&niData, 0, sizeof(niData));
    niData.uVersion = NOTIFYICON_VERSION;
    niData.cbSize = NOTIFYICONDATA_V1_SIZE;
    niData.hWnd = mainWnd;
    niData.uID = 1;
    niData.uFlags = NIF_ICON | NIF_MESSAGE;
    niData.uCallbackMessage = WM_TRAY_ICON;
    niData.hIcon = LoadIcon(hInst, MAKEINTRESOURCE(IDI_SMALL));

    settings.setDevWnd(combo(mainWnd, L"FFB device:", 44, 10));
    settings.setMaxWnd(slider(mainWnd, L"Overall Effect Strength:", 44, 80, L"0", L"100", false));
    settings.setSopWnd(slider(mainWnd, L"SoP effect:", 464, 20, L"0", L"100", true));
    settings.setSopOffsetWnd(slider(mainWnd, L"SoP attack:", 464, 80, L"0", L"100", true));
    settings.setGLatWnd(slider(mainWnd, L"G effect:", 464, 140, L"0", L"100", true));
    settings.setUndersteerWnd(slider(mainWnd, L"Understeer effect:", 464, 200, L"0", L"100", true));

    //settings.setReduceWhenParkedWnd(
    //    checkbox(mainWnd, L" Reduce force when parked?", 460, 200)
    //);
    settings.setRunOnStartupWnd(
        checkbox(mainWnd, L" Run on startup?", 460, 260)
    );
    settings.setStartMinimisedWnd(
        checkbox(mainWnd, L" Start minimised?", 460, 300)
    );
    settings.setDebugWnd(
        checkbox(mainWnd, L"Debug logging?", 460, 340)
    );

    int statusParts[] = { 256, 424, 864 };

    statusWnd = CreateWindowEx(
        0, STATUSCLASSNAME, NULL,
        WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0, mainWnd, NULL, hInst, NULL
    );
    SendMessage(statusWnd, SB_SETPARTS, 3, LPARAM(statusParts));
    
    textWnd = CreateWindowEx(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_VISIBLE | WS_VSCROLL | WS_CHILD | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL,
        32, 160, 376, 240,
        mainWnd, NULL, hInst, NULL
    );
    SendMessage(textWnd, EM_SETLIMITTEXT, WPARAM(256000), 0);

    ShowWindow(mainWnd, SW_HIDE);
    UpdateWindow(mainWnd);

    return TRUE;

}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {

    HWND wnd = (HWND)lParam;

    switch (message) {

        case WM_COMMAND: {

            int wmId = LOWORD(wParam);
            switch (wmId) {

                case IDM_ABOUT:
                    DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, About);
                    break;
                case IDM_EXIT:
                    DestroyWindow(hWnd);
                    break;
                default:
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        if (wnd == settings.getDevWnd()) {
                            GUID oldDevice = settings.getFfbDevice();
                            DWORD vidpid = 0;  
                            if (oldDevice != GUID_NULL)
                                vidpid = getDeviceVidPid(ffdevice); 
                            settings.setFfbDevice(SendMessage((HWND)lParam, CB_GETCURSEL, 0, 0));
                        }
                    }
                    else if (HIWORD(wParam) == BN_CLICKED) {
                        bool oldValue = SendMessage((HWND)lParam, BM_GETCHECK, 0, 0) == BST_CHECKED;
                        if (wnd == settings.getReduceWhenParkedWnd())
                            settings.setReduceWhenParked(!oldValue);
                        else if (wnd == settings.getRunOnStartupWnd())
                            settings.setRunOnStartup(!oldValue);
                        else if (wnd == settings.getStartMinimisedWnd())
                            settings.setStartMinimised(!oldValue);
                        else if (wnd == settings.getDebugWnd()) {
                            settings.setDebug(!oldValue);
                            if (settings.getDebug()) {
                                debugHnd = CreateFileW(settings.getLogPath(), GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                                int chars = SendMessageW(textWnd, WM_GETTEXTLENGTH, 0, 0);
                                wchar_t *buf = new wchar_t[chars + 1], *str = buf;
                                SendMessageW(textWnd, WM_GETTEXT, chars + 1, (LPARAM)buf);
                                wchar_t *end = StrStrW(str, L"\r\n");
                                while (end) {                                    
                                    *end = '\0';
                                    debug(str);
                                    str = end + 2;
                                    end = StrStrW(str, L"\r\n");
                                }
                                delete[] buf;
                            }
                            else if (debugHnd != INVALID_HANDLE_VALUE) {
                                CloseHandle(debugHnd);
                                debugHnd = INVALID_HANDLE_VALUE;
                            }
                        }
                    }
                    return DefWindowProc(hWnd, message, wParam, lParam);
            }
        }
        break;

        case WM_EDIT_VALUE: {
            if (wnd == settings.getMaxWnd()->value)
                settings.setMaxForce(wParam, wnd);
            else if (wnd == settings.getGLatWnd()->value)
                settings.setGLatFactor(reinterpret_cast<float&>(wParam), wnd);
            else if (wnd == settings.getSopWnd()->value)
                settings.setSopFactor(reinterpret_cast<float &>(wParam), wnd);
            else if (wnd == settings.getSopOffsetWnd()->value)
                settings.setSopOffset(reinterpret_cast<float&>(wParam), wnd);
            else if (wnd == settings.getUndersteerWnd()->value)
                settings.setUndersteerFactor(reinterpret_cast<float&>(wParam), wnd);
        }
        break;
             

        case WM_HSCROLL: {
            if (wnd == settings.getMaxWnd()->trackbar)
                settings.setMaxForce(SendMessage(wnd, TBM_GETPOS, 0, 0), wnd);
            else if (wnd == settings.getGLatWnd()->trackbar)
                settings.setGLatFactor((float)SendMessage(wnd, TBM_GETPOS, 0, 0), wnd);
            else if (wnd == settings.getSopWnd()->trackbar)
                settings.setSopFactor((float)SendMessage(wnd, TBM_GETPOS, 0, 0), wnd);
            else if (wnd == settings.getSopOffsetWnd()->trackbar)
                settings.setSopOffset((float)SendMessage(wnd, TBM_GETPOS, 0, 0), wnd);
            else if (wnd == settings.getUndersteerWnd()->trackbar)
                settings.setUndersteerFactor((float)SendMessage(wnd, TBM_GETPOS, 0, 0), wnd);
        }
        break;

        case WM_CTLCOLORSTATIC: {
            SetBkColor((HDC)wParam, RGB(0xff, 0xff, 0xff));
            return (LRESULT)CreateSolidBrush(RGB(0xff, 0xff, 0xff));
        }
        break;

        case WM_PRINTCLIENT: {
            RECT r = { 0 };
            GetClientRect(hWnd, &r);
            FillRect((HDC)wParam, &r, CreateSolidBrush(RGB(0xff, 0xff, 0xff)));
        }
        break;

        case WM_SIZE: {
            SendMessage(statusWnd, WM_SIZE, wParam, lParam);
            return DefWindowProc(hWnd, message, wParam, lParam);
        }
        break;

        case WM_POWERBROADCAST: {
            int wmId = LOWORD(wParam);
            switch (wmId) {
                case PBT_APMSUSPEND:
                    debug(L"Computer is suspending, release all");
                    releaseAll();
                break;
                case PBT_APMRESUMESUSPEND:
                    debug(L"Computer is resuming, init all");
                    initAll();
                break;
            }
        }
        break;

        case WM_TRAY_ICON: {
            switch (lParam) {
                case WM_LBUTTONUP:
                    restore();
                    break;
                case WM_RBUTTONUP: {
                    HMENU trayMenu = CreatePopupMenu();
                    POINT curPoint;
                    AppendMenuW(trayMenu, MF_STRING, ID_TRAY_EXIT, L"Exit");
                    GetCursorPos(&curPoint);
                    SetForegroundWindow(hWnd);
                    if (
                        TrackPopupMenu(
                            trayMenu, TPM_RETURNCMD | TPM_NONOTIFY,
                            curPoint.x, curPoint.y, 0, hWnd, NULL
                        ) == ID_TRAY_EXIT
                    )
                        PostQuitMessage(0);
                    DestroyMenu(trayMenu);
                }
                break;
            }
                    
        }
        break;

        case WM_DEVICECHANGE: {
            DEV_BROADCAST_HDR *hdr = (DEV_BROADCAST_HDR *)lParam;
            if (wParam != DBT_DEVICEARRIVAL && wParam != DBT_DEVICEREMOVECOMPLETE)
                return 0;
            if (hdr->dbch_devicetype != DBT_DEVTYP_DEVICEINTERFACE)
                return 0;
            deviceChange();
        }
        break;

        case WM_SYSCOMMAND: {
            switch (wParam & 0xfff0) {
                case SC_MINIMIZE:
                    minimise();
                    return 0;
                default:
                    return DefWindowProc(hWnd, message, wParam, lParam);
            }
        }
        break;

        case WM_DESTROY: {
            debug(L"Exiting");
            Shell_NotifyIcon(NIM_DELETE, &niData);
            releaseAll();
            if (settings.getUseCarSpecific() && car[0] != 0)
                settings.writeSettingsForCar(car);
            else
                settings.writeGenericSettings();
            settings.writeRegSettings();
            if (debugHnd != INVALID_HANDLE_VALUE)
                CloseHandle(debugHnd);
            CloseHandle(globalMutex);
            exit(0);
        }
        break;

        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
    }

    return 0;
}

INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) {

    UNREFERENCED_PARAMETER(lParam);

    switch (message) {

        case WM_INITDIALOG:
            return (INT_PTR)TRUE;

        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
                EndDialog(hDlg, LOWORD(wParam));
                return (INT_PTR)TRUE;
            }
        break;
    }

    return (INT_PTR)FALSE;

}

void text(wchar_t *fmt, ...) {

    va_list argp;
    wchar_t msg[512];
    va_start(argp, fmt);

    StringCbVPrintf(msg, sizeof(msg) - 2 * sizeof(wchar_t), fmt, argp);
    va_end(argp);
    StringCbCat(msg, sizeof(msg), L"\r\n");

    SendMessage(textWnd, EM_SETSEL, 0, -1);
    SendMessage(textWnd, EM_SETSEL, -1, 1);
    SendMessage(textWnd, EM_REPLACESEL, 0, (LPARAM)msg);
    SendMessage(textWnd, EM_SCROLLCARET, 0, 0);

    debug(msg);

}

void text(wchar_t *fmt, char *charstr) {

    int len = strlen(charstr) + 1;
    wchar_t *wstr = new wchar_t[len];
    mbstowcs_s(nullptr, wstr, len, charstr, len);
    text(fmt, wstr);
    delete[] wstr;

}

void debug(wchar_t *msg) {

    if (!settings.getDebug())
        return;
    
    DWORD written;
    wchar_t buf[512];
    SYSTEMTIME lt;

    GetLocalTime(&lt);
    StringCbPrintfW(
        buf, sizeof(buf), L"%d-%02d-%02d %02d:%02d:%02d.%03d ",
        lt.wYear, lt.wMonth, lt.wDay, lt.wHour, lt.wMinute, lt.wSecond, lt.wMilliseconds
    );

    StringCbCat(buf, sizeof(buf), msg);
    StringCbCat(buf, sizeof(buf), L"\r\n");

    if (!wcscmp(msg, debugLastMsg)) {
        debugRepeat++;
        return;
    }
    else if (debugRepeat) {
        wchar_t rm[256];
        StringCbPrintfW(rm, sizeof(rm), L"-- Last message repeated %d times --\r\n", debugRepeat);
        WriteFile(debugHnd, rm, wcslen(rm) * sizeof(wchar_t), &written, NULL);
        debugRepeat = 0;
    }

    StringCbCopy(debugLastMsg, sizeof(debugLastMsg), msg);
    WriteFile(debugHnd, buf, wcslen(buf) * sizeof(wchar_t), &written, NULL);

}

template <typename... T>
void debug(wchar_t *fmt, T... args) {

    if (!settings.getDebug())
        return;

    wchar_t msg[512];
    StringCbPrintf(msg, sizeof(msg), fmt, args...);
    debug(msg);

}

void setCarStatus(char *carStr) {

    if (!carStr || carStr[0] == 0) {
        SendMessage(statusWnd, SB_SETTEXT, STATUS_CAR_PART, LPARAM(L"Car: generic"));
        return;
    }

    int len = strlen(carStr) + 1;
    wchar_t *wstr = new wchar_t[len + 5];
    lstrcpy(wstr, L"Car: ");
    mbstowcs_s(nullptr, wstr + 5, len, carStr, len);
    SendMessage(statusWnd, SB_SETTEXT, STATUS_CAR_PART, LPARAM(wstr));
    delete[] wstr;

}

void setConnectedStatus(bool connected) {

    SendMessage(
        statusWnd, SB_SETTEXT, STATUS_CONNECTED_PART,
        LPARAM(connected ? L"ACC connected" : L"ACC disconnected")
    );

}

void setOnTrackStatus(bool onTrack) {

    SendMessage(
        statusWnd, SB_SETTEXT, STATUS_ONTRACK_PART,
        LPARAM(onTrack ? L"On track" : L"Not on track")
    );

    if (!onTrack && deviceChangePending) {
        debug(L"Processing deferred device change notification");
        deviceChange();
    }

}

void setLogiWheelRange(WORD prodId) {

    if (prodId == G25PID || prodId == DFGTPID || prodId == G27PID) {

        GUID hidGuid;
        HidD_GetHidGuid(&hidGuid);

        text(L"DFGT/G25/G27 detected, setting range using raw HID");

        HANDLE devInfoSet = SetupDiGetClassDevsW(&hidGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (devInfoSet == INVALID_HANDLE_VALUE) {
            text(L"LogiWheel: Error enumerating HID devices");
            return;
        }

        SP_DEVICE_INTERFACE_DATA intfData;
        SP_DEVICE_INTERFACE_DETAIL_DATA *intfDetail;
        DWORD idx = 0;
        DWORD error = 0;
        DWORD size;

        while (true) {

            intfData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

            if (!SetupDiEnumDeviceInterfaces(devInfoSet, NULL, &hidGuid, idx++, &intfData)) {
                if (GetLastError() == ERROR_NO_MORE_ITEMS)
                    break;
                continue;
            }

            if (!SetupDiGetDeviceInterfaceDetailW(devInfoSet, &intfData, NULL, 0, &size, NULL))
                if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
                    text(L"LogiWheel: Error getting intf detail");
                    continue;
                }

            intfDetail = (SP_DEVICE_INTERFACE_DETAIL_DATA *)malloc(size);
            intfDetail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

            if (!SetupDiGetDeviceInterfaceDetailW(devInfoSet, &intfData, intfDetail, size, NULL, NULL)) {
                free(intfDetail);
                continue;
            }

            if (
                wcsstr(intfDetail->DevicePath, G25PATH)  != NULL ||
                wcsstr(intfDetail->DevicePath, DFGTPATH) != NULL ||
                wcsstr(intfDetail->DevicePath, G27PATH) != NULL
            ) {

                HANDLE file = CreateFileW(
                    intfDetail->DevicePath,
                    GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL
                );

                if (file == INVALID_HANDLE_VALUE) {
                    text(L"LogiWheel: Failed to open HID device");
                    free(intfDetail);
                    SetupDiDestroyDeviceInfoList(devInfoSet);
                    return;
                }

                DWORD written;

                if (!WriteFile(file, LOGI_WHEEL_HID_CMD, LOGI_WHEEL_HID_CMD_LEN, &written, NULL))
                    text(L"LogiWheel: Failed to write to HID device");
                else
                    text(L"LogiWheel: Range set to 900 deg via raw HID");

                CloseHandle(file);
                free(intfDetail);
                SetupDiDestroyDeviceInfoList(devInfoSet);
                return;

            }

            free(intfDetail);

        }

        text(L"Failed to locate Logitech wheel HID device, can't set range");
        SetupDiDestroyDeviceInfoList(devInfoSet);
        return;

    }

    text(L"Attempting to set range via LGS");

    UINT msgId = RegisterWindowMessage(L"LGS_Msg_SetOperatingRange");
    if (!msgId) {
        text(L"Failed to register LGS window message, can't set range..");
        return;
    }

    HWND LGSmsgHandler =
        FindWindowW(
            L"LCore_MessageHandler_{C464822E-04D1-4447-B918-6D5EB33E0E5D}",
            NULL
        );

    if (LGSmsgHandler == NULL) {
        text(L"Failed to locate LGS msg handler, can't set range..");
        return;
    }

    SendMessageW(LGSmsgHandler, msgId, prodId, 900);
    text(L"Range of Logitech wheel set to 900 deg via LGS");

}

BOOL CALLBACK EnumFFDevicesCallback(LPCDIDEVICEINSTANCE diDevInst, VOID *wnd) {

    UNREFERENCED_PARAMETER(wnd);

    if (lstrcmp(diDevInst->tszProductName, L"vJoy Device") == 0)
        return true;

    settings.addFfbDevice(diDevInst->guidInstance, diDevInst->tszProductName);
    debug(L"Adding DI device: %s", diDevInst->tszProductName);

    return true;

}

BOOL CALLBACK EnumObjectCallback(const LPCDIDEVICEOBJECTINSTANCE inst, VOID *dw) {

    UNREFERENCED_PARAMETER(inst);

    (*(int *)dw)++;
    return DIENUM_CONTINUE;

}

void enumDirectInput() {

    settings.clearFfbDevices();

    if (
        FAILED(
            DirectInput8Create(
                GetModuleHandle(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8,
                (VOID **)&pDI, nullptr
            )
        )
    ) {
        text(L"Failed to initialise DirectInput");
        return;
    }

    pDI->EnumDevices(
        DI8DEVCLASS_GAMECTRL, EnumFFDevicesCallback, settings.getDevWnd(),
        DIEDFL_ATTACHEDONLY | DIEDFL_FORCEFEEDBACK
    );

}

void initDirectInput() {

    DIDEVICEINSTANCE di;
    HRESULT hr;

    numButtons = numPov = 0;
    di.dwSize = sizeof(DIDEVICEINSTANCE);

    if (ffdevice && effect && ffdevice->GetDeviceInfo(&di) >= 0 && di.guidInstance == settings.getFfbDevice())
        return;

    releaseDirectInput();

    if (
        FAILED(
            DirectInput8Create(
                GetModuleHandle(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8,
                (VOID **)&pDI, nullptr
            )
        )
    ) {
        text(L"Failed to initialise DirectInput");
        return;
    }

    if (FAILED(pDI->CreateDevice(settings.getFfbDevice(), &ffdevice, nullptr))) {
        text(L"Failed to create DI device");
        text(L"Is it connected and powered on?");
        return;
    }
    if (FAILED(ffdevice->SetDataFormat(&c_dfDIJoystick))) {
        text(L"Failed to set DI device DataFormat!");
        return;
    }
    if (FAILED(ffdevice->SetCooperativeLevel(mainWnd, DISCL_EXCLUSIVE | DISCL_BACKGROUND))) {
        text(L"Failed to set DI device CooperativeLevel!");
        return;
    }

    if (FAILED(ffdevice->GetDeviceInfo(&di))) {
        text(L"Failed to get info for DI device!");
        return;
    }

    if (FAILED(ffdevice->EnumObjects(EnumObjectCallback, (VOID *)&numButtons, DIDFT_BUTTON))) {
        text(L"Failed to enumerate DI device buttons");
        return;
    }

    if (FAILED(ffdevice->EnumObjects(EnumObjectCallback, (VOID *)&numPov, DIDFT_POV))) {
        text(L"Failed to enumerate DI device povs");
        return;
    }

    if (FAILED(ffdevice->SetEventNotification(wheelEvent))) {
        text(L"Failed to set event notification on DI device");
        return;
    }

    DWORD vidpid = getDeviceVidPid(ffdevice);
    if (LOWORD(vidpid) == 0x046d) {
        logiWheel = true;
        setLogiWheelRange(HIWORD(vidpid));
    }
    else
        logiWheel = false;

    if (FAILED(ffdevice->Acquire())) {
        text(L"Failed to acquire DI device");
        return;
    }

    text(L"Acquired DI device with %d buttons and %d POV", numButtons, numPov);

    EnterCriticalSection(&effectCrit);

    if (FAILED(ffdevice->CreateEffect(GUID_Sine, &dieff, &effect, nullptr))) {
        text(L"Failed to create sine periodic effect");
        LeaveCriticalSection(&effectCrit);
        return;
    }

    if (!effect) {
        text(L"Effect creation failed");
        LeaveCriticalSection(&effectCrit);
        return;
    }

    hr = effect->SetParameters(&dieff, DIEP_TYPESPECIFICPARAMS | DIEP_START);
    if (hr == DIERR_NOTINITIALIZED || hr == DIERR_INPUTLOST || hr == DIERR_INCOMPLETEEFFECT || hr == DIERR_INVALIDPARAM)
        text(L"Error setting parameters of DIEFFECT: %d", hr);

    LeaveCriticalSection(&effectCrit);

}

void releaseDirectInput() {

    if (effect) {
        setFFB(0);
        EnterCriticalSection(&effectCrit);
        effect->Stop();
        effect->Release();
        effect = nullptr;
        LeaveCriticalSection(&effectCrit);
    }
    if (ffdevice) {
        ffdevice->Unacquire();
        ffdevice->Release();
        ffdevice = nullptr;
    }
    if (pDI) {
        pDI->Release();
        pDI = nullptr;
    }

}

void reacquireDIDevice() {

    if (ffdevice == nullptr) {
        debug(L"!! ffdevice was null during reacquire !!");
        return;
    }

    HRESULT hr;

    ffdevice->Unacquire();
    ffdevice->Acquire();

    EnterCriticalSection(&effectCrit);

    if (effect == nullptr) {
        if (FAILED(ffdevice->CreateEffect(GUID_Sine, &dieff, &effect, nullptr))) {
            text(L"Failed to create periodic effect during reacquire");
            LeaveCriticalSection(&effectCrit);
            return;
        }
    }

    hr = effect->SetParameters(&dieff, DIEP_TYPESPECIFICPARAMS | DIEP_START);
    if (hr == DIERR_NOTINITIALIZED || hr == DIERR_INPUTLOST || hr == DIERR_INCOMPLETEEFFECT || hr == DIERR_INVALIDPARAM)
        text(L"Error setting parameters of DIEFFECT during reacquire: 0x%x", hr);

    LeaveCriticalSection(&effectCrit);

}

inline int scaleTorque(float t) {

    return (int)(t * settings.getScaleFactor());

}

inline void setFFB(int mag) {

    if (!effect)
        return;

    if (mag <= -DI_MAX) {
        mag = -DI_MAX;
        clippedSamples++;
    }
    else if (mag >= DI_MAX) {
        mag = DI_MAX;
        clippedSamples++;
    }

    samples++;

    if (stopped && settings.getReduceWhenParked())
        mag /= 4;

    ffbMag = mag;

}

bool initVJD() {

    WORD verDll, verDrv;
    int maxVjDev;
    VjdStat vjdStatus = VJD_STAT_UNKN;

    if (!vJoyEnabled()) {
        text(L"vJoy not enabled!");
        return false;
    }
    else if (!DriverMatch(&verDll, &verDrv)) {
        if (verDrv < verDll) {
            text(L"vJoy driver version %04x < required version %04x!", verDrv, verDll);
            return false;
        }
    }
    
    text(L"vJoy driver version %04x init OK", verDrv);

    vjDev = 1;

    if (!GetvJoyMaxDevices(&maxVjDev)) {
        text(L"Failed to determine max number of vJoy devices");
        return false;
    }

    while (vjDev <= maxVjDev) {

        vjdStatus = GetVJDStatus(vjDev);

        if (vjdStatus == VJD_STAT_BUSY || vjdStatus == VJD_STAT_MISS)
            goto NEXT;
        if (!GetVJDAxisExist(vjDev, HID_USAGE_X))
            goto NEXT;
        if (!IsDeviceFfb(vjDev))
            goto NEXT;
        if (
            !IsDeviceFfbEffect(vjDev, HID_USAGE_CONST) ||
            !IsDeviceFfbEffect(vjDev, HID_USAGE_SINE)  ||
            !IsDeviceFfbEffect(vjDev, HID_USAGE_DMPR)  ||
            !IsDeviceFfbEffect(vjDev, HID_USAGE_FRIC)  ||
            !IsDeviceFfbEffect(vjDev, HID_USAGE_SPRNG)
        ) {
            text(L"vjDev %d: Not all required FFB effects are enabled", vjDev);
            text(L"Enable all FFB effects to use this device");
            goto NEXT;
        }
        break;

NEXT:
        vjDev++;

    }

    if (vjDev > maxVjDev) {
        text(L"Failed to find suitable vJoy device!");
        text(L"Create a device with an X axis and all FFB effects enabled");
        return false;
    }

    memset(&ffbPacket, 0 ,sizeof(ffbPacket));

    if (vjdStatus == VJD_STAT_OWN) {
        RelinquishVJD(vjDev);
        vjdStatus = GetVJDStatus(vjDev);
    }
    if (vjdStatus == VJD_STAT_FREE) {
        if (!AcquireVJD(vjDev, ffbEvent, &ffbPacket)) {
            text(L"Failed to acquire vJoy device %d!", vjDev);
            return false;
        }
    }
    else {
        text(L"ERROR: vJoy device %d status is %d", vjDev, vjdStatus);
        return false;
    }

    vjButtons = GetVJDButtonNumber(vjDev);
    vjPov = GetVJDContPovNumber(vjDev);
    vjPov += GetVJDDiscPovNumber(vjDev);

    text(L"Acquired vJoy device %d", vjDev);
    ResetVJD(vjDev);

    return true;

}

void initAll() {

    initVJD();
    initDirectInput();

}

void releaseAll() {

    releaseDirectInput();

    RelinquishVJD(vjDev);

}