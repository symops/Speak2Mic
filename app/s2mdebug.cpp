// s2mdebug - debugging tool for Speak2Mic sound formats.
//
//   s2mdebug                 full report (below); needs administrator rights (the manifest requests them)
//   s2mdebug propose on|off  answer KSPROPERTY_PIN_PROPOSEDATAFORMAT in the driver or not (ProposeFormat
//                            setting), restart the device, then the full report
//   s2mdebug formats         only the endpoint and format tests (no driver diagnostics)
//
// Report:
//   1. driver settings and diagnostics (device, driver version, KS filters, events)
//   2. every Speak2Mic sound endpoint: state, stored formats, all endpoint properties
//   3. format matrix per endpoint: IAudioClient::IsFormatSupported (exclusive/shared) and a real
//      exclusive-mode Initialize (opens a stream in the driver) for 16/24/32-bit PCM and float
//   4. default-format tests: IPolicyConfig::SetDeviceFormat with every way of writing 16/24/32 bits,
//      the previous format is put back after each test
//   5. the driver log of everything Windows asked the driver during the tests
// Output goes to the console and to %ProgramData%\Speak2Mic\logs\debug.log.
#include "audio.h"
#include "lang.h"
#include "ready.h"
#include "applog.h"
#include "audiosvc.h"
#include "devctl.h"
#include "diag.h"
#include "setupcore.h"
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <shlobj.h>
#include <math.h>
#include <stdio.h>
#include <wchar.h>
#include <io.h>
#include <fcntl.h>

static wchar_t g_mark[32];      // driver log lines after this time stamp belong to this run

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

static void OutLog(void*, const wchar_t* line)
{
    Out(L"%ls", line);
}

static const wchar_t* HrText(HRESULT hr)
{
    switch (hr)
    {
    case S_OK:                                  return L"OK";
    case S_FALSE:                               return L"S_FALSE (closest match offered)";
    case (HRESULT)0x88890008:                   return L"AUDCLNT_E_UNSUPPORTED_FORMAT";
    case (HRESULT)0x8889000A:                   return L"AUDCLNT_E_DEVICE_IN_USE";
    case (HRESULT)0x8889000E:                   return L"AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED";
    case (HRESULT)0x88890004:                   return L"AUDCLNT_E_DEVICE_INVALIDATED";
    case (HRESULT)0x88890019:                   return L"AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED";
    case (HRESULT)0x88890020:                   return L"AUDCLNT_E_INVALID_DEVICE_PERIOD";
    case (HRESULT)0x88890006:                   return L"AUDCLNT_E_BUFFER_SIZE_ERROR";
    case (HRESULT)0x88890003:                   return L"AUDCLNT_E_WRONG_ENDPOINT_TYPE";
    case (HRESULT)0x8889000C:                   return L"AUDCLNT_E_ENDPOINT_CREATE_FAILED";
    case (HRESULT)0x88890010:                   return L"AUDCLNT_E_SERVICE_NOT_RUNNING";
    case E_INVALIDARG:                          return L"E_INVALIDARG";
    case E_NOTFOUND:                            return L"E_NOTFOUND";
    case E_ACCESSDENIED:                        return L"E_ACCESSDENIED";
    case E_NOINTERFACE:                         return L"E_NOINTERFACE";
    default:                                    return L"";
    }
}

// ---------------------------------------------------------------------------
// Driver log

static void SetMark()
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    _snwprintf(g_mark, 32, L"%04u-%02u-%02u %02u:%02u:%02u.%03u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
               t.wSecond, t.wMilliseconds);
    g_mark[31] = 0;
}

static void DumpDriverLogSinceMark()
{
    DWORD size = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, S2M_PARAMS_KEY, L"DriverLog", RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS ||
        !size)
    {
        Out(TR(L"(журнал драйвера пуст или недоступен)"));
        return;
    }
    wchar_t* text = (wchar_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size + 2);
    if (!text) return;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, S2M_PARAMS_KEY, L"DriverLog", RRF_RT_REG_SZ, nullptr, text, &size) == ERROR_SUCCESS)
    {
        for (wchar_t* line = text; *line;)
        {
            wchar_t* end = line;
            while (*end && *end != L'\r' && *end != L'\n') end++;
            wchar_t saved = *end;
            *end = 0;
            if (wcslen(line) >= 23 && wcsncmp(line, g_mark, 23) >= 0) Out(L"[driver] %ls", line);
            *end = saved;
            while (*end == L'\r' || *end == L'\n') end++;
            line = end;
        }
    }
    HeapFree(GetProcessHeap(), 0, text);
}

// ---------------------------------------------------------------------------
// Endpoints

struct Endpoint
{
    wchar_t   id[256];
    wchar_t   name[256];
    EDataFlow flow;
    DWORD     state;
};

static int ListSpeak2MicEndpoints(Endpoint* out, int max)
{
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                (void**)&en)))
        return 0;
    int n = 0;
    for (int f = 0; f < 2; f++)
    {
        IMMDeviceCollection* col = nullptr;
        if (FAILED(en->EnumAudioEndpoints(f == 0 ? eRender : eCapture, DEVICE_STATEMASK_ALL, &col))) continue;
        UINT count = 0;
        col->GetCount(&count);
        for (UINT i = 0; i < count && n < max; i++)
        {
            IMMDevice* dev = nullptr;
            if (FAILED(col->Item(i, &dev))) continue;
            IPropertyStore* ps = nullptr;
            PROPVARIANT v;
            PropVariantInit(&v);
            Endpoint e = {};
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps)) && SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) &&
                v.vt == VT_LPWSTR && wcsstr(v.pwszVal, L"Speak2Mic"))
            {
                LPWSTR id = nullptr;
                dev->GetId(&id);
                if (id)
                {
                    wcsncpy(e.id, id, 255);
                    CoTaskMemFree(id);
                }
                wcsncpy(e.name, v.pwszVal, 255);
                e.flow = f == 0 ? eRender : eCapture;
                dev->GetState(&e.state);
                out[n++] = e;
            }
            PropVariantClear(&v);
            if (ps) ps->Release();
            dev->Release();
        }
        col->Release();
    }
    en->Release();
    return n;
}

static IMMDevice* OpenEndpoint(const wchar_t* id)
{
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                (void**)&en)))
        return nullptr;
    IMMDevice* dev = nullptr;
    if (FAILED(en->GetDevice(id, &dev))) dev = nullptr;
    en->Release();
    return dev;
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

static void DumpProperties(IMMDevice* dev)
{
    IPropertyStore* ps = nullptr;
    if (FAILED(dev->OpenPropertyStore(STGM_READ, &ps))) return;
    DWORD count = 0;
    ps->GetCount(&count);
    for (DWORD i = 0; i < count; i++)
    {
        PROPERTYKEY key;
        if (FAILED(ps->GetAt(i, &key))) continue;
        PROPVARIANT v;
        PropVariantInit(&v);
        if (FAILED(ps->GetValue(key, &v))) continue;
        wchar_t k[64], val[400] = L"";
        const GUID& g = key.fmtid;
        _snwprintf(k, 64, L"{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x},%lu", g.Data1, g.Data2, g.Data3,
                   g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7],
                   key.pid);
        k[63] = 0;
        switch (v.vt)
        {
        case VT_LPWSTR: _snwprintf(val, 400, L"\"%ls\"", v.pwszVal); break;
        case VT_UI4:    _snwprintf(val, 400, L"%lu (0x%lX)", v.ulVal, v.ulVal); break;
        case VT_I4:     _snwprintf(val, 400, L"%ld", v.lVal); break;
        case VT_BOOL:   _snwprintf(val, 400, L"%ls", v.boolVal ? L"true" : L"false"); break;
        case VT_CLSID:
            if (v.puuid)
                _snwprintf(val, 400, L"{%08lx-%04x-%04x-...}", v.puuid->Data1, v.puuid->Data2, v.puuid->Data3);
            break;
        case VT_BLOB:
            if (v.blob.cbSize >= sizeof(WAVEFORMATEX) &&
                v.blob.cbSize >= sizeof(WAVEFORMATEX) + ((WAVEFORMATEX*)v.blob.pBlobData)->cbSize &&
                ((WAVEFORMATEX*)v.blob.pBlobData)->nChannels && ((WAVEFORMATEX*)v.blob.pBlobData)->nSamplesPerSec)
            {
                wchar_t f[200];
                DescribeWaveFormat((WAVEFORMATEX*)v.blob.pBlobData, f, 200);
                _snwprintf(val, 400, L"blob %lu bytes, format: %ls", v.blob.cbSize, f);
            }
            else
            {
                int n = _snwprintf(val, 400, L"blob %lu bytes:", v.blob.cbSize);
                for (ULONG b = 0; b < v.blob.cbSize && b < 48 && n > 0 && n < 390; b++)
                    n += _snwprintf(val + n, 400 - n, L" %02X", v.blob.pBlobData[b]);
            }
            break;
        default: _snwprintf(val, 400, L"(vt %u)", v.vt); break;
        }
        val[399] = 0;
        Out(L"    prop %ls = %ls", k, val);
        PropVariantClear(&v);
    }
    ps->Release();
}

static void ShowStoredFormats(const Endpoint& e)
{
    wchar_t f[200];
    WAVEFORMATEX* w = nullptr;
    HRESULT hr = PolicyGetFormat(e.id, false, &w);
    DescribeWaveFormat(w, f, 200);
    Out(TR(L"  формат устройства (Windows):  %ls  [0x%08lX]"), f, (unsigned long)hr);
    if (w) CoTaskMemFree(w);
    hr = PolicyGetFormat(e.id, true, &w);
    DescribeWaveFormat(w, f, 200);
    Out(TR(L"  формат по умолчанию (OEM):    %ls  [0x%08lX]"), f, (unsigned long)hr);
    if (w) CoTaskMemFree(w);
    hr = PolicyGetMixFormat(e.id, &w);
    DescribeWaveFormat(w, f, 200);
    Out(TR(L"  формат микшера:               %ls  [0x%08lX]"), f, (unsigned long)hr);
    if (w) CoTaskMemFree(w);
}

// ---------------------------------------------------------------------------
// Format tests

struct SampleType { WORD container, valid; bool isFloat; const wchar_t* name; };
static const SampleType kTypes[] = {
    { 16, 16, false, L"PCM 16" },
    { 24, 24, false, TR(L"PCM 24 (3 байта)") },
    { 32, 24, false, TR(L"PCM 24 в 32") },
    { 32, 32, false, L"PCM 32" },
    { 32, 32, true,  L"float 32" },
};

static void FormatMatrix(const Endpoint& e, DWORD rate, WORD maxChannels)
{
    IMMDevice* dev = OpenEndpoint(e.id);
    if (!dev) return;
    Out(TR(L"  проверка форматов (%lu Гц): IsFormatSupported монопольно / общий режим / открытие потока монопольно"), rate);
    WORD chans[2] = { 1, maxChannels };
    int nch = maxChannels > 1 ? 2 : 1;
    for (int c = 0; c < nch; c++)
    {
        for (const SampleType& t : kTypes)
        {
            for (int ext = 1; ext >= 0; ext--)
            {
                if (!ext && t.container != t.valid) continue;   // "24 in 32" needs WAVEFORMATEXTENSIBLE
                WAVEFORMATEXTENSIBLE fmt;
                MakeWaveFormat(&fmt, rate, t.container, t.valid, chans[c], t.isFloat, ext != 0);

                IAudioClient* ac = nullptr;
                HRESULT hrEx = E_FAIL, hrSh = E_FAIL, hrInit = E_FAIL;
                if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac)))
                {
                    hrEx = ac->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &fmt.Format, nullptr);
                    WAVEFORMATEX* closest = nullptr;
                    hrSh = ac->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, &fmt.Format, &closest);
                    if (closest) CoTaskMemFree(closest);
                    REFERENCE_TIME def = 0, min = 0;
                    ac->GetDevicePeriod(&def, &min);
                    hrInit = ac->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, 0, def, def, &fmt.Format, nullptr);
                    ac->Release();
                }
                Out(TR(L"    %u кан. %-17ls %-10ls  монопольно 0x%08lX %-28ls общий 0x%08lX %-22ls открытие 0x%08lX %ls"),
                    chans[c], t.name, ext ? L"extensible" : TR(L"простой"), (unsigned long)hrEx, HrText(hrEx),
                    (unsigned long)hrSh, HrText(hrSh), (unsigned long)hrInit, HrText(hrInit));
            }
        }
    }
    dev->Release();
}

// SetDeviceFormat the way the panel does (and a few more), putting the old pair back after each success.
static void DefaultFormatTests(const Endpoint& e, DWORD rate, WORD channels)
{
    WAVEFORMATEX* oldDev = nullptr;
    WAVEFORMATEX* oldMix = nullptr;
    PolicyGetFormat(e.id, false, &oldDev);
    PolicyGetMixFormat(e.id, &oldMix);
    Out(TR(L"  смена формата по умолчанию (IPolicyConfig::SetDeviceFormat), %lu Гц %u кан.:"), rate, channels);

    WAVEFORMATEXTENSIBLE mixFloat, mixFloatPlain;
    MakeWaveFormat(&mixFloat, rate, 32, 32, channels, true, true);
    MakeWaveFormat(&mixFloatPlain, rate, 32, 32, channels, true, false);
    for (const SampleType& t : kTypes)
    {
        for (int ext = 1; ext >= 0; ext--)
        {
            if (!ext && t.container != t.valid) continue;
            WAVEFORMATEXTENSIBLE dev;
            MakeWaveFormat(&dev, rate, t.container, t.valid, channels, t.isFloat, ext != 0);
            struct { const WAVEFORMATEX* mix; const wchar_t* name; } mixes[] = {
                { &mixFloat.Format, TR(L"микшер float ext") },
                { &mixFloatPlain.Format, TR(L"микшер float простой") },
                { &dev.Format, TR(L"микшер = устройство") },
            };
            for (auto& m : mixes)
            {
                HRESULT hr = PolicySetFormat(e.id, &dev.Format, m.mix);
                wchar_t now[200] = L"";
                if (SUCCEEDED(hr))
                {
                    WAVEFORMATEX* w = nullptr;
                    PolicyGetFormat(e.id, false, &w);
                    DescribeWaveFormat(w, now, 200);
                    if (w) CoTaskMemFree(w);
                    if (oldDev && oldMix) PolicySetFormat(e.id, oldDev, oldMix);
                }
                Out(L"    %-17ls %-10ls %-22ls 0x%08lX %ls%ls%ls", t.name, ext ? L"extensible" : TR(L"простой"), m.name,
                    (unsigned long)hr, HrText(hr), now[0] ? TR(L" -> стало: ") : L"", now);
            }
        }
    }
    if (oldDev && oldMix)
    {
        HRESULT hr = PolicySetFormat(e.id, oldDev, oldMix);
        Out(TR(L"  прежний формат восстановлен: 0x%08lX"), (unsigned long)hr);
    }
    if (oldDev) CoTaskMemFree(oldDev);
    if (oldMix) CoTaskMemFree(oldMix);
}

static void EndpointReport(bool tests)
{
    DWORD rate = S2mGetParam(L"SampleRate", 48000);
    WORD channels = (WORD)S2mGetParam(L"Channels", 2);
    WORD micChannels = (WORD)S2mGetParam(L"MicChannels", 1);
    if (!micChannels) micChannels = channels;       // 0 = as the speaker
    Endpoint eps[32];
    int n = ListSpeak2MicEndpoints(eps, 32);
    Out(L"");
    Out(TR(L"==== звуковые устройства Speak2Mic: %d ===="), n);
    for (int i = 0; i < n; i++)
    {
        const Endpoint& e = eps[i];
        Out(L"");
        Out(L"«%ls» (%ls): %ls", e.name, e.flow == eRender ? TR(L"воспроизведение") : TR(L"запись"), StateText(e.state));
        Out(L"  id: %ls", e.id);
        ShowStoredFormats(e);
        IMMDevice* dev = OpenEndpoint(e.id);
        if (dev)
        {
            Out(TR(L"  свойства устройства:"));
            DumpProperties(dev);
            dev->Release();
        }
        if (tests && e.state == DEVICE_STATE_ACTIVE)
        {
            WORD ch = e.flow == eCapture ? micChannels : channels;
            FormatMatrix(e, rate, ch);
            DefaultFormatTests(e, rate, ch);
            ShowStoredFormats(e);
        }
    }
}

// ---------------------------------------------------------------------------
// Level test: a known tone is played into Speak2Mic Speaker and measured on the speaker output
// (loopback, what the panel's upper meter shows) and on Speak2Mic Microphone at the same time.

static void VolumeInfo(const Endpoint& e)
{
    EndpointVolumeInfo v;
    if (!GetEndpointVolumeInfo(e.id, &v)) return;
    Out(TR(L"  громкость Windows «%ls»: %.0f%% (%.1f дБ, диапазон %.1f…%.1f дБ), звук %ls, регулировка %ls"), e.name,
        v.scalar * 100.0f, v.db, v.minDb, v.maxDb, v.mute ? TR(L"выключен") : TR(L"включён"),
        v.hardware ? TR(L"в драйвере") : TR(L"программная"));
}

struct LevelStream
{
    IAudioClient*        client = nullptr;
    IAudioCaptureClient* capture = nullptr;
    WAVEFORMATEX*        fmt = nullptr;
    bool                 isFloat = false;
    float                peak = 0;
    double               sumSq = 0;
    unsigned long long   count = 0;
};

static bool IsFloatFormat(const WAVEFORMATEX* f)
{
    return f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
           (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && ((const WAVEFORMATEXTENSIBLE*)f)->SubFormat.Data1 == 3);
}

static float SampleValue(const BYTE* s, bool isFloat, int bytes)
{
    if (isFloat && bytes == 4) return *(const float*)s;
    if (bytes == 2) return *(const short*)s / 32768.0f;
    if (bytes == 3) return (float)(((int)s[0] << 8 | (int)s[1] << 16 | (int)s[2] << 24) >> 8) / 8388608.0f;
    if (bytes == 4) return (float)(*(const int*)s / 2147483648.0);
    return 0;
}

static HRESULT OpenCaptureStream(const wchar_t* id, bool loopback, LevelStream* st)
{
    IMMDevice* dev = OpenEndpoint(id);
    if (!dev) return E_NOTFOUND;
    HRESULT hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&st->client);
    dev->Release();
    if (SUCCEEDED(hr)) hr = st->client->GetMixFormat(&st->fmt);
    if (SUCCEEDED(hr)) hr = st->client->Initialize(AUDCLNT_SHAREMODE_SHARED, loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0,
                                                   2000000, 0, st->fmt, nullptr);
    if (SUCCEEDED(hr)) hr = st->client->GetService(__uuidof(IAudioCaptureClient), (void**)&st->capture);
    if (SUCCEEDED(hr)) st->isFloat = IsFloatFormat(st->fmt);
    return hr;
}

static void DrainCapture(LevelStream* st, bool measure)
{
    UINT32 packet = 0;
    int bytes = st->fmt->wBitsPerSample / 8;
    while (SUCCEEDED(st->capture->GetNextPacketSize(&packet)) && packet > 0)
    {
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        if (FAILED(st->capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
        if (measure && !(flags & AUDCLNT_BUFFERFLAGS_SILENT))
        {
            for (UINT32 f = 0; f < frames; f++)
                for (int c = 0; c < st->fmt->nChannels; c++)
                {
                    float v = SampleValue(data + (size_t)f * st->fmt->nBlockAlign + c * bytes, st->isFloat, bytes);
                    if (fabsf(v) > st->peak) st->peak = fabsf(v);
                    st->sumSq += (double)v * v;
                    st->count++;
                }
        }
        else if (measure)
        {
            st->count += (unsigned long long)frames * st->fmt->nChannels;    // silence counts as zeros
        }
        st->capture->ReleaseBuffer(frames);
    }
}

static float ToDb(double v) { return v > 1e-6 ? (float)(20.0 * log10(v)) : -120.0f; }

static void CloseStream(LevelStream* st)
{
    if (st->client) st->client->Stop();
    if (st->capture) st->capture->Release();
    if (st->client) st->client->Release();
    if (st->fmt) CoTaskMemFree(st->fmt);
    *st = LevelStream();
}

static void LevelTest()
{
    Out(L"");
    Out(TR(L"==== проверка уровня (тон 1 кГц, −20 дБFS, 3 с) ===="));
    Endpoint eps[32];
    int n = ListSpeak2MicEndpoints(eps, 32);
    const Endpoint* spk = nullptr;
    const Endpoint* mic = nullptr;
    for (int i = 0; i < n; i++)
    {
        if (eps[i].state != DEVICE_STATE_ACTIVE) continue;
        if (eps[i].flow == eRender && !spk) spk = &eps[i];
        if (eps[i].flow == eCapture && !mic) mic = &eps[i];
    }
    if (!spk || !mic)
    {
        Out(TR(L"  Speak2Mic Speaker/Mic не найдены или не активны — проверка уровня пропущена."));
        return;
    }
    VolumeInfo(*spk);
    VolumeInfo(*mic);

    const float amplitude = 0.1f;       // -20 dBFS
    IAudioClient* render = nullptr;
    IAudioRenderClient* renderClient = nullptr;
    WAVEFORMATEX* rfmt = nullptr;
    LevelStream loop, cap;
    HRESULT hr;
    int step = 1;
    IMMDevice* dev = OpenEndpoint(spk->id);
    hr = dev ? dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&render) : E_NOTFOUND;
    if (dev) dev->Release();
    if (SUCCEEDED(hr)) hr = render->GetMixFormat(&rfmt);
    if (SUCCEEDED(hr)) hr = render->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 2000000, 0, rfmt, nullptr);
    if (SUCCEEDED(hr)) hr = render->GetService(__uuidof(IAudioRenderClient), (void**)&renderClient);
    if (SUCCEEDED(hr)) { step = 2; hr = OpenCaptureStream(spk->id, true, &loop); }
    if (SUCCEEDED(hr)) { step = 3; hr = OpenCaptureStream(mic->id, false, &cap); }
    UINT32 bufferFrames = 0;
    if (SUCCEEDED(hr)) hr = render->GetBufferSize(&bufferFrames);
    if (SUCCEEDED(hr)) { step = 4; hr = loop.client->Start(); }
    if (SUCCEEDED(hr)) hr = cap.client->Start();
    if (SUCCEEDED(hr)) hr = render->Start();
    if (FAILED(hr))
    {
        Out(TR(L"  не удалось запустить проверку уровня (0x%08lX, шаг %d)"), (unsigned long)hr, step);
    }
    else
    {
        bool isFloat = IsFloatFormat(rfmt);
        int bytes = rfmt->wBitsPerSample / 8;
        double phase = 0, dphase = 2.0 * 3.14159265358979 * 1000.0 / rfmt->nSamplesPerSec;
        DWORD t0 = GetTickCount();
        for (;;)
        {
            DWORD t = GetTickCount() - t0;
            if (t > 3000) break;
            UINT32 padding = 0;
            render->GetCurrentPadding(&padding);
            UINT32 frames = bufferFrames - padding;
            BYTE* data = nullptr;
            if (frames && SUCCEEDED(renderClient->GetBuffer(frames, &data)))
            {
                for (UINT32 f = 0; f < frames; f++)
                {
                    float v = amplitude * (float)sin(phase);
                    phase += dphase;
                    for (int c = 0; c < rfmt->nChannels; c++)
                    {
                        BYTE* s = data + (size_t)f * rfmt->nBlockAlign + c * bytes;
                        if (isFloat && bytes == 4) *(float*)s = v;
                        else if (bytes == 2) *(short*)s = (short)(v * 32767.0f);
                        else if (bytes == 4) *(int*)s = (int)(v * 2147483647.0);
                    }
                }
                renderClient->ReleaseBuffer(frames, 0);
            }
            bool measure = t > 800;     // skip the start (latency, engine warm-up)
            DrainCapture(&loop, measure);
            DrainCapture(&cap, measure);
            Sleep(10);
        }
        render->Stop();
        float rmsTone = amplitude / sqrtf(2.0f);
        Out(TR(L"  проиграно в Speak2Mic Speaker:            пик %6.1f дБFS, RMS %6.1f дБFS"), ToDb(amplitude), ToDb(rmsTone));
        Out(TR(L"  выход Speak2Mic Speaker (loopback):       пик %6.1f дБFS, RMS %6.1f дБFS"), ToDb(loop.peak),
            ToDb(loop.count ? sqrt(loop.sumSq / loop.count) : 0));
        Out(TR(L"  Speak2Mic Microphone:                     пик %6.1f дБFS, RMS %6.1f дБFS"), ToDb(cap.peak),
            ToDb(cap.count ? sqrt(cap.sumSq / cap.count) : 0));
        Out(TR(L"  разница микрофон − выход динамика: %+.1f дБ (должна быть около 0)"), ToDb(cap.peak) - ToDb(loop.peak));
    }
    if (render) render->Stop();
    if (renderClient) renderClient->Release();
    if (render) render->Release();
    if (rfmt) CoTaskMemFree(rfmt);
    CloseStream(&loop);
    CloseStream(&cap);
}

// ---------------------------------------------------------------------------
// "s2mdebug sounds": extracts the WAV resources of the Windows audio modules (the speaker "Test" melody is
// one of them) to %ProgramData%\Speak2Mic\sounds, to find which one Windows plays.

struct SoundDump
{
    const wchar_t* module;
    wchar_t        dir[MAX_PATH];
    int            count;
};

static void WaveInfo(const BYTE* p, DWORD size, wchar_t* out, size_t len)
{
    _snwprintf(out, len, L"?");
    for (DWORD pos = 12; pos + 8 <= size;)
    {
        DWORD n = *(const DWORD*)(p + pos + 4);
        if (memcmp(p + pos, "fmt ", 4) == 0 && n >= 16)
        {
            const WAVEFORMATEX* f = (const WAVEFORMATEX*)(p + pos + 8);
            double sec = f->nAvgBytesPerSec ? (double)(size - 44) / f->nAvgBytesPerSec : 0;
            _snwprintf(out, len, L"%lu Hz, %u bit, %u ch, tag 0x%04X, ~%.1f s", f->nSamplesPerSec, f->wBitsPerSample,
                       f->nChannels, f->wFormatTag, sec);
            break;
        }
        pos += 8 + n + (n & 1);
    }
    out[len - 1] = 0;
}

static BOOL CALLBACK DumpName(HMODULE h, LPCWSTR type, LPWSTR name, LONG_PTR param)
{
    SoundDump* d = (SoundDump*)param;
    HRSRC r = FindResourceW(h, name, type);
    HGLOBAL g = r ? LoadResource(h, r) : nullptr;
    const BYTE* p = g ? (const BYTE*)LockResource(g) : nullptr;
    DWORD size = r ? SizeofResource(h, r) : 0;
    if (!p || size < 44 || memcmp(p, "RIFF", 4) != 0 || memcmp(p + 8, "WAVE", 4) != 0) return TRUE;
    wchar_t id[64], tp[64], file[MAX_PATH], info[128];
    if (IS_INTRESOURCE(name)) _snwprintf(id, 64, L"%u", (unsigned)(ULONG_PTR)name); else _snwprintf(id, 64, L"%ls", name);
    if (IS_INTRESOURCE(type)) _snwprintf(tp, 64, L"%u", (unsigned)(ULONG_PTR)type); else _snwprintf(tp, 64, L"%ls", type);
    id[63] = tp[63] = 0;
    _snwprintf(file, MAX_PATH, L"%ls\\%ls_%ls_%ls.wav", d->dir, d->module, tp, id);
    file[MAX_PATH - 1] = 0;
    HANDLE f = CreateFileW(file, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        WriteFile(f, p, size, &written, nullptr);
        CloseHandle(f);
    }
    WaveInfo(p, size, info, 128);
    Out(L"  %ls  (%ls)", file, info);
    d->count++;
    return TRUE;
}

static BOOL CALLBACK DumpType(HMODULE h, LPWSTR type, LONG_PTR param)
{
    EnumResourceNamesW(h, type, DumpName, param);
    return TRUE;
}

static void DumpSounds()
{
    Out(TR(L"==== звуки Windows (ресурсы WAV) ===="));
    SoundDump d = {};
    PWSTR data = nullptr;
    wchar_t base[MAX_PATH] = L"C:\\ProgramData";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &data)))
    {
        wcsncpy(base, data, MAX_PATH - 1);
        CoTaskMemFree(data);
    }
    _snwprintf(d.dir, MAX_PATH, L"%ls\\Speak2Mic\\sounds", base);
    d.dir[MAX_PATH - 1] = 0;
    SHCreateDirectoryExW(nullptr, d.dir, nullptr);
    const wchar_t* modules[] = { L"mmres.dll", L"mmsys.cpl", L"SndVolSSO.dll", L"AudioSrv.dll", L"imageres.dll" };
    for (const wchar_t* m : modules)
    {
        // As a data file: on Windows 10 1903+ the resources live in SystemResources\<name>.mun, the loader finds them.
        HMODULE h = LoadLibraryExW(m, nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        if (!h)
        {
            Out(L"  %ls: %lu", m, GetLastError());
            continue;
        }
        d.module = m;
        EnumResourceTypesW(h, DumpType, (LONG_PTR)&d);
        FreeLibrary(h);
    }
    Out(TR(L"Найдено звуков: %d. Папка: %ls"), d.count, d.dir);
}

// ---------------------------------------------------------------------------

static void SettingsReport()
{
    Out(TR(L"==== настройки драйвера (%ls) ===="), S2M_PARAMS_KEY);
    const wchar_t* names[] = { L"SampleRate", L"Channels", L"MicChannels", L"BitsPerSample", L"LatencyMs",
                               L"ProposeFormat", L"StartStatus", L"CablesCreated" };
    for (const wchar_t* name : names)
    {
        DWORD v = S2mGetParam(name, 0xFFFFFFFF);
        if (v == 0xFFFFFFFF) Out(TR(L"  %-14ls (нет, по умолчанию)"), name);
        else Out(L"  %-14ls %lu", name, v);
    }
}

static bool SwitchPropose(DWORD value)
{
    Out(TR(L"ProposeFormat = %lu, перезапуск устройства Speak2Mic…"), value);
    if (!S2mSetParam(L"ProposeFormat", value))
    {
        Out(TR(L"Не удалось записать настройку (нужны права администратора)."));
        return false;
    }
    AudioServices svc;
    AudioStopServices(false, &svc);
    bool found = false, reboot = false;
    bool ok = S2mRestartDevice(&found, &reboot);
    AudioStartServices(&svc);
    if (!found)
    {
        Out(TR(L"Устройство Speak2Mic не найдено."));
        return false;
    }
    Out(TR(L"Перезапуск: %ls%ls"), ok ? TR(L"выполнен") : TR(L"ОШИБКА"), reboot ? TR(L", Windows просит перезагрузку") : L"");
    DiagResult r = WaitForEndpoints(15000);
    Out(r == DiagOk ? TR(L"Звуковые устройства Speak2Mic на месте.") : TR(L"Звуковые устройства Speak2Mic НЕ появились за 15 с."));
    if (r == DiagOk) S2mCleanupOrphanEndpoints(5000);
    return true;
}

int wmain(int argc, wchar_t** argv)
{
    LangArgs(&argc, argv);
    _setmode(_fileno(stdout), _O_U16TEXT);
    _setmode(_fileno(stderr), _O_U16TEXT);
    SetConsoleTitleW(TR(L"Speak2Mic — отладка форматов"));
    AppLogOpen(L"debug");
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    S2mNotReady notReady = S2mCheckReady();
    if (notReady != S2mReadyOk)
    {
        Out(L"%ls", S2mNotReadyText(notReady));
        DWORD procs[4];
        if (GetConsoleProcessList(procs, 4) <= 1)
        {
            Out(L"");
            Out(TR(L"Нажмите Enter для выхода."));
            getwchar();
        }
        CoUninitialize();
        return 4;
    }
    SetMark();

    bool full = true;
    if (argc >= 3 && _wcsicmp(argv[1], L"propose") == 0)
    {
        bool on = _wcsicmp(argv[2], L"on") == 0 || wcscmp(argv[2], L"1") == 0;
        SwitchPropose(on ? 1 : 0);
    }
    else if (argc >= 2 && _wcsicmp(argv[1], L"formats") == 0)
    {
        full = false;
    }
    else if (argc >= 2 && _wcsicmp(argv[1], L"sounds") == 0)
    {
        DumpSounds();
        Out(L"");
        Out(TR(L"Готово. Полный отчёт: %ls"), AppLogPath());
        DWORD procs[4];
        if (GetConsoleProcessList(procs, 4) <= 1)
        {
            Out(TR(L"Нажмите Enter для выхода."));
            getwchar();
        }
        CoUninitialize();
        return 0;
    }
    else if (argc >= 2 && _wcsicmp(argv[1], L"level") == 0)
    {
        LevelTest();
        Out(L"");
        Out(TR(L"Готово. Полный отчёт: %ls"), AppLogPath());
        DWORD procs[4];
        if (GetConsoleProcessList(procs, 4) <= 1)
        {
            Out(TR(L"Нажмите Enter для выхода."));
            getwchar();
        }
        CoUninitialize();
        return 0;
    }
    else if (argc >= 2)
    {
        Out(TR(L"Использование: s2mdebug [formats | level | sounds | propose on|off]"));
        return 2;
    }

    Out(TR(L"Закройте панель Speak2Mic и программы, использующие Speak2Mic Speaker/Mic: иначе проверка открытия"));
    Out(TR(L"потока в монопольном режиме покажет «устройство занято»."));
    Out(L"");
    SettingsReport();
    S2mLogEndpointNodes();      // Device Manager "Audio inputs and outputs", to debug.log
    LevelTest();
    Out(L"");
    DumpSounds();
    if (full)
    {
        Out(L"");
        Out(TR(L"==== диагностика ===="));
        RunDiagnostics(OutLog, nullptr);
        SetMark();      // the driver log below: only what the format tests caused
    }
    EndpointReport(true);
    Out(L"");
    Out(TR(L"==== журнал драйвера во время проверок ===="));
    Sleep(300);
    DumpDriverLogSinceMark();
    Out(L"");
    Out(TR(L"Готово. Полный отчёт: %ls"), AppLogPath());

    // Started by double click: keep the window open.
    DWORD procs[4];
    if (GetConsoleProcessList(procs, 4) <= 1)
    {
        Out(TR(L"Нажмите Enter для выхода."));
        getwchar();
    }
    CoUninitialize();
    return 0;
}
