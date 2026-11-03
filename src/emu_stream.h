// openEMUdriver — isochronous streaming engine over WinUSB
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Design:
//  * One WinUSB isochronous transfer per USB (micro)frame — exact per-ms packet
//    sizes at every sample rate (44.1k family included), unlike multi-packet
//    URBs whose tail packet would absorb rounding.
//  * K transfers kept in flight per direction (default 6 ms queue).
//  * Lock-free SPSC ring between the USB completion thread and the ASIO
//    bufferSwitch callback (which runs on the same completion thread, ordered).
//  * Async feedback endpoint (UAC1) consumed to drive the rate accumulator;
//    adaptive/sync modes fall back to nominal packet sizing.
#ifndef OED_EMU_STREAM_H
#define OED_EMU_STREAM_H

#include "emu_usb.h"
#include <stdint.h>

namespace oed {

class EmuStream {
public:
    typedef void (*SwitchFn)(void* ctx, uint64_t framesDone);

    struct Config {
        uint32_t rate;
        int      outChannels;   // 0 = no playback stream
        int      inChannels;    // 0 = no capture stream
        int      framesPerSwitch;   // ASIO buffer size
        int      queueMs;       // iso transfers in flight per direction (default 6)
        SwitchFn onSwitch;      // called on engine thread each buffer period
        void*    ctx;
    };

    EmuStream() = default;
    ~EmuStream() { stop(); }

    bool start(EmuDevice* dev, const Config& cfg);
    void stop();
    bool running() const { return m_run; }

    // ---- called from the switch callback (engine thread) ----
    // playback: copy `frames` frames of packed USB samples (24-bit in 3 bytes,
    // or 16-bit) from src into the output ring
    int  pushOut(const uint8_t* src, int frames);
    // capture: copy `frames` frames from input ring into dst (same packing)
    int  popIn(uint8_t* dst, int frames);

    // actual USB channel count of the running stream (per direction)
    int usbChannels(int dir) const { return m_usbCh[dir]; }

    // stats
    uint32_t xruns() const { return m_xruns; }
    uint64_t framesDone() const { return m_framesDone; }

    // sample packing: bytes per sample as sent over USB (2 or 3)
    static int usbBytesPerSample() { return 3; } // CA0189 streams 24-bit

private:
    struct Xfer {
        OVERLAPPED ov{};
        int        len = 0;      // bytes this ms
        bool       active = false;
        int        slot = -1;    // ring slot index (for OUT)
    };

    static DWORD WINAPI outThreadTrampoline(void* p) { return ((EmuStream*)p)->outThread(); }
    static DWORD WINAPI inThreadTrampoline(void* p)  { return ((EmuStream*)p)->inThread(); }
    static DWORD WINAPI fbThreadTrampoline(void* p)  { return ((EmuStream*)p)->fbThread(); }

    DWORD outThread();
    DWORD inThread();
    DWORD fbThread();
    bool  fireFrames(int frames); // accumulate frames, fire switch per period
    int   ringFree() const;      // out ring free frames
    int   ringAvailIn() const;   // in ring available frames

    EmuDevice* m_dev = nullptr;
    Config     m_cfg{};
    bool       m_run = false;
    HANDLE     m_stopEvt = nullptr;

    // computed per rate
    int      m_bytesPerFrame = 0;   // usb bytes per stereo/sample frame (all ch)
    uint32_t m_framesOutPerMs = 0;  // nominal (fixed-point handled by accumulator)
    uint32_t m_framesInPerMs = 0;
    int      m_usbCh[2] {0, 0};            // channels carried by the USB stream
    uint8_t  m_outPipe = 0, m_inPipe = 0, m_fbPipe = 0;
    bool     m_hasFeedback = false;
    uint8_t  m_outAlt = 0, m_inAlt = 0;

    // rings (out: ASIO->USB, in: USB->ASIO), frame-granular
    uint8_t* m_ring[2] {nullptr, nullptr};   // 0=out 1=in
    int      m_ringCap[2] {0, 0};            // frames
    volatile long m_ringRd[2] {0, 0};        // frame index
    volatile long m_ringWr[2] {0, 0};

    // iso transfers
    WINUSB_ISOCH_BUFFER_HANDLE m_bufH[2] {nullptr, nullptr};
    uint8_t*  m_isoBuf[2] {nullptr, nullptr};   // queueMs * 1ms bytes
    int       m_isoBufBytes[2] {0, 0};
    Xfer*     m_xfers[2] {nullptr, nullptr};    // per direction
    int       m_xferCount = 0;

    // rate accumulator (feedback adaptation), out direction
    double    m_rateAcc = 0;         // fractional frames pending
    volatile double m_fbRate = 0;    // measured rate (frames per ms), 0 = nominal
    uint8_t   m_hold[2048] {0};      // last played frame (underrun hold), bpf<=2048

    // switch timing
    uint64_t  m_framesDone = 0;      // total frames handed to switch
    int       m_switchAccum = 0;
    volatile uint32_t m_xruns = 0;
    ULONG m_nextFrame = 0;                 // next USB frame to schedule

    HANDLE    m_threads[3] {nullptr, nullptr, nullptr};
};

} // namespace oed
#endif
