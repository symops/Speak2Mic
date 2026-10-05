// Deep diagnostics: looks at the Speak2Mic KS filters the same way the Windows Audio Endpoint
// Builder does (device interfaces, pin properties, physical connections, format proposals), and
// collects audio service states and recent audio-related events. Everything goes to the program log;
// a short verdict goes to the caller's log callback.
#include "devctl.h"
#include "lang.h"
#include "diag.h"
#include "applog.h"
#include <setupapi.h>
#include <winioctl.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <winevt.h>
#include <stdio.h>
#include <wchar.h>

// GUIDs spelled out locally (no INITGUID / ksguid.lib needed).
static const GUID kCatAudio    = { 0x6994AD04, 0x93EF, 0x11D0, { 0xA3, 0xCC, 0x00, 0xA0, 0xC9, 0x22, 0x31, 0x96 } };
static const GUID kCatRender   = { 0x65E8773E, 0x8F56, 0x11D0, { 0xA3, 0xB9, 0x00, 0xA0, 0xC9, 0x22, 0x31, 0x96 } };
static const GUID kCatCapture  = { 0x65E8773D, 0x8F56, 0x11D0, { 0xA3, 0xB9, 0x00, 0xA0, 0xC9, 0x22, 0x31, 0x96 } };
static const GUID kCatTopology = { 0xDDA54A40, 0x1E4C, 0x11D1, { 0xA0, 0x50, 0x40, 0x57, 0x05, 0xC1, 0x00, 0x00 } };
static const GUID kCatRealtime = { 0xEB115FFC, 0x10C8, 0x4964, { 0x83, 0x1D, 0x6D, 0xCB, 0x02, 0xE6, 0xF2, 0x3F } };
static const GUID kPropSetPin  = { 0x8C134960, 0x51AD, 0x11CF, { 0x87, 0x8A, 0x94, 0xF8, 0x01, 0xC1, 0x00, 0x00 } };
static const GUID kTypeAudio   = { 0x73647561, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
static const GUID kSubPcm      = { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
static const GUID kSpecWfx     = { 0x05589F81, 0xC356, 0x11CE, { 0xBF, 0x01, 0x00, 0xAA, 0x00, 0x55, 0x59, 0x5A } };

struct NamedGuid { const GUID* g; const wchar_t* name; };

static void GuidText(const GUID& g, wchar_t* out, size_t len)
{
    static const struct { GUID g; const wchar_t* n; } known[] = {
        { { 0x6994AD04, 0x93EF, 0x11D0, { 0xA3, 0xCC, 0x00, 0xA0, 0xC9, 0x22, 0x31, 0x96 } }, L"KSCATEGORY_AUDIO" },
        { { 0xDFF21BE1, 0xF70F, 0x11D0, { 0xB9, 0x17, 0x00, 0xA0, 0xC9, 0x22, 0x31, 0x96 } }, L"KSNODETYPE_MICROPHONE" },
        { { 0xDFF21FE3, 0xF70F, 0x11D0, { 0xB9, 0x17, 0x00, 0xA0, 0xC9, 0x22, 0x31, 0x96 } }, L"KSNODETYPE_LINE_CONNECTOR" },
        { { 0xFB6C4281, 0x0353, 0x11D1, { 0x90, 0x5F, 0x00, 0x00, 0xC0, 0xCC, 0x16, 0xBA } }, L"PINNAME_CAPTURE" },
    };
    for (auto& k : known)
    {
        if (IsEqualGUID(k.g, g))
        {
            wcsncpy(out, k.n, len - 1);
            out[len - 1] = 0;
            return;
        }
    }
    _snwprintf(out, len, L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", g.Data1, g.Data2, g.Data3,
               g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    out[len - 1] = 0;
}

// KS property GET on a pin; returns bytes returned or -1 (error code in *err).
static int PinGet(HANDLE h, ULONG pin, ULONG id, void* out, DWORD outLen, DWORD* err)
{
    KSP_PIN p = {};
    p.Property.Set = kPropSetPin;
    p.Property.Id = id;
    p.Property.Flags = KSPROPERTY_TYPE_GET;
    p.PinId = pin;
    DWORD got = 0;
    if (!DeviceIoControl(h, IOCTL_KS_PROPERTY, &p, sizeof(p), out, outLen, &got, nullptr))
    {
        *err = GetLastError();
        return -1;
    }
    return (int)got;
}

// Asks a streaming pin whether it accepts a PCM format (what the endpoint builder does before
// it picks the device format).
static bool ProposeFormat(HANDLE h, ULONG pin, ULONG rate, WORD bits, WORD channels, DWORD* err)
{
#pragma pack(push, 1)
    struct { KSP_PIN prop; } req = {};
    struct { KSDATAFORMAT df; WAVEFORMATEXTENSIBLE wfx; } fmt = {};
#pragma pack(pop)
    req.prop.Property.Set = kPropSetPin;
    req.prop.Property.Id = KSPROPERTY_PIN_PROPOSEDATAFORMAT;
    req.prop.Property.Flags = KSPROPERTY_TYPE_SET;
    req.prop.PinId = pin;

    fmt.df.FormatSize = sizeof(fmt);
    fmt.df.MajorFormat = kTypeAudio;
    fmt.df.SubFormat = kSubPcm;
    fmt.df.Specifier = kSpecWfx;
    fmt.df.SampleSize = channels * bits / 8;
    fmt.wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    fmt.wfx.Format.nChannels = channels;
    fmt.wfx.Format.nSamplesPerSec = rate;
    fmt.wfx.Format.wBitsPerSample = bits;
    fmt.wfx.Format.nBlockAlign = channels * bits / 8;
    fmt.wfx.Format.nAvgBytesPerSec = rate * fmt.wfx.Format.nBlockAlign;
    fmt.wfx.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    fmt.wfx.Samples.wValidBitsPerSample = bits;
    fmt.wfx.dwChannelMask = channels == 1 ? SPEAKER_FRONT_CENTER : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
    fmt.wfx.SubFormat = kSubPcm;

    DWORD got = 0;
    if (!DeviceIoControl(h, IOCTL_KS_PROPERTY, &req, sizeof(req), &fmt, sizeof(fmt), &got, nullptr))
    {
        *err = GetLastError();
        return false;
    }
    return true;
}

struct Iface
{
    wchar_t path[512];
    bool audio, render, capture, topology, realtime;
};

// Collects our interfaces (by device instance ID) in all audio categories.
static int CollectInterfaces(const wchar_t* instanceId, Iface* out, int max)
{
    const struct { const GUID* g; int which; } cats[] = {
        { &kCatAudio, 0 }, { &kCatRender, 1 }, { &kCatCapture, 2 }, { &kCatTopology, 3 }, { &kCatRealtime, 4 } };
    int n = 0;
    for (auto& c : cats)
    {
        HDEVINFO set = SetupDiGetClassDevsW(c.g, nullptr, nullptr, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
        if (set == INVALID_HANDLE_VALUE) continue;
        SP_DEVICE_INTERFACE_DATA ifd = { sizeof(ifd) };
        for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, c.g, i, &ifd); i++)
        {
            BYTE buf[2048];
            auto* det = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)buf;
            det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            SP_DEVINFO_DATA dev = { sizeof(dev) };
            if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, sizeof(buf), nullptr, &dev)) continue;
            wchar_t id[256] = {};
            SetupDiGetDeviceInstanceIdW(set, &dev, id, 256, nullptr);
            if (_wcsicmp(id, instanceId) != 0) continue;

            // One filter has one interface per category; the paths differ in the category GUID but
            // share the reference string after the last backslash.
            const wchar_t* ref = wcsrchr(det->DevicePath, L'\\');
            if (!ref) ref = det->DevicePath;
            int k = 0;
            while (k < n)
            {
                const wchar_t* r = wcsrchr(out[k].path, L'\\');
                if (_wcsicmp(r ? r : out[k].path, ref) == 0) break;
                k++;
            }
            if (k == n)
            {
                if (n == max) continue;
                ZeroMemory(&out[n], sizeof(Iface));
                wcsncpy(out[n].path, det->DevicePath, 511);
                n++;
            }
            bool* flags[5] = { &out[k].audio, &out[k].render, &out[k].capture, &out[k].topology, &out[k].realtime };
            *flags[c.which] = true;
            if (!(ifd.Flags & SPINT_ACTIVE)) AppLog(L"[ks] interface not active: %ls", det->DevicePath);
        }
        SetupDiDestroyDeviceInfoList(set);
    }
    return n;
}

static void ProbeFilter(const Iface& f, int* proposeOk, int* proposeTried, DWORD* lastErr)
{
    const wchar_t* ref = wcsrchr(f.path, L'\\');
    AppLog(L"[ks] filter %ls  [%ls%ls%ls%ls%ls]", ref ? ref + 1 : f.path, f.audio ? L"AUDIO " : L"",
           f.render ? L"RENDER " : L"", f.capture ? L"CAPTURE " : L"", f.topology ? L"TOPOLOGY " : L"",
           f.realtime ? L"REALTIME" : L"");

    HANDLE h = CreateFileW(f.path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        AppLog(L"[ks]   cannot open filter: error %lu", GetLastError());
        return;
    }

    DWORD err = 0;
    ULONG pins = 0;
    if (PinGet(h, 0, KSPROPERTY_PIN_CTYPES, &pins, sizeof(pins), &err) < 0)
    {
        AppLog(L"[ks]   PIN_CTYPES failed: error %lu", err);
        CloseHandle(h);
        return;
    }
    AppLog(L"[ks]   pins: %lu", pins);

    for (ULONG p = 0; p < pins && p < 16; p++)
    {
        KSPIN_DATAFLOW flow = (KSPIN_DATAFLOW)0;
        KSPIN_COMMUNICATION comm = (KSPIN_COMMUNICATION)0;
        GUID cat = {};
        wchar_t name[128] = L"-", catText[80] = L"-";
        BYTE ranges[4096];
        ULONG nRanges = 0;

        PinGet(h, p, KSPROPERTY_PIN_DATAFLOW, &flow, sizeof(flow), &err);
        PinGet(h, p, KSPROPERTY_PIN_COMMUNICATION, &comm, sizeof(comm), &err);
        if (PinGet(h, p, KSPROPERTY_PIN_CATEGORY, &cat, sizeof(cat), &err) >= 0) GuidText(cat, catText, 80);
        int nb = PinGet(h, p, KSPROPERTY_PIN_NAME, name, sizeof(name) - 2, &err);
        if (nb < 0) _snwprintf(name, 128, L"(no name, error %lu)", err);
        if (PinGet(h, p, KSPROPERTY_PIN_DATARANGES, ranges, sizeof(ranges), &err) >= 0)
            nRanges = ((KSMULTIPLE_ITEM*)ranges)->Count;

        AppLog(L"[ks]   pin %lu: %ls, comm=%ls, category=%ls, name=\"%ls\", dataranges=%lu", p,
               flow == KSPIN_DATAFLOW_IN ? L"IN" : flow == KSPIN_DATAFLOW_OUT ? L"OUT" : L"?",
               comm == KSPIN_COMMUNICATION_NONE ? L"NONE" : comm == KSPIN_COMMUNICATION_SINK ? L"SINK" :
               comm == KSPIN_COMMUNICATION_SOURCE ? L"SOURCE" : comm == KSPIN_COMMUNICATION_BOTH ? L"BOTH" : L"BRIDGE",
               catText, name, nRanges);

        BYTE phys[2048];
        int pb = PinGet(h, p, KSPROPERTY_PIN_PHYSICALCONNECTION, phys, sizeof(phys), &err);
        if (pb > 0)
        {
            auto* pc = (KSPIN_PHYSICALCONNECTION*)phys;
            const wchar_t* to = wcsrchr(pc->SymbolicLinkName, L'\\');
            AppLog(L"[ks]     physical connection -> %ls pin %lu", to ? to + 1 : pc->SymbolicLinkName, pc->Pin);
        }

        if (comm == KSPIN_COMMUNICATION_SINK)
        {
            // At the driver's current sample rate and channel count (other rates are rejected by design).
            static const WORD bits[3] = { 16, 24, 32 };
            DWORD rate = S2mGetParam(L"SampleRate", 48000);
            WORD channels = (WORD)S2mGetParam(L"Channels", 2);
            WORD micChannels = (WORD)S2mGetParam(L"MicChannels", 1);
            if (f.capture && micChannels) channels = micChannels;     // the microphone's own channel count
            for (WORD b : bits)
            {
                (*proposeTried)++;
                bool ok = ProposeFormat(h, p, rate, b, channels, &err);
                if (ok) (*proposeOk)++;
                AppLog(L"[ks]     propose %lu Hz %u bit %u ch: %ls", rate, b, channels, ok ? L"accepted" : L"REJECTED");
                if (!ok)
                {
                    AppLog(L"[ks]       error %lu", err);
                    *lastErr = err;
                }
            }
        }
    }
    CloseHandle(h);
}

static void LogService(const wchar_t* name)
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    SC_HANDLE svc = scm ? OpenServiceW(scm, name, SERVICE_QUERY_STATUS) : nullptr;
    SERVICE_STATUS st = {};
    if (svc && QueryServiceStatus(svc, &st))
        AppLog(L"[svc] %ls: %ls", name, st.dwCurrentState == SERVICE_RUNNING ? L"running" : L"NOT running");
    else
        AppLog(L"[svc] %ls: cannot query (%lu)", name, GetLastError());
    if (svc) CloseServiceHandle(svc);
    if (scm) CloseServiceHandle(scm);
}

// Recent warnings/errors of the audio stack from the event log.
static void LogEvents(const wchar_t* channel, const wchar_t* xpath, int maxEvents)
{
    EVT_HANDLE q = EvtQuery(nullptr, channel, xpath, EvtQueryChannelPath | EvtQueryReverseDirection);
    if (!q)
    {
        AppLog(L"[events] %ls: query failed (%lu)", channel, GetLastError());
        return;
    }
    int shown = 0;
    EVT_HANDLE ev[16];
    DWORD got = 0;
    while (shown < maxEvents && EvtNext(q, 16, ev, 1000, 0, &got))
    {
        for (DWORD i = 0; i < got; i++)
        {
            if (shown < maxEvents)
            {
                wchar_t xml[4096] = {};
                DWORD used = 0, props = 0;
                EvtRender(nullptr, ev[i], EvtRenderEventXml, sizeof(xml) - 2, xml, &used, &props);
                // Provider name for message formatting.
                wchar_t provider[128] = {};
                const wchar_t* pn = wcsstr(xml, L"Provider Name='");
                if (!pn) pn = wcsstr(xml, L"Provider Name=\"");
                if (pn)
                {
                    pn += 15;
                    int k = 0;
                    while (pn[k] && pn[k] != L'\'' && pn[k] != L'"' && k < 127) { provider[k] = pn[k]; k++; }
                }
                wchar_t msg[1024] = {};
                EVT_HANDLE pub = provider[0] ? EvtOpenPublisherMetadata(nullptr, provider, nullptr, 0, 0) : nullptr;
                if (!pub || !EvtFormatMessage(pub, ev[i], 0, 0, nullptr, EvtFormatMessageEvent, 1023, msg, &used))
                {
                    wcsncpy(msg, xml, 1023);        // no message text: log the raw XML
                }
                if (pub) EvtClose(pub);
                const wchar_t* t = wcsstr(xml, L"SystemTime='");
                if (!t) t = wcsstr(xml, L"SystemTime=\"");
                wchar_t when[32] = L"?";
                if (t) { wcsncpy(when, t + 12, 19); when[19] = 0; }
                for (wchar_t* c = msg; *c; c++) if (*c == L'\r' || *c == L'\n') *c = L' ';
                AppLog(L"[events] %ls %ls: %ls", when, provider, msg);
                shown++;
            }
            EvtClose(ev[i]);
        }
    }
    if (!shown) AppLog(L"[events] %ls: no matching events", channel);
    EvtClose(q);
}

// Called by RunDiagnostics when the device is started but no endpoints exist.
void DiagDeepProbe(SetupLog log, void* ctx, const wchar_t* instanceId)
{
    AppLog(TR(L"---- проверка фильтров (как их видит Audio Endpoint Builder) ----"));
    LogService(L"AudioEndpointBuilder");
    LogService(L"Audiosrv");

    Iface ifs[32];
    int n = CollectInterfaces(instanceId, ifs, 32);
    AppLog(L"[ks] interfaces of %ls: %d", instanceId, n);
    int ok = 0, tried = 0;
    DWORD lastErr = 0;
    for (int i = 0; i < n; i++) ProbeFilter(ifs[i], &ok, &tried, &lastErr);

    wchar_t line[256];
    _snwprintf(line, 256, TR(L"Фильтры драйвера: найдено интерфейсов %d; форматов принято при проверке (PROPOSEDATAFORMAT): %d из %d."),
               n, ok, tried);
    line[255] = 0;
    if (log) log(ctx, line);
    if (n == 0 && log) log(ctx, TR(L"Windows не видит ни одного интерфейса фильтров драйвера."));
    else if (tried && !ok && log)
    {
        // ERROR_NOT_FOUND / ERROR_SET_NOT_FOUND / ERROR_NOT_SUPPORTED / ERROR_INVALID_FUNCTION: the query itself is
        // not implemented, which does not stop Windows from choosing a format another way.
        bool unsupported = lastErr == 1168 || lastErr == 1170 || lastErr == 50 || lastErr == 1;
        bool off = S2mGetParam(L"ProposeFormat", 1) == 0;
        _snwprintf(line, 256, unsupported
            ? (off ? TR(L"Проверка форматов (PROPOSEDATAFORMAT) выключена в драйвере (ProposeFormat = 0, код %lu); Windows "
                     L"подбирает формат пробным открытием потока.")
                   : TR(L"Драйвер не ответил на проверку форматов (PROPOSEDATAFORMAT, код %lu), хотя она включена — "
                     L"пришлите setup.log."))
            : TR(L"Фильтр отклонил проверочные форматы (код %lu). Если звук не проходит — пришлите setup.log."), lastErr);
        line[255] = 0;
        log(ctx, line);
    }

    LogEvents(L"System",
              L"*[System[Provider[@Name='Microsoft-Windows-Audio' or @Name='AudioEndpointBuilder' or @Name='Audiosrv' "
              L"or @Name='Microsoft-Windows-Kernel-PnP'] and (Level=1 or Level=2 or Level=3) and "
              L"TimeCreated[timediff(@SystemTime) <= 86400000]]]", 25);
    LogEvents(L"Microsoft-Windows-Audio/Operational",
              L"*[System[(Level=1 or Level=2 or Level=3) and TimeCreated[timediff(@SystemTime) <= 86400000]]]", 25);
    AppLog(TR(L"---- конец проверки фильтров ----"));
}
