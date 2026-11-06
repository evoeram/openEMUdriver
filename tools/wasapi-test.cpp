// wasapi-test — enumerate WASAPI render endpoints; optionally play a 2 s tone
//   wasapi-test            list render endpoints (ALL states, endpoint id, desc,
//                          friendly name, shared-mode mix format)
//   wasapi-test -p <idx>   play 440 Hz through render endpoint <idx>
//
// The openEMUdriver M4 WaveRT miniport exposes exactly one format:
// 48000 Hz stereo, 24 valid bits in 32-bit containers. Any endpoint
// reporting a different mix rate is NOT the openEMUdriver render endpoint.
// Indexes refer to the printout order in both modes (same enumeration).
// SPDX-License-Identifier: LGPL-2.1-or-later
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdio.h>
#include <math.h>

static const CLSID k_CLSID_MMDeviceEnumerator = { 0xbcde0395, 0xe52f, 0x467c, { 0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e } };
static const IID k_IID_IMMDeviceEnumerator = { 0xa95664d2, 0x9614, 0x4f35, { 0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6 } };
static const IID k_IID_IAudioClient = { 0x1cb9ad4c, 0xdbfa, 0x4c32, { 0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2 } };
static const IID k_IID_IAudioRenderClient = { 0xf294acfc, 0x3146, 0x4483, { 0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2 } };

// PKEY_Device_DeviceDesc = {A45C254E-DF1C-4EFD-8020-67D146A850E0}, pid 2
static const PROPERTYKEY k_PKEY_Device_DeviceDesc = { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 2 };
// PKEY_Device_FriendlyName = {A45C254E-DF1C-4EFD-8020-67D146A850E0}, pid 14
static const PROPERTYKEY k_PKEY_Device_FriendlyName = { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };
// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT = {00000003-0000-0010-8000-00AA00389B71}
static const GUID k_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

static bool GuidEq(const GUID& a, const GUID& b) {
    return a.Data1 == b.Data1 && a.Data2 == b.Data2 && a.Data3 == b.Data3 &&
           memcmp(a.Data4, b.Data4, 8) == 0;
}

static void StateString(DWORD state, wchar_t* out, size_t outc) {
    out[0] = 0;
    struct { DWORD bit; const wchar_t* name; } bits[] = {
        { DEVICE_STATE_ACTIVE,     L"ACTIVE" },
        { DEVICE_STATE_DISABLED,   L"DISABLED" },
        { DEVICE_STATE_NOTPRESENT, L"NOTPRESENT" },
        { DEVICE_STATE_UNPLUGGED,  L"UNPLUGGED" },
    };
    for (auto& b : bits) {
        if (state & b.bit) {
            if (out[0]) wcscat_s(out, outc, L"|");
            wcscat_s(out, outc, b.name);
        }
    }
    if (!out[0]) wcscat_s(out, outc, L"UNKNOWN");
}

static void GetStrProp(IPropertyStore* ps, const PROPERTYKEY& key, wchar_t* out, size_t outc) {
    wcscpy_s(out, outc, L"?");
    PROPVARIANT v;
    PropVariantInit(&v);
    if (SUCCEEDED(ps->GetValue(key, &v)) && v.vt == VT_LPWSTR && v.pwszVal && v.pwszVal[0]) {
        wcsncpy_s(out, outc, v.pwszVal, _TRUNCATE);
    }
    PropVariantClear(&v);
}

// Query the shared-mode mix format the audio engine would use for this
// endpoint. Pure property query: no stream is created.
static void ProbeMixFormat(IMMDevice* d, wchar_t* out, size_t outc) {
    IAudioClient* ac = nullptr;
    HRESULT hr = d->Activate(k_IID_IAudioClient, CLSCTX_ALL, nullptr, (void**)&ac);
    if (FAILED(hr)) { wsprintfW(out, L"activate=0x%08lX", (unsigned long)hr); return; }
    WAVEFORMATEX* wf = nullptr;
    hr = ac->GetMixFormat(&wf);
    if (FAILED(hr)) { wsprintfW(out, L"GetMixFormat=0x%08lX", (unsigned long)hr); ac->Release(); return; }
    const wchar_t* sub = L"PCM";
    unsigned validBits = wf->wBitsPerSample;
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22) {
        WAVEFORMATEXTENSIBLE* ext = (WAVEFORMATEXTENSIBLE*)wf;
        validBits = ext->Samples.wValidBitsPerSample;
        if (GuidEq(ext->SubFormat, k_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) sub = L"float";
    } else if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        sub = L"float";
    }
    wsprintfW(out, L"%lu Hz %uch %u/%u-bit %ls", (unsigned long)wf->nSamplesPerSec,
              wf->nChannels, validBits, wf->wBitsPerSample, sub);
    CoTaskMemFree(wf);
    ac->Release();
}

static void PrintEndpoint(UINT i, IMMDevice* d) {
    DWORD state = 0;
    wchar_t stateStr[64] = L"?";
    if (FAILED(d->GetState(&state))) wcscpy_s(stateStr, L"GetState=failed");
    else StateString(state, stateStr, 64);

    LPWSTR id = nullptr;
    if (FAILED(d->GetId(&id))) id = nullptr;

    wchar_t desc[256] = L"?", name[256] = L"?", mix[128] = L"?";
    IPropertyStore* ps = nullptr;
    if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
        GetStrProp(ps, k_PKEY_Device_DeviceDesc, desc, 256);
        GetStrProp(ps, k_PKEY_Device_FriendlyName, name, 256);
        ps->Release();
    }
    if (state & DEVICE_STATE_ACTIVE) ProbeMixFormat(d, mix, 128);
    else wcscpy_s(mix, L"(not ACTIVE — not probed)");

    printf("[%u] state=%ls\n     id=%ls\n     desc=\"%ls\" name=\"%ls\"\n     mix=%ls\n",
           i, stateStr, id ? id : L"?", desc, name, mix);
    if (id) CoTaskMemFree(id);
}

int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    HRESULT hr = CoCreateInstance(k_CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                  k_IID_IMMDeviceEnumerator, (void**)&en);
    if (FAILED(hr)) { printf("no enumerator hr=0x%08lX\n", hr); return 1; }

    bool play = argc >= 3 && !wcscmp(argv[1], L"-p");
    int target = play ? _wtoi(argv[2]) : -1;

    IMMDeviceCollection* coll = nullptr;
    hr = en->EnumAudioEndpoints(eRender, DEVICE_STATEMASK_ALL, &coll);
    if (FAILED(hr)) { printf("enum hr=0x%08lX\n", hr); return 1; }
    UINT n = 0; coll->GetCount(&n);
    printf("WASAPI render endpoints (all states): %u\n", n);

    int emuIdx = -1;
    int p48[16]; int p48count = 0;
    for (UINT i = 0; i < n; i++) {
        IMMDevice* d = nullptr;
        if (FAILED(coll->Item(i, &d))) continue;
        PrintEndpoint(i, d);
        LPWSTR id = nullptr; d->GetId(&id);
        wchar_t name[256] = L"";
        IPropertyStore* ps = nullptr;
        if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
            GetStrProp(ps, k_PKEY_Device_FriendlyName, name, 256);
            ps->Release();
        }
        if (wcsstr(name, L"E-MU") || wcsstr(name, L"0404")) emuIdx = (int)i;
        if (id) CoTaskMemFree(id);
        d->Release();
    }

    // Second pass marker: mix rate is already printed above; call it out so the
    // 48 kHz candidate(s) are obvious without probing by hand.
    for (UINT i = 0; i < n; i++) {
        IMMDevice* d = nullptr;
        if (FAILED(coll->Item(i, &d))) continue;
        DWORD state = 0; d->GetState(&state);
        if (state & DEVICE_STATE_ACTIVE) {
            wchar_t mix[128];
            ProbeMixFormat(d, mix, 128);
            if (wcsstr(mix, L"48000 Hz") && p48count < 16) p48[p48count++] = (int)i;
        }
        d->Release();
    }
    if (p48count) {
        printf("48 kHz candidates (openEMUdriver M4 requires 48000 Hz stereo):");
        for (int i = 0; i < p48count; i++) printf(" %d", p48[i]);
        printf("\n");
    }
    if (emuIdx >= 0) printf("E-MU endpoint (by friendly name) at index %d\n", emuIdx);

    if (play) {
        if (target < 0 || target >= (int)n) { printf("bad index\n"); return 2; }
        IMMDevice* d = nullptr;
        if (FAILED(coll->Item((UINT)target, &d))) return 2;
        DWORD state = 0; d->GetState(&state);
        if (!(state & DEVICE_STATE_ACTIVE)) {
            printf("endpoint [%d] is not ACTIVE (state bits 0x%08lX); enable it in Sound control panel\n",
                   target, (unsigned long)state);
            return 3;
        }
        IAudioClient* ac = nullptr;
        hr = d->Activate(k_IID_IAudioClient, CLSCTX_ALL, nullptr, (void**)&ac);
        if (FAILED(hr)) { printf("activate hr=0x%08lX\n", hr); return 3; }
        WAVEFORMATEX* wf = nullptr;
        hr = ac->GetMixFormat(&wf);
        if (FAILED(hr)) { printf("mixfmt hr=0x%08lX\n", hr); return 4; }
        printf("endpoint [%d]: %lu Hz, %d ch, tag=0x%04X\n", target,
               (unsigned long)wf->nSamplesPerSec, wf->nChannels, wf->wFormatTag);
        if (wf->nSamplesPerSec != 48000)
            printf("WARNING: mix rate is not 48000 Hz — this is NOT the openEMUdriver M4 render endpoint\n");
        hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 2000000, 0, wf, nullptr);
        if (FAILED(hr)) { printf("init hr=0x%08lX\n", hr); ac->Release(); return 5; }
        UINT32 frames = 0; ac->GetBufferSize(&frames);
        IAudioRenderClient* rc = nullptr;
        hr = ac->GetService(k_IID_IAudioRenderClient, (void**)&rc);
        if (FAILED(hr)) { printf("render svc hr=0x%08lX\n", hr); ac->Release(); return 6; }
        BYTE* p = nullptr;
        hr = rc->GetBuffer(frames, &p);
        if (FAILED(hr)) { printf("getbuffer hr=0x%08lX\n", hr); rc->Release(); ac->Release(); return 7; }
        // shared mix format is float; handle both
        int channels = wf->nChannels;
        double phase = 0.0;
        for (UINT32 i = 0; i < frames; i++) {
            float v = (float)(0.2 * sin(phase));
            phase += 2.0 * 3.14159265358979 * 440.0 / (double)wf->nSamplesPerSec;
            if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE || wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
                float* f = (float*)p;
                for (int c = 0; c < channels; c++) f[(size_t)i * channels + c] = v;
            } else {
                short* s = (short*)p;
                short sv = (short)(v * 20000);
                for (int c = 0; c < channels; c++) s[(size_t)i * channels + c] = sv;
            }
        }
        rc->ReleaseBuffer(frames, 0);
        printf("playing 440 Hz through [%d] for 2 s (%lu Hz, %d ch)...\n",
               target, (unsigned long)wf->nSamplesPerSec, channels);
        hr = ac->Start();
        if (FAILED(hr)) { printf("start hr=0x%08lX\n", hr); rc->Release(); ac->Release(); return 8; }
        Sleep(2000);
        hr = ac->Stop();
        if (FAILED(hr)) printf("stop hr=0x%08lX\n", hr);
        printf("done\n");
        printf("now check: emu-probe test-stats   (expect rate 48000, running 1, growing submits/completes)\n");
        printf("           reg query \"HKLM\\SYSTEM\\CurrentControlSet\\Services\\EmuUSBAudio\\Parameters\" /v WaveStartStatus\n");
        CoTaskMemFree(wf);
        rc->Release(); ac->Release(); d->Release();
    }

    coll->Release(); en->Release();
    CoUninitialize();
    return 0;
}
