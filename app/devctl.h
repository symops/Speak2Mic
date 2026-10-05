// Speak2Mic device control shared by the panel, the installer and s2mdebug: driver settings in the
// registry and the device restart that makes the driver re-read them. Needs administrator rights.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define S2M_PARAMS_KEY  L"SYSTEM\\CurrentControlSet\\Services\\Speak2Mic\\Parameters"
#define S2M_HARDWARE_ID L"ROOT\\Speak2Mic"

// Disable + enable of every ROOT\Speak2Mic device (like Device Manager). The driver is torn down
// completely and re-creates its filters with the new settings. (DICS_PROPCHANGE only sends
// STOP/START, which keeps the filters PortCls already registered, i.e. the old sample rate.)
// Stop Windows Audio around it (AudioStopServices), audiodg.exe vetoes the disable otherwise.
bool S2mRestartDevice(bool* found, bool* reboot);

// Sound endpoints Windows keeps after they are gone (older versions, earlier names such as "Cable 1 In
// (Virtual Audio Cable)", renamed ones): not present and belonging to this driver's filters (see devctl.cpp);
// Device Manager lists them under "Audio inputs and outputs".
// remove = false: only counts them (no admin rights needed); true: removes them (needs admin rights).
int S2mStaleEndpoints(bool remove);

// Device Manager keeps the name a Speak2Mic endpoint had when its device node was created; renaming it
// (panel, Sound settings) does not update that. Counts (fix = false, no admin rights needed) or updates
// (fix = true, needs admin rights) the present Speak2Mic endpoint nodes whose name differs.
int S2mSyncEndpointNames(bool fix);

// Endpoints the audio service keeps for every Speak2Mic endpoint it ever created (each earlier reinstall left a
// pair): every endpoint of ours except the active ones. Call while the audio services run (it asks them which
// endpoints are active). ids: "{0.0.0.00000000}.{guid}". Returns the count.
// Returns -1 while our speaker or microphone is not active (e.g. during a device restart): nothing can be judged.
int S2mFindOrphanEndpoints(wchar_t (*ids)[80], int max, bool log = true);
// Deletes their records (MMDevices registry, needs admin: backup/restore privileges) and device nodes.
// Stop AudioEndpointBuilder first, or it brings them back. Returns how many were removed.
int S2mRemoveEndpoints(wchar_t (*ids)[80], int n);
// Finds and removes leftover endpoint records of ours: waits up to waitMs for our endpoints to be up (nothing can be
// judged before), keeps only what a second look a second later still finds, stops the endpoint builder meanwhile.
// Needs admin. The first device restart after an installation leaves the pair the installation created; every
// device restart (Apply) calls this. Returns how many were removed.
int S2mCleanupOrphanEndpoints(DWORD waitMs);

// Writes every node of the AudioEndpoint class (present or not, ours or not, name, instance ID) to the log.
void S2mLogEndpointNodes();

// What the Speak2Mic programs need before they can do anything: the driver is test-signed, so Windows loads it only
// in test signing mode, which needs Secure Boot off; and the driver (its device) must be installed. The panel,
// s2mctl, s2mdebug and the autotest show the matching notice and refuse to run; the installers do not (they are what
// fixes it). With a Microsoft-signed driver only S2mNotReadyDriver would remain.
enum S2mNotReady { S2mReadyOk = 0, S2mNotReadySecureBoot, S2mNotReadyTestMode, S2mNotReadyDriver };
bool S2mSecureBootEnabled();        // UEFI Secure Boot on
bool S2mTestSigningEnabled();       // test signing mode active in the running system
bool S2mDriverInstalled();          // a ROOT\Speak2Mic device exists
S2mNotReady S2mCheckReady();        // the first thing that is wrong, in that order
const wchar_t* S2mNotReadyTextEn(S2mNotReady reason);   // English, one line (console tools); ready.h has the translated one

// DWORD value under S2M_PARAMS_KEY; `def` if missing.
DWORD S2mGetParam(const wchar_t* name, DWORD def);
bool S2mSetParam(const wchar_t* name, DWORD value);
