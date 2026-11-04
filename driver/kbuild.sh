#!/bin/bash
# Kernel build harness for the EmuUSBAudio PortCLS driver.
#
#   kbuild.sh <out.sys> <src...>          run from anywhere
#
# Toolchain is discovered, not hardcoded. Overrides (all optional):
#   OED_VS       Visual Studio install directory
#   OED_MSVC     MSVC toolset version (default: newest installed)
#   OED_KITS     Windows Kits root (MSI layout:  <root>/Include, <root>/Lib;
#                NuGet layout:                  <root>/c/Include, <root>/c/Lib)
#   OED_SDKVER   WDK/SDK version (default: newest under Include/)
#   OED_WDFVER   KMDF version (default: newest under Include/wdf/kmdf/)
#   OED_NTDDI    NTDDI_VERSION (default: 0x0A00000C)
#
# SPDX-License-Identifier: LGPL-2.1-or-later
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
DRV="$SCRIPT_DIR/emuusbaudio"
NTDDI=${OED_NTDDI:-0x0A00000C}

err() { echo "kbuild: $*" >&2; exit 1; }

# --- MSVC ------------------------------------------------------------------
find_msvc_root() {
    if [ -n "${OED_VS:-}" ]; then echo "$OED_VS"; return; fi
    # vswhere is the canonical locator, but it is not always present in a
    # BuildTools-only install, so fall back to globbing the standard layouts.
    local vw out
    for vw in "/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe" \
              "/c/Program Files/Microsoft Visual Studio/Installer/vswhere.exe"; do
        [ -f "$vw" ] || continue
        out=$("$vw" -latest -prerelease -products '*' \
                   -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 \
                   -property installationPath 2>/dev/null | head -1 | tr -d '\r')
        [ -n "$out" ] && { echo "$out"; return; }
    done
    local d
    for d in "/c/Program Files (x86)/Microsoft Visual Studio/2022"/* \
             "/c/Program Files/Microsoft Visual Studio/2022"/* \
             "/c/Program Files (x86)/Microsoft Visual Studio/2019"/* \
             "/c/Program Files/Microsoft Visual Studio/2019"/*; do
        [ -d "$d/VC/Tools/MSVC" ] || continue
        cygpath -w "$d"; return
    done
}

MSVC_ROOT=$(find_msvc_root || true)
[ -n "$MSVC_ROOT" ] || err "no Visual Studio with VC++ tools found.
  Install: winget install Microsoft.VisualStudio.2022.BuildTools --override
    \"--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended\"
  or point OED_VS at an existing install."

MSVC_MSVC_DIR=$(cygpath -u "$MSVC_ROOT")/VC/Tools/MSVC
[ -d "$MSVC_MSVC_DIR" ] || err "no VC/Tools/MSVC under $MSVC_ROOT"

if [ -n "${OED_MSVC:-}" ]; then
    MSVC_VER=$OED_MSVC
else
    MSVC_VER=$(ls -1 "$MSVC_MSVC_DIR" | grep -E '^[0-9]+\.' | sort -V | tail -1)
fi
[ -n "$MSVC_VER" ] && [ -d "$MSVC_MSVC_DIR/$MSVC_VER" ] || err "MSVC toolset '$MSVC_VER' not found in $MSVC_MSVC_DIR"
MSVC="$MSVC_MSVC_DIR/$MSVC_VER"

CL="$MSVC/bin/Hostx64/x64/cl.exe"
LINK="$MSVC/bin/Hostx64/x64/link.exe"
[ -f "$CL" ] || err "cl.exe missing: $CL"
[ -f "$LINK" ] || err "link.exe missing: $LINK"

# --- Windows Kits (WDK + SDK) ----------------------------------------------
find_kits() {
    if [ -n "${OED_KITS:-}" ]; then echo "$OED_KITS"; return; fi
    local cand
    for cand in "/c/Program Files (x86)/Windows Kits/10" \
                "/c/Program Files/Windows Kits/10"; do
        [ -d "$cand" ] && { echo "$cand"; return; }
    done
    for cand in /c/dev/tools/wdk /c/dev/tools/sdkall; do
        [ -d "$cand/c/Include" ] && { echo "$cand"; return; }
    done
}

KITS_UNIX=$(find_kits || true)
[ -n "$KITS_UNIX" ] || err "no Windows Kits found.
  Install the WDK:  winget install Microsoft.WindowsWDK.10.0.26100
  or point OED_KITS at a WDK/SDK tree (MSI or NuGet layout)."

# include/lib may live at <root>/Include or <root>/c/Include (NuGet layout)
kit_inc() { if [ -d "$KITS_UNIX/Include" ]; then echo "$KITS_UNIX/Include"; else echo "$KITS_UNIX/c/Include"; fi; }
kit_lib() { if [ -d "$KITS_UNIX/Lib" ];    then echo "$KITS_UNIX/Lib";    else echo "$KITS_UNIX/c/Lib";    fi; }

INC_ROOT=$(kit_inc)
LIB_ROOT=$(kit_lib)

if [ -n "${OED_SDKVER:-}" ]; then
    SDKVER=$OED_SDKVER
else
    SDKVER=$(ls -1 "$INC_ROOT" 2>/dev/null | grep -E '^10\.0\.[0-9]+\.[0-9]+$' | sort -V | tail -1)
fi
[ -n "$SDKVER" ] || err "no 10.0.* SDK version under $INC_ROOT"
[ -f "$INC_ROOT/$SDKVER/km/ntddk.h" ] || err \
    "km headers missing under $INC_ROOT/$SDKVER — that is the WDK part, the Windows SDK alone is not enough.
  Install: winget install Microsoft.WindowsWDK.10.0.26100"

# The WDK and the SDK can be installed at different versions; prefer a version
# that has both km headers and ucrt headers, fall back to two roots.
pick_shared_root() {
    if [ -f "$INC_ROOT/$SDKVER/ucrt/stdio.h" ]; then
        WDK_LIB=$LIB_ROOT; SDK_LIB=$LIB_ROOT
    else
        err "ucrt headers missing under $INC_ROOT/$SDKVER — Windows SDK component is not installed
  (BuildTools: --add Microsoft.VisualStudio.Component.Windows11SDK.26100)."
    fi
}
pick_shared_root

WDF_INC=$(ls -1d "$INC_ROOT"/wdf/kmdf/[0-9]* 2>/dev/null | sort -V | tail -1)
WDF_LIB=$(ls -1d "$LIB_ROOT"/wdf/kmdf/x64/[0-9]* 2>/dev/null | sort -V | tail -1)
if [ -n "${OED_WDFVER:-}" ]; then
    WDF_INC="$INC_ROOT/wdf/kmdf/$OED_WDFVER"
    WDF_LIB="$LIB_ROOT/wdf/kmdf/x64/$OED_WDFVER"
fi
[ -f "$WDF_INC/wdf.h" ] || err "no KMDF headers under $INC_ROOT/wdf/kmdf — WDK incomplete"

w() { cygpath -w "$1"; }

# Proven include order: km/crt must precede ucrt, otherwise _CRTIMP_ALT explodes.
export INCLUDE="$(w "$INC_ROOT/$SDKVER/km/crt");$(w "$INC_ROOT/$SDKVER/ucrt");$(w "$INC_ROOT/$SDKVER/um");$(w "$INC_ROOT/$SDKVER/shared");$(w "$MSVC/include");$(w "$INC_ROOT/$SDKVER/km");$(w "$INC_ROOT/$SDKVER/shared");$(w "$DRV/Inc");$(w "$DRV/Emu");$(w "$DRV/Main");$(w "$DRV/Filters");$(w "$DRV/Utilities");$(w "$WDF_INC");$(w "${WDF_INC%/*}/umdf")"
export LIB="$(w "$MSVC/lib/x64");$(w "$WDK_LIB/$SDKVER/km/x64");$(w "$SDK_LIB/$SDKVER/um/x64");$(w "$WDF_LIB")"
export PATH="$MSVC/bin/Hostx64/x64:$PATH"
export MSYS_NO_PATHCONV=1

for l in ntoskrnl hal wdm libcntpr portcls stdunk ksguid ks; do
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
    # shellcheck disable=SC2086
    "$CL" -nologo -W3 -WX- -c -Zi -kernel -GS- -D_AMD64_ -DAMD64 -DNTDDI_VERSION=$NTDDI \
          -D_WIN32_WINNT=0x0A00 -DNDEBUG -D_NEW_DELETE_OPERATORS_ \
          "-Fo$(cygpath -w "$obj")" "$switch$(cygpath -w "$f")" || exit 1
    OBJS="$OBJS $(cygpath -w "$obj")"
done
# shellcheck disable=SC2086
"$LINK" -nologo -DRIVER -SUBSYSTEM:NATIVE "-OUT:$OUT" -ENTRY:DriverEntry -INCREMENTAL:NO \
        $OBJS ntoskrnl.lib hal.lib wdm.lib libcntpr.lib \
        portcls.lib stdunk.lib ksguid.lib ks.lib wdfdriverentry.lib wdfldr.lib
echo "BUILT: $OUT  (MSVC $MSVC_VER, kits $(cygpath -w "$KITS_UNIX"), SDK $SDKVER, WDF $(basename "$WDF_LIB"))"
