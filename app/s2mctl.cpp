// s2mctl - Speak2Mic settings from the command line (everything the control panel does).
//
//   s2mctl status
//   s2mctl set [--preset voice|standard|high|studio|max] [--rate HZ] [--bits 0|16|24|32] [--channels N]
//              [--mic-channels N] [--latency MS]            (admin; restarts the device like "Apply")
//   s2mctl name [--speaker "NAME"] [--mic "NAME"]          ("" or "default" = the default name)
//   s2mctl volume PERCENT                                  (microphone, 0..300; 100 = unchanged signal)
//   s2mctl mute on|off
//   s2mctl reset                                            (admin; preset "Standard", default names, 100 %, sound on)
//   s2mctl export FILE.ini   /   s2mctl import FILE.ini    (import: admin; same file format as the panel)
//   s2mctl test [SECONDS]                                   (the panel's "Test": Windows' melody on every channel)
//
// Exit codes: 0 ok, 1 failed, 2 bad arguments, 3 administrator rights needed, 4 cannot work (Secure Boot on, test
// signing mode off or the driver not installed: every command refuses). Output and log in English
// (%ProgramData%\Speak2Mic\logs\ctl.log).
#include "audio.h"
#include "applog.h"
#include "audiosvc.h"
#include "devctl.h"
#include "../driver/version.h"
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>
#include <math.h>
#include <io.h>
#include <fcntl.h>

#define S2M_USER_KEY L"Software\\Speak2Mic"

static const wchar_t kDefaultName[2][32] = { L"Speak2Mic Speaker", L"Speak2Mic Microphone" };
static const DWORD kRates[] = { 8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000, 176400, 192000 };
static const DWORD kChannels[] = { 1, 2, 4, 6, 8 };

static void Out(const wchar_t* fmt, ...)
{
    wchar_t buf[1024];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf(buf, 1024, fmt, args);
    va_end(args);
    buf[1023] = 0;
    wprintf(L"%ls\n", buf);
    AppLog(L"%ls", buf);
}

static bool IsElevated()
{
    HANDLE token = nullptr;
    TOKEN_ELEVATION el = {};
    DWORD size = 0;
    bool elevated = false;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    {
        if (GetTokenInformation(token, TokenElevation, &el, sizeof(el), &size)) elevated = el.TokenIsElevated != 0;
        CloseHandle(token);
    }
    return elevated;
}

static bool InList(DWORD v, const DWORD* list, int n)
{
    for (int i = 0; i < n; i++)
        if (list[i] == v) return true;
    return false;
}

// ---------------------------------------------------------------------------
// State

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

static bool NeedDevices(AudioDevice* spk, AudioDevice* mic)
{
    if (FindOurs(spk, mic)) return true;
    Out(L"error: the Speak2Mic Speaker / Microphone devices were not found (is Speak2Mic installed?)");
    return false;
}

// The panel remembers names and the microphone volume (it restores them after a reinstall).
static void SaveUserString(const wchar_t* name, const wchar_t* value)
{
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, S2M_USER_KEY, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    if (!value) RegDeleteValueW(key, name);
    else RegSetValueExW(key, name, 0, REG_SZ, (const BYTE*)value, (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}

static void SaveMicVolume(float db)
{
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, S2M_USER_KEY, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    DWORD centi = (DWORD)(LONG)floorf(db * 100.0f + 0.5f);
    RegSetValueExW(key, L"MicVolumeCentiDb", 0, REG_DWORD, (const BYTE*)&centi, sizeof(centi));
    RegCloseKey(key);
}

static int DbToPercent(float db)
{
    // The top step of the driver's volume node (+9.5625 dB on its 1/16 dB grid) is exactly 300 % (gain x3.0).
    if (db > 9.53f && db < 9.60f) return 300;
    int pct = (int)(100.0f * powf(10.0f, db / 20.0f) + 0.5f);
    return pct < 0 ? 0 : pct;
}

// ---------------------------------------------------------------------------
// Operations

// Registry + device restart (Windows Audio stopped meanwhile) + default formats: the panel's "Apply".
static int ApplyQuality(const Quality& q)
{
    if (!IsElevated())
    {
        Out(L"error: administrator rights are needed (run the command prompt as administrator)");
        return 3;
    }
    S2mSetParam(L"SampleRate", q.rate);
    S2mSetParam(L"Channels", q.channels);
    S2mSetParam(L"MicChannels", q.micChannels);
    S2mSetParam(L"BitsPerSample", q.bits);
    S2mSetParam(L"LatencyMs", q.latency);
    AppLog(L"apply: %lu Hz, %lu bit, speaker %lu ch, microphone %lu ch, latency %lu ms", q.rate, q.bits, q.channels,
           q.micChannels, q.latency);
    // The microphone's volume and mute right now (kept over the restart, see HoldEndpointSettings).
    AudioDevice spk, mic;
    float levelDb = 0, mn = 0, mx = 0;
    bool levelMute = false;
    bool level = FindOurs(&spk, &mic) && GetEndpointVolumeDb(mic.id, &levelDb, &mn, &mx) && GetEndpointMute(mic.id, &levelMute);
    AudioServices svc;
    AudioStopServices(false, &svc);
    bool found = false, reboot = false;
    bool ok = S2mRestartDevice(&found, &reboot);
    AudioStartServices(&svc);
    if (!found)
    {
        Out(L"settings saved; the Speak2Mic device was not found (they apply once it is installed)");
        return 1;
    }
    if (!ok || reboot)
    {
        Out(L"settings saved, but the device could not be restarted: restart the computer");
        return 1;
    }
    DWORD t0 = GetTickCount();
    bool back = false;
    while (!(back = FindOurs(&spk, &mic)) && GetTickCount() - t0 < 20000) Sleep(250);
    // Leftover records (the first restart after an installation leaves the installation's pair); removing them
    // restarts the endpoint builder, so look for the endpoints again.
    if (back && S2mCleanupOrphanEndpoints(5000) > 0)
    {
        t0 = GetTickCount();
        while (!(back = FindOurs(&spk, &mic)) && GetTickCount() - t0 < 20000) Sleep(250);
    }
    if (!back)
    {
        Out(L"error: the Speak2Mic devices did not come back after the restart");
        return 1;
    }
    const AudioDevice* d[2] = { &spk, &mic };
    WORD bits = (WORD)(q.bits ? q.bits : 24);
    EndpointHold holds[2] = {};
    for (int side = 0; side < 2; side++)
    {
        DWORD ch = side == 0 || !q.micChannels ? q.channels : q.micChannels;
        wchar_t err[160];
        WORD used = bits;
        bool fmtOk = SetEndpointFormat(d[side]->id, q.rate, bits, (WORD)ch, err, 160);
        if (!fmtOk && bits > 16)
        {
            fmtOk = SetEndpointFormat(d[side]->id, q.rate, 16, (WORD)ch, err, 160);
            used = 16;
            if (fmtOk) Out(L"%ls: Windows did not accept %u bit, the default format is 16 bit", side ? L"microphone" : L"speaker", bits);
        }
        if (!fmtOk) Out(L"warning: %ls default format not set (%ls)", side ? L"microphone" : L"speaker", err);
        holds[side] = { d[side]->id, fmtOk, q.rate, used, (WORD)ch, side == 1 && level, levelDb, levelMute };
    }
    HoldEndpointSettings(holds, 2, 3000);
    Out(L"applied: %lu Hz, %lu bit, speaker %lu ch, microphone %lu ch%ls, latency %lu ms", q.rate, q.bits, q.channels,
        q.micChannels ? q.micChannels : q.channels, q.micChannels ? L"" : L" (as the speaker)", q.latency);
    return 0;
}

static int CmdStatus()
{
    Quality q = ReadQuality();
    Out(L"Speak2Mic %ls", L"" S2M_VER_STR);
    bool installed = S2mGetParam(L"SampleRate", 0xFFFFFFFF) != 0xFFFFFFFF || S2mGetParam(L"StartStatus", 0xFFFFFFFF) != 0xFFFFFFFF;
    Out(L"settings: %lu Hz, %lu bit, speaker %lu ch, microphone %lu ch%ls, latency %lu ms%ls", q.rate, q.bits, q.channels,
        q.micChannels ? q.micChannels : q.channels, q.micChannels ? L"" : L" (as the speaker)", q.latency,
        installed ? L"" : L" (defaults: driver not installed?)");
    AudioDevice spk, mic;
    if (!NeedDevices(&spk, &mic)) return 1;
    const AudioDevice* d[2] = { &spk, &mic };
    for (int side = 0; side < 2; side++)
    {
        DWORD r = 0;
        WORD b = 0, c = 0;
        GetEndpointFormat(d[side]->id, &r, &b, &c);
        Out(L"%ls: \"%ls\", default format %lu Hz %u bit %u ch", side ? L"microphone" : L"speaker", d[side]->desc, r, b, c);
    }
    float db = 0, mn = 0, mx = 0;
    bool mute = false;
    GetEndpointVolumeDb(mic.id, &db, &mn, &mx);
    GetEndpointMute(mic.id, &mute);
    Out(L"microphone volume: %d %% (%.1f dB), sound %ls", DbToPercent(db), db, mute ? L"off (muted)" : L"on");
    return 0;
}

static bool ParseDword(const wchar_t* s, DWORD* v)
{
    wchar_t* end = nullptr;
    unsigned long x = wcstoul(s, &end, 10);
    if (!end || *end || end == s) return false;
    *v = x;
    return true;
}

static int CmdSet(int argc, wchar_t** argv)
{
    Quality q = ReadQuality();
    for (int i = 2; i < argc; i += 2)     // every option takes one value
    {
        const wchar_t* a = argv[i];
        const wchar_t* v = i + 1 < argc ? argv[i + 1] : nullptr;
        DWORD x = 0;
        if (_wcsicmp(a, L"--preset") == 0 && v)
        {
            struct { const wchar_t* name; DWORD rate, bits, channels; } presets[] = {
                { L"voice", 16000, 16, 1 }, { L"standard", 48000, 16, 2 }, { L"high", 48000, 24, 2 },
                { L"studio", 96000, 24, 2 }, { L"max", 192000, 32, 2 } };
            bool found = false;
            for (auto& p : presets)
                if (_wcsicmp(v, p.name) == 0)
                {
                    q.rate = p.rate; q.bits = p.bits; q.channels = p.channels; q.micChannels = 1;     // like the panel
                    found = true;
                }
            if (!found) { Out(L"error: unknown preset \"%ls\" (voice, standard, high, studio, max)", v); return 2; }
        }
        else if (_wcsicmp(a, L"--rate") == 0 && v && ParseDword(v, &x) && InList(x, kRates, 11)) q.rate = x;
        else if (_wcsicmp(a, L"--bits") == 0 && v && ParseDword(v, &x) && (x == 0 || x == 16 || x == 24 || x == 32)) q.bits = x;
        else if (_wcsicmp(a, L"--channels") == 0 && v && ParseDword(v, &x) && InList(x, kChannels, 5)) q.channels = x;
        else if (_wcsicmp(a, L"--mic-channels") == 0 && v && ParseDword(v, &x) && (x == 0 || InList(x, kChannels, 5))) q.micChannels = x;
        else if (_wcsicmp(a, L"--latency") == 0 && v && ParseDword(v, &x) && x >= 10 && x <= 500) q.latency = x;
        else
        {
            Out(L"error: bad option or value: %ls %ls", a, v ? v : L"");
            Out(L"  --rate 8000|11025|16000|22050|32000|44100|48000|88200|96000|176400|192000, --bits 0|16|24|32,");
            Out(L"  --channels 1|2|4|6|8, --mic-channels 0|1|2|4|6|8 (0 = as the speaker), --latency 10..500");
            return 2;
        }
    }
    return ApplyQuality(q);
}

static int CmdName(int argc, wchar_t** argv)
{
    const wchar_t* names[2] = { nullptr, nullptr };
    for (int i = 2; i + 1 < argc; i += 2)
    {
        if (_wcsicmp(argv[i], L"--speaker") == 0) names[0] = argv[i + 1];
        else if (_wcsicmp(argv[i], L"--mic") == 0) names[1] = argv[i + 1];
        else { Out(L"error: use --speaker \"NAME\" and/or --mic \"NAME\""); return 2; }
    }
    if ((argc - 2) % 2 || (!names[0] && !names[1])) { Out(L"error: use --speaker \"NAME\" and/or --mic \"NAME\""); return 2; }
    AudioDevice spk, mic;
    if (!NeedDevices(&spk, &mic)) return 1;
    const AudioDevice* d[2] = { &spk, &mic };
    int rc = 0;
    for (int side = 0; side < 2; side++)
    {
        if (!names[side]) continue;
        wchar_t name[64];
        const wchar_t* src = names[side];
        if (!*src || _wcsicmp(src, L"default") == 0) src = kDefaultName[side];
        if (wcslen(src) > 60) { Out(L"error: a name may have at most 60 characters"); return 2; }
        wcsncpy(name, src, 63);
        name[63] = 0;
        HRESULT hr = SetEndpointName(d[side]->id, name);
        if (SUCCEEDED(hr))
        {
            Out(L"%ls renamed: \"%ls\"", side ? L"microphone" : L"speaker", name);
            SaveUserString(side ? L"MicName" : L"SpeakerName", wcscmp(name, kDefaultName[side]) == 0 ? nullptr : name);
        }
        else
        {
            Out(L"error: %ls not renamed (0x%08lX; try an administrator command prompt)", side ? L"microphone" : L"speaker",
                (unsigned long)hr);
            rc = 1;
        }
    }
    return rc;
}

static int CmdVolume(int argc, wchar_t** argv)
{
    DWORD pct = 0;
    if (argc != 3 || !ParseDword(argv[2], &pct) || pct > 300) { Out(L"error: s2mctl volume 0..300"); return 2; }
    AudioDevice spk, mic;
    if (!NeedDevices(&spk, &mic)) return 1;
    float db = pct > 0 ? 20.0f * log10f(pct / 100.0f) : -200.0f;       // -200: the endpoint's minimum
    if (!SetEndpointVolumeDb(mic.id, db)) { Out(L"error: the volume could not be set"); return 1; }
    float now = db, mn = -96.0f, mx = 30.0f;
    if (!GetEndpointVolumeDb(mic.id, &now, &mn, &mx))
    {
        now = db < mn ? mn : (db > mx ? mx : db);          // not readable right now: remember what was asked
        AppLog(L"volume: read back failed, saving the requested %.2f dB", now);
    }
    SaveMicVolume(now);
    Out(L"microphone volume: %d %% (%.1f dB)", DbToPercent(now), now);
    return 0;
}

static int CmdMute(int argc, wchar_t** argv)
{
    if (argc != 3 || (_wcsicmp(argv[2], L"on") != 0 && _wcsicmp(argv[2], L"off") != 0)) { Out(L"error: s2mctl mute on|off"); return 2; }
    AudioDevice spk, mic;
    if (!NeedDevices(&spk, &mic)) return 1;
    bool mute = _wcsicmp(argv[2], L"on") == 0;
    if (!SetEndpointMute(mic.id, mute)) { Out(L"error: mute could not be set"); return 1; }
    Out(L"microphone sound %ls", mute ? L"off (muted)" : L"on");
    return 0;
}

static int CmdReset()
{
    if (!IsElevated()) { Out(L"error: administrator rights are needed"); return 3; }
    AudioDevice spk, mic;
    if (FindOurs(&spk, &mic))
    {
        SetEndpointName(spk.id, kDefaultName[0]);
        SetEndpointName(mic.id, kDefaultName[1]);
        SetEndpointVolumeDb(mic.id, 0.0f);
        SetEndpointMute(mic.id, false);
    }
    SaveUserString(L"SpeakerName", nullptr);
    SaveUserString(L"MicName", nullptr);
    SaveMicVolume(0.0f);
    Quality q = { 48000, 2, 1, 16, 30 };
    Quality now = ReadQuality();
    Out(L"reset: preset \"Standard\", default names, microphone 100 %%, sound on");
    if (memcmp(&q, &now, sizeof(q)) == 0) return 0;
    return ApplyQuality(q);
}

static const wchar_t kIniSection[] = L"Speak2Mic";

static int CmdExport(int argc, wchar_t** argv)
{
    if (argc != 3) { Out(L"error: s2mctl export FILE.ini"); return 2; }
    wchar_t path[MAX_PATH];
    if (!GetFullPathNameW(argv[2], MAX_PATH, path, nullptr)) return 2;
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { Out(L"error: cannot write %ls (%lu)", path, GetLastError()); return 1; }
    const WORD bom = 0xFEFF;
    DWORD written = 0;
    WriteFile(f, &bom, sizeof(bom), &written, nullptr);
    CloseHandle(f);
    Quality q = ReadQuality();
    wchar_t v[64];
    auto num = [&](const wchar_t* key, long value) {
        _snwprintf(v, 64, L"%ld", value);
        WritePrivateProfileStringW(kIniSection, key, v, path);
    };
    WritePrivateProfileStringW(kIniSection, L"Version", L"" S2M_VER_STR, path);
    num(L"SampleRate", (long)q.rate);
    num(L"Channels", (long)q.channels);
    num(L"MicChannels", (long)q.micChannels);
    num(L"BitsPerSample", (long)q.bits);
    num(L"LatencyMs", (long)q.latency);
    AudioDevice spk, mic;
    if (FindOurs(&spk, &mic))
    {
        WritePrivateProfileStringW(kIniSection, L"SpeakerName", spk.desc[0] ? spk.desc : kDefaultName[0], path);
        WritePrivateProfileStringW(kIniSection, L"MicName", mic.desc[0] ? mic.desc : kDefaultName[1], path);
        float db = 0, mn = 0, mx = 0;
        bool mute = false;
        if (GetEndpointVolumeDb(mic.id, &db, &mn, &mx)) num(L"MicVolumePercent", DbToPercent(db));
        if (GetEndpointMute(mic.id, &mute)) num(L"MicMute", mute ? 1 : 0);
    }
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path);
    Out(L"settings saved to %ls", path);
    return 0;
}

static int CmdImport(int argc, wchar_t** argv)
{
    if (argc != 3) { Out(L"error: s2mctl import FILE.ini"); return 2; }
    wchar_t path[MAX_PATH];
    if (!GetFullPathNameW(argv[2], MAX_PATH, path, nullptr) || GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
    {
        Out(L"error: file not found: %ls", argv[2]);
        return 1;
    }
    const DWORD none = 0xFFFFFFFF;
    auto num = [&](const wchar_t* key) { return (DWORD)GetPrivateProfileIntW(kIniSection, key, (INT)none, path); };
    wchar_t names[2][256];
    GetPrivateProfileStringW(kIniSection, L"SpeakerName", L"", names[0], 256, path);
    GetPrivateProfileStringW(kIniSection, L"MicName", L"", names[1], 256, path);
    DWORD rate = num(L"SampleRate"), channels = num(L"Channels"), micChannels = num(L"MicChannels"), bits = num(L"BitsPerSample"),
          latency = num(L"LatencyMs"), volume = num(L"MicVolumePercent"), mute = num(L"MicMute");
    if (rate == none && channels == none && bits == none && latency == none && micChannels == none && !names[0][0] &&
        !names[1][0] && volume == none && mute == none)
    {
        Out(L"error: the file does not look like Speak2Mic settings");
        return 1;
    }
    Quality q = ReadQuality();
    if (InList(rate, kRates, 11)) q.rate = rate;
    if (InList(channels, kChannels, 5)) q.channels = channels;
    if (micChannels == 0 || InList(micChannels, kChannels, 5)) q.micChannels = micChannels;
    if (bits == 0 || bits == 16 || bits == 24 || bits == 32) q.bits = bits;
    if (latency != none && latency >= 10 && latency <= 500) q.latency = latency;
    Quality now = ReadQuality();
    int rc = 0;
    if (memcmp(&q, &now, sizeof(q)) != 0) rc = ApplyQuality(q);        // needs admin (3) like "Apply"
    if (rc == 3) return rc;
    AudioDevice spk, mic;
    if (FindOurs(&spk, &mic))
    {
        const AudioDevice* d[2] = { &spk, &mic };
        for (int side = 0; side < 2; side++)
        {
            if (!names[side][0] || wcslen(names[side]) > 60 || wcscmp(names[side], d[side]->desc) == 0) continue;
            if (SUCCEEDED(SetEndpointName(d[side]->id, names[side])))
                SaveUserString(side ? L"MicName" : L"SpeakerName", wcscmp(names[side], kDefaultName[side]) == 0 ? nullptr : names[side]);
            else rc = 1;
        }
        if (volume != none)
        {
            if (volume > 300) volume = 300;     // the panel's range
            float db = volume > 0 ? 20.0f * log10f(volume / 100.0f) : -200.0f;
            SetEndpointVolumeDb(mic.id, db);
            float nowDb = db, mn = -96.0f, mx = 30.0f;
            if (!GetEndpointVolumeDb(mic.id, &nowDb, &mn, &mx)) nowDb = db < mn ? mn : db;   // not readable: as asked
            SaveMicVolume(nowDb);
        }
        if (mute == 0 || mute == 1) SetEndpointMute(mic.id, mute == 1);
    }
    Out(L"settings loaded from %ls", path);
    return rc;
}

struct CtlTestCtx { wchar_t id[256]; volatile LONG stop; HRESULT hr; };

// WINAPI: a thread function must be stdcall on x86 (a lambda is not).
static DWORD WINAPI CtlTestCtxThread(LPVOID p)
{
    CtlTestCtx* c = (CtlTestCtx*)p;
    c->hr = PlayChannelTest(c->id, &c->stop);
    return 0;
}

static int CmdTest(int argc, wchar_t** argv)
{
    DWORD seconds = 0;
    if (argc > 3 || (argc == 3 && (!ParseDword(argv[2], &seconds) || seconds == 0))) { Out(L"error: s2mctl test [SECONDS]"); return 2; }
    AudioDevice spk, mic;
    if (!NeedDevices(&spk, &mic)) return 1;
    CtlTestCtx ctx = {};
    wcsncpy(ctx.id, spk.id, 255);
    HANDLE t = CreateThread(nullptr, 0, CtlTestCtxThread, &ctx, 0, nullptr);
    if (!t) return 1;
    Out(L"test sound on \"%ls\"%ls", spk.desc, seconds ? L"" : L" (all channels)");
    if (seconds && WaitForSingleObject(t, seconds * 1000) == WAIT_TIMEOUT) InterlockedExchange(&ctx.stop, 1);
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    if (FAILED(ctx.hr)) { Out(L"error: the test sound failed (0x%08lX)", (unsigned long)ctx.hr); return 1; }
    return 0;
}

static void Usage()
{
    Out(L"Speak2Mic %ls - settings from the command line", L"" S2M_VER_STR);
    Out(L"  s2mctl status");
    Out(L"  s2mctl set [--preset voice|standard|high|studio|max] [--rate HZ] [--bits 0|16|24|32]");
    Out(L"             [--channels N] [--mic-channels N] [--latency MS]         (administrator)");
    Out(L"  s2mctl name [--speaker \"NAME\"] [--mic \"NAME\"]                    (\"default\" = default name)");
    Out(L"  s2mctl volume 0..300                                              (microphone, 100 = unchanged)");
    Out(L"  s2mctl mute on|off");
    Out(L"  s2mctl reset                                                      (administrator)");
    Out(L"  s2mctl export FILE.ini | import FILE.ini                          (import: administrator)");
    Out(L"  s2mctl test [SECONDS]");
    Out(L"Exit codes: 0 ok, 1 failed, 2 bad arguments, 3 administrator rights needed,");
    Out(L"            4 cannot work (Secure Boot on, test signing mode off or driver not installed).");
}

int wmain(int argc, wchar_t** argv)
{
    _setmode(_fileno(stdout), _O_U16TEXT);
    AppLogOpen(L"ctl");
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int rc = 2;
    const wchar_t* cmd = argc >= 2 ? argv[1] : L"";
    wchar_t line[1024] = L"";
    for (int i = 1; i < argc; i++)
    {
        wcsncat(line, L" ", 1023 - wcslen(line));
        wcsncat(line, argv[i], 1023 - wcslen(line));
    }
    AppLog(L"command:%ls", line);
    S2mNotReady notReady = argc >= 2 ? S2mCheckReady() : S2mReadyOk;      // no arguments: only the usage text
    if (notReady != S2mReadyOk)
    {
        Out(L"error: %ls", S2mNotReadyTextEn(notReady));
        rc = 4;
    }
    else if (_wcsicmp(cmd, L"status") == 0 && argc == 2) rc = CmdStatus();
    else if (_wcsicmp(cmd, L"set") == 0 && argc > 2) rc = CmdSet(argc, argv);
    else if (_wcsicmp(cmd, L"name") == 0) rc = CmdName(argc, argv);
    else if (_wcsicmp(cmd, L"volume") == 0) rc = CmdVolume(argc, argv);
    else if (_wcsicmp(cmd, L"mute") == 0) rc = CmdMute(argc, argv);
    else if (_wcsicmp(cmd, L"reset") == 0 && argc == 2) rc = CmdReset();
    else if (_wcsicmp(cmd, L"export") == 0) rc = CmdExport(argc, argv);
    else if (_wcsicmp(cmd, L"import") == 0) rc = CmdImport(argc, argv);
    else if (_wcsicmp(cmd, L"test") == 0) rc = CmdTest(argc, argv);
    else Usage();
    AppLog(L"exit code %d", rc);
    CoUninitialize();
    return rc;
}
