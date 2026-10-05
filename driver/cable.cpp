// CCable implementation: ring buffer, sample format conversion.
#include "cable.h"
#include "voltable.h"

// ---------------------------------------------------------------------------
// Sample conversion. The ring keeps left-justified signed 32-bit samples.

static inline LONG LoadSample(_In_ const BYTE* p, _In_ ULONG bytes)
{
    switch (bytes)
    {
    case 2:  return (LONG)(*(const SHORT UNALIGNED*)p) * 65536;
    case 3:  return (LONG)(((ULONG)p[0] << 8) | ((ULONG)p[1] << 16) | ((ULONG)p[2] << 24));
    default: return *(const LONG UNALIGNED*)p;
    }
}

static inline void StoreSample(_Out_ BYTE* p, _In_ ULONG bytes, _In_ LONG v)
{
    switch (bytes)
    {
    case 2:
        *(SHORT UNALIGNED*)p = (SHORT)(v >> 16);
        break;
    case 3:
        p[0] = (BYTE)((ULONG)v >> 8);
        p[1] = (BYTE)((ULONG)v >> 16);
        p[2] = (BYTE)((ULONG)v >> 24);
        break;
    default:
        *(LONG UNALIGNED*)p = v;
        break;
    }
}

// ---------------------------------------------------------------------------

NTSTATUS CCable::Create(_In_ ULONG Index, _In_ const S2M_CONFIG& Config, _Out_ CCable** Cable)
{
    *Cable = nullptr;

    CCable* c = new(POOL_FLAG_NON_PAGED, S2M_POOLTAG) CCable();
    if (!c)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    c->m_index = Index;
    c->m_rate = Config.SampleRate;
    // The ring holds the wider side: a mono source fills every channel, a wider source is downmixed or
    // truncated for a narrower microphone (see StoreFrames/Read).
    c->m_renderChannels = Config.Channels;
    c->m_captureChannels = Config.MicChannels;
    c->m_channels = Config.Channels > Config.MicChannels ? Config.Channels : Config.MicChannels;
    c->m_bits = Config.BitsPerSample;
    c->m_latencyFrames = (ULONG)((ULONGLONG)Config.SampleRate * Config.LatencyMs / 1000);
    c->m_ringFrames = Config.SampleRate;    // one second of audio
    for (ULONG ch = 0; ch < 8; ch++)
    {
        // where the volume node was left (0 dB the first time); Windows may set it again when the endpoint starts
        c->SetVolume(ch, Config.MicVolume);
        c->SetMute(ch, Config.MicMute != 0);
    }
    KeInitializeSpinLock(&c->m_lock);

    LARGE_INTEGER freq;
    LARGE_INTEGER now = KeQueryPerformanceCounter(&freq);
    c->m_qpcBase = (ULONGLONG)now.QuadPart;
    c->m_qpcFreq = (ULONGLONG)freq.QuadPart;

    SIZE_T bytes = (SIZE_T)c->m_ringFrames * c->m_channels * sizeof(LONG);
    c->m_ring = (LONG*)ExAllocatePool2(POOL_FLAG_NON_PAGED, bytes, S2M_POOLTAG);
    if (!c->m_ring)
    {
        delete c;
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    *Cable = c;
    return STATUS_SUCCESS;
}

CCable::~CCable()
{
    if (m_ring)
    {
        ExFreePoolWithTag(m_ring, S2M_POOLTAG);
    }
}

void CCable::AddRef()
{
    InterlockedIncrement(&m_refs);
}

void CCable::Release()
{
    if (InterlockedDecrement(&m_refs) == 0)
    {
        delete this;
    }
}

ULONGLONG CCable::FrameAt(_In_ ULONGLONG Qpc) const
{
    ULONGLONG ticks = Qpc > m_qpcBase ? Qpc - m_qpcBase : 0;
    return S2mTicksToFrames(ticks, m_qpcFreq, m_rate);
}

void CCable::RenderStart(_In_ ULONGLONG Start)
{
    KIRQL irql;
    KeAcquireSpinLock(&m_lock, &irql);
    m_writeStart = Start;
    m_writeHead = Start;
    KeReleaseSpinLock(&m_lock, irql);
}

// Converts and stores Frames frames at absolute frame Frame; Src == nullptr stores silence.
// Caller holds m_lock and guarantees Frames <= m_ringFrames.
void CCable::StoreFrames(_In_ ULONGLONG Frame, _In_opt_ const BYTE* Src, _In_ ULONG Frames, _In_ const S2M_FORMAT& Format)
{
    const ULONG cch = m_channels;
    const ULONG sch = Format.Channels;
    const ULONG bytes = Format.ContainerBytes;

    ULONG pos = (ULONG)(Frame % m_ringFrames);
    for (ULONG i = 0; i < Frames; i++)
    {
        LONG* dst = m_ring + (SIZE_T)pos * cch;
        if (!Src)
        {
            RtlZeroMemory(dst, cch * sizeof(LONG));
        }
        else
        {
            const BYTE* s = Src + (SIZE_T)i * Format.BlockAlign;
            for (ULONG c = 0; c < cch; c++)
            {
                if (c < sch)
                    dst[c] = LoadSample(s + c * bytes, bytes);
                else if (sch == 1)
                    dst[c] = dst[0];                // mono source feeds every cable channel
                else
                    dst[c] = 0;
            }
        }
        if (++pos == m_ringFrames)
        {
            pos = 0;
        }
    }
}

void CCable::Write(_In_ ULONGLONG Frame, _In_reads_(Frames) const BYTE* Src, _In_ ULONG Frames, _In_ const S2M_FORMAT& Format)
{
    if (Frames == 0)
    {
        return;
    }

    KIRQL irql;
    KeAcquireSpinLock(&m_lock, &irql);

    // Only the newest m_ringFrames frames can be kept anyway.
    if (Frames > m_ringFrames)
    {
        ULONG skip = Frames - m_ringFrames;
        Src += (SIZE_T)skip * Format.BlockAlign;
        Frame += skip;
        Frames = m_ringFrames;
    }

    // A gap after the previous write holds stale data from an earlier lap: silence it.
    if (Frame > m_writeHead && m_writeHead >= m_writeStart)
    {
        ULONGLONG gap = Frame - m_writeHead;
        if (gap > m_ringFrames)
        {
            gap = m_ringFrames;
        }
        StoreFrames(Frame - gap, nullptr, (ULONG)gap, Format);
    }

    StoreFrames(Frame, Src, Frames, Format);
    m_writeChannels = Format.Channels < m_channels ? Format.Channels : m_channels;

    if (Frame + Frames > m_writeHead)
    {
        m_writeHead = Frame + Frames;
    }

    KeReleaseSpinLock(&m_lock, irql);
}

// ---------------------------------------------------------------------------
// Microphone volume / mute

void CCable::UpdateGain(_In_ ULONG Channel)
{
    InterlockedExchange(&m_gainQ24[Channel], m_mute[Channel] ? 0 : (LONG)g_S2mVolumeGainQ24[m_volIndex[Channel]]);
}

void CCable::SetVolume(_In_ ULONG Channel, _In_ LONG Level)
{
    if (Channel >= 8) return;
    if (Level < S2M_VOL_MIN) Level = S2M_VOL_MIN;
    if (Level > S2M_VOL_MAX) Level = S2M_VOL_MAX;
    InterlockedExchange(&m_volIndex[Channel], (Level - S2M_VOL_MIN + S2M_VOL_STEP / 2) / S2M_VOL_STEP);
    UpdateGain(Channel);
}

LONG CCable::Volume(_In_ ULONG Channel) const
{
    return Channel < 8 ? S2M_VOL_MIN + m_volIndex[Channel] * S2M_VOL_STEP : 0;
}

void CCable::SetMute(_In_ ULONG Channel, _In_ BOOL Mute)
{
    if (Channel >= 8) return;
    InterlockedExchange(&m_mute[Channel], Mute ? 1 : 0);
    UpdateGain(Channel);
}

BOOL CCable::Mute(_In_ ULONG Channel) const
{
    return Channel < 8 ? m_mute[Channel] != 0 : FALSE;
}

inline LONG CCable::ApplyGain(_In_ ULONG Channel, _In_ LONGLONG Sample) const
{
    LONG g = Channel < 8 ? m_gainQ24[Channel] : (1 << 24);
    LONGLONG v = g == (1 << 24) ? Sample : (Sample * g) >> 24;
    if (v > 0x7FFFFFFF) v = 0x7FFFFFFF;
    if (v < -0x7FFFFFFFLL - 1) v = -0x7FFFFFFFLL - 1;
    return (LONG)v;
}

void CCable::Read(_In_ ULONGLONG Frame, _Out_writes_(Frames) BYTE* Dst, _In_ ULONG Frames, _In_ const S2M_FORMAT& Format)
{
    const ULONG cch = m_channels;
    const ULONG sch = Format.Channels;
    const ULONG bytes = Format.ContainerBytes;

    KIRQL irql;
    KeAcquireSpinLock(&m_lock, &irql);

    // Source channels in the ring: a mono render stream was copied to every channel (treat it as 1:1).
    const ULONG wch = m_writeChannels <= 1 ? 1 : m_writeChannels;

    // Valid frames: [lo, hi)
    ULONGLONG hi = m_writeHead;
    ULONGLONG lo = m_writeStart;
    if (hi > m_ringFrames && hi - m_ringFrames > lo)
    {
        lo = hi - m_ringFrames;
    }

    for (ULONG i = 0; i < Frames; i++)
    {
        ULONGLONG f = Frame + i;
        BYTE* d = Dst + (SIZE_T)i * Format.BlockAlign;

        if (f < lo || f >= hi)
        {
            RtlZeroMemory(d, Format.BlockAlign);
            continue;
        }

        const LONG* src = m_ring + (SIZE_T)(f % m_ringFrames) * cch;
        if (wch <= sch)
        {
            // The microphone has at least as many channels as the source: 1:1 (a mono source was already
            // copied to every ring channel by StoreFrames), missing ones silent.
            for (ULONG c = 0; c < sch; c++)
                StoreSample(d + c * bytes, bytes, c < cch ? ApplyGain(c, src[c]) : 0);
            continue;
        }

        // Fewer microphone channels than the source: every source channel ends up somewhere.
        LONGLONG acc[8] = {};
        if (sch == 1)
        {
            for (ULONG c = 0; c < wch; c++) acc[0] += src[c];              // mono: all channels, x 0.5
            acc[0] /= 2;                                                     // (stereo: the average of L and R)
        }
        else if (sch == 2)
        {
            // Stereo: WAVEFORMATEXTENSIBLE order FL FR C LFE BL BR SL SR (4 ch: FL FR BL BR).
            for (ULONG c = 0; c < wch; c++)
            {
                ULONG pos = (wch == 4 && c >= 2) ? c + 2 : c;               // 4 ch: BL BR at positions 4/5
                if (pos == 2 || pos == 3)
                {
                    LONGLONG half = (LONGLONG)src[c] * 181 / 256;          // centre, LFE: both sides x 0.707
                    acc[0] += half;
                    acc[1] += half;
                }
                else
                {
                    acc[(pos == 0 || pos == 4 || pos == 6) ? 0 : 1] += src[c];
                }
            }
        }
        else
        {
            for (ULONG c = 0; c < sch; c++) acc[c] = src[c];
            for (ULONG c = sch; c < wch; c++) acc[c % sch] += (LONGLONG)src[c] * 181 / 256;
        }
        for (ULONG c = 0; c < sch && c < 8; c++)
            StoreSample(d + c * bytes, bytes, ApplyGain(c, acc[c]));     // clamps too
    }

    KeReleaseSpinLock(&m_lock, irql);
}
