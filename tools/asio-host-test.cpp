// asio-host-test — minimal ASIO host exercising openEMUdriver.dll end-to-end:
// DllGetClassObject -> IClassFactory -> IASIO -> init/config/createBuffers/
// start (measures real callback cadence) -> stop. No registry needed.
// SPDX-License-Identifier: LGPL-2.1-or-later
#include <windows.h>
#include <stdio.h>
#include <math.h>
#include <cstdint>
#include "../../include/asiosdk/asiosys.h"
#include "../../include/asiosdk/asio.h"
#include "../../include/asiosdk/iasiodrv.h"

// {C3F1E7A8-6B2D-4C49-9A5E-8D2F10B7C604} — must match asio_emu.cpp
static const GUID kClsid =
    { 0xc3f1e7a8, 0x6b2d, 0x4c49, { 0x9a, 0x5e, 0x8d, 0x2f, 0x10, 0xb7, 0xc6, 0x04 } };
// {8325F4A7-2CB7-4D0A-9DE9-3BE30180DC51}
static const IID kIID_IASIO =
    { 0x8325f4a7, 0x2cb7, 0x4d0a, { 0x9d, 0xe9, 0x3b, 0xe3, 0x01, 0x80, 0xdc, 0x51 } };

static IASIO* g_driver = nullptr;
static long g_bufSize = 0;
static long g_inCh = 0, g_outCh = 0;
static long g_inRegistered = 0, g_outRegistered = 0;
static ASIOBufferInfo g_bi[8];
static int g_switches = 0;
static LARGE_INTEGER g_qpf, g_t0, g_tlast;
static double g_minDt = 1e9, g_maxDt = 0, g_sumDt = 0;
static double g_rmsOut = 0;

static void onBufferSwitch(long index, ASIOBool) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_switches > 0) {
        double dt = (double)(now.QuadPart - g_tlast.QuadPart) * 1000.0 / (double)g_qpf.QuadPart;
        if (dt < g_minDt) g_minDt = dt;
        if (dt > g_maxDt) g_maxDt = dt;
        g_sumDt += dt;
    }
    g_tlast = now;
    if (g_switches == 0) g_t0 = now;
    g_switches++;

    // generate a soft test tone on output channel 0 (and mirror to 1)
    static double phase = 0.0;
    if (g_outRegistered > 0) {
        int32_t* L = (int32_t*)g_bi[0].buffers[index];
        int32_t* R = (int32_t*)g_bi[1].buffers[index];
        for (long i = 0; i < g_bufSize; i++) {
            double v = 0.25 * sin(phase);
            phase += 2.0 * 3.14159265358979 * 440.0 / 48000.0;
            int32_t s = (int32_t)(v * 2147483647.0);
            L[i] = s;
            if (g_outRegistered > 1) R[i] = s;
        }
    }
    // measure input RMS if present (inputs were registered after outputs)
    if (g_inRegistered > 0) {
        int32_t* in = (int32_t*)g_bi[g_outRegistered].buffers[index];
        double acc = 0;
        for (long i = 0; i < g_bufSize; i++) {
            double v = in[i] / 2147483648.0;
            acc += v * v;
        }
        g_rmsOut = sqrt(acc / (double)g_bufSize);
    }
}

static ASIOTime* onBufferSwitchTime(ASIOTime*, long index, ASIOBool) {
    onBufferSwitch(index, ASIOFalse);
    return nullptr;
}

int main(int argc, char** argv) {
    const char* dllPath = argc > 1 ? argv[1] : "bin\\openEMUdriver.asio";
    printf("asio-host-test: loading %s\n", dllPath);
    HMODULE dll = LoadLibraryA(dllPath);
    if (!dll) { printf("LoadLibrary failed GLE=%lu\n", GetLastError()); return 1; }

    typedef HRESULT(STDAPICALLTYPE* FnGetClassObject)(REFCLSID, REFIID, void**);
    FnGetClassObject gco = (FnGetClassObject)GetProcAddress(dll, "DllGetClassObject");
    if (!gco) { printf("no DllGetClassObject\n"); return 1; }

    IClassFactory* cf = nullptr;
    HRESULT hr = gco(kClsid, __uuidof(IClassFactory), (void**)&cf);
    if (FAILED(hr) || !cf) { printf("DllGetClassObject hr=0x%08lX\n", hr); return 1; }
    hr = cf->CreateInstance(nullptr, kIID_IASIO, (void**)&g_driver);
    cf->Release();
    if (FAILED(hr) || !g_driver) { printf("CreateInstance hr=0x%08lX\n", hr); return 1; }

    char name[64] = {0};
    g_driver->getDriverName(name);
    printf("driver: %s v%ld\n", name, g_driver->getDriverVersion());

    if (g_driver->init(nullptr) != ASIOTrue) {
        char err[256] = {0};
        g_driver->getErrorMessage(err);
        printf("init FAILED: %s\n", err);
        return 2;
    }
    printf("init OK\n");

    g_driver->getChannels(&g_inCh, &g_outCh);
    printf("channels: in=%ld out=%ld\n", g_inCh, g_outCh);

    long minS, maxS, prefS, gran;
    g_driver->getBufferSize(&minS, &maxS, &prefS, &gran);
    printf("buffer: min=%ld max=%ld pref=%ld gran=%ld\n", minS, maxS, prefS, gran);

    ASIOSampleRate rate = getenv("OED_RATE") ? atof(getenv("OED_RATE")) : 48000.0;
    if (g_driver->canSampleRate(rate) == ASE_OK) {
        g_driver->setSampleRate(rate);
    } else {
        g_driver->getSampleRate(&rate);
        printf("48k unsupported, device at %.0f\n", rate);
    }
    printf("sample rate: %.0f\n", rate);

    ASIOClockSource clocks[8];
    long nClocks = 0;
    g_driver->getClockSources(clocks, &nClocks);
    for (long i = 0; i < nClocks; i++)
        printf("clock[%ld]: %s%s\n", i, clocks[i].name, clocks[i].isCurrentSource ? " *" : "");

    // activate: 2 out (inputs disabled for the cadence experiment)
    long n = 0;
    int onlyOut = getenv("OED_OUT_ONLY") != NULL;
    for (long c = 0; c < 2 && c < g_outCh; c++) {
        g_bi[n].isInput = ASIOFalse; g_bi[n].channelNum = c; n++;
    }
    g_outRegistered = n;
    if (!onlyOut)
        for (long c = 0; c < 2 && c < g_inCh; c++) {
            g_bi[n].isInput = ASIOTrue; g_bi[n].channelNum = c; n++;
        }
    g_inRegistered = n - g_outRegistered;
    g_bufSize = prefS;
    ASIOCallbacks cb{};
    cb.bufferSwitch = &onBufferSwitch;
    cb.asioMessage = nullptr;
    cb.bufferSwitchTimeInfo = &onBufferSwitchTime;

    hr = g_driver->createBuffers(g_bi, n, g_bufSize, &cb);
    if (hr != ASE_OK) { printf("createBuffers hr=%ld\n", hr); return 3; }
    printf("createBuffers OK (%ld ch, %ld frames)\n", n, g_bufSize);

    QueryPerformanceFrequency(&g_qpf);
    hr = g_driver->start();
    if (hr != ASE_OK) { printf("start hr=%ld\n", hr); return 4; }
    printf("streaming for 2 seconds... (440 Hz tone on OUT L/R — listen!)\n");
    Sleep(2000);
    g_driver->stop();
    printf("stopped. switches=%d avg=%.3fms min=%.3fms max=%.3fms (target %.3fms)\n",
           g_switches, g_sumDt / (g_switches > 1 ? g_switches - 1 : 1),
           g_minDt, g_maxDt, (double)g_bufSize * 1000.0 / rate);
    printf("input RMS (last block): %.6f\n", g_rmsOut);

    long il, ol;
    g_driver->getLatencies(&il, &ol);
    printf("latency: in=%ld out=%ld frames\n", il, ol);

    g_driver->disposeBuffers();
    g_driver->Release();
    printf("DONE\n");
    return 0;
}
