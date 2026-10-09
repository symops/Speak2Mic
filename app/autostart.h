// Start the control panel with Windows (in the tray: "/t"): HKCU\Software\Microsoft\Windows\CurrentVersion\Run
// "Speak2Mic". On by default: the installer turns it on unless the user switched it off in the panel
// (HKCU\Software\Speak2Mic\Autostart = 0). Shared by the panel and the installer.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>

#define S2M_RUN_KEY   L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define S2M_RUN_VALUE L"Speak2Mic"
// WM_CLOSE wParam: the installer (or a language change) closes the panel: no question, unsaved settings are applied
#define S2M_CLOSE_FOR_SETUP 0x53324D31

inline bool S2mAutostartEnabled()
{
    DWORD size = 0;
    return RegGetValueW(HKEY_CURRENT_USER, S2M_RUN_KEY, S2M_RUN_VALUE, RRF_RT_REG_SZ, nullptr, nullptr, &size) == ERROR_SUCCESS;
}

// The user's choice (true unless switched off).
inline bool S2mAutostartWanted()
{
    DWORD v = 1, size = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Speak2Mic", L"Autostart", RRF_RT_REG_DWORD, nullptr, &v, &size);
    return v != 0;
}

// on: run `panelExe` /t at logon; off: remove it. remember: store the choice (the panel's checkbox).
inline bool S2mSetAutostart(bool on, const wchar_t* panelExe, bool remember)
{
    HKEY key;
    bool ok = false;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, S2M_RUN_KEY, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS)
    {
        if (on)
        {
            wchar_t cmd[MAX_PATH + 16];
            _snwprintf(cmd, MAX_PATH + 16, L"\"%ls\" /t", panelExe);
            cmd[MAX_PATH + 15] = 0;
            ok = RegSetValueExW(key, S2M_RUN_VALUE, 0, REG_SZ, (const BYTE*)cmd, (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
        }
        else
        {
            LSTATUS rs = RegDeleteValueW(key, S2M_RUN_VALUE);
            ok = rs == ERROR_SUCCESS || rs == ERROR_FILE_NOT_FOUND;
        }
        RegCloseKey(key);
    }
    if (remember && RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Speak2Mic", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS)
    {
        DWORD v = on ? 1 : 0;
        RegSetValueExW(key, L"Autostart", 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
        RegCloseKey(key);
    }
    return ok;
}
