// s2mautotest - randomized end-to-end test of Speak2Mic (driver + endpoints + the functions the panel uses).
//
//   s2mautotest [minutes] [--seed N]      default: 10 minutes, random seed
//
// Needs administrator rights (the manifest requests them). Closes a running control panel, remembers the current
// state (driver settings, endpoint names, microphone volume/mute, default formats), then for N minutes runs random
// actions and checks their results:
//   apply      random signal quality (rate, bits, speaker/microphone channels, latency): registry + device restart,
//              endpoints back, default formats as configured, driver log shows the new configuration
//   signal     a tone through the cable on every speaker channel in turn: the microphone gets it (no channel lost),
//              not clipped, no dropouts, silence when nothing plays, latency within bounds
//   rename     random names (Latin, Cyrillic, long) on both endpoints, read back
//   volume     microphone volume 0..300 % and mute, read back
//   format     default format with a random bit depth (Windows may refuse 24/32 bit: 16 is accepted as fallback)
//   stress     open/close shared-mode streams many times
//   testsound  the panel's "Test" (Windows' test melodies per channel), started and stopped
//   music      the panel's "Play / Pause" on music it generates at the start (the same melody as MP3, FLAC and WAV when
//              Windows has the encoders; one folder per format, one with all of them plus a broken file and a text file)
//              or on the Generator: plays, pauses, continues the same track, measured on the microphone; a folder
//              without music starts nothing. The generated music is removed at the end.
//   orphans    the number of leftover endpoint records must not grow
//   cli        s2mctl.exe with random commands (status, set, name, volume, mute, export/import, invalid arguments,
//              test): exit codes and the resulting state
// Everything is restored at the end (also after Ctrl+C). Every check is logged as PASS / FAIL / WARN to
// %ProgramData%\Speak2Mic\logs\autotest.log; exit code 1 if anything failed.
#include "audio.h"
#include "applog.h"
#include "audiosvc.h"
#include "devctl.h"
#include "diag.h"
#include "mp3player.h"
#include "atmusic.h"
#include <mfapi.h>
#include "../driver/version.h"
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>
#include <math.h>
#include <io.h>
#include <fcntl.h>
#include <wtsapi32.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include "setupcore.h"

// ---------------------------------------------------------------------------
// Output and checks

static int g_pass, g_fail, g_warn;
static volatile LONG g_stop;
static unsigned long long g_rng = 88172645463325252ULL;    // xorshift64* state (no C++ library here)

// Volume changes made outside Speak2Mic, reported on a system thread: queued, written by the next Out().
static CRITICAL_SECTION g_eventLock;
static wchar_t g_events[32][200];
static int g_eventCount, g_eventLost;

static void FlushEvents()
{
    wchar_t copy[32][200];
    int n, lost;
    EnterCriticalSection(&g_eventLock);
    n = g_eventCount;
    lost = g_eventLost;
    memcpy(copy, g_events, sizeof(copy[0]) * n);
    g_eventCount = g_eventLost = 0;
    LeaveCriticalSection(&g_eventLock);
    for (int i = 0; i < n; i++)
    {
        wprintf(L"%ls\n", copy[i]);
        AppLog(L"%ls", copy[i]);
    }
    if (lost) AppLog(L"  (%d more outside volume change(s) not logged)", lost);
    if (n)
    {
        // Who may have done it: the programs with an audio session on the microphone right now.
        extern void LogMicSessions();
        LogMicSessions();
    }
}

static volatile LONG g_outsideChanges;      // microphone volume changes made outside Speak2Mic (all of them)

static void OnForeignVolume(float db, float scalar, bool mute, const GUID& c, void*)
{
    InterlockedIncrement(&g_outsideChanges);
    SYSTEMTIME t;
    GetLocalTime(&t);
    EnterCriticalSection(&g_eventLock);
    if (g_eventCount < 32)
    {
        _snwprintf(g_events[g_eventCount], 200, L"  info %02u:%02u:%02u.%03u microphone volume changed OUTSIDE Speak2Mic: %.2f dB "
                   L"(scalar %.3f)%ls, context {%08lX-...}", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, db, scalar,
                   mute ? L", muted" : L"", (unsigned long)c.Data1);
        g_events[g_eventCount++][199] = 0;
    }
    else g_eventLost++;
    LeaveCriticalSection(&g_eventLock);
}

static void Out(const wchar_t* fmt, ...)
{
    FlushEvents();
    wchar_t buf[1024];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf(buf, 1024, fmt, args);
    va_end(args);
    buf[1023] = 0;
    wprintf(L"%ls\n", buf);
    AppLog(L"%ls", buf);
}

static bool Check(bool ok, const wchar_t* fmt, ...)
{
    wchar_t buf[900];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf(buf, 900, fmt, args);
    va_end(args);
    buf[899] = 0;
    Out(L"  %ls %ls", ok ? L"PASS" : L"FAIL", buf);
    if (ok) g_pass++; else g_fail++;
    return ok;
}

static void Warn(const wchar_t* fmt, ...)
{
    wchar_t buf[900];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf(buf, 900, fmt, args);
    va_end(args);
    buf[899] = 0;
    Out(L"  WARN %ls", buf);
    g_warn++;
}

static int Rand(int lo, int hi)
{
    g_rng ^= g_rng >> 12;
    g_rng ^= g_rng << 25;
    g_rng ^= g_rng >> 27;
    unsigned long long r = g_rng * 2685821657736338717ULL;
    return lo + (int)((r >> 33) % (unsigned long long)(hi - lo + 1));
}

static BOOL WINAPI CtrlHandler(DWORD)
{
    InterlockedExchange(&g_stop, 1);        // finish the current action, then restore everything
    return TRUE;
}

// ---------------------------------------------------------------------------
// Devices

static bool FindOurs(AudioDevice* spk, AudioDevice* mic)
{
    static AudioDevice list[128];
    int n = ListAudioDevices(list, 128);
    bool s = false, m = false;
    for (int i = 0; i < n; i++)
    {
        bool ours = list[i].adapter[0] ? _wcsicmp(list[i].adapter, L"Speak2Mic") == 0 : wcsstr(list[i].name, L"(Speak2Mic)") != nullptr;
        if (!ours) continue;
        if (list[i].flow == eRender && !s) { *spk = list[i]; s = true; }
        if (list[i].flow == eCapture && !m) { *mic = list[i]; m = true; }
    }
    return s && m;
}

static bool WaitOurs(AudioDevice* spk, AudioDevice* mic, DWORD ms)
{
    DWORD t0 = GetTickCount();
    do
    {
        if (FindOurs(spk, mic)) return true;
        Sleep(250);
    } while (GetTickCount() - t0 < ms);
    return false;
}

struct Quality { DWORD rate, channels, micChannels, bits, latency; };

static Quality ReadQuality()
{
    Quality q;
    q.rate = S2mGetParam(L"SampleRate", 48000);
    q.channels = S2mGetParam(L"Channels", 2);
    q.micChannels = S2mGetParam(L"MicChannels", 1);
    q.bits = S2mGetParam(L"BitsPerSample", 16);
    q.latency = S2mGetParam(L"LatencyMs", 30);
    return q;
}

static DWORD MicCh(const Quality& q) { return q.micChannels ? q.micChannels : q.channels; }

// Registry + device restart (Windows Audio stopped meanwhile), like "Apply" in the panel.
static bool ApplyQuality(const Quality& q)
{
    S2mSetParam(L"SampleRate", q.rate);
    S2mSetParam(L"Channels", q.channels);
    S2mSetParam(L"MicChannels", q.micChannels);
    S2mSetParam(L"BitsPerSample", q.bits);
    S2mSetParam(L"LatencyMs", q.latency);
    AudioServices svc;
    AudioStopServices(false, &svc);
    bool found = false, reboot = false;
    bool ok = S2mRestartDevice(&found, &reboot);
    AudioStartServices(&svc);
    if (ok && found && !reboot) S2mCleanupOrphanEndpoints(8000);     // as the panel / s2mctl do
    return ok && found && !reboot;
}

// ---------------------------------------------------------------------------
// Endpoints that do not come back after a restart: what is going on (device node, endpoint states, audio service, the
// driver's last lines), a longer wait, then one more restart, so a 10-hour run does not end at the first stall.

static AudioDevice g_spk, g_mic;        // the cable's endpoints (found again after every restart)

static void LogEndpointStates()
{
    IMMDeviceEnumerator* en = nullptr;
    IMMDeviceCollection* list = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return;
    UINT n = 0;
    if (SUCCEEDED(en->EnumAudioEndpoints(eAll, DEVICE_STATEMASK_ALL, &list))) list->GetCount(&n);
    int shown = 0;
    for (UINT i = 0; i < n; i++)
    {
        IMMDevice* d = nullptr;
        IPropertyStore* ps = nullptr;
        if (SUCCEEDED(list->Item(i, &d)) && SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps)))
        {
            PROPVARIANT name, adapter;
            PropVariantInit(&name);
            PropVariantInit(&adapter);
            ps->GetValue(PKEY_Device_FriendlyName, &name);
            ps->GetValue(PKEY_DeviceInterface_FriendlyName, &adapter);
            bool ours = (adapter.vt == VT_LPWSTR && wcsstr(adapter.pwszVal, L"Speak2Mic")) ||
                        (name.vt == VT_LPWSTR && wcsstr(name.pwszVal, L"Speak2Mic"));
            DWORD state = 0;
            d->GetState(&state);
            if (ours && shown < 12)
            {
                shown++;
                Out(L"  info endpoint \"%ls\": state %ls", name.vt == VT_LPWSTR ? name.pwszVal : L"?",
                    state == DEVICE_STATE_ACTIVE ? L"active" : state == DEVICE_STATE_DISABLED ? L"DISABLED" :
                    state == DEVICE_STATE_NOTPRESENT ? L"NOT PRESENT" : L"UNPLUGGED");
            }
            PropVariantClear(&name);
            PropVariantClear(&adapter);
        }
        if (ps) ps->Release();
        if (d) d->Release();
    }
    if (!shown) Out(L"  info no Speak2Mic endpoint records at all");
    if (list) list->Release();
    en->Release();
}

static void LogDriverTail(int maxLines)
{
    DWORD size = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, S2M_PARAMS_KEY, L"DriverLog", RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || !size)
        return;
    wchar_t* text = (wchar_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size + 2);
    if (!text) return;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, S2M_PARAMS_KEY, L"DriverLog", RRF_RT_REG_SZ, nullptr, text, &size) == ERROR_SUCCESS)
    {
        const wchar_t* lines[64];
        int n = 0;
        for (wchar_t* p = text; *p;)
        {
            wchar_t* e = p;
            while (*e && *e != L'\r' && *e != L'\n') e++;
            wchar_t c = *e;
            *e = 0;
            if (*p)
            {
                if (n == 64) { memmove(lines, lines + 1, sizeof(lines[0]) * 63); n--; }
                lines[n++] = p;
            }
            p = c ? e + 1 : e;
        }
        for (int i = n > maxLines ? n - maxLines : 0; i < n; i++) Out(L"  driver: %ls", lines[i]);
    }
    HeapFree(GetProcessHeap(), 0, text);
}

static void LogWhyMissing()
{
    ULONG status = 0, problem = 0;
    wchar_t inst[200] = L"";
    if (SetupGetDeviceState(&status, &problem, inst, 200))
        Out(L"  info device %ls: status 0x%08lX, problem %lu%ls", inst, status, problem, problem ? L" (see CM_PROB_*)" : L"");
    else
        Out(L"  info no Speak2Mic device node");
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    SC_HANDLE svc = scm ? OpenServiceW(scm, L"Audiosrv", SERVICE_QUERY_STATUS) : nullptr;
    SERVICE_STATUS ss = {};
    if (svc && QueryServiceStatus(svc, &ss)) Out(L"  info Windows Audio service state %lu (4 = running)", ss.dwCurrentState);
    if (svc) CloseServiceHandle(svc);
    if (scm) CloseServiceHandle(scm);
    LogEndpointStates();
    LogDriverTail(15);
}

// After the first 20 s: diagnostics, 40 s more, then one more restart with the current settings. true: back.
static bool RecoverEndpoints(const wchar_t* when)
{
    Out(L"  info Speak2Mic endpoints not back 20 s after %ls:", when);
    LogWhyMissing();
    DWORD t0 = GetTickCount();
    if (WaitOurs(&g_spk, &g_mic, 40000))
    {
        Warn(L"endpoints came back late: %lu s after %ls", 20 + (GetTickCount() - t0) / 1000, when);
        return true;
    }
    Out(L"  info still missing after 60 s: one more device restart");
    LogWhyMissing();
    bool restarted = ApplyQuality(ReadQuality());
    bool back = WaitOurs(&g_spk, &g_mic, 30000);
    Out(L"  info recovery restart: %ls, endpoints %ls", restarted ? L"done" : L"FAILED", back ? L"back" : L"STILL MISSING");
    if (!back) LogWhyMissing();
    return back;
}

// Default formats of both endpoints (the panel's ApplyEndpointFormats), 16-bit fallback; then, like s2mctl,
// holds them and the microphone level read before the restart while Windows re-applies stored values.
static int SetFormats(const Quality& q, const AudioDevice& spk, const AudioDevice& mic, bool level, float db, bool mute)
{
    const AudioDevice* d[2] = { &spk, &mic };
    EndpointHold holds[2] = {};
    for (int side = 0; side < 2; side++)
    {
        DWORD ch = side == 0 ? q.channels : MicCh(q);
        wchar_t err[160];
        WORD used = (WORD)q.bits;
        bool ok = SetEndpointFormat(d[side]->id, q.rate, used, (WORD)ch, err, 160);
        if (!ok && q.bits > 16) ok = SetEndpointFormat(d[side]->id, q.rate, used = 16, (WORD)ch, err, 160);
        holds[side] = { d[side]->id, ok, q.rate, used, (WORD)ch, side == 1 && level, db, mute };
    }
    return HoldEndpointSettings(holds, 2, 3000);
}

// Last "Config:" line of the driver log.
static bool DriverConfigLine(wchar_t* out, size_t len)
{
    DWORD size = 0;
    out[0] = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, S2M_PARAMS_KEY, L"DriverLog", RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || !size)
        return false;
    wchar_t* text = (wchar_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size + 2);
    if (!text) return false;
    bool ok = false;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, S2M_PARAMS_KEY, L"DriverLog", RRF_RT_REG_SZ, nullptr, text, &size) == ERROR_SUCCESS)
    {
        const wchar_t* last = nullptr;
        for (const wchar_t* p = wcsstr(text, L"Config: "); p; p = wcsstr(p + 1, L"Config: ")) last = p;
        if (last)
        {
            size_t n = 0;
            while (last[n] && last[n] != L'\r' && last[n] != L'\n' && n + 1 < len) { out[n] = last[n]; n++; }
            out[n] = 0;
            ok = true;
        }
    }
    HeapFree(GetProcessHeap(), 0, text);
    return ok;
}

// The driver log lines about the microphone volume node (value at start, every SET), newest last.
static void DumpMicNodeLog(int maxLines)
{
    DWORD size = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, S2M_PARAMS_KEY, L"DriverLog", RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || !size)
        return;
    wchar_t* text = (wchar_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size + 2);
    if (!text) return;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, S2M_PARAMS_KEY, L"DriverLog", RRF_RT_REG_SZ, nullptr, text, &size) == ERROR_SUCCESS)
    {
        const wchar_t* lines[256];
        int n = 0;
        for (wchar_t* p = text; *p;)
        {
            wchar_t* e = p;
            while (*e && *e != L'\r' && *e != L'\n') e++;
            wchar_t c = *e;
            *e = 0;
            if (wcsstr(p, L"microphone volume") || wcsstr(p, L"microphone mute") || wcsstr(p, L"Microphone node") ||
                wcsstr(p, L"StartDevice"))
            {
                if (n == 256) { memmove(lines, lines + 1, sizeof(lines[0]) * 255); n--; }
                lines[n++] = p;
            }
            p = c ? e + 1 : e;
        }
        for (int i = n > maxLines ? n - maxLines : 0; i < n; i++) Out(L"  driver: %ls", lines[i]);
    }
    HeapFree(GetProcessHeap(), 0, text);
}

// ---------------------------------------------------------------------------
// Signal through the cable

static bool IsFloatFormat(const WAVEFORMATEX* f)
{
    return f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
           (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && ((const WAVEFORMATEXTENSIBLE*)f)->SubFormat.Data1 == 3);
}

static float Sample(const BYTE* s, bool isFloat, int bytes)
{
    if (isFloat && bytes == 4) return *(const float*)s;
    if (bytes == 2) return *(const short*)s / 32768.0f;
    if (bytes == 3) return (float)(((int)s[0] << 8 | (int)s[1] << 16 | (int)s[2] << 24) >> 8) / 8388608.0f;
    if (bytes == 4) return (float)(*(const int*)s / 2147483648.0);
    return 0;
}

static float Db(double v) { return v > 1e-7 ? (float)(20.0 * log10(v)) : -140.0f; }

struct SignalResult
{
    HRESULT hr;
    int     micChannels;
    float   peak[8];            // per microphone channel, after the warm-up
    float   peakAll;
    int     windows, dropouts;  // 10-ms windows while the tone plays / windows far below the loudest one
    int     latencyMs;          // tone start -> first loud microphone window (-1: not detected)
    wchar_t map[160];           // the tone's windows: '#' >= 1/2 of the loudest, '+' >= 1/4, '.' dropout
};

// Plays a 1-kHz tone of amplitude `amp` on speaker channel `onlyChannel` (-1: all) for `ms`, starting after 300 ms of
// silence, and captures the microphone at the same time.
static SignalResult RunSignalOnce(const wchar_t* spkId, const wchar_t* micId, int onlyChannel, float amp, int ms, int latencyMs)
{
    SignalResult r = {};
    r.latencyMs = -1;
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* ds = nullptr;
    IMMDevice* dm = nullptr;
    IAudioClient* rc = nullptr;
    IAudioClient* cc = nullptr;
    IAudioRenderClient* render = nullptr;
    IAudioCaptureClient* capture = nullptr;
    WAVEFORMATEX* rf = nullptr;
    WAVEFORMATEX* cf = nullptr;
    UINT32 rbuf = 0;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en);
    if (SUCCEEDED(hr)) hr = en->GetDevice(spkId, &ds);
    if (SUCCEEDED(hr)) hr = en->GetDevice(micId, &dm);
    if (SUCCEEDED(hr)) hr = ds->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&rc);
    if (SUCCEEDED(hr)) hr = dm->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&cc);
    if (SUCCEEDED(hr)) hr = rc->GetMixFormat(&rf);
    if (SUCCEEDED(hr)) hr = cc->GetMixFormat(&cf);
    if (SUCCEEDED(hr)) hr = rc->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, rf, nullptr);
    // micId == spkId: capture the speaker's own output (WASAPI loopback, what the audio engine sends to the driver).
    const bool loopback = wcscmp(spkId, micId) == 0;
    if (SUCCEEDED(hr)) hr = cc->Initialize(AUDCLNT_SHAREMODE_SHARED, loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0, 1000000, 0, cf, nullptr);
    if (SUCCEEDED(hr)) hr = rc->GetBufferSize(&rbuf);
    if (SUCCEEDED(hr))
    {
        // The test tone must reach the cable as written: no ducking, no turned-down session (Volume mixer).
        wchar_t fix[200];
        // Only the playing side: on a capture endpoint the session volume IS the endpoint volume (Windows 11 showed
        // "session volume 68 %" = the microphone at 0 dB), so setting it to 100 % turned the microphone to its maximum.
        PrepareTestSession(rc, fix, 200);
        if (fix[0]) Out(L"  info test stream (speaker): %ls", fix);
    }
    if (SUCCEEDED(hr)) hr = rc->GetService(__uuidof(IAudioRenderClient), (void**)&render);
    if (SUCCEEDED(hr)) hr = cc->GetService(__uuidof(IAudioCaptureClient), (void**)&capture);
    if (SUCCEEDED(hr)) hr = cc->Start();
    if (SUCCEEDED(hr)) hr = rc->Start();
    if (SUCCEEDED(hr))
    {
        const bool rFloat = IsFloatFormat(rf), cFloat = IsFloatFormat(cf);
        const int rBytes = rf->wBitsPerSample / 8, cBytes = cf->wBitsPerSample / 8;
        const int cch = cf->nChannels < 8 ? cf->nChannels : 8;
        r.micChannels = cch;
        // Silence first, longer than the cable delay: the microphone first plays back the tail of the previous
        // measurement (still in the cable); only what arrives after `skip` is analysed.
        const int silenceMs = latencyMs + 400, skipMs = latencyMs + 300;
        const UINT64 silence = (UINT64)rf->nSamplesPerSec * silenceMs / 1000, tone = (UINT64)rf->nSamplesPerSec * ms / 1000;
        UINT64 written = 0;
        double phase = 0, dphase = 2.0 * 3.14159265358979 * 1000.0 / rf->nSamplesPerSec;
        // Capture analysis in 10-ms windows (by capture sample time).
        const UINT32 win = cf->nSamplesPerSec / 100;
        UINT64 captured = 0;
        double winSq = 0;
        UINT32 winN = 0;
        float winRms[4096];
        int nWin = 0;
        DWORD t0 = GetTickCount();
        DWORD total = (DWORD)(silenceMs + ms + latencyMs + 400);
        while (GetTickCount() - t0 < total)
        {
            UINT32 pad = 0;
            rc->GetCurrentPadding(&pad);
            UINT32 n = rbuf - pad;
            BYTE* data = nullptr;
            if (n && SUCCEEDED(render->GetBuffer(n, &data)))
            {
                for (UINT32 i = 0; i < n; i++, written++)
                {
                    float v = 0;
                    if (written >= silence && written < silence + tone)
                    {
                        v = amp * (float)sin(phase);
                        phase += dphase;
                    }
                    for (int c = 0; c < rf->nChannels; c++)
                    {
                        float s = (onlyChannel < 0 || c == onlyChannel) ? v : 0.0f;
                        BYTE* p = data + (size_t)i * rf->nBlockAlign + c * rBytes;
                        if (rFloat && rBytes == 4) *(float*)p = s;
                        else if (rBytes == 2) *(short*)p = (short)(s * 32767.0f);
                        else if (rBytes == 4) *(int*)p = (int)(s * 2147483647.0);
                        else memset(p, 0, rBytes);
                    }
                }
                render->ReleaseBuffer(n, 0);
            }
            UINT32 packet = 0;
            while (SUCCEEDED(capture->GetNextPacketSize(&packet)) && packet > 0)
            {
                BYTE* cd = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                if (FAILED(capture->GetBuffer(&cd, &frames, &flags, nullptr, nullptr))) break;
                for (UINT32 i = 0; i < frames; i++, captured++)
                {
                    double sq = 0;
                    for (int c = 0; c < cch; c++)
                    {
                        float v = (flags & AUDCLNT_BUFFERFLAGS_SILENT) ? 0.0f
                                  : Sample(cd + (size_t)i * cf->nBlockAlign + c * cBytes, cFloat, cBytes);
                        float a = fabsf(v);
                        // peaks only once the tone had time to arrive (skip the first 150 ms of it)
                        if (captured > (UINT64)cf->nSamplesPerSec * skipMs / 1000 && a > r.peak[c]) r.peak[c] = a;
                        sq += (double)v * v;
                    }
                    winSq += sq / (cch ? cch : 1);
                    if (++winN == win)
                    {
                        if (nWin < 4096) winRms[nWin++] = (float)sqrt(winSq / win);
                        winSq = 0;
                        winN = 0;
                    }
                }
                capture->ReleaseBuffer(frames);
            }
            Sleep(5);
        }
        // Analysis: loudest window, the tone's windows (above 1/4 of it), dropouts inside the tone, latency.
        const int skipWin = skipMs / 10;
        float maxRms = 0;
        for (int i = skipWin; i < nWin; i++) if (winRms[i] > maxRms) maxRms = winRms[i];
        for (int c = 0; c < cch; c++) if (r.peak[c] > r.peakAll) r.peakAll = r.peak[c];
        int first = -1, last = -1;
        for (int i = skipWin; i < nWin; i++)
            if (maxRms > 0.001f && winRms[i] > maxRms * 0.25f) { if (first < 0) first = i; last = i; }
        if (first >= 0)
        {
            r.latencyMs = first * 10 - silenceMs;
            for (int i = first + 2; i <= last - 2; i++)
            {
                r.windows++;
                if (winRms[i] < maxRms * 0.25f) r.dropouts++;
            }
            int n = 0;
            for (int i = first; i <= last && n < 159; i++)
                r.map[n++] = winRms[i] >= maxRms * 0.5f ? L'#' : winRms[i] >= maxRms * 0.25f ? L'+' : L'.';
            r.map[n] = 0;
        }
    }
    r.hr = hr;
    if (rc) rc->Stop();
    if (cc) cc->Stop();
    if (render) render->Release();
    if (capture) capture->Release();
    if (rc) rc->Release();
    if (cc) cc->Release();
    if (rf) CoTaskMemFree(rf);
    if (cf) CoTaskMemFree(cf);
    if (ds) ds->Release();
    if (dm) dm->Release();
    if (en) en->Release();
    return r;
}

// Retries while Windows recreates the streams after a format change (AUDCLNT_E_DEVICE_INVALIDATED): the
// endpoint ids stay, the open streams become invalid for a moment.
static SignalResult RunSignal(const wchar_t* spkId, const wchar_t* micId, int onlyChannel, float amp, int ms, int latencyMs)
{
    SignalResult r = {};
    for (int attempt = 0; attempt < 6; attempt++)
    {
        r = RunSignalOnce(spkId, micId, onlyChannel, amp, ms, latencyMs);
        if (r.hr != (HRESULT)0x88890004) break;
        Out(L"  info streams invalidated (format change in progress), retrying");
        Sleep(700);
    }
    return r;
}

// ---------------------------------------------------------------------------
// Actions


// Audio processing objects (APOs) Windows runs for an endpoint before the driver: CLSIDs under the endpoint's
// FxProperties, with the DLL that implements each. Third-party ones can change or silence the sound.
static void LogEndpointEffects(const wchar_t* label, const wchar_t* id)
{
    const wchar_t* guid = wcschr(id + 1, L'{');
    if (!guid) return;
    wchar_t path[200];
    _snwprintf(path, 200, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\%ls\\%ls\\FxProperties",
               wcsncmp(id, L"{0.0.1.", 7) == 0 ? L"Capture" : L"Render", guid);
    path[199] = 0;
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
    {
        Out(L"  info %ls audio effects: none (no FxProperties)", label);
        return;
    }
    int found = 0;
    for (DWORD i = 0;; i++)
    {
        wchar_t name[256], data[512] = {};
        DWORD nlen = 256, dlen = sizeof(data) - 4, type = 0;
        if (RegEnumValueW(key, i, name, &nlen, nullptr, &type, (BYTE*)data, &dlen) != ERROR_SUCCESS) break;
        if (type != REG_SZ && type != REG_MULTI_SZ) continue;
        for (const wchar_t* clsid = data; *clsid; clsid += wcslen(clsid) + 1)
        {
            if (clsid[0] == L'{' && wcslen(clsid) == 38)
            {
                wchar_t sub[128], dll[MAX_PATH] = L"?";
                _snwprintf(sub, 128, L"CLSID\\%ls\\InprocServer32", clsid);
                sub[127] = 0;
                DWORD size = sizeof(dll) - 2;
                RegGetValueW(HKEY_CLASSES_ROOT, sub, nullptr, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, dll, &size);
                Out(L"  info %ls audio effect %ls: %ls (%ls)", label, name, clsid, dll);
                found++;
            }
            if (type == REG_SZ) break;
        }
    }
    RegCloseKey(key);
    if (!found) Out(L"  info %ls audio effects: none", label);
}

void LogMicSessions()
{
    wchar_t s[1500];
    DescribeAudioSessions(g_mic.id, s, 1500);
    wprintf(L"  info audio sessions on the microphone: %ls\n", s);
    AppLog(L"  info audio sessions on the microphone: %ls", s);
}

// Re-registers the outside-volume watch on the current microphone (a device restart ends the old one).
static void WatchMic()
{
    static void* watch;
    UnwatchEndpointVolume(watch);
    watch = WatchEndpointVolume(g_mic.id, OnForeignVolume, nullptr);
}

static float DbToPercentF(float db) { return 100.0f * powf(10.0f, db / 20.0f); }

// Sets the microphone to 0 dB / unmuted and waits until the endpoint reports it (another process may be changing
// it at the same time). Returns the value read back.
static float MicToUnity()
{
    float db = 999, mn = 0, mx = 0;
    for (int i = 0; i < 10; i++)
    {
        SetEndpointVolumeDb(g_mic.id, 0.0f);
        SetEndpointMute(g_mic.id, false);
        Sleep(100);
        bool mute = true;
        if (GetEndpointVolumeDb(g_mic.id, &db, &mn, &mx) && fabsf(db) < 0.1f && GetEndpointMute(g_mic.id, &mute) && !mute) break;
    }
    return db;
}

// Checks one channel's measurement; `report` = log failures (false: only count them, for the first attempt).
static int EvaluateChannel(const SignalResult& r, int ch, int spkChannels, const Quality& q, bool report)
{
    if (report && !Check(SUCCEEDED(r.hr), L"channel %d/%d: streams opened (0x%08lX)", ch + 1, spkChannels, (unsigned long)r.hr))
        return 1;
    if (FAILED(r.hr)) return 1;
    int bad = 0;
    bool reach = Db(r.peakAll) > -40.0f, clip = Db(r.peakAll) < -1.0f;
    bool level = !(r.micChannels >= spkChannels && ch < r.micChannels && fabsf(Db(r.peak[ch]) + 20.0f) > 3.0f);
    bool drop = !(r.windows > 0 && r.dropouts * 20 > r.windows);
    bad += !reach + !clip + !level + !drop;
    if (!report) return bad;
    Check(reach, L"channel %d/%d reaches the microphone (%d ch): peak %.1f dBFS > -40", ch + 1, spkChannels, r.micChannels, Db(r.peakAll));
    Check(clip, L"channel %d/%d not clipped: peak %.1f dBFS < -1", ch + 1, spkChannels, Db(r.peakAll));
    // Level of every microphone channel (for the log), and the expected -20 dBFS when the cable passes the
    // speaker channel 1:1 (the microphone has at least as many channels).
    wchar_t peaks[200];
    int n = 0;
    for (int c = 0; c < r.micChannels && n < 190; c++) n += _snwprintf(peaks + n, 200 - n, L" %.1f", Db(r.peak[c]));
    peaks[199] = 0;
    Out(L"  info channel %d/%d microphone peaks (dBFS):%ls", ch + 1, spkChannels, peaks);
    if (!level)
        Warn(L"channel %d/%d arrives at %.1f dBFS on microphone channel %d (expected -20 +-3, 1:1)", ch + 1, spkChannels,
             Db(r.peak[ch]), ch + 1);
    if (r.windows > 0 && r.dropouts > 0)
    {
        if (!drop) Check(false, L"channel %d/%d: %d of %d 10-ms windows dropped out", ch + 1, spkChannels, r.dropouts, r.windows);
        else Warn(L"channel %d/%d: %d of %d 10-ms windows dropped out", ch + 1, spkChannels, r.dropouts, r.windows);
    }
    if (bad || r.dropouts) Out(L"  info channel %d/%d windows: %ls", ch + 1, spkChannels, r.map);
    if (r.latencyMs >= 0)
    {
        // cable latency + engine buffers; generous bound
        int bound = (int)q.latency + 250;
        if (r.latencyMs > bound) Warn(L"channel %d/%d: latency %d ms > %d ms", ch + 1, spkChannels, r.latencyMs, bound);
        else Out(L"  info channel %d/%d latency ~%d ms", ch + 1, spkChannels, r.latencyMs);
    }
    return bad;
}

// Upper limit for "silence" on the microphone: -80 dBFS, but a 16-bit speaker carries the engine's dither (+-1 LSB =
// -90.3 dBFS per channel), and a microphone with fewer channels sums several of them (8 -> 2: up to ~4-5 LSB, about
// -78 dBFS).
static float QuietLimit(const Quality& q)
{
    WAVEFORMATEX* f = nullptr;
    int spkChannels = 2;
    if (SUCCEEDED(PolicyGetMixFormat(g_spk.id, &f)) && f) { spkChannels = f->nChannels; CoTaskMemFree(f); }
    float quietLimit = -80.0f;
    DWORD spkRate = 0;
    WORD spkBits = 0, spkCh = 0;
    // the endpoint's current default format (a [format] action may have changed it from the driver setting)
    if (!GetEndpointFormat(g_spk.id, &spkRate, &spkBits, &spkCh)) spkBits = (WORD)q.bits;
    if (spkBits == 16)
    {
        int sum = (int)q.micChannels != 0 && (int)q.micChannels < spkChannels ? spkChannels : 1;
        float lsb = Db(1.5f * sum / 32768.0f);
        if (lsb > quietLimit) quietLimit = lsb;
    }
    return quietLimit;
}

static void ActionSignal()
{
    Out(L"[signal] tone on every speaker channel, measured on the microphone");
    // Unity microphone gain for the measurement (restored afterwards).
    float db = 0, mn = 0, mx = 0;
    bool mute = false;
    GetEndpointVolumeDb(g_mic.id, &db, &mn, &mx);
    GetEndpointMute(g_mic.id, &mute);
    float unity = MicToUnity();
    Check(fabsf(unity) < 0.1f, L"microphone volume set to 0 dB for the measurement (reads %.2f dB, was %.2f dB)", unity, db);
    Quality q = ReadQuality();

    WAVEFORMATEX* f = nullptr;
    int spkChannels = 2;
    if (SUCCEEDED(PolicyGetMixFormat(g_spk.id, &f)) && f) { spkChannels = f->nChannels; CoTaskMemFree(f); }
    float quietLimit = QuietLimit(q);
    SignalResult quiet = RunSignal(g_spk.id, g_mic.id, -1, 0.0f, 300, (int)q.latency);
    if (Check(SUCCEEDED(quiet.hr), L"silence run opened both streams (0x%08lX)", (unsigned long)quiet.hr))
    {
        float peak = Db(quiet.peakAll);
        if (peak >= quietLimit)
        {
            // Something else reached the microphone (another program's sound, a recorder that raised the volume, an
            // old buffer): log who uses the endpoints and the volume now, then listen once more. A clean repeat is a
            // passing glitch (warning); only a repeated one fails.
            float vdb = 0, vmn = 0, vmx = 0;
            bool vmute = false;
            GetEndpointVolumeDb(g_mic.id, &vdb, &vmn, &vmx);
            GetEndpointMute(g_mic.id, &vmute);
            Out(L"  info silence first attempt: peak %.1f dBFS, %d/%d windows dropped; microphone volume now %.2f dB%ls", peak,
                quiet.dropouts, quiet.windows, vdb, vmute ? L", muted" : L"");
            LogMicSessions();
            MicToUnity();
            Sleep(300);
            SignalResult again = RunSignal(g_spk.id, g_mic.id, -1, 0.0f, 300, (int)q.latency);
            float peak2 = SUCCEEDED(again.hr) ? Db(again.peakAll) : 0.0f;
            if (peak2 < quietLimit)
                Warn(L"silence: the first measurement had %.1f dBFS, the repeat is clean (%.1f dBFS; passing glitch, see above)", peak,
                     peak2);
            else
                Check(false, L"silence: microphone peak %.1f dBFS, again %.1f dBFS (< %.1f wanted)", peak, peak2, quietLimit);
        }
        else
            Check(true, L"silence: microphone peak %.1f dBFS < %.1f", peak, quietLimit);
    }
    for (int ch = 0; ch < spkChannels && !g_stop; ch++)
    {
        SignalResult r = RunSignal(g_spk.id, g_mic.id, ch, 0.1f, 700, (int)q.latency);
        if (SUCCEEDED(r.hr) && EvaluateChannel(r, ch, spkChannels, q, false))
        {
            // Something is off: log what the first attempt saw and the state now, then measure once more. A clean
            // second run means a passing glitch (warning); only a repeated problem fails.
            float vdb = 0;
            bool vmute = false;
            GetEndpointVolumeDb(g_mic.id, &vdb, &mn, &mx);
            GetEndpointMute(g_mic.id, &vmute);
            wchar_t peaks[200];
            int n = 0;
            for (int c = 0; c < r.micChannels && n < 190; c++) n += _snwprintf(peaks + n, 200 - n, L" %.1f", Db(r.peak[c]));
            peaks[199] = 0;
            WAVEFORMATEX *sf = nullptr, *mf = nullptr;
            PolicyGetMixFormat(g_spk.id, &sf);
            PolicyGetMixFormat(g_mic.id, &mf);
            Out(L"  info channel %d/%d first attempt: peaks%ls, %d/%d windows dropped [%ls]; microphone volume now %.2f dB%ls; "
                L"mix formats %lu Hz %u ch / %lu Hz %u ch", ch + 1, spkChannels, peaks, r.dropouts, r.windows, r.map, vdb,
                vmute ? L" (muted)" : L"", sf ? sf->nSamplesPerSec : 0, sf ? sf->nChannels : 0, mf ? mf->nSamplesPerSec : 0,
                mf ? mf->nChannels : 0);
            if (sf) CoTaskMemFree(sf);
            if (mf) CoTaskMemFree(mf);
            if (r.peakAll < 1e-6f)
            {
                // Nothing at all arrived: the speaker side (muted / turned down by Windows or another program, e.g.
                // communications ducking while a call app is active), the driver's microphone node, or the sessions.
                float sdb = 0, smn = 0, smx = 0;
                bool smute = false;
                GetEndpointVolumeDb(g_spk.id, &sdb, &smn, &smx);
                GetEndpointMute(g_spk.id, &smute);
                wchar_t ss[1500], ms[1500];
                DescribeAudioSessions(g_spk.id, ss, 1500);
                DescribeAudioSessions(g_mic.id, ms, 1500);
                Out(L"  info silence: speaker volume %.2f dB%ls; speaker sessions: %ls; microphone sessions: %ls", sdb,
                    smute ? L" MUTED" : L"", ss, ms);
                DumpMicNodeLog(8);
                // Control: does Windows deliver the tone to the speaker at all? Loopback = the mixed output the audio
                // engine hands to the driver. Tone there + silence on the microphone: lost in or after the driver
                // (cable, a filter driver of another program); silence there too: stopped before the driver.
                SignalResult lb = RunSignal(g_spk.id, g_spk.id, ch, 0.1f, 500, 0);
                LogEndpointEffects(L"speaker", g_spk.id);
                {
                    // Windows keeps the sound of a session that is not the active one at the console (locked screen,
                    // disconnected VM console / remote desktop, another user) away from the devices.
                    DWORD mine = 0;
                    ProcessIdToSessionId(GetCurrentProcessId(), &mine);
                    DWORD console = WTSGetActiveConsoleSessionId();
                    LPWSTR buf = nullptr;
                    DWORD bytes = 0;
                    const wchar_t* lock = L"unknown";
                    if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION, WTSSessionInfoEx, &buf, &bytes) && buf)
                    {
                        WTSINFOEXW* ex = (WTSINFOEXW*)buf;
                        LONG flags = ex->Data.WTSInfoExLevel1.SessionFlags;
                        lock = flags == WTS_SESSIONSTATE_LOCK ? L"LOCKED" : flags == WTS_SESSIONSTATE_UNLOCK ? L"unlocked" : L"unknown";
                        WTSFreeMemory(buf);
                    }
                    Out(L"  info silence check: this session %lu, active console session %lu%ls, screen %ls", mine, console,
                        mine == console ? L"" : L" (NOT this one)", lock);
                }
                Out(L"  info silence check: speaker loopback %ls peak %.1f dBFS (0x%08lX) -> %ls", SUCCEEDED(lb.hr) ? L"ok," : L"FAILED,",
                    Db(lb.peakAll), (unsigned long)lb.hr,
                    FAILED(lb.hr) ? L"unknown" : Db(lb.peakAll) > -40.0f ? L"the tone reaches the speaker: lost in the driver/cable or a filter after it"
                                                                         : L"the tone does not even reach the speaker: stopped in Windows before the driver");
            }
            if (fabsf(vdb) >= 0.1f || vmute) Warn(L"microphone volume changed during the measurement: %.2f dB%ls", vdb, vmute ? L", muted" : L"");
            MicToUnity();
            Sleep(1000);
            SignalResult again = RunSignal(g_spk.id, g_mic.id, ch, 0.1f, 700, (int)q.latency);
            if (SUCCEEDED(again.hr) && EvaluateChannel(again, ch, spkChannels, q, false) == 0)
                Warn(L"channel %d/%d: the first measurement was off, the repeat is clean (passing glitch, see above)", ch + 1, spkChannels);
            r = again;
        }
        EvaluateChannel(r, ch, spkChannels, q, true);
    }
    SetEndpointVolumeDb(g_mic.id, db);
    SetEndpointMute(g_mic.id, mute);
}

static void ActionApply()
{
    static const DWORD rates[] = { 8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000, 176400, 192000 };
    static const DWORD chans[] = { 1, 2, 4, 6, 8 };
    static const DWORD mics[] = { 0, 1, 2, 4, 6, 8 };
    static const DWORD bits[] = { 16, 24, 32 };
    Quality q;
    q.rate = rates[Rand(0, 10)];
    q.channels = chans[Rand(0, 4)];
    q.micChannels = mics[Rand(0, 5)];
    q.bits = bits[Rand(0, 2)];
    q.latency = (DWORD)Rand(10, 200);
    Out(L"[apply] %lu Hz, %lu bit, speaker %lu ch, microphone %lu ch, latency %lu ms", q.rate, q.bits, q.channels,
        q.micChannels, q.latency);
    // The microphone level right before the restart (Windows may bring back an older one: SetFormats holds it).
    float volBefore = 0, vmn = 0, vmx = 0;
    bool muteBefore = false;
    bool level = GetEndpointVolumeDb(g_mic.id, &volBefore, &vmn, &vmx) && GetEndpointMute(g_mic.id, &muteBefore);
    if (!Check(ApplyQuality(q), L"device restarted")) return;
    if (!WaitOurs(&g_spk, &g_mic, 20000))
    {
        // a stall (seen once at 192 kHz / 32 bit / 8 ch): a failure, but the run goes on once they are back
        Check(false, L"both endpoints back within 20 s");
        if (!RecoverEndpoints(L"the restart")) return;
    }
    else
        Check(true, L"both endpoints back");
    WatchMic();
    wchar_t cfg[256];
    wchar_t want[128];
    _snwprintf(want, 128, L"rate=%lu channels=%lu mic channels=%lu", q.rate, q.channels, MicCh(q));
    want[127] = 0;
    // The driver rewrites its log in the registry while it starts: a read can fall between two writes (seen once:
    // an empty line), so look again for a moment.
    bool cfgOk = false;
    for (int i = 0; i < 10 && !(cfgOk = DriverConfigLine(cfg, 256) && wcsstr(cfg, want) != nullptr); i++) Sleep(200);
    Check(cfgOk, L"driver runs with it (%ls)", cfg);
    int fixes = SetFormats(q, g_spk, g_mic, level, volBefore, muteBefore);
    if (fixes) Out(L"  info after the restart Windows brought back older values %d time(s), set again", fixes);
    {
        float db = 99, mn = 0, mx = 0;
        bool mute = !muteBefore;
        GetEndpointVolumeDb(g_mic.id, &db, &mn, &mx);
        GetEndpointMute(g_mic.id, &mute);
        if (!Check(fabsf(db - volBefore) < 0.1f && mute == muteBefore, L"microphone volume after the restart %.2f dB%ls (before %.2f dB%ls)",
                   db, mute ? L" muted" : L"", volBefore, muteBefore ? L" muted" : L""))
            DumpMicNodeLog(12);
    }
    DWORD r = 0;
    WORD b = 0, c = 0;
    Check(GetEndpointFormat(g_spk.id, &r, &b, &c) && r == q.rate && c == q.channels,
          L"speaker default format %lu Hz %u bit %u ch (want %lu Hz %lu ch)", r, b, c, q.rate, q.channels);
    Check(GetEndpointFormat(g_mic.id, &r, &b, &c) && r == q.rate && c == MicCh(q),
          L"microphone default format %lu Hz %u bit %u ch (want %lu Hz %lu ch)", r, b, c, q.rate, MicCh(q));
    ActionSignal();
}

static void ActionRename()
{
    static const wchar_t* pool[] = { L"Test", L"Тест", L"Мир", L"Symo", L"Кабель", L"Line", L"Ω", L"Zażółć", L"日本", L"X" };
    wchar_t names[2][80];
    for (int side = 0; side < 2; side++)
    {
        int n = _snwprintf(names[side], 80, L"%ls", side == 0 ? L"Spk" : L"Mic");
        int parts = Rand(1, 4);
        for (int i = 0; i < parts && n < 50; i++)
            n += _snwprintf(names[side] + n, 80 - n, L" %ls%d", pool[Rand(0, 9)], Rand(0, 999));
        names[side][79] = 0;
    }
    Out(L"[rename] \"%ls\" / \"%ls\"", names[0], names[1]);
    Check(SUCCEEDED(SetEndpointName(g_spk.id, names[0])), L"speaker renamed");
    Check(SUCCEEDED(SetEndpointName(g_mic.id, names[1])), L"microphone renamed");
    Sleep(400);
    AudioDevice s, m;
    if (Check(FindOurs(&s, &m), L"endpoints found after renaming"))
    {
        Check(wcscmp(s.desc, names[0]) == 0, L"speaker name reads back (\"%ls\")", s.desc);
        Check(wcscmp(m.desc, names[1]) == 0, L"microphone name reads back (\"%ls\")", m.desc);
        g_spk = s;
        g_mic = m;
    }
}

static void ActionVolume()
{
    int pct = Rand(0, 300);
    bool mute = Rand(0, 1) != 0;
    float want = pct > 0 ? 20.0f * log10f(pct / 100.0f) : -96.0f;
    Out(L"[volume] microphone %d %% (%.1f dB), mute %ls", pct, want, mute ? L"on" : L"off");
    Check(SetEndpointVolumeDb(g_mic.id, want), L"volume set");
    Check(SetEndpointMute(g_mic.id, mute), L"mute set");
    float db = 0, mn = 0, mx = 0;
    bool m = !mute;
    HRESULT hr = S_OK;
    bool read = GetEndpointVolumeDb(g_mic.id, &db, &mn, &mx, &hr);
    if (!read || FAILED(hr))
    {
        // Seen after device restarts: the master level fails with E_INVALIDARG for a while. Log every read.
        wchar_t d[400];
        DescribeEndpointVolume(g_mic.id, d, 400);
        Out(L"  info volume read %ls (0x%08lX): %ls", read ? L"used the channel levels" : L"failed", (unsigned long)hr, d);
        if (!read)
        {
            Sleep(300);
            read = GetEndpointVolumeDb(g_mic.id, &db, &mn, &mx, &hr);
            if (read) Warn(L"volume read failed once, the second read worked (passing glitch)");
        }
        else Warn(L"master volume not readable (0x%08lX), the channel levels were used", (unsigned long)hr);
    }
    if (Check(read, L"volume read (0x%08lX)", (unsigned long)hr))
    {
        float expect = want < mn ? mn : (want > mx ? mx : want);
        Check(fabsf(db - expect) < 0.6f, L"volume reads back %.2f dB (want %.2f)", db, expect);
    }
    Check(GetEndpointMute(g_mic.id, &m) && m == mute, L"mute reads back");
}

static void ActionFormat()
{
    static const WORD bits[] = { 16, 24, 32 };
    Quality q = ReadQuality();
    WORD b = bits[Rand(0, 2)];
    bool mic = Rand(0, 1) != 0;
    const AudioDevice& d = mic ? g_mic : g_spk;
    DWORD ch = mic ? MicCh(q) : q.channels;
    Out(L"[format] %ls: %lu Hz %u bit %lu ch", mic ? L"microphone" : L"speaker", q.rate, b, ch);
    wchar_t err[160];
    bool ok = SetEndpointFormat(d.id, q.rate, b, (WORD)ch, err, 160);
    if (!ok && b > 16)
    {
        Warn(L"%u bit refused (%ls), trying 16", b, err);
        ok = SetEndpointFormat(d.id, q.rate, 16, (WORD)ch, err, 160);
    }
    Check(ok, L"default format accepted (%ls)", err);
    DWORD r = 0;
    WORD rb = 0, rc = 0;
    Check(GetEndpointFormat(d.id, &r, &rb, &rc) && r == q.rate && rc == ch, L"reads back %lu Hz %u bit %u ch", r, rb, rc);
}

static void ActionStress()
{
    int rounds = Rand(10, 40), failed = 0;
    Out(L"[stress] %d x open/start/stop/close on both endpoints", rounds);
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en)))
    {
        Check(false, L"device enumerator");
        return;
    }
    for (int i = 0; i < rounds && !g_stop; i++)
    {
        for (int side = 0; side < 2; side++)
        {
            IMMDevice* dev = nullptr;
            IAudioClient* ac = nullptr;
            WAVEFORMATEX* f = nullptr;
            HRESULT hr = en->GetDevice(side == 0 ? g_spk.id : g_mic.id, &dev);
            if (SUCCEEDED(hr)) hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac);
            if (SUCCEEDED(hr)) hr = ac->GetMixFormat(&f);
            if (SUCCEEDED(hr)) hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, (REFERENCE_TIME)Rand(1, 20) * 100000, 0, f, nullptr);
            if (SUCCEEDED(hr)) hr = ac->Start();
            if (SUCCEEDED(hr)) { Sleep(Rand(0, 30)); hr = ac->Stop(); }
            if (FAILED(hr)) failed++;
            if (f) CoTaskMemFree(f);
            if (ac) ac->Release();
            if (dev) dev->Release();
        }
    }
    en->Release();
    Check(failed == 0, L"%d stream operations failed", failed);
}

struct TestSoundCtx { wchar_t id[256]; volatile LONG stop; HRESULT hr; };

// WINAPI: a thread function must be stdcall on x86 (a lambda is not).
static DWORD WINAPI TestSoundCtxThread(LPVOID p)
{
    TestSoundCtx* c = (TestSoundCtx*)p;
    c->hr = PlayChannelTest(c->id, &c->stop);
    return 0;
}

static void ActionTestSound()
{
    Out(L"[testsound] the panel's \"Test\" for ~2 s");
    TestSoundCtx ctx = {};
    wcsncpy(ctx.id, g_spk.id, 255);
    HANDLE t = CreateThread(nullptr, 0, TestSoundCtxThread, &ctx, 0, nullptr);
    if (!Check(t != nullptr, L"test thread started")) return;
    Sleep(2000);
    InterlockedExchange(&ctx.stop, 1);
    bool ended = WaitForSingleObject(t, 5000) == WAIT_OBJECT_0;
    CloseHandle(t);
    Check(ended, L"test stops when asked");
    Check(SUCCEEDED(ctx.hr), L"test sound played (0x%08lX)", (unsigned long)ctx.hr);
}

// ---------------------------------------------------------------------------
// Command line (s2mctl.exe next to this program)

static int RunCtl(const wchar_t* args)
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    wchar_t* slash = wcsrchr(exe, L'\\');
    wcscpy(slash ? slash + 1 : exe, L"s2mctl.exe");
    wchar_t cmd[1200];
    _snwprintf(cmd, 1200, L"\"%ls\" %ls", exe, args);
    cmd[1199] = 0;
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;          // its output stays out of this console (it logs to ctl.log)
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exe, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    {
        Out(L"  cannot start %ls (%lu)", exe, GetLastError());
        return -1;
    }
    WaitForSingleObject(pi.hProcess, 120000);
    DWORD code = (DWORD)-1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

static void ActionCli()
{
    int which = Rand(0, 8);
    AudioDevice s, m;
    switch (which)
    {
    case 0:
        Out(L"[cli] status");
        Check(RunCtl(L"status") == 0, L"s2mctl status exit code 0");
        break;
    case 1:
    {
        int pct = Rand(0, 300);
        wchar_t a[64];
        _snwprintf(a, 64, L"volume %d", pct);
        Out(L"[cli] %ls", a);
        if (!Check(RunCtl(a) == 0, L"exit code 0")) break;
        float db = 0, mn = 0, mx = 0;
        HRESULT hr = S_OK;
        if (!Check(GetEndpointVolumeDb(g_mic.id, &db, &mn, &mx, &hr), L"volume read (0x%08lX)", (unsigned long)hr)) break;
        float want = pct > 0 ? 20.0f * log10f(pct / 100.0f) : mn;
        if (want < mn) want = mn;
        Check(fabsf(db - want) < 0.6f, L"volume %.2f dB (want %.2f)", db, want);
        break;
    }
    case 2:
    {
        bool mute = Rand(0, 1) != 0;
        Out(L"[cli] mute %ls", mute ? L"on" : L"off");
        if (!Check(RunCtl(mute ? L"mute on" : L"mute off") == 0, L"exit code 0")) break;
        bool m2 = !mute;
        Check(GetEndpointMute(g_mic.id, &m2) && m2 == mute, L"mute reads back");
        break;
    }
    case 3:
    {
        static const wchar_t* pool[] = { L"Cli", L"Тест", L"Консоль", L"Name", L"Ümlaut", L"Kabel" };
        wchar_t sn[64], mn[64], a[200];
        _snwprintf(sn, 64, L"%ls %d", pool[Rand(0, 5)], Rand(0, 9999));
        _snwprintf(mn, 64, L"%ls %d", pool[Rand(0, 5)], Rand(0, 9999));
        _snwprintf(a, 200, L"name --speaker \"%ls\" --mic \"%ls\"", sn, mn);
        Out(L"[cli] %ls", a);
        if (!Check(RunCtl(a) == 0, L"exit code 0")) break;
        Sleep(400);
        if (Check(FindOurs(&s, &m), L"endpoints found"))
        {
            Check(wcscmp(s.desc, sn) == 0, L"speaker \"%ls\"", s.desc);
            Check(wcscmp(m.desc, mn) == 0, L"microphone \"%ls\"", m.desc);
            g_spk = s;
            g_mic = m;
        }
        break;
    }
    case 4:
    {
        // export -> change -> import -> the exported state is back
        wchar_t file[MAX_PATH], a[MAX_PATH + 32];
        GetTempPathW(MAX_PATH, file);
        wcsncat(file, L"s2m_autotest.ini", MAX_PATH - wcslen(file) - 1);
        float db0 = 0, mn = 0, mx = 0;
        GetEndpointVolumeDb(g_mic.id, &db0, &mn, &mx);
        LONG outsideBefore = g_outsideChanges;
        wchar_t name0[256];
        wcscpy(name0, g_spk.desc);
        Out(L"[cli] export / change / import (%ls)", file);
        _snwprintf(a, MAX_PATH + 32, L"export \"%ls\"", file);
        if (!Check(RunCtl(a) == 0, L"export exit code 0")) break;
        Check(GetPrivateProfileIntW(L"Speak2Mic", L"SampleRate", 0, file) == (UINT)ReadQuality().rate, L"export has the sample rate");
        SetEndpointVolumeDb(g_mic.id, db0 > -20.0f ? -20.0f : 0.0f);
        SetEndpointName(g_spk.id, L"Autotest temporary");
        _snwprintf(a, MAX_PATH + 32, L"import \"%ls\"", file);
        if (!Check(RunCtl(a) == 0, L"import exit code 0")) break;
        Sleep(400);
        float db = 0;
        GetEndpointVolumeDb(g_mic.id, &db, &mn, &mx);
        float was = DbToPercentF(db0) > 300.0f ? 300.0f : DbToPercentF(db0);     // import clamps to the panel's 0..300 %
        if (fabsf(DbToPercentF(db) - was) > 1.0f && g_outsideChanges != outsideBefore)
            Warn(L"volume back: %.1f %% (was %.1f %%), but another program changed the microphone volume meanwhile (%ld "
                 L"time(s), see the audio sessions above)", DbToPercentF(db), DbToPercentF(db0), g_outsideChanges - outsideBefore);
        else
            Check(fabsf(DbToPercentF(db) - was) <= 1.0f, L"volume back: %.1f %% (was %.1f %%)", DbToPercentF(db), DbToPercentF(db0));
        if (FindOurs(&s, &m))
        {
            Check(wcscmp(s.desc, name0) == 0, L"speaker name back: \"%ls\"", s.desc);
            g_spk = s;
            g_mic = m;
        }
        DeleteFileW(file);
        break;
    }
    case 5:
    {
        static const wchar_t* bad[] = { L"set --rate 12345", L"volume 999", L"mute maybe", L"bogus", L"set --channels 3",
                                        L"set --latency 5", L"name --speaker", L"set --preset loud" };
        const wchar_t* a = bad[Rand(0, 7)];
        Out(L"[cli] invalid: %ls", a);
        Check(RunCtl(a) == 2, L"rejected with exit code 2");
        break;
    }
    case 6:
    {
        static const wchar_t* presets[] = { L"voice", L"standard", L"high", L"studio", L"max" };
        static const DWORD rates[] = { 16000, 48000, 48000, 96000, 192000 };
        int p = Rand(0, 4);
        wchar_t a[80];
        _snwprintf(a, 80, L"set --preset %ls --latency %d", presets[p], Rand(10, 100));
        Out(L"[cli] %ls", a);
        if (!Check(RunCtl(a) == 0, L"exit code 0")) break;
        Quality q = ReadQuality();
        Check(q.rate == rates[p] && q.micChannels == 1, L"driver settings %lu Hz, microphone %lu ch", q.rate, q.micChannels);
        if (Check(WaitOurs(&g_spk, &g_mic, 20000), L"endpoints back"))
        {
            DWORD r = 0;
            WORD b = 0, c = 0;
            Check(GetEndpointFormat(g_spk.id, &r, &b, &c) && r == q.rate && c == q.channels,
                  L"speaker default format %lu Hz %u ch", r, c);
        }
        break;
    }
    case 7:
    {
        static const DWORD rates[] = { 22050, 44100, 48000, 96000 };
        static const DWORD mics[] = { 0, 1, 2, 4 };
        DWORD rate = rates[Rand(0, 3)], ch = (DWORD)(Rand(0, 1) ? 2 : 6), mic = mics[Rand(0, 3)];
        wchar_t a[120];
        _snwprintf(a, 120, L"set --rate %lu --channels %lu --mic-channels %lu --bits 24", rate, ch, mic);
        Out(L"[cli] %ls", a);
        if (!Check(RunCtl(a) == 0, L"exit code 0")) break;
        Quality q = ReadQuality();
        Check(q.rate == rate && q.channels == ch && q.micChannels == mic && q.bits == 24, L"driver settings as requested");
        if (Check(WaitOurs(&g_spk, &g_mic, 20000), L"endpoints back"))
        {
            DWORD r = 0;
            WORD b = 0, c = 0;
            Check(GetEndpointFormat(g_mic.id, &r, &b, &c) && r == rate && c == (mic ? mic : ch),
                  L"microphone default format %lu Hz %u ch", r, c);
        }
        break;
    }
    default:
        Out(L"[cli] test 2");
        Check(RunCtl(L"test 2") == 0, L"s2mctl test exit code 0");
        break;
    }
}

static int g_orphansAtStart;

static int g_orphansKnown;       // reported so far (every new one fails once)

// After every action: new leftover endpoint records, with the action that left them.
static void CheckNewOrphans(const wchar_t* action)
{
    static wchar_t ids[128][80];
    int n = S2mFindOrphanEndpoints(ids, 128, false);
    if (n < 0) return;                          // our endpoints are restarting: nothing to judge (keep the count)
    if (n <= g_orphansKnown)
    {
        g_orphansKnown = n;
        return;
    }
    S2mFindOrphanEndpoints(ids, 128, true);       // once more, with the names in the log
    Out(L"  info active endpoints: speaker %ls, microphone %ls", g_spk.id, g_mic.id);
    Check(false, L"%d new leftover endpoint record(s) after [%ls]", n - g_orphansKnown, action);
    g_orphansKnown = n;
}

// The panel's "Play / Pause": the mp3 player (same code) on the speaker, measured on the microphone.
static TestMusic g_music;          // generated at the start, removed at the end (atmusic.h)

static void MusicLogLine(const wchar_t* line) { Out(L"%ls", line); }

static void ActionMusic()
{
    // The source: the generated music (one format, or all of them with a broken file and a text file among them),
    // the Generator, or the program's own mp3 folder when nothing could be generated.
    wchar_t folder[MAX_PATH];
    const wchar_t* what = L"";
    int pick = g_music.formatCount ? Rand(0, g_music.formatCount + 1) : -1;
    if (pick >= 0 && pick < g_music.formatCount)
    {
        wcscpy(folder, g_music.formats[pick].folder);
        what = g_music.formats[pick].format;
    }
    else if (pick == g_music.formatCount)
    {
        wcscpy(folder, g_music.all);
        what = L"all formats + a broken file";
    }
    else if (pick > g_music.formatCount)
    {
        wcscpy(folder, MP3_GENERATOR);
        what = L"Generator";
    }
    else
    {
        Mp3DefaultFolder(folder);
        what = L"the program's folder";
        if (!Mp3FolderHasFiles(folder))
        {
            Out(L"[music] skipped: no test music could be made and no music files in %ls", folder);
            return;
        }
    }
    Out(L"[music] %ls (%ls) on the speaker", what, folder);

    // A folder without music (a text file only): nothing starts (the panel's button is disabled then).
    if (g_music.empty[0])
    {
        Check(!Mp3FolderHasFiles(g_music.empty), L"folder without music: no files found");
        Check(!Mp3Play(g_spk.id, g_music.empty, nullptr, 0) && !Mp3Playing(), L"folder without music: nothing plays");
    }

    float db = 0, mn = 0, mx = 0;
    bool mute = false;
    GetEndpointVolumeDb(g_mic.id, &db, &mn, &mx);
    GetEndpointMute(g_mic.id, &mute);
    MicToUnity();
    Quality q = ReadQuality();
    // Music has quiet passages: a generous limit over 1.5 s (repeated, see musicLevel).
    auto level = [&]() { SignalResult r = RunSignal(g_spk.id, g_mic.id, -1, 0.0f, 1500, (int)q.latency); return SUCCEEDED(r.hr) ? Db(r.peakAll) : -999.0f; };
    // The music files have silent passages, also at their ends (a continued track near its end + the next one's
    // lead-in: over 2 s): while the player still plays, listen up to twice more before calling it silence.
    auto musicLevel = [&]() {
        float peak = level();
        for (int retry = 1; retry <= 2 && peak <= -60.0f && Mp3Playing(); retry++)
        {
            Out(L"  info music: %.1f dBFS (a silent passage? %ls), listening again", peak, Mp3LastFile());
            peak = level();
        }
        return peak;
    };

    if (Check(Mp3Play(g_spk.id, folder, nullptr, 0) && Mp3Playing(), L"playback started"))
    {
        Sleep(300);
        wchar_t track[MAX_PATH];
        wcsncpy(track, Mp3LastFile(), MAX_PATH - 1);
        track[MAX_PATH - 1] = 0;
        const wchar_t* name = wcsrchr(track, L'\\');
        float on = musicLevel();
        Check(on > -60.0f, L"music reaches the microphone: peak %.1f dBFS > -60 (%ls)", on, name ? name + 1 : track);
        Check(Mp3Playing(), L"still playing after the measurement");

        // The track may have ended during the measurement (a continued one near its end): the player then went on to
        // the next one - that is the track Pause / Play must keep.
        // The track paused is the one the player was on when it stopped (a short track may have ended a moment
        // before the pause and the next one begun): read after the pause.
        Mp3Pause();
        wcsncpy(track, Mp3LastFile(), MAX_PATH - 1);
        track[MAX_PATH - 1] = 0;
        Check(!Mp3Playing(), L"paused");
        LONG outsideBefore = g_outsideChanges;
        float off = level();
        float quietLimit = QuietLimit(q);      // the dither of a 16-bit speaker is no music
        if (off >= quietLimit)
        {
            // As the silence of [signal]: a recorder may have raised the volume meanwhile; once more at unity gain.
            Out(L"  info paused first attempt: %.1f dBFS", off);
            LogMicSessions();
            MicToUnity();
            float again = level();
            if (again < quietLimit) Warn(L"paused: the first measurement had %.1f dBFS, the repeat is clean (%.1f dBFS)", off, again);
            else if (g_outsideChanges != outsideBefore)
                Warn(L"paused: microphone peak %.1f dBFS, again %.1f dBFS (< %.1f wanted), but another program changed the "
                     L"microphone volume meanwhile (%ld time(s), see the audio sessions above)", off, again, quietLimit,
                     g_outsideChanges - outsideBefore);
            else Check(false, L"paused: microphone peak %.1f dBFS, again %.1f dBFS (< %.1f wanted)", off, again, quietLimit);
        }
        else
            Check(true, L"paused: microphone peak %.1f dBFS < %.1f", off, quietLimit);

        bool resumed = Mp3Play(g_spk.id, folder, nullptr, 0);
        // the track Play starts with, read at once: a short track's rest may end within moments and the next one begin
        wchar_t first[MAX_PATH];
        wcsncpy(first, Mp3LastFile(), MAX_PATH - 1);
        first[MAX_PATH - 1] = 0;
        Check(resumed && (Mp3Playing() || _wcsicmp(Mp3LastFile(), first) != 0), L"playback continued");
        Check(_wcsicmp(first, track) == 0, L"continues the same track (%ls; paused: %ls)", first, track);
        // Another folder chosen meanwhile: Play starts a track of that one, not the paused one of the old folder.
        if (g_music.formatCount > 1 && _wcsicmp(folder, MP3_GENERATOR) != 0)
        {
            Mp3Pause();
            const TestMusicFolder& other = g_music.formats[Rand(0, g_music.formatCount - 1)];
            if (_wcsicmp(other.folder, folder) != 0 && Mp3Play(g_spk.id, other.folder, nullptr, 0))
            {
                const wchar_t* now = Mp3LastFile();
                size_t n = wcslen(other.folder);
                Check(_wcsnicmp(now, other.folder, n) == 0 && now[n] == L'\\',
                      L"another folder: a track of it plays (%ls), not the paused one", now);
                Mp3Pause();
            }
            Mp3Play(g_spk.id, folder, nullptr, 0);      // back for the level check below
        }
        float again = musicLevel();
        Check(again > -60.0f, L"music again after Play: peak %.1f dBFS > -60", again);
        Mp3Pause();
    }
    SetEndpointVolumeDb(g_mic.id, db);
    SetEndpointMute(g_mic.id, mute);
}

static void ActionOrphans()
{
    static wchar_t ids[128][80];
    int n = S2mFindOrphanEndpoints(ids, 128);
    Out(L"[orphans] leftover endpoint records: %d (at start: %d)", n, g_orphansAtStart);
    if (n < 0) return;                          // not judged right now (logged)
    if (n <= g_orphansKnown) Check(true, L"no new leftover endpoints");
    else CheckNewOrphans(L"orphans: found before this check");
}

// ---------------------------------------------------------------------------
// State snapshot / restore

struct Snapshot
{
    Quality q;
    bool    qPresent[5];
    wchar_t names[2][256];
    float   micDb;
    bool    micMute;
    DWORD   rate[2];
    WORD    bits[2], ch[2];
    wchar_t userName[2][256];   // HKCU\Software\Speak2Mic SpeakerName / MicName ("" = none)
    DWORD   userVol;            // MicVolumeCentiDb
    bool    userVolPresent;
};

static void TakeSnapshot(Snapshot* s)
{
    const wchar_t* keys[5] = { L"SampleRate", L"Channels", L"MicChannels", L"BitsPerSample", L"LatencyMs" };
    for (int i = 0; i < 5; i++) s->qPresent[i] = S2mGetParam(keys[i], 0xFFFFFFFF) != 0xFFFFFFFF;
    s->q = ReadQuality();
    wcscpy(s->names[0], g_spk.desc);
    wcscpy(s->names[1], g_mic.desc);
    float mn = 0, mx = 0;
    s->micDb = 0;
    GetEndpointVolumeDb(g_mic.id, &s->micDb, &mn, &mx);
    s->micMute = false;
    GetEndpointMute(g_mic.id, &s->micMute);
    GetEndpointFormat(g_spk.id, &s->rate[0], &s->bits[0], &s->ch[0]);
    GetEndpointFormat(g_mic.id, &s->rate[1], &s->bits[1], &s->ch[1]);
    const wchar_t* values[2] = { L"SpeakerName", L"MicName" };
    for (int i = 0; i < 2; i++)
    {
        DWORD size = sizeof(s->userName[i]);
        s->userName[i][0] = 0;
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Speak2Mic", values[i], RRF_RT_REG_SZ, nullptr, s->userName[i], &size);
    }
    DWORD size = sizeof(s->userVol);
    s->userVolPresent = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Speak2Mic", L"MicVolumeCentiDb", RRF_RT_REG_DWORD, nullptr,
                                     &s->userVol, &size) == ERROR_SUCCESS;
}

static void Restore(const Snapshot& s)
{
    Mp3Pause();                 // [music] interrupted (Ctrl+C)
    Out(L"[restore] driver settings, names, microphone volume/mute, default formats");
    Quality now = ReadQuality();
    bool restart = memcmp(&now, &s.q, sizeof(Quality)) != 0;
    if (restart)
    {
        Check(ApplyQuality(s.q), L"original settings applied");
        Check(WaitOurs(&g_spk, &g_mic, 20000), L"endpoints back");
    }
    else
    {
        FindOurs(&g_spk, &g_mic);
    }
    const wchar_t* keys[5] = { L"SampleRate", L"Channels", L"MicChannels", L"BitsPerSample", L"LatencyMs" };
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, S2M_PARAMS_KEY, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS)
    {
        for (int i = 0; i < 5; i++)
            if (!s.qPresent[i]) RegDeleteValueW(key, keys[i]);       // was a default before the test
        RegCloseKey(key);
    }
    wchar_t err[160];
    SetEndpointFormat(g_spk.id, s.rate[0], s.bits[0], s.ch[0], err, 160);
    SetEndpointFormat(g_mic.id, s.rate[1], s.bits[1], s.ch[1], err, 160);
    Check(SUCCEEDED(SetEndpointName(g_spk.id, s.names[0])), L"speaker name \"%ls\"", s.names[0]);
    Check(SUCCEEDED(SetEndpointName(g_mic.id, s.names[1])), L"microphone name \"%ls\"", s.names[1]);
    SetEndpointVolumeDb(g_mic.id, s.micDb);
    SetEndpointMute(g_mic.id, s.micMute);
    EndpointHold holds[2] = { { g_spk.id, true, s.rate[0], s.bits[0], s.ch[0], false, 0, false },
                              { g_mic.id, true, s.rate[1], s.bits[1], s.ch[1], true, s.micDb, s.micMute } };
    HoldEndpointSettings(holds, 2, restart ? 3000 : 500);
    HKEY user;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Speak2Mic", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &user, nullptr) == ERROR_SUCCESS)
    {
        const wchar_t* values[2] = { L"SpeakerName", L"MicName" };
        for (int i = 0; i < 2; i++)
        {
            if (s.userName[i][0])
                RegSetValueExW(user, values[i], 0, REG_SZ, (const BYTE*)s.userName[i], (DWORD)((wcslen(s.userName[i]) + 1) * sizeof(wchar_t)));
            else
                RegDeleteValueW(user, values[i]);
        }
        if (s.userVolPresent) RegSetValueExW(user, L"MicVolumeCentiDb", 0, REG_DWORD, (const BYTE*)&s.userVol, sizeof(DWORD));
        else RegDeleteValueW(user, L"MicVolumeCentiDb");
        RegCloseKey(user);
    }
}

// ---------------------------------------------------------------------------

int wmain(int argc, wchar_t** argv)
{
    InitializeCriticalSection(&g_eventLock);
    _setmode(_fileno(stdout), _O_U16TEXT);
    AppLogOpen(L"autotest");
    SetConsoleTitleW(L"Speak2Mic autotest");
    int minutes = 10;
    unsigned seed = (unsigned)GetTickCount() ^ (unsigned)GetCurrentProcessId() * 2654435761u;
    for (int i = 1; i < argc; i++)
    {
        if (_wcsicmp(argv[i], L"--seed") == 0 && i + 1 < argc) seed = (unsigned)wcstoul(argv[++i], nullptr, 10);
        else if (_wtoi(argv[i]) > 0) minutes = _wtoi(argv[i]);
        else
        {
            Out(L"Usage: s2mautotest [minutes] [--seed N]");
            return 2;
        }
    }
    g_rng = 0x9E3779B97F4A7C15ULL ^ ((unsigned long long)seed << 1 | 1);
    SetConsoleCtrlHandler(CtrlHandler, TRUE);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    S2mNotReady notReady = S2mCheckReady();
    if (notReady != S2mReadyOk)
    {
        Out(L"%ls", S2mNotReadyTextEn(notReady));
        DWORD procs[4];
        if (GetConsoleProcessList(procs, 4) <= 1)
        {
            Out(L"Press Enter to exit.");
            getwchar();
        }
        CoUninitialize();
        return 3;
    }
    Out(L"Speak2Mic autotest: %d minute(s), seed %u (repeat with --seed %u). Ctrl+C stops and restores.", minutes, seed, seed);
    Out(L"Log: %ls", AppLogPath());
    {
        // The programs and the driver are updated separately (Setup updates both): log what actually runs.
        wchar_t running[64] = L"", packaged[64] = L"";
        bool haveRunning = DiagRunningDriverVersion(running, 64);
        bool havePackage = DiagPackageDriverVersion(packaged, 64);
        Out(L"Build: programs %ls, driver running %ls, driver package next to this program %ls", L"" S2M_VER_STR,
            haveRunning ? running : L"(unknown)", havePackage ? packaged : L"(none)");
    }

    // The control panel would react to the changes (names, volume): close it.
    HWND panel = FindWindowW(L"S2mPanel", nullptr);
    if (panel)
    {
        Out(L"Closing the Speak2Mic control panel.");
        PostMessageW(panel, WM_CLOSE, 0, 0);
        Sleep(1500);
    }

    if (!FindOurs(&g_spk, &g_mic))
    {
        Out(L"FAIL Speak2Mic Speaker / Microphone not found: install Speak2Mic first.");
        CoUninitialize();
        return 1;
    }
    Snapshot snap;
    TakeSnapshot(&snap);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (!TestMusicCreate(&g_music, MusicLogLine)) Warn(L"no test music could be made: [music] uses the program's mp3 folder");
    {
        static wchar_t ids[128][80];
        g_orphansAtStart = S2mFindOrphanEndpoints(ids, 128);
        if (g_orphansAtStart < 0) g_orphansAtStart = 0;
        g_orphansKnown = g_orphansAtStart;
    }
    Out(L"Start: \"%ls\" / \"%ls\", %lu Hz %lu bit, speaker %lu ch, microphone %lu ch, latency %lu ms, leftover endpoints %d",
        snap.names[0], snap.names[1], snap.q.rate, snap.q.bits, snap.q.channels, snap.q.micChannels, snap.q.latency,
        g_orphansAtStart);
    LogEndpointEffects(L"speaker", g_spk.id);
    LogEndpointEffects(L"microphone", g_mic.id);
    {
        // The driver's volume node (-96..+9.56 dB) must be the microphone's endpoint volume, not Windows' software
        // volume (-96..+30 dB).
        float db = 0, mn = 0, mx = 0;
        HRESULT hr = S_OK;
        bool read = GetEndpointVolumeDb(g_mic.id, &db, &mn, &mx, &hr);
        Check(read && hr == S_OK && mx > 9.0f && mx < 10.0f, L"microphone volume range %.2f..%.2f dB (driver volume node: -96..+9.56)",
              mn, mx);
    }

    // Weighted random actions.
    struct { void (*fn)(); int weight; const wchar_t* name; } actions[] = {
        { ActionSignal, 25, L"signal" }, { ActionApply, 12, L"apply" }, { ActionRename, 15, L"rename" },
        { ActionVolume, 15, L"volume" }, { ActionFormat, 12, L"format" }, { ActionStress, 10, L"stress" },
        { ActionTestSound, 6, L"testsound" }, { ActionOrphans, 5, L"orphans" }, { ActionCli, 20, L"cli" },
        { ActionMusic, 6, L"music" },
    };
    int total = 0;
    for (auto& a : actions) total += a.weight;
    DWORD end = GetTickCount() + (DWORD)minutes * 60000;
    int iteration = 0;
    while (!g_stop && (LONG)(end - GetTickCount()) > 0)
    {
        if (!FindOurs(&g_spk, &g_mic) && !WaitOurs(&g_spk, &g_mic, 20000) && !RecoverEndpoints(L"the last action"))
        {
            Check(false, L"Speak2Mic endpoints disappeared (also after a recovery restart)");
            break;
        }
        WatchMic();
        int pick = Rand(1, total);
        iteration++;
        wprintf(L"\n");
        AppLog(L"");
        Out(L"#%d (%d s left)", iteration, (int)((LONG)(end - GetTickCount()) / 1000));
        for (auto& a : actions)
        {
            if (pick <= a.weight)
            {
                a.fn();
                CheckNewOrphans(a.name);
                break;
            }
            pick -= a.weight;
        }
    }
    Out(L"");
    Mp3Pause();
    TestMusicDelete(&g_music);
    Out(L"Test music removed.");
    Restore(snap);
    Out(L"");
    Out(L"==== %d action(s): %d passed, %d FAILED, %d warning(s); seed %u ====", iteration, g_pass, g_fail, g_warn, seed);
    Out(L"Log: %ls", AppLogPath());
    CoUninitialize();

    DWORD procs[4];
    if (GetConsoleProcessList(procs, 4) <= 1)
    {
        Out(L"Press Enter to exit.");
        getwchar();
    }
    return g_fail ? 1 : 0;
}
