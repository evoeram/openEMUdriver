#!/bin/bash
# Kernel build harness for the EmuUSBAudio PortCLS driver.
#
#   kbuild.sh <out.sys> <src...>          run from anywhere
#
# Toolchain is discovered, not hardcoded — see toolfound.sh for the search and
# for the OED_VS / OED_MSVC / OED_KITS / OED_SDKVER / OED_WDFVER overrides.
#   OED_NTDDI    NTDDI_VERSION (default: 0x0A00000C)
#
# SPDX-License-Identifier: LGPL-2.1-or-later
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
DRV="$SCRIPT_DIR/emuusbaudio"
NTDDI=${OED_NTDDI:-0x0A00000C}

err() { echo "kbuild: $*" >&2; exit 1; }

. "$SCRIPT_DIR/toolfound.sh"

w() { cygpath -w "$1"; }

# Proven include order: km/crt must precede ucrt, otherwise _CRTIMP_ALT explodes.
export INCLUDE="$(w "$INC_ROOT/$SDKVER/km/crt");$(w "$INC_ROOT/$SDKVER/ucrt");$(w "$INC_ROOT/$SDKVER/um");$(w "$INC_ROOT/$SDKVER/shared");$(w "$MSVC/include");$(w "$INC_ROOT/$SDKVER/km");$(w "$INC_ROOT/$SDKVER/shared");$(w "$DRV/Inc");$(w "$DRV/Emu");$(w "$DRV/Main");$(w "$DRV/Filters");$(w "$DRV/Utilities");$(w "$WDF_INC");$(w "${WDF_INC%/*}/umdf")"
export LIB="$(w "$MSVC/lib/x64");$(w "$WDK_LIB/$SDKVER/km/x64");$(w "$SDK_LIB/$SDKVER/um/x64");$(w "$WDF_LIB")"
export PATH="$MSVC/bin/Hostx64/x64:$PATH"
export MSYS_NO_PATHCONV=1

# Kernel /GS support (see the link line below) and the USB helper libraries.
for l in ntoskrnl hal wdm libcntpr portcls stdunk ksguid ks usbd usbdex bufferoverflowk wdmsec; do
    ls "$WDK_LIB/$SDKVER/km/x64/$l.lib" >/dev/null 2>&1 || err "link library missing: $l.lib (WDK $SDKVER km/x64)"
done
for l in wdfdriverentry wdfldr; do
    ls "$WDF_LIB/$l.lib" >/dev/null 2>&1 || err "link library missing: $l.lib (KMDF $WDF_LIB)"
done

[ $# -ge 2 ] || err "usage: kbuild.sh <out.sys> <src...>"
OUT=$(cygpath -w "$1"); shift
mkdir -p "$DRV/build"
OBJS=""
for f in "$@"; do
    [ -f "$f" ] || err "source not found: $f"
    case "$f" in
        *.cpp) switch=-Tp ;;
        *)     switch=-Tc ;;
    esac
    # Directory stays in the object name: sources that share a base name in
    # Main/ and Filters/ must not overwrite each other.
    obj="$DRV/build/$(basename "$(dirname "$f")")-$(basename "${f%.*}").obj"
    # EMU_KERNEL tells a dual-target source (Emu/emu_desc.h) that it is being
    # built for the driver image and must take its types from <ntddk.h>.
    # shellcheck disable=SC2086
    "$CL" -nologo -W3 -WX- -c -Zi -kernel -GS- -D_AMD64_ -DAMD64 -DNTDDI_VERSION=$NTDDI \
          -D_WIN32_WINNT=0x0A00 -DNDEBUG -D_NEW_DELETE_OPERATORS_ -DEMU_KERNEL \
          "-Fo$(cygpath -w "$obj")" "$switch$(cygpath -w "$f")" || exit 1
    OBJS="$OBJS $(cygpath -w "$obj")"
done
# LIBCMT stays out and BufferOverflowK.lib supplies the security-check runtime:
# /GS-built libraries (usbdex.lib, and anything from the sample tree) otherwise
# resolve __report_gsfailure to LIBCMT's user-mode gs_report.obj, whose
# __imp_QueryPerformanceCounter/__imp_TerminateProcess references cannot be
# satisfied in a driver image (M0's LNK2019 wall).
# shellcheck disable=SC2086
"$LINK" -nologo -DRIVER -SUBSYSTEM:NATIVE "-OUT:$OUT" -ENTRY:DriverEntry \
        -INCREMENTAL:NO -NODEFAULTLIB:LIBCMT.lib \
        $OBJS ntoskrnl.lib hal.lib wdm.lib libcntpr.lib \
        portcls.lib stdunk.lib ksguid.lib ks.lib bufferoverflowk.lib wdmsec.lib \
        usbd.lib usbdex.lib wdfdriverentry.lib wdfldr.lib
echo "BUILT: $OUT  (MSVC $MSVC_VER, kits $(cygpath -w "$KITS_UNIX"), SDK $SDKVER, WDF $(basename "$WDF_LIB"))"
