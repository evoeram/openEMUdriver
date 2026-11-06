// wasapi-test — enumerate WASAPI render endpoints; optionally play a 2 s tone
//   wasapi-test            list render endpoints
//   wasapi-test -p <idx>   play 440 Hz through render endpoint <idx>
// SPDX-License-Identifier: LGPL-2.1-or-later
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdio.h>
#include <math.h>

static const CLSID k_CLSID_MMDeviceEnumerator = { 0xbcde0395, 0xe52f, 0x467c, { 0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e } };
static const IID k_IID_IMMDeviceEnumerator = { 0xa95664d2, 0x9614, 0x4f35, { 0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6 } };
static const IID k_IID_IAudioClient = { 0x1cb9ad4c, 0xdbfa, 0x4c32, { 0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2 } };

int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    HRESULT hr = CoCreateInstance(k_CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                  k_IID_IMMDeviceEnumerator, (void**)&en);
    if (FAILED(hr)) { printf("no enumerator hr=0x%08lX\n", hr); return 1; }

    bool play = argc >= 3 && !wcscmp(argv[1], L"-p");
    int target = play ? _wtoi(argv[2]) : -1;

    IMMDeviceCollection* coll = nullptr;
    hr = en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll);
    if (FAILED(hr)) { printf("enum hr=0x%08lX\n", hr); return 1; }
    UINT n = 0; coll->GetCount(&n);
    printf("WASAPI render endpoints: %u\n", n);

    int emuIdx = -1;
    for (UINT i = 0; i < n; i++) {
        IMMDevice* d = nullptr;
        if (FAILED(coll->Item(i, &d))) continue;
        LPWSTR id = nullptr; d->GetId(&id);
        IPropertyStore* ps = nullptr;
        wchar_t name[256] = L"?";
        if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
            PROPVARIANT v;
            PropVariantInit(&v);
            PROPERTYKEY pkey = { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 }; // PKEY_Device_FriendlyName
            if (SUCCEEDED(ps->GetValue(pkey, &v)) && v.pwszVal)
                wcsncpy(name, v.pwszVal, 255);
            PropVariantClear(&v);
            ps->Release();
        }
        printf("[%u] %ls\n", i, name);
        if (wcsstr(name, L"E-MU") || wcsstr(name, L"0404")) emuIdx = i;
        if (id) CoTaskMemFree(id);
        d->Release();
    }

    if (play) {
        if (target < 0 || target >= (int)n) { printf("bad index\n"); return 2; }
        IMMDevice* d = nullptr;
        if (FAILED(coll->Item((UINT)target, &d))) return 2;
        IAudioClient* ac = nullptr;
        hr = d->Activate(k_IID_IAudioClient, CLSCTX_ALL, nullptr, (void**)&ac);
        if (FAILED(hr)) { printf("activate hr=0x%08lX\n", hr); return 3; }
        WAVEFORMATEX* wf = nullptr;
        hr = ac->GetMixFormat(&wf);
        if (FAILED(hr)) { printf("mixfmt hr=0x%08lX\n", hr); return 4; }
        hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 2000000, 0, wf, nullptr);
        if (FAILED(hr)) { printf("init hr=0x%08lX\n", hr); return 5; }
        UINT32 frames = 0; ac->GetBufferSize(&frames);
        IAudioRenderClient* rc = nullptr;
        static const IID k_IID_IAudioRenderClient = { 0xf294acfc, 0x3146, 0x4483, { 0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2 } };
        hr = ac->GetService(k_IID_IAudioRenderClient, (void**)&rc);
        if (FAILED(hr)) { printf("render svc hr=0x%08lX\n", hr); return 6; }
        BYTE* p = nullptr;
        rc->GetBuffer(frames, &p);
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
        ac->Start();
        Sleep(2000);
        ac->Stop();
        printf("done\n");
        rc->Release(); ac->Release(); d->Release();
    } else if (emuIdx >= 0) {
        printf("E-MU endpoint found at index %d (use -p %d to test)\n", emuIdx, emuIdx);
    }
    coll->Release(); en->Release();
    CoUninitialize();
    return 0;
}
