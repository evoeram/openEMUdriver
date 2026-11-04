#!/bin/bash
# Build the user-mode control-interface probe (driver/tools/emu_probe.c).
#
#   kprobe.sh          build emuusbaudio/build/emu-probe.exe
#   kprobe.sh -run     build and run it (the device must be bound to
#                      openemu-ks.inf and started; run elevated, the device
#                      object is admin-only)
#
# User-mode flags mirror the utest.sh user-mode step: the probe compiles the
# same Emu/emu_ioctl.h the driver ships, so a layout drift is a compile error,
# not a wrong number on screen.
#
# SPDX-License-Identifier: LGPL-2.1-or-later
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
DRV="$SCRIPT_DIR/emuusbaudio"
SRC="$SCRIPT_DIR/tools/emu_probe.c"
OUT="$DRV/build/emu-probe.exe"

err() { echo "kprobe: $*" >&2; exit 1; }

[ -f "$SRC" ] || err "probe source missing: $SRC"

RUN=0
[ $# -le 1 ] || err "usage: kprobe.sh [-run]"
[ $# -eq 1 ] && { [ "$1" = "-run" ] || err "unknown argument: $1"; RUN=1; }

. "$SCRIPT_DIR/toolfound.sh"

w() { cygpath -w "$1"; }
mkdir -p "$DRV/build"

export INCLUDE="$(w "$INC_ROOT/$SDKVER/ucrt");$(w "$INC_ROOT/$SDKVER/um");$(w "$INC_ROOT/$SDKVER/shared");$(w "$MSVC/include");$(w "$DRV/Emu")"
export LIB="$(w "$MSVC/lib/x64");$(w "$SDK_LIB/$SDKVER/um/x64");$(w "$SDK_LIB/$SDKVER/ucrt/x64")"
export PATH="$MSVC/bin/Hostx64/x64:$PATH"
export MSYS_NO_PATHCONV=1

"$CL" -nologo -W3 -WX -Zi -D_WIN32_WINNT=0x0A00 -DNDEBUG \
      "-Fe$(w "$OUT")" "-Fo$(w "$DRV/build/")" -Tc$(w "$SRC") \
      -link setupapi.lib
echo "BUILT: $(cygpath -w "$OUT")  (MSVC $MSVC_VER, SDK $SDKVER)"

if [ "$RUN" = "1" ]; then
    "$OUT"
fi
