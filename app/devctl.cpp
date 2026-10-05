// Speak2Mic device control (see devctl.h).
#include "devctl.h"
#include "applog.h"
#include "audiosvc.h"
#include <devpropdef.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <wchar.h>

// {4d36e96c-e325-11ce-bfc1-08002be10318}: "Sound, video and game controllers"
static const GUID kClassMedia = { 0x4d36e96c, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } };

static bool ChangeState(HDEVINFO set, SP_DEVINFO_DATA* info, DWORD state, bool* reboot)
{
    SP_PROPCHANGE_PARAMS pc = {};
    pc.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
    pc.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
    pc.StateChange = state;
    pc.Scope = DICS_FLAG_GLOBAL;
    bool ok = SetupDiSetClassInstallParamsW(set, info, &pc.ClassInstallHeader, sizeof(pc)) &&
              SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, info);
    if (!ok) AppLog(L"restart: state change %lu failed, error %lu", state, GetLastError());

    SP_DEVINSTALL_PARAMS_W dip = {};
    dip.cbSize = sizeof(dip);
    if (SetupDiGetDeviceInstallParamsW(set, info, &dip) && (dip.Flags & (DI_NEEDREBOOT | DI_NEEDRESTART)))
    {
        *reboot = true;
    }
    return ok;
}

bool S2mRestartDevice(bool* found, bool* reboot)
{
    *found = false;
    *reboot = false;
    HDEVINFO set = SetupDiGetClassDevsW(&kClassMedia, nullptr, nullptr, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    bool ok = true;
    SP_DEVINFO_DATA info;
    info.cbSize = sizeof(info);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &info); i++)
    {
        wchar_t ids[1024] = {};
        if (!SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_HARDWAREID, nullptr, (BYTE*)ids, sizeof(ids) - 4, nullptr))
        {
            continue;
        }
        bool match = false;
        for (const wchar_t* p = ids; *p; p += wcslen(p) + 1)
        {
            if (_wcsicmp(p, S2M_HARDWARE_ID) == 0) match = true;
        }
        if (!match)
        {
            continue;
        }

        *found = true;
        if (!ChangeState(set, &info, DICS_DISABLE, reboot)) ok = false;
        if (!ChangeState(set, &info, DICS_ENABLE, reboot)) ok = false;
        AppLog(L"restart: disable + enable %ls%ls", ok ? L"done" : L"FAILED", *reboot ? L", reboot required" : L"");
    }
    SetupDiDestroyDeviceInfoList(set);
    return ok;
}

DWORD S2mGetParam(const wchar_t* name, DWORD def)
{
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, S2M_PARAMS_KEY, name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return def;
    return value;
}

bool S2mSetParam(const wchar_t* name, DWORD value)
{
    HKEY key;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, S2M_PARAMS_KEY, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    bool ok = RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE*)&value, sizeof(value)) == ERROR_SUCCESS;
    RegCloseKey(key);
    return ok;
}

// Device Manager name of a device node: DEVPKEY_Device_FriendlyName (endpoint nodes created by the audio
// service keep it only in the property store, SPDRP_FRIENDLYNAME fails for them), else the description.
static const DEVPROPKEY kDevFriendlyName = { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };
static const DEVPROPKEY kDevDeviceDesc = { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 2 };

static bool GetNodeName(HDEVINFO set, SP_DEVINFO_DATA* info, wchar_t* out, DWORD bytes)
{
    DEVPROPTYPE type = 0;
    out[0] = 0;
    if (SetupDiGetDevicePropertyW(set, info, &kDevFriendlyName, &type, (BYTE*)out, bytes - 2, nullptr, 0) &&
        type == DEVPROP_TYPE_STRING && out[0])
        return true;
    if (SetupDiGetDeviceRegistryPropertyW(set, info, SPDRP_FRIENDLYNAME, nullptr, (BYTE*)out, bytes - 2, nullptr) && out[0])
        return true;
    return SetupDiGetDevicePropertyW(set, info, &kDevDeviceDesc, &type, (BYTE*)out, bytes - 2, nullptr, 0) &&
           type == DEVPROP_TYPE_STRING && out[0];
}

// Not declared by the MinGW headers (exported by setupapi.dll since Windows Vista).
#ifdef __MINGW32__
extern "C" WINSETUPAPI BOOL WINAPI SetupDiSetDevicePropertyW(HDEVINFO, PSP_DEVINFO_DATA, const DEVPROPKEY*, DEVPROPTYPE,
                                                              const BYTE*, DWORD, DWORD);
#endif

static bool SetNodeName(HDEVINFO set, SP_DEVINFO_DATA* info, const wchar_t* name)
{
    DWORD bytes = (DWORD)((wcslen(name) + 1) * sizeof(wchar_t));
    if (SetupDiSetDevicePropertyW(set, info, &kDevFriendlyName, DEVPROP_TYPE_STRING, (const BYTE*)name, bytes, 0)) return true;
    return SetupDiSetDeviceRegistryPropertyW(set, info, SPDRP_FRIENDLYNAME, (const BYTE*)name, bytes) != FALSE;
}

// True for endpoints of this driver, whatever their name (early builds were called "Virtual Audio Cable" with
// "Cable N In/Out" endpoints; users rename them): Windows records the KS filter behind each endpoint in
// MMDevices\Audio\<Render|Capture>\{id}\Properties, value {b3f8fa53-0004-438e-9003-51a46e139bfc},2, e.g.
// "{2}.\\?\root#media#0001#{6994ad04-...}\topocapture0". Ours: a ROOT\MEDIA device with a
// TopoRender/TopoCapture filter. Without that record: the "(Speak2Mic)" name suffix.
static bool IsOurEndpoint(const wchar_t* instanceId, const wchar_t* name)
{
    // Instance ID: SWD\MMDEVAPI\{0.0.0.00000000}.{guid} (render) or {0.0.1.00000000}.{guid} (capture).
    const wchar_t* p = wcsstr(instanceId, L"{0.0.");
    if (p && (p[5] == L'0' || p[5] == L'1'))
    {
        const wchar_t* guid = wcschr(p + 1, L'{');
        if (guid)
        {
            wchar_t key[256];
            _snwprintf(key, 256, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\%ls\\%ls\\Properties",
                       p[5] == L'0' ? L"Render" : L"Capture", guid);
            key[255] = 0;
            wchar_t path[512] = {};
            DWORD size = sizeof(path) - 2;
            if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"{b3f8fa53-0004-438e-9003-51a46e139bfc},2", RRF_RT_REG_SZ, nullptr,
                             path, &size) == ERROR_SUCCESS)
            {
                _wcslwr(path);
                if (wcsstr(path, L"root#media#") && (wcsstr(path, L"\\toporender") || wcsstr(path, L"\\topocapture")))
                    return true;
            }
        }
    }
    return wcsstr(name, L"(Speak2Mic)") != nullptr;
}

int S2mStaleEndpoints(bool remove)
{
    // {c166523c-fe0c-4a94-a586-f1a80cfbbf3e}: AudioEndpoint device class
    static const GUID kAudioEndpointClass = { 0xc166523c, 0xfe0c, 0x4a94, { 0xa5, 0x86, 0xf1, 0xa8, 0x0c, 0xfb, 0xbf, 0x3e } };
    HDEVINFO set = SetupDiGetClassDevsW(&kAudioEndpointClass, nullptr, nullptr, 0);     // present and not present
    if (set == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    SP_DEVINFO_DATA info;
    info.cbSize = sizeof(info);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &info); i++)
    {
        wchar_t name[256] = {};
        GetNodeName(set, &info, name, sizeof(name));
        wchar_t instanceId[256] = {};
        SetupDiGetDeviceInstanceIdW(set, &info, instanceId, 256, nullptr);
        if (!IsOurEndpoint(instanceId, name)) continue;
        ULONG status = 0, problem = 0;
        if (CM_Get_DevNode_Status(&status, &problem, info.DevInst, 0) == CR_SUCCESS) continue;     // present: keep
        if (!remove)
        {
            n++;
            continue;
        }
        bool ok = SetupDiCallClassInstaller(DIF_REMOVE, set, &info) != FALSE;
        AppLog(L"stale endpoint \"%ls\" (%ls): %ls", name, instanceId, ok ? L"removed" : L"remove FAILED");
        if (ok) n++;
    }
    SetupDiDestroyDeviceInfoList(set);
    return n;
}

int S2mSyncEndpointNames(bool fix)
{
    // Present endpoint device nodes: SWD\MMDEVAPI\<endpoint id>. Device Manager shows their FriendlyName,
    // which renaming an endpoint (IPolicyConfig, the same as Sound settings) does not update.
    static const GUID kAudioEndpointClass = { 0xc166523c, 0xfe0c, 0x4a94, { 0xa5, 0x86, 0xf1, 0xa8, 0x0c, 0xfb, 0xbf, 0x3e } };
    static const PROPERTYKEY kFriendlyName = {
        { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };
    static const PROPERTYKEY kInterfaceName = {
        { 0x026e516e, 0xb814, 0x414b, { 0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22 } }, 2 };
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en)))
    {
        if (SUCCEEDED(co)) CoUninitialize();
        return 0;
    }
    int n = 0;
    HDEVINFO set = SetupDiGetClassDevsW(&kAudioEndpointClass, nullptr, nullptr, DIGCF_PRESENT);
    SP_DEVINFO_DATA info;
    info.cbSize = sizeof(info);
    for (DWORD i = 0; set != INVALID_HANDLE_VALUE && SetupDiEnumDeviceInfo(set, i, &info); i++)
    {
        wchar_t instanceId[256] = {};
        SetupDiGetDeviceInstanceIdW(set, &info, instanceId, 256, nullptr);
        const wchar_t* prefix = L"SWD\\MMDEVAPI\\";
        if (_wcsnicmp(instanceId, prefix, wcslen(prefix)) != 0) continue;
        wchar_t id[200];
        wcsncpy(id, instanceId + wcslen(prefix), 199);
        id[199] = 0;
        _wcslwr(id);                                    // endpoint ids are lower case
        IMMDevice* dev = nullptr;
        if (FAILED(en->GetDevice(id, &dev))) continue;
        IPropertyStore* ps = nullptr;
        wchar_t want[256] = {}, adapter[64] = {};
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps)))
        {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(ps->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR) wcsncpy(want, v.pwszVal, 255);
            PropVariantClear(&v);
            if (SUCCEEDED(ps->GetValue(kInterfaceName, &v)) && v.vt == VT_LPWSTR) wcsncpy(adapter, v.pwszVal, 63);
            PropVariantClear(&v);
            ps->Release();
        }
        dev->Release();
        if (_wcsicmp(adapter, L"Speak2Mic") != 0 || !want[0]) continue;     // not ours
        wchar_t shown[256] = {};
        GetNodeName(set, &info, shown, sizeof(shown));
        if (wcscmp(shown, want) == 0) continue;
        if (!fix)
        {
            n++;
            continue;
        }
        bool ok = SetNodeName(set, &info, want);
        AppLog(L"Device Manager name \"%ls\" -> \"%ls\": %ls (%lu)", shown, want, ok ? L"updated" : L"update FAILED",
               ok ? 0 : GetLastError());
        if (ok) n++;
    }
    if (set != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(set);
    en->Release();
    if (SUCCEEDED(co)) CoUninitialize();
    return n;
}

void S2mLogEndpointNodes()
{
    static const GUID kAudioEndpointClass = { 0xc166523c, 0xfe0c, 0x4a94, { 0xa5, 0x86, 0xf1, 0xa8, 0x0c, 0xfb, 0xbf, 0x3e } };
    HDEVINFO set = SetupDiGetClassDevsW(&kAudioEndpointClass, nullptr, nullptr, 0);
    if (set == INVALID_HANDLE_VALUE)
    {
        AppLog(L"[endpoints] cannot list the AudioEndpoint class (%lu)", GetLastError());
        return;
    }
    SP_DEVINFO_DATA info;
    info.cbSize = sizeof(info);
    DWORD i = 0;
    for (; SetupDiEnumDeviceInfo(set, i, &info); i++)
    {
        wchar_t name[256] = {}, instanceId[256] = {};
        GetNodeName(set, &info, name, sizeof(name));
        SetupDiGetDeviceInstanceIdW(set, &info, instanceId, 256, nullptr);
        ULONG status = 0, problem = 0;
        bool present = CM_Get_DevNode_Status(&status, &problem, info.DevInst, 0) == CR_SUCCESS;
        AppLog(L"[endpoints] %ls | %ls | %ls | %ls", present ? L"present" : L"NOT present", IsOurEndpoint(instanceId, name) ? L"ours" : L"-",
               name, instanceId);
    }
    AppLog(L"[endpoints] %lu node(s) in the AudioEndpoint class", i);
    SetupDiDestroyDeviceInfoList(set);
}

// ---------------------------------------------------------------------------
// Orphan endpoints: the audio service keeps a registry record (MMDevices\Audio\<Render|Capture>\{guid}) for every
// endpoint it ever created and brings its device node back after each start, "present" for PnP. Every earlier
// reinstall of Speak2Mic (device removed and created again) left one pair behind.

int S2mFindOrphanEndpoints(wchar_t (*ids)[80], int max, bool log)
{
    static const GUID kAudioEndpointClass = { 0xc166523c, 0xfe0c, 0x4a94, { 0xa5, 0x86, 0xf1, 0xa8, 0x0c, 0xfb, 0xbf, 0x3e } };
    static const PROPERTYKEY kInterfaceName = {
        { 0x026e516e, 0xb814, 0x414b, { 0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22 } }, 2 };
    // 1. The active Speak2Mic endpoints (kept).
    wchar_t active[16][80];
    int nActive = 0;
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IMMDeviceEnumerator* en = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en)))
    {
        IMMDeviceCollection* coll = nullptr;
        if (SUCCEEDED(en->EnumAudioEndpoints(eAll, DEVICE_STATE_ACTIVE, &coll)))
        {
            UINT count = 0;
            coll->GetCount(&count);
            for (UINT i = 0; i < count && nActive < 16; i++)
            {
                IMMDevice* dev = nullptr;
                if (FAILED(coll->Item(i, &dev))) continue;
                IPropertyStore* ps = nullptr;
                wchar_t adapter[64] = {};
                if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps)))
                {
                    PROPVARIANT v;
                    PropVariantInit(&v);
                    if (SUCCEEDED(ps->GetValue(kInterfaceName, &v)) && v.vt == VT_LPWSTR) wcsncpy(adapter, v.pwszVal, 63);
                    PropVariantClear(&v);
                    ps->Release();
                }
                LPWSTR id = nullptr;
                if (_wcsicmp(adapter, L"Speak2Mic") == 0 && SUCCEEDED(dev->GetId(&id)))
                {
                    wcsncpy(active[nActive], id, 79);
                    active[nActive][79] = 0;
                    _wcslwr(active[nActive]);
                    nActive++;
                    CoTaskMemFree(id);
                }
                dev->Release();
            }
            coll->Release();
        }
        en->Release();
    }
    if (SUCCEEDED(co)) CoUninitialize();

    // While the device restarts, an endpoint is briefly not active and would look like an orphan (the autotest
    // saw the live microphone listed right after "s2mctl set"): judge only when both of ours are up.
    bool render = false, capture = false;
    for (int a = 0; a < nActive; a++)
    {
        if (wcsncmp(active[a], L"{0.0.0.", 7) == 0) render = true;
        if (wcsncmp(active[a], L"{0.0.1.", 7) == 0) capture = true;
    }
    if (!render || !capture)
    {
        if (log) AppLog(L"orphan endpoints: not judged (active Speak2Mic endpoints: %d, speaker %ls, microphone %ls)", nActive,
                        render ? L"up" : L"DOWN", capture ? L"up" : L"DOWN");
        return -1;
    }

    // 2. Every other endpoint node of ours.
    HDEVINFO set = SetupDiGetClassDevsW(&kAudioEndpointClass, nullptr, nullptr, 0);
    if (set == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    SP_DEVINFO_DATA info;
    info.cbSize = sizeof(info);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &info) && n < max; i++)
    {
        wchar_t name[256] = {}, instanceId[256] = {};
        GetNodeName(set, &info, name, sizeof(name));
        SetupDiGetDeviceInstanceIdW(set, &info, instanceId, 256, nullptr);
        const wchar_t* prefix = L"SWD\\MMDEVAPI\\";
        if (_wcsnicmp(instanceId, prefix, wcslen(prefix)) != 0 || !IsOurEndpoint(instanceId, name)) continue;
        wchar_t id[80];
        wcsncpy(id, instanceId + wcslen(prefix), 79);
        id[79] = 0;
        _wcslwr(id);
        bool keep = false;
        for (int a = 0; a < nActive; a++)
            if (wcscmp(active[a], id) == 0) keep = true;
        if (keep) continue;
        wcscpy(ids[n++], id);
        if (log) AppLog(L"orphan endpoint \"%ls\" %ls", name, id);
    }
    SetupDiDestroyDeviceInfoList(set);

    // 3. Records of ours in the endpoint builder's registry without a device node right now (it recreates the
    //    node from the record after its next start, e.g. after a device restart).
    for (int flow = 0; flow < 2 && n < max; flow++)
    {
        wchar_t path[128];
        _snwprintf(path, 128, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\%ls", flow ? L"Capture" : L"Render");
        path[127] = 0;
        HKEY parent;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &parent) != ERROR_SUCCESS) continue;
        wchar_t guid[64];
        for (DWORD k = 0; n < max; k++)
        {
            DWORD len = 64;
            if (RegEnumKeyExW(parent, k, guid, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            wchar_t id[80];
            _snwprintf(id, 80, L"{0.0.%d.00000000}.%ls", flow, guid);
            id[79] = 0;
            _wcslwr(id);
            bool known = false;
            for (int a = 0; a < nActive && !known; a++) known = wcscmp(active[a], id) == 0;
            for (int o = 0; o < n && !known; o++) known = wcscmp(ids[o], id) == 0;
            if (known) continue;
            wchar_t sub[128], adapter[64] = {}, name[256] = {};
            _snwprintf(sub, 128, L"%ls\\Properties", guid);
            sub[127] = 0;
            DWORD size = sizeof(adapter) - 2;
            RegGetValueW(parent, sub, L"{026e516e-b814-414b-83cd-856d6fef4822},2", RRF_RT_REG_SZ, nullptr, adapter, &size);
            if (_wcsicmp(adapter, L"Speak2Mic") != 0) continue;
            size = sizeof(name) - 2;
            RegGetValueW(parent, sub, L"{a45c254e-df1c-4efd-8020-67d146a850e0},2", RRF_RT_REG_SZ, nullptr, name, &size);
            wcscpy(ids[n++], id);
            if (log) AppLog(L"orphan endpoint record \"%ls\" %ls (no device node)", name, id);
        }
        RegCloseKey(parent);
    }
    if (log) AppLog(L"orphan endpoints: %d (active Speak2Mic endpoints kept: %d)", n, nActive);
    return n;
}

static void EnablePrivilege(const wchar_t* name)
{
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return;
    TOKEN_PRIVILEGES tp = {};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (LookupPrivilegeValueW(nullptr, name, &tp.Privileges[0].Luid))
        AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr);
    CloseHandle(token);
}

typedef LONG (NTAPI* NtDeleteKeyFn)(HANDLE);

// Deletes a key and its subkeys although its ACL does not let administrators write (the audio service owns
// MMDevices): opened for backup/restore (SeBackupPrivilege/SeRestorePrivilege), deleted by handle.
static bool DeleteKeyTreePrivileged(HKEY parent, const wchar_t* sub, NtDeleteKeyFn ntDeleteKey)
{
    HKEY key;
    if (RegCreateKeyExW(parent, sub, 0, nullptr, REG_OPTION_BACKUP_RESTORE, KEY_ALL_ACCESS, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    wchar_t child[256];
    for (;;)
    {
        DWORD len = 256;
        if (RegEnumKeyExW(key, 0, child, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        if (!DeleteKeyTreePrivileged(key, child, ntDeleteKey)) break;
    }
    bool ok = ntDeleteKey(key) == 0;
    RegCloseKey(key);
    return ok;
}

int S2mRemoveEndpoints(wchar_t (*ids)[80], int n)
{
    EnablePrivilege(L"SeBackupPrivilege");
    EnablePrivilege(L"SeRestorePrivilege");
    NtDeleteKeyFn ntDeleteKey = (NtDeleteKeyFn)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtDeleteKey");
    static const GUID kAudioEndpointClass = { 0xc166523c, 0xfe0c, 0x4a94, { 0xa5, 0x86, 0xf1, 0xa8, 0x0c, 0xfb, 0xbf, 0x3e } };
    int removed = 0;
    for (int i = 0; i < n; i++)
    {
        // "{0.0.0.00000000}.{guid}": render ("0.0.0") or capture ("0.0.1"), then the registry key name.
        const wchar_t* guid = wcschr(ids[i] + 1, L'{');
        bool capture = wcsncmp(ids[i], L"{0.0.1.", 7) == 0;
        bool regOk = false;
        if (guid && ntDeleteKey)
        {
            wchar_t parentPath[128];
            _snwprintf(parentPath, 128, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\%ls",
                       capture ? L"Capture" : L"Render");
            parentPath[127] = 0;
            HKEY parent;
            if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, parentPath, 0, nullptr, REG_OPTION_BACKUP_RESTORE, KEY_ALL_ACCESS,
                                nullptr, &parent, nullptr) == ERROR_SUCCESS)
            {
                regOk = DeleteKeyTreePrivileged(parent, guid, ntDeleteKey);
                RegCloseKey(parent);
            }
        }
        // The device node itself (Device Manager).
        bool nodeOk = false;
        wchar_t instanceId[128];
        _snwprintf(instanceId, 128, L"SWD\\MMDEVAPI\\%ls", ids[i]);
        instanceId[127] = 0;
        HDEVINFO set = SetupDiCreateDeviceInfoList(&kAudioEndpointClass, nullptr);
        SP_DEVINFO_DATA info;
        info.cbSize = sizeof(info);
        if (set != INVALID_HANDLE_VALUE && SetupDiOpenDeviceInfoW(set, instanceId, nullptr, 0, &info))
            nodeOk = SetupDiCallClassInstaller(DIF_REMOVE, set, &info) != FALSE;
        if (set != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(set);
        AppLog(L"remove endpoint %ls: registry %ls, device node %ls", ids[i], regOk ? L"deleted" : L"FAILED",
               nodeOk ? L"removed" : L"FAILED");
        if (regOk || nodeOk) removed++;
    }
    return removed;
}


int S2mCleanupOrphanEndpoints(DWORD waitMs)
{
    static wchar_t ids[128][80], again[128][80];
    int n = -1;
    DWORD t0 = GetTickCount();
    while ((n = S2mFindOrphanEndpoints(ids, 128, false)) < 0 && GetTickCount() - t0 < waitMs) Sleep(250);
    if (n <= 0) return 0;                             // none, or not judged in time
    S2mFindOrphanEndpoints(ids, 128, true);           // with the names in the log
    // Only what a second look a moment later still finds (never an endpoint that was just being recreated).
    Sleep(1000);
    int m = S2mFindOrphanEndpoints(again, 128, false), kept = 0;
    if (m < 0) m = 0;                                 // not judged: confirms nothing
    for (int i = 0; i < n; i++)
    {
        bool both = false;
        for (int j = 0; j < m; j++) both = both || wcscmp(ids[i], again[j]) == 0;
        if (both && kept != i) wcscpy(ids[kept], ids[i]);
        if (both) kept++;
    }
    if (!kept) return 0;
    // The audio endpoint builder recreates what it knows about: stop it while its records are deleted.
    AudioServices svc;
    AudioStopServices(true, &svc);
    int removed = S2mRemoveEndpoints(ids, kept);
    AudioStartServices(&svc);
    AppLog(L"leftover endpoint records removed: %d", removed);
    return removed;
}

// ---------------------------------------------------------------------------
// Ready to work?

bool S2mSecureBootEnabled()
{
    DWORD v = 0, size = sizeof(v);
    return RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\SecureBoot\\State",
                        L"UEFISecureBootEnabled", RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS && v != 0;
}

bool S2mTestSigningEnabled()
{
    wchar_t opts[1024] = {};
    DWORD size = sizeof(opts);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control", L"SystemStartOptions",
                     RRF_RT_REG_SZ, nullptr, opts, &size) != ERROR_SUCCESS)
    {
        return false;
    }
    _wcsupr(opts);
    return wcsstr(opts, L"TESTSIGNING") != nullptr;
}

bool S2mDriverInstalled()
{
    HDEVINFO set = SetupDiGetClassDevsW(&kClassMedia, nullptr, nullptr, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    SP_DEVINFO_DATA info;
    info.cbSize = sizeof(info);
    for (DWORD i = 0; !found && SetupDiEnumDeviceInfo(set, i, &info); i++)
    {
        wchar_t ids[1024] = {};
        if (!SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_HARDWAREID, nullptr, (BYTE*)ids, sizeof(ids) - 4, nullptr))
            continue;
        for (const wchar_t* p = ids; *p; p += wcslen(p) + 1)
            if (_wcsicmp(p, S2M_HARDWARE_ID) == 0) found = true;
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

S2mNotReady S2mCheckReady()
{
    bool secure = S2mSecureBootEnabled(), test = S2mTestSigningEnabled(), driver = S2mDriverInstalled();
    S2mNotReady r = secure ? S2mNotReadySecureBoot : !test ? S2mNotReadyTestMode : !driver ? S2mNotReadyDriver : S2mReadyOk;
    if (r != S2mReadyOk)
        AppLog(L"not ready to work: Secure Boot %ls, test signing %ls, driver %ls", secure ? L"ON" : L"off", test ? L"on" : L"OFF",
               driver ? L"installed" : L"NOT installed");
    return r;
}

const wchar_t* S2mNotReadyTextEn(S2mNotReady reason)
{
    switch (reason)
    {
    case S2mNotReadySecureBoot:
        return L"Speak2Mic cannot work: Secure Boot is on, and the test-signed Speak2Mic driver does not load with it. "
               L"Turn Secure Boot off in the UEFI settings, then run Speak2Mic-Setup.exe.";
    case S2mNotReadyTestMode:
        return L"Speak2Mic cannot work: Windows test signing mode is off, and the Speak2Mic driver does not load without it. "
               L"Run Speak2Mic-Setup.exe, click \"Enable test mode\" and restart the computer.";
    case S2mNotReadyDriver:
        return L"Speak2Mic cannot work: the Speak2Mic driver is not installed. Run Speak2Mic-Setup.exe and click \"Install\".";
    default:
        return L"";
    }
}
