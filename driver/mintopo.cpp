// Topology miniport implementation. Render: no nodes (Windows applies software volume, -96..0 dB).
// Capture: volume + mute nodes backed by CCable (see mintopo.h).
#include "mintopo.h"
#include "cable.h"
#include "log.h"
#include "voltable.h"

static const GUID kTopoPropTypeSetGeneral = { 0x97E99BA0, 0xBDEA, 0x11CF, { 0xA5, 0xD6, 0x28, 0xDB, 0x04, 0xC1, 0x00, 0x00 } };

// KSPROPERTY_AUDIO_VOLUMELEVEL / KSPROPERTY_AUDIO_MUTE on the capture nodes. The instance data is the channel
// (KSNODEPROPERTY_AUDIO_CHANNEL after its KSNODEPROPERTY header); -1 = every channel.
static NTSTATUS NTAPI PropertyHandler_Level(_In_ PPCPROPERTY_REQUEST Request)
{
    CMiniportTopology* miniport = static_cast<CMiniportTopology*>(reinterpret_cast<IMiniportTopology*>(Request->MajorTarget));
    CCable* cable = miniport->Cable();
    const bool volume = Request->PropertyItem->Id == KSPROPERTY_AUDIO_VOLUMELEVEL;
    const ULONG channels = cable ? cable->CaptureChannels() : 0;
    if (!cable || channels == 0) return STATUS_INVALID_DEVICE_REQUEST;
    const ULONG size = Request->ValueSize;      // what the caller's buffer holds

    if (Request->Verb & KSPROPERTY_TYPE_BASICSUPPORT)
    {
        // Volume: one stepped range per channel (the channel count Windows gives the endpoint volume).
        const ULONG access = KSPROPERTY_TYPE_BASICSUPPORT | KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET;
        const ULONG full = sizeof(KSPROPERTY_DESCRIPTION) +
                           (volume ? sizeof(KSPROPERTY_MEMBERSHEADER) + channels * sizeof(KSPROPERTY_STEPPING_LONG) : 0);
        if (size >= sizeof(KSPROPERTY_DESCRIPTION))
        {
            PKSPROPERTY_DESCRIPTION d = (PKSPROPERTY_DESCRIPTION)Request->Value;
            d->AccessFlags = access;
            d->DescriptionSize = full;
            d->PropTypeSet.Set = kTopoPropTypeSetGeneral;
            d->PropTypeSet.Id = volume ? VT_I4 : VT_BOOL;
            d->PropTypeSet.Flags = 0;
            d->MembersListCount = volume ? 1 : 0;
            d->Reserved = 0;
            Request->ValueSize = sizeof(KSPROPERTY_DESCRIPTION);
            if (volume && size >= full)
            {
                PKSPROPERTY_MEMBERSHEADER m = (PKSPROPERTY_MEMBERSHEADER)(d + 1);
                m->MembersFlags = KSPROPERTY_MEMBER_STEPPEDRANGES;
                m->MembersSize = sizeof(KSPROPERTY_STEPPING_LONG);
                m->MembersCount = channels;
                m->Flags = KSPROPERTY_MEMBER_FLAG_BASICSUPPORT_MULTICHANNEL;
                PKSPROPERTY_STEPPING_LONG r = (PKSPROPERTY_STEPPING_LONG)(m + 1);
                for (ULONG c = 0; c < channels; c++)
                {
                    r[c].SteppingDelta = S2M_VOL_STEP;
                    r[c].Reserved = 0;
                    r[c].Bounds.SignedMinimum = S2M_VOL_MIN;
                    r[c].Bounds.SignedMaximum = S2M_VOL_MAX;
                }
                Request->ValueSize = full;
            }
            return STATUS_SUCCESS;
        }
        if (size >= sizeof(ULONG))
        {
            *(PULONG)Request->Value = access;
            Request->ValueSize = sizeof(ULONG);
            return STATUS_SUCCESS;
        }
        Request->ValueSize = full;
        return STATUS_BUFFER_OVERFLOW;
    }

    if (!Request->Instance || Request->InstanceSize < sizeof(LONG)) return STATUS_INVALID_PARAMETER;
    const LONG channel = *(PLONG)Request->Instance;
    if (channel != -1 && (channel < 0 || (ULONG)channel >= channels)) return STATUS_INVALID_PARAMETER;
    if (size < sizeof(LONG))
    {
        Request->ValueSize = sizeof(LONG);
        return size ? STATUS_BUFFER_TOO_SMALL : STATUS_BUFFER_OVERFLOW;
    }
    const ULONG first = channel == -1 ? 0 : (ULONG)channel, last = channel == -1 ? channels - 1 : (ULONG)channel;

    if (Request->Verb & KSPROPERTY_TYPE_GET)
    {
        *(PLONG)Request->Value = volume ? cable->Volume(first) : (LONG)cable->Mute(first);
        Request->ValueSize = sizeof(LONG);
        return STATUS_SUCCESS;
    }
    if (Request->Verb & KSPROPERTY_TYPE_SET)
    {
        const LONG value = *(PLONG)Request->Value;
        // Mute is one switch for the whole microphone: Windows sets it only on the channels it assumes (seen: 2
        // of 8, the rest stayed muted after a start with a stored mute), so any channel's SET mutes all.
        for (ULONG c = volume ? first : 0; c <= (volume ? last : 7); c++)
        {
            if (volume) cable->SetVolume(c, value);
            else cable->SetMute(c, value != 0);
        }
        if (volume)
            S2mLog("Cable %lu microphone volume, channel %ld: %ld/65536 dB (stored %ld)", cable->Index() + 1, channel, value,
                   cable->Volume(first));
        else
            S2mLog("Cable %lu microphone mute, channel %ld: %s", cable->Index() + 1, channel, value ? "on" : "off");
        // Remembered for the next device start (channel 1 stands for all: Windows sets every channel alike).
        if (volume && first == 0) S2mLogSetValue(L"MicVolume", (ULONG)cable->Volume(0));     // mute: see ReadConfig
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_PARAMETER;
}

static const PCPROPERTY_ITEM g_VolumeProperties[] = {
    { &KSPROPSETID_Audio, KSPROPERTY_AUDIO_VOLUMELEVEL,
      PCPROPERTY_ITEM_FLAG_GET | PCPROPERTY_ITEM_FLAG_SET | PCPROPERTY_ITEM_FLAG_BASICSUPPORT, PropertyHandler_Level },
};
static const PCPROPERTY_ITEM g_MuteProperties[] = {
    { &KSPROPSETID_Audio, KSPROPERTY_AUDIO_MUTE,
      PCPROPERTY_ITEM_FLAG_GET | PCPROPERTY_ITEM_FLAG_SET | PCPROPERTY_ITEM_FLAG_BASICSUPPORT, PropertyHandler_Level },
};
static const PCAUTOMATION_TABLE g_VolumeAutomation = {
    sizeof(PCPROPERTY_ITEM), 1, g_VolumeProperties, 0, 0, nullptr, 0, 0, nullptr, 0
};
static const PCAUTOMATION_TABLE g_MuteAutomation = {
    sizeof(PCPROPERTY_ITEM), 1, g_MuteProperties, 0, 0, nullptr, 0, 0, nullptr, 0
};

NTSTATUS CreateMiniportTopology(_Out_ PUNKNOWN* Unknown, _In_ CCable* Cable, _In_ BOOLEAN Capture)
{
    *Unknown = nullptr;

    CMiniportTopology* obj = new(POOL_FLAG_NON_PAGED, S2M_POOLTAG) CMiniportTopology(nullptr);
    if (!obj)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    obj->Setup(Cable, Capture);

    *Unknown = PUNKNOWN(PMINIPORTTOPOLOGY(obj));
    (*Unknown)->AddRef();
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportTopology::NonDelegatingQueryInterface(_In_ REFIID Interface, _Out_ PVOID* Object)
{
    if (IsEqualGUIDAligned(Interface, IID_IUnknown))
        *Object = PVOID(PUNKNOWN(PMINIPORTTOPOLOGY(this)));
    else if (IsEqualGUIDAligned(Interface, IID_IMiniport))
        *Object = PVOID(PMINIPORT(this));
    else if (IsEqualGUIDAligned(Interface, IID_IMiniportTopology))
        *Object = PVOID(PMINIPORTTOPOLOGY(this));
    else
    {
        *Object = nullptr;
        return STATUS_INVALID_PARAMETER;
    }

    PUNKNOWN(*Object)->AddRef();
    return STATUS_SUCCESS;
}

CMiniportTopology::~CMiniportTopology()
{
    if (m_cableObj) m_cableObj->Release();
}

void CMiniportTopology::Setup(_In_ CCable* Cable, _In_ BOOLEAN Capture)
{
    const ULONG CableIndex = Cable->Index();
    m_cable = CableIndex;
    m_cableObj = Cable;
    Cable->AddRef();
    m_capture = Capture;
    RtlZeroMemory(&m_bridgeRange, sizeof(m_bridgeRange));
    m_bridgeRange.FormatSize = sizeof(KSDATARANGE);
    m_bridgeRange.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    m_bridgeRange.SubFormat = KSDATAFORMAT_SUBTYPE_ANALOG;
    m_bridgeRange.Specifier = KSDATAFORMAT_SPECIFIER_NONE;
    m_bridgeRanges[0] = &m_bridgeRange;

    RtlZeroMemory(m_pins, sizeof(m_pins));
    for (ULONG i = 0; i < 2; i++)
    {
        m_pins[i].KsPinDescriptor.DataRangesCount = 1;
        m_pins[i].KsPinDescriptor.DataRanges = m_bridgeRanges;
        m_pins[i].KsPinDescriptor.Communication = KSPIN_COMMUNICATION_NONE;
    }
    m_pins[0].KsPinDescriptor.DataFlow = KSPIN_DATAFLOW_IN;
    m_pins[1].KsPinDescriptor.DataFlow = KSPIN_DATAFLOW_OUT;

    if (Capture)
    {
        // The jack pin category makes the endpoint a microphone; its Name GUID gives "Speak2Mic Microphone".
        m_pins[S2M_TOPO_CAPTURE_JACK_PIN].KsPinDescriptor.Category = &KSNODETYPE_MICROPHONE;
        m_pins[S2M_TOPO_CAPTURE_JACK_PIN].KsPinDescriptor.Name = &g_S2mPinNames[CableIndex][1];
        m_pins[S2M_TOPO_CAPTURE_BRIDGE_PIN].KsPinDescriptor.Category = &KSCATEGORY_AUDIO;
    }
    else
    {
        m_pins[S2M_TOPO_RENDER_BRIDGE_PIN].KsPinDescriptor.Category = &KSCATEGORY_AUDIO;
        m_pins[S2M_TOPO_RENDER_JACK_PIN].KsPinDescriptor.Category = &KSNODETYPE_LINE_CONNECTOR;
        m_pins[S2M_TOPO_RENDER_JACK_PIN].KsPinDescriptor.Name = &g_S2mPinNames[CableIndex][0];
    }

    ULONG connections = 1;
    if (Capture)
    {
        // jack (pin 0) -> volume -> mute -> bridge (pin 1)
        RtlZeroMemory(m_nodes, sizeof(m_nodes));
        m_nodes[S2M_TOPO_NODE_VOLUME].AutomationTable = &g_VolumeAutomation;
        m_nodes[S2M_TOPO_NODE_VOLUME].Type = &KSNODETYPE_VOLUME;
        m_nodes[S2M_TOPO_NODE_MUTE].AutomationTable = &g_MuteAutomation;
        m_nodes[S2M_TOPO_NODE_MUTE].Type = &KSNODETYPE_MUTE;
        m_connections[0] = { PCFILTER_NODE, S2M_TOPO_CAPTURE_JACK_PIN, S2M_TOPO_NODE_VOLUME, KSNODEPIN_STANDARD_IN };
        m_connections[1] = { S2M_TOPO_NODE_VOLUME, KSNODEPIN_STANDARD_OUT, S2M_TOPO_NODE_MUTE, KSNODEPIN_STANDARD_IN };
        m_connections[2] = { S2M_TOPO_NODE_MUTE, KSNODEPIN_STANDARD_OUT, PCFILTER_NODE, S2M_TOPO_CAPTURE_BRIDGE_PIN };
        connections = 3;
    }
    else
    {
        m_connections[0].FromNode = PCFILTER_NODE;
        m_connections[0].FromNodePin = 0;
        m_connections[0].ToNode = PCFILTER_NODE;
        m_connections[0].ToNodePin = 1;
    }

    m_categories[0] = KSCATEGORY_AUDIO;
    m_categories[1] = KSCATEGORY_TOPOLOGY;

    RtlZeroMemory(&m_filter, sizeof(m_filter));
    m_filter.PinSize = sizeof(PCPIN_DESCRIPTOR);
    m_filter.PinCount = 2;
    m_filter.Pins = m_pins;
    m_filter.NodeSize = sizeof(PCNODE_DESCRIPTOR);
    m_filter.NodeCount = Capture ? 2 : 0;
    m_filter.Nodes = Capture ? m_nodes : nullptr;
    m_filter.ConnectionCount = connections;
    m_filter.Connections = m_connections;
    m_filter.CategoryCount = 2;
    m_filter.Categories = m_categories;
}

STDMETHODIMP_(NTSTATUS) CMiniportTopology::Init(_In_ PUNKNOWN UnknownAdapter, _In_ PRESOURCELIST ResourceList, _In_ PPORTTOPOLOGY Port)
{
    UNREFERENCED_PARAMETER(UnknownAdapter);
    UNREFERENCED_PARAMETER(ResourceList);
    UNREFERENCED_PARAMETER(Port);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportTopology::GetDescription(_Out_ PPCFILTER_DESCRIPTOR* Description)
{
    if (!m_described)
    {
        m_described = TRUE;
        S2mLog("Cable %lu %s topology filter: description requested", m_cable + 1, m_capture ? "capture" : "render");
    }
    *Description = &m_filter;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CMiniportTopology::DataRangeIntersection(
    _In_ ULONG PinId, _In_ PKSDATARANGE DataRange, _In_ PKSDATARANGE MatchingDataRange,
    _In_ ULONG OutputBufferLength, _Out_opt_ PVOID ResultantFormat, _Out_ PULONG ResultantFormatLength)
{
    UNREFERENCED_PARAMETER(PinId);
    UNREFERENCED_PARAMETER(DataRange);
    UNREFERENCED_PARAMETER(MatchingDataRange);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(ResultantFormat);
    UNREFERENCED_PARAMETER(ResultantFormatLength);
    return STATUS_NOT_IMPLEMENTED;
}
