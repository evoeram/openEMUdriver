// Shared descriptor fixture for the EmuUSBAudio unit tests.
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The reconstructed 0404 configuration descriptor (see the note in
// emu_desctest.cpp: it is built from the measurements in docs/RE_NOTES.md, not
// captured from hardware). Any test that needs a parsed EMU_DEVICE_CAPS builds
// these bytes and runs them through EmuParseUsbConfigDescriptor, so there is one
// description of the device in the tree and it is replaced once in one place
// when `emu-dump -o docs/descriptors` produces a live capture.
#ifndef _EMU_TEST_DESCFIXTURE_H_
#define _EMU_TEST_DESCFIXTURE_H_

#include <initializer_list>
#include <vector>
#include "emu_desc.h"

typedef std::vector<unsigned char> Block;

static void Bytes(_Inout_ Block& b, std::initializer_list<int> values)
{
    for (int v : values) {
        b.push_back((unsigned char)v);
    }
}

static void U16(_Inout_ Block& b, _In_ unsigned value)
{
    b.push_back((unsigned char)(value & 0xFF));
    b.push_back((unsigned char)((value >> 8) & 0xFF));
}

static void U24(_Inout_ Block& b, _In_ unsigned value)
{
    b.push_back((unsigned char)(value & 0xFF));
    b.push_back((unsigned char)((value >> 8) & 0xFF));
    b.push_back((unsigned char)((value >> 16) & 0xFF));
}

// Configuration descriptor header: nine bytes, patched by ConfigFinish().
static void ConfigStart(_Inout_ Block& b)
{
    Bytes(b, {9, 0x02, 0, 0, 0, 1, 0, 0xC0, 250});
}

static void ConfigFinish(_Inout_ Block& b, _In_ unsigned numInterfaces)
{
    unsigned total = (unsigned)b.size();
    b[2] = (unsigned char)(total & 0xFF);
    b[3] = (unsigned char)(total >> 8);
    b[4] = (unsigned char)numInterfaces;
}

// Standard interface descriptor (9 bytes).
static void Interface(_Inout_ Block& b, _In_ unsigned number, _In_ unsigned alt,
                      _In_ unsigned numEndpoints, _In_ unsigned cls,
                      _In_ unsigned subclass, _In_ unsigned protocol)
{
    Bytes(b, {9, 0x04, (int)number, (int)alt, (int)numEndpoints,
              (int)cls, (int)subclass, (int)protocol, 0});
}

static void Endpoint(_Inout_ Block& b, _In_ unsigned address, _In_ unsigned attributes,
                     _In_ unsigned maxPacket, _In_ unsigned interval)
{
    Bytes(b, {7, 0x05, (int)address, (int)attributes});
    U16(b, maxPacket);
    b.push_back((unsigned char)interval);
}

// AudioControl HEADER (8 bytes).
static void AcHeader(_Inout_ Block& b)
{
    Bytes(b, {8, 0x24, 0x01, 0x00, 0x01});
    U16(b, 0x0100);          // wTotalLength of the AC interface, informational
    b.push_back(0x02);       // bInChannelSizes
}

// INPUT_TERMINAL / OUTPUT_TERMINAL: the size grows with bControls, so the
// builders compute bLength from what they actually emitted.
static unsigned Mark(_In_ const Block& b)
{
    return (unsigned)b.size();
}

static void FixLength(_Inout_ Block& b, _In_ unsigned start)
{
    b[start] = (unsigned char)(b.size() - start);
}

static void InputTerminal(_Inout_ Block& b, _In_ unsigned id, _In_ unsigned type,
                          _In_ unsigned channels)
{
    unsigned start = Mark(b);
    Bytes(b, {0, 0x24, 0x02, (int)id});
    U16(b, type);
    Bytes(b, {0x00});        // bAssocTerminal
    b.push_back(0x00);       // bSourceID
    U16(b, channels == 2 ? 0x0003 : 0x000F);   // wChannelConfig
    Bytes(b, {0x00, 0x00, 0x00});              // iChannelNames, bControls, iTerminal
    FixLength(b, start);
}

static void OutputTerminal(_Inout_ Block& b, _In_ unsigned id, _In_ unsigned type,
                           _In_ unsigned channels)
{
    unsigned start = Mark(b);
    Bytes(b, {0, 0x24, 0x03, (int)id});
    U16(b, type);
    Bytes(b, {0x00});        // bAssocTerminal
    b.push_back(0x01);       // bSourceID
    U16(b, channels == 2 ? 0x0003 : 0x000F);
    Bytes(b, {0x00, 0x00, 0x00});
    FixLength(b, start);
}

// FEATURE_UNIT: master + `channels` controls, one byte each.
static void FeatureUnit(_Inout_ Block& b, _In_ unsigned id, _In_ unsigned source,
                        _In_ unsigned channels, _In_ unsigned controlMask)
{
    unsigned length = 6 + (channels + 1) + 1;
    b.push_back((unsigned char)length);
    Bytes(b, {0x24, 0x06, (int)id, (int)source, 0x01});
    for (unsigned i = 0; i <= channels; i++) {
        b.push_back((unsigned char)controlMask);
    }
    b.push_back(0x00);       // iFeature
}

// EXTENSION_UNIT in the layout CA0189 actually uses: code right after unit ID.
static void XuEmu(_Inout_ Block& b, _In_ unsigned id, _In_ unsigned code)
{
    Bytes(b, {8, 0x24, 0x08, (int)id});
    U16(b, code);
    Bytes(b, {0x01, 0x03});  // padding so the descriptor really is 8 bytes long
}

// EXTENSION_UNIT in the class-spec layout:
// bUnitID, bNrSources, bmaSource[], baCnSource[4n], wExtensionCode, baControlSize.
static void XuSpec(_Inout_ Block& b, _In_ unsigned id, _In_ unsigned code)
{
    Bytes(b, {14, 0x24, 0x08, (int)id, 0x01, 0x03});
    Bytes(b, {0x03, 0x00, 0x00, 0x00});
    U16(b, code);
    Bytes(b, {0x01, 0x03});
}

// AudioStreaming AS_GENERAL (7 bytes).
static void AsGeneral(_Inout_ Block& b, _In_ unsigned terminalLink, _In_ unsigned formatTag)
{
    Bytes(b, {7, 0x24, 0x01, (int)terminalLink, 0x03});
    U16(b, formatTag);
}

static void FormatTypeIDiscrete(_Inout_ Block& b, _In_ unsigned channels,
                                _In_ unsigned subFrame, _In_ unsigned bitResolution,
                                const std::vector<unsigned>& rates)
{
    b.push_back((unsigned char)(8 + 3 * rates.size()));
    Bytes(b, {0x24, 0x02, 0x01, (int)channels, (int)subFrame, (int)bitResolution,
              (int)rates.size()});
    for (unsigned r : rates) {
        U24(b, r);
    }
}

static void FormatTypeIContinuous(_Inout_ Block& b, _In_ unsigned channels,
                                  _In_ unsigned subFrame, _In_ unsigned bitResolution,
                                  _In_ unsigned minHz, _In_ unsigned maxHz)
{
    Bytes(b, {14, 0x24, 0x02, 0x01, (int)channels, (int)subFrame, (int)bitResolution, 0x00});
    U24(b, minHz);
    U24(b, maxHz);
}

// One audio interface of the reconstructed 0404: AC with terminals, one feature
// unit and the six E-MU extension units.
static void AcInterface(_Inout_ Block& b, _In_ unsigned number, _In_ unsigned cls)
{
    Interface(b, number, 0, 1, cls, 0x01, 0x00);
    AcHeader(b);
    InputTerminal(b, 0x01, 0x0201, 2);      // digital media input
    OutputTerminal(b, 0x03, 0x0301, 2);     // speaker output -> render streams
    InputTerminal(b, 0x05, 0x0601, 2);      // microphone input -> capture streams
    FeatureUnit(b, 0x02, 0x01, 2, 0x03);    // VOLUME|MUTE on master + 2 channels
    XuEmu(b, 0x0A, EMU_XU_CLOCK_RATE);
    XuEmu(b, 0x0B, EMU_XU_CLOCK_SOURCE);
    XuEmu(b, 0x0C, EMU_XU_DIGITAL_IO);
    XuEmu(b, 0x0D, EMU_XU_DEVICE_OPTIONS);
    XuEmu(b, 0x0E, EMU_XU_DIRECT_MONITOR);
    XuEmu(b, 0x0F, EMU_XU_METERING);
    Endpoint(b, 0x83, 0x03, 2, 1);          // interrupt, 2-byte status packets
}

// The reconstructed 0404: render 2ch over 44.1-192 kHz plus render 4ch over
// 44.1-96 kHz, capture 2ch over 44.1/48 kHz. Render alt 0 and capture alt 0 are
// the zero-bandwidth settings.
static void Build0404(_Out_ Block& b)
{
    b.clear();
    ConfigStart(b);
    AcInterface(b, 0, EMU_INTERFACE_CLASS_VENDOR);

    Interface(b, 1, 0, 0, EMU_INTERFACE_CLASS_VENDOR, 0x02, 0x00);
    Interface(b, 1, 1, 2, EMU_INTERFACE_CLASS_VENDOR, 0x02, 0x00);
    AsGeneral(b, 0x03, 1);
    FormatTypeIDiscrete(b, 2, 3, 24, {44100, 48000, 88200, 96000, 176400, 192000});
    Endpoint(b, 0x00, 0x01, 1152, 1);       // OUT, isochronous async, data
    Endpoint(b, 0x81, 0x11, 4, 1);          // IN, isochronous feedback, 4 bytes
    Interface(b, 1, 2, 1, EMU_INTERFACE_CLASS_VENDOR, 0x02, 0x00);
    AsGeneral(b, 0x03, 1);
    FormatTypeIDiscrete(b, 4, 3, 24, {44100, 48000, 88200, 96000});
    Endpoint(b, 0x00, 0x01, 1152, 1);

    Interface(b, 2, 0, 0, EMU_INTERFACE_CLASS_VENDOR, 0x02, 0x00);
    Interface(b, 2, 1, 1, EMU_INTERFACE_CLASS_VENDOR, 0x02, 0x00);
    AsGeneral(b, 0x05, 1);
    FormatTypeIDiscrete(b, 2, 3, 24, {44100, 48000});
    Endpoint(b, 0x82, 0x25, 192, 1);        // IN, async, usage type 2
    ConfigFinish(b, 3);
}

// Parse into Caps, or stop the test: every fixture-driven assertion below
// assumes the parse itself worked.
static bool Parse0404(_Out_ EMU_DEVICE_CAPS* caps)
{
    Block b;
    Build0404(b);
    return EmuParseUsbConfigDescriptor(b.data(), (ULONG)b.size(), caps) == STATUS_SUCCESS;
}

#endif // _EMU_TEST_DESCFIXTURE_H_
