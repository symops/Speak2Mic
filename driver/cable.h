// CCable: the shared ring buffer that connects "Speak2Mic Speaker" (render) to "Speak2Mic Microphone" (capture).
//
// Both streams address the ring by an absolute frame index derived from the performance counter
// (see Now()). The render stream writes frames at its absolute position, the capture stream reads
// frames LatencyFrames behind the current time. Frames that were never written (render stopped,
// not started yet, or too old) read back as silence, so the capture side never stalls.
#pragma once

#include "common.h"

class CCable
{
public:
    static NTSTATUS Create(_In_ ULONG Index, _In_ const S2M_CONFIG& Config, _Out_ CCable** Cable);

    void AddRef();
    void Release();

    ULONG Index() const         { return m_index; }
    ULONG SampleRate() const    { return m_rate; }
    ULONG Channels() const      { return m_channels; }          // samples per frame in the ring
    ULONG RenderChannels() const  { return m_renderChannels; }  // most channels "Speak2Mic Speaker" takes
    ULONG CaptureChannels() const { return m_captureChannels; } // most channels "Speak2Mic Microphone" gives
    ULONG BitsPerSample() const { return m_bits; }
    ULONG LatencyFrames() const { return m_latencyFrames; }
    ULONGLONG QpcFrequency() const { return m_qpcFreq; }

    // Absolute cable frame for a performance counter value.
    ULONGLONG FrameAt(_In_ ULONGLONG Qpc) const;

    // Render side: a new render run begins at absolute frame Start.
    void RenderStart(_In_ ULONGLONG Start);
    void Write(_In_ ULONGLONG Frame, _In_reads_(Frames) const BYTE* Src, _In_ ULONG Frames, _In_ const S2M_FORMAT& Format);

    // Capture side: fills Frames frames starting at absolute frame Frame (silence where no data).
    void Read(_In_ ULONGLONG Frame, _Out_writes_(Frames) BYTE* Dst, _In_ ULONG Frames, _In_ const S2M_FORMAT& Format);

    // Microphone volume / mute per capture channel (the capture topology's volume and mute nodes), applied by
    // Read(). Level in KS units (1/65536 dB), stepped and clamped to S2M_VOL_MIN..S2M_VOL_MAX (voltable.h).
    void SetVolume(_In_ ULONG Channel, _In_ LONG Level);
    LONG Volume(_In_ ULONG Channel) const;
    void SetMute(_In_ ULONG Channel, _In_ BOOL Mute);
    BOOL Mute(_In_ ULONG Channel) const;

private:
    CCable() {}
    ~CCable();

    void StoreFrames(_In_ ULONGLONG Frame, _In_opt_ const BYTE* Src, _In_ ULONG Frames, _In_ const S2M_FORMAT& Format);

    LONG        m_refs = 1;
    ULONG       m_index = 0;
    ULONG       m_rate = 0;
    ULONG       m_channels = 0;         // max(render, capture)
    ULONG       m_renderChannels = 0;
    ULONG       m_captureChannels = 0;
    ULONG       m_bits = 0;
    ULONG       m_latencyFrames = 0;
    ULONGLONG   m_qpcBase = 0;
    ULONGLONG   m_qpcFreq = 1;

    KSPIN_LOCK  m_lock;
    LONG*       m_ring = nullptr;       // m_ringFrames * m_channels samples, left-justified 32-bit PCM
    ULONG       m_ringFrames = 0;
    ULONGLONG   m_writeStart = 0;       // first valid frame of the current render run
    ULONGLONG   m_writeHead = 0;        // one past the newest written frame
    ULONG       m_writeChannels = 0;    // channels of the render stream that filled the ring (1 = copied to all)

    void UpdateGain(_In_ ULONG Channel);
    LONG ApplyGain(_In_ ULONG Channel, _In_ LONGLONG Sample) const;

    volatile LONG m_volIndex[8] = {};   // step index into g_S2mVolumeGainQ24 (Create: 0 dB)
    volatile LONG m_mute[8] = {};
    volatile LONG m_gainQ24[8] = {};    // what Read() multiplies with: 0 when muted, 1 << 24 = unchanged
};
