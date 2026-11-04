#!/bin/bash
# kernel build harness: kbuild.sh <out.sys> <src...>  (run from project dir)
set -e
MSVC="C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Tools\\MSVC\\14.44.35207"
WDK="C:\\dev\\tools\\wdk\\c\\Include\\10.0.26100.0"
WDKLIB="C:\\dev\\tools\\wdk\\c\\Lib\\10.0.26100.0\\km\\x64"
SDKINC="C:\\dev\\tools\\sdkall\\c\\Include\\10.0.26100.0"
SDKLIB="C:\\dev\\tools\\sdkall\\c\\Lib\\10.0.26100.0\\um\\x64"
CL="$(cygpath -u "$MSVC")/bin/Hostx64/x64/cl.exe"
LINK="$(cygpath -u "$MSVC")/bin/Hostx64/x64/link.exe"
export INCLUDE="$(cygpath -w "$WDK")\\km\\crt;$(cygpath -w "$SDKINC")\\ucrt;$(cygpath -w "$SDKINC")\\um;$(cygpath -w "$SDKINC")\\shared;$(cygpath -w "$MSVC")\\include;$(cygpath -w "$WDK")\\km;$(cygpath -w "$WDK")\\shared"
export LIB="$(cygpath -w "$MSVC")\\lib\\x64;$(cygpath -w "$WDKLIB");$(cygpath -w "$SDKLIB")"
export MSYS_NO_PATHCONV=1
export INCLUDE="$INCLUDE;$(cygpath -w "$PWD")\\Inc;$(cygpath -w "$PWD")\\Main;$(cygpath -w "$PWD")\\Filters;C:\\dev\\tools\\wdk\\c\\Include\\wdf\\kmdf\\1.35;C:\\dev\\tools\\wdk\\c\\Include\\wdf\\umdf\\2.35"
export LIB="$LIB;C:\\dev\\tools\\wdk\\c\\Lib\\wdf\\kmdf\\x64\\1.35"

OUT=$(cygpath -w "$1"); shift
OBJS=""
mkdir -p build
for f in "$@"; do
  case "$f" in
    *.cpp) switch=-Tp ;;
    *)     switch=-Tc ;;
  esac
  obj="build/$(basename "${f%.*}").obj"
  "$CL" -nologo -W3 -WX- -c -Zi -kernel -GS- -D_AMD64_ -DAMD64 -DNTDDI_VERSION=0x0A00000C \
        -D_WIN32_WINNT=0x0A00 -DNDEBUG "-Fo$(cygpath -w "$obj")" "$switch$(cygpath -w "$f")" || exit 1
  OBJS="$OBJS $(cygpath -w "$obj")"
done
"$LINK" -nologo -DRIVER -SUBSYSTEM:NATIVE "-OUT:$OUT" -ENTRY:DriverEntry -INCREMENTAL:NO \
        $OBJS ntoskrnl.lib hal.lib wdm.lib libcntpr.lib \
        portcls.lib stdunk.lib ksguid.lib ks.lib || exit 1
echo "BUILT: $1"
