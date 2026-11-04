#!/bin/bash
# User-mode unit tests for the EmuUSBAudio driver sources (M2 onwards).
#
#   utest.sh                  build and run every test
#   utest.sh -n               build only, do not run
#   utest.sh desc ctrl state ioctl   only the named units
#                  (desc = parser, ctrl = EP0 requests, state = settings cache,
#                   ioctl = the panel contract)
#
# Per unit, two things are proven, in this order:
#   1. the source compiles with the exact kernel flags kbuild.sh uses, so the
#      object code under test is the code that goes into emuusbaudio.sys;
#   2. the same source, compiled user-mode, is linked with its
#      tests/<unit>test.cpp into build/<unit>test.exe and the cases are executed.
#      Each binary prints "RESULT: PASS" or "RESULT: FAIL".
#
# A unit can need more than one source: the cache in Emu/emu_state.cpp decides
# what to believe from the tables Emu/emu_desc.cpp produced, so its binary links
# both and the test runs against a parsed descriptor instead of a hand-written
# one. Every source listed is compiled both ways.
#
# The dual-target sources (Emu/emu_desc.h) pick their types from EMU_KERNEL, so
# step 1 and step 2 really are two compilations of one file, not two files.
#
# Toolchain discovery lives in toolfound.sh, shared with kbuild.sh.
#   OED_NTDDI    NTDDI_VERSION (default: 0x0A00000C)
#
# SPDX-License-Identifier: LGPL-2.1-or-later
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
DRV="$SCRIPT_DIR/emuusbaudio"
NTDDI=${OED_NTDDI:-0x0A00000C}
RUN=1
WANT=""

err() { echo "utest: $*" >&2; exit 1; }

for a in "$@"; do
    case "$a" in
        -n) RUN=0 ;;
        desc|ctrl|state|ioctl|isoch) WANT="$WANT $a" ;;
        *) err "unknown argument: $a" ;;
    esac
done
[ -n "$WANT" ] || WANT="desc ctrl state ioctl isoch"

. "$SCRIPT_DIR/toolfound.sh"

w() { cygpath -w "$1"; }
mkdir -p "$DRV/build"

# unit -> sources under test (space-separated; a unit may need a helper module)
# and its test driver.
src_for() {
    case "$1" in
        desc)  echo "$DRV/Emu/emu_desc.cpp" ;;
        ctrl)  echo "$DRV/Emu/emu_ctrl.cpp" ;;
        state) echo "$DRV/Emu/emu_state.cpp $DRV/Emu/emu_desc.cpp" ;;
        ioctl) echo "$DRV/Emu/emu_ioctl.cpp $DRV/Emu/emu_desc.cpp $DRV/Emu/emu_state.cpp" ;;
        isoch) echo "$DRV/Emu/emu_isoch.cpp" ;;
    esac
}
test_for() { echo "$SCRIPT_DIR/tests/emu_$1test.cpp"; }

for u in $WANT; do
    for s in $(src_for "$u"); do
        [ -f "$s" ] || err "source under test missing: $s"
    done
    [ -f "$(test_for "$u")" ] || err "test source missing: $(test_for "$u")"
done

# --- 1. kernel-mode compile --------------------------------------------------
# Flags copied from kbuild.sh on purpose: if a unit stops compiling here, the
# driver build stops too.
export INCLUDE="$(w "$INC_ROOT/$SDKVER/km/crt");$(w "$INC_ROOT/$SDKVER/ucrt");$(w "$INC_ROOT/$SDKVER/um");$(w "$INC_ROOT/$SDKVER/shared");$(w "$MSVC/include");$(w "$INC_ROOT/$SDKVER/km");$(w "$DRV/Emu")"
export LIB="$(w "$MSVC/lib/x64");$(w "$WDK_LIB/$SDKVER/km/x64")"
export PATH="$MSVC/bin/Hostx64/x64:$PATH"
export MSYS_NO_PATHCONV=1

for u in $WANT; do
    for s in $(src_for "$u"); do
        "$CL" -nologo -W3 -WX -c -Zi -kernel -GS- -D_AMD64_ -DAMD64 -DNTDDI_VERSION=$NTDDI \
              -D_WIN32_WINNT=0x0A00 -DNDEBUG -DEMU_KERNEL \
              "-Fo$(w "$DRV/build/Emu-$(basename "$s" .cpp).kernel.obj")" \
              "-Tp$(w "$s")"
        echo "KERNEL-COMPILE: $(basename "$s") ok (kernel flags, /WX)"
    done
done

# --- 2. user-mode test binaries ----------------------------------------------
export INCLUDE="$(w "$INC_ROOT/$SDKVER/ucrt");$(w "$INC_ROOT/$SDKVER/um");$(w "$INC_ROOT/$SDKVER/shared");$(w "$MSVC/include");$(w "$DRV/Emu");$(w "$SCRIPT_DIR/tests")"
export LIB="$(w "$MSVC/lib/x64");$(w "$SDK_LIB/$SDKVER/um/x64");$(w "$SDK_LIB/$SDKVER/ucrt/x64")"

FAILED=0
RAN=0
for u in $WANT; do
    SOURCES=()
    for s in $(src_for "$u"); do
        SOURCES+=("-Tp$(w "$s")")
    done
    SOURCES+=("-Tp$(w "$(test_for "$u")")")
    "$CL" -nologo -W3 -WX -EHsc -Zi -D_WIN32_WINNT=0x0A00 -DNDEBUG \
          "-Fe$(w "$DRV/build/emu_${u}test.exe")" \
          "-Fo$(w "$DRV/build/")" \
          "${SOURCES[@]}"
    echo "BUILT: $(cygpath -w "$DRV/build/emu_${u}test.exe")  (MSVC $MSVC_VER, SDK $SDKVER)"
    if [ "$RUN" = "1" ]; then
        RAN=$((RAN + 1))
        "$DRV/build/emu_${u}test.exe" || FAILED=$((FAILED + 1))
    fi
done

if [ "$RUN" = "1" ]; then
    if [ "$FAILED" -ne 0 ]; then
        echo "UTEST: $FAILED of $RAN test binaries failed"
        exit 1
    fi
    echo "UTEST: $RAN of $RAN test binaries passed"
fi
