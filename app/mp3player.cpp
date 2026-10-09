// See mp3player.h.
#include "mp3player.h"
#include "applog.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <stdlib.h>
#include <bcrypt.h>
#include <wchar.h>
#include <stdio.h>
#include <math.h>
#define STB_VORBIS_HEADER_ONLY
#include "third_party/stb_vorbis.c"     // OGG Vorbis (Windows has no decoder for it); compiled in stbvorbis.c

static bool IsGeneratorPath(const wchar_t* path) { return wcsncmp(path, MP3_GENERATOR, wcslen(MP3_GENERATOR)) == 0; }

static HANDLE        g_thread;
static volatile LONG g_stop;
static wchar_t       g_device[256], g_folder[MAX_PATH];
static HWND          g_notify;
static UINT          g_msg, g_trackMsg;
// What Pause interrupted (Play continues it).
static wchar_t       g_resumeFile[MAX_PATH];
static LONGLONG      g_resumePos;           // 100-ns units
static wchar_t       g_lastFile[MAX_PATH];
static wchar_t       g_bad[32][MAX_PATH];   // files that could not be played (skipped for a minute)
static int           g_badCount;
static DWORD         g_badReset;
// The first track, chosen by Mp3Play (so the caller can name it), and its start position.
static wchar_t       g_firstFile[MAX_PATH];
static LONGLONG      g_firstPos;

// A random index 0..n-1 from the system generator (rand() has one state per thread and was never seeded on the
// playback thread: every start of the panel played the same file first).
static int RandomIndex(int n)
{
    ULONG r = 0;
    if (BCryptGenRandom(nullptr, (PUCHAR)&r, sizeof(r), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        r = GetTickCount() ^ (GetCurrentProcessId() << 16);
    return (int)(r % (ULONG)n);
}

static const GUID kSubtypeFloat = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

// Music files the player takes: Media Foundation decodes mp3, wav and flac; OGG Vorbis goes to stb_vorbis.
static bool IsMusicFile(const wchar_t* name)
{
    const wchar_t* dot = wcsrchr(name, L'.');
    if (!dot) return false;
    static const wchar_t* kExt[] = { L".mp3", L".wav", L".flac", L".ogg", L".oga" };
    for (const wchar_t* e : kExt)
        if (_wcsicmp(dot, e) == 0) return true;
    return false;
}

static bool IsOgg(const wchar_t* path)
{
    const wchar_t* dot = wcsrchr(path, L'.');
    return dot && (_wcsicmp(dot, L".ogg") == 0 || _wcsicmp(dot, L".oga") == 0);
}

// The music files of the folder (names only), up to max.
static int ListMp3(const wchar_t* folder, wchar_t (*names)[MAX_PATH], int max)
{
    wchar_t pattern[MAX_PATH];
    _snwprintf(pattern, MAX_PATH, L"%ls\\*", folder);
    pattern[MAX_PATH - 1] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && IsMusicFile(fd.cFileName) && n < max)
            wcsncpy(names[n++], fd.cFileName, MAX_PATH - 1);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

void Mp3DefaultFolder(wchar_t* folder)
{
    wchar_t exeDir[MAX_PATH];
    GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
    wchar_t* slash = wcsrchr(exeDir, L'\\');
    if (slash) *slash = 0;
    _snwprintf(folder, MAX_PATH, L"%ls\\mp3", exeDir);
    folder[MAX_PATH - 1] = 0;
    if (GetFileAttributesW(folder) != INVALID_FILE_ATTRIBUTES) return;
    wchar_t up[MAX_PATH];
    _snwprintf(up, MAX_PATH, L"%ls\\..\\mp3", exeDir);
    up[MAX_PATH - 1] = 0;
    if (GetFileAttributesW(up) != INVALID_FILE_ATTRIBUTES && GetFullPathNameW(up, MAX_PATH, folder, nullptr)) return;
}

bool Mp3FolderHasFiles(const wchar_t* folder)
{
    if (IsGeneratorPath(folder)) return true;
    static wchar_t one[1][MAX_PATH];
    return ListMp3(folder, one, 1) > 0;
}

const wchar_t* Mp3ResumeFile()
{
    return g_resumeFile;
}

const wchar_t* Mp3LastFile()
{
    return g_lastFile;
}

bool Mp3Playing()
{
    return g_thread && WaitForSingleObject(g_thread, 0) == WAIT_TIMEOUT;
}

// The next file: the paused one if it is still there, else a random one (not the one just played, if there is a choice).
static bool PickFile(wchar_t* path, LONGLONG* pos)
{
    if (IsGeneratorPath(g_folder))
    {
        // the generator: the paused track again, else a new one (its seed is the "file name")
        *pos = 0;
        if (g_resumeFile[0] && IsGeneratorPath(g_resumeFile))
        {
            wcscpy(path, g_resumeFile);
            *pos = g_resumePos;
        }
        else
            _snwprintf(path, MAX_PATH, L"%ls\\%d", MP3_GENERATOR, 1 + RandomIndex(999999));
        g_resumeFile[0] = 0;
        return true;
    }
    static wchar_t names[512][MAX_PATH];
    int n = ListMp3(g_folder, names, 512);
    *pos = 0;
    // The paused track continues only when it belongs to the folder chosen now (another folder: a track of that one).
    const wchar_t* slash = g_resumeFile[0] ? wcsrchr(g_resumeFile, L'\\') : nullptr;
    size_t dirLen = slash ? (size_t)(slash - g_resumeFile) : 0;
    size_t folderLen = wcslen(g_folder);
    while (folderLen && g_folder[folderLen - 1] == L'\\') folderLen--;
    bool sameFolder = dirLen && dirLen == folderLen && _wcsnicmp(g_resumeFile, g_folder, dirLen) == 0;
    if (g_resumeFile[0] && !sameFolder) AppLog(L"mp3: paused track %ls not in %ls: not continued", g_resumeFile, g_folder);
    if (g_resumeFile[0] && sameFolder && GetFileAttributesW(g_resumeFile) != INVALID_FILE_ATTRIBUTES)
    {
        wcscpy(path, g_resumeFile);
        *pos = g_resumePos;
        g_resumeFile[0] = 0;
        return true;
    }
    g_resumeFile[0] = 0;
    if (n == 0) return false;
    // The files that can be played (not remembered as broken), the last one played only when it is the only one.
    // Random among them.
    DWORD now = GetTickCount();
    if ((int)(now - g_badReset) >= 0)
    {
        g_badCount = 0;                         // files may have been replaced: tried again after a minute
        g_badReset = now + 60000;
    }
    static int ok[512];
    int good = 0, last = -1;
    for (int i = 0; i < n; i++)
    {
        wchar_t candidate[MAX_PATH];
        _snwprintf(candidate, MAX_PATH, L"%ls\\%ls", g_folder, names[i]);
        candidate[MAX_PATH - 1] = 0;
        bool bad = false;
        for (int b = 0; b < g_badCount && !bad; b++) bad = _wcsicmp(g_bad[b], candidate) == 0;
        if (bad) continue;
        if (_wcsicmp(candidate, g_lastFile) == 0) last = i;
        else ok[good++] = i;
    }
    if (!good && last < 0) return false;        // every file is one that cannot be played
    int pick = good ? ok[RandomIndex(good)] : last;
    _snwprintf(path, MAX_PATH, L"%ls\\%ls", g_folder, names[pick]);
    path[MAX_PATH - 1] = 0;
    return true;
}

static void Store(BYTE* p, float v, int bytes, bool isFloat)
{
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    if (isFloat && bytes == 4) { *(float*)p = v; return; }
    LONG s = (LONG)(v * 2147483647.0);
    if (bytes == 2) *(SHORT*)p = (SHORT)(s >> 16);
    else if (bytes == 3) { p[0] = (BYTE)(s >> 8); p[1] = (BYTE)(s >> 16); p[2] = (BYTE)(s >> 24); }
    else *(LONG*)p = s;
}

// ---------------------------------------------------------------------------
// The generator ("folder" MP3_GENERATOR): endless pleasant music made up on the fly, as "tracks" of 45..90 s, each
// with its own key, mode and tempo - a soft pad chord per bar, a bass, a bell-like pentatonic melody (no wrong notes)
// and a stereo echo; fade in / out. A track is its seed ("::generator\<seed>"): Pause / Play continues it exactly.

struct Synth
{
    ULONG    seed;
    double   rate;
    LONGLONG n, total;                 // sample position / track length
    LONGLONG stepLen, nextStep;        // half a beat
    int      step;
    int      root, minor;              // MIDI note of the key, minor mode
    int      chordDegree, melodyIdx;
    double   pad[3], padPhase[3], bass, bassPhase;
    struct Note { double freq, phase, amp, decay, pan; bool on; } notes[12];
    float*   delay;                    // stereo echo line
    int      delayLen, delayPos;
};

static ULONG SynthRand(Synth* s)
{
    s->seed ^= s->seed << 13;
    s->seed ^= s->seed >> 17;
    s->seed ^= s->seed << 5;
    return s->seed;
}

static double MidiFreq(double note) { return 440.0 * pow(2.0, (note - 69) / 12.0); }

// Scale degree (any integer, 7 per octave) of the key -> MIDI note
static int ScaleNote(const Synth* s, int degree)
{
    static const int major[7] = { 0, 2, 4, 5, 7, 9, 11 }, minor[7] = { 0, 2, 3, 5, 7, 8, 10 };
    int oct = degree >= 0 ? degree / 7 : -((-degree + 6) / 7);
    int d = degree - oct * 7;
    return s->root + oct * 12 + (s->minor ? minor[d] : major[d]);
}

// Pentatonic step (5 per octave) -> MIDI note (major / minor pentatonic of the key)
static int PentaNote(const Synth* s, int idx)
{
    static const int major[5] = { 0, 2, 4, 7, 9 }, minor[5] = { 0, 3, 5, 7, 10 };
    int oct = idx >= 0 ? idx / 5 : -((-idx + 4) / 5);
    int d = idx - oct * 5;
    return s->root + 12 + oct * 12 + (s->minor ? minor[d] : major[d]);
}

static void SynthChord(Synth* s)
{
    // a pleasant progression: I - V - vi - IV (major) / i - VI - III - VII (minor), then random steps among them
    static const int progMajor[4] = { 0, 4, 5, 3 }, progMinor[4] = { 0, 5, 2, 6 };
    int bar = s->step / 8;
    int deg = (bar < 4 ? (s->minor ? progMinor : progMajor)[bar % 4]
                       : (s->minor ? progMinor : progMajor)[SynthRand(s) % 4]);
    s->chordDegree = deg;
    for (int i = 0; i < 3; i++) s->pad[i] = MidiFreq(ScaleNote(s, deg + 2 * i) - 12) * (1.0 + (i - 1) * 0.0015);
    s->bass = MidiFreq(ScaleNote(s, deg) - 24);
}

static bool SynthOpen(Synth* s, ULONG seed, double rate)
{
    ZeroMemory(s, sizeof(*s));
    s->seed = seed ? seed : 1;
    s->rate = rate;
    s->root = 57 + (int)(SynthRand(s) % 12);                        // A3..G#4
    s->minor = (int)(SynthRand(s) % 2);
    double bpm = 66 + (SynthRand(s) % 30);
    s->stepLen = (LONGLONG)(rate * 30.0 / bpm);                      // half a beat
    s->total = (LONGLONG)(rate * (45 + SynthRand(s) % 46));
    s->delayLen = (int)(s->stepLen * 3 / 2);                          // dotted-quarter echo
    s->delay = (float*)calloc((size_t)s->delayLen * 2, sizeof(float));
    s->melodyIdx = 5;
    SynthChord(s);
    return s->delay != nullptr;
}

static void SynthClose(Synth* s)
{
    free(s->delay);
    s->delay = nullptr;
}

// One stereo frame.
static void SynthFrame(Synth* s, float* lr)
{
    const double twoPi = 6.283185307179586;
    if (s->n >= s->nextStep)
    {
        if (s->step % 8 == 0 && s->step) SynthChord(s);
        // melody: a note on most half beats, a small step on the pentatonic scale
        if (SynthRand(s) % 100 < 62)
        {
            int move = (int)(SynthRand(s) % 5) - 2;
            s->melodyIdx += move;
            if (s->melodyIdx < 0) s->melodyIdx = 1;
            if (s->melodyIdx > 11) s->melodyIdx = 9;
            for (auto& nt : s->notes)
                if (!nt.on)
                {
                    nt.on = true;
                    nt.freq = MidiFreq(PentaNote(s, s->melodyIdx));
                    nt.phase = 0;
                    nt.amp = 0.16 + (SynthRand(s) % 60) / 1000.0;
                    nt.decay = exp(-1.0 / (s->rate * (0.6 + (SynthRand(s) % 100) / 100.0)));
                    nt.pan = ((int)(SynthRand(s) % 120) - 60) / 100.0;
                    break;
                }
        }
        s->step++;
        s->nextStep += s->stepLen;
    }
    // fade in / out of the track (2 s)
    double t = (double)s->n, fade = 1.0, edge = 2.0 * s->rate;
    if (t < edge) fade = t / edge;
    if (t > s->total - edge) fade = (s->total - t) / edge;
    if (fade < 0) fade = 0;
    double l = 0, r = 0;
    // pad: three soft voices, slowly breathing
    double breathe = 0.75 + 0.25 * sin(twoPi * t / (s->rate * 6));
    for (int i = 0; i < 3; i++)
    {
        s->padPhase[i] += s->pad[i] / s->rate;
        if (s->padPhase[i] > 1) s->padPhase[i] -= 1;
        double v = sin(twoPi * s->padPhase[i]) * 0.7 + sin(2 * twoPi * s->padPhase[i]) * 0.15;
        l += v * 0.055 * breathe * (i == 0 ? 1.0 : 0.8);
        r += v * 0.055 * breathe * (i == 2 ? 1.0 : 0.8);
    }
    // bass
    s->bassPhase += s->bass / s->rate;
    if (s->bassPhase > 1) s->bassPhase -= 1;
    double b = sin(twoPi * s->bassPhase) * 0.10;
    l += b;
    r += b;
    // melody bells
    for (auto& nt : s->notes)
    {
        if (!nt.on) continue;
        nt.phase += nt.freq / s->rate;
        if (nt.phase > 1) nt.phase -= 1;
        double v = (sin(twoPi * nt.phase) + 0.3 * sin(2 * twoPi * nt.phase) + 0.1 * sin(3 * twoPi * nt.phase)) * nt.amp;
        nt.amp *= nt.decay;
        if (nt.amp < 0.0005) nt.on = false;
        l += v * (1 - nt.pan) * 0.5;
        r += v * (1 + nt.pan) * 0.5;
    }
    // ping-pong echo
    float* d = s->delay + (size_t)s->delayPos * 2;
    double dl = d[0], dr = d[1];
    d[0] = (float)(l * 0.6 + dr * 0.35);
    d[1] = (float)(r * 0.6 + dl * 0.35);
    if (++s->delayPos >= s->delayLen) s->delayPos = 0;
    l = (l + dl * 0.45) * fade;
    r = (r + dr * 0.45) * fade;
    lr[0] = (float)tanh(l * 1.4) * 0.8f;
    lr[1] = (float)tanh(r * 1.4) * 0.8f;
    s->n++;
}

// One track being decoded to float samples: Media Foundation (mp3, wav, flac), stb_vorbis (ogg) or the generator.
struct Decoder
{
    IMFSourceReader* mf;
    stb_vorbis*      ogg;
    Synth*           syn;
    UINT32           ch, rate;
    LONGLONG         pos;           // position of the data returned last, 100-ns units
    float*           tmp;           // decoded frames handed out by DecRead
    size_t           tmpCap;        // floats
};

static HRESULT DecOpen(Decoder* d, const wchar_t* path, LONGLONG pos)
{
    ZeroMemory(d, sizeof(*d));
    d->pos = pos;
    if (IsGeneratorPath(path))
    {
        const wchar_t* sl = wcsrchr(path, L'\\');
        ULONG seed = sl ? (ULONG)wcstoul(sl + 1, nullptr, 10) : 1;
        d->syn = (Synth*)calloc(1, sizeof(Synth));
        if (!d->syn || !SynthOpen(d->syn, seed * 2654435761u + 12345, 48000)) return E_OUTOFMEMORY;
        d->ch = 2;
        d->rate = 48000;
        // continue a paused track exactly: run the generator silently up to the position
        LONGLONG skip = pos * 48000 / 10000000;
        float lr[2];
        for (LONGLONG i = 0; i < skip && d->syn->n < d->syn->total; i++) SynthFrame(d->syn, lr);
        return S_OK;
    }
    if (IsOgg(path))
    {
        FILE* f = _wfopen(path, L"rb");
        int err = 0;
        d->ogg = f ? stb_vorbis_open_file(f, 1, &err, nullptr) : nullptr;     // closes f when freed
        if (!d->ogg)
        {
            if (f) fclose(f);
            return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
        }
        stb_vorbis_info info = stb_vorbis_get_info(d->ogg);
        d->ch = (UINT32)info.channels;
        d->rate = info.sample_rate;
        if (pos > 0) stb_vorbis_seek(d->ogg, (unsigned)(pos * d->rate / 10000000));
        return d->ch && d->rate ? S_OK : E_UNEXPECTED;
    }
    IMFMediaType* type = nullptr;
    HRESULT fr = MFCreateSourceReaderFromURL(path, nullptr, &d->mf);
    if (SUCCEEDED(fr)) fr = d->mf->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (SUCCEEDED(fr)) fr = d->mf->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    if (SUCCEEDED(fr)) fr = MFCreateMediaType(&type);
    if (SUCCEEDED(fr)) fr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED(fr)) fr = type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    if (SUCCEEDED(fr)) fr = d->mf->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, type);
    if (type) { type->Release(); type = nullptr; }
    if (SUCCEEDED(fr)) fr = d->mf->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &type);
    if (SUCCEEDED(fr)) fr = type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &d->ch);
    if (SUCCEEDED(fr)) fr = type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &d->rate);
    if (type) type->Release();
    if (SUCCEEDED(fr) && (d->ch == 0 || d->rate == 0)) fr = E_UNEXPECTED;
    if (SUCCEEDED(fr) && pos > 0)
    {
        PROPVARIANT var;
        PropVariantInit(&var);
        var.vt = VT_I8;
        var.hVal.QuadPart = pos;
        d->mf->SetCurrentPosition(GUID_NULL, var);
    }
    return fr;
}

static bool DecGrow(Decoder* d, size_t floats)
{
    if (floats <= d->tmpCap) return true;
    float* bigger = (float*)realloc(d->tmp, floats * sizeof(float));
    if (!bigger) return false;
    d->tmp = bigger;
    d->tmpCap = floats;
    return true;
}

// The next decoded frames (interleaved, d->ch channels): *frames = 0 with S_OK = end of the track.
static HRESULT DecRead(Decoder* d, const float** data, size_t* frames)
{
    *frames = 0;
    if (d->syn)
    {
        const size_t chunk = 2400;                          // 50 ms
        if (!DecGrow(d, chunk * 2)) return E_OUTOFMEMORY;
        d->pos = d->syn->n * 10000000 / 48000;
        size_t n = 0;
        while (n < chunk && d->syn->n < d->syn->total) SynthFrame(d->syn, d->tmp + n++ * 2);
        *data = d->tmp;
        *frames = n;                                        // 0: the end of this "track"
        return S_OK;
    }
    if (d->ogg)
    {
        const int chunk = 4096;
        if (!DecGrow(d, (size_t)chunk * d->ch)) return E_OUTOFMEMORY;
        d->pos = (LONGLONG)stb_vorbis_get_sample_offset(d->ogg) * 10000000 / d->rate;
        int n = stb_vorbis_get_samples_float_interleaved(d->ogg, (int)d->ch, d->tmp, chunk * (int)d->ch);
        *data = d->tmp;
        *frames = n > 0 ? (size_t)n : 0;
        return S_OK;
    }
    for (;;)
    {
        DWORD flags = 0;
        LONGLONG ts = 0;
        IMFSample* sample = nullptr;
        if (FAILED(d->mf->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, &ts, &sample)) ||
            (flags & MF_SOURCE_READERF_ENDOFSTREAM))
        {
            if (sample) sample->Release();
            return S_OK;                                    // end (or unreadable from here on)
        }
        if (!sample) continue;
        d->pos = ts;
        IMFMediaBuffer* buf = nullptr;
        BYTE* bytes = nullptr;
        DWORD len = 0;
        HRESULT hr = S_OK;
        if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf)) && SUCCEEDED(buf->Lock(&bytes, nullptr, &len)))
        {
            size_t n = len / (sizeof(float) * d->ch);
            if (DecGrow(d, n * d->ch)) memcpy(d->tmp, bytes, n * d->ch * sizeof(float)), *frames = n;
            else hr = E_OUTOFMEMORY;
            buf->Unlock();
        }
        if (buf) buf->Release();
        sample->Release();
        if (FAILED(hr) || *frames) { *data = d->tmp; return hr; }
    }
}

static void DecClose(Decoder* d)
{
    if (d->mf) d->mf->Release();
    if (d->ogg) stb_vorbis_close(d->ogg);
    if (d->syn)
    {
        SynthClose(d->syn);
        free(d->syn);
    }
    free(d->tmp);
    ZeroMemory(d, sizeof(*d));
}

static DWORD WINAPI PlayThread(LPVOID)
{
    bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    bool mf = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    WAVEFORMATEX* fmt = nullptr;
    UINT32 bufferFrames = 0;
    HRESULT hr = mf ? CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en)
                    : E_FAIL;
    if (SUCCEEDED(hr)) hr = en->GetDevice(g_device, &dev);
    if (SUCCEEDED(hr)) hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client);
    if (SUCCEEDED(hr)) hr = client->GetMixFormat(&fmt);
    if (SUCCEEDED(hr)) hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 2000000 /* 200 ms */, 0, fmt, nullptr);
    if (SUCCEEDED(hr)) hr = client->GetBufferSize(&bufferFrames);
    if (SUCCEEDED(hr)) hr = client->GetService(__uuidof(IAudioRenderClient), (void**)&render);
    if (SUCCEEDED(hr)) hr = client->Start();

    const int outCh = fmt ? fmt->nChannels : 2, outBytes = fmt ? fmt->wBitsPerSample / 8 : 4;
    const bool outFloat = fmt && (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
        (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && IsEqualGUID(((WAVEFORMATEXTENSIBLE*)fmt)->SubFormat, kSubtypeFloat)));
    const double outRate = fmt ? fmt->nSamplesPerSec : 48000;

    float* src = nullptr;                   // decoded frames waiting to be resampled (srcCh interleaved)
    size_t srcFrames = 0, srcCap = 0;
    HRESULT result = hr;
    while (SUCCEEDED(hr) && !g_stop)
    {
        wchar_t path[MAX_PATH];
        LONGLONG pos = 0;
        if (g_firstFile[0])
        {
            wcscpy(path, g_firstFile);      // chosen by Mp3Play
            pos = g_firstPos;
            g_firstFile[0] = 0;
        }
        else if (PickFile(path, &pos))
        {
            // the next track: tell the window (lParam = file name, malloc'ed: the receiver frees it)
            if (g_notify && g_trackMsg)
            {
                const wchar_t* name = wcsrchr(path, L'\\');
                wchar_t* copy = _wcsdup(name ? name + 1 : path);
                if (copy && !PostMessageW(g_notify, g_trackMsg, 0, (LPARAM)copy)) free(copy);
            }
        }
        else
        {
            result = S_FALSE;               // the folder has no .mp3 files (any more)
            break;
        }
        Decoder dec;
        HRESULT fr = DecOpen(&dec, path, pos);
        UINT32 srcCh = dec.ch, srcRate = dec.rate;
        if (FAILED(fr))
        {
            AppLog(L"mp3: cannot play %ls (0x%08lX), skipped for a minute", path, (unsigned long)fr);
            DecClose(&dec);
            if (g_badCount < 32) wcscpy(g_bad[g_badCount++], path);
            if (g_badCount == 1) g_badReset = GetTickCount() + 60000;
            continue;                           // the next file at once (no gap of silence)
        }
        AppLog(L"mp3: playing %ls%ls (%u Hz, %u ch)", path, pos > 0 ? L" (continued)" : L"", srcRate, srcCh);
        wcscpy(g_lastFile, path);

        const double step = srcRate / outRate;      // source frames per output frame
        double t = 0;                               // position in src (frames)
        srcFrames = 0;
        LONGLONG trackPos = pos;
        bool eof = false;
        while (!g_stop && SUCCEEDED(hr))
        {
            // Enough source for the next output frames?
            while (!eof && (size_t)t + 2 >= srcFrames)
            {
                const float* data = nullptr;
                size_t frames = 0;
                if (FAILED(hr = DecRead(&dec, &data, &frames))) break;
                if (!frames)
                {
                    eof = true;
                    break;
                }
                trackPos = dec.pos;
                {
                    // Drop what was consumed, append the new frames.
                    size_t keep = (size_t)t < srcFrames ? srcFrames - (size_t)t : 0;
                    if (keep && (size_t)t) memmove(src, src + (size_t)t * srcCh, keep * srcCh * sizeof(float));
                    t -= (double)(size_t)t;
                    srcFrames = keep;
                    if (srcFrames + frames > srcCap)
                    {
                        srcCap = (srcFrames + frames) * 2;
                        float* bigger = (float*)realloc(src, srcCap * srcCh * sizeof(float));
                        if (!bigger) { hr = E_OUTOFMEMORY; break; }
                        src = bigger;
                    }
                    memcpy(src + srcFrames * srcCh, data, frames * srcCh * sizeof(float));
                    srcFrames += frames;
                }
            }
            if (eof && (size_t)t + 1 >= srcFrames) break;       // track finished

            UINT32 padding = 0;
            client->GetCurrentPadding(&padding);
            UINT32 room = bufferFrames - padding;
            if (room == 0) { Sleep(10); continue; }
            BYTE* out = nullptr;
            if (FAILED(hr = render->GetBuffer(room, &out))) break;
            UINT32 written = 0;
            for (; written < room && (size_t)t + 1 < srcFrames; written++, t += step)
            {
                size_t i = (size_t)t;
                float frac = (float)(t - i);
                const float* a = src + i * srcCh;
                const float* b = a + srcCh;
                BYTE* o = out + (size_t)written * fmt->nBlockAlign;
                for (int c = 0; c < outCh; c++)
                {
                    // mono: front left + right; more channels than the file: silent
                    int sc = srcCh == 1 ? (c < 2 ? 0 : -1) : (c < (int)srcCh ? c : -1);
                    float v = sc < 0 ? 0.0f : a[sc] + (b[sc] - a[sc]) * frac;
                    Store(o + c * outBytes, v, outBytes, outFloat);
                }
            }
            render->ReleaseBuffer(written, 0);
            if (written < room && eof) continue;
            if (written == room) Sleep(10);
        }
        if (g_stop)
        {
            wcscpy(g_resumeFile, path);         // Play continues here
            g_resumePos = trackPos;
        }
        DecClose(&dec);
    }
    if (FAILED(hr)) result = hr;
    if (client) client->Stop();
    free(src);
    if (render) render->Release();
    if (client) client->Release();
    if (fmt) CoTaskMemFree(fmt);
    if (dev) dev->Release();
    if (en) en->Release();
    if (mf) MFShutdown();
    if (com) CoUninitialize();
    AppLog(L"mp3: playback ended (0x%08lX)%ls", (unsigned long)result, g_stop ? L", paused" : L"");
    if (!g_stop && g_notify) PostMessageW(g_notify, g_msg, (WPARAM)result, 0);
    return 0;
}

bool Mp3Play(const wchar_t* deviceId, const wchar_t* folder, HWND hwnd, UINT msg, UINT trackMsg)
{
    Mp3Pause();
    if (!Mp3FolderHasFiles(folder)) return false;
    wcsncpy(g_device, deviceId, 255);
    g_device[255] = 0;
    wcsncpy(g_folder, folder, MAX_PATH - 1);
    g_folder[MAX_PATH - 1] = 0;
    g_notify = hwnd;
    g_msg = msg;
    g_trackMsg = trackMsg;
    if (!PickFile(g_firstFile, &g_firstPos)) return false;
    wcscpy(g_lastFile, g_firstFile);        // Mp3LastFile() names it right away
    g_stop = 0;
    g_thread = CreateThread(nullptr, 0, PlayThread, nullptr, 0, nullptr);
    return g_thread != nullptr;
}

void Mp3Pause()
{
    if (!g_thread) return;
    InterlockedExchange(&g_stop, 1);
    WaitForSingleObject(g_thread, 5000);
    CloseHandle(g_thread);
    g_thread = nullptr;
}
