// Driver log implementation (see log.h).
#include "log.h"
#include <stdarg.h>

#define S2M_LOG_SIZE    65536           // characters kept (oldest lines are dropped)
#define S2M_PARAMS_KEY  L"Speak2Mic\\Parameters"

static KSPIN_LOCK g_logLock;
static char       g_log[S2M_LOG_SIZE];
static ULONG      g_logLen;
static BOOLEAN    g_logDirty;

extern "C" int __cdecl _vsnprintf(char* buffer, size_t count, const char* format, va_list args);
extern "C" int __cdecl _snprintf(char* buffer, size_t count, const char* format, ...);

void S2mLogInit()
{
    KeInitializeSpinLock(&g_logLock);
    g_logLen = 0;
    g_log[0] = 0;
}

void S2mLog(_In_z_ _Printf_format_string_ const char* Format, ...)
{
    char line[320];

    // Local wall-clock time stamp.
    LARGE_INTEGER sys, local;
    KeQuerySystemTimePrecise(&sys);
    ExSystemTimeToLocalTime(&sys, &local);
    TIME_FIELDS tf;
    RtlTimeToTimeFields(&local, &tf);

    int n = _snprintf(line, sizeof(line), "%04d-%02d-%02d %02d:%02d:%02d.%03d  ",
                      tf.Year, tf.Month, tf.Day, tf.Hour, tf.Minute, tf.Second, tf.Milliseconds);
    if (n < 0) n = 0;

    va_list args;
    va_start(args, Format);
    int m = _vsnprintf(line + n, sizeof(line) - n - 2, Format, args);
    va_end(args);
    if (m < 0) m = (int)(sizeof(line) - n - 3);     // truncated
    ULONG len = (ULONG)(n + m);
    line[len] = 0;

    DbgPrintEx(DPFLTR_IHVAUDIO_ID, DPFLTR_ERROR_LEVEL, "Speak2Mic: %s\n", line + n);

    line[len++] = '\n';
    line[len] = 0;

    KIRQL irql;
    KeAcquireSpinLock(&g_logLock, &irql);
    if (g_logLen + len >= S2M_LOG_SIZE)
    {
        // Drop the oldest half, cut at a line boundary.
        ULONG cut = g_logLen / 2;
        while (cut < g_logLen && g_log[cut] != '\n') cut++;
        if (cut < g_logLen) cut++;
        RtlMoveMemory(g_log, g_log + cut, g_logLen - cut);
        g_logLen -= cut;
    }
    RtlCopyMemory(g_log + g_logLen, line, len);
    g_logLen += len;
    g_log[g_logLen] = 0;
    g_logDirty = TRUE;
    KeReleaseSpinLock(&g_logLock, irql);
}

void S2mLogFlush()
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || !g_logDirty)
    {
        return;
    }

    // Snapshot the log as UTF-16 (the text is ASCII) with CRLF line ends.
    SIZE_T cap = (SIZE_T)(S2M_LOG_SIZE * 2 + 2) * sizeof(WCHAR);
    WCHAR* w = (WCHAR*)ExAllocatePool2(POOL_FLAG_NON_PAGED, cap, S2M_POOLTAG);
    if (!w)
    {
        return;
    }
    ULONG wl = 0;
    KIRQL irql;
    KeAcquireSpinLock(&g_logLock, &irql);
    for (ULONG i = 0; i < g_logLen; i++)
    {
        if (g_log[i] == '\n') w[wl++] = L'\r';
        w[wl++] = (WCHAR)(unsigned char)g_log[i];
    }
    w[wl] = 0;
    g_logDirty = FALSE;
    KeReleaseSpinLock(&g_logLock, irql);

    RtlWriteRegistryValue(RTL_REGISTRY_SERVICES, S2M_PARAMS_KEY, L"DriverLog", REG_SZ, w, (wl + 1) * sizeof(WCHAR));
    ExFreePoolWithTag(w, S2M_POOLTAG);
}

void S2mLogSetValue(_In_z_ PCWSTR Name, _In_ ULONG Value)
{
    if (KeGetCurrentIrql() == PASSIVE_LEVEL)
    {
        RtlWriteRegistryValue(RTL_REGISTRY_SERVICES, S2M_PARAMS_KEY, Name, REG_DWORD, &Value, sizeof(Value));
    }
}
