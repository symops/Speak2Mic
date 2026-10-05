// Replacement for the WDK's stdunk.lib (CUnknown) plus C++ runtime stubs, for the MinGW build only.
#include "../common.h"

CUnknown::CUnknown(PUNKNOWN pUnknownOuter)
    : m_ref_count(0)
{
    // Without an outer object, delegate IUnknown calls to our own INonDelegatingUnknown
    // (same vtable layout: QueryInterface, AddRef, Release).
    m_outer_unknown = pUnknownOuter ? pUnknownOuter : (PUNKNOWN)(PNONDELEGATINGUNKNOWN)this;
}

CUnknown::~CUnknown()
{
}

STDMETHODIMP_(ULONG) CUnknown::NonDelegatingAddRef()
{
    return (ULONG)InterlockedIncrement(&m_ref_count);
}

STDMETHODIMP_(ULONG) CUnknown::NonDelegatingRelease()
{
    LONG r = InterlockedDecrement(&m_ref_count);
    if (r == 0)
    {
        m_ref_count = 1;    // guard against re-entry from the destructor
        delete this;
    }
    return (ULONG)r;
}

STDMETHODIMP_(NTSTATUS) CUnknown::NonDelegatingQueryInterface(REFIID rIID, PVOID* ppVoid)
{
    if (IsEqualGUIDAligned(rIID, IID_IUnknown))
    {
        *ppVoid = (PVOID)(PNONDELEGATINGUNKNOWN)this;
        NonDelegatingAddRef();
        return STATUS_SUCCESS;
    }
    *ppVoid = nullptr;
    return STATUS_INVALID_PARAMETER;
}

// Called if a pure virtual function is ever invoked; must never happen.
extern "C" void __cxa_pure_virtual()
{
    KeBugCheckEx(0xDEADDEAD, 0, 0, 0, 0);
}
