// openEMUdriver — device layer implementation
// SPDX-License-Identifier: LGPL-2.1-or-later
#include "emu_usb.h"
#include <string.h>
#include <stdio.h>
#include <usbiodef.h>

namespace oed {

// ---------------------------------------------------------------------------
// discovery
// ---------------------------------------------------------------------------

static EmuModel modelFromPid(uint16_t pid) {
    switch (pid) {
        case 0x3F02: return MODEL_0202;
        case 0x3F04: return MODEL_0404;
        case 0x3F0A: return MODEL_TRACKER;
        case 0x3F0C: return MODEL_0204;
        default:     return MODEL_UNKNOWN;
    }
}

static const char* modelName(EmuModel m) {
    switch (m) {
        case MODEL_0202:    return "E-MU 0202 USB";
        case MODEL_0404:    return "E-MU 0404 USB";
        case MODEL_TRACKER: return "E-MU Tracker Pre USB";
        case MODEL_0204:    return "E-MU 0204 USB";
        default:            return "E-MU USB Audio";
    }
}

// Device interface GUID assigned by inf/openemu.inf to the E-MU devices.
static const GUID kOedGuid =
    { 0x9d1a2c31, 0x4e6f, 0x5b7a, { 0x8c, 0x21, 0x3d, 0xe4, 0x6f, 0xa5, 0xb7, 0x08 } };

int enumerateDevices(DeviceInfo* out, int maxCount) {
    int found = 0;
    const GUID* guids[2] = { &kOedGuid, &GUID_DEVINTERFACE_USB_DEVICE };
    for (int g = 0; g < 2 && found < maxCount; g++) {
        HDEVINFO devInfo = SetupDiGetClassDevsA(guids[g], nullptr, nullptr,
                                                DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (devInfo == INVALID_HANDLE_VALUE) continue;

        SP_DEVICE_INTERFACE_DATA ifData{};
        ifData.cbSize = sizeof(ifData);
        for (DWORD idx = 0; SetupDiEnumDeviceInterfaces(devInfo, nullptr, guids[g], idx, &ifData); idx++) {
            DWORD need = 0;
            SetupDiGetDeviceInterfaceDetailA(devInfo, &ifData, nullptr, 0, &need, nullptr);
            if (!need) continue;
            struct Blob {
                SP_DEVICE_INTERFACE_DETAIL_DATA_A d;
                char extra[512];
            } blob{};
            memset(&blob, 0, sizeof(blob));
            blob.d.cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
            SP_DEVINFO_DATA devData{};
            devData.cbSize = sizeof(devData);
            if (!SetupDiGetDeviceInterfaceDetailA(devInfo, &ifData, &blob.d, need, nullptr, &devData))
                continue;

            char hwid[512]{};
            DWORD sz = 0, type = 0;
            if (!SetupDiGetDeviceRegistryPropertyA(devInfo, &devData, SPDRP_HARDWAREID, &type,
                                                   (PBYTE)hwid, sizeof(hwid), &sz))
                continue;
            uint16_t vid = 0, pid = 0;
            for (const char* h = hwid; *h; h += strlen(h) + 1) {
                unsigned v = 0, pr = 0;
                if (sscanf(h, "USB\\VID_%4x&PID_%4x", &v, &pr) == 2 && v == 0x041E) {
                    vid = (uint16_t)v; pid = (uint16_t)pr;
                    break;
                }
            }
            if (vid != 0x041E || modelFromPid(pid) == MODEL_UNKNOWN) continue;
            if (g == 1) {
                // generic USB GUID path opens the whole device, not a WinUSB
                // interface; only usable when oed GUID missed (device bound to
                // WinUSB via WCID). Skip in that case to avoid duplicates.
                bool dup = false;
                for (int i = 0; i < found; i++) if (out[i].pid == pid) dup = true;
                if (dup) continue;
            }

            DeviceInfo& di = out[found];
            memset(&di, 0, sizeof(di));
            mbstowcs(di.path, blob.d.DevicePath, 511);
            di.vid = vid; di.pid = pid;
            di.model = modelFromPid(pid);
            strncpy(di.name, modelName(di.model), sizeof(di.name) - 1);
            found++;
            if (found >= maxCount) break;
        }
        SetupDiDestroyDeviceInfoList(devInfo);
        if (found >= maxCount) break;
    }
    return found;
}

// ---------------------------------------------------------------------------
// open / close
// ---------------------------------------------------------------------------

// "&MI_xx" token of a device interface path; 0xFF if not a composite child
static int parseMiNumber(const wchar_t* path) {
    const wchar_t* mi = wcsstr(path, L"&MI_");
    if (!mi) return -1;
    int v = 0;
    const wchar_t* p = mi + 4;
    while (*p) {
        wchar_t c = *p;
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        v = v * 16 + d;
        p++;
    }
    return v;
}

// open one WinUSB node
static bool openNode(const wchar_t* path, HANDLE* hOut, WINUSB_INTERFACE_HANDLE* uOut) {
    HANDLE h = CreateFileW(path, GENERIC_WRITE | GENERIC_READ,
                           FILE_SHARE_WRITE | FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    WINUSB_INTERFACE_HANDLE u = nullptr;
    if (!WinUsb_Initialize(h, &u)) { CloseHandle(h); return false; }
    *hOut = h; *uOut = u;
    return true;
}

bool EmuDevice::open(const DeviceInfo& info) {
    close();
    m_info = info;
    if (!openNode(info.path, &m_handle, &m_usb)) return false;
    m_boundIface = (uint8_t)(parseMiNumber(info.path) < 0 ? 0xFF : parseMiNumber(info.path));

    if (!parseDescriptors()) { close(); return false; }

    // The E-MU device presents vendor-class interfaces (0xFF) grouped by usbccgp
    // into a single MI_00 node that covers the whole configuration: interface 0
    // plus associated 1..n. Fill handles from associated interfaces first.
    m_ih[0] = m_usb;
    for (int i = 1; i < 8; i++) {
        WINUSB_INTERFACE_HANDLE u = nullptr;
        if (WinUsb_GetAssociatedInterface(m_usb, (UCHAR)(i - 1), &u)) m_ih[i] = u;
    }
    if (m_boundIface != 0xFF) m_ih[m_boundIface] = m_usb;

    // fallback: any interface still without a handle gets its own sibling node
    DeviceInfo list[16];
    int n = enumerateDevices(list, 16);
    for (int i = 0; i < n; i++) {
        int mi = parseMiNumber(list[i].path);
        if (mi < 0 || mi >= 8 || m_ih[mi]) continue;
        HANDLE h = nullptr; WINUSB_INTERFACE_HANDLE u = nullptr;
        if (openNode(list[i].path, &h, &u)) { m_xh[mi] = h; m_ih[mi] = u; }
    }
    return true;
}

void EmuDevice::close() {
    for (int i = 0; i < 8; i++) {
        if (m_ih[i] && m_ih[i] != m_usb) { WinUsb_Free(m_ih[i]); }
        m_ih[i] = nullptr;
        if (m_xh[i]) { CloseHandle(m_xh[i]); m_xh[i] = nullptr; }
    }
    if (m_usb) { WinUsb_Free(m_usb); m_usb = nullptr; }
    if (m_handle) { CloseHandle(m_handle); m_handle = nullptr; }
    m_boundIface = 0xFF;
}

// ---------------------------------------------------------------------------
// descriptor parsing
// ---------------------------------------------------------------------------

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd24(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16); }

bool EmuDevice::parseDescriptors() {
    USB_CONFIGURATION_DESCRIPTOR cfgHdr{};
    ULONG hdrGot = 0;
    if (!WinUsb_GetDescriptor(m_usb, USB_CONFIGURATION_DESCRIPTOR_TYPE, 0, 0,
                              (PBYTE)&cfgHdr, sizeof(cfgHdr), &hdrGot))
        return false;
    m_cfgLen = cfgHdr.wTotalLength;
    if (m_cfgLen > sizeof(m_cfg)) m_cfgLen = sizeof(m_cfg);
    ULONG cfgGot = 0;
    if (!WinUsb_GetDescriptor(m_usb, USB_CONFIGURATION_DESCRIPTOR_TYPE, 0, 0,
                              m_cfg, m_cfgLen, &cfgGot))
        return false;

    // UAC1 class-specific AC interface descriptor subtypes (Table A-5):
    // 1 HEADER, 2 INPUT_TERMINAL, 3 OUTPUT_TERMINAL, 4 MIXER_UNIT,
    // 5 SELECTOR_UNIT, 6 FEATURE_UNIT, 7 PROCESSING_UNIT, 8 EXTENSION_UNIT.
    const uint8_t CS_XU = 0x08, CS_FEATURE = 0x06;

    const uint8_t* p = m_cfg;
    const uint8_t* end = m_cfg + m_cfgLen;
    uint8_t iface = 0, alt = 0;

    // single pass: interfaces are vendor-class (0xFF) but carry the standard
    // UAC1 class-specific descriptor layout. Direction is taken from the data
    // endpoint (OUT data = playback stream, IN data = capture stream; tiny IN
    // iso endpoints are UAC1 feedback). Formats accumulated per alt setting
    // are committed when their data endpoint is seen.
    struct PendFmt { uint32_t rate; uint8_t ch, bits, alt; };
    PendFmt pend[64];
    int pendN = 0;
    StreamFormat cur{};
    bool curValid = false;
    while (p + 2 <= end) {
        uint8_t len = p[0], type = p[1];
        if (len < 2 || p + len > end) break;

        if (type == USB_INTERFACE_DESCRIPTOR_TYPE && len >= 9) {
            iface = p[2]; alt = p[3];
            bool audioCls = (p[5] == 1 || p[5] == 0xFF);
            if (audioCls && p[6] == 1 && alt == 0) m_acIface = iface; // AUDIOCONTROL
            if (audioCls && p[6] == 2 && alt != 0) {
                memset(&cur, 0, sizeof(cur));
                cur.altSetting = alt;
                curValid = true;
                pendN = 0;
            } else curValid = false;
        } else if (curValid && type == 0x24 && len >= 8) { // CS_INTERFACE (streaming)
            uint8_t subtype = p[2];
            if (subtype == 0x01 && len >= 7) {              // AS_GENERAL
                if (rd16(p + 5) != 1) curValid = false;     // wFormatTag != PCM
            } else if (subtype == 0x02 && p[3] == 1) {      // FORMAT_TYPE, TYPE_I
                cur.channels = p[4];
                cur.bitDepth = p[6];
                uint8_t fsType = p[7];
                const uint8_t* f = p + 8;
                if (fsType == 0 && len >= 11) {
                    uint32_t lo = rd24(f), hi = rd24(f + 3);
                    static const uint32_t kRates[] = {44100, 48000, 88200, 96000, 176400, 192000};
                    for (uint32_t r : kRates)
                        if (r >= lo && r <= hi && pendN < 64)
                            pend[pendN++] = {r, cur.channels, cur.bitDepth, cur.altSetting};
                } else {
                    const uint8_t* fe = p + len;
                    while (f + 3 <= fe && pendN < 64) {
                        pend[pendN++] = {rd24(f), cur.channels, cur.bitDepth, cur.altSetting};
                        f += 3;
                    }
                }
            }
        } else if (curValid && type == USB_ENDPOINT_DESCRIPTOR_TYPE && len >= 6) {
            uint8_t addr = p[2], attrs = p[3];
            uint16_t maxP = rd16(p + 4);
            if ((attrs & 3) == 1) { // isochronous
                bool isIn = (addr & 0x80) != 0;
                if (isIn && maxP <= 4) {
                    m_fbPipe[0] = addr; // UAC1 feedback belongs to the OUT stream
                } else if (cur.channels) {
                    int dir = isIn ? 1 : 0;
                    cur.maxPacket = maxP & 0x7FF;
                    m_isoPipe[dir] = addr;
                    m_asIface[dir] = iface; // streaming interface of this direction
                    for (int q = 0; q < pendN && m_fmtCount[dir] < 16; q++)
                        m_fmt[dir][m_fmtCount[dir]++] =
                            {pend[q].rate, pend[q].bits, pend[q].ch, pend[q].alt, cur.maxPacket};
                    pendN = 0;
                }
            }
        }
        p += len;
    }

    // pass 3: AudioControl units — Extension Units (XU) and Feature Unit
    // UAC1 Table A-5 subtypes: 6=FEATURE_UNIT, 8=EXTENSION_UNIT.
    // EXTENSION_UNIT layout: bLength,type,subtype,unitID,wExtensionCode(2),...
    p = m_cfg;
    while (p + 2 <= end) {
        uint8_t len = p[0], type = p[1];
        if (len < 2 || p + len > end) break;
        if (type == 0x24 && len >= 6) {
            uint8_t subtype = p[2];
            if (subtype == 0x08) {
                uint8_t unitID = p[3];
                uint16_t wExtCode = rd16(p + 4);
                if (wExtCode >= 0xE301 && wExtCode <= 0xE306)
                    for (int i = 0; i < 6; i++)
                        if (m_xuUnit[i] == 0 && wExtCode == (uint16_t)(0xE301 + i))
                            m_xuUnit[i] = unitID;
            } else if (subtype == 0x06) {
                if (m_featUnit == 0) m_featUnit = p[3];
            }
        }
        p += len;
    }
    return m_acIface != 0xFF;
}

// ---------------------------------------------------------------------------
// XU control
// ---------------------------------------------------------------------------

const StreamFormat* EmuDevice::formats(int dir) const { return m_fmt[dir]; }
int EmuDevice::formatCount(int dir) const { return m_fmtCount[dir]; }
uint8_t EmuDevice::xuUnit(XUCode code) const {
    int i = (int)code - 0xE301;
    return (i >= 0 && i < 6) ? m_xuUnit[i] : 0;
}

// class request on AudioControl interface; channel goes into wValue low byte
static bool classReq(WINUSB_INTERFACE_HANDLE usb, uint8_t acIface, bool out,
                     uint8_t unitID, uint8_t cs, uint8_t channel,
                     void* buf, uint16_t len) {
    if (!usb || !unitID) return false;
    WINUSB_SETUP_PACKET pkt{};
    pkt.RequestType = out ? 0x21 : 0xA1;
    pkt.Request = 0x01;                          // SET_CUR / GET_CUR
    pkt.Value = (uint16_t)((cs << 8) | channel);
    pkt.Index = (uint16_t)((unitID << 8) | acIface);
    pkt.Length = len;
    ULONG done = 0;
    return WinUsb_ControlTransfer(usb, pkt, (PBYTE)buf, len, &done, nullptr) && done == len;
}

bool EmuDevice::xuGet(uint8_t u, uint8_t cs, void* buf, uint16_t len) {
    return classReq(m_usb, m_acIface, false, u, cs, 0, buf, len);
}
bool EmuDevice::xuSet(uint8_t u, uint8_t cs, const void* buf, uint16_t len) {
    return classReq(m_usb, m_acIface, true, u, cs, 0, (void*)buf, len);
}

static const uint32_t kRates[6] = {44100, 48000, 88200, 96000, 176400, 192000};

bool EmuDevice::getClockSource(uint8_t& src) { return xuGet(m_xuUnit[1], CS_CLKSRC_SOURCE, &src, 1); }
bool EmuDevice::setClockSource(uint8_t src) { return xuSet(m_xuUnit[1], CS_CLKSRC_SOURCE, &src, 1); }

bool EmuDevice::getClockRate(uint32_t& rate) {
    uint8_t idx = 0;
    if (!xuGet(m_xuUnit[0], CS_CLOCKRATE_RATE, &idx, 1)) return false;
    rate = idx < 6 ? kRates[idx] : 0;
    return true;
}
bool EmuDevice::setClockRate(uint32_t rate) {
    uint8_t idx = 0xFF;
    for (int i = 0; i < 6; i++) if (kRates[i] == rate) { idx = (uint8_t)i; break; }
    if (idx == 0xFF) return false;
    return xuSet(m_xuUnit[0], CS_CLOCKRATE_RATE, &idx, 1);
}
bool EmuDevice::getSoftLimit(uint8_t& on)   { return xuGet(m_xuUnit[3], CS_DEVOPT_SOFTLIMIT, &on, 1); }
bool EmuDevice::setSoftLimit(uint8_t on)    { return xuSet(m_xuUnit[3], CS_DEVOPT_SOFTLIMIT, &on, 1); }
bool EmuDevice::getSpdifFormat(uint8_t& f)  { return xuGet(m_xuUnit[2], CS_DIGIO_FORMAT, &f, 1); }
bool EmuDevice::setSpdifFormat(uint8_t f)   { return xuSet(m_xuUnit[2], CS_DIGIO_FORMAT, &f, 1); }
bool EmuDevice::getDigLock(uint8_t& locked) { return xuGet(m_xuUnit[2], CS_DIGIO_LOCK, &locked, 1); }
bool EmuDevice::getDigRate(uint32_t& rate)  {
    uint8_t b[4]{};
    if (!xuGet(m_xuUnit[2], CS_DIGIO_RATE, b, 4)) return false;
    rate = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    return true;
}

// Feature Unit: volume is a signed 1/256-dB value; channel in wValue low byte
bool EmuDevice::getVolume(int16_t& v, uint8_t fu, uint8_t ch) {
    return classReq(m_usb, m_acIface, false, fu, 2 /*VOLUME*/, ch, &v, 2);
}
bool EmuDevice::setVolume(int16_t v, uint8_t fu, uint8_t ch) {
    return classReq(m_usb, m_acIface, true, fu, 2, ch, &v, 2);
}

} // namespace oed
