// WaveRT miniport: the streaming filter of "Speak2Mic Speaker" (render) or "Speak2Mic Microphone" (capture).
#pragma once

#include "common.h"
#include "cable.h"

// Pin layout of the wave filters.
//   render : pin 0 = streaming sink (from the audio engine), pin 1 = bridge to topology
//   capture: pin 0 = bridge from topology,                   pin 1 = streaming source (to the engine)
#define S2M_WAVE_RENDER_STREAM_PIN   0
#define S2M_WAVE_RENDER_BRIDGE_PIN   1
#define S2M_WAVE_CAPTURE_BRIDGE_PIN  0
#define S2M_WAVE_CAPTURE_STREAM_PIN  1

class CMiniportWaveRT : public IMiniportWaveRT, public CUnknown
{
public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CMiniportWaveRT);
    ~CMiniportWaveRT();

    IMP_IMiniportWaveRT;

    void Setup(_In_ CCable* Cable, _In_ BOOLEAN Capture);

    // Same rules for KSPROPERTY_PIN_PROPOSEDATAFORMAT, NewStream and SetFormat: the cable's sample rate,
    // at most the cable's channels, and the configured bit depth (16-32 when it is "auto").
    ULONG StreamPin() const { return m_capture ? S2M_WAVE_CAPTURE_STREAM_PIN : S2M_WAVE_RENDER_STREAM_PIN; }
    const char* Name() const { return m_name; }   // "Cable 1 render" (for the driver log)
    void LogRepeatable(_In_z_ const char* Line);  // driver log line, identical repeats are only counted

    NTSTATUS CheckFormat(_In_ PKSDATAFORMAT DataFormat, _In_ ULONG BufferSize, _Out_opt_ S2M_FORMAT* Format, _In_ const char* Who);

private:
    CCable*             m_cable = nullptr;
    BOOLEAN             m_capture = FALSE;
    PPORTWAVERT         m_port = nullptr;
    BOOLEAN             m_described = FALSE;
    char                m_name[32] = "";
    KSPIN_LOCK          m_logLock = 0;
    char                m_lastLog[200] = "";
    ULONG               m_logRepeats = 0;

    PCFILTER_DESCRIPTOR     m_filter;
    PCPIN_DESCRIPTOR        m_pins[2];
    PCCONNECTION_DESCRIPTOR m_connections[1];
    GUID                    m_categories[3];
    KSDATARANGE_AUDIO       m_streamRange;
    PKSDATARANGE            m_streamRanges[1];
    KSDATARANGE             m_bridgeRange;
    PKSDATARANGE            m_bridgeRanges[1];
};

class CMiniportWaveRTStream : public IMiniportWaveRTStream, public CUnknown
{
public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CMiniportWaveRTStream);
    ~CMiniportWaveRTStream();

    IMP_IMiniportWaveRTStream;

    NTSTATUS Init(_In_ CCable* Cable, _In_ BOOLEAN Capture, _In_ PPORTWAVERTSTREAM PortStream, _In_ const S2M_FORMAT& Format);

private:
    static VOID NTAPI TimerCallback(_In_ PEX_TIMER Timer, _In_opt_ PVOID Context);
    void UpdateLocked(_In_ ULONGLONG Qpc);
    void ReleaseBuffer();

    CCable*             m_cable = nullptr;
    BOOLEAN             m_capture = FALSE;
    PPORTWAVERTSTREAM   m_portStream = nullptr;
    S2M_FORMAT           m_format;

    KSPIN_LOCK          m_lock;
    KSSTATE             m_state = KSSTATE_STOP;
    PEX_TIMER           m_timer = nullptr;

    PMDL                m_mdl = nullptr;
    BYTE*               m_buffer = nullptr;
    ULONG               m_bufferBytes = 0;
    ULONG               m_bufferFrames = 0;

    ULONGLONG           m_qpcFreq = 1;
    ULONGLONG           m_runStartQpc = 0;      // QPC when the stream last entered RUN
    ULONGLONG           m_framesAtRunStart = 0; // m_framesDone at that moment
    ULONGLONG           m_framesDone = 0;       // frames moved between the buffer and the cable
    ULONGLONG           m_cableBase = 0;        // absolute cable frame = m_cableBase + m_framesDone
};
