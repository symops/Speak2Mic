// Pre-included only by check_syntax.sh to paper over MinGW DDK header gaps.
// None of this is used by the real Visual Studio + WDK build.
#pragma once
#define DECLSPEC_NOVTABLE __declspec(novtable)
#define DECLSPEC_NOTHROW __declspec(nothrow)
#ifndef interface
#define interface struct
#endif
typedef char TCHAR;

// intrin.h and wdm.h both define these; keep the wdm.h versions.
#define __INTRINSIC_DEFINED_InterlockedBitTestAndSet
#define __INTRINSIC_DEFINED_InterlockedBitTestAndReset

// Present in WDK ksmedia.h, missing in MinGW.
typedef struct { unsigned long FifoSize, ChipsetDelay, CodecDelay; } KSRTAUDIO_HWLATENCY, *PKSRTAUDIO_HWLATENCY;
typedef struct { void* Register; unsigned long Width; unsigned long long Numerator, Denominator; unsigned long Accuracy; }
    KSRTAUDIO_HWREGISTER, *PKSRTAUDIO_HWREGISTER;

// Present in WDK wdm.h, missing in MinGW.
#define RTL_QUERY_REGISTRY_TYPECHECK        0x00000100
#define RTL_QUERY_REGISTRY_TYPECHECK_SHIFT  24

// Present in WDK portcls.h, missing in MinGW.
#define IMP_IMiniportTopology \
    IMP_IMiniport; \
    STDMETHODIMP_(NTSTATUS) Init(PUNKNOWN UnknownAdapter, PRESOURCELIST ResourceList, PPORTTOPOLOGY Port)
