// Shared WASAPI helpers: device enumeration, default formats, level metering.
#include "audio.h"
#include "lang.h"
#include "applog.h"
#include <avrt.h>
#include <endpointvolume.h>
#include <audiopolicy.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <wchar.h>
#include <math.h>

// Declared locally to avoid depending on functiondiscoverykeys / ksmedia headers.
static const PROPERTYKEY kFriendlyName = {
    { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };
// PKEY_Device_DeviceDesc: the endpoint's own name ("Speak2Mic Speaker"), what Sound settings rename.
static const PROPERTYKEY kDeviceDesc = {
    { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 2 };
// PKEY_DeviceInterface_FriendlyName: name of the adapter the endpoint belongs to ("Speak2Mic").
static const PROPERTYKEY kInterfaceName = {
    { 0x026e516e, 0xb814, 0x414b, { 0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22 } }, 2 };
static const GUID kSubtypeFloat = {
    0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

static const REFERENCE_TIME kHnsPerMs = 10000;

// ---------------------------------------------------------------------------
// IPolicyConfig: undocumented but stable (Windows 7 - 11) interface of the Sound control panel.

static const CLSID kPolicyConfigClient = { 0x870af99c, 0x171d, 0x4f9e, { 0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9 } };
static const IID kIPolicyConfig = { 0xf8679f50, 0x850a, 0x41cf, { 0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8 } };
static const GUID kSubtypePcm = { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

struct IPolicyConfig : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, BOOL fxStore, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, BOOL fxStore, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};

static DWORD ChannelMask(WORD channels)
{
    switch (channels)
    {
    case 1:  return 0x4;        // front center
    case 2:  return 0x3;        // front left, front right
    case 4:  return 0x33;       // FL, FR, back left, back right
    case 6:  return 0x3F;       // 5.1
    case 8:  return 0x63F;      // 7.1 surround
    default: return 0;
    }
}

void MakeWaveFormat(WAVEFORMATEXTENSIBLE* f, DWORD rate, WORD container, WORD valid, WORD channels, bool isFloat,
                    bool extensible)
{
    ZeroMemory(f, sizeof(*f));
    f->Format.wFormatTag = extensible ? WAVE_FORMAT_EXTENSIBLE : (isFloat ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM);
    f->Format.nChannels = channels;
    f->Format.nSamplesPerSec = rate;
    f->Format.wBitsPerSample = container;
    f->Format.nBlockAlign = (WORD)(channels * container / 8);
    f->Format.nAvgBytesPerSec = rate * f->Format.nBlockAlign;
    if (extensible)
    {
        f->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        f->Samples.wValidBitsPerSample = valid;
        f->dwChannelMask = ChannelMask(channels);
        f->SubFormat = isFloat ? kSubtypeFloat : kSubtypePcm;
    }
}

void DescribeWaveFormat(const WAVEFORMATEX* f, wchar_t* out, size_t len)
{
    if (!f)
    {
        _snwprintf(out, len, L"(none)");
    }
    else if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= 22)
    {
        const WAVEFORMATEXTENSIBLE* x = (const WAVEFORMATEXTENSIBLE*)f;
        const wchar_t* sub = IsEqualGUID(x->SubFormat, kSubtypePcm) ? L"PCM" : IsEqualGUID(x->SubFormat, kSubtypeFloat) ? L"float" : L"other";
        _snwprintf(out, len, L"%lu Hz %u/%u bit %u ch %ls (extensible, mask 0x%lX)", f->nSamplesPerSec,
                   x->Samples.wValidBitsPerSample, f->wBitsPerSample, f->nChannels, sub, x->dwChannelMask);
    }
    else
    {
        _snwprintf(out, len, L"%lu Hz %u bit %u ch %ls", f->nSamplesPerSec, f->wBitsPerSample, f->nChannels,
                   f->wFormatTag == WAVE_FORMAT_PCM ? L"PCM" : f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ? L"float" : L"tag?");
    }
    out[len - 1] = 0;
}

static IPolicyConfig* OpenPolicyConfig()
{
    IPolicyConfig* pc = nullptr;
    if (FAILED(CoCreateInstance(kPolicyConfigClient, nullptr, CLSCTX_ALL, kIPolicyConfig, (void**)&pc))) return nullptr;
    return pc;
}

template <class T> static void SafeRelease(T*& p)
{
    if (p)
    {
        p->Release();
        p = nullptr;
    }
}

static void Copy(wchar_t* dst, size_t len, const wchar_t* src)
{
    if (len == 0) return;
    wcsncpy(dst, src, len - 1);
    dst[len - 1] = 0;
}

static void FillDevice(IMMDevice* dev, EDataFlow flow, AudioDevice* out)
{
    out->id[0] = 0;
    out->name[0] = 0;
    out->desc[0] = 0;
    out->adapter[0] = 0;
    out->flow = flow;

    LPWSTR id = nullptr;
    if (SUCCEEDED(dev->GetId(&id)))
    {
        Copy(out->id, 256, id);
        CoTaskMemFree(id);
    }

    IPropertyStore* props = nullptr;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)))
    {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(props->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR)
        {
            Copy(out->name, 256, v.pwszVal);
        }
        PropVariantClear(&v);
        if (SUCCEEDED(props->GetValue(kDeviceDesc, &v)) && v.vt == VT_LPWSTR)
        {
            Copy(out->desc, 256, v.pwszVal);
        }
        PropVariantClear(&v);
        if (SUCCEEDED(props->GetValue(kInterfaceName, &v)) && v.vt == VT_LPWSTR)
        {
            Copy(out->adapter, 64, v.pwszVal);
        }
        PropVariantClear(&v);
        props->Release();
    }
}

int ListAudioDevices(AudioDevice* out, int max)
{
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&en)))
    {
        return 0;
    }

    int n = 0;
    const EDataFlow flows[2] = { eRender, eCapture };
    for (int f = 0; f < 2; f++)
    {
        IMMDeviceCollection* coll = nullptr;
        if (FAILED(en->EnumAudioEndpoints(flows[f], DEVICE_STATE_ACTIVE, &coll)))
        {
            continue;
        }
        UINT count = 0;
        coll->GetCount(&count);
        for (UINT i = 0; i < count && n < max; i++)
        {
            IMMDevice* dev = nullptr;
            if (SUCCEEDED(coll->Item(i, &dev)))
            {
                FillDevice(dev, flows[f], &out[n++]);
                dev->Release();
            }
        }
        coll->Release();
    }
    en->Release();
    return n;
}

bool GetDefaultRenderDevice(AudioDevice* out)
{
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&en)))
    {
        return false;
    }
    IMMDevice* dev = nullptr;
    bool ok = SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev));
    if (ok)
    {
        FillDevice(dev, eRender, out);
        dev->Release();
    }
    en->Release();
    return ok;
}

static IMMDevice* OpenDevice(const wchar_t* id)
{
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&en)))
    {
        return nullptr;
    }
    IMMDevice* dev = nullptr;
    if (FAILED(en->GetDevice(id, &dev)))
    {
        dev = nullptr;
    }
    en->Release();
    return dev;
}

static IAudioEndpointVolume* OpenEndpointVolume(const wchar_t* deviceId)
{
    IMMDevice* dev = OpenDevice(deviceId);
    if (!dev) return nullptr;
    IAudioEndpointVolume* vol = nullptr;
    if (FAILED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&vol))) vol = nullptr;
    dev->Release();
    return vol;
}

bool GetEndpointVolume(const wchar_t* deviceId, float* scalar)
{
    IAudioEndpointVolume* vol = OpenEndpointVolume(deviceId);
    if (!vol) return false;
    bool ok = SUCCEEDED(vol->GetMasterVolumeLevelScalar(scalar));
    vol->Release();
    return ok;
}

const GUID kS2mVolumeContext = { 0x5b2d7a1e, 0x9c4f, 0x4e8a, { 0x9b, 0x31, 0x6d, 0x52, 0x0e, 0x7c, 0x44, 0xa1 } };

bool GetEndpointVolumeDb(const wchar_t* deviceId, float* db, float* minDb, float* maxDb, HRESULT* hrOut)
{
    // E_NOINTERFACE: the endpoint or its volume interface could not be opened
    IAudioEndpointVolume* vol = OpenEndpointVolume(deviceId);
    if (hrOut) *hrOut = vol ? S_OK : E_NOINTERFACE;
    if (!vol) return false;
    float step = 0;
    HRESULT hr = vol->GetMasterVolumeLevel(db), master = hr;
    if (FAILED(hr))
    {
        // the per-channel levels (all equal unless someone set a balance): the loudest one
        UINT n = 0;
        if (SUCCEEDED(vol->GetChannelCount(&n)) && n > 0)
        {
            float best = -1000.0f, v = 0;
            HRESULT h = S_OK;
            for (UINT c = 0; c < n && SUCCEEDED(h); c++)
                if (SUCCEEDED(h = vol->GetChannelVolumeLevel(c, &v)) && v > best) best = v;
            if (SUCCEEDED(h)) { *db = best; hr = S_OK; }
        }
    }
    HRESULT range = E_FAIL;
    if (SUCCEEDED(hr))
    {
        // The range never changes for an endpoint, but after a device restart Windows sometimes fails it
        // (E_INVALIDARG, "0..0") while the level reads fine: then the last range read for this endpoint.
        static struct { wchar_t id[256]; float mn, mx; } cache[8];
        static int next;
        int slot = -1;
        for (int i = 0; i < 8; i++) if (wcscmp(cache[i].id, deviceId) == 0) slot = i;
        range = vol->GetVolumeRange(minDb, maxDb, &step);
        if (SUCCEEDED(range))
        {
            if (slot < 0) { slot = next; next = (next + 1) % 8; wcsncpy(cache[slot].id, deviceId, 255); }
            cache[slot].mn = *minDb;
            cache[slot].mx = *maxDb;
        }
        else if (slot >= 0) { *minDb = cache[slot].mn; *maxDb = cache[slot].mx; }
        else
        {
            // Nothing read yet in this process (e.g. s2mctl): -96 dB up to +9.5625 dB for a microphone (the
            // driver's volume node) and 0 dB for a speaker (Windows' software volume).
            EDataFlow flow = eRender;
            IMMDevice* dev = OpenDevice(deviceId);
            IMMEndpoint* ep = nullptr;
            if (dev && SUCCEEDED(dev->QueryInterface(__uuidof(IMMEndpoint), (void**)&ep)))
            {
                ep->GetDataFlow(&flow);
                ep->Release();
            }
            if (dev) dev->Release();
            *minDb = -96.0f;
            *maxDb = flow == eCapture ? 9.5625f : 0.0f;
        }
    }
    vol->Release();
    if (hrOut) *hrOut = FAILED(hr) ? hr : FAILED(master) ? master : range;
    return SUCCEEDED(hr);
}

void DescribeEndpointVolume(const wchar_t* deviceId, wchar_t* out, size_t len)
{
    IAudioEndpointVolume* vol = OpenEndpointVolume(deviceId);
    if (!vol)
    {
        _snwprintf(out, len, L"volume interface not available");
        out[len - 1] = 0;
        return;
    }
    float db = 0, scalar = 0, mn = 0, mx = 0, step = 0, ch0 = 0;
    UINT n = 0;
    BOOL mute = FALSE;
    HRESULT h1 = vol->GetMasterVolumeLevel(&db), h2 = vol->GetMasterVolumeLevelScalar(&scalar),
            h3 = vol->GetVolumeRange(&mn, &mx, &step), h4 = vol->GetChannelCount(&n),
            h5 = n ? vol->GetChannelVolumeLevel(0, &ch0) : E_FAIL, h6 = vol->GetMute(&mute);
    _snwprintf(out, len, L"level %.2f dB (0x%08lX), scalar %.3f (0x%08lX), range %.1f..%.1f (0x%08lX), channels %u (0x%08lX), "
               L"channel 1 %.2f dB (0x%08lX), mute %d (0x%08lX)", db, (unsigned long)h1, scalar, (unsigned long)h2, mn, mx,
               (unsigned long)h3, n, (unsigned long)h4, ch0, (unsigned long)h5, (int)mute, (unsigned long)h6);
    out[len - 1] = 0;
    vol->Release();
}

bool SetEndpointVolumeDb(const wchar_t* deviceId, float db)
{
    IAudioEndpointVolume* vol = OpenEndpointVolume(deviceId);
    if (!vol) return false;
    float mn = -96.0f, mx = 0.0f, step = 0;
    if (SUCCEEDED(vol->GetVolumeRange(&mn, &mx, &step)))     // unknown range: no clamping (not "0 dB at most")
    {
        if (db < mn) db = mn;
        if (db > mx) db = mx;
    }
    bool ok = SUCCEEDED(vol->SetMasterVolumeLevel(db, &kS2mVolumeContext));
    vol->Release();
    return ok;
}

bool GetEndpointMute(const wchar_t* deviceId, bool* mute)
{
    IAudioEndpointVolume* vol = OpenEndpointVolume(deviceId);
    if (!vol) return false;
    BOOL m = FALSE;
    bool ok = SUCCEEDED(vol->GetMute(&m));
    vol->Release();
    *mute = m != FALSE;
    return ok;
}

bool SetEndpointMute(const wchar_t* deviceId, bool mute)
{
    IAudioEndpointVolume* vol = OpenEndpointVolume(deviceId);
    if (!vol) return false;
    // Windows does not pass a SetMute to the driver when it believes the state is already that one; if the driver's
    // node disagrees (seen: microphone silent while Windows showed it unmuted), only a real change reaches it. So
    // when the state already reads as wanted, switch it over and back.
    BOOL now = !mute;
    if (SUCCEEDED(vol->GetMute(&now)) && (now != FALSE) == mute) vol->SetMute(mute ? FALSE : TRUE, &kS2mVolumeContext);
    bool ok = SUCCEEDED(vol->SetMute(mute ? TRUE : FALSE, &kS2mVolumeContext));
    vol->Release();
    return ok;
}

bool GetEndpointVolumeInfo(const wchar_t* deviceId, EndpointVolumeInfo* info)
{
    IAudioEndpointVolume* vol = OpenEndpointVolume(deviceId);
    if (!vol) return false;
    BOOL mute = FALSE;
    DWORD hw = 0;
    float step = 0;
    bool ok = SUCCEEDED(vol->GetMasterVolumeLevelScalar(&info->scalar)) && SUCCEEDED(vol->GetMasterVolumeLevel(&info->db));
    vol->GetVolumeRange(&info->minDb, &info->maxDb, &step);
    vol->GetMute(&mute);
    vol->QueryHardwareSupport(&hw);
    info->mute = mute != FALSE;
    info->hardware = (hw & ENDPOINT_HARDWARE_SUPPORT_VOLUME) != 0;
    vol->Release();
    return ok;
}

bool SetEndpointVolume(const wchar_t* deviceId, float scalar)
{
    IAudioEndpointVolume* vol = OpenEndpointVolume(deviceId);
    if (!vol) return false;
    bool ok = SUCCEEDED(vol->SetMasterVolumeLevelScalar(scalar, &kS2mVolumeContext));
    vol->Release();
    return ok;
}

HRESULT SetEndpointName(const wchar_t* deviceId, const wchar_t* name)
{
    IPolicyConfig* pc = OpenPolicyConfig();
    if (!pc) return E_NOINTERFACE;
    PROPVARIANT v;
    PropVariantInit(&v);
    v.vt = VT_LPWSTR;
    v.pwszVal = (LPWSTR)name;       // not owned: no PropVariantClear
    // Windows occasionally answers a rename by resetting a volume to 100 % within about a second (seen on the
    // microphone, +30 dB, also when the SPEAKER was renamed): watch every Speak2Mic endpoint and put it back.
    struct { wchar_t id[256], desc[256]; float db; bool fixed; } w[4];
    int nw = 0;
    static AudioDevice list[64];
    int n = ListAudioDevices(list, 64);
    float mn = 0, mx = 0;
    for (int i = 0; i < n && nw < 4; i++)
    {
        bool ours = list[i].adapter[0] ? _wcsicmp(list[i].adapter, L"Speak2Mic") == 0 : wcsstr(list[i].name, L"(Speak2Mic)") != nullptr;
        if (!ours || !GetEndpointVolumeDb(list[i].id, &w[nw].db, &mn, &mx)) continue;
        wcscpy(w[nw].id, list[i].id);
        wcscpy(w[nw].desc, list[i].desc);
        w[nw++].fixed = false;
    }
    HRESULT hr = pc->SetPropertyValue(deviceId, FALSE, kDeviceDesc, &v);
    pc->Release();
    for (int i = 0; nw && SUCCEEDED(hr) && i < 15; i++)
    {
        Sleep(100);
        for (int k = 0; k < nw; k++)
        {
            float now = 0;
            if (w[k].fixed || !GetEndpointVolumeDb(w[k].id, &now, &mn, &mx) || fabsf(now - w[k].db) <= 0.05f) continue;
            SetEndpointVolumeDb(w[k].id, w[k].db);
            w[k].fixed = true;
            AppLog(L"rename: Windows changed the volume of \"%ls\" to %.2f dB, set back to %.2f dB", w[k].desc, now, w[k].db);
        }
    }
    return hr;
}

HRESULT PolicySetFormat(const wchar_t* deviceId, const WAVEFORMATEX* device, const WAVEFORMATEX* mix)
{
    IPolicyConfig* pc = OpenPolicyConfig();
    if (!pc) return E_NOINTERFACE;
    HRESULT hr = pc->SetDeviceFormat(deviceId, (WAVEFORMATEX*)device, (WAVEFORMATEX*)mix);
    pc->Release();
    return hr;
}

HRESULT PolicyGetFormat(const wchar_t* deviceId, bool defaultFormat, WAVEFORMATEX** format)
{
    *format = nullptr;
    IPolicyConfig* pc = OpenPolicyConfig();
    if (!pc) return E_NOINTERFACE;
    HRESULT hr = pc->GetDeviceFormat(deviceId, defaultFormat ? TRUE : FALSE, format);
    pc->Release();
    return hr;
}

HRESULT PolicyGetMixFormat(const wchar_t* deviceId, WAVEFORMATEX** format)
{
    *format = nullptr;
    IPolicyConfig* pc = OpenPolicyConfig();
    if (!pc) return E_NOINTERFACE;
    HRESULT hr = pc->GetMixFormat(deviceId, format);
    pc->Release();
    return hr;
}

bool SetEndpointFormat(const wchar_t* deviceId, DWORD rate, WORD bits, WORD channels, wchar_t* err, size_t errLen)
{
    err[0] = 0;
    IPolicyConfig* pc = OpenPolicyConfig();
    if (!pc)
    {
        Copy(err, errLen, TR(L"системный интерфейс настройки формата недоступен"));
        return false;
    }

    // The current pair is put back if nothing is accepted (a failed set can leave the endpoint without
    // a usable format; ResetDeviceFormat did exactly that for the microphone).
    WAVEFORMATEX* oldDevice = nullptr;
    WAVEFORMATEX* oldMix = nullptr;
    pc->GetDeviceFormat(deviceId, FALSE, &oldDevice);
    pc->GetMixFormat(deviceId, &oldMix);

    // Ways to write the same bit depth: Windows is picky about which one it takes.
    struct Variant { WORD container; bool extensible; const wchar_t* name; };
    Variant variants[3];
    int count = 0;
    variants[count++] = { bits, true, L"extensible" };
    if (bits == 24) variants[count++] = { 32, true, L"24 in 32-bit container" };
    variants[count++] = { bits, false, L"plain WAVEFORMATEX" };

    WAVEFORMATEXTENSIBLE mix;
    MakeWaveFormat(&mix, rate, 32, 32, channels, true, true);   // the audio engine mixes in float
    HRESULT hr = E_FAIL, first = S_OK;
    const wchar_t* how = L"";
    for (int v = 0; v < count && FAILED(hr); v++)
    {
        WAVEFORMATEXTENSIBLE device;
        MakeWaveFormat(&device, rate, variants[v].container, bits, channels, false, variants[v].extensible);
        // Float mix (what the Sound control panel does), then mix = device format (older builds).
        hr = pc->SetDeviceFormat(deviceId, &device.Format, &mix.Format);
        if (SUCCEEDED(hr))
        {
            how = variants[v].name;
            break;
        }
        if (first == S_OK) first = hr;
        AppLog(L"SetDeviceFormat %u bit (%ls, float mix): 0x%08lX", bits, variants[v].name, (unsigned long)hr);
        hr = pc->SetDeviceFormat(deviceId, &device.Format, &device.Format);
        if (SUCCEEDED(hr))
        {
            how = variants[v].extensible ? L"extensible, mix = device format" : L"plain, mix = device format";
            break;
        }
        AppLog(L"SetDeviceFormat %u bit (%ls, mix = device): 0x%08lX", bits, variants[v].name, (unsigned long)hr);
    }

    bool restored = false;
    if (FAILED(hr) && oldDevice && oldMix)
    {
        restored = SUCCEEDED(pc->SetDeviceFormat(deviceId, oldDevice, oldMix));
    }
    if (oldDevice) CoTaskMemFree(oldDevice);
    if (oldMix) CoTaskMemFree(oldMix);
    pc->Release();
    if (FAILED(hr))
    {
        _snwprintf(err, errLen, restored ? TR(L"Windows не приняла формат (0x%08lX); прежний формат оставлен")
                                         : TR(L"Windows не приняла формат (0x%08lX)"), (unsigned long)first);
        err[errLen - 1] = 0;
        return false;
    }
    _snwprintf(err, errLen, L"%ls", how);   // on success: which variant worked
    err[errLen - 1] = 0;
    return true;
}

bool GetEndpointFormat(const wchar_t* deviceId, DWORD* rate, WORD* bits, WORD* channels)
{
    IPolicyConfig* pc = OpenPolicyConfig();
    if (!pc) return false;
    WAVEFORMATEX* f = nullptr;
    HRESULT hr = pc->GetDeviceFormat(deviceId, FALSE, &f);
    pc->Release();
    if (FAILED(hr) || !f) return false;
    *rate = f->nSamplesPerSec;
    *channels = f->nChannels;
    *bits = f->wBitsPerSample;
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && ((WAVEFORMATEXTENSIBLE*)f)->Samples.wValidBitsPerSample)
        *bits = ((WAVEFORMATEXTENSIBLE*)f)->Samples.wValidBitsPerSample;
    CoTaskMemFree(f);
    return true;
}

// ---------------------------------------------------------------------------
// Level meter

LevelMeter::LevelMeter()
    : m_thread(nullptr), m_stop(0), m_ended(0), m_channels(0)
{
    InitializeCriticalSection(&m_cs);
    ZeroMemory(&m_dev, sizeof(m_dev));
    ZeroMemory(m_peaks, sizeof(m_peaks));
    m_status[0] = 0;
}

LevelMeter::~LevelMeter()
{
    Stop();
    DeleteCriticalSection(&m_cs);
}

bool LevelMeter::Start(const AudioDevice& dev)
{
    Stop();
    m_dev = dev;
    m_stop = 0;
    m_ended = 0;
    m_channels = 0;
    ZeroMemory(m_peaks, sizeof(m_peaks));
    SetStatus(TR(L"Подключение…"));
    m_thread = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
    return m_thread != nullptr;
}

void LevelMeter::Stop()
{
    if (m_thread)
    {
        InterlockedExchange(&m_stop, 1);
        WaitForSingleObject(m_thread, 3000);
        CloseHandle(m_thread);
        m_thread = nullptr;
    }
}

int LevelMeter::TakePeaks(float* peaks, int max)
{
    EnterCriticalSection(&m_cs);
    int n = m_channels < max ? m_channels : max;
    for (int i = 0; i < n; i++)
    {
        peaks[i] = m_peaks[i];
        m_peaks[i] = 0;
    }
    LeaveCriticalSection(&m_cs);
    return n;
}

void LevelMeter::GetStatus(wchar_t* buf, size_t len)
{
    EnterCriticalSection(&m_cs);
    Copy(buf, len, m_status);
    LeaveCriticalSection(&m_cs);
}

void LevelMeter::SetStatus(const wchar_t* text)
{
    EnterCriticalSection(&m_cs);
    Copy(m_status, 160, text);
    LeaveCriticalSection(&m_cs);
}

DWORD WINAPI LevelMeter::ThreadProc(LPVOID param)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ((LevelMeter*)param)->Loop();
    CoUninitialize();
    return 0;
}

void LevelMeter::Loop()
{
    IMMDevice* dev = OpenDevice(m_dev.id);
    IAudioClient* client = nullptr;
    IAudioCaptureClient* capture = nullptr;
    WAVEFORMATEX* fmt = nullptr;
    wchar_t text[160];
    bool isFloat = false;
    int channels = 0, bytes = 0;
    HRESULT hr;

    if (!dev)
    {
        SetStatus(TR(L"Устройство не найдено"));
        goto done;
    }
    hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client);
    if (SUCCEEDED(hr)) hr = client->GetMixFormat(&fmt);
    if (SUCCEEDED(hr))
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, m_dev.flow == eRender ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0,
                                100 * kHnsPerMs, 0, fmt, nullptr);
    if (SUCCEEDED(hr)) hr = client->GetService(__uuidof(IAudioCaptureClient), (void**)&capture);
    if (SUCCEEDED(hr)) hr = client->Start();
    if (FAILED(hr))
    {
        _snwprintf(text, 160, TR(L"Нет доступа к устройству (0x%08lX)"), (unsigned long)hr);
        text[159] = 0;
        SetStatus(text);
        goto done;
    }

    isFloat = fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
              (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
               IsEqualGUID(((WAVEFORMATEXTENSIBLE*)fmt)->SubFormat, kSubtypeFloat));
    channels = fmt->nChannels < S2M_MAX_METER_CHANNELS ? fmt->nChannels : S2M_MAX_METER_CHANNELS;
    bytes = fmt->wBitsPerSample / 8;

    // Mix format = what the Windows audio engine uses for this device (set in Sound settings).
    _snwprintf(text, 160, TR(L"%lu Гц · %u бит%ls · %u кан."), (unsigned long)fmt->nSamplesPerSec,
               (unsigned)fmt->wBitsPerSample, isFloat ? L" float" : L"", (unsigned)fmt->nChannels);
    text[159] = 0;
    SetStatus(text);
    EnterCriticalSection(&m_cs);
    m_channels = channels;
    LeaveCriticalSection(&m_cs);

    while (!m_stop)
    {
        Sleep(15);
        UINT32 packet = 0;
        while (SUCCEEDED(hr = capture->GetNextPacketSize(&packet)) && packet > 0)
        {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
            {
                break;
            }

            float peak[S2M_MAX_METER_CHANNELS] = {};
            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT))
            {
                for (UINT32 f = 0; f < frames; f++)
                {
                    const BYTE* frame = data + (size_t)f * fmt->nBlockAlign;
                    for (int c = 0; c < channels; c++)
                    {
                        const BYTE* s = frame + c * bytes;
                        float v;
                        if (isFloat && bytes == 4)
                            v = fabsf(*(const float*)s);
                        else if (bytes == 2)
                            v = fabsf(*(const short*)s / 32768.0f);
                        else if (bytes == 3)
                            v = fabsf((float)(((int)s[0] << 8 | (int)s[1] << 16 | (int)s[2] << 24) >> 8) / 8388608.0f);
                        else if (bytes == 4)
                            v = fabsf(*(const int*)s / 2147483648.0f);
                        else
                            v = 0;
                        if (v > peak[c]) peak[c] = v;
                    }
                }
            }
            capture->ReleaseBuffer(frames);

            EnterCriticalSection(&m_cs);
            for (int c = 0; c < channels; c++)
            {
                if (peak[c] > m_peaks[c]) m_peaks[c] = peak[c];
            }
            LeaveCriticalSection(&m_cs);
        }
        if (FAILED(hr))
        {
            SetStatus(hr == AUDCLNT_E_DEVICE_INVALIDATED ? TR(L"Устройство отключено") : TR(L"Ошибка захвата"));
            break;
        }
    }

done:
    if (!m_stop) InterlockedExchange(&m_ended, 1);
    if (client) client->Stop();
    SafeRelease(capture);
    SafeRelease(client);
    SafeRelease(dev);
    if (fmt) CoTaskMemFree(fmt);
    EnterCriticalSection(&m_cs);
    m_channels = 0;
    LeaveCriticalSection(&m_cs);
}

// ---------------------------------------------------------------------------
// Channel test

// Mono float samples of an in-memory WAV (PCM 8/16/24/32 bit or 32-bit float); false if it cannot be used.
static bool ParseWavMono(const BYTE* data, DWORD size, float** out, UINT32* frames, UINT32* rate)
{
    if (!data || size <= 44 || memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0) return false;
    bool ok = true;
    const WAVEFORMATEX* fmt = nullptr;
    const BYTE* pcm = nullptr;
    DWORD pcmBytes = 0;
    for (DWORD pos = 12; pos + 8 <= size;)
    {
        DWORD len = *(const DWORD*)(data + pos + 4);
        if (memcmp(data + pos, "fmt ", 4) == 0 && len >= 16) fmt = (const WAVEFORMATEX*)(data + pos + 8);
        if (memcmp(data + pos, "data", 4) == 0) { pcm = data + pos + 8; pcmBytes = len; }
        pos += 8 + len + (len & 1);
    }
    ok = fmt && pcm && fmt->nChannels && fmt->nBlockAlign && fmt->nSamplesPerSec && pcm + pcmBytes <= data + size;
    WORD tag = fmt ? fmt->wFormatTag : 0;
    if (ok && tag == WAVE_FORMAT_EXTENSIBLE)
        tag = ((const WAVEFORMATEXTENSIBLE*)fmt)->SubFormat.Data1 == 3 ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
    int bytes = ok ? fmt->wBitsPerSample / 8 : 0;
    ok = ok && ((tag == WAVE_FORMAT_PCM && bytes >= 1 && bytes <= 4) || (tag == WAVE_FORMAT_IEEE_FLOAT && bytes == 4));
    if (ok)
    {
        *frames = pcmBytes / fmt->nBlockAlign;
        *rate = fmt->nSamplesPerSec;
        *out = (float*)HeapAlloc(GetProcessHeap(), 0, sizeof(float) * (*frames ? *frames : 1));
        ok = *out != nullptr;
    }
    for (UINT32 i = 0; ok && i < *frames; i++)
    {
        float sum = 0;
        for (int c = 0; c < fmt->nChannels; c++)
        {
            const BYTE* s = pcm + (size_t)i * fmt->nBlockAlign + c * bytes;
            float v;
            if (tag == WAVE_FORMAT_IEEE_FLOAT) v = *(const float*)s;
            else if (bytes == 1) v = (s[0] - 128) / 128.0f;
            else if (bytes == 2) v = *(const short*)s / 32768.0f;
            else if (bytes == 3) v = (float)(((int)s[0] << 8 | (int)s[1] << 16 | (int)s[2] << 24) >> 8) / 8388608.0f;
            else v = (float)(*(const int*)s / 2147483648.0);
            sum += v;
        }
        (*out)[i] = sum / fmt->nChannels;
    }
    return ok;
}

static bool LoadWavMono(const wchar_t* path, float** out, UINT32* frames, UINT32* rate)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(f, nullptr), got = 0;
    BYTE* data = size > 44 && size < 16 * 1024 * 1024 ? (BYTE*)HeapAlloc(GetProcessHeap(), 0, size) : nullptr;
    bool ok = data && ReadFile(f, data, size, &got, nullptr) && got == size;
    CloseHandle(f);
    ok = ok && ParseWavMono(data, size, out, frames, rate);
    if (data) HeapFree(GetProcessHeap(), 0, data);
    return ok;
}

// A WAVE resource of a system module (e.g. mmres.dll; on Windows 10 1903+ the loader finds the resources in
// SystemResources\<name>.mun).
static bool LoadWavResource(const wchar_t* module, WORD id, float** out, UINT32* frames, UINT32* rate)
{
    HMODULE h = LoadLibraryExW(module, nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!h) return false;
    HRSRC r = FindResourceW(h, MAKEINTRESOURCEW(id), L"WAVE");
    HGLOBAL g = r ? LoadResource(h, r) : nullptr;
    const BYTE* p = g ? (const BYTE*)LockResource(g) : nullptr;
    bool ok = p && ParseWavMono(p, SizeofResource(h, r), out, frames, rate);
    FreeLibrary(h);
    return ok;
}

// A short two-tone chime (fallback when the Windows sound is missing).
static void MakeChime(float** out, UINT32* frames, UINT32 rate)
{
    *frames = rate * 6 / 10;
    *out = (float*)HeapAlloc(GetProcessHeap(), 0, sizeof(float) * *frames);
    if (!*out) { *frames = 0; return; }
    for (UINT32 i = 0; i < *frames; i++)
    {
        double t = (double)i / rate;
        double f = t < 0.2 ? 880.0 : 1318.5;
        double env = exp(-(t < 0.2 ? t : t - 0.2) * 6.0) * (t < 0.2 ? 1.0 : 0.9);
        (*out)[i] = (float)(0.35 * env * sin(2.0 * 3.14159265358979 * f * t));
    }
}

HRESULT PlayChannelTest(const wchar_t* deviceId, volatile LONG* stop)
{
    bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    IMMDevice* dev = OpenDevice(deviceId);
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    WAVEFORMATEX* fmt = nullptr;
    // [0] odd channels (1st, 3rd, ...), [1] even channels: the melodies of Windows' own speaker test.
    float* sound[2] = {};
    UINT32 soundFrames[2] = {}, soundRate[2] = {}, bufferFrames = 0;
    HRESULT hr = dev ? dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client) : E_NOTFOUND;
    if (SUCCEEDED(hr)) hr = client->GetMixFormat(&fmt);
    if (SUCCEEDED(hr)) hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 200 * kHnsPerMs, 0, fmt, nullptr);
    if (SUCCEEDED(hr)) hr = client->GetBufferSize(&bufferFrames);
    if (SUCCEEDED(hr)) hr = client->GetService(__uuidof(IAudioRenderClient), (void**)&render);
    if (SUCCEEDED(hr))
    {
        for (int k = 0; k < 2; k++)
        {
            WORD id = k == 0 ? 3110 : 3111;
            if (LoadWavResource(L"mmres.dll", id, &sound[k], &soundFrames[k], &soundRate[k])) continue;
            wchar_t path[MAX_PATH];
            GetWindowsDirectoryW(path, MAX_PATH);
            wcsncat(path, L"\\Media\\Windows Background.wav", MAX_PATH - wcslen(path) - 1);
            AppLog(L"sound test: mmres.dll WAVE %u not found, trying %ls", id, path);
            if (!LoadWavMono(path, &sound[k], &soundFrames[k], &soundRate[k]))
            {
                soundRate[k] = fmt->nSamplesPerSec;
                MakeChime(&sound[k], &soundFrames[k], soundRate[k]);
            }
        }
        if (!sound[0] || !sound[1]) hr = E_OUTOFMEMORY;
    }
    if (SUCCEEDED(hr))
    {
        const int channels = fmt->nChannels;
        const int bytes = fmt->wBitsPerSample / 8;
        const bool isFloat = fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
            (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && IsEqualGUID(((WAVEFORMATEXTENSIBLE*)fmt)->SubFormat, kSubtypeFloat));
        // One pass per channel: its sound resampled to the device rate + 0.3 s of silence.
        double step[2];
        UINT64 soundOut[2], passFrames[2];
        for (int k = 0; k < 2; k++)
        {
            step[k] = (double)soundRate[k] / fmt->nSamplesPerSec;
            soundOut[k] = (UINT64)(soundFrames[k] / step[k]);
            passFrames[k] = soundOut[k] + fmt->nSamplesPerSec * 3 / 10;
        }
        UINT64 total = 0;
        for (int c = 0; c < channels; c++) total += passFrames[c & 1];
        UINT64 pos = 0;
        int active = 0;             // channel being tested
        UINT64 passStart = 0;       // output frame where its pass began
        AppLog(L"sound test: %d channel(s), %lu Hz", channels, (unsigned long)fmt->nSamplesPerSec);
        hr = client->Start();
        while (SUCCEEDED(hr) && !*stop)
        {
            UINT32 padding = 0;
            if (FAILED(hr = client->GetCurrentPadding(&padding))) break;
            if (pos >= total)
            {
                if (padding == 0) break;        // everything played
                Sleep(10);
                continue;
            }
            UINT32 n = bufferFrames - padding;
            BYTE* data = nullptr;
            if (n && SUCCEEDED(hr = render->GetBuffer(n, &data)))
            {
                for (UINT32 i = 0; i < n; i++, pos++)
                {
                    while (active < channels && pos - passStart >= passFrames[active & 1])
                    {
                        passStart += passFrames[active & 1];
                        active++;
                    }
                    const int w = active & 1;       // odd channel (1st, 3rd, ...) -> 0, even -> 1
                    UINT64 inPass = pos - passStart;
                    float v = 0;
                    if (active < channels && inPass < soundOut[w])
                    {
                        double x = inPass * step[w];
                        UINT32 k = (UINT32)x;
                        float a = sound[w][k], b = k + 1 < soundFrames[w] ? sound[w][k + 1] : 0.0f;
                        v = a + (b - a) * (float)(x - k);
                    }
                    for (int c = 0; c < channels; c++)
                    {
                        float s = (c == active) ? v : 0.0f;
                        BYTE* p = data + (size_t)i * fmt->nBlockAlign + c * bytes;
                        if (isFloat && bytes == 4) *(float*)p = s;
                        else if (bytes == 2) *(short*)p = (short)(s * 32767.0f);
                        else if (bytes == 4) *(int*)p = (int)(s * 2147483647.0);
                        else memset(p, 0, bytes);
                    }
                }
                hr = render->ReleaseBuffer(n, 0);
            }
            Sleep(10);
        }
        client->Stop();
    }
    if (FAILED(hr)) AppLog(L"sound test failed: 0x%08lX", (unsigned long)hr);
    for (float* p : sound)
        if (p) HeapFree(GetProcessHeap(), 0, p);
    SafeRelease(render);
    SafeRelease(client);
    SafeRelease(dev);
    if (fmt) CoTaskMemFree(fmt);
    if (com) CoUninitialize();
    return hr;
}


// ---------------------------------------------------------------------------
// Volume changes made outside Speak2Mic

// placement new (no C++ library headers in this build)
#ifndef __PLACEMENT_NEW_INLINE      // MSVC's <new> defines it
#define __PLACEMENT_NEW_INLINE
inline void* operator new(size_t, void* p) noexcept { return p; }
#endif

namespace {
class VolumeWatch final : public IAudioEndpointVolumeCallback
{
public:
    VolumeWatch(IAudioEndpointVolume* vol, ForeignVolumeFn fn, void* ctx) : m_vol(vol), m_refs(1), m_fn(fn), m_ctx(ctx) {}
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_refs); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG n = InterlockedDecrement(&m_refs);
        if (n == 0)
        {
            this->~VolumeWatch();       // no C++ runtime library in these programs: heap + placement new
            HeapFree(GetProcessHeap(), 0, this);
        }
        return n;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioEndpointVolumeCallback))
        {
            *ppv = static_cast<IAudioEndpointVolumeCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA d) override
    {
        if (!d || IsEqualGUID(d->guidEventContext, kS2mVolumeContext)) return S_OK;
        float db = -1000.0f;
        if (FAILED(m_vol->GetMasterVolumeLevel(&db)) && d->nChannels > 0) m_vol->GetChannelVolumeLevel(0, &db);
        m_fn(db, d->fMasterVolume, d->bMuted != FALSE, d->guidEventContext, m_ctx);
        return S_OK;
    }
    IAudioEndpointVolume* m_vol;

private:
    LONG            m_refs;
    ForeignVolumeFn m_fn;
    void*           m_ctx;
};
}

void* WatchEndpointVolume(const wchar_t* deviceId, ForeignVolumeFn fn, void* ctx)
{
    IAudioEndpointVolume* vol = OpenEndpointVolume(deviceId);
    if (!vol) return nullptr;
    void* mem = HeapAlloc(GetProcessHeap(), 0, sizeof(VolumeWatch));
    if (!mem)
    {
        vol->Release();
        return nullptr;
    }
    VolumeWatch* w = new (mem) VolumeWatch(vol, fn, ctx);
    if (FAILED(vol->RegisterControlChangeNotify(w)))
    {
        w->Release();
        vol->Release();
        return nullptr;
    }
    return w;
}

void UnwatchEndpointVolume(void* watch)
{
    if (!watch) return;
    VolumeWatch* w = (VolumeWatch*)watch;
    IAudioEndpointVolume* vol = w->m_vol;
    vol->UnregisterControlChangeNotify(w);
    w->Release();
    vol->Release();
}

int HoldEndpointSettings(EndpointHold* holds, int n, DWORD ms)
{
    int fixes = 0;
    DWORD t0 = GetTickCount();
    for (;;)
    {
        for (int i = 0; i < n; i++)
        {
            EndpointHold& h = holds[i];
            DWORD r = 0;
            WORD b = 0, c = 0;
            if (h.format && GetEndpointFormat(h.id, &r, &b, &c) && (r != h.rate || b != h.bits || c != h.channels))
            {
                wchar_t err[160];
                bool ok = SetEndpointFormat(h.id, h.rate, h.bits, h.channels, err, 160);
                AppLog(L"after the restart Windows put back the default format %lu Hz %u bit %u ch: set %lu Hz %u bit %u ch again (%ls)",
                       r, b, c, h.rate, h.bits, h.channels, ok ? L"ok" : err);
                fixes++;
            }
            float db = 0, mn = 0, mx = 0;
            bool mute = false;
            if (h.level && GetEndpointVolumeDb(h.id, &db, &mn, &mx) && fabsf(db - h.db) > 0.05f)
            {
                SetEndpointVolumeDb(h.id, h.db);
                AppLog(L"after the restart Windows put back the volume %.2f dB: set %.2f dB again", db, h.db);
                fixes++;
            }
            if (h.level && GetEndpointMute(h.id, &mute) && mute != h.mute)
            {
                SetEndpointMute(h.id, h.mute);
                AppLog(L"after the restart Windows put back mute %ls: set %ls again", mute ? L"on" : L"off", h.mute ? L"on" : L"off");
                fixes++;
            }
        }
        if (GetTickCount() - t0 >= ms) return fixes;
        Sleep(250);
    }
}

void DescribeAudioSessions(const wchar_t* deviceId, wchar_t* out, size_t len)
{
    out[0] = 0;
    size_t n = 0;
    IMMDevice* dev = OpenDevice(deviceId);
    IAudioSessionManager2* mgr = nullptr;
    IAudioSessionEnumerator* en = nullptr;
    if (dev && SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&mgr)) &&
        SUCCEEDED(mgr->GetSessionEnumerator(&en)))
    {
        int count = 0;
        en->GetCount(&count);
        for (int i = 0; i < count && n + 80 < len; i++)
        {
            IAudioSessionControl* c = nullptr;
            IAudioSessionControl2* c2 = nullptr;
            if (FAILED(en->GetSession(i, &c))) continue;
            if (SUCCEEDED(c->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&c2)))
            {
                DWORD pid = 0;
                AudioSessionState st = AudioSessionStateInactive;
                c2->GetProcessId(&pid);
                c2->GetState(&st);
                wchar_t exe[MAX_PATH] = L"?";
                HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                if (h)
                {
                    DWORD sz = MAX_PATH;
                    if (QueryFullProcessImageNameW(h, 0, exe, &sz))
                    {
                        wchar_t* slash = wcsrchr(exe, L'\\');
                        if (slash) memmove(exe, slash + 1, (wcslen(slash + 1) + 1) * sizeof(wchar_t));
                    }
                    CloseHandle(h);
                }
                else if (pid == 0) wcscpy(exe, L"system");
                else
                {
                    // No access (protected process, other user): the process list still has its name.
                    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
                    PROCESSENTRY32W pe = { sizeof(pe) };
                    if (snap != INVALID_HANDLE_VALUE)
                    {
                        for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe))
                            if (pe.th32ProcessID == pid) { wcsncpy(exe, pe.szExeFile, MAX_PATH - 1); break; }
                        CloseHandle(snap);
                    }
                }
                // The session identifier names the program (path or app id) even when the process is gone or closed to us:
                // "{endpoint}|\Device\HarddiskVolumeN\...\app.exe%b{...}" - keep the part between "|" and "%b".
                wchar_t ident[200] = L"";
                LPWSTR sid = nullptr;
                if (wcscmp(exe, L"?") == 0 && SUCCEEDED(c2->GetSessionIdentifier(&sid)) && sid)
                {
                    const wchar_t* bar = wcschr(sid, L'|');
                    const wchar_t* from = bar ? bar + 1 : sid;
                    const wchar_t* to = wcsstr(from, L"%b");
                    size_t k = to ? (size_t)(to - from) : wcslen(from);
                    if (k > 199) { from += k - 199; k = 199; }   // the end (file name) matters most
                    wcsncpy(ident, from, k);
                    ident[k] = 0;
                    CoTaskMemFree(sid);
                }
                if (ident[0]) { wcsncpy(exe, ident, MAX_PATH - 1); exe[MAX_PATH - 1] = 0; }
                int w = _snwprintf(out + n, len - n, L"%ls%ls (pid %lu, %ls)", n ? L"; " : L"", exe, pid,
                                   st == AudioSessionStateActive ? L"active" : st == AudioSessionStateExpired ? L"expired" : L"inactive");
                if (w > 0) n += w;
                ISimpleAudioVolume* sv = nullptr;
                float level = 1.0f;
                BOOL muted = FALSE;
                if (SUCCEEDED(c->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&sv)))
                {
                    sv->GetMasterVolume(&level);
                    sv->GetMute(&muted);
                    sv->Release();
                    if ((level < 0.999f || muted) && n + 40 < len)
                    {
                        // a session turned down or muted (Volume mixer, ducking): part of the description
                        n -= 1;     // before the closing ")"
                        w = _snwprintf(out + n, len - n, L", session volume %d %%%ls)", (int)(level * 100.0f + 0.5f), muted ? L" MUTED" : L"");
                        if (w > 0) n += w;
                    }
                }
                c2->Release();
            }
            c->Release();
        }
    }
    if (en) en->Release();
    if (mgr) mgr->Release();
    if (dev) dev->Release();
    if (!out[0]) _snwprintf(out, len, L"(none)");
    out[len - 1] = 0;
}

void PrepareTestSession(IAudioClient* client, wchar_t* info, size_t len)
{
    info[0] = 0;
    size_t n = 0;
    IAudioSessionControl* c = nullptr;
    if (SUCCEEDED(client->GetService(__uuidof(IAudioSessionControl), (void**)&c)))
    {
        IAudioSessionControl2* c2 = nullptr;
        if (SUCCEEDED(c->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&c2)))
        {
            c2->SetDuckingPreference(TRUE);
            c2->Release();
        }
        c->Release();
    }
    ISimpleAudioVolume* sv = nullptr;
    if (SUCCEEDED(client->GetService(__uuidof(ISimpleAudioVolume), (void**)&sv)))
    {
        float level = 1.0f;
        BOOL muted = FALSE;
        sv->GetMasterVolume(&level);
        sv->GetMute(&muted);
        if (level < 0.999f)
        {
            int w = _snwprintf(info + n, len - n, L"session volume was %d %%, set to 100 %%; ", (int)(level * 100.0f + 0.5f));
            if (w > 0) n += w;
            sv->SetMasterVolume(1.0f, &kS2mVolumeContext);
        }
        if (muted)
        {
            _snwprintf(info + n, len - n, L"session was muted, unmuted; ");
            sv->SetMute(FALSE, &kS2mVolumeContext);
        }
        sv->Release();
    }
    info[len - 1] = 0;
}

// exe file name of a process ("" if unknown): QueryFullProcessImageName, else the process list (works without access)
static void ProcessExeName(DWORD pid, wchar_t* out, size_t len)
{
    out[0] = 0;
    wchar_t path[MAX_PATH] = L"";
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h)
    {
        DWORD sz = MAX_PATH;
        if (!QueryFullProcessImageNameW(h, 0, path, &sz)) path[0] = 0;
        CloseHandle(h);
    }
    if (!path[0])
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32W pe = { sizeof(pe) };
        if (snap != INVALID_HANDLE_VALUE)
        {
            for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe))
                if (pe.th32ProcessID == pid) { wcsncpy(path, pe.szExeFile, MAX_PATH - 1); break; }
            CloseHandle(snap);
        }
    }
    const wchar_t* name = wcsrchr(path, L'\\');
    wcsncpy(out, name ? name + 1 : path, len - 1);
    out[len - 1] = 0;
}

void ActiveSessionPrograms(const wchar_t* deviceId, wchar_t* out, size_t len)
{
    out[0] = 0;
    size_t n = 0;
    IMMDevice* dev = OpenDevice(deviceId);
    IAudioSessionManager2* mgr = nullptr;
    IAudioSessionEnumerator* en = nullptr;
    if (dev && SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&mgr)) &&
        SUCCEEDED(mgr->GetSessionEnumerator(&en)))
    {
        int count = 0;
        en->GetCount(&count);
        for (int i = 0; i < count; i++)
        {
            IAudioSessionControl* c = nullptr;
            IAudioSessionControl2* c2 = nullptr;
            if (FAILED(en->GetSession(i, &c))) continue;
            DWORD pid = 0;
            AudioSessionState st = AudioSessionStateInactive;
            if (SUCCEEDED(c->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&c2)))
            {
                c2->GetProcessId(&pid);
                c2->GetState(&st);
                c2->Release();
            }
            c->Release();
            if (st != AudioSessionStateActive || pid == 0 || pid == GetCurrentProcessId()) continue;
            wchar_t name[MAX_PATH];
            ProcessExeName(pid, name, MAX_PATH);
            if (!name[0]) _snwprintf(name, MAX_PATH, L"pid %lu", pid);
            if (wcsstr(out, name)) continue;            // one entry per program
            int w = _snwprintf(out + n, len - n, L"%ls%ls", n ? L", " : L"", name);
            if (w < 0) break;
            n += w;
        }
    }
    if (en) en->Release();
    if (mgr) mgr->Release();
    if (dev) dev->Release();
    out[len - 1] = 0;
}
