#define INITGUID
/* ks-probe.c — query the E-MU wave filter's pin properties from user mode,
 * replicating the KS calls the audio engine makes when it builds a mix
 * format. The KS filter device object is world-readable per the INF ACL, so
 * this needs no elevation (unlike the private EmuUSBAudioCtl probe).
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include <initguid.h>
#include <windows.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <cfgmgr32.h>
#include <stdio.h>

/* MinGW declares the KS GUIDs via DEFINE_GUIDEX (extern only) and ships no
 * instantiation library — provide the definitions here. */
EXTERN_C const GUID GUID_NULL =
{ 0x00000000, 0x0000, 0x0000, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } };
EXTERN_C const GUID KSPROPSETID_Pin =
{ 0x8C134960, 0x51AD, 0x11CF, { 0x87, 0x8A, 0x94, 0xF8, 0x01, 0xC1, 0x00, 0x00 } };
EXTERN_C const GUID KSCATEGORY_AUDIO =
{ 0x6994AD04, 0x93EF, 0x11D0, { 0xA3, 0xCC, 0x00, 0xA0, 0xC9, 0x22, 0x31, 0x96 } };
EXTERN_C const GUID KSDATAFORMAT_TYPE_AUDIO =
{ 0x73647561, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
EXTERN_C const GUID KSDATAFORMAT_SUBTYPE_PCM =
{ 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
EXTERN_C const GUID KSDATAFORMAT_SUBTYPE_ANALOG =
{ 0x6DBA3190, 0x67BD, 0x11CF, { 0xA0, 0xF7, 0x00, 0x20, 0xAF, 0xD1, 0x56, 0xE4 } };
EXTERN_C const GUID KSDATAFORMAT_SPECIFIER_WAVEFORMATEX =
{ 0x05589F81, 0xC356, 0x11CE, { 0xBF, 0x01, 0x00, 0xAA, 0x00, 0x55, 0x59, 0x5A } };
EXTERN_C const GUID KSDATAFORMAT_SPECIFIER_NONE =
{ 0x0F6417D6, 0xC318, 0x11D0, { 0xA4, 0x3F, 0x00, 0xA0, 0xC9, 0x22, 0x31, 0x96 } };
#include <string.h>

/* MinGW's ks.h predates the Win8 pin properties; use explicit numeric ids
 * (verified against WDK 10.0.26100 shared/ks.h enum KSPROPERTY_PIN). */
#define EMU_KSPROPERTY_PIN_DATARANGES          3
#define EMU_KSPROPERTY_PIN_DATAINTERSECTION    4
#define EMU_KSPROPERTY_PIN_PROPOSEDATAFORMAT  13
#define EMU_KSPROPERTY_PIN_PROPOSEDATAFORMAT2 14

/* MinGW ksmedia.h lacks the audio processing-mode attribute GUID. */
DEFINE_GUID(EMU_KSATTRIBUTEID_AUDIOSIGNALPROCESSING_MODE,
            0xe1f89eb5, 0x5f46, 0x419b, 0x96, 0x7b, 0xff, 0x67, 0x70,
            0xb9, 0x84, 0x01);

/* MinGW ksmedia.h also lacks KSDATAFORMAT_SUBTYPE_IEEE_FLOAT. */
static const GUID EMU_GUID_IEEE_FLOAT =
{ 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

/* ...and the KSDATAFORMAT_WAVEFORMATEXTENSIBLE wrapper (WDK-only type). */
typedef struct {
    KSDATAFORMAT         DataFormat;
    WAVEFORMATEXTENSIBLE WaveFormatEx;
} EMU_KSWFEXT;

#define EMU_WAVE_PIN_SINK 0

static const char* GuidName(REFGUID g)
{
#define CMP(name) if (IsEqualGUID(g, name)) return #name
    CMP(KSDATAFORMAT_TYPE_AUDIO);
    CMP(KSDATAFORMAT_SUBTYPE_PCM);
    CMP(EMU_GUID_IEEE_FLOAT);
    CMP(GUID_NULL);
    CMP(KSDATAFORMAT_SUBTYPE_ANALOG);
    CMP(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX);
    CMP(KSDATAFORMAT_SPECIFIER_NONE);
    CMP(GUID_NULL);
    CMP(EMU_KSATTRIBUTEID_AUDIOSIGNALPROCESSING_MODE);
#undef CMP
    return "?";
}

static void PrintGuid(REFGUID g)
{
    printf("%s {%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
           GuidName(g), g.Data1, g.Data2, g.Data3,
           g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
           g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

static void PrintWin32Error(const char* what, DWORD err)
{
    LPSTR msg = NULL;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                   FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                   (LPSTR)&msg, 0, NULL);
    printf("%s failed, Win32 error %lu (%s)\n", what,
           (unsigned long)err, msg ? msg : "");
    if (msg) LocalFree(msg);
}

static void PrintFormat(const BYTE* buf, ULONG size)
{
    PKSDATAFORMAT df = (PKSDATAFORMAT)buf;
    if (size < sizeof(KSDATAFORMAT)) { printf("  (too small: %lu)\n", size); return; }
    printf("  KSDATAFORMAT: size=%lu flags=0x%08lX sample=%lu\n",
           df->FormatSize, df->Flags, df->SampleSize);
    printf("    major="); PrintGuid(df->MajorFormat); printf("\n");
    printf("    sub=");   PrintGuid(df->SubFormat);   printf("\n");
    printf("    spec=");  PrintGuid(df->Specifier);  printf("\n");
    if (size >= sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEX) &&
        IsEqualGUID(df->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)) {
        PWAVEFORMATEX w = (PWAVEFORMATEX)(df + 1);
        printf("    WAVEFORMATEX: tag=0x%04X ch=%u rate=%lu avg=%lu align=%u bits=%u cbSize=%u\n",
               w->wFormatTag, w->nChannels, (unsigned long)w->nSamplesPerSec,
               (unsigned long)w->nAvgBytesPerSec, w->nBlockAlign,
               w->wBitsPerSample, w->cbSize);
        if (w->wFormatTag == WAVE_FORMAT_EXTENSIBLE && w->cbSize >= 22) {
            PWAVEFORMATEXTENSIBLE e = (PWAVEFORMATEXTENSIBLE)w;
            printf("      valid=%u mask=0x%08lX subfmt=",
                   e->Samples.wValidBitsPerSample,
                   (unsigned long)e->dwChannelMask);
            PrintGuid(e->SubFormat); printf("\n");
        }
    }
    if (df->Flags & KSDATAFORMAT_ATTRIBUTES && size > df->FormatSize) {
        PKSMULTIPLE_ITEM mi = (PKSMULTIPLE_ITEM)(buf + ((df->FormatSize + 7) & ~7));
        printf("    attributes: mi.Size=%lu mi.Count=%lu\n",
               (unsigned long)mi->Size, (unsigned long)mi->Count);
        if (mi->Count) {
            PKSATTRIBUTE attr = (PKSATTRIBUTE)(mi + 1);
            printf("      attr size=%lu flags=0x%08lX id=",
                   attr->Size, attr->Flags);
            PrintGuid(attr->Attribute); printf("\n");
        }
    }
}

static int PropGet(HANDLE h, ULONG id, PVOID inExtra, ULONG inExtraSize,
                   PVOID out, ULONG outSize, const char* name)
{
    KSP_PIN ksp;
    DWORD got;

    memset(&ksp, 0, sizeof(ksp));
    ksp.Property.Set = KSPROPSETID_Pin;
    ksp.Property.Id = id;
    ksp.Property.Flags = KSPROPERTY_TYPE_GET;
    ksp.PinId = EMU_WAVE_PIN_SINK;

    BYTE in[sizeof(KSP_PIN) + 64];
    memcpy(in, &ksp, sizeof(ksp));
    if (inExtra && inExtraSize)
        memcpy(in + sizeof(ksp), inExtra, inExtraSize);

    if (!DeviceIoControl(h, IOCTL_KS_PROPERTY, in,
                         sizeof(ksp) + inExtraSize, out, outSize, &got, NULL)) {
        PrintWin32Error(name, GetLastError());
        return 1;
    }
    printf("%s: OK, %lu bytes\n", name, (unsigned long)got);
    return 0;
}

static int PropSetFormat(HANDLE h, const EMU_KSWFEXT* fmt,
                         const char* name)
{
    KSP_PIN ksp;
    DWORD got;
    BYTE in[sizeof(KSP_PIN) + sizeof(EMU_KSWFEXT)];

    memset(&ksp, 0, sizeof(ksp));
    ksp.Property.Set = KSPROPSETID_Pin;
    ksp.Property.Id = EMU_KSPROPERTY_PIN_PROPOSEDATAFORMAT;
    ksp.Property.Flags = KSPROPERTY_TYPE_SET;
    ksp.PinId = EMU_WAVE_PIN_SINK;
    memcpy(in, &ksp, sizeof(ksp));
    memcpy(in + sizeof(ksp), fmt, sizeof(*fmt));

    if (!DeviceIoControl(h, IOCTL_KS_PROPERTY, in, sizeof(in), NULL, 0,
                         &got, NULL)) {
        PrintWin32Error(name, GetLastError());
        return 1;
    }
    printf("%s: OK\n", name);
    return 0;
}

int main(int argc, char** argv)
{
    const char* devId =
        (argc > 1) ? argv[1]
                   : "USB\\VID_041E&PID_3F04\\E-MU-CA-3F04-07D90A1D-0190E-8761T2A";
    WCHAR devIdW[512];
    ULONG listLen = 0;
    WCHAR* list = NULL;
    ULONG i;
    HANDLE h = INVALID_HANDLE_VALUE;
    int failures = 0;
    static BYTE out[4096];
    EMU_KSWFEXT fmt;
    KSMULTIPLE_ITEM mi;

    MultiByteToWideChar(CP_UTF8, 0, devId, -1, devIdW, 512);

    if (CM_Get_Device_Interface_List_SizeW(&listLen,
            (LPGUID)&KSCATEGORY_AUDIO, devIdW,
            CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS || !listLen) {
        printf("no KSCATEGORY_AUDIO interfaces for device\n");
        return 1;
    }
    list = (WCHAR*)HeapAlloc(GetProcessHeap(), 0, listLen * sizeof(WCHAR));
    if (CM_Get_Device_Interface_ListW((LPGUID)&KSCATEGORY_AUDIO, devIdW, list,
            listLen, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS) {
        printf("CM_Get_Device_Interface_List failed\n");
        return 1;
    }
    printf("KSCATEGORY_AUDIO interfaces:\n");
    for (i = 0; list[i]; i += wcslen(list + i) + 1) {
        WCHAR* slash = wcsrchr(list + i, L'\\');
        printf("  %ls\n", list + i);
        if (slash && _wcsnicmp(slash + 1, L"wave", 4) == 0 &&
            h == INVALID_HANDLE_VALUE) {
            h = CreateFileW(list + i, GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                            OPEN_EXISTING, 0, NULL);
        }
    }
    if (h == INVALID_HANDLE_VALUE) {
        printf("wave filter not found or cannot open (GetLastError=%lu)\n",
               (unsigned long)GetLastError());
        return 1;
    }
    printf("wave filter opened\n\n");

    /* 1. DATARANGES — what the pin advertises */
    if (PropGet(h, EMU_KSPROPERTY_PIN_DATARANGES, NULL, 0, out, sizeof(out),
                "DATARANGES") == 0) {
        PKSMULTIPLE_ITEM mi2 = (PKSMULTIPLE_ITEM)out;
        ULONG n = 0;
        BYTE* end = out + mi2->Size;
        for (BYTE* p = (BYTE*)(mi2 + 1); p < end; n++) {
            PKSDATARANGE r = (PKSDATARANGE)p;
            if (end - p < sizeof(KSDATAFORMAT) || r->FormatSize < sizeof(KSDATAFORMAT) ||
                p + r->FormatSize > end) {
                /* not a data range (attribute block follows an
                 * KSDATARANGE_ATTRIBUTES range) — hex-dump the rest */
                printf("  tail[%lu]: %lu bytes:", n,
                       (unsigned long)(end - p));
                for (ULONG b = 0; p + b < end && b < 48; b++)
                    printf(" %02X", p[b]);
                printf("\n");
                break;
            }
            printf("  range[%lu]: size=%lu flags=0x%08lX major=", n,
                   r->FormatSize, r->Flags);
            PrintGuid(r->MajorFormat);
            printf(" sub="); PrintGuid(r->SubFormat);
            printf(" spec="); PrintGuid(r->Specifier); printf("\n");
            if (IsEqualGUID(r->Specifier,
                                   KSDATAFORMAT_SPECIFIER_WAVEFORMATEX) &&
                r->FormatSize >= sizeof(KSDATARANGE_AUDIO)) {
                PKSDATARANGE_AUDIO a = (PKSDATARANGE_AUDIO)r;
                printf("    channels=%lu bits=%lu..%lu rate=%lu..%lu\n",
                       (unsigned long)a->MaximumChannels,
                       (unsigned long)a->MinimumBitsPerSample,
                       (unsigned long)a->MaximumBitsPerSample,
                       (unsigned long)a->MinimumSampleFrequency,
                       (unsigned long)a->MaximumSampleFrequency);
            }
            if (!r->FormatSize) break;
            p += r->FormatSize;
        }
    }
    printf("\n");

    /* 2. PROPOSEDATAFORMAT2 GET with an empty attribute list — the modern
     *    call the audio engine uses to learn the default device format */
    mi.Size = sizeof(mi); mi.Count = 0;
    if (PropGet(h, EMU_KSPROPERTY_PIN_PROPOSEDATAFORMAT2, &mi, sizeof(mi),
                out, sizeof(out), "PROPOSEDATAFORMAT2 (empty attrs)") == 0)
        PrintFormat(out, sizeof(out));
    printf("\n");

    /* 3. PROPOSEDATAFORMAT2 GET with the DEFAULT-mode attribute */
    {
        BYTE attrs[sizeof(KSMULTIPLE_ITEM) + sizeof(KSATTRIBUTE)];
        PKSMULTIPLE_ITEM m = (PKSMULTIPLE_ITEM)attrs;
        PKSATTRIBUTE a = (PKSATTRIBUTE)(m + 1);
        m->Size = sizeof(attrs); m->Count = 1;
        a->Size = sizeof(KSATTRIBUTE); a->Flags = 0;
        a->Attribute = EMU_KSATTRIBUTEID_AUDIOSIGNALPROCESSING_MODE;
        if (PropGet(h, EMU_KSPROPERTY_PIN_PROPOSEDATAFORMAT2, attrs,
                    sizeof(attrs), out, sizeof(out),
                    "PROPOSEDATAFORMAT2 (default mode)") == 0)
            PrintFormat(out, sizeof(out));
    }
    printf("\n");

    /* 4. DATAINTERSECTION with a float32 wildcard probe — the legacy path
     *    PortCls services through the miniport/its own fallback */
    {
        BYTE in[sizeof(KSP_PIN) + sizeof(KSMULTIPLE_ITEM) +
                sizeof(KSDATARANGE_AUDIO)];
        KSP_PIN ksp;
        DWORD got;
        memset(&ksp, 0, sizeof(ksp));
        ksp.Property.Set = KSPROPSETID_Pin;
        ksp.Property.Id = EMU_KSPROPERTY_PIN_DATAINTERSECTION;
        ksp.Property.Flags = KSPROPERTY_TYPE_GET;
        ksp.PinId = EMU_WAVE_PIN_SINK;
        PKSMULTIPLE_ITEM m = (PKSMULTIPLE_ITEM)(in + sizeof(ksp));
        PKSDATARANGE_AUDIO r = (PKSDATARANGE_AUDIO)(m + 1);
        memcpy(in, &ksp, sizeof(ksp));
        m->Size = sizeof(KSMULTIPLE_ITEM) + sizeof(KSDATARANGE_AUDIO);
        m->Count = 1;
        memset(r, 0, sizeof(*r));
        r->DataRange.FormatSize = sizeof(KSDATARANGE_AUDIO);
        r->DataRange.Flags = 0;
        r->DataRange.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
        r->DataRange.SubFormat = GUID_NULL;
        r->DataRange.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
        r->MaximumChannels = 2;
        r->MinimumBitsPerSample = 32;
        r->MaximumBitsPerSample = 32;
        r->MinimumSampleFrequency = 48000;
        r->MaximumSampleFrequency = 48000;
        if (!DeviceIoControl(h, IOCTL_KS_PROPERTY, in, sizeof(in), out,
                             sizeof(out), &got, NULL)) {
            PrintWin32Error("DATAINTERSECTION (float32 wildcard)",
                            GetLastError());
            failures++;
        } else {
            printf("DATAINTERSECTION (float32 wildcard): OK, %lu bytes\n",
                   (unsigned long)got);
            PrintFormat(out, got);
        }
    }
    printf("\n");

    /* 7. BASICSUPPORT on the custom pin properties — distinguishes "item not
     *    in the effective automation table" from "handler GET/SET path bug" */
    {
        KSP_PIN ksp;
        DWORD got;
        memset(&ksp, 0, sizeof(ksp));
        ksp.Property.Set = KSPROPSETID_Pin;
        ksp.Property.Flags = KSPROPERTY_TYPE_BASICSUPPORT;
        ksp.PinId = EMU_WAVE_PIN_SINK;
        const ULONG ids[] = { 13, 14, 15, 16, 17,
                              EMU_KSPROPERTY_PIN_PROPOSEDATAFORMAT2,
                              15 /* MODEDATAFORMATS */ };
        for (ULONG k = 0; k < 5; k++) {
            ksp.Property.Id = ids[k];
            if (!DeviceIoControl(h, IOCTL_KS_PROPERTY, &ksp, sizeof(ksp),
                                 out, sizeof(out), &got, NULL)) {
                printf("BASICSUPPORT id=%lu failed, Win32 error %lu\n",
                       (unsigned long)ids[k], (unsigned long)GetLastError());
            } else {
                PKSPROPERTY_DESCRIPTION d = (PKSPROPERTY_DESCRIPTION)out;
                if (got >= sizeof(KSPROPERTY_DESCRIPTION)) {
                    printf("BASICSUPPORT id=%lu: OK, access=0x%08lX\n",
                           (unsigned long)ids[k],
                           (unsigned long)d->AccessFlags);
                } else if (got >= sizeof(ULONG)) {
                    printf("BASICSUPPORT id=%lu: OK (flags), access=0x%08lX\n",
                           (unsigned long)ids[k], *(ULONG*)out);
                } else {
                    printf("BASICSUPPORT id=%lu: OK, %lu bytes\n",
                           (unsigned long)ids[k], (unsigned long)got);
                }
            }
        }
    }
    printf("\n");

    /* 5. PROPOSEDATAFORMAT SET with float32 — what the shared-mode engine
     *    proposes for the mix format */
    memset(&fmt, 0, sizeof(fmt));
    fmt.DataFormat.FormatSize = sizeof(fmt);
    fmt.DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    fmt.DataFormat.SubFormat = EMU_GUID_IEEE_FLOAT;
    fmt.DataFormat.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
    fmt.WaveFormatEx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    fmt.WaveFormatEx.Format.nChannels = 2;
    fmt.WaveFormatEx.Format.nSamplesPerSec = 48000;
    fmt.WaveFormatEx.Format.nAvgBytesPerSec = 48000 * 2 * 4;
    fmt.WaveFormatEx.Format.nBlockAlign = 2 * 4;
    fmt.WaveFormatEx.Format.wBitsPerSample = 32;
    fmt.WaveFormatEx.Format.cbSize = (WORD)(sizeof(WAVEFORMATEXTENSIBLE) -
                                     sizeof(WAVEFORMATEX));
    fmt.WaveFormatEx.Samples.wValidBitsPerSample = 32;
    fmt.WaveFormatEx.dwChannelMask = KSAUDIO_SPEAKER_STEREO;
    fmt.WaveFormatEx.SubFormat = EMU_GUID_IEEE_FLOAT;
    failures += PropSetFormat(h, &fmt, "PROPOSEDATAFORMAT SET (float32 48000/2)");
    printf("\n");

    /* 6. PROPOSEDATAFORMAT SET with the device's own PCM format — must
     *    succeed if the negotiation basics work */
    fmt.DataFormat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    fmt.WaveFormatEx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    fmt.WaveFormatEx.Samples.wValidBitsPerSample = 24;
    fmt.WaveFormatEx.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    failures += PropSetFormat(h, &fmt, "PROPOSEDATAFORMAT SET (pcm32/24 48000/2)");

    printf("\nsizes: KSDATAFORMAT=%u EMU_KSWFEXT=%u "
           "WAVEFORMATEXTENSIBLE=%u\n",
           (unsigned)sizeof(KSDATAFORMAT),
           (unsigned)sizeof(EMU_KSWFEXT),
           (unsigned)sizeof(WAVEFORMATEXTENSIBLE));
    CloseHandle(h);
    return failures;
}
