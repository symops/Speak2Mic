// Test music of s2mautotest (see atmusic.h).
#include "atmusic.h"
#include "applog.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <shellapi.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

static const UINT32 kRate = 44100, kChannels = 2;
static const double kSeconds = 8.0;

static void LogF(MusicLog log, const wchar_t* fmt, ...)
{
    wchar_t buf[600];
    va_list a;
    va_start(a, fmt);
    _vsnwprintf(buf, 600, fmt, a);
    va_end(a);
    buf[599] = 0;
    if (log) log(buf);
    else AppLog(L"%ls", buf);
}

// The melody: notes of 0.25 s with soft edges over a quiet pad, never silent (the level checks look for sound).
static SHORT* MakeMelody(UINT32* frames)
{
    static const double kNotes[] = { 523.25, 587.33, 659.25, 783.99, 880.00, 783.99, 659.25, 587.33 };
    *frames = (UINT32)(kSeconds * kRate);
    SHORT* pcm = (SHORT*)malloc((size_t)*frames * kChannels * sizeof(SHORT));
    if (!pcm) return nullptr;
    const double tau = 2.0 * 3.14159265358979;
    UINT32 noteLen = kRate / 4;
    for (UINT32 i = 0; i < *frames; i++)
    {
        double t = (double)i / kRate;
        UINT32 n = i / noteLen, k = i % noteLen;
        double env = k < 400 ? k / 400.0 : (k > noteLen - 400 ? (noteLen - k) / 400.0 : 1.0);
        double v = 0.30 * env * sin(tau * kNotes[n % 8] * t) + 0.10 * sin(tau * 130.81 * t) + 0.06 * sin(tau * 196.0 * t);
        // a short fade at the ends
        double edge = t < 0.05 ? t / 0.05 : (kSeconds - t < 0.05 ? (kSeconds - t) / 0.05 : 1.0);
        SHORT s = (SHORT)(v * edge * 32767.0);
        pcm[(size_t)i * 2] = s;
        pcm[(size_t)i * 2 + 1] = (SHORT)(s * 0.8);
    }
    return pcm;
}

static bool WriteWav(const wchar_t* path, const SHORT* pcm, UINT32 frames)
{
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD data = frames * kChannels * 2, n = 0;
    struct { char riff[4]; DWORD size; char wave[4]; char fmt[4]; DWORD fmtSize; WORD tag, ch; DWORD rate, bytes; WORD align, bits;
             char dataId[4]; DWORD dataSize; } h = {
        { 'R', 'I', 'F', 'F' }, 36 + data, { 'W', 'A', 'V', 'E' }, { 'f', 'm', 't', ' ' }, 16, 1, (WORD)kChannels, kRate,
        kRate * kChannels * 2, (WORD)(kChannels * 2), 16, { 'd', 'a', 't', 'a' }, data };
    bool ok = WriteFile(f, &h, sizeof(h), &n, nullptr) && n == sizeof(h) && WriteFile(f, pcm, data, &n, nullptr) && n == data;
    CloseHandle(f);
    if (!ok) DeleteFileW(path);
    return ok;
}

// MP3 / FLAC by the Media Foundation sink writer (the container follows the extension).
static HRESULT WriteEncoded(const wchar_t* path, const GUID& subtype, const SHORT* pcm, UINT32 frames)
{
    IMFSinkWriter* w = nullptr;
    HRESULT hr = MFCreateSinkWriterFromURL(path, nullptr, nullptr, &w);
    if (FAILED(hr)) return hr;
    DWORD stream = 0;
    IMFMediaType* t = nullptr;
    hr = MFCreateMediaType(&t);
    if (SUCCEEDED(hr)) hr = t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED(hr)) hr = t->SetGUID(MF_MT_SUBTYPE, subtype);
    if (SUCCEEDED(hr)) hr = t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate);
    if (SUCCEEDED(hr)) hr = t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels);
    if (SUCCEEDED(hr) && IsEqualGUID(subtype, MFAudioFormat_MP3)) hr = t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 16000);
    if (SUCCEEDED(hr) && IsEqualGUID(subtype, MFAudioFormat_FLAC)) hr = t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (SUCCEEDED(hr)) hr = w->AddStream(t, &stream);
    if (t) t->Release();
    t = nullptr;
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&t);
    if (SUCCEEDED(hr)) hr = t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED(hr)) hr = t->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    if (SUCCEEDED(hr)) hr = t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (SUCCEEDED(hr)) hr = t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate);
    if (SUCCEEDED(hr)) hr = t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels);
    if (SUCCEEDED(hr)) hr = t->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, kChannels * 2);
    if (SUCCEEDED(hr)) hr = t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kRate * kChannels * 2);
    if (SUCCEEDED(hr)) hr = w->SetInputMediaType(stream, t, nullptr);
    if (t) t->Release();
    if (SUCCEEDED(hr)) hr = w->BeginWriting();
    const UINT32 chunk = kRate / 10;
    for (UINT32 pos = 0; pos < frames && SUCCEEDED(hr); pos += chunk)
    {
        UINT32 count = frames - pos < chunk ? frames - pos : chunk;
        IMFMediaBuffer* b = nullptr;
        IMFSample* s = nullptr;
        hr = MFCreateMemoryBuffer(count * kChannels * 2, &b);
        BYTE* p = nullptr;
        if (SUCCEEDED(hr)) hr = b->Lock(&p, nullptr, nullptr);
        if (SUCCEEDED(hr))
        {
            memcpy(p, pcm + (size_t)pos * kChannels, (size_t)count * kChannels * 2);
            b->Unlock();
            b->SetCurrentLength(count * kChannels * 2);
        }
        if (SUCCEEDED(hr)) hr = MFCreateSample(&s);
        if (SUCCEEDED(hr)) hr = s->AddBuffer(b);
        if (SUCCEEDED(hr)) hr = s->SetSampleTime((LONGLONG)pos * 10000000 / kRate);
        if (SUCCEEDED(hr)) hr = s->SetSampleDuration((LONGLONG)count * 10000000 / kRate);
        if (SUCCEEDED(hr)) hr = w->WriteSample(stream, s);
        if (s) s->Release();
        if (b) b->Release();
    }
    if (SUCCEEDED(hr)) hr = w->Finalize();
    w->Release();
    if (FAILED(hr)) DeleteFileW(path);
    return hr;
}

static void Join(wchar_t* out, const wchar_t* a, const wchar_t* b)
{
    _snwprintf(out, MAX_PATH, L"%ls\\%ls", a, b);
    out[MAX_PATH - 1] = 0;
}

static void WriteJunk(const wchar_t* path, const char* text, DWORD size)
{
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD n = 0;
    if (text) WriteFile(f, text, (DWORD)strlen(text), &n, nullptr);
    else
    {
        BYTE junk[8192];
        for (DWORD i = 0; i < sizeof(junk); i++) junk[i] = (BYTE)(i * 73 + 5);
        WriteFile(f, junk, size < sizeof(junk) ? size : sizeof(junk), &n, nullptr);
    }
    CloseHandle(f);
}

// One RCDATA resource into a file (an empty one is written as an empty file).
static bool WriteResource(int id, const wchar_t* path)
{
    HRSRC fr = FindResourceW(nullptr, MAKEINTRESOURCEW(id), (LPCWSTR)RT_RCDATA);
    if (!fr) return false;
    HGLOBAL fg = LoadResource(nullptr, fr);
    const void* data = fg ? LockResource(fg) : nullptr;
    DWORD bytes = SizeofResource(nullptr, fr);
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD n = 0;
    bool ok = f != INVALID_HANDLE_VALUE && (!bytes || (data && WriteFile(f, data, bytes, &n, nullptr) && n == bytes));
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    return ok;
}

// Broken samples are not carried in the program (nor so in its installer): made here on every run, a new random choice and
// new random bytes each time.
static unsigned BrokenRand(unsigned lo, unsigned hi)
{
    static unsigned s;
    if (!s)
    {
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        s = (q.LowPart ^ GetCurrentProcessId() * 2654435761u) | 1;
    }
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return lo + s % (hi - lo + 1);
}

static bool ReadWholeFile(const wchar_t* path, BYTE** data, DWORD* size)
{
    *data = nullptr;
    *size = 0;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD n = GetFileSize(f, nullptr), got = 0;
    BYTE* p = n != INVALID_FILE_SIZE ? (BYTE*)malloc(n ? n : 1) : nullptr;
    bool ok = p && ReadFile(f, p, n, &got, nullptr) && got == n;
    CloseHandle(f);
    if (!ok) { free(p); return false; }
    *data = p;
    *size = n;
    return true;
}

// `head` `repeat` times, then `junk` random bytes.
static bool WriteBroken(const wchar_t* path, const void* head, DWORD headLen, unsigned repeat, DWORD junk)
{
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    DWORD n = 0;
    for (unsigned i = 0; i < repeat && headLen && ok; i++) ok = WriteFile(f, head, headLen, &n, nullptr) && n == headLen;
    BYTE* b = junk ? (BYTE*)malloc(junk) : nullptr;
    if (b)
    {
        for (DWORD i = 0; i < junk; i++) b[i] = (BYTE)BrokenRand(0, 255);
        ok = ok && WriteFile(f, b, junk, &n, nullptr) && n == junk;
        free(b);
    }
    CloseHandle(f);
    return ok;
}

// Which of n entries are used: at least `least`, in a random selection.
static void BrokenPick(bool* use, int n, int least)
{
    int want = (int)BrokenRand((unsigned)least, (unsigned)n);
    for (int i = 0; i < n; i++) use[i] = false;
    for (int have = 0; have < want;)
    {
        int k = (int)BrokenRand(0, (unsigned)n - 1);
        if (!use[k]) { use[k] = true; have++; }
    }
}

// Next to one good file: empty, text named as music, random bytes, a header followed by junk, a WAV whose header claims far
// more data than there is, files cut short (MP3 / FLAC: the start plays, then the next file). `mp3` / `flac`: good samples.
static int MakeBrokenSamples(const wchar_t* folder, const wchar_t* mp3Path, const wchar_t* flacPath, MusicLog log)
{
    BYTE* mp3 = nullptr;
    BYTE* flac = nullptr;
    DWORD mp3Len = 0, flacLen = 0;
    ReadWholeFile(mp3Path, &mp3, &mp3Len);
    ReadWholeFile(flacPath, &flac, &flacLen);
    bool use[9];
    BrokenPick(use, 9, 5);
    int made = 0;
    for (int i = 0; i < 9; i++)
    {
        if (!use[i]) continue;
        wchar_t name[64], path[MAX_PATH];
        const void* head = nullptr;
        DWORD len = 0, junk = 0;
        unsigned repeat = 1;
        static const char text[] = "not music, just text\r\n";
        static const BYTE id3[20] = { 'I', 'D', '3', 3, 0, 0, 0, 0, 0, 10 };
        BYTE* wav = nullptr;
        switch (i)
        {
        case 0: wcscpy(name, L"broken-empty.mp3"); break;
        case 1: wcscpy(name, L"broken-text.wav"); head = text; len = sizeof(text) - 1; repeat = 40; break;
        case 2: wcscpy(name, L"broken-random.ogg"); junk = BrokenRand(2000, 20000); break;
        case 3: wcscpy(name, L"broken-ogg-header-junk.ogg"); head = "OggS"; len = 4; junk = BrokenRand(2000, 20000); break;
        case 4: wcscpy(name, L"broken-flac-header-junk.flac"); head = "fLaC"; len = 4; junk = BrokenRand(2000, 20000); break;
        case 5: wcscpy(name, L"broken-id3-only.mp3"); head = id3; len = sizeof(id3); break;
        case 6:
        {
            // 0.5 s of a tone, the header claiming 5-50 times as much
            const DWORD frames = 22050, bytes = frames * 4, claimed = bytes * BrokenRand(5, 50);
            wcscpy(name, L"broken-wav-lying-size.wav");
            wav = (BYTE*)malloc(44 + bytes);
            if (!wav) continue;
            struct { char riff[4]; DWORD size; char wave[8]; DWORD fmtLen; WORD tag, ch; DWORD rate, bps; WORD align, bits; char data[4]; DWORD dataLen; } h =
                { { 'R', 'I', 'F', 'F' }, 36 + claimed, { 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ' }, 16, 1, 2, 44100, 176400, 4, 16, { 'd', 'a', 't', 'a' }, claimed };
            memcpy(wav, &h, 44);
            short* s = (short*)(wav + 44);
            for (DWORD f = 0; f < frames; f++)
            {
                s[f * 2] = (short)(8000 * sin(f / 7.0));
                s[f * 2 + 1] = 0;
            }
            head = wav;
            len = 44 + bytes;
            break;
        }
        case 7:
        case 8:
        {
            int cut = (int)BrokenRand(10, 60);
            _snwprintf(name, 64, i == 7 ? L"broken-cut-%d.mp3" : L"broken-cut-%d.flac", cut);
            name[63] = 0;
            head = i == 7 ? mp3 : flac;
            len = (DWORD)((ULONGLONG)(i == 7 ? mp3Len : flacLen) * cut / 100);
            if (!len) continue;                                // its good source is not there
            break;
        }
        }
        Join(path, folder, name);
        if (WriteBroken(path, head, len, repeat, junk)) made++;
        else LogF(log, L"  broken sample %ls not written", name);
        free(wav);
    }
    free(mp3);
    free(flac);
    return made;
}

// The samples built in as resources (RCDATA 500 = manifest "music|file|-", 501.. = the files in its order).
static int AddSamples(TestMusic* m, MusicLog log)
{
    wchar_t brokenFolder[MAX_PATH] = L"";
    int brokenCount = 0;
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(500), (LPCWSTR)RT_RCDATA);
    HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
    const char* text = g ? (const char*)LockResource(g) : nullptr;
    DWORD size = r ? SizeofResource(nullptr, r) : 0;
    int added = 0;
    const char* p = text;
    const char* end = text ? text + size : nullptr;
    for (int id = 501; p && p < end && m->formatCount < 24; id++)
    {
        const char* eol = p;
        while (eol < end && *eol != '\n') eol++;
        char line[256];
        size_t len = (size_t)(eol - p) < sizeof(line) - 1 ? (size_t)(eol - p) : sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = 0;
        p = eol + 1;
        if (len && line[len - 1] == '\r') line[--len] = 0;
        char* f1 = strchr(line, '|');
        char* f2 = f1 ? strchr(f1 + 1, '|') : nullptr;
        if (!f1 || !f2)
        {
            id--;
            continue;
        }
        *f2 = 0;
        *f1 = 0;
        wchar_t file[128], sub[160], path[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, f1 + 1, -1, file, 128);
        _snwprintf(sub, 160, L"sample-%ls", file);
        for (wchar_t* q = sub; *q; q++)
            if (*q == L'.') *q = L'-';
        TestMusicFolder& tf = m->formats[m->formatCount];
        Join(tf.folder, m->root, sub);
        CreateDirectoryW(tf.folder, nullptr);
        Join(path, tf.folder, file);
        if (!WriteResource(id, path))
        {
            LogF(log, L"  sample %ls not written", file);
            continue;
        }
        _snwprintf(tf.format, 64, L"%ls (sample)", file);
        m->formatCount++;
        added++;
        wchar_t copy[MAX_PATH], name[140];
        _snwprintf(name, 140, L"s-%ls", file);
        Join(copy, m->all, name);
        CopyFileW(path, copy, FALSE);
    }
    {
        wchar_t sampleMp3[MAX_PATH], sampleFlac[MAX_PATH];
        Join(sampleMp3, m->root, L"sample-melody-vbr-id3-mp3\\melody-vbr-id3.mp3");
        Join(sampleFlac, m->root, L"sample-melody-24bit-96k-flac\\melody-24bit-96k.flac");
        Join(brokenFolder, m->root, L"music-broken");
        CreateDirectoryW(brokenFolder, nullptr);
        brokenCount = MakeBrokenSamples(brokenFolder, sampleMp3, sampleFlac, log);
    }
    // the broken files' folder gets one good file (the generated WAV): the player must skip the others and play it
    if (brokenCount && m->formatCount < 24)
    {
        wchar_t good[MAX_PATH] = L"", copy[MAX_PATH];
        for (int i = 0; i < m->formatCount && !good[0]; i++)
            if (!wcscmp(m->formats[i].format, L"WAV")) Join(good, m->formats[i].folder, L"melody.wav");
        Join(copy, brokenFolder, L"good-melody.wav");
        if (good[0] && CopyFileW(good, copy, FALSE))
        {
            TestMusicFolder& tf = m->formats[m->formatCount++];
            wcscpy(tf.folder, brokenFolder);
            _snwprintf(tf.format, 64, L"%d broken files + a good WAV", brokenCount);
        }
    }
    LogF(log, L"  built-in samples: %d, broken files: %d", added, brokenCount);
    return added;
}

// Folders of earlier runs whose program no longer runs (closed with the window's close box: no time to clean up).
static void RemoveStaleFolders(const wchar_t* tmp, const wchar_t* prefix)
{
    wchar_t pattern[MAX_PATH];
    _snwprintf(pattern, MAX_PATH, L"%ls\\%ls*", tmp, prefix);
    pattern[MAX_PATH - 1] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        DWORD pid = (DWORD)wcstoul(fd.cFileName + wcslen(prefix), nullptr, 10);
        if (!pid || pid == GetCurrentProcessId()) continue;
        HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid);
        bool alive = p && WaitForSingleObject(p, 0) == WAIT_TIMEOUT;
        if (p) CloseHandle(p);
        if (alive) continue;
        wchar_t from[MAX_PATH + 2] = {};
        _snwprintf(from, MAX_PATH, L"%ls\\%ls", tmp, fd.cFileName);
        SHFILEOPSTRUCTW op = {};
        op.wFunc = FO_DELETE;
        op.pFrom = from;
        op.fFlags = FOF_NO_UI;
        int rc = SHFileOperationW(&op);
        AppLog(L"left over from an earlier run: %ls removed (%d)", from, rc);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

bool TestMusicCreate(TestMusic* m, MusicLog log)
{
    ZeroMemory(m, sizeof(*m));
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH - 40, tmp);
    size_t len = wcslen(tmp);
    if (len && tmp[len - 1] == L'\\') tmp[len - 1] = 0;
    RemoveStaleFolders(tmp, L"s2mautotest-");
    _snwprintf(m->root, MAX_PATH, L"%ls\\s2mautotest-%lu", tmp, GetCurrentProcessId());
    m->root[MAX_PATH - 1] = 0;
    if (!CreateDirectoryW(m->root, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
    {
        LogF(log, L"  test music: cannot create %ls (%lu)", m->root, GetLastError());
        m->root[0] = 0;
        return false;
    }
    LogF(log, L"Test music in %ls", m->root);
    UINT32 frames = 0;
    SHORT* pcm = MakeMelody(&frames);
    if (!pcm) return false;
    Join(m->all, m->root, L"music-all");
    CreateDirectoryW(m->all, nullptr);
    Join(m->empty, m->root, L"music-none");
    CreateDirectoryW(m->empty, nullptr);
    {
        wchar_t f[MAX_PATH];
        Join(f, m->empty, L"readme.txt");
        WriteJunk(f, "no music here", 0);
    }

    struct { const wchar_t* name; const wchar_t* ext; const GUID* sub; } kFormats[] = {
        { L"MP3", L"mp3", &MFAudioFormat_MP3 }, { L"FLAC", L"flac", &MFAudioFormat_FLAC }, { L"WAV", L"wav", nullptr },
    };
    for (auto& k : kFormats)
    {
        TestMusicFolder& tf = m->formats[m->formatCount];
        wchar_t sub[32], file[MAX_PATH], name[32];
        _snwprintf(sub, 32, L"music-%ls", k.ext);
        Join(tf.folder, m->root, sub);
        CreateDirectoryW(tf.folder, nullptr);
        _snwprintf(name, 32, L"melody.%ls", k.ext);
        Join(file, tf.folder, name);
        HRESULT hr = k.sub ? WriteEncoded(file, *k.sub, pcm, frames) : (WriteWav(file, pcm, frames) ? S_OK : E_FAIL);
        if (FAILED(hr))
        {
            LogF(log, L"  test music: %ls not made (0x%08lX): skipped", k.name, (unsigned long)hr);
            RemoveDirectoryW(tf.folder);
            continue;
        }
        wcsncpy(tf.format, k.name, 63);
        m->formatCount++;
        wchar_t copy[MAX_PATH];
        _snwprintf(name, 32, L"all.%ls", k.ext);
        Join(copy, m->all, name);
        CopyFileW(file, copy, FALSE);
    }
    free(pcm);
    {
        wchar_t f[MAX_PATH];
        Join(f, m->all, L"broken.mp3");
        WriteJunk(f, nullptr, 8192);
        Join(f, m->all, L"readme.txt");
        WriteJunk(f, "not music", 0);
    }
    LogF(log, L"  %d music format(s)", m->formatCount);
    AddSamples(m, log);
    return m->formatCount > 0;
}

void TestMusicDelete(TestMusic* m)
{
    if (!m->root[0]) return;
    wchar_t from[MAX_PATH + 2] = {};
    wcsncpy(from, m->root, MAX_PATH);
    SHFILEOPSTRUCTW op = {};
    op.wFunc = FO_DELETE;
    op.pFrom = from;
    op.fFlags = FOF_NO_UI;
    int rc = SHFileOperationW(&op);
    AppLog(L"test music %ls removed: %ls (%d)", m->root, GetFileAttributesW(m->root) == INVALID_FILE_ATTRIBUTES ? L"yes" : L"NO", rc);
    m->root[0] = 0;
}
