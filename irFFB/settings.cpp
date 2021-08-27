#include "settings.h"
#include <fstream>
#include <iostream>
#include <string>

HKEY Settings::getSettingsRegKey() {

    HKEY key;
    if (
        RegCreateKeyExW(
            HKEY_CURRENT_USER, SETTINGS_KEY, 0, nullptr,
            REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, nullptr, &key, nullptr
        )
    )
        return NULL;
    return key;
    
}

LSTATUS Settings::setRegSetting(HKEY key, wchar_t *name, int val) {
    
    DWORD sz = sizeof(int);
    return RegSetValueExW(key, name, 0, REG_DWORD, (BYTE *)&val, sz);

}

LSTATUS Settings::setRegSetting(HKEY key, wchar_t *name, float val) {

    DWORD sz = sizeof(float);
    return RegSetValueExW(key, name, 0, REG_DWORD, (BYTE *)&val, sz);

}


LSTATUS Settings::setRegSetting(HKEY key, wchar_t *name, bool val) {

    DWORD sz = sizeof(DWORD);
    DWORD dw = val ? 1 : 0;
    return RegSetValueExW(key, name, 0, REG_DWORD, (BYTE *)&dw, sz);

}

int Settings::getRegSetting(HKEY key, wchar_t *name, int def) {

    int val;
    DWORD sz = sizeof(int);

    if (RegGetValue(key, nullptr, name, RRF_RT_REG_DWORD, nullptr, &val, &sz))
        return def;
    else
        return val;

}

float Settings::getRegSetting(HKEY key, wchar_t *name, float def) {

    float val;
    DWORD sz = sizeof(float);

    if (RegGetValue(key, nullptr, name, RRF_RT_REG_DWORD, nullptr, &val, &sz))
        return def;
    else
        return val;

}

bool Settings::getRegSetting(HKEY key, wchar_t *name, bool def) {

    DWORD val, sz = sizeof(DWORD);

    if (RegGetValue(key, nullptr, name, RRF_RT_REG_DWORD, nullptr, &val, &sz))
        return def;
    else
        return val > 0;

}
    
Settings::Settings()
{
    memset(ffdevices, 0, MAX_FFB_DEVICES * sizeof(GUID));
    ffdeviceIdx = 0;
    devGuid = GUID_NULL;
}

void Settings::setDevWnd(HWND wnd) { devWnd = wnd; }
        
void Settings::setMinWnd(sWins_t *wnd) {
    minWnd = wnd; 
    SendMessage(minWnd->trackbar, TBM_SETRANGE, TRUE, MAKELPARAM(0, 20));
}
        
void Settings::setMaxWnd(sWins_t *wnd) { 
    maxWnd = wnd;
    SendMessage(maxWnd->trackbar, TBM_SETRANGE, TRUE, MAKELPARAM(MIN_MAXFORCE, 100));
}

void Settings::setGLatWnd(sWins_t* wnd) { gLatWnd = wnd; }
void Settings::setUndersteerWnd(sWins_t* wnd) { understeerWnd = wnd; }
void Settings::setSopWnd(sWins_t *wnd) { sopWnd = wnd; }
void Settings::setSopOffsetWnd(sWins_t *wnd) { sopOffsetWnd = wnd; }
void Settings::setReduceWhenParkedWnd(HWND wnd) { reduceWhenParkedWnd = wnd; }
void Settings::setRunOnStartupWnd(HWND wnd) { runOnStartupWnd = wnd; }
void Settings::setStartMinimisedWnd(HWND wnd) { startMinimisedWnd = wnd; }
void Settings::setDebugWnd(HWND wnd) { debugWnd = wnd; }

void Settings::clearFfbDevices() {
    memset(ffdevices, 0, sizeof(ffdevices));
    ffdeviceIdx = 0;
    SendMessage(devWnd, CB_RESETCONTENT, 0, 0);
}

void Settings::addFfbDevice(GUID dev, const wchar_t *name) {
    
    if (ffdeviceIdx == MAX_FFB_DEVICES)
        return;
    ffdevices[ffdeviceIdx++] = dev;
    SendMessage(devWnd, CB_ADDSTRING, 0, LPARAM(name));
    if (devGuid == dev)
        setFfbDevice(ffdeviceIdx - 1);

}

void Settings::setFfbDevice(int idx) {
    if (idx >= ffdeviceIdx)
        return;
    SendMessage(devWnd, CB_SETCURSEL, idx, 0);
    devGuid = ffdevices[idx];
    initDirectInput();
}

bool Settings::isFfbDevicePresent() {
    for (int i = 0; i < ffdeviceIdx; i++)
        if (ffdevices[i] == devGuid)
            return true;
    return false;
}
         
bool Settings::setMaxForce(int max, HWND wnd) {
    if (max < MIN_MAXFORCE || max > MAX_MAXFORCE)
        return false;
    maxForce = max;
    if (wnd != maxWnd->trackbar)
        SendMessage(maxWnd->trackbar, TBM_SETPOS, TRUE, maxForce);
    if (wnd != maxWnd->value) {
        swprintf_s(strbuf, L"%d", max);
        SendMessage(maxWnd->value, WM_SETTEXT, NULL, LPARAM(strbuf));
    }
    scaleFactor = ((float)maxForce) / 100.0f;
    return true;
}

bool Settings::setGLatFactor(float factor, HWND wnd) {
    if (factor < 0.0f || factor > 100.0f)
        return false;
    gLatFactor = factor;
    if (wnd != gLatWnd->trackbar)
        SendMessage(gLatWnd->trackbar, TBM_SETPOS, TRUE, (int)factor);
    if (wnd != gLatWnd->value) {
        swprintf_s(strbuf, L"%.1f", factor);
        SendMessage(gLatWnd->value, WM_SETTEXT, NULL, LPARAM(strbuf));
    }
    return true;
}

bool Settings::setUndersteerFactor(float factor, HWND wnd) {
    if (factor < 0.0f || factor > 100.0f)
        return false;
    understeerFactor = factor;
    if (wnd != understeerWnd->trackbar)
        SendMessage(understeerWnd->trackbar, TBM_SETPOS, TRUE, (int)factor);
    if (wnd != understeerWnd->value) {
        swprintf_s(strbuf, L"%.1f", factor);
        SendMessage(understeerWnd->value, WM_SETTEXT, NULL, LPARAM(strbuf));
    }
    return true;
}

bool Settings::setSopFactor(float factor, HWND wnd) {
    if (factor < 0.0f || factor > 100.0f)
        return false;
    sopFactor = factor;
    if (wnd != sopWnd->trackbar)
        SendMessage(sopWnd->trackbar, TBM_SETPOS, TRUE, (int)factor);
    if (wnd != sopWnd->value) {
        swprintf_s(strbuf, L"%.1f", factor);
        SendMessage(sopWnd->value, WM_SETTEXT, NULL, LPARAM(strbuf));
    }
    EnableWindow(sopOffsetWnd->trackbar, factor != 0);
    return true;
}

bool Settings::setSopOffset(float offset, HWND wnd) {
    if (offset < 0.0f || offset > 100.0f)
        return false;
    sopOffset = offset;
    if (wnd != sopOffsetWnd->trackbar)
        SendMessage(sopOffsetWnd->trackbar, TBM_SETPOS, TRUE, (int)offset);
    if (wnd != sopOffsetWnd->value) {
        swprintf_s(strbuf, L"%.1f", offset);
        SendMessage(sopOffsetWnd->value, WM_SETTEXT, NULL, LPARAM(strbuf));
    }
    return true;
}

void Settings::setReduceWhenParked(bool reduce) { 
    reduceWhenParked = reduce; 
    SendMessage(reduceWhenParkedWnd, BM_SETCHECK, reduce ? BST_CHECKED : BST_UNCHECKED, NULL);
}

void Settings::setRunOnStartup(bool run) { 

    HKEY regKey;
    wchar_t module[MAX_PATH];

    runOnStartup = run;
    SendMessage(runOnStartupWnd, BM_SETCHECK, run ? BST_CHECKED : BST_UNCHECKED, NULL);

    DWORD len = GetModuleFileNameW(NULL, module, MAX_PATH);
    
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_ON_STARTUP_KEY, 0, KEY_ALL_ACCESS, &regKey)) {
        text(L"Failed to open startup registry key");
        return;
    }

    if (run) {
        if (RegSetValueExW(regKey, L"irFFB", 0, REG_SZ, (BYTE *)&module, ++len * sizeof(wchar_t)))
            text(L"Failed to create startup registry value");
    }
    else
        RegDeleteValueW(regKey, L"irFFB");

    RegCloseKey(regKey);

}

void Settings::setStartMinimised(bool minimised) {
    startMinimised = minimised;
    SendMessage(startMinimisedWnd, BM_SETCHECK, minimised ? BST_CHECKED : BST_UNCHECKED, NULL);
}

void Settings::setDebug(bool enabled) {
    debug = enabled;
    SendMessage(debugWnd, BM_SETCHECK, enabled ? BST_CHECKED : BST_UNCHECKED, NULL);
}

float Settings::getSopOffsetSetting() {
    return sopOffset;
}

void Settings::readRegSettings(char *car) {

    wchar_t dguid[GUIDSTRING_MAX];
    DWORD dgsz = sizeof(dguid);
    HKEY key = getSettingsRegKey();

    if (key == NULL) {
        setReduceWhenParked(true);
        setStartMinimised(false);
        setRunOnStartup(false);
        return;
    }

        
    if (!RegGetValue(key, nullptr, L"device", RRF_RT_REG_SZ, nullptr, dguid, &dgsz))
        if (FAILED(IIDFromString(dguid, &devGuid)))
            devGuid = GUID_NULL;

    setReduceWhenParked(getRegSetting(key, L"reduceWhenParked", true));
    setRunOnStartup(getRegSetting(key, L"runOnStartup", false));
    setStartMinimised(getRegSetting(key, L"startMinimised", false));

    RegCloseKey(key);
    
}

void Settings::readGenericSettings() {

    wchar_t dguid[GUIDSTRING_MAX];
    DWORD dgsz = sizeof(dguid);
    HKEY key = getSettingsRegKey();

    if (key == NULL) {
        setMaxForce(50, (HWND)-1);
        setGLatFactor(0.0f, (HWND)-1);
        setSopFactor(0.0f, (HWND)-1);
        setSopOffset(0.0f, (HWND)-1);
        setUndersteerFactor(0.0f, (HWND)-1);
        return;
    }

    setMaxForce(getRegSetting(key, L"maxForce", 50), (HWND)-1);
    setGLatFactor(getRegSetting(key, L"gLatFactor", 0.0f), (HWND)-1);
    setSopFactor(getRegSetting(key, L"yawFactor", 0.0f), (HWND)-1);
    setSopOffset(getRegSetting(key, L"yawOffset", 0.0f), (HWND)-1);
    setUndersteerFactor(getRegSetting(key, L"usteerFactor", 0.0f), (HWND)-1);

    RegCloseKey(key);

}

void Settings::writeRegSettings() {

    wchar_t *guid;
    int len;
    HKEY key = getSettingsRegKey();

    if (key == NULL)
        return;

    if (SUCCEEDED(StringFromCLSID(devGuid, (LPOLESTR *)&guid))) {
        len = (lstrlenW(guid) + 1) * sizeof(wchar_t);
        RegSetValueEx(key, L"device", 0, REG_SZ, (BYTE *)guid, len);
    }
    
    setRegSetting(key, L"reduceWhenParked", getReduceWhenParked());
    setRegSetting(key, L"runOnStartup", getRunOnStartup());
    setRegSetting(key, L"startMinimised", getStartMinimised());

    RegCloseKey(key);

}

void Settings::writeGenericSettings() {

    HKEY key = getSettingsRegKey();

    if (key == NULL)
        return;

    setRegSetting(key, L"gLatFactor", gLatFactor);
    setRegSetting(key, L"usteerFactor", understeerFactor);
    setRegSetting(key, L"yawFactor", sopFactor);
    setRegSetting(key, L"yawOffset", getSopOffsetSetting());
    setRegSetting(key, L"maxForce", maxForce);

    RegCloseKey(key);

}

void Settings::readSettingsForCar(char *car) {

}

void Settings::writeSettingsForCar(char *car) {

}

PWSTR Settings::getIniPath() {

    PWSTR docsPath;
    wchar_t *path;

    if (SHGetKnownFolderPath(FOLDERID_Documents, 0, NULL, &docsPath) != S_OK)
        return nullptr;

    path = new wchar_t[lstrlen(docsPath) + lstrlen(INI_PATH) + 1];

    lstrcpyW(path, docsPath);
    lstrcatW(path, INI_PATH);
    CoTaskMemFree(docsPath);

    return path;

}

PWSTR Settings::getLogPath() {

    PWSTR docsPath;
    wchar_t buf[64];
    wchar_t *path;
    SYSTEMTIME lt;

    if (SHGetKnownFolderPath(FOLDERID_Documents, 0, NULL, &docsPath) != S_OK)
        return nullptr;

    GetLocalTime(&lt);
    
    lstrcpyW(buf, L"\\irFFB-");
    int len = wcslen(buf) * sizeof(wchar_t);
    StringCbPrintf(
        buf + wcslen(buf), sizeof(buf) - len, L"%d-%02d-%02d-%02d-%02d-%02d.log",
        lt.wYear, lt.wMonth, lt.wDay, lt.wHour, lt.wMinute, lt.wSecond
    );

    path = new wchar_t[lstrlen(docsPath) + lstrlen(buf) + 1];

    lstrcpyW(path, docsPath);
    lstrcatW(path, buf);
    CoTaskMemFree(docsPath);

    return path;

}

void Settings::writeWithNewline(std::ofstream &file, char *buf) {
    size_t len = strlen(buf);
    buf[len] = '\n';
    file.write(buf, ++len);
}