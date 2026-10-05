// Program log: %ProgramData%\Speak2Mic\logs\<name>.log (UTF-8, time-stamped, thread-safe).
// Rotated to <name>.old.log when it grows beyond 1 MB.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

void AppLogOpen(const wchar_t* name);                   // once, at program start
void AppLog(const wchar_t* fmt, ...);                   // one line
void AppLogText(const wchar_t* prefix, const wchar_t* text);   // multi-line block, each line prefixed
const wchar_t* AppLogPath();                            // full path of the current log file
const wchar_t* AppLogDir();                             // folder with all Speak2Mic logs
