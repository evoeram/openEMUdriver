#!/bin/bash
# Shared toolchain discovery for the driver scripts (kbuild.sh, utest.sh).
#
# Exports: MSVC_ROOT MSVC MSVC_VER CL LINK KITS_UNIX INC_ROOT LIB_ROOT SDKVER
#          WDF_INC WDF_LIB
# Overrides (all optional): OED_VS OED_MSVC OED_KITS OED_SDKVER OED_WDFVER
#
# SPDX-License-Identifier: LGPL-2.1-or-later

tf_err() { echo "toolchain: $*" >&2; exit 1; }

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
[ -n "$MSVC_ROOT" ] || tf_err "no Visual Studio with VC++ tools found.
  Install: winget install Microsoft.VisualStudio.2022.BuildTools --override
    \"--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended\"
  or point OED_VS at an existing install."

MSVC_MSVC_DIR=$(cygpath -u "$MSVC_ROOT")/VC/Tools/MSVC
[ -d "$MSVC_MSVC_DIR" ] || tf_err "no VC/Tools/MSVC under $MSVC_ROOT"

if [ -n "${OED_MSVC:-}" ]; then
    MSVC_VER=$OED_MSVC
else
    MSVC_VER=$(ls -1 "$MSVC_MSVC_DIR" | grep -E '^[0-9]+\.' | sort -V | tail -1)
fi
[ -n "$MSVC_VER" ] && [ -d "$MSVC_MSVC_DIR/$MSVC_VER" ] || tf_err "MSVC toolset '$MSVC_VER' not found in $MSVC_MSVC_DIR"
MSVC="$MSVC_MSVC_DIR/$MSVC_VER"

CL="$MSVC/bin/Hostx64/x64/cl.exe"
LINK="$MSVC/bin/Hostx64/x64/link.exe"
[ -f "$CL" ] || tf_err "cl.exe missing: $CL"
[ -f "$LINK" ] || tf_err "link.exe missing: $LINK"

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
[ -n "$KITS_UNIX" ] || tf_err "no Windows Kits found.
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
[ -n "$SDKVER" ] || tf_err "no 10.0.* SDK version under $INC_ROOT"
[ -f "$INC_ROOT/$SDKVER/km/ntddk.h" ] || tf_err \
    "km headers missing under $INC_ROOT/$SDKVER — that is the WDK part, the Windows SDK alone is not enough.
  Install: winget install Microsoft.WindowsWDK.10.0.26100"

# The WDK and the SDK can be installed at different versions; a single root has
# to carry both km headers and ucrt headers.
if [ -f "$INC_ROOT/$SDKVER/ucrt/stdio.h" ]; then
    WDK_LIB=$LIB_ROOT; SDK_LIB=$LIB_ROOT
else
    tf_err "ucrt headers missing under $INC_ROOT/$SDKVER — Windows SDK component is not installed
  (BuildTools: --add Microsoft.VisualStudio.Component.Windows11SDK.26100)."
fi

WDF_INC=$(ls -1d "$INC_ROOT"/wdf/kmdf/[0-9]* 2>/dev/null | sort -V | tail -1)
WDF_LIB=$(ls -1d "$LIB_ROOT"/wdf/kmdf/x64/[0-9]* 2>/dev/null | sort -V | tail -1)
if [ -n "${OED_WDFVER:-}" ]; then
    WDF_INC="$INC_ROOT/wdf/kmdf/$OED_WDFVER"
    WDF_LIB="$LIB_ROOT/wdf/kmdf/x64/$OED_WDFVER"
fi
[ -f "$WDF_INC/wdf.h" ] || tf_err "no KMDF headers under $INC_ROOT/wdf/kmdf — WDK incomplete"
