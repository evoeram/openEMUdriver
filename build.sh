#!/bin/bash
# openEMUdriver build script (MinGW-w64 / WinLibs GCC)
# SPDX-License-Identifier: LGPL-2.1-or-later
set -e
M=${MINGW:-/c/dev/tools/mingw64/mingw64/bin}
CXX="$M/g++.exe"
cd "$(dirname "$0")"
mkdir -p build bin

CXXFLAGS="-std=c++17 -O2 -Wall -Isrc -Isrc/asiosdk -Iinclude/asiosdk"

echo "== core =="
$CXX $CXXFLAGS -c src/emu_usb.cpp    -o build/emu_usb.o
$CXX $CXXFLAGS -c src/emu_stream.cpp -o build/emu_stream.o

echo "== asio sdk + driver =="
$CXX $CXXFLAGS -w -c src/asiosdk/combase.cpp  -o build/combase.o
$CXX $CXXFLAGS -w -c src/asiosdk/dllentry.cpp -o build/dllentry.o
$CXX $CXXFLAGS -w -c src/asiosdk/register.cpp -o build/register.o
$CXX $CXXFLAGS -w -c src/asiodrvr_base.cpp    -o build/asiodrvr_base.o
$CXX $CXXFLAGS -c src/asio_emu.cpp            -o build/asio_emu.o

echo "== link openEMUdriver.asio =="
$CXX $CXXFLAGS -shared -o bin/openEMUdriver.asio \
    build/emu_usb.o build/emu_stream.o build/asio_emu.o \
    build/asiodrvr_base.o build/combase.o build/dllentry.o build/register.o \
    -lwinusb -lsetupapi -lole32 -luuid -lwinmm -ladvapi32

echo "== tools =="
$CXX $CXXFLAGS tools/emu-dump.cpp build/emu_usb.o -o bin/emu-dump.exe -lwinusb -lsetupapi
$CXX -std=c++17 -O2 -Wall -Iinclude/asiosdk tools/asio-host-test.cpp -o bin/asio-host-test.exe -lole32 -luuid -loleaut32

echo "== panel =="
$CXX -std=c++17 -O2 -mwindows -Wall -Isrc panel/openemu-panel.cpp build/emu_usb.o \
    -o bin/openEMUPanel.exe -lwinusb -lsetupapi -lcomctl32 -lgdi32 -luser32

echo "OK: bin/openEMUdriver.asio bin/emu-dump.exe bin/asio-host-test.exe bin/openEMUPanel.exe"
