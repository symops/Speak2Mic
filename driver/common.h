// Common declarations for the Speak2Mic PortCls/WaveRT driver.
#pragma once

// We supply our own new/delete (see adapter.cpp); keep stdunk.h from defining them.
#define _NEW_DELETE_OPERATORS_

#include <portcls.h>
#include <stdunk.h>
#include <ksmedia.h>

#include "compat.h"
#include "pinnames.h"

#define S2M_POOLTAG          'M2kS'

// Kernel-pool new/delete (implemented in adapter.cpp). Memory comes back zeroed.
void* __cdecl operator new(size_t Size, POOL_FLAGS Flags, ULONG Tag);
void __cdecl operator delete(void* Ptr);
void __cdecl operator delete(void* Ptr, size_t Size);
void __cdecl operator delete[](void* Ptr);

// Subdevice (filter) indices inside one cable.
enum S2M_SUBDEVICE
{
    S2mWaveRender = 0,
    S2mTopoRender,
    S2mWaveCapture,
    S2mTopoCapture,
    S2mSubdevicesPerCable
};

// Driver settings, read from HKLM\SYSTEM\CurrentControlSet\Services\Speak2Mic\Parameters
// when the device starts. Changing them requires a device restart.
struct S2M_CONFIG
{
    ULONG CableCount;   // always S2M_MAX_CABLES (1)
    ULONG SampleRate;   // 8000..192000, the only rate the cable endpoints expose
    ULONG Channels;     // 1..8, "Speak2Mic Speaker" (render)
    ULONG MicChannels;  // 1..8, "Speak2Mic Microphone" (capture); setting 0 = same as Channels
    ULONG BitsPerSample; // preferred 16/24/32 (0 = auto); the pins always accept 16-32 bits
    ULONG LatencyMs;    // capture lags behind render by this much (jitter margin)
    ULONG ProposeFormat; // 1 = answer KSPROPERTY_PIN_PROPOSEDATAFORMAT on the filter, 0 = leave it "not found"
    LONG  MicVolume;    // microphone volume node, KS units (1/65536 dB); the node stores every change here
    ULONG MicMute;      // microphone mute node
};

// Driver-wide copy of S2M_CONFIG::ProposeFormat (set in StartDevice before the filters are built).
extern ULONG g_S2mProposeFormat;

// Short text of a data format for the driver log ("48000 Hz 24/32 bit 2 ch PCM ext mask 0x3").
void S2mFormatText(_In_ PKSDATAFORMAT DataFormat, _In_ ULONG BufferSize, _Out_writes_(Size) char* Out, _In_ ULONG Size);

// Frames elapsed for a QPC tick delta, without 64-bit overflow for long uptimes.
inline ULONGLONG S2mTicksToFrames(ULONGLONG ticks, ULONGLONG freq, ULONG rate)
{
    return (ticks / freq) * rate + ((ticks % freq) * rate) / freq;
}

// Stream sample layout, parsed from a KSDATAFORMAT_WAVEFORMATEX.
struct S2M_FORMAT
{
    ULONG SampleRate;
    ULONG Channels;
    ULONG ContainerBytes;   // 2, 3 or 4
    ULONG BlockAlign;       // Channels * ContainerBytes
};

// Smallest wave format: KSDATAFORMAT + plain WAVEFORMATEX (82 bytes; sizeof(KSDATAFORMAT_WAVEFORMATEX)
// is 88 because of padding, and plain PCM formats arrive with 82).
#define S2M_MIN_WAVE_FORMAT (sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEX))

NTSTATUS S2mParseFormat(_In_ PKSDATAFORMAT DataFormat, _Out_ S2M_FORMAT* Format);

class CCable;

NTSTATUS CreateMiniportWaveRT(_Out_ PUNKNOWN* Unknown, _In_ CCable* Cable, _In_ BOOLEAN Capture);
NTSTATUS CreateMiniportTopology(_Out_ PUNKNOWN* Unknown, _In_ CCable* Cable, _In_ BOOLEAN Capture);
