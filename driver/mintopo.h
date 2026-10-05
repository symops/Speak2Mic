// Topology miniport: tells the audio endpoint builder what kind of jack the endpoint is.
//   render : pin 0 = bridge from the wave filter, pin 1 = "line out" jack named "Speak2Mic Speaker"
//   capture: pin 0 = "microphone" jack named "Speak2Mic Microphone", pin 1 = bridge to the wave filter,
//            with a volume node and a mute node in between: Windows then uses them as the microphone's
//            endpoint volume (range -96..+9.56 dB = the control panel's 0..300 %) instead of its own
//            software volume (-96..+30 dB, which it kept resetting to +30 dB).
#pragma once

#include "common.h"

#define S2M_TOPO_RENDER_BRIDGE_PIN   0
#define S2M_TOPO_RENDER_JACK_PIN     1
#define S2M_TOPO_CAPTURE_JACK_PIN    0
#define S2M_TOPO_CAPTURE_BRIDGE_PIN  1
#define S2M_TOPO_NODE_VOLUME         0
#define S2M_TOPO_NODE_MUTE           1

class CCable;

class CMiniportTopology : public IMiniportTopology, public CUnknown
{
public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CMiniportTopology);
    ~CMiniportTopology();

    IMP_IMiniportTopology;

    void Setup(_In_ CCable* Cable, _In_ BOOLEAN Capture);
    CCable* Cable() const { return m_cableObj; }

private:
    ULONG                   m_cable = 0;
    CCable*                 m_cableObj = nullptr;   // referenced; volume / mute live there (capture)
    BOOLEAN                 m_capture = FALSE;
    BOOLEAN                 m_described = FALSE;
    PCFILTER_DESCRIPTOR     m_filter;
    PCPIN_DESCRIPTOR        m_pins[2];
    PCNODE_DESCRIPTOR       m_nodes[2];
    PCCONNECTION_DESCRIPTOR m_connections[3];
    GUID                    m_categories[2];
    KSDATARANGE             m_bridgeRange;
    PKSDATARANGE            m_bridgeRanges[1];
};
