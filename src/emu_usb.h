// openEMUdriver — E-MU USB (CA0189) WinUSB engine
// Device layer: discovery, UAC1 descriptor parsing, Extension Unit control.
//
// SPDX-License-Identifier: LGPL-2.1-or-later
// Protocol knowledge derived from the officially LGPL'd "EMU CA0189 USB Audio
// Driver" (E-MU Systems / Creative Technology, 2008) and verified against the
// disassembly of emusba10.sys / emasio.dll 1.30.07.
#ifndef OED_EMU_USB_H
#define OED_EMU_USB_H

#include <windows.h>
#include <winusb.h>
#include <setupapi.h>
#include <stdint.h>

namespace oed {

// ---- device identity ------------------------------------------------------
enum EmuModel {
    MODEL_UNKNOWN = 0,
    MODEL_0202,   // EM8740 MicroPod,   PID 0x3F02
    MODEL_0404,   // EM8761 HulaPod,    PID 0x3F04
    MODEL_TRACKER,// EM8730 MicroPre,   PID 0x3F0A
    MODEL_0204,   //                    PID 0x3F0C
};

struct DeviceInfo {
    wchar_t  path[512];     // CreateFile path of the USB device interface
    uint16_t vid, pid;
    EmuModel model;
    char     name[64];      // friendly name
};

// discovered streaming format of one alternate setting
struct StreamFormat {
    uint32_t rate;          // Hz
    uint8_t  bitDepth;      // 16 / 24 (in 3-byte slots)
    uint8_t  channels;
    uint8_t  altSetting;    // bAlternateSetting to select
    uint16_t maxPacket;     // endpoint wMaxPacketSize for this alt
};

// ---- XU protocol (see docs/PROTOCOL.md) ------------------------------------
enum XUCode : uint16_t {
    XU_CLOCK_RATE       = 0xE301,
    XU_CLOCK_SOURCE     = 0xE302,
    XU_DIGITAL_IO       = 0xE303,
    XU_DEVICE_OPTIONS   = 0xE304,
    XU_DIRECT_MONITOR   = 0xE305,
    XU_METERING         = 0xE306,
};

enum ClockSource : uint8_t { CLK_INTERNAL = 0, CLK_SPDIF_EXTERNAL = 1 };

static const uint8_t CS_CLOCKRATE_SUPPORT = 2;
static const uint8_t CS_CLOCKRATE_RATE    = 3;
static const uint8_t CS_CLKSRC_SOURCE     = 2;
static const uint8_t CS_DIGIO_RATE        = 2;   // 4 bytes BE
static const uint8_t CS_DIGIO_LOCK        = 3;
static const uint8_t CS_DIGIO_SRC         = 4;
static const uint8_t CS_DIGIO_FORMAT      = 5;   // 0 none / 1 spdif / 2 aes
static const uint8_t CS_DEVOPT_PAD        = 2;
static const uint8_t CS_DEVOPT_SOFTLIMIT  = 3;
static const uint8_t CS_DIRMON_STEREO     = 2;   // 20-byte structure
static const uint8_t CS_DIRMON_ENABLE     = 3;

// ---- device ----------------------------------------------------------------
class EmuDevice {
public:
    EmuDevice() = default;
    ~EmuDevice() { close(); }

    bool open(const DeviceInfo& info);
    void close();
    bool isOpen() const { return m_usb != nullptr; }
    const DeviceInfo& info() const { return m_info; }

    // ---- descriptor access ----
    const struct StreamFormat* formats(int direction) const; // direction 0=out 1=in
    int formatCount(int direction) const;

    // ---- XU control (class requests on AudioControl interface) ----
    bool xuGet(uint8_t unitID, uint8_t cs, void* buf, uint16_t len);
    bool xuSet(uint8_t unitID, uint8_t cs, const void* buf, uint16_t len);

    // unit IDs resolved from descriptors (0 = absent)
    uint8_t xuUnit(XUCode code) const;

    // convenience wrappers
    bool getClockSource(uint8_t& src);
    bool setClockSource(uint8_t src);
    bool getClockRate(uint32_t& rate);           // 4-byte BE
    bool setClockRate(uint32_t rate);
    bool getSoftLimit(uint8_t& on);
    bool setSoftLimit(uint8_t on);
    bool getSpdifFormat(uint8_t& fmt);           // 0/1/2
    bool setSpdifFormat(uint8_t fmt);
    bool getDigLock(uint8_t& locked);
    bool getDigRate(uint32_t& rate);
    // Feature Unit master volume (1/256 dB, SInt16)
    bool getVolume(int16_t& v256dB, uint8_t featureUnit, uint8_t channel);
    bool setVolume(int16_t v256dB, uint8_t featureUnit, uint8_t channel);

private:
    bool parseDescriptors();
    bool selectAltSetting(uint8_t iface, uint8_t alt); // used by stream layer
    friend class EmuStream;

    DeviceInfo m_info{};
    HANDLE     m_handle = nullptr;
    WINUSB_INTERFACE_HANDLE m_usb = nullptr;
    HANDLE     m_xh[8] {nullptr};                    // sibling child handles
    WINUSB_INTERFACE_HANDLE m_ih[8] {nullptr};       // WinUSB handle by USB interface number
    uint8_t    m_boundIface = 0xFF;                  // USB iface number of the main node

    // WinUSB handle for a given USB interface number
    WINUSB_INTERFACE_HANDLE ifaceHandle(uint8_t ifaceNum) const {
        return (ifaceNum < 8) ? m_ih[ifaceNum] : nullptr;
    }

    // USB descriptors (raw copies)
    uint8_t m_cfg[2048]{};
    uint16_t m_cfgLen = 0;

    struct Iface {
        uint8_t num;
        uint8_t altCount;
    };
    // parsed
    uint8_t m_acIface = 0xFF;             // AudioControl interface number
    uint8_t m_asIface[2] {0xFF, 0xFF};    // streaming interfaces out/in
    uint8_t m_fbPipe[2] {0, 0};           // feedback endpoint pipe ids (out/in? indexed by dir of stream)
    uint8_t m_isoPipe[2] {0, 0};          // iso data pipes
    uint8_t m_featUnit = 0;               // first Feature Unit ID
    uint8_t m_xuUnit[6] {0};              // unit IDs for XUCode-0xE301..0xE306

    StreamFormat m_fmt[2][16];            // [dir][idx]
    int m_fmtCount[2] {0, 0};
};

// enumerate all attached E-MU devices; returns count
int enumerateDevices(DeviceInfo* out, int maxCount);

} // namespace oed
#endif
