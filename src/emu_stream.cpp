// openEMUdriver — isochronous streaming engine over WinUSB
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Scheduling note: WinUsb_WriteIsochPipeAsap/ReadIsochPipeAsap chain-invalidates
// on the CA0189 pipes (transfers complete instantly with ERROR_INVALID_PARAMETER),
// so the engine uses explicit frame-numbered transfers (WinUsb_WriteIsochPipe /
// WinUsb_ReadIsochPipe) — one packet per USB frame, deterministic 1 ms grid.
#include "emu_stream.h"
#include <string.h>
#include <stdio.h>
#include <timeapi.h>
#include <malloc.h>

namespace oed {

static const uint32_t kRates[6] = {44100, 48000, 88200, 96000, 176400, 192000};

bool EmuStream::start(EmuDevice* dev, const Config& cfg) {
    if (m_run) stop();
    if (!dev || !dev->isOpen() || !cfg.onSwitch || cfg.framesPerSwitch <= 0) return false;

    m_dev = dev;
    m_cfg = cfg;
    m_xruns = 0;
    m_framesDone = 0;
    m_switchAccum = 0;
    m_rateAcc = 0;
    m_fbRate = 0;
    m_nextFrame = 0;
    m_xferCount = cfg.queueMs > 0 ? cfg.queueMs : 6;
    if (m_xferCount < 3) m_xferCount = 3;
    if (m_xferCount > 16) m_xferCount = 16;

    const int bpf = EmuStream::usbBytesPerSample();

    EmuDevice* d = dev;
    for (int dir = 0; dir < 2; dir++) {
        int needCh = dir == 0 ? cfg.outChannels : cfg.inChannels;
        if (!needCh) continue;
        const StreamFormat* fs = d->formats(dir);
        int n = d->formatCount(dir);
        int best = -1;
        // prefer the smallest channel count that still covers the activated
        // channels; at 192 kHz the device only exposes a 2-channel alt while
        // 48/96 kHz also have 4-channel ones
        for (int i = 0; i < n; i++) {
            if (fs[i].rate != cfg.rate || fs[i].bitDepth != 24) continue;
            if ((int)fs[i].channels >= needCh) {
                if (best < 0 || fs[i].channels < fs[best].channels) best = i;
            }
        }
        if (best < 0) {
            // nothing covers all channels — fall back to the largest
            for (int i = 0; i < n; i++) {
                if (fs[i].rate != cfg.rate || fs[i].bitDepth != 24) continue;
                if (best < 0 || fs[i].channels > fs[best].channels) best = i;
            }
        }
        if (best < 0) return false;
        if (dir == 0) m_cfg.outChannels = fs[best].channels;
        else          m_cfg.inChannels = fs[best].channels;
        m_usbCh[dir] = fs[best].channels;
    }

    m_stopEvt = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    m_run = true;

    int periods = m_xferCount + 2;
    for (int dir = 0; dir < 2; dir++) {
        int ch = dir == 0 ? cfg.outChannels : cfg.inChannels;
        if (!ch) continue;
        m_ringCap[dir] = periods * cfg.framesPerSwitch + 64;
        m_ring[dir] = (uint8_t*)calloc((size_t)m_ringCap[dir] * ch * bpf, 1);
    }

    timeBeginPeriod(1);

    if (cfg.outChannels) {
        m_threads[0] = CreateThread(nullptr, 0, outThreadTrampoline, this, 0, nullptr);
        SetThreadPriority(m_threads[0], THREAD_PRIORITY_TIME_CRITICAL);
    }
    if (cfg.inChannels) {
        m_threads[1] = CreateThread(nullptr, 0, inThreadTrampoline, this, 0, nullptr);
        SetThreadPriority(m_threads[1], THREAD_PRIORITY_TIME_CRITICAL);
    }
    // NOTE: the feedback thread is disabled in v1 — concurrent iso reads on the
    // feedback pipe of the same interface corrupt the OUT pipe scheduling
    // (every transfer completes with ERROR_INVALID_PARAMETER). The engine
    // streams at nominal packet sizes; drift over minutes is absorbed by the
    // ring buffer. Re-enable once per-interface scheduling is better understood.
    // if (m_fbPipe && cfg.outChannels) {
    //     m_threads[2] = CreateThread(nullptr, 0, fbThreadTrampoline, this, 0, nullptr);
    //     SetThreadPriority(m_threads[2], THREAD_PRIORITY_HIGHEST);
    // }
    return true;
}

void EmuStream::stop() {
    if (!m_run && !m_stopEvt) return;
    m_run = false;
    if (m_stopEvt) SetEvent(m_stopEvt);
    for (HANDLE t : m_threads) {
        if (!t) continue;
        WaitForSingleObject(t, 3000);
        CloseHandle(t);
    }
    memset(m_threads, 0, sizeof(m_threads));
    for (int d = 0; d < 2; d++) {
        if (m_bufH[d]) { WinUsb_UnregisterIsochBuffer(m_bufH[d]); m_bufH[d] = nullptr; }
        _aligned_free(m_isoBuf[d]); m_isoBuf[d] = nullptr;
        m_isoBufBytes[d] = 0;
        if (m_xfers[d]) {
            for (int i = 0; i < m_xferCount; i++)
                if (m_xfers[d][i].ov.hEvent) CloseHandle(m_xfers[d][i].ov.hEvent);
            delete[] m_xfers[d]; m_xfers[d] = nullptr;
        }
        free(m_ring[d]); m_ring[d] = nullptr;
        m_ringCap[d] = 0;
        m_ringRd[d] = m_ringWr[d] = 0;
    }
    if (m_stopEvt) { CloseHandle(m_stopEvt); m_stopEvt = nullptr; }
    timeEndPeriod(1);
}

// ---------------------------------------------------------------------------
// ring helpers (frame-granular, SPSC)
// ---------------------------------------------------------------------------

int EmuStream::pushOut(const uint8_t* src, int frames) {
    int ch = m_cfg.outChannels;
    if (!ch) return frames;
    int bpf = ch * usbBytesPerSample();
    long cap = m_ringCap[0];
    long wr = m_ringWr[0];
    long freeF = cap - (wr - m_ringRd[0]);
    if (frames > freeF) { m_xruns++; frames = (int)freeF; }
    long w = wr % cap;
    long first = cap - w;
    if (first > frames) first = frames;
    memcpy(m_ring[0] + (size_t)w * bpf, src, (size_t)first * bpf);
    if (frames > first)
        memcpy(m_ring[0], src + (size_t)first * bpf, (size_t)(frames - first) * bpf);
    m_ringWr[0] = wr + frames;
    return frames;
}

int EmuStream::popIn(uint8_t* dst, int frames) {
    int ch = m_cfg.inChannels;
    if (!ch) return frames;
    int bpf = ch * usbBytesPerSample();
    long cap = m_ringCap[1];
    long rd = m_ringRd[1];
    long avail = m_ringWr[1] - rd;
    if (frames > avail) {
        m_xruns++;
        long have = avail > 0 ? avail : 0;
        memset(dst + (size_t)have * bpf, 0, (size_t)(frames - have) * bpf);
        if (avail <= 0) return frames;
        frames = (int)avail;
    }
    long r = rd % cap;
    long first = cap - r;
    if (first > frames) first = frames;
    memcpy(dst, m_ring[1] + (size_t)r * bpf, (size_t)first * bpf);
    if (frames > first)
        memcpy(dst + (size_t)first * bpf, m_ring[1], (size_t)(frames - first) * bpf);
    m_ringRd[1] = rd + frames;
    return frames;
}

bool EmuStream::fireFrames(int frames) {
    m_switchAccum += frames;
    bool fired = false;
    while (m_switchAccum >= m_cfg.framesPerSwitch) {
        m_switchAccum -= m_cfg.framesPerSwitch;
        m_framesDone += m_cfg.framesPerSwitch;
        m_cfg.onSwitch(m_cfg.ctx, m_framesDone);
        fired = true;
    }
    return fired;
}

// ---------------------------------------------------------------------------
// OUT (playback) thread — frame-numbered isochronous transfers
// ---------------------------------------------------------------------------

DWORD EmuStream::outThread() {
    int ch = m_usbCh[0];
    int bpfF = ch * usbBytesPerSample();
    int iface = m_dev->m_asIface[0];
    WINUSB_INTERFACE_HANDLE ifh = m_dev->ifaceHandle((uint8_t)iface);
    if (ifh == nullptr || m_dev->m_isoPipe[0] == 0) return 1;

    double nominal = (double)m_cfg.rate / 1000.0;
    m_framesOutPerMs = (uint32_t)nominal;
    int maxFramesMs = (int)nominal + 2;
    int maxSlot = maxFramesMs * bpfF;

    m_isoBufBytes[0] = maxSlot * m_xferCount;
    m_isoBuf[0] = (uint8_t*)_aligned_malloc(m_isoBufBytes[0], 64);
    memset(m_isoBuf[0], 0, m_isoBufBytes[0]);

    m_xfers[0] = new Xfer[m_xferCount]{};
    for (int i = 0; i < m_xferCount; i++) {
        m_xfers[0][i].slot = i;
        m_xfers[0][i].ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    }

    // IMPORTANT: select the alt setting BEFORE registering the isoch buffer —
    // the registration binds to the pipe of the current alternate setting, and
    // alt 0 is zero-bandwidth (no pipe at all).
    {
        uint8_t alt = 0;
        const StreamFormat* fs = m_dev->formats(0);
        for (int i = 0; i < m_dev->formatCount(0); i++)
            if (fs[i].rate == m_cfg.rate && (int)fs[i].channels == ch) { alt = fs[i].altSetting; break; }
        WinUsb_SetCurrentAlternateSetting(ifh, alt);
        m_outAlt = alt;
    }
    if (!WinUsb_RegisterIsochBuffer(ifh, m_dev->m_isoPipe[0], m_isoBuf[0],
                                    m_isoBufBytes[0], &m_bufH[0])) {
        m_run = false;
        return 2;
    }

    // reserve the frame window
    ULONG cur = 0; LARGE_INTEGER ts{};
    WinUsb_GetCurrentFrameNumber(ifh, &cur, &ts);
    m_nextFrame = cur + 8;
    memset(m_hold, 0, sizeof(m_hold));

    // prime: first switch so the host fills the ring, then submit the pipeline
    fireFrames(0);
    for (int i = 0; i < m_xferCount && m_run; i++) {
        m_rateAcc += m_fbRate > 0 ? m_fbRate : nominal;
        int frames = (int)m_rateAcc;
        m_rateAcc -= frames;
        if (frames < 1) frames = 1;
        int bytes = frames * bpfF;
        uint8_t* dst = m_isoBuf[0] + (size_t)i * maxSlot;
        long rd = m_ringRd[0];
        long avail = m_ringWr[0] - rd;
        if (avail >= frames) {
            long r = rd % m_ringCap[0];
            long first = m_ringCap[0] - r;
            if (first > frames) first = frames;
            memcpy(dst, m_ring[0] + (size_t)r * bpfF, (size_t)first * bpfF);
            if (frames > first)
                memcpy(dst + (size_t)first * bpfF, m_ring[0], (size_t)(frames - first) * bpfF);
            m_ringRd[0] = rd + frames;
            memcpy(m_hold, dst, (size_t)frames * bpfF);
        } else {
            m_xruns++;
            memcpy(dst, m_hold, (size_t)frames * bpfF);
        }
        m_xfers[0][i].len = bytes;
        ULONG fno = m_nextFrame++;
        if (!WinUsb_WriteIsochPipe(m_bufH[0], (ULONG)(i * maxSlot), (ULONG)bytes,
                                   &fno, &m_xfers[0][i].ov)) {
            DWORD e = GetLastError();
            if (e != 997) { m_run = false; break; }
        }
        fireFrames(frames);
    }

    HANDLE evts[16];
    for (int i = 0; i < m_xferCount; i++) evts[i] = m_xfers[0][i].ov.hEvent;
    while (m_run) {
        DWORD w = WaitForMultipleObjects((DWORD)m_xferCount, evts, FALSE, 200);
        if (w == WAIT_TIMEOUT) {
            if (WaitForSingleObject(m_stopEvt, 0) == WAIT_OBJECT_0) break;
            continue;
        }
        if (w >= WAIT_OBJECT_0 + (DWORD)m_xferCount) break;
        int i = (int)(w - WAIT_OBJECT_0);
        ResetEvent(evts[i]);

        m_rateAcc += m_fbRate > 0 ? m_fbRate : nominal;
        int frames = (int)m_rateAcc;
        m_rateAcc -= frames;
        if (frames < 1) frames = 1;
        int bytes = frames * bpfF;
        uint8_t* dst = m_isoBuf[0] + (size_t)i * maxSlot;
        long rd = m_ringRd[0];
        long avail = m_ringWr[0] - rd;
        if (avail >= frames) {
            long r = rd % m_ringCap[0];
            long first = m_ringCap[0] - r;
            if (first > frames) first = frames;
            memcpy(dst, m_ring[0] + (size_t)r * bpfF, (size_t)first * bpfF);
            if (frames > first)
                memcpy(dst + (size_t)first * bpfF, m_ring[0], (size_t)(frames - first) * bpfF);
            m_ringRd[0] = rd + frames;
            memcpy(m_hold, dst, (size_t)frames * bpfF);
        } else {
            m_xruns++;
            memcpy(dst, m_hold, (size_t)frames * bpfF);
        }
        m_xfers[0][i].len = bytes;
        ULONG fno = m_nextFrame++;
        if (!WinUsb_WriteIsochPipe(m_bufH[0], (ULONG)(i * maxSlot), (ULONG)bytes,
                                   &fno, &m_xfers[0][i].ov)) {
            DWORD e = GetLastError();
            if (e != 997) break;
        }
        fireFrames(frames);
    }

    m_run = false;
    WinUsb_AbortPipe(ifh, m_dev->m_isoPipe[0]);
    if (m_outAlt) WinUsb_SetCurrentAlternateSetting(ifh, 0);
    return 0;
}

// ---------------------------------------------------------------------------
// IN (capture) thread — frame-numbered isochronous reads
// ---------------------------------------------------------------------------

DWORD EmuStream::inThread() {
    int ch = m_usbCh[1];
    int bpfF = ch * usbBytesPerSample();
    int iface = m_dev->m_asIface[1];
    WINUSB_INTERFACE_HANDLE ifh = m_dev->ifaceHandle((uint8_t)iface);
    if (ifh == nullptr || m_dev->m_isoPipe[1] == 0) return 1;

    double nominal = (double)m_cfg.rate / 1000.0;
    int maxFramesMs = (int)nominal + 2;
    int maxSlot = maxFramesMs * bpfF;

    m_isoBufBytes[1] = maxSlot * m_xferCount;
    m_isoBuf[1] = (uint8_t*)_aligned_malloc(m_isoBufBytes[1], 64);
    memset(m_isoBuf[1], 0, m_isoBufBytes[1]);

    m_xfers[1] = new Xfer[m_xferCount]{};
    for (int i = 0; i < m_xferCount; i++)
        m_xfers[1][i].ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);

    // alt setting first (see note in outThread), then register the buffer
    {
        uint8_t alt = 0;
        const StreamFormat* fs = m_dev->formats(1);
        for (int i = 0; i < m_dev->formatCount(1); i++)
            if (fs[i].rate == m_cfg.rate && (int)fs[i].channels == ch) { alt = fs[i].altSetting; break; }
        WinUsb_SetCurrentAlternateSetting(ifh, alt);
        m_inAlt = alt;
    }
    if (!WinUsb_RegisterIsochBuffer(ifh, m_dev->m_isoPipe[1], m_isoBuf[1],
                                    m_isoBufBytes[1], &m_bufH[1])) {
        return 2;
    }

    USBD_ISO_PACKET_DESCRIPTOR* pkts = new USBD_ISO_PACKET_DESCRIPTOR[m_xferCount]{};

    ULONG cur = 0; LARGE_INTEGER ts{};
    WinUsb_GetCurrentFrameNumber(ifh, &cur, &ts);
    ULONG next = cur + 8;

    for (int i = 0; i < m_xferCount; i++) {
        ULONG fno = next++;
        if (!WinUsb_ReadIsochPipe(m_bufH[1], (ULONG)(i * maxSlot), (ULONG)maxSlot,
                                  &fno, 1, &pkts[i], &m_xfers[1][i].ov)) {
            DWORD e = GetLastError();
            if (e != 997) { m_run = false; break; }
        }
    }

    HANDLE evts[16];
    for (int i = 0; i < m_xferCount; i++) evts[i] = m_xfers[1][i].ov.hEvent;
    while (m_run) {
        DWORD w = WaitForMultipleObjects((DWORD)m_xferCount, evts, FALSE, 200);
        if (w == WAIT_TIMEOUT) {
            if (WaitForSingleObject(m_stopEvt, 0) == WAIT_OBJECT_0) break;
            continue;
        }
        if (w >= WAIT_OBJECT_0 + (DWORD)m_xferCount) break;
        int i = (int)(w - WAIT_OBJECT_0);
        ResetEvent(evts[i]);

        int bytes = (int)pkts[i].Length;
        if (bytes > 0) {
            int frames = bytes / bpfF;
            const uint8_t* src = m_isoBuf[1] + (size_t)i * maxSlot;
            long cap = m_ringCap[1];
            long wr = m_ringWr[1];
            long freeF = cap - (wr - m_ringRd[1]);
            if (frames > freeF) { m_xruns++; frames = (int)freeF; }
            if (frames > 0) {
                long wpos = wr % cap;
                long first = cap - wpos;
                if (first > frames) first = frames;
                uint8_t* dst = m_ring[1] + (size_t)wpos * bpfF;
                memcpy(dst, src, (size_t)first * bpfF);
                if (frames > first)
                    memcpy(m_ring[1], src + (size_t)first * bpfF, (size_t)(frames - first) * bpfF);
                m_ringWr[1] = wr + frames;
            }
        }
        ULONG fno = next++;
        if (!WinUsb_ReadIsochPipe(m_bufH[1], (ULONG)(i * maxSlot), (ULONG)maxSlot,
                                  &fno, 1, &pkts[i], &m_xfers[1][i].ov)) {
            DWORD e = GetLastError();
            if (e != 997) break;
        }
        if (!m_cfg.outChannels && bytes > 0) fireFrames(bytes / bpfF);
    }

    m_run = false;
    WinUsb_AbortPipe(ifh, m_dev->m_isoPipe[1]);
    if (m_inAlt) WinUsb_SetCurrentAlternateSetting(ifh, 0);
    delete[] pkts;
    return 0;
}

// ---------------------------------------------------------------------------
// feedback thread (optional) — read the UAC1 feedback endpoint
// ---------------------------------------------------------------------------

DWORD EmuStream::fbThread() {
    if (!m_fbPipe) return 0;
    WINUSB_INTERFACE_HANDLE ifh = m_dev ? m_dev->ifaceHandle(m_dev->m_asIface[0]) : nullptr;
    if (!ifh) return 1;
    uint8_t fb[8]{};
    WINUSB_ISOCH_BUFFER_HANDLE h = nullptr;
    OVERLAPPED ov{};
    ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!WinUsb_RegisterIsochBuffer(ifh, m_fbPipe, fb, sizeof(fb), &h)) {
        CloseHandle(ov.hEvent);
        return 2;
    }
    USBD_ISO_PACKET_DESCRIPTOR pkt{};
    ULONG cur = 0; LARGE_INTEGER ts{};
    WinUsb_GetCurrentFrameNumber(ifh, &cur, &ts);
    ULONG next = cur + 8;
    while (m_run) {
        ResetEvent(ov.hEvent);
        ULONG fno = next++;
        if (!WinUsb_ReadIsochPipe(h, 0, (ULONG)sizeof(fb), &fno, 1, &pkt, &ov)) {
            DWORD e = GetLastError();
            if (e != 997) break;
        }
        DWORD w = WaitForSingleObject(ov.hEvent, 500);
        if (!m_run) break;
        if (w != WAIT_OBJECT_0) { WinUsb_AbortPipe(ifh, m_fbPipe); continue; }
        ULONG got = 0;
        if (WinUsb_GetOverlappedResult(ifh, &ov, &got, FALSE) && pkt.Length >= 3) {
            double perMs;
            if (pkt.Length >= 4) {
                uint32_t v = fb[0] | (fb[1] << 8) | (fb[2] << 16) | ((uint32_t)fb[3] << 24);
                perMs = (double)v / 65536.0;      // 16.16 (this device reports 4 bytes)
            } else {
                uint32_t v = fb[0] | (fb[1] << 8) | (fb[2] << 16);
                perMs = (double)v / 16384.0;      // 10.14
            }
            double nominal = (double)m_cfg.rate / 1000.0;
            if (perMs > nominal * 0.98 && perMs < nominal * 1.02) {
                m_fbRate = m_fbRate > 0 ? m_fbRate * 0.8 + perMs * 0.2 : perMs;
            }
        }
    }
    WinUsb_UnregisterIsochBuffer(h);
    CloseHandle(ov.hEvent);
    return 0;
}

} // namespace oed
