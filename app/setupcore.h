// Shared install/uninstall logic for s2minstall.exe (console) and Speak2Mic-Setup.exe (GUI).
// All functions need administrator rights.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Receives progress messages (one line each, no trailing newline).
typedef void (*SetupLog)(void* ctx, const wchar_t* line);

// Trusts the test certificates: the root CA (.cer) goes to the machine "Root" store, the code-signing
// (publisher) certificate to "TrustedPublisher".
bool SetupTrustCertificate(const wchar_t* rootCerPath, const wchar_t* publisherCerPath, SetupLog log, void* ctx);

// Creates ROOT\Speak2Mic if needed and installs the driver from `infPath` on it.
// *rebootNeeded is set when Windows asks for a restart.
bool SetupInstallDriver(const wchar_t* infPath, bool* rebootNeeded, SetupLog log, void* ctx);

// Removes every ROOT\Speak2Mic device and deletes the Speak2Mic driver package(s) from the driver store.
// keepSettings (update / reinstall): the signal quality settings survive for the new version; otherwise
// (uninstall) all driver settings are reset.
bool SetupRemoveDriver(SetupLog log, void* ctx, bool keepSettings = false);

// Removes the Speak2Mic sound endpoints the audio service keeps from earlier installations (all of ours but
// the active ones; Device Manager "Audio inputs and outputs"). Restarts the audio services when there are
// any. Returns how many were removed.
int SetupRemoveStaleEndpoints(SetupLog log, void* ctx);

// After an in-place driver update: removes the Speak2Mic driver packages the device no longer uses.
int SetupRemoveOldDriverPackages(SetupLog log, void* ctx);
// Clears the driver's status values (ProposeFormat: a new driver tries format proposals again) but keeps
// the user's settings.
void SetupResetDriverStatus();
// A ROOT\Speak2Mic device is present (not only a leftover registration).
bool SetupDevicePresent();

// Number of ROOT\Speak2Mic devices currently registered (present or not).
int SetupDeviceCount();

// Device node state of the first ROOT\Speak2Mic device. Returns false if there is none.
// *status = DN_* flags, *problem = CM_PROB_* code (0 = no problem), *instanceId = device instance ID.
bool SetupGetDeviceState(ULONG* status, ULONG* problem, wchar_t* instanceId, size_t idLen);

// Program files (setupfiles.cpp): %ProgramFiles%\Speak2Mic
bool SetupProgramDir(wchar_t* dir);                                         // MAX_PATH buffer
bool SetupInstallFiles(const wchar_t* sourceDir, SetupLog log, void* ctx);  // copies the package there
bool SetupCreateShortcuts(bool desktop, bool startMenu, SetupLog log, void* ctx);   // all-users shortcuts
// Removes shortcuts and program files; *deleteLater = the running installer is inside the folder,
// call SetupScheduleSelfDelete() right before exiting.
bool SetupRemoveFiles(bool* deleteLater, SetupLog log, void* ctx);
void SetupScheduleSelfDelete();

// Boot configuration checks.
bool SetupTestSigningEnabled();         // test signing mode active in the running system
bool SetupSecureBootEnabled();          // UEFI Secure Boot on (blocks test signing)
bool SetupEnableTestSigning(SetupLog log, void* ctx);   // "bcdedit /set testsigning on"
bool SetupSetTestSigning(bool on, SetupLog log, void* ctx);     // on or off; takes effect after a restart
// What the boot configuration says for the next start: 1 on, 0 off, -1 unknown (then assume the running state).
int  SetupTestSigningConfigured();
