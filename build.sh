#!/bin/sh
# Build the mod into dist/. Needs devkitA64 + libnx (devkitPro); set DEVKITPRO if it is not /opt/devkitpro.
# Make cannot handle spaces in paths, so the build runs in a space-free copy with a space-free DEVKITPRO link.
set -e
P="$(dirname "$(readlink -f "$0")")"
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
W="${XDG_RUNTIME_DIR:-/tmp}/b3-dynfps-build"; B="$W/src"   # fixed path: source paths end up in the binary, so this keeps builds byte-identical
rm -rf "$W"; mkdir -p "$B"; ln -s "$DEVKITPRO" "$W/devkitpro"; export DEVKITPRO="$W/devkitpro"
cp -r "$P/source" "$P/misc" "$P/Makefile" "$P/config.mk" "$P/config.json" "$B/"
make -C "$B" -j"$(nproc)"
D="$P/dist/Dynamic FPS/exefs"
rm -rf "$P/dist"; mkdir -p "$D"
cp "$B/deploy/subsdk9" "$B/deploy/main.npdm" "$D/"
ls -l "$D"
