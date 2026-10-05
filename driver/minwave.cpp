// WaveRT miniport and stream.
//
// The stream has no hardware behind it: its position advances with the performance counter at
// the cable's sample rate. Every time the position is updated (on GetPosition calls from the audio
// engine and on a 5 ms timer) the frames that became "played" are copied from the render buffer
// into the cable, or the frames that became "recorded" are copied from the cable into the capture
// buffer. Data is therefore moved exactly when the engine is told it has been consumed/produced.
#include "minwave.h"
#include "log.h"

#define S2M_TIMER_PERIOD_100NS   (5 * 10000LL)   // 5 ms

// ---------------------------------------------------------------------------
// Format parsing

NTSTATUS S2mParseFormat(_In_ PKSDATAFORMAT DataFormat, _Out_ S2M_FORMAT* Format)
{
    RtlZeroMemory(Format, sizeof(*Format));

    if (DataFormat->FormatSize < S2M_MIN_WAVE_FORMAT ||
        !IsEqualGUIDAligned(DataFormat->MajorFormat, KSDATAFORMAT_TYPE_AUDIO) ||
        !IsEqualGUIDAligned(DataFormat->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX))
    {
        return STATUS_NO_MATCH;
    }

    WAVEFORMATEX* wfx = &((PKSDATAFORMAT_WAVEFORMATEX)DataFormat)->WaveFormatEx;

    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
    {
        if (DataFormat->FormatSize < sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEXTENSIBLE) ||
            !IsEqualGUIDAligned(((WAVEFORMATEXTENSIBLE*)wfx)->SubFormat, KSDATAFORMAT_SUBTYPE_PCM))
        {
            return STATUS_NO_MATCH;
        }
    }
    else if (wfx->wFormatTag != WAVE_FORMAT_PCM)
    {
        S2mLog("Format rejected: tag=0x%04X (not PCM)", wfx->wFormatTag);
        return STATUS_NO_MATCH;
    }

    ULONG bytes = wfx->wBitsPerSample / 8;
    if (wfx->wBitsPerSample % 8 != 0 || bytes < 2 || bytes > 4 ||
        wfx->nChannels < 1 || wfx->nChannels > 8 ||
        wfx->nBlockAlign != wfx->nChannels * bytes ||
        wfx->nSamplesPerSec == 0)
    {
        S2mLog("Format rejected: tag=0x%04X rate=%lu bits=%u channels=%u align=%u",
               wfx->wFormatTag, wfx->nSamplesPerSec, wfx->wBitsPerSample, wfx->nChannels, wfx->nBlockAlign);
        return STATUS_NO_MATCH;
    }

    Format->SampleRate = wfx->nSamplesPerSec;
    Format->Channels = wfx->nChannels;
    Format->ContainerBytes = bytes;
    Format->BlockAlign = wfx->nBlockAlign;
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// Streaming pin properties

// KSPROPTYPESETID_General
static const GUID kPropTypeSetGeneral = { 0x97E99BA0, 0xBDEA, 0x11CF, { 0xA5, 0xD6, 0x28, 0xDB, 0x04, 0xC1, 0x00, 0x00 } };

ULONG g_S2mProposeFormat = 1;

extern "C" int __cdecl _vsnprintf(char* buffer, size_t count, const char* format, va_list args);

static void S2mPrintf(_Out_writes_(Size) char* Out, _In_ ULONG Size, _In_z_ _Printf_format_string_ const char* Format, ...)
{
    va_list args;
    va_start(args, Format);
    int n = _vsnprintf(Out, Size - 1, Format, args);
    va_end(args);
    Out[(n < 0 || (ULONG)n >= Size) ? Size - 1 : (ULONG)n] = 0;
}

void S2mFormatText(_In_ PKSDATAFORMAT DataFormat, _In_ ULONG BufferSize, _Out_writes_(Size) char* Out, _In_ ULONG Size)
{
    if (!DataFormat || BufferSize < sizeof(KSDATAFORMAT))
    {
        S2mPrintf(Out, Size, "(no format, %lu bytes)", BufferSize);
        return;
    }
    if (DataFormat->FormatSize < S2M_MIN_WAVE_FORMAT || BufferSize < S2M_MIN_WAVE_FORMAT ||
        !IsEqualGUIDAligned(DataFormat->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX))
    {
        S2mPrintf(Out, Size, "(non-wave format, size %lu, subformat %08lX)", DataFormat->FormatSize,
                           DataFormat->SubFormat.Data1);
        return;
    }
    WAVEFORMATEX* wfx = &((PKSDATAFORMAT_WAVEFORMATEX)DataFormat)->WaveFormatEx;
    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && DataFormat->FormatSize >= sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEXTENSIBLE) &&
        BufferSize >= sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEXTENSIBLE))
    {
        WAVEFORMATEXTENSIBLE* x = (WAVEFORMATEXTENSIBLE*)wfx;
        const char* sub = IsEqualGUIDAligned(x->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) ? "PCM"
                        : IsEqualGUIDAligned(x->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) ? "FLOAT" : "other";
        S2mPrintf(Out, Size, "%lu Hz %u/%u bit %u ch %s ext mask 0x%lX align %u", wfx->nSamplesPerSec,
                           x->Samples.wValidBitsPerSample, wfx->wBitsPerSample, wfx->nChannels, sub,
                           x->dwChannelMask, wfx->nBlockAlign);
        return;
    }
    S2mPrintf(Out, Size, "%lu Hz %u bit %u ch tag 0x%04X align %u", wfx->nSamplesPerSec, wfx->wBitsPerSample,
                       wfx->nChannels, wfx->wFormatTag, wfx->nBlockAlign);
}

// KSPROPERTY_PIN_PROPOSEDATAFORMAT: Windows asks whether a pin would accept a format (Sound control panel
// format list, default-format validation, IAudioClient::IsFormatSupported in exclusive mode).
// Sent to the filter it carries a KSP_PIN (the pin id is the instance data after KSPROPERTY); sent to an
// open pin there is no instance data. Same semantics as the SYSVAD sample: only streaming pins take it,
// SET/BASICSUPPORT only, STATUS_NO_MATCH for formats the pin cannot open. Every call goes to the driver log.
static NTSTATUS NTAPI PropertyHandler_ProposeFormat(_In_ PPCPROPERTY_REQUEST Request)
{
    // PortCls passes the miniport's IUnknown, i.e. the IMiniportWaveRT sub-object.
    CMiniportWaveRT* miniport = static_cast<CMiniportWaveRT*>(reinterpret_cast<IMiniportWaveRT*>(Request->MajorTarget));
    const bool onPin = Request->MinorTarget != nullptr;
    ULONG pin = miniport->StreamPin();
    const char* verb = (Request->Verb & KSPROPERTY_TYPE_BASICSUPPORT) ? "BASICSUPPORT"
                     : (Request->Verb & KSPROPERTY_TYPE_SET) ? "SET"
                     : (Request->Verb & KSPROPERTY_TYPE_GET) ? "GET" : "?";
    NTSTATUS status;
    char text[128] = "";

    if (!onPin)
    {
        if (!Request->Instance || Request->InstanceSize < sizeof(ULONG))
        {
            status = STATUS_INVALID_PARAMETER;
            goto done;
        }
        pin = *(PULONG)Request->Instance;
        if (pin != miniport->StreamPin())
        {
            status = STATUS_NOT_SUPPORTED;     // bridge pin: analog, no PCM formats
            goto done;
        }
    }

    if (Request->Verb & KSPROPERTY_TYPE_BASICSUPPORT)
    {
        const ULONG access = KSPROPERTY_TYPE_BASICSUPPORT | KSPROPERTY_TYPE_SET;
        if (Request->ValueSize >= sizeof(KSPROPERTY_DESCRIPTION))
        {
            PKSPROPERTY_DESCRIPTION d = (PKSPROPERTY_DESCRIPTION)Request->Value;
            d->AccessFlags = access;
            d->DescriptionSize = sizeof(KSPROPERTY_DESCRIPTION);
            d->PropTypeSet.Set = kPropTypeSetGeneral;
            d->PropTypeSet.Id = VT_ILLEGAL;
            d->PropTypeSet.Flags = 0;
            d->MembersListCount = 0;
            d->Reserved = 0;
            Request->ValueSize = sizeof(KSPROPERTY_DESCRIPTION);
            status = STATUS_SUCCESS;
        }
        else if (Request->ValueSize >= sizeof(ULONG))
        {
            *(PULONG)Request->Value = access;
            Request->ValueSize = sizeof(ULONG);
            status = STATUS_SUCCESS;
        }
        else
        {
            Request->ValueSize = sizeof(ULONG);
            status = STATUS_BUFFER_OVERFLOW;
        }
    }
    else if (Request->Verb & KSPROPERTY_TYPE_SET)
    {
        if (Request->ValueSize == 0)
        {
            Request->ValueSize = sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEXTENSIBLE);
            status = STATUS_BUFFER_OVERFLOW;
        }
        else if (!Request->Value || Request->ValueSize < S2M_MIN_WAVE_FORMAT)
        {
            // Windows sends these (e.g. a bare KSDATAFORMAT) to see whether the property exists.
            S2mPrintf(text, sizeof(text), "%lu bytes, no wave format", Request->ValueSize);
            status = STATUS_BUFFER_TOO_SMALL;
        }
        else
        {
            S2mFormatText((PKSDATAFORMAT)Request->Value, Request->ValueSize, text, sizeof(text));
            status = miniport->CheckFormat((PKSDATAFORMAT)Request->Value, Request->ValueSize, nullptr, "propose");
        }
    }
    else
    {
        status = STATUS_INVALID_PARAMETER;     // GET is not supported (BASICSUPPORT says so)
    }

done:
    char line[200];
    S2mPrintf(line, sizeof(line), "propose %s on %s pin %lu%s%s -> 0x%08lX", verb, onPin ? "open" : "filter", pin,
              text[0] ? ": " : "", text, (ULONG)status);
    miniport->LogRepeatable(line);
    return status;
}

static const PCPROPERTY_ITEM g_ProposeProperties[] = {
    { &KSPROPSETID_Pin, KSPROPERTY_PIN_PROPOSEDATAFORMAT,
      PCPROPERTY_ITEM_FLAG_SET | PCPROPERTY_ITEM_FLAG_BASICSUPPORT, PropertyHandler_ProposeFormat },
};

// Requests on an open streaming pin.
static const PCAUTOMATION_TABLE g_StreamPinAutomation = {
    sizeof(PCPROPERTY_ITEM), sizeof(g_ProposeProperties) / sizeof(g_ProposeProperties[0]), g_ProposeProperties,
    0, 0, nullptr,
    0, 0, nullptr,
    0
};

// Requests on the filter with a KSP_PIN (how Windows normally proposes formats). Only used when the
// ProposeFormat setting is 1 (default): driver 1.0.269.743 had it and Windows did not create the
// endpoints on one test system; the installer turns it off automatically if that happens again.
static const PCAUTOMATION_TABLE g_WaveFilterAutomation = {
    sizeof(PCPROPERTY_ITEM), sizeof(g_ProposeProperties) / sizeof(g_ProposeProperties[0]), g_ProposeProperties,
    0, 0, nullptr,
    0, 0, nullptr,
    0
};

// ---------------------------------------------------------------------------
// CMiniportWaveRT

NTSTATUS CreateMiniportWaveRT(_Out_ PUNKNOWN* Unknown, _In_ CCable* Cable, _In_ BOOLEAN Capture)
{
    *Unknown = nullptr;

    CMiniportWaveRT* obj = new(POOL_FLAG_NON_PAGED, S2M_POOLTAG) CMiniportWaveRT(nullptr);
    if (!obj)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    obj->Setup(Cable, Capture);

    *Unknown = PUNKNOWN(PMINIPORTWAVERT(obj));
    (*Unknown)->AddRef();
    return STATUS_SUCCESS;
}

CMiniportWaveRT::~CMiniportWaveRT()
{
    if (m_port)
    {
        m_port->Release();
    }
    if (m_cable)
    {
        m_cable->Release();
    }
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveRT::NonDelegatingQueryInterface(_In_ REFIID Interface, _Out_ PVOID* Object)
{
    if (IsEqualGUIDAligned(Interface, IID_IUnknown))
        *Object = PVOID(PUNKNOWN(PMINIPORTWAVERT(this)));
    else if (IsEqualGUIDAligned(Interface, IID_IMiniport))
        *Object = PVOID(PMINIPORT(this));
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportWaveRT))
        *Object = PVOID(PMINIPORTWAVERT(this));
    else
    {
        *Object = nullptr;
        return STATUS_INVALID_PARAMETER;
    }

    PUNKNOWN(*Object)->AddRef();
    return STATUS_SUCCESS;
}

// Builds the filter descriptor. The streaming data range depends on the cable settings,
// so the descriptor lives in the miniport object instead of static tables.
void CMiniportWaveRT::Setup(_In_ CCable* Cable, _In_ BOOLEAN Capture)
{
    m_cable = Cable;
    m_cable->AddRef();
    m_capture = Capture;
    S2mPrintf(m_name, sizeof(m_name), "Cable %lu %s", Cable->Index() + 1, Capture ? "capture" : "render");
    KeInitializeSpinLock(&m_logLock);

    // Streaming pin: PCM at exactly the cable sample rate, up to the cable channel count,
    // either a fixed bit depth or anything from 16 to 32 bits.
    RtlZeroMemory(&m_streamRange, sizeof(m_streamRange));
    m_streamRange.DataRange.FormatSize = sizeof(KSDATARANGE_AUDIO);
    m_streamRange.DataRange.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    m_streamRange.DataRange.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    m_streamRange.DataRange.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
    m_streamRange.MaximumChannels = Capture ? Cable->CaptureChannels() : Cable->RenderChannels();
    // Always 16-32 bits: offering only 24 or 32 bits leaves Windows without a format it will use for
    // the endpoint (capture endpoint not created, render streams fail). The chosen bit depth is set as
    // the endpoint's default format by the control panel instead.
    m_streamRange.MinimumBitsPerSample = 16;
    m_streamRange.MaximumBitsPerSample = 32;
    m_streamRange.MinimumSampleFrequency = Cable->SampleRate();
    m_streamRange.MaximumSampleFrequency = Cable->SampleRate();
    m_streamRanges[0] = &m_streamRange.DataRange;

    // Bridge pin: analog connection to the topology filter.
    RtlZeroMemory(&m_bridgeRange, sizeof(m_bridgeRange));
    m_bridgeRange.FormatSize = sizeof(KSDATARANGE);
    m_bridgeRange.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    m_bridgeRange.SubFormat = KSDATAFORMAT_SUBTYPE_ANALOG;
    m_bridgeRange.Specifier = KSDATAFORMAT_SPECIFIER_NONE;
    m_bridgeRanges[0] = &m_bridgeRange;

    RtlZeroMemory(m_pins, sizeof(m_pins));
    PCPIN_DESCRIPTOR& stream = m_pins[Capture ? S2M_WAVE_CAPTURE_STREAM_PIN : S2M_WAVE_RENDER_STREAM_PIN];
    PCPIN_DESCRIPTOR& bridge = m_pins[Capture ? S2M_WAVE_CAPTURE_BRIDGE_PIN : S2M_WAVE_RENDER_BRIDGE_PIN];

    stream.AutomationTable = &g_StreamPinAutomation;
    stream.MaxGlobalInstanceCount = 1;
    stream.MaxFilterInstanceCount = 1;
    stream.MinFilterInstanceCount = 0;
    stream.KsPinDescriptor.DataRangesCount = 1;
    stream.KsPinDescriptor.DataRanges = m_streamRanges;
    stream.KsPinDescriptor.DataFlow = Capture ? KSPIN_DATAFLOW_OUT : KSPIN_DATAFLOW_IN;
    stream.KsPinDescriptor.Communication = KSPIN_COMMUNICATION_SINK;
    stream.KsPinDescriptor.Category = Capture ? &PINNAME_CAPTURE : &KSCATEGORY_AUDIO;

    bridge.KsPinDescriptor.DataRangesCount = 1;
    bridge.KsPinDescriptor.DataRanges = m_bridgeRanges;
    bridge.KsPinDescriptor.DataFlow = Capture ? KSPIN_DATAFLOW_IN : KSPIN_DATAFLOW_OUT;
    bridge.KsPinDescriptor.Communication = KSPIN_COMMUNICATION_NONE;
    bridge.KsPinDescriptor.Category = &KSCATEGORY_AUDIO;

    // Data flows pin 0 -> pin 1 in both filters.
    m_connections[0].FromNode = PCFILTER_NODE;
    m_connections[0].FromNodePin = 0;
    m_connections[0].ToNode = PCFILTER_NODE;
    m_connections[0].ToNodePin = 1;

    m_categories[0] = KSCATEGORY_AUDIO;
    m_categories[1] = Capture ? KSCATEGORY_CAPTURE : KSCATEGORY_RENDER;
    m_categories[2] = KSCATEGORY_REALTIME;

    RtlZeroMemory(&m_filter, sizeof(m_filter));
    m_filter.Version = 0;
    m_filter.AutomationTable = g_S2mProposeFormat ? &g_WaveFilterAutomation : nullptr;
    m_filter.PinSize = sizeof(PCPIN_DESCRIPTOR);
    m_filter.PinCount = 2;
    m_filter.Pins = m_pins;
    m_filter.NodeSize = sizeof(PCNODE_DESCRIPTOR);
    m_filter.NodeCount = 0;
    m_filter.Nodes = nullptr;
    m_filter.ConnectionCount = 1;
    m_filter.Connections = m_connections;
    m_filter.CategoryCount = 3;
    m_filter.Categories = m_categories;
}

// Windows repeats the same format proposals dozens of times; identical consecutive lines are counted
// instead of filling the (size-limited) driver log. Serialized by a spin lock (callers may run in parallel).
void CMiniportWaveRT::LogRepeatable(_In_z_ const char* Line)
{
    KIRQL irql;
    KeAcquireSpinLock(&m_logLock, &irql);
    ULONG i = 0;
    while (i < sizeof(m_lastLog) - 1 && Line[i] && Line[i] == m_lastLog[i]) i++;
    bool same = Line[i] == m_lastLog[i] || (i == sizeof(m_lastLog) - 1);
    ULONG repeats = m_logRepeats;
    if (same)
    {
        m_logRepeats++;
    }
    else
    {
        m_logRepeats = 0;
        for (i = 0; i < sizeof(m_lastLog) - 1 && Line[i]; i++) m_lastLog[i] = Line[i];
        m_lastLog[i] = 0;
    }
    KeReleaseSpinLock(&m_logLock, irql);

    if (same) return;
    if (repeats) S2mLog("%s: previous line repeated %lu more time(s)", m_name, repeats);
    S2mLog("%s %s", m_name, Line);
    S2mLogFlush();
}

NTSTATUS CMiniportWaveRT::CheckFormat(_In_ PKSDATAFORMAT DataFormat, _In_ ULONG BufferSize, _Out_opt_ S2M_FORMAT* Format,
                                      _In_ const char* Who)
{
    const char* side = m_capture ? "capture" : "render";
    if (BufferSize < sizeof(KSDATAFORMAT) || DataFormat->FormatSize > BufferSize)
    {
        S2mLog("Cable %lu %s %s: format buffer too small (%lu)", m_cable->Index() + 1, side, Who, BufferSize);
        S2mLogFlush();
        return STATUS_BUFFER_TOO_SMALL;
    }

    S2M_FORMAT f;
    NTSTATUS status = S2mParseFormat(DataFormat, &f);
    if (!NT_SUCCESS(status))
    {
        S2mLogFlush();
        return STATUS_NO_MATCH;
    }

    // Valid bits of WAVE_FORMAT_EXTENSIBLE (e.g. 24 bits in a 32-bit container) count as that depth too.
    ULONG bits = f.ContainerBytes * 8, validBits = bits;
    WAVEFORMATEX* wfx = &((PKSDATAFORMAT_WAVEFORMATEX)DataFormat)->WaveFormatEx;
    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && ((WAVEFORMATEXTENSIBLE*)wfx)->Samples.wValidBitsPerSample)
    {
        validBits = ((WAVEFORMATEXTENSIBLE*)wfx)->Samples.wValidBitsPerSample;
    }
    const char* reason = nullptr;
    if (f.SampleRate != m_cable->SampleRate()) reason = "sample rate";
    else if (f.Channels > m_streamRange.MaximumChannels) reason = "channel count";

    if (reason)
    {
        S2mLog("Cable %lu %s %s: %lu Hz %lu bit %lu ch rejected (%s; cable: %lu Hz, %lu bit, up to %lu ch)",
               m_cable->Index() + 1, side, Who, f.SampleRate, validBits, f.Channels, reason,
               m_cable->SampleRate(), m_cable->BitsPerSample(), m_streamRange.MaximumChannels);
        S2mLogFlush();
        return STATUS_NO_MATCH;
    }

    if (Format) *Format = f;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveRT::Init(_In_ PUNKNOWN UnknownAdapter, _In_ PRESOURCELIST ResourceList, _In_ PPORTWAVERT Port)
{
    UNREFERENCED_PARAMETER(UnknownAdapter);
    UNREFERENCED_PARAMETER(ResourceList);

    m_port = Port;
    m_port->AddRef();
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveRT::GetDescription(_Out_ PPCFILTER_DESCRIPTOR* Description)
{
    if (!m_described)
    {
        m_described = TRUE;
        S2mLog("Cable %lu %s wave filter: description requested (rate %lu, up to %lu ch, bits %lu-%lu)",
               m_cable->Index() + 1, m_capture ? "capture" : "render", m_streamRange.MaximumSampleFrequency,
               m_streamRange.MaximumChannels, m_streamRange.MinimumBitsPerSample, m_streamRange.MaximumBitsPerSample);
    }
    *Description = &m_filter;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveRT::DataRangeIntersection(
    _In_ ULONG PinId, _In_ PKSDATARANGE DataRange, _In_ PKSDATARANGE MatchingDataRange,
    _In_ ULONG OutputBufferLength, _Out_opt_ PVOID ResultantFormat, _Out_ PULONG ResultantFormatLength)
{
    UNREFERENCED_PARAMETER(MatchingDataRange);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(ResultantFormat);
    UNREFERENCED_PARAMETER(ResultantFormatLength);

    // Logged to see how Windows negotiates formats (e.g. which bit depths it asks for).
    if (DataRange && DataRange->FormatSize >= sizeof(KSDATARANGE_AUDIO) &&
        IsEqualGUIDAligned(DataRange->MajorFormat, KSDATAFORMAT_TYPE_AUDIO))
    {
        PKSDATARANGE_AUDIO r = (PKSDATARANGE_AUDIO)DataRange;
        S2mLog("%s intersection pin %lu: asked %lu-%lu Hz, %lu-%lu bit, up to %lu ch, subformat %08lX", Name(), PinId,
               r->MinimumSampleFrequency, r->MaximumSampleFrequency, r->MinimumBitsPerSample, r->MaximumBitsPerSample,
               r->MaximumChannels, DataRange->SubFormat.Data1);
    }
    else if (DataRange)
    {
        S2mLog("%s intersection pin %lu: asked range size %lu, subformat %08lX", Name(), PinId, DataRange->FormatSize,
               DataRange->SubFormat.Data1);
    }

    // Let PortCls do the default intersection against our data ranges.
    return STATUS_NOT_IMPLEMENTED;
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveRT::GetDeviceDescription(_Out_ PDEVICE_DESCRIPTION DeviceDescription)
{
    RtlZeroMemory(DeviceDescription, sizeof(DEVICE_DESCRIPTION));
    DeviceDescription->Master = TRUE;
    DeviceDescription->ScatterGather = TRUE;
    DeviceDescription->Dma32BitAddresses = TRUE;
    DeviceDescription->InterfaceType = PCIBus;
    DeviceDescription->MaximumLength = 0xFFFFFFFF;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveRT::NewStream(
    _Out_ PMINIPORTWAVERTSTREAM* Stream, _In_ PPORTWAVERTSTREAM PortStream,
    _In_ ULONG Pin, _In_ BOOLEAN Capture, _In_ PKSDATAFORMAT DataFormat)
{
    *Stream = nullptr;

    const char* side = m_capture ? "capture" : "render";
    ULONG streamPin = m_capture ? S2M_WAVE_CAPTURE_STREAM_PIN : S2M_WAVE_RENDER_STREAM_PIN;
    if (Pin != streamPin || (Capture ? TRUE : FALSE) != m_capture)
    {
        S2mLog("Cable %lu %s: NewStream on wrong pin %lu (capture=%d)", m_cable->Index() + 1, side, Pin, (int)Capture);
        S2mLogFlush();
        return STATUS_INVALID_PARAMETER;
    }

    char text[128];
    S2mFormatText(DataFormat, DataFormat->FormatSize, text, sizeof(text));

    S2M_FORMAT format;
    NTSTATUS status = CheckFormat(DataFormat, DataFormat->FormatSize, &format, "NewStream");
    if (!NT_SUCCESS(status))
    {
        return STATUS_NO_MATCH;
    }

    CMiniportWaveRTStream* obj = new(POOL_FLAG_NON_PAGED, S2M_POOLTAG) CMiniportWaveRTStream(nullptr);
    if (!obj)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    obj->AddRef();

    status = obj->Init(m_cable, m_capture, PortStream, format);
    if (!NT_SUCCESS(status))
    {
        S2mLog("Cable %lu %s: stream init failed 0x%08lX", m_cable->Index() + 1, side, (ULONG)status);
        S2mLogFlush();
        obj->Release();
        return status;
    }

    S2mLog("Cable %lu %s: stream opened, %lu Hz, %lu bit, %lu ch (%s)", m_cable->Index() + 1, side,
           format.SampleRate, format.ContainerBytes * 8, format.Channels, text);
    S2mLogFlush();
    *Stream = PMINIPORTWAVERTSTREAM(obj);
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// CMiniportWaveRTStream

NTSTATUS CMiniportWaveRTStream::Init(_In_ CCable* Cable, _In_ BOOLEAN Capture, _In_ PPORTWAVERTSTREAM PortStream, _In_ const S2M_FORMAT& Format)
{
    m_cable = Cable;
    m_cable->AddRef();
    m_capture = Capture;
    m_portStream = PortStream;
    m_portStream->AddRef();
    m_format = Format;
    m_state = KSSTATE_STOP;
    m_qpcFreq = Cable->QpcFrequency();
    KeInitializeSpinLock(&m_lock);

    m_timer = ExAllocateTimer(TimerCallback, this, EX_TIMER_HIGH_RESOLUTION);
    if (!m_timer)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    return STATUS_SUCCESS;
}

CMiniportWaveRTStream::~CMiniportWaveRTStream()
{
    if (m_cable)
    {
        S2mLog("Cable %lu %s: stream closed", m_cable->Index() + 1, m_capture ? "capture" : "render");
    }
    if (m_timer)
    {
        // Cancels the timer and waits for a running callback to finish.
        ExDeleteTimer(m_timer, TRUE, TRUE, nullptr);
        m_timer = nullptr;
    }
    ReleaseBuffer();
    if (m_portStream)
    {
        m_portStream->Release();
    }
    if (m_cable)
    {
        m_cable->Release();
    }
    S2mLogFlush();
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveRTStream::NonDelegatingQueryInterface(_In_ REFIID Interface, _Out_ PVOID* Object)
{
    if (IsEqualGUIDAligned(Interface, IID_IUnknown))
        *Object = PVOID(PUNKNOWN(PMINIPORTWAVERTSTREAM(this)));
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportWaveRTStream))
        *Object = PVOID(PMINIPORTWAVERTSTREAM(this));
    else
    {
        *Object = nullptr;
        return STATUS_INVALID_PARAMETER;
    }

    PUNKNOWN(*Object)->AddRef();
    return STATUS_SUCCESS;
}

VOID NTAPI CMiniportWaveRTStream::TimerCallback(_In_ PEX_TIMER Timer, _In_opt_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Timer);

    CMiniportWaveRTStream* self = (CMiniportWaveRTStream*)Context;
    ULONGLONG now = (ULONGLONG)KeQueryPerformanceCounter(nullptr).QuadPart;

    KIRQL irql;
    KeAcquireSpinLock(&self->m_lock, &irql);
    self->UpdateLocked(now);
    KeReleaseSpinLock(&self->m_lock, irql);
}

// Moves every frame that became due since the last call between the buffer and the cable.
void CMiniportWaveRTStream::UpdateLocked(_In_ ULONGLONG Qpc)
{
    if (m_state != KSSTATE_RUN || !m_buffer || m_bufferFrames == 0)
    {
        return;
    }

    ULONGLONG elapsed = Qpc > m_runStartQpc ? Qpc - m_runStartQpc : 0;
    ULONGLONG target = m_framesAtRunStart + S2mTicksToFrames(elapsed, m_qpcFreq, m_format.SampleRate);
    if (target <= m_framesDone)
    {
        return;
    }

    ULONGLONG due = target - m_framesDone;
    if (due > m_bufferFrames)
    {
        // Stalled for longer than the buffer (e.g. the VM was paused): drop what is gone.
        m_framesDone += due - m_bufferFrames;
        due = m_bufferFrames;
    }

    while (due > 0)
    {
        ULONG pos = (ULONG)(m_framesDone % m_bufferFrames);
        ULONG chunk = m_bufferFrames - pos;
        if (chunk > due)
        {
            chunk = (ULONG)due;
        }

        BYTE* p = m_buffer + (SIZE_T)pos * m_format.BlockAlign;
        ULONGLONG cableFrame = m_cableBase + m_framesDone;
        if (m_capture)
            m_cable->Read(cableFrame, p, chunk, m_format);
        else
            m_cable->Write(cableFrame, p, chunk, m_format);

        m_framesDone += chunk;
        due -= chunk;
    }
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveRTStream::SetState(_In_ KSSTATE State)
{
    static const char* names[] = { "STOP", "ACQUIRE", "PAUSE", "RUN" };
    S2mLog("Cable %lu %s: state %s -> %s", m_cable->Index() + 1, m_capture ? "capture" : "render",
           (ULONG)m_state < 4 ? names[m_state] : "?", (ULONG)State < 4 ? names[State] : "?");
    S2mLogFlush();
    ULONGLONG now = (ULONGLONG)KeQueryPerformanceCounter(nullptr).QuadPart;
    KIRQL irql;

    if (State == KSSTATE_RUN && m_state != KSSTATE_RUN)
    {
        KeAcquireSpinLock(&m_lock, &irql);

        ULONGLONG cableNow = m_cable->FrameAt(now);
        m_runStartQpc = now;
        m_framesAtRunStart = m_framesDone;
        if (m_capture)
        {
            // Read LatencyFrames behind "now" so the render side has always written the data.
            ULONGLONG start = cableNow > m_cable->LatencyFrames() ? cableNow - m_cable->LatencyFrames() : 0;
            m_cableBase = start - m_framesDone;
        }
        else
        {
            m_cableBase = cableNow - m_framesDone;
            m_cable->RenderStart(cableNow);
        }
        m_state = KSSTATE_RUN;

        KeReleaseSpinLock(&m_lock, irql);
        ExSetTimer(m_timer, -S2M_TIMER_PERIOD_100NS, S2M_TIMER_PERIOD_100NS, nullptr);
        return STATUS_SUCCESS;
    }

    if (State != KSSTATE_RUN && m_state == KSSTATE_RUN)
    {
        ExCancelTimer(m_timer, nullptr);
        KeAcquireSpinLock(&m_lock, &irql);
        UpdateLocked(now);
        KeReleaseSpinLock(&m_lock, irql);
    }

    KeAcquireSpinLock(&m_lock, &irql);
    m_state = State;
    if (State == KSSTATE_STOP)
    {
        m_framesDone = 0;
    }
    KeReleaseSpinLock(&m_lock, irql);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveRTStream::GetPosition(_Out_ PKSAUDIO_POSITION Position)
{
    ULONGLONG now = (ULONGLONG)KeQueryPerformanceCounter(nullptr).QuadPart;

    KIRQL irql;
    KeAcquireSpinLock(&m_lock, &irql);
    UpdateLocked(now);
    ULONGLONG offset = m_bufferFrames ? (m_framesDone % m_bufferFrames) * m_format.BlockAlign : 0;
    KeReleaseSpinLock(&m_lock, irql);

    Position->PlayOffset = offset;
    Position->WriteOffset = offset;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveRTStream::SetFormat(_In_ PKSDATAFORMAT DataFormat)
{
    if (m_state == KSSTATE_RUN)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }

    S2M_FORMAT format;
    NTSTATUS status = S2mParseFormat(DataFormat, &format);
    ULONG validBits = format.ContainerBytes * 8;
    WAVEFORMATEX* wfx = &((PKSDATAFORMAT_WAVEFORMATEX)DataFormat)->WaveFormatEx;
    if (NT_SUCCESS(status) && wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        ((WAVEFORMATEXTENSIBLE*)wfx)->Samples.wValidBitsPerSample)
    {
        validBits = ((WAVEFORMATEXTENSIBLE*)wfx)->Samples.wValidBitsPerSample;
    }
    if (!NT_SUCCESS(status) || format.SampleRate != m_cable->SampleRate() || format.Channels > (m_capture ? m_cable->CaptureChannels() : m_cable->RenderChannels()))
    {
        S2mLog("Cable %lu %s SetFormat: %lu Hz %lu bit %lu ch rejected", m_cable->Index() + 1,
               m_capture ? "capture" : "render", format.SampleRate, validBits, format.Channels);
        S2mLogFlush();
        return STATUS_NO_MATCH;
    }

    KIRQL irql;
    KeAcquireSpinLock(&m_lock, &irql);
    m_format = format;
    m_bufferFrames = m_bufferBytes / m_format.BlockAlign;
    KeReleaseSpinLock(&m_lock, irql);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveRTStream::AllocateAudioBuffer(
    _In_ ULONG RequestedSize, _Out_ PMDL* AudioBufferMdl, _Out_ ULONG* ActualSize,
    _Out_ ULONG* OffsetFromFirstPage, _Out_ MEMORY_CACHING_TYPE* CacheType)
{
    *AudioBufferMdl = nullptr;
    *ActualSize = 0;
    *OffsetFromFirstPage = 0;
    *CacheType = MmCached;

    if (m_mdl)
    {
        return STATUS_UNSUCCESSFUL;
    }

    // Whole frames only; at least 10 ms.
    ULONG minBytes = (m_format.SampleRate / 100) * m_format.BlockAlign;
    ULONG size = RequestedSize < minBytes ? minBytes : RequestedSize;
    size -= size % m_format.BlockAlign;

    PHYSICAL_ADDRESS high;
    high.HighPart = 0;
    high.LowPart = MAXULONG;

    PMDL mdl = m_portStream->AllocatePagesForMdl(high, size);
    if (!mdl)
    {
        S2mLog("Cable %lu: AllocatePagesForMdl(%lu) failed", m_cable->Index() + 1, size);
        S2mLogFlush();
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    BYTE* buffer = (BYTE*)m_portStream->MapAllocatedPages(mdl, MmCached);
    if (!buffer)
    {
        m_portStream->FreePagesFromMdl(mdl);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(buffer, size);

    KIRQL irql;
    KeAcquireSpinLock(&m_lock, &irql);
    m_mdl = mdl;
    m_buffer = buffer;
    m_bufferBytes = size;
    m_bufferFrames = size / m_format.BlockAlign;
    KeReleaseSpinLock(&m_lock, irql);

    S2mLog("Cable %lu %s: audio buffer %lu bytes (requested %lu)", m_cable->Index() + 1,
           m_capture ? "capture" : "render", size, RequestedSize);
    S2mLogFlush();
    *AudioBufferMdl = mdl;
    *ActualSize = size;
    return STATUS_SUCCESS;
}

void CMiniportWaveRTStream::ReleaseBuffer()
{
    KIRQL irql;
    KeAcquireSpinLock(&m_lock, &irql);
    PMDL mdl = m_mdl;
    BYTE* buffer = m_buffer;
    m_mdl = nullptr;
    m_buffer = nullptr;
    m_bufferBytes = 0;
    m_bufferFrames = 0;
    KeReleaseSpinLock(&m_lock, irql);

    if (mdl)
    {
        m_portStream->UnmapAllocatedPages(buffer, mdl);
        m_portStream->FreePagesFromMdl(mdl);
    }
}

STDMETHODIMP_(VOID) CMiniportWaveRTStream::FreeAudioBuffer(_In_opt_ PMDL AudioBufferMdl, _In_ ULONG BufferSize)
{
    UNREFERENCED_PARAMETER(AudioBufferMdl);
    UNREFERENCED_PARAMETER(BufferSize);
    ReleaseBuffer();
}

STDMETHODIMP_(VOID) CMiniportWaveRTStream::GetHWLatency(_Out_ KSRTAUDIO_HWLATENCY* hwLatency)
{
    hwLatency->FifoSize = 0;
    hwLatency->ChipsetDelay = 0;
    hwLatency->CodecDelay = 0;
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveRTStream::GetPositionRegister(_Out_ KSRTAUDIO_HWREGISTER* Register)
{
    UNREFERENCED_PARAMETER(Register);
    return STATUS_NOT_SUPPORTED;
}

STDMETHODIMP_(NTSTATUS) CMiniportWaveRTStream::GetClockRegister(_Out_ KSRTAUDIO_HWREGISTER* Register)
{
    UNREFERENCED_PARAMETER(Register);
    return STATUS_NOT_SUPPORTED;
}
