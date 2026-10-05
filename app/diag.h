// Diagnostics of an installed Speak2Mic: device node, driver service, driver log, sound endpoints.
// Summary lines go to `log` (shown to the user); details go only to the program log file (applog).
#pragma once

#include "setupcore.h"

enum DiagResult
{
    DiagNotInstalled,       // no ROOT\Speak2Mic device
    DiagDeviceProblem,      // device exists but is not started (problem code)
    DiagNoEndpoints,        // driver runs, but Windows created no Speak2Mic sound devices
    DiagEndpointsInactive,  // sound devices exist but are disabled/unplugged
    DiagOk,                 // "Speak2Mic Speaker" / "Speak2Mic Microphone" are active
};

DiagResult RunDiagnostics(SetupLog log, void* ctx);

// Version of the driver that last started (from its log), e.g. "1.0.269.795"; false if unknown.
bool DiagRunningDriverVersion(wchar_t* out, size_t len);
// Driver version of the Speak2Mic.inf next to this program; false if there is none.
bool DiagPackageDriverVersion(wchar_t* out, size_t len);

// Opens the driver's KS filters like the Audio Endpoint Builder does and logs what it sees
// (interfaces, pins, physical connections, format proposals), plus audio services and events.
void DiagDeepProbe(SetupLog log, void* ctx, const wchar_t* instanceId);

// Sets the Speak2Mic microphone to the volume saved by the control panel, or 0 dB (no gain) if there is none.
// Windows gives a microphone without a hardware volume the range -96..+30 dB and puts it at +30 dB after
// an install / update, which overdrives the cable's signal.
int  DiagSetMicUnityGain();                     // returns the number of Speak2Mic microphones set

// Waits up to `timeoutMs` for the device to start and its sound endpoints to appear.
DiagResult WaitForEndpoints(DWORD timeoutMs);
