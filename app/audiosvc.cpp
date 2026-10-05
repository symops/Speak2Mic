// Windows audio service control (see audiosvc.h).
#include "audiosvc.h"
#include "applog.h"
#include <wchar.h>

static bool WaitServiceState(SC_HANDLE svc, DWORD state, DWORD timeoutMs)
{
    DWORD t0 = GetTickCount();
    SERVICE_STATUS st = {};
    while (QueryServiceStatus(svc, &st) && st.dwCurrentState != state)
    {
        if (GetTickCount() - t0 > timeoutMs) return false;
        Sleep(100);
    }
    return st.dwCurrentState == state;
}

// Stops `name` after its active dependents; appends every stopped service to `out`.
static void StopTree(SC_HANDLE scm, const wchar_t* name, AudioServices* out)
{
    SC_HANDLE svc = OpenServiceW(scm, name, SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_ENUMERATE_DEPENDENTS);
    if (!svc)
    {
        AppLog(L"audio service %ls: open failed (%lu)", name, GetLastError());
        return;
    }
    SERVICE_STATUS st = {};
    QueryServiceStatus(svc, &st);
    if (st.dwCurrentState != SERVICE_STOPPED)
    {
        BYTE buf[8192];
        DWORD needed = 0, count = 0;
        if (EnumDependentServicesW(svc, SERVICE_ACTIVE, (LPENUM_SERVICE_STATUSW)buf, sizeof(buf), &needed, &count))
        {
            auto* deps = (LPENUM_SERVICE_STATUSW)buf;
            for (DWORD i = 0; i < count; i++) StopTree(scm, deps[i].lpServiceName, out);
        }
        if (ControlService(svc, SERVICE_CONTROL_STOP, &st) || GetLastError() == ERROR_SERVICE_NOT_ACTIVE)
        {
            bool ok = WaitServiceState(svc, SERVICE_STOPPED, 15000);
            AppLog(L"audio service %ls: %ls", name, ok ? L"stopped" : L"did not stop in time");
            bool known = false;
            for (int i = 0; i < out->count; i++)
                if (_wcsicmp(out->names[i], name) == 0) known = true;
            if (!known && out->count < AUDIO_SVC_MAX)
            {
                wcsncpy(out->names[out->count], name, 63);
                out->names[out->count][63] = 0;
                out->count++;
            }
        }
        else
        {
            AppLog(L"audio service %ls: stop failed (%lu)", name, GetLastError());
        }
    }
    CloseServiceHandle(svc);
}

void AudioStopServices(bool includeBuilder, AudioServices* stopped)
{
    stopped->count = 0;
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm)
    {
        AppLog(L"audio services: cannot open service manager (%lu)", GetLastError());
        return;
    }
    // AudioEndpointBuilder's dependents include Audiosrv, so stopping it stops both.
    StopTree(scm, includeBuilder ? L"AudioEndpointBuilder" : L"Audiosrv", stopped);
    CloseServiceHandle(scm);
}

void AudioStartServices(const AudioServices* stopped)
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return;
    for (int i = stopped->count - 1; i >= 0; i--)     // base services before their dependents
    {
        SC_HANDLE svc = OpenServiceW(scm, stopped->names[i], SERVICE_START | SERVICE_QUERY_STATUS);
        if (!svc) continue;
        bool ok = StartServiceW(svc, 0, nullptr) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;
        if (ok) ok = WaitServiceState(svc, SERVICE_RUNNING, 15000);
        AppLog(L"audio service %ls: %ls", stopped->names[i], ok ? L"started" : L"START FAILED");
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
}
