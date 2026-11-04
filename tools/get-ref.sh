#!/bin/bash
# Fetch the read-only reference trees that docs/ refers to as ref/.
# They are deliberately NOT in the repository (license + size) and are never
# compiled by build.sh or driver/kbuild.sh — the driver sources are already
# vendored under driver/emuusbaudio.
#
#   tools/get-ref.sh samples     # microsoft/Windows-driver-samples (sparse)
#   tools/get-ref.sh emu-driver  # E-MU CA0189 macOS driver source (manual)
#   tools/get-ref.sh check       # report what is present
#
# SPDX-License-Identifier: LGPL-2.1-or-later
set -euo pipefail
cd "$(dirname "$0")/.."
REF=ref
SAMPLES_URL=${OED_SAMPLES_URL:-https://github.com/microsoft/Windows-driver-samples.git}
# The subtree this project actually consults: the PortCLS/WaveRT sample driver.
SAMPLES_PATHS=(audio/simpleaudiosample)
mkdir -p "$REF"

case "${1:-check}" in
    samples)
        target="$REF/Windows-driver-samples"
        if [ -d "$target/.git" ]; then
            echo "already cloned: $target"
        else
            git clone --filter=tree:0 --depth 1 --no-checkout \
                --sparse "$SAMPLES_URL" "$target"
        fi
        (cd "$target" && git sparse-checkout set "${SAMPLES_PATHS[@]}" && git checkout)
        echo "commit: $(cd "$target" && git rev-parse HEAD)"
        echo "minwavert reference: $target/audio/simpleaudiosample/Source"
        ;;

    emu-driver)
        cat <<'EOF'
The E-MU "CA0189 USB Audio Driver" for macOS (E-MU Systems / Creative, 2008,
LGPLv2.1+) is the wire-protocol source behind docs/PROTOCOL.md. Creative's
download pages no longer serve it, so there is no URL to automate here.

Place a copy at   ref/EMU-driver/src/EMUUSBAudio
Keeping the tree out of git means docs/PROTOCOL.md cites it by version only.
Every XU selector and request format in that source has been cross-checked
against the 1.30.07 Windows binaries — see docs/RE_NOTES.md — so the protocol
document is usable without this reference tree.
EOF
        ;;

    check)
        for e in "$REF/Windows-driver-samples/audio/simpleaudiosample" \
                 "$REF/EMU-driver/src/EMUUSBAudio"; do
            if [ -d "$e" ]; then echo "present:  $e"; else echo "missing:  $e  (tools/get-ref.sh ${e#"$REF/"})"; fi
        done
        ;;

    *)  echo "usage: get-ref.sh {samples|emu-driver|check}" >&2; exit 2 ;;
esac
