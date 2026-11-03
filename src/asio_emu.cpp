// openEMUdriver — ASIO 2 driver implementation (IASIO) on top of the
// WinUSB isochronous engine. Direct USB path: no WDM/KS layering.
//
// SPDX-License-Identifier: LGPL-2.1-or-later
// ASIO interface plumbing uses the Steinberg ASIO SDK (common/) per its license.
#include <windows.h>
#include "asiosys.h"
#include "asio.h"
#include "asiodrvr.h"
#include "emu_usb.h"
#include "emu_stream.h"

#include <stdio.h>
#include <string.h>
#include <olectl.h>

// {C3F1E7A8-6B2D-4C49-9A5E-8D2F10B7C604}
static const GUID CLSID_OpenEmuAsio =
    { 0xc3f1e7a8, 0x6b2d, 0x4c49, { 0x9a, 0x5e, 0x8d, 0x2f, 0x10, 0xb7, 0xc6, 0x04 } };

static int kOedAnchor = 0; // address anchor for GetModuleHandleEx

namespace {

const long kMinBuf = 64;
const long kMaxBuf = 4096;
const long kPrefBuf = 256;
const int  kMaxCh = 4;

const char* kInNames[kMaxCh]  = { "Analog IN A", "Analog IN B", "SPDIF IN L", "SPDIF IN R" };
const char* kOutNames[kMaxCh] = { "Analog OUT L", "Analog OUT R", "SPDIF OUT L", "SPDIF OUT R" };

LONG readPrefDWORD(const char* name, LONG def) {
    const char* keys[2] = { "SOFTWARE\\ASIO\\openEMUdriver", "SOFTWARE\\Wow6432Node\\ASIO\\openEMUdriver" };
    for (int i = 0; i < 2; i++) {
        HKEY k;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, keys[i], 0, KEY_READ, &k) != ERROR_SUCCESS)
            if (RegOpenKeyExA(HKEY_CURRENT_USER, keys[i], 0, KEY_READ, &k) != ERROR_SUCCESS)
                continue;
        DWORD v = 0, sz = sizeof(v), type = 0;
        LONG r = RegQueryValueExA(k, name, nullptr, &type, (BYTE*)&v, &sz);
        RegCloseKey(k);
        if (r == ERROR_SUCCESS && (type == REG_DWORD || type == REG_QWORD) && v > 0) return (LONG)v;
    }
    return def;
}

class EmuAsioDriver : public AsioDriver {
public:
    EmuAsioDriver(LPUNKNOWN pUnk, HRESULT* phr) : AsioDriver(pUnk, phr) {
        InitializeCriticalSection(&m_lock);
        memset(m_asioBuf, 0, sizeof(m_asioBuf));
    }
    virtual ~EmuAsioDriver() {
        teardown();
        DeleteCriticalSection(&m_lock);
    }

    static CUnknown* CreateInstance(LPUNKNOWN pUnk, HRESULT* phr) {
        return new EmuAsioDriver(pUnk, phr);
    }

    ASIOBool init(void*) override;
    void getDriverName(char* name) override { strcpy_s(name, 32, "openEMUdriver"); }
    long getDriverVersion() override { return 100; }
    void getErrorMessage(char* string) override { strcpy_s(string, 124, m_err[0] ? m_err : "Unknown error"); }

    ASIOError start() override;
    ASIOError stop() override;
    ASIOError getChannels(long* ni, long* no) override {
        if (ni) *ni = m_inCh; if (no) *no = m_outCh; return ASE_OK;
    }
    ASIOError getLatencies(long* in, long* out) override;
    ASIOError getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) override;
    ASIOError canSampleRate(ASIOSampleRate rate) override;
    ASIOError getSampleRate(ASIOSampleRate* rate) override {
        if (!rate) return ASE_InvalidParameter;
        *rate = m_rate; return ASE_OK;
    }
    ASIOError setSampleRate(ASIOSampleRate rate) override;
    ASIOError getClockSources(ASIOClockSource* clocks, long* num) override;
    ASIOError setClockSource(long ref) override;
    ASIOError getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) override;
    ASIOError getChannelInfo(ASIOChannelInfo* info) override;
    ASIOError createBuffers(ASIOBufferInfo* infos, long numChannels, long bufferSize,
                            ASIOCallbacks* cb) override;
    ASIOError disposeBuffers() override;
    ASIOError controlPanel() override;
    ASIOError future(long, void*) override { return ASE_NotPresent; }
    ASIOError outputReady() override { return ASE_OK; }

private:
    void teardown();
    static void switchTrampoline(void* ctx, uint64_t) { ((EmuAsioDriver*)ctx)->onSwitch(); }
    void onSwitch();

    CRITICAL_SECTION m_lock;
    oed::DeviceInfo  m_info{};
    oed::EmuDevice   m_dev;
    oed::EmuStream   m_stream;

    bool    m_inited = false;
    char    m_err[128] = {0};
    double  m_rate = 48000.0;
    int     m_inCh = 2, m_outCh = 2;
    long    m_bufSize = 0;
    volatile int m_bufSelector = 0;
    volatile bool m_started = false;
    LARGE_INTEGER m_qpf{};

    ASIOCallbacks* m_cb = nullptr;
    long  m_activeOut = 0, m_activeIn = 0;
    long  m_chIndex[2][kMaxCh] = {{0}};
    int32_t* m_asioBuf[2][kMaxCh][2] = {{{nullptr}}};
    uint8_t* m_tmp[2] = {nullptr, nullptr};
    long     m_tmpBytes = 0;
};

// ---------------------------------------------------------------------------

ASIOBool EmuAsioDriver::init(void*) {
    EnterCriticalSection(&m_lock);
    QueryPerformanceFrequency(&m_qpf);
    do {
        oed::DeviceInfo list[8];
        int n = oed::enumerateDevices(list, 8);
        if (n <= 0) {
            strcpy_s(m_err, "No E-MU USB device found (0202/0404/Tracker Pre with openEMUdriver INF installed)");
            break;
        }
        bool ok = false;
        for (int i = 0; i < n; i++)
            if (m_dev.open(list[i])) { m_info = list[i]; ok = true; break; }
        if (!ok) { strcpy_s(m_err, "E-MU USB device found but cannot be opened (driver binding?)"); break; }

        m_inCh = m_outCh = (m_info.model == oed::MODEL_0404) ? 4 : 2;
        m_rate = 48000.0;
        m_inited = true;
        LeaveCriticalSection(&m_lock);
        return ASIOTrue;
    } while (0);
    LeaveCriticalSection(&m_lock);
    return ASIOFalse;
}

ASIOError EmuAsioDriver::getLatencies(long* in, long* out) {
    long queue = readPrefDWORD("QueueMs", 4) * (long)m_rate / 1000;
    if (in)  *in  = m_started ? m_bufSize + queue : m_bufSize;
    if (out) *out = m_started ? m_bufSize + queue : m_bufSize;
    return ASE_OK;
}

ASIOError EmuAsioDriver::getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) {
    if (minSize) *minSize = kMinBuf;
    if (maxSize) *maxSize = kMaxBuf;
    if (preferredSize) *preferredSize = m_bufSize ? m_bufSize : readPrefDWORD("PreferredBufferSize", kPrefBuf);
    if (granularity) *granularity = -1; // power-of-2
    return ASE_OK;
}

ASIOError EmuAsioDriver::canSampleRate(ASIOSampleRate rate) {
    if (!m_inited) return ASE_NotPresent;
    for (int dir = 0; dir < 2; dir++) {
        const oed::StreamFormat* fs = m_dev.formats(dir);
        int n = m_dev.formatCount(dir);
        bool found = false;
        for (int i = 0; i < n; i++) if (fs[i].rate == (uint32_t)rate) { found = true; break; }
        if (!found) return ASE_NoClock;
    }
    return ASE_OK;
}

ASIOError EmuAsioDriver::setSampleRate(ASIOSampleRate rate) {
    if (!m_inited) return ASE_NotPresent;
    if (m_started) return ASE_HWMalfunction;
    if (canSampleRate(rate) != ASE_OK) return ASE_NoClock;
    EnterCriticalSection(&m_lock);
    m_dev.setClockRate((uint32_t)rate);
    m_rate = rate;
    LeaveCriticalSection(&m_lock);
    return ASE_OK;
}

ASIOError EmuAsioDriver::getClockSources(ASIOClockSource* clocks, long* num) {
    if (!clocks || !num) return ASE_InvalidParameter;
    long i = 0;
    clocks[i].index = i; clocks[i].associatedChannel = -1;
    clocks[i].associatedGroup = 0; clocks[i].isCurrentSource = ASIOTrue;
    strcpy_s(clocks[i].name, 32, "Internal");
    i++;
    if (m_dev.xuUnit(oed::XU_CLOCK_SOURCE)) {
        clocks[i].index = i; clocks[i].associatedChannel = -1;
        clocks[i].associatedGroup = 0; clocks[i].isCurrentSource = ASIOFalse;
        strcpy_s(clocks[i].name, 32, "S/PDIF");
        i++;
    }
    *num = i;
    return ASE_OK;
}

ASIOError EmuAsioDriver::setClockSource(long ref) {
    if (!m_inited) return ASE_NotPresent;
    if (ref == 0) return m_dev.setClockSource(0) ? ASE_OK : ASE_HWMalfunction;
    if (ref == 1 && m_dev.xuUnit(oed::XU_CLOCK_SOURCE))
        return m_dev.setClockSource(1) ? ASE_OK : ASE_HWMalfunction;
    return ASE_InvalidParameter;
}

ASIOError EmuAsioDriver::getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) {
    if (tStamp) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double ns = (double)now.QuadPart * 1e9 / (double)(m_qpf.QuadPart ? m_qpf.QuadPart : 1);
        unsigned long long u = (unsigned long long)ns;
        tStamp->lo = (unsigned long)(u & 0xFFFFFFFF);
        tStamp->hi = (unsigned long)(u >> 32);
    }
    if (sPos) {
        uint64_t f = m_stream.framesDone();
        sPos->lo = (unsigned long)(f & 0xFFFFFFFF);
        sPos->hi = (unsigned long)(f >> 32);
    }
    return ASE_OK;
}

ASIOError EmuAsioDriver::getChannelInfo(ASIOChannelInfo* info) {
    if (!info) return ASE_InvalidParameter;
    int ch = (int)info->channel;
    if (ch < 0) return ASE_InvalidParameter;
    if (info->isInput) {
        if (ch >= m_inCh) return ASE_InvalidParameter;
        info->type = ASIOSTInt32LSB;
        info->channelGroup = 0;
        info->isActive = ASIOTrue;
        strcpy_s(info->name, 32, kInNames[ch]);
    } else {
        if (ch >= m_outCh) return ASE_InvalidParameter;
        info->type = ASIOSTInt32LSB;
        info->channelGroup = 0;
        info->isActive = ASIOTrue;
        strcpy_s(info->name, 32, kOutNames[ch]);
    }
    return ASE_OK;
}

ASIOError EmuAsioDriver::createBuffers(ASIOBufferInfo* infos, long numChannels,
                                       long bufferSize, ASIOCallbacks* cb) {
    if (!m_inited) return ASE_NotPresent;
    if (!infos || !cb || numChannels <= 0) return ASE_InvalidParameter;
    if (bufferSize < kMinBuf || bufferSize > kMaxBuf) return ASE_InvalidMode;

    EnterCriticalSection(&m_lock);
    bool ok = true;
    for (long i = 0; i < numChannels && ok; i++) {
        ASIOBufferInfo& bi = infos[i];
        int dir = bi.isInput ? 1 : 0;
        long used = 0;
        // count already-claimed channels of this direction
        for (long j = 0; j < i; j++)
            if (infos[j].isInput == bi.isInput) used++;
        long maxCh = dir ? m_inCh : m_outCh;
        if (used >= maxCh || bi.channelNum < 0 || bi.channelNum >= maxCh) { ok = false; break; }
        long slot = used;
        m_chIndex[dir][slot] = bi.channelNum;
        for (int b = 0; b < 2; b++) {
            m_asioBuf[dir][slot][b] = new int32_t[bufferSize];
            memset(m_asioBuf[dir][slot][b], 0, (size_t)bufferSize * 4);
        }
        bi.buffers[0] = m_asioBuf[dir][slot][0];
        bi.buffers[1] = m_asioBuf[dir][slot][1];
        if (dir) m_activeIn = slot + 1; else m_activeOut = slot + 1;
    }
    if (ok) {
        m_tmpBytes = (long)bufferSize * 3 * kMaxCh + 64;
        m_tmp[0] = new uint8_t[m_tmpBytes];
        m_tmp[1] = new uint8_t[m_tmpBytes];
        m_bufSize = bufferSize;
        m_cb = cb;
        m_bufSelector = 0;
        LeaveCriticalSection(&m_lock);
        return ASE_OK;
    }
    LeaveCriticalSection(&m_lock);
    disposeBuffers();
    return ASE_InvalidParameter;
}

ASIOError EmuAsioDriver::disposeBuffers() {
    if (!m_inited) return ASE_NotPresent;
    if (m_started) stop();
    EnterCriticalSection(&m_lock);
    for (int dir = 0; dir < 2; dir++)
        for (int c = 0; c < kMaxCh; c++)
            for (int b = 0; b < 2; b++)
                if (m_asioBuf[dir][c][b]) { delete[] m_asioBuf[dir][c][b]; m_asioBuf[dir][c][b] = nullptr; }
    delete[] m_tmp[0]; m_tmp[0] = nullptr;
    delete[] m_tmp[1]; m_tmp[1] = nullptr;
    m_activeOut = m_activeIn = 0;
    m_cb = nullptr;
    m_bufSize = 0;
    LeaveCriticalSection(&m_lock);
    return ASE_OK;
}

ASIOError EmuAsioDriver::start() {
    if (!m_inited) return ASE_NotPresent;
    if (!m_cb || (!m_activeOut && !m_activeIn)) return ASE_InvalidParameter;
    if (m_started) return ASE_OK;
    EnterCriticalSection(&m_lock);
    do {
        m_dev.setClockRate((uint32_t)m_rate);
        oed::EmuStream::Config cfg;
        cfg.rate = (uint32_t)m_rate;
        // the USB frame always carries the full channel set of the interface
        cfg.outChannels = m_activeOut ? m_outCh : 0;
        cfg.inChannels  = m_activeIn  ? m_inCh  : 0;
        cfg.framesPerSwitch = (int)m_bufSize;
        cfg.queueMs = (int)readPrefDWORD("QueueMs", 4);
        cfg.onSwitch = &EmuAsioDriver::switchTrampoline;
        cfg.ctx = this;
        if (!m_stream.start(&m_dev, cfg)) {
            strcpy_s(m_err, "stream start failed (rate/format unsupported by device?)");
            break;
        }
        m_started = true;
        LeaveCriticalSection(&m_lock);
        return ASE_OK;
    } while (0);
    LeaveCriticalSection(&m_lock);
    return ASE_HWMalfunction;
}

ASIOError EmuAsioDriver::stop() {
    if (!m_inited) return ASE_NotPresent;
    if (m_started) {
        m_started = false;
        m_stream.stop();
    }
    return ASE_OK;
}

ASIOError EmuAsioDriver::controlPanel() {
    char path[MAX_PATH]{};
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&kOedAnchor, &self);
    GetModuleFileNameA(self, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), "openEMUPanel.exe");
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
        HINSTANCE h = ShellExecuteA(nullptr, "open", path, nullptr, nullptr, SW_SHOWNORMAL);
        if ((intptr_t)h > 32) return ASE_OK;
    }
    MessageBoxA(nullptr,
        "openEMU Panel not found next to the driver DLL.\n"
        "Settings (HKLM\\SOFTWARE\\ASIO\\openEMUdriver):\n"
        "  PreferredBufferSize (DWORD frames), QueueMs (DWORD).",
        "openEMUdriver", MB_OK | MB_ICONINFORMATION);
    return ASE_OK;
}

// ---------------------------------------------------------------------------
// switch callback (engine thread)
// ---------------------------------------------------------------------------

void EmuAsioDriver::onSwitch() {
    ASIOCallbacks* cb = m_cb;
    if (!cb) return;
    int sel = m_bufSelector;
    long S = m_bufSize;
    int usbCh = m_stream.usbChannels(0) ? m_stream.usbChannels(0) : 2;
    int bpf = usbCh * 3;

    if (m_activeOut && m_tmp[0]) {
        uint8_t* inter = m_tmp[0];
        memset(inter, 0, (size_t)S * bpf);
        for (long c = 0; c < m_activeOut; c++) {
            long devCh = m_chIndex[0][c];
            if (devCh >= usbCh) continue;
            const int32_t* src = m_asioBuf[0][c][sel];
            for (long i = 0; i < S; i++) {
                int32_t v = src[i] >> 8; // 32 -> 24
                uint8_t* d = inter + (size_t)i * bpf + devCh * 3;
                d[0] = (uint8_t)(v & 0xFF);
                d[1] = (uint8_t)((v >> 8) & 0xFF);
                d[2] = (uint8_t)((v >> 16) & 0xFF);
            }
        }
        m_stream.pushOut(inter, (int)S);
    }

    if (m_activeIn && m_tmp[1]) {
        uint8_t* inter = m_tmp[1];
        int usbChIn = m_stream.usbChannels(1) ? m_stream.usbChannels(1) : usbCh;
        int bpfIn = usbChIn * 3;
        m_stream.popIn(inter, (int)S);
        for (long c = 0; c < m_activeIn; c++) {
            long devCh = m_chIndex[1][c];
            if (devCh >= usbChIn) continue;
            int32_t* dst = m_asioBuf[1][c][sel];
            for (long i = 0; i < S; i++) {
                const uint8_t* s = inter + (size_t)i * bpfIn + devCh * 3;
                dst[i] = (int32_t)(((uint32_t)s[0] | ((uint32_t)s[1] << 8) | ((uint32_t)s[2] << 16)) << 8);
            }
        }
    }

    m_bufSelector = sel ^ 1;
    cb->bufferSwitch(sel ^ 1, ASIOFalse);
}

void EmuAsioDriver::teardown() {
    if (m_started) { m_started = false; m_stream.stop(); }
    m_dev.close();
    m_inited = false;
}

} // anonymous namespace

// SDK factory wiring ---------------------------------------------------------

CFactoryTemplate g_Templates[1] = {
    {
        L"openEMUdriver",
        &CLSID_OpenEmuAsio,
        EmuAsioDriver::CreateInstance,
        nullptr
    }
};
int g_cTemplates = 1;

// self-registration: COM class + HKLM\SOFTWARE\ASIO\<name>
extern LONG RegisterAsioDriver (CLSID,char *,char *,char *,char *);
extern LONG UnregisterAsioDriver (CLSID,char *,char *);

STDAPI DllRegisterServer() {
    char dllPath[MAX_PATH]{};
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&kOedAnchor, &self);
    GetModuleFileNameA(self, dllPath, MAX_PATH);
    // register COM class under HKCR\CLSID\... and the ASIO host key
    LONG r = RegisterAsioDriver(CLSID_OpenEmuAsio, dllPath, "openEMUdriver",
                                "openEMUdriver (E-MU 0202/0404/Tracker Pre USB)", "Both");
    // RegisterAsioDriver writes Description? ensure both keys:
    HKEY k;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\ASIO\\openEMUdriver", 0, KEY_WRITE, &k) == ERROR_SUCCESS) {
        RegSetValueExA(k, "Description", 0, REG_SZ, (const BYTE*)"openEMUdriver (E-MU 0202/0404/Tracker Pre USB)", 47);
        RegCloseKey(k);
    }
    return r == ERROR_SUCCESS ? S_OK : SELFREG_E_CLASS;
}

STDAPI DllUnregisterServer() {
    UnregisterAsioDriver(CLSID_OpenEmuAsio, "openEMUdriver", "openEMUdriver (E-MU 0202/0404/Tracker Pre USB)");
    RegDeleteKeyA(HKEY_LOCAL_MACHINE, "SOFTWARE\\ASIO\\openEMUdriver");
    return S_OK;
}
