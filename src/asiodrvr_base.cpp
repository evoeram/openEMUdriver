// openEMUdriver — AsioDriver base bits (the SDK ships asiodrvr.cpp as an
// "#error do not use this" stub, so we provide the minimal glue).
// SPDX-License-Identifier: LGPL-2.1-or-later
#include "asiodrvr.h" // already pulls in iasiodrv.h via asiosys/asio chain

// The IASIO interface IID as used by every ASIO host (Steinberg asio SDK).
// {8325F4A7-2CB7-4D0A-9DE9-3BE30180DC51}
static const IID kIID_IASIO =
    { 0x8325f4a7, 0x2cb7, 0x4d0a, { 0x9d, 0xe9, 0x3b, 0xe3, 0x01, 0x80, 0xdc, 0x51 } };

AsioDriver::AsioDriver(LPUNKNOWN pUnk, HRESULT* phr)
    : CUnknown((TCHAR*)L"openEMUdriver", pUnk, phr) {}

AsioDriver::~AsioDriver() {}

HRESULT AsioDriver::NonDelegatingQueryInterface(REFIID riid, void** ppvObject) {
    if (ppvObject == nullptr) return E_POINTER;
    if (riid == kIID_IASIO) return GetInterface((IASIO*)this, ppvObject);
    return CUnknown::NonDelegatingQueryInterface(riid, ppvObject);
}

// vtable defaults; every concrete driver (EmuAsioDriver) overrides these
ASIOBool AsioDriver::init(void*) { return ASIOFalse; }
void AsioDriver::getDriverName(char* name) { name[0] = 0; }
long AsioDriver::getDriverVersion() { return 0; }
void AsioDriver::getErrorMessage(char* s) { s[0] = 0; }
ASIOError AsioDriver::start() { return ASE_NotPresent; }
ASIOError AsioDriver::stop() { return ASE_NotPresent; }
ASIOError AsioDriver::getChannels(long*, long*) { return ASE_NotPresent; }
ASIOError AsioDriver::getLatencies(long*, long*) { return ASE_NotPresent; }
ASIOError AsioDriver::getBufferSize(long*, long*, long*, long*) { return ASE_NotPresent; }
ASIOError AsioDriver::canSampleRate(ASIOSampleRate) { return ASE_NotPresent; }
ASIOError AsioDriver::getSampleRate(ASIOSampleRate*) { return ASE_NotPresent; }
ASIOError AsioDriver::setSampleRate(ASIOSampleRate) { return ASE_NotPresent; }
ASIOError AsioDriver::getClockSources(ASIOClockSource*, long*) { return ASE_NotPresent; }
ASIOError AsioDriver::setClockSource(long) { return ASE_NotPresent; }
ASIOError AsioDriver::getSamplePosition(ASIOSamples*, ASIOTimeStamp*) { return ASE_NotPresent; }
ASIOError AsioDriver::getChannelInfo(ASIOChannelInfo*) { return ASE_NotPresent; }
ASIOError AsioDriver::createBuffers(ASIOBufferInfo*, long, long, ASIOCallbacks*) { return ASE_NotPresent; }
ASIOError AsioDriver::disposeBuffers() { return ASE_NotPresent; }
ASIOError AsioDriver::controlPanel() { return ASE_NotPresent; }
ASIOError AsioDriver::future(long, void*) { return ASE_NotPresent; }
ASIOError AsioDriver::outputReady() { return ASE_NotPresent; }
