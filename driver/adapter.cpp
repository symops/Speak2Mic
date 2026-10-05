// Driver entry, device start and subdevice registration.
#include "common.h"
#include "cable.h"
#include "minwave.h"
#include "mintopo.h"
#include "log.h"
#include "version.h"

extern "C" DRIVER_INITIALIZE DriverEntry;

// ---------------------------------------------------------------------------
// Kernel-pool new/delete

void* __cdecl operator new(size_t Size, POOL_FLAGS Flags, ULONG Tag)
{
    return ExAllocatePool2(Flags, Size, Tag);   // zero-initialized
}

void __cdecl operator delete(void* Ptr)
{
    if (Ptr)
    {
        ExFreePool(Ptr);
    }
}

void __cdecl operator delete(void* Ptr, size_t Size)
{
    UNREFERENCED_PARAMETER(Size);
    if (Ptr)
    {
        ExFreePool(Ptr);
    }
}

void __cdecl operator delete[](void* Ptr)
{
    if (Ptr)
    {
        ExFreePool(Ptr);
    }
}

// ---------------------------------------------------------------------------
// Settings

static ULONG Clamp(ULONG v, ULONG lo, ULONG hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void ReadConfig(_Out_ S2M_CONFIG* Config)
{
    ULONG sampleRate = 48000, channels = 2, latencyMs = 30, bits = 16;   // defaults = preset "Standard"
    ULONG propose = 1;
    ULONG micChannels = 1, defMicChannels = micChannels;     // mono by default; 0 = same as the speaker
    ULONG defRate = sampleRate, defChannels = channels, defLatency = latencyMs, defBits = bits;
    ULONG defPropose = propose;
    ULONG micVolume = 0, micMute = 0, defMicVolume = 0, defMicMute = 0;     // 0 dB, not muted

    RTL_QUERY_REGISTRY_TABLE table[9];
    RtlZeroMemory(table, sizeof(table));

    const ULONG flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK;
    const ULONG type = (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_DWORD;

    table[0].Flags = flags; table[0].Name = (PWSTR)L"SampleRate"; table[0].EntryContext = &sampleRate;
    table[0].DefaultType = type; table[0].DefaultData = &defRate; table[0].DefaultLength = sizeof(ULONG);
    table[1].Flags = flags; table[1].Name = (PWSTR)L"Channels"; table[1].EntryContext = &channels;
    table[1].DefaultType = type; table[1].DefaultData = &defChannels; table[1].DefaultLength = sizeof(ULONG);
    table[2].Flags = flags; table[2].Name = (PWSTR)L"LatencyMs"; table[2].EntryContext = &latencyMs;
    table[2].DefaultType = type; table[2].DefaultData = &defLatency; table[2].DefaultLength = sizeof(ULONG);
    table[3].Flags = flags; table[3].Name = (PWSTR)L"BitsPerSample"; table[3].EntryContext = &bits;
    table[3].DefaultType = type; table[3].DefaultData = &defBits; table[3].DefaultLength = sizeof(ULONG);
    table[4].Flags = flags; table[4].Name = (PWSTR)L"ProposeFormat"; table[4].EntryContext = &propose;
    table[4].DefaultType = type; table[4].DefaultData = &defPropose; table[4].DefaultLength = sizeof(ULONG);
    table[5].Flags = flags; table[5].Name = (PWSTR)L"MicChannels"; table[5].EntryContext = &micChannels;
    table[5].DefaultType = type; table[5].DefaultData = &defMicChannels; table[5].DefaultLength = sizeof(ULONG);
    // Written by the microphone's volume / mute nodes (mintopo.cpp): the node starts where it was left, so a
    // device restart does not depend on Windows re-applying the endpoint volume in time.
    table[6].Flags = flags; table[6].Name = (PWSTR)L"MicVolume"; table[6].EntryContext = &micVolume;
    table[6].DefaultType = type; table[6].DefaultData = &defMicVolume; table[6].DefaultLength = sizeof(ULONG);
    table[7].Flags = flags; table[7].Name = (PWSTR)L"MicMute"; table[7].EntryContext = &micMute;
    table[7].DefaultType = type; table[7].DefaultData = &defMicMute; table[7].DefaultLength = sizeof(ULONG);

    // A missing key or value simply leaves the defaults in place.
    NTSTATUS rs = RtlQueryRegistryValues(RTL_REGISTRY_SERVICES, L"Speak2Mic\\Parameters", table, nullptr, nullptr);
    if (!NT_SUCCESS(rs))
    {
        S2mLog("Config: registry read failed 0x%08lX, using defaults", (ULONG)rs);
    }

    Config->CableCount = S2M_MAX_CABLES;     // one cable: "Speak2Mic Speaker" -> "Speak2Mic Microphone"
    Config->SampleRate = Clamp(sampleRate, 8000, 192000);
    Config->Channels = Clamp(channels, 1, 8);
    Config->MicChannels = micChannels ? Clamp(micChannels, 1, 8) : Config->Channels;   // 0 = as the speaker
    Config->LatencyMs = Clamp(latencyMs, 10, 500);
    Config->BitsPerSample = (bits == 16 || bits == 24 || bits == 32) ? bits : 0;
    Config->ProposeFormat = propose ? 1 : 0;
    Config->MicVolume = (LONG)micVolume;         // clamped by the cable
    // Mute is NOT taken over a restart: when Windows' own idea of the mute state differed from the stored one (a
    // change shortly before the restart, another program), the microphone stayed silent while Windows showed it
    // unmuted, and Windows never sent "unmute" again (it thought it was already off). The node starts unmuted;
    // Windows re-applies the endpoint's mute state when it starts the endpoint.
    UNREFERENCED_PARAMETER(micMute);
    Config->MicMute = 0;

    S2mLog("Config: cables=%lu rate=%lu channels=%lu mic channels=%lu bits=%lu latency=%lums propose=%lu",
           Config->CableCount, Config->SampleRate, Config->Channels, Config->MicChannels, Config->BitsPerSample, Config->LatencyMs,
           Config->ProposeFormat);
    S2mLog("Microphone node at start: volume %ld/65536 dB, mute %lu", Config->MicVolume, Config->MicMute);
}

// ---------------------------------------------------------------------------
// Subdevices

// Creates a port, binds the miniport to it and registers it as a named subdevice.
// On success *Port holds a reference the caller must release.
static NTSTATUS InstallSubdevice(
    _In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp, _In_ PCWSTR Name, _In_ REFGUID PortClass,
    _In_ PUNKNOWN Miniport, _In_opt_ PRESOURCELIST ResourceList, _Out_ PUNKNOWN* Port)
{
    *Port = nullptr;

    PPORT port = nullptr;
    NTSTATUS status = PcNewPort(&port, PortClass);
    if (!NT_SUCCESS(status))
    {
        S2mLog("%ls: PcNewPort failed 0x%08lX", Name, (ULONG)status);
        return status;
    }

    status = port->Init(DeviceObject, Irp, Miniport, nullptr, ResourceList);
    if (!NT_SUCCESS(status))
    {
        S2mLog("%ls: port Init failed 0x%08lX", Name, (ULONG)status);
    }
    else
    {
        status = PcRegisterSubdevice(DeviceObject, (PWSTR)Name, port);
        if (!NT_SUCCESS(status))
            S2mLog("%ls: PcRegisterSubdevice failed 0x%08lX", Name, (ULONG)status);
        else
            S2mLog("%ls: registered", Name);
    }

    if (!NT_SUCCESS(status))
    {
        port->Release();
        return status;
    }

    *Port = port;
    return STATUS_SUCCESS;
}

// Registers the four filters of one cable and the physical connections between them:
//   WaveRenderN.pin1  -> TopoRenderN.pin0      (render: engine -> wave -> "Speak2Mic Speaker" jack)
//   TopoCaptureN.pin1 -> WaveCaptureN.pin0     (capture: "Speak2Mic Microphone" mic jack -> wave -> engine)
static NTSTATUS InstallCable(
    _In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp, _In_opt_ PRESOURCELIST ResourceList,
    _In_ ULONG Index, _In_ const S2M_CONFIG& Config)
{
    CCable* cable = nullptr;
    NTSTATUS status = CCable::Create(Index, Config, &cable);
    if (!NT_SUCCESS(status))
    {
        S2mLog("Cable %lu: ring buffer allocation failed 0x%08lX", Index + 1, (ULONG)status);
        return status;
    }

    PUNKNOWN ports[S2mSubdevicesPerCable] = {};

    for (ULONG s = 0; s < S2mSubdevicesPerCable && NT_SUCCESS(status); s++)
    {
        BOOLEAN capture = (s == S2mWaveCapture || s == S2mTopoCapture);
        BOOLEAN wave = (s == S2mWaveRender || s == S2mWaveCapture);

        PUNKNOWN miniport = nullptr;
        status = wave ? CreateMiniportWaveRT(&miniport, cable, capture)
                      : CreateMiniportTopology(&miniport, cable, capture);
        if (!NT_SUCCESS(status))
        {
            S2mLog("Cable %lu: miniport %lu creation failed 0x%08lX", Index + 1, s, (ULONG)status);
            break;
        }

        // The name must stay valid while the subdevice exists: PortCls stores the pointer (static table).
        status = InstallSubdevice(DeviceObject, Irp, g_S2mSubdeviceNames[Index][s], wave ? CLSID_PortWaveRT : CLSID_PortTopology,
                                  miniport, ResourceList, &ports[s]);
        miniport->Release();    // the port keeps its own reference
    }

    if (NT_SUCCESS(status))
    {
        status = PcRegisterPhysicalConnection(DeviceObject, ports[S2mWaveRender], S2M_WAVE_RENDER_BRIDGE_PIN,
                                              ports[S2mTopoRender], S2M_TOPO_RENDER_BRIDGE_PIN);
        if (!NT_SUCCESS(status))
            S2mLog("Cable %lu: render physical connection failed 0x%08lX", Index + 1, (ULONG)status);
    }
    if (NT_SUCCESS(status))
    {
        status = PcRegisterPhysicalConnection(DeviceObject, ports[S2mTopoCapture], S2M_TOPO_CAPTURE_BRIDGE_PIN,
                                              ports[S2mWaveCapture], S2M_WAVE_CAPTURE_BRIDGE_PIN);
        if (!NT_SUCCESS(status))
            S2mLog("Cable %lu: capture physical connection failed 0x%08lX", Index + 1, (ULONG)status);
    }
    if (NT_SUCCESS(status))
    {
        S2mLog("Cable %lu: ready", Index + 1);
    }

    for (ULONG s = 0; s < S2mSubdevicesPerCable; s++)
    {
        if (ports[s])
        {
            ports[s]->Release();
        }
    }
    cable->Release();           // the wave miniports hold their own references
    return status;
}

static NTSTATUS NTAPI StartDevice(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp, _In_opt_ PRESOURCELIST ResourceList)
{
    S2mLog("StartDevice");
    S2M_CONFIG config;
    ReadConfig(&config);
    g_S2mProposeFormat = config.ProposeFormat;

    NTSTATUS status = STATUS_SUCCESS;
    ULONG created = 0;
    for (ULONG i = 0; i < config.CableCount && NT_SUCCESS(status); i++)
    {
        status = InstallCable(DeviceObject, Irp, ResourceList, i, config);
        if (NT_SUCCESS(status)) created++;
    }

    S2mLog("StartDevice: %lu of %lu cable(s) created, status 0x%08lX", created, config.CableCount, (ULONG)status);
    S2mLogSetValue(L"StartStatus", (ULONG)status);
    S2mLogSetValue(L"CablesCreated", created);
    S2mLogFlush();
    return status;
}

static NTSTATUS NTAPI AddDevice(_In_ PDRIVER_OBJECT DriverObject, _In_ PDEVICE_OBJECT PhysicalDeviceObject)
{
    NTSTATUS status = PcAddAdapterDevice(DriverObject, PhysicalDeviceObject, (PCPFNSTARTDEVICE)StartDevice,
                                         S2M_MAX_CABLES * S2mSubdevicesPerCable, 0);
    S2mLog("AddDevice: 0x%08lX", (ULONG)status);
    S2mLogFlush();
    return status;
}

extern "C" NTSTATUS NTAPI DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    S2mLogInit();
    S2mLog("DriverEntry: Speak2Mic " S2M_VER_STR ", built " __DATE__ " " __TIME__);
    NTSTATUS status = PcInitializeAdapterDriver(DriverObject, RegistryPath, (PDRIVER_ADD_DEVICE)AddDevice);
    S2mLog("DriverEntry: PcInitializeAdapterDriver 0x%08lX", (ULONG)status);
    S2mLogFlush();
    return status;
}
