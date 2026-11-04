#!/bin/bash
# openEMUdriver build script (v1, user-mode: ASIO DLL + panel + tools)
# Requires a MinGW-w64 GCC (x86_64, ucrt runtime). Discovery order:
#   $MINGW (directory that contains g++.exe)
#   $MINGW_HOME/bin or $MINGW_HOME/mingw64/bin
#   WinLibs unpacked under C:\dev\tools\mingw64, MSYS2, C:\mingw64
#   g++ already on PATH
# SPDX-License-Identifier: LGPL-2.1-or-later
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build bin

find_gxx() {
    if [ -n "${MINGW:-}" ]; then
        for c in "$MINGW/g++.exe" "$MINGW/bin/g++.exe" "$MINGW/mingw64/bin/g++.exe"; do
            [ -f "$c" ] && { echo "$c"; return; }
        done
        echo "MINGW=$MINGW contains no g++.exe" >&2
        return
    fi
    if [ -n "${MINGW_HOME:-}" ]; then
        for c in "$MINGW_HOME/bin/g++.exe" "$MINGW_HOME/mingw64/bin/g++.exe"; do
            [ -f "$c" ] && { echo "$c"; return; }
        done
    fi
    for c in /c/dev/tools/mingw64/mingw64/bin/g++.exe \
             /c/dev/tools/*/mingw64/bin/g++.exe \
             /c/msys64/mingw64/bin/g++.exe \
             /c/mingw64/bin/g++.exe \
             /c/Dev/mingw64/bin/g++.exe; do
        [ -f "$c" ] && { echo "$c"; return; }
    done
    local p
    p=$(command -v g++ || true)
    [ -n "$p" ] && echo "$p"
}

CXX=$(find_gxx || true)
if [ -z "${CXX:-}" ]; then
    cat >&2 <<'EOF'
No MinGW-w64 g++ found.
  Install WinLibs GCC (ucrt, x86_64, posix-seh) and unpack it, then either
    export MINGW=/c/<path>/mingw64/bin
  or add its bin/ directory to PATH. See docs/BUILD.md for the exact archive.
EOF
    exit 1
fi
echo "using CXX: $CXX"
"$CXX" --version | head -1

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
$CXX $CXXFLAGS tools/emu-dump.cpp build/emu_usb.o -o bin/emu-dump.exe \
    -lwinusb -lsetupapi
$CXX -std=c++17 -O2 -Wall -Iinclude/asiosdk tools/asio-host-test.cpp \
    -o bin/asio-host-test.exe -lole32 -luuid -loleaut32
$CXX -std=c++17 -O2 -Wall -municode tools/wasapi-test.cpp -o bin/wasapi-test.exe \
    -lole32 -lpropsys -lm

echo "== panel =="
$CXX -std=c++17 -O2 -mwindows -Isrc panel/openemu-panel.cpp build/emu_usb.o \
    -o bin/openEMUPanel.exe -lwinusb -lsetupapi -lcomctl32 -lgdi32 -luser32

echo "OK: bin/openEMUdriver.asio bin/emu-dump.exe bin/asio-host-test.exe bin/wasapi-test.exe bin/openEMUPanel.exe"
