// Driver log: a ring of text lines kept in memory, mirrored to the kernel debugger (DebugView) and
// saved to HKLM\SYSTEM\CurrentControlSet\Services\Speak2Mic\Parameters\DriverLog (REG_SZ) so the
// user-mode tools can show it. S2mLog may be called at IRQL <= DISPATCH_LEVEL; S2mLogFlush only at
// PASSIVE_LEVEL (it is a no-op otherwise, the lines are written by the next flush).
#pragma once

#include "common.h"

void S2mLogInit();
void S2mLog(_In_z_ _Printf_format_string_ const char* Format, ...);
void S2mLogFlush();

// Stores a DWORD next to the log (e.g. StartStatus, CablesCreated). PASSIVE_LEVEL only.
void S2mLogSetValue(_In_z_ PCWSTR Name, _In_ ULONG Value);
