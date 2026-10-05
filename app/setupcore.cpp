// Shared install/uninstall logic (see setupcore.h).
#include "setupcore.h"
#include "applog.h"
#include "devctl.h"
#include "audiosvc.h"
#include <devpropdef.h>
#include "lang.h"
#include "../driver/version.h"     // S2M_AUTHOR (the INF provider)
#include <setupapi.h>
#include <newdev.h>
#include <wincrypt.h>
#include <cfgmgr32.h>
#include <stdio.h>
#include <wchar.h>

static const wchar_t kHardwareId[] = L"ROOT\\Speak2Mic";

static void Log(SetupLog log, void* ctx, const wchar_t* fmt, ...)
{
    if (!log) return;
    wchar_t buf[512];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf(buf, 512, fmt, args);
    va_end(args);
    buf[511] = 0;
    log(ctx, buf);
}

// Explanations for SetupAPI / trust errors that FormatMessage has no text for.
static const wchar_t* KnownError(DWORD err)
{
    switch (err)
    {
    case 0xE000022F: return TR(L"в пакете драйвера нет подписанного каталога (.cat)");
    case 0xE0000247: return TR(L"Windows не смогла добавить пакет в хранилище драйверов");
    case 0xE0000203: return TR(L"не найден подходящий драйвер в INF");
    case 0x800B0100: return TR(L"у файла нет цифровой подписи");
    case 0x800B0109: return TR(L"сертификат подписи не доверенный (корневой сертификат не установлен)");
    case 0x800B0110: return TR(L"сертификат нельзя использовать для этой цели");
    case 0x80096010: return TR(L"подпись повреждена или файл изменён после подписи");
    case 0x800B0101: return TR(L"срок действия сертификата истёк или ещё не начался (проверьте часы)");
    default: return nullptr;
    }
}

static void LogError(SetupLog log, void* ctx, const wchar_t* what, DWORD err)
{
    const wchar_t* known = KnownError(err);
    if (known)
    {
        Log(log, ctx, TR(L"Ошибка: %ls (0x%08lX): %ls. Подробности: C:\\Windows\\INF\\setupapi.dev.log"),
            what, (unsigned long)err, known);
        return;
    }
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, err, 0, (LPWSTR)&msg, 0, nullptr);
    if (msg)
    {
        for (wchar_t* p = msg; *p; p++)
            if (*p == L'\r' || *p == L'\n') *p = L' ';
    }
    Log(log, ctx, TR(L"Ошибка: %ls (0x%08lX) %ls"), what, (unsigned long)err, msg ? msg : L"");
    if (msg) LocalFree(msg);
}

// ---------------------------------------------------------------------------
// Certificate

static bool AddCertificate(const wchar_t* path, const wchar_t* storeName, SetupLog log, void* ctx)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
    {
        LogError(log, ctx, TR(L"не найден сертификат"), GetLastError());
        return false;
    }
    BYTE data[8192];
    DWORD len = 0;
    BOOL read = ReadFile(f, data, sizeof(data), &len, nullptr);
    CloseHandle(f);
    if (!read || len == 0)
    {
        LogError(log, ctx, TR(L"не удалось прочитать сертификат"), GetLastError());
        return false;
    }

    HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_LOCAL_MACHINE, storeName);
    bool ok = store &&
              CertAddEncodedCertificateToStore(store, X509_ASN_ENCODING, data, len, CERT_STORE_ADD_REPLACE_EXISTING, nullptr);
    if (!ok) LogError(log, ctx, storeName, GetLastError());
    if (store) CertCloseStore(store, 0);
    return ok;
}

bool SetupTrustCertificate(const wchar_t* rootCerPath, const wchar_t* publisherCerPath, SetupLog log, void* ctx)
{
    bool ok = AddCertificate(rootCerPath, L"Root", log, ctx) &&
              AddCertificate(publisherCerPath, L"TrustedPublisher", log, ctx);
    if (ok) Log(log, ctx, TR(L"Тестовые сертификаты Speak2Mic добавлены в доверенные (корневой ЦС и издатель)."));
    return ok;
}

// ---------------------------------------------------------------------------
// Devices

static bool HasHardwareId(HDEVINFO set, SP_DEVINFO_DATA* info)
{
    wchar_t ids[2048] = {};
    if (!SetupDiGetDeviceRegistryPropertyW(set, info, SPDRP_HARDWAREID, nullptr, (BYTE*)ids, sizeof(ids) - 4, nullptr))
    {
        return false;
    }
    for (const wchar_t* p = ids; *p; p += wcslen(p) + 1)
    {
        if (_wcsicmp(p, kHardwareId) == 0) return true;
    }
    return false;
}

// Calls fn for every device (present or not) with our hardware ID; returns how many matched.
template <class F> static int ForEachDevice(F fn)
{
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES);
    if (set == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    SP_DEVINFO_DATA info;
    info.cbSize = sizeof(info);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &info); i++)
    {
        if (HasHardwareId(set, &info))
        {
            n++;
            fn(set, &info);
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return n;
}

int SetupDeviceCount()
{
    return ForEachDevice([](HDEVINFO, SP_DEVINFO_DATA*) {});
}

bool SetupGetDeviceState(ULONG* status, ULONG* problem, wchar_t* instanceId, size_t idLen)
{
    bool found = false;
    *status = 0;
    *problem = 0;
    if (idLen) instanceId[0] = 0;
    ForEachDevice([&](HDEVINFO set, SP_DEVINFO_DATA* info) {
        if (found) return;
        found = true;
        SetupDiGetDeviceInstanceIdW(set, info, instanceId, (DWORD)idLen, nullptr);
        if (CM_Get_DevNode_Status(status, problem, info->DevInst, 0) != CR_SUCCESS)
        {
            *status = 0;
            *problem = CM_PROB_PHANTOM;     // registered but not present
        }
    });
    return found;
}

bool SetupInstallDriver(const wchar_t* infArg, bool* rebootNeeded, SetupLog log, void* ctx)
{
    *rebootNeeded = false;
    wchar_t inf[MAX_PATH];
    if (!GetFullPathNameW(infArg, MAX_PATH, inf, nullptr) || GetFileAttributesW(inf) == INVALID_FILE_ATTRIBUTES)
    {
        Log(log, ctx, TR(L"Ошибка: не найден файл %ls"), infArg);
        return false;
    }

    // Create the root-enumerated device node once.
    if (SetupDeviceCount() == 0)
    {
        GUID classGuid;
        wchar_t className[64];          // MAX_CLASS_NAME_LEN is 32
        if (!SetupDiGetINFClassW(inf, &classGuid, className, 64, nullptr))
        {
            LogError(log, ctx, TR(L"не удалось прочитать класс устройства из INF"), GetLastError());
            return false;
        }
        HDEVINFO set = SetupDiCreateDeviceInfoList(&classGuid, nullptr);
        if (set == INVALID_HANDLE_VALUE)
        {
            LogError(log, ctx, L"SetupDiCreateDeviceInfoList", GetLastError());
            return false;
        }
        SP_DEVINFO_DATA info;
        info.cbSize = sizeof(info);
        wchar_t hwid[64] = {};
        wcscpy(hwid, kHardwareId);      // REG_MULTI_SZ: string + extra terminator (array is zeroed)
        DWORD hwidBytes = (DWORD)((wcslen(hwid) + 2) * sizeof(wchar_t));

        bool ok = SetupDiCreateDeviceInfoW(set, className, &classGuid, nullptr, nullptr, DICD_GENERATE_ID, &info) &&
                  SetupDiSetDeviceRegistryPropertyW(set, &info, SPDRP_HARDWAREID, (BYTE*)hwid, hwidBytes) &&
                  SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, &info);
        DWORD err = GetLastError();
        SetupDiDestroyDeviceInfoList(set);
        if (!ok)
        {
            LogError(log, ctx, TR(L"не удалось создать устройство"), err);
            return false;
        }
        Log(log, ctx, TR(L"Создано устройство %ls."), kHardwareId);
    }

    Log(log, ctx, TR(L"Установка драйвера… Если Windows спросит про издателя, выберите «Все равно установить»."));
    BOOL reboot = FALSE;
    if (!UpdateDriverForPlugAndPlayDevicesW(nullptr, kHardwareId, inf, INSTALLFLAG_FORCE, &reboot))
    {
        DWORD err = GetLastError();
        LogError(log, ctx, TR(L"не удалось установить драйвер"), err);
        if (err == ERROR_CANCELLED)
            Log(log, ctx, TR(L"Установка отменена в окне предупреждения о драйвере."));
        return false;
    }
    *rebootNeeded = reboot != FALSE;
    Log(log, ctx, reboot ? TR(L"Драйвер установлен. Требуется перезагрузка.") : TR(L"Драйвер установлен."));
    return true;
}

// Driver packages in the store are %windir%\INF\oemNN.inf copies of Speak2Mic.inf.
static int RemoveDriverPackages(SetupLog log, void* ctx, const wchar_t* keepInf = nullptr)
{
    wchar_t dir[MAX_PATH], pattern[MAX_PATH];
    GetWindowsDirectoryW(dir, MAX_PATH);
    _snwprintf(pattern, MAX_PATH, L"%ls\\INF\\oem*.inf", dir);
    pattern[MAX_PATH - 1] = 0;

    int removed = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do
    {
        wchar_t path[MAX_PATH], provider[64] = {}, service[64] = {};
        _snwprintf(path, MAX_PATH, L"%ls\\INF\\%ls", dir, fd.cFileName);
        path[MAX_PATH - 1] = 0;
        GetPrivateProfileStringW(L"Strings", L"ProviderName", L"", provider, 64, path);
        GetPrivateProfileStringW(L"Speak2Mic_Device.NT.Services", L"AddService", L"", service, 64, path);
        // the provider is the author since 1.0.278 ("Speak2Mic" before); the service name identifies the package
        if ((_wcsicmp(provider, L"Speak2Mic") != 0 && _wcsicmp(provider, L"" S2M_AUTHOR) != 0) || _wcsnicmp(service, L"Speak2Mic", 9) != 0)
        {
            continue;
        }
        if (keepInf && _wcsicmp(fd.cFileName, keepInf) == 0) continue;     // the package the device uses now
        if (SetupUninstallOEMInfW(fd.cFileName, SUOI_FORCEDELETE, nullptr))
        {
            Log(log, ctx, TR(L"Пакет драйвера %ls удалён из хранилища драйверов."), fd.cFileName);
            removed++;
        }
        else
        {
            LogError(log, ctx, fd.cFileName, GetLastError());
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return removed;
}

#define S2M_PARAMS_PATH L"SYSTEM\\CurrentControlSet\\Services\\Speak2Mic\\Parameters"

// Signal quality settings kept across an update / reinstall (the driver reads them when it starts).
static const wchar_t* const kQualityValues[] = { L"SampleRate", L"Channels", L"MicChannels", L"BitsPerSample", L"LatencyMs" };
struct QualitySettings
{
    DWORD value[5];
    bool  present[5];
};

static void SaveQualitySettings(QualitySettings* q)
{
    for (int i = 0; i < 5; i++)
    {
        DWORD size = sizeof(DWORD);
        q->present[i] = RegGetValueW(HKEY_LOCAL_MACHINE, S2M_PARAMS_PATH, kQualityValues[i], RRF_RT_REG_DWORD, nullptr,
                                     &q->value[i], &size) == ERROR_SUCCESS;
    }
}

// Written back right after the old version is removed, before the new one is installed: the INF does not
// overwrite existing values (NOCLOBBER), so the new driver starts with them.
static void RestoreQualitySettings(const QualitySettings* q, SetupLog log, void* ctx)
{
    HKEY key;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, S2M_PARAMS_PATH, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    int n = 0;
    for (int i = 0; i < 5; i++)
    {
        if (!q->present[i]) continue;
        RegSetValueExW(key, kQualityValues[i], 0, REG_DWORD, (const BYTE*)&q->value[i], sizeof(DWORD));
        AppLog(L"kept setting %ls = %lu", kQualityValues[i], q->value[i]);
        n++;
    }
    RegCloseKey(key);
    if (n) Log(log, ctx, TR(L"Настройки качества сигнала сохранены для новой версии."));
}

// Removes the saved driver settings so that the next installation starts from the INF defaults
// (preset "Standard": 48 kHz, 16 bit, stereo).
static void ResetDriverSettings(SetupLog log, void* ctx)
{
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\Speak2Mic\\Parameters", 0,
                      KEY_SET_VALUE, &key) != ERROR_SUCCESS)
    {
        return;
    }
    const wchar_t* names[] = { L"CableCount", L"SampleRate", L"Channels", L"MicChannels", L"BitsPerSample", L"LatencyMs",
                               L"ProposeFormat", L"StartStatus", L"CablesCreated" };
    for (const wchar_t* n : names) RegDeleteValueW(key, n);
    RegCloseKey(key);
    Log(log, ctx, TR(L"Настройки драйвера сброшены (после установки — пресет «Стандарт»)."));
}

bool SetupRemoveDriver(SetupLog log, void* ctx, bool keepSettings)
{
    QualitySettings kept = {};
    if (keepSettings)
    {
        SaveQualitySettings(&kept);
        // Everything else is cleared: ProposeFormat (the new driver tries format proposals again), status values.
        HKEY key;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, S2M_PARAMS_PATH, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS)
        {
            const wchar_t* names[] = { L"CableCount", L"ProposeFormat", L"StartStatus", L"CablesCreated" };
            for (const wchar_t* n : names) RegDeleteValueW(key, n);
            RegCloseKey(key);
        }
    }
    else
    {
        ResetDriverSettings(log, ctx);
    }
    int failed = 0;
    int n = ForEachDevice([&](HDEVINFO set, SP_DEVINFO_DATA* info) {
        if (!SetupDiCallClassInstaller(DIF_REMOVE, set, info))
        {
            LogError(log, ctx, TR(L"не удалось удалить устройство"), GetLastError());
            failed++;
        }
    });
    Log(log, ctx, TR(L"Удалено устройств: %d."), n - failed);
    int packages = RemoveDriverPackages(log, ctx);
    if (n == 0 && packages == 0)
    {
        Log(log, ctx, TR(L"Speak2Mic не был установлен."));
    }
    if (keepSettings) RestoreQualitySettings(&kept, log, ctx);
    return failed == 0;
}

// ---------------------------------------------------------------------------
// Boot configuration

bool SetupTestSigningEnabled()
{
    return S2mTestSigningEnabled();
}

bool SetupSecureBootEnabled()
{
    return S2mSecureBootEnabled();
}

// The setting in the boot configuration store (what the next start will use). The store is a registry hive:
// the boot manager's default entry ({9dea862c-...}, element 23000003) names the Windows loader entry, whose element
// 16000049 (BcdLibraryBoolean_AllowPrereleaseSignatures) is "testsigning"; no such element means off.
int SetupTestSigningConfigured()
{
    wchar_t entry[64] = {};
    DWORD size = sizeof(entry) - sizeof(wchar_t);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"BCD00000000\\Objects\\{9dea862c-5cdd-4e70-acc1-f32b344d4795}\\Elements\\23000003",
                     L"Element", RRF_RT_REG_SZ, nullptr, entry, &size) != ERROR_SUCCESS || entry[0] != L'{')
        return -1;
    wchar_t path[200];
    _snwprintf(path, 200, L"BCD00000000\\Objects\\%ls", entry);
    path[199] = 0;
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &key) != ERROR_SUCCESS) return -1;
    BYTE value[8] = {};
    size = sizeof(value);
    LSTATUS rs = RegGetValueW(key, L"Elements\\16000049", L"Element", RRF_RT_REG_BINARY, nullptr, value, &size);
    RegCloseKey(key);
    if (rs == ERROR_FILE_NOT_FOUND) return 0;
    if (rs != ERROR_SUCCESS || size < 1) return -1;
    return value[0] ? 1 : 0;
}

bool SetupSetTestSigning(bool on, SetupLog log, void* ctx)
{
    wchar_t sys[MAX_PATH], cmd[MAX_PATH + 64];
    GetSystemDirectoryW(sys, MAX_PATH);
    _snwprintf(cmd, MAX_PATH + 64, L"\"%ls\\bcdedit.exe\" /set {current} testsigning %ls", sys, on ? L"on" : L"off");
    cmd[MAX_PATH + 63] = 0;

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    {
        LogError(log, ctx, TR(L"не удалось запустить bcdedit"), GetLastError());
        return false;
    }
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    AppLog(L"bcdedit testsigning %ls: exit code %lu", on ? L"on" : L"off", (unsigned long)code);
    if (code != 0)
    {
        Log(log, ctx, on ? TR(L"Ошибка: bcdedit завершился с кодом %lu. Если включён Secure Boot, сначала выключите его.")
                         : TR(L"Ошибка: bcdedit завершился с кодом %lu."),
            (unsigned long)code);
        return false;
    }
    Log(log, ctx, on ? TR(L"Тестовый режим подписи включён. Перезагрузите компьютер и снова запустите установку.")
                     : TR(L"Тестовый режим подписи будет выключен после перезагрузки компьютера."));
    return true;
}

bool SetupEnableTestSigning(SetupLog log, void* ctx)
{
    return SetupSetTestSigning(true, log, ctx);
}

int SetupRemoveStaleEndpoints(SetupLog log, void* ctx)
{
    int removed = S2mCleanupOrphanEndpoints(0);       // the installer already waited for the endpoints
    if (removed) Log(log, ctx, TR(L"Удалено старых звуковых устройств Speak2Mic: %d."), removed);
    return removed;
}

// DEVPKEY_Device_DriverInfPath: the INF (oemNN.inf) the device uses.
static const DEVPROPKEY kDevDriverInfPath = { { 0xa8b865dd, 0x2e3d, 0x4094, { 0xad, 0x97, 0xe5, 0x93, 0xa7, 0x0c, 0x75, 0xd6 } }, 5 };

int SetupRemoveOldDriverPackages(SetupLog log, void* ctx)
{
    wchar_t current[64] = {};
    ForEachDevice([&](HDEVINFO set, SP_DEVINFO_DATA* info) {
        DEVPROPTYPE type = 0;
        wchar_t inf[64] = {};
        if (!current[0] && SetupDiGetDevicePropertyW(set, info, &kDevDriverInfPath, &type, (BYTE*)inf, sizeof(inf) - 2, nullptr, 0) &&
            type == DEVPROP_TYPE_STRING)
            wcscpy(current, inf);
    });
    if (!current[0]) return 0;      // unknown: keep everything
    AppLog(L"driver package in use: %ls", current);
    return RemoveDriverPackages(log, ctx, current);
}

void SetupResetDriverStatus()
{
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, S2M_PARAMS_PATH, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    const wchar_t* names[] = { L"CableCount", L"ProposeFormat", L"StartStatus", L"CablesCreated" };
    for (const wchar_t* n : names) RegDeleteValueW(key, n);
    RegCloseKey(key);
}

bool SetupDevicePresent()
{
    ULONG status = 0, problem = 0;
    wchar_t id[200];
    return SetupGetDeviceState(&status, &problem, id, 200) && problem != CM_PROB_PHANTOM;
}

