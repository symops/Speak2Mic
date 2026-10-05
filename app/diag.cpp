// Diagnostics implementation (see diag.h).
#include "diag.h"
#include "lang.h"
#include "applog.h"
#include <cfgmgr32.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>

#define PARAMS_KEY L"SYSTEM\\CurrentControlSet\\Services\\Speak2Mic\\Parameters"

static const PROPERTYKEY kFriendlyName = {
    { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };

static void Say(SetupLog log, void* ctx, const wchar_t* fmt, ...)
{
    wchar_t buf[512];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf(buf, 512, fmt, args);
    va_end(args);
    buf[511] = 0;
    if (log) log(ctx, buf);
    else AppLog(L"%ls", buf);
}

static const wchar_t* ProblemText(ULONG p)
{
    switch (p)
    {
    case 1:  return TR(L"устройство не настроено");
    case 10: return TR(L"устройство не удалось запустить (драйвер вернул ошибку при старте)");
    case 18: return TR(L"драйвер нужно переустановить");
    case 22: return TR(L"устройство отключено");
    case 24: return TR(L"устройство отсутствует");
    case 28: return TR(L"драйвер не установлен");
    case 31: return TR(L"Windows не смогла загрузить драйвер для устройства");
    case 37: return TR(L"ошибка в DriverEntry драйвера");
    case 39: return TR(L"драйвер не загрузился (повреждён или несовместим)");
    case 43: return TR(L"драйвер сообщил об ошибке устройства");
    case 45: return TR(L"устройство сейчас не подключено");
    case 48: return TR(L"драйвер заблокирован политикой");
    case 52: return TR(L"Windows не смогла проверить подпись драйвера (включите тестовый режим)");
    default: return TR(L"см. код в Диспетчере устройств");
    }
}

static const wchar_t* StateText(DWORD s)
{
    switch (s)
    {
    case DEVICE_STATE_ACTIVE:     return TR(L"активно");
    case DEVICE_STATE_DISABLED:   return TR(L"отключено");
    case DEVICE_STATE_NOTPRESENT: return TR(L"отсутствует");
    case DEVICE_STATE_UNPLUGGED:  return TR(L"не подключено");
    default:                      return L"?";
    }
}

// Counts Speak2Mic sound endpoints; optionally reports each one.
static void ScanEndpoints(int* active, int* inactive, SetupLog log, void* ctx, bool report)
{
    *active = *inactive = 0;
    bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    IMMDeviceEnumerator* en = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                   (void**)&en)))
    {
        IMMDeviceCollection* coll = nullptr;
        if (SUCCEEDED(en->EnumAudioEndpoints(eAll, DEVICE_STATEMASK_ALL, &coll)))
        {
            UINT count = 0;
            coll->GetCount(&count);
            for (UINT i = 0; i < count; i++)
            {
                IMMDevice* dev = nullptr;
                if (FAILED(coll->Item(i, &dev))) continue;
                wchar_t name[256] = {};
                IPropertyStore* props = nullptr;
                if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)))
                {
                    PROPVARIANT v;
                    PropVariantInit(&v);
                    if (SUCCEEDED(props->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR)
                    {
                        wcsncpy(name, v.pwszVal, 255);
                    }
                    PropVariantClear(&v);
                    props->Release();
                }
                DWORD state = 0;
                dev->GetState(&state);
                if (wcsstr(name, L"Speak2Mic"))
                {
                    if (state == DEVICE_STATE_ACTIVE) (*active)++;
                    else (*inactive)++;
                    if (report) Say(log, ctx, TR(L"  звуковое устройство «%ls»: %ls"), name, StateText(state));
                }
                dev->Release();
            }
            coll->Release();
        }
        en->Release();
    }
    if (com) CoUninitialize();
}

static DWORD ReadDword(const wchar_t* name, DWORD def)
{
    DWORD v = def, size = sizeof(v);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, PARAMS_KEY, name, RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS)
        return def;
    return v;
}

bool DiagRunningDriverVersion(wchar_t* running, size_t len)
{
    running[0] = 0;
    bool found = false;
    DWORD size = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, PARAMS_KEY, L"DriverLog", RRF_RT_REG_SZ, nullptr, nullptr, &size) == ERROR_SUCCESS && size)
    {
        wchar_t* text = (wchar_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size + 2);
        if (text && RegGetValueW(HKEY_LOCAL_MACHINE, PARAMS_KEY, L"DriverLog", RRF_RT_REG_SZ, nullptr, text, &size) == ERROR_SUCCESS)
        {
            // Last "DriverEntry: Speak2Mic <version>," line.
            const wchar_t* key = L"DriverEntry: Speak2Mic ";
            const wchar_t* last = nullptr;
            for (const wchar_t* p = wcsstr(text, key); p; p = wcsstr(p + 1, key)) last = p;
            if (last)
            {
                last += wcslen(key);
                size_t k = 0;
                while (last[k] && last[k] != L',' && last[k] != L' ' && k + 1 < len) { running[k] = last[k]; k++; }
                running[k] = 0;
                found = k > 0;
            }
        }
        if (text) HeapFree(GetProcessHeap(), 0, text);
    }
    return found;
}

bool DiagPackageDriverVersion(wchar_t* packaged, size_t len)
{
    wchar_t inf[MAX_PATH], ver[128] = L"";
    GetModuleFileNameW(nullptr, inf, MAX_PATH);
    wchar_t* slash = wcsrchr(inf, L'\\');
    wcscpy(slash ? slash + 1 : inf, L"Speak2Mic.inf");
    GetPrivateProfileStringW(L"Version", L"DriverVer", L"", ver, 128, inf);
    const wchar_t* v = wcschr(ver, L',');
    packaged[0] = 0;
    if (!v || !v[1]) return false;
    wcsncpy(packaged, v + 1, len - 1);
    packaged[len - 1] = 0;
    return true;
}

// Version of the running driver (from its log) vs. the package next to this program.
static void CheckDriverVersion(SetupLog log, void* ctx)
{
    wchar_t running[64] = L"?";
    DiagRunningDriverVersion(running, 64);
    if (!running[0]) wcscpy(running, L"?");

    wchar_t packaged[64];
    if (DiagPackageDriverVersion(packaged, 64))
    {
        Say(log, ctx, TR(L"Запущен драйвер версии %ls, в пакете — %ls."), running, packaged);
        if (_wcsicmp(running, packaged) != 0)
            Say(log, ctx, TR(L"ВНИМАНИЕ: работает не та версия драйвера, что в пакете. Нажмите «Переустановить» или перезагрузите компьютер."));
    }
    else
    {
        Say(log, ctx, TR(L"Запущен драйвер версии %ls."), running);
    }
}

static void LogDriverLog()
{
    DWORD size = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, PARAMS_KEY, L"DriverLog", RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS ||
        size == 0)
    {
        AppLog(TR(L"[driver] журнал драйвера пуст: драйвер ни разу не запускался"));
        return;
    }
    wchar_t* text = (wchar_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size + 2);
    if (!text) return;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, PARAMS_KEY, L"DriverLog", RRF_RT_REG_SZ, nullptr, text, &size) == ERROR_SUCCESS)
    {
        AppLog(TR(L"[driver] ---- журнал драйвера (DriverLog) ----"));
        AppLogText(L"[driver] ", text);
    }
    HeapFree(GetProcessHeap(), 0, text);
}

// Copies the last setupapi.dev.log section that mentions Speak2Mic into the program log.
static void LogSetupApiSection()
{
    wchar_t path[MAX_PATH];
    GetWindowsDirectoryW(path, MAX_PATH);
    wcscat(path, L"\\INF\\setupapi.dev.log");
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
    {
        AppLog(TR(L"[setupapi] не удалось открыть %ls (%lu)"), path, GetLastError());
        return;
    }
    LARGE_INTEGER size = {};
    GetFileSizeEx(f, &size);
    const LONGLONG tail = 4 * 1024 * 1024;
    LONGLONG start = size.QuadPart > tail ? size.QuadPart - tail : 0;
    DWORD len = (DWORD)(size.QuadPart - start), got = 0;
    char* buf = (char*)HeapAlloc(GetProcessHeap(), 0, len + 1);
    if (buf)
    {
        LARGE_INTEGER pos;
        pos.QuadPart = start;
        SetFilePointerEx(f, pos, nullptr, FILE_BEGIN);
        ReadFile(f, buf, len, &got, nullptr);
        buf[got] = 0;

        // Find the last "speak2mic" (case-insensitive), then the section around it.
        char* hit = nullptr;
        for (char* p = buf; *p; p++)
            if (_strnicmp(p, "speak2mic", 9) == 0) hit = p;
        if (!hit)
        {
            AppLog(TR(L"[setupapi] в setupapi.dev.log нет записей о Speak2Mic"));
        }
        else
        {
            char* sec = hit;
            while (sec > buf && strncmp(sec, ">>>  [", 6) != 0) sec--;
            char* end = strstr(hit, "<<<  [Exit status");
            if (end)
            {
                char* nl = strchr(end, '\n');
                end = nl ? nl + 1 : end + strlen(end);
            }
            else
            {
                end = buf + got;
            }
            *end = 0;
            int wn = MultiByteToWideChar(CP_ACP, 0, sec, -1, nullptr, 0);
            wchar_t* w = (wchar_t*)HeapAlloc(GetProcessHeap(), 0, wn * sizeof(wchar_t));
            if (w)
            {
                MultiByteToWideChar(CP_ACP, 0, sec, -1, w, wn);
                AppLog(TR(L"[setupapi] ---- последняя секция setupapi.dev.log о Speak2Mic ----"));
                AppLogText(L"[setupapi] ", w);
                HeapFree(GetProcessHeap(), 0, w);
            }
        }
        HeapFree(GetProcessHeap(), 0, buf);
    }
    CloseHandle(f);
}

static void LogServiceState(SetupLog log, void* ctx)
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    SC_HANDLE svc = scm ? OpenServiceW(scm, L"Speak2Mic", SERVICE_QUERY_STATUS) : nullptr;
    if (!svc)
    {
        Say(log, ctx, TR(L"Служба драйвера Speak2Mic не зарегистрирована."));
    }
    else
    {
        SERVICE_STATUS st = {};
        QueryServiceStatus(svc, &st);
        Say(log, ctx, TR(L"Служба драйвера: %ls."), st.dwCurrentState == SERVICE_RUNNING ? TR(L"запущена")
                                               : st.dwCurrentState == SERVICE_STOPPED ? TR(L"остановлена") : TR(L"запускается/останавливается"));
        CloseServiceHandle(svc);
    }
    if (scm) CloseServiceHandle(scm);
}

DiagResult RunDiagnostics(SetupLog log, void* ctx)
{
    AppLog(TR(L"---- диагностика ----"));
    Say(log, ctx, TR(L"Тестовый режим подписи: %ls; Secure Boot: %ls."),
        SetupTestSigningEnabled() ? TR(L"включён") : TR(L"выключен"), SetupSecureBootEnabled() ? TR(L"включён") : TR(L"выключен"));

    ULONG status = 0, problem = 0;
    wchar_t id[256];
    DiagResult result;
    if (!SetupGetDeviceState(&status, &problem, id, 256))
    {
        Say(log, ctx, TR(L"Устройство ROOT\\Speak2Mic не найдено: драйвер не установлен."));
        result = DiagNotInstalled;
    }
    else
    {
        AppLog(L"device %ls: status=0x%08lX problem=%lu", id, status, problem);
        LogServiceState(log, ctx);
        CheckDriverVersion(log, ctx);
        DWORD start = ReadDword(L"StartStatus", 0xFFFFFFFF), cables = ReadDword(L"CablesCreated", 0xFFFFFFFF);
        if (start != 0xFFFFFFFF)
            Say(log, ctx, TR(L"Драйвер при запуске создал кабелей: %lu (код 0x%08lX)."), cables, start);

        if (problem != 0 || !(status & DN_STARTED))
        {
            Say(log, ctx, TR(L"Устройство не запущено: код %lu — %ls."), problem, ProblemText(problem));
            result = DiagDeviceProblem;
        }
        else
        {
            Say(log, ctx, TR(L"Устройство Speak2Mic запущено."));
            int active = 0, inactive = 0;
            ScanEndpoints(&active, &inactive, log, ctx, true);
            if (active > 0)
            {
                result = DiagOk;
            }
            else if (inactive > 0)
            {
                Say(log, ctx, TR(L"Звуковые устройства Speak2Mic не активны. «Отключено» — включите в «Звук» (mmsys.cpl); "
                              L"«отсутствует» — перезапустите службы звука Windows или перезагрузите компьютер."));
                result = DiagEndpointsInactive;
            }
            else
            {
                Say(log, ctx, TR(L"Драйвер работает, но Windows не создала звуковые устройства Speak2Mic."));
                result = DiagNoEndpoints;
            }
            DiagDeepProbe(log, ctx, id);
        }
        LogDriverLog();
    }
    LogSetupApiSection();
    AppLog(TR(L"---- конец диагностики (результат %d) ----"), (int)result);
    return result;
}

DiagResult WaitForEndpoints(DWORD timeoutMs)
{
    DWORD t0 = GetTickCount();
    for (;;)
    {
        ULONG status = 0, problem = 0;
        wchar_t id[256];
        DiagResult r;
        if (!SetupGetDeviceState(&status, &problem, id, 256))
            r = DiagNotInstalled;
        else if (problem != 0)
            r = DiagDeviceProblem;
        else if (!(status & DN_STARTED))
            r = DiagDeviceProblem;
        else
        {
            int active = 0, inactive = 0;
            ScanEndpoints(&active, &inactive, nullptr, nullptr, false);
            r = active ? DiagOk : (inactive ? DiagEndpointsInactive : DiagNoEndpoints);
        }
        if (r == DiagOk || GetTickCount() - t0 > timeoutMs) return r;
        Sleep(500);
    }
}

int DiagSetMicUnityGain()
{
    int done = 0;
    // The volume chosen in the control panel (HKCU\Software\Speak2Mic\MicVolumeCentiDb, hundredths of a dB,
    // signed; the installer runs elevated as the same user), else 0 dB.
    DWORD centi = 0, size = sizeof(centi);
    bool saved = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Speak2Mic", L"MicVolumeCentiDb", RRF_RT_REG_DWORD, nullptr,
                              &centi, &size) == ERROR_SUCCESS;
    float target = saved ? (LONG)centi / 100.0f : 0.0f;
    bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    IMMDeviceEnumerator* en = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                   (void**)&en)))
    {
        IMMDeviceCollection* coll = nullptr;
        if (SUCCEEDED(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &coll)))
        {
            UINT count = 0;
            coll->GetCount(&count);
            for (UINT i = 0; i < count; i++)
            {
                IMMDevice* dev = nullptr;
                if (FAILED(coll->Item(i, &dev))) continue;
                wchar_t name[256] = {};
                IPropertyStore* props = nullptr;
                if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)))
                {
                    PROPVARIANT v;
                    PropVariantInit(&v);
                    if (SUCCEEDED(props->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR) wcsncpy(name, v.pwszVal, 255);
                    PropVariantClear(&v);
                    props->Release();
                }
                IAudioEndpointVolume* vol = nullptr;
                if (wcsstr(name, L"(Speak2Mic)") &&
                    SUCCEEDED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&vol)))
                {
                    float before = 0, mn = -96.0f, mx = 30.0f, step = 0;
                    vol->GetMasterVolumeLevel(&before);
                    vol->GetVolumeRange(&mn, &mx, &step);
                    float want = target < mn ? mn : (target > mx ? mx : target);
                    HRESULT hr = vol->SetMasterVolumeLevel(want, nullptr);
                    AppLog(L"microphone \"%ls\": volume %.1f dB -> %.1f dB (%ls, 0x%08lX)", name, before, want,
                           saved ? L"saved in the panel" : L"unity gain", (unsigned long)hr);
                    if (SUCCEEDED(hr)) done++;
                    vol->Release();
                }
                dev->Release();
            }
            coll->Release();
        }
        en->Release();
    }
    if (com) CoUninitialize();
    return done;
}
