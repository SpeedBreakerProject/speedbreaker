#!/bin/bash
# CI entry point (GitHub Actions, ubuntu-24.04): the helper scripts want the tree at
# /home/claude/speedbreaker, so link the checkout there, then run rebuild.sh as root.
# usage: win-build/ci-build.sh /path/to/default.xex   ->  dist/SpeedBreaker.exe
set -euo pipefail
# default.xex comes from your own USA (NTSC-U) Xbox 360 disc and is never part of the repository.
# Give its path as the argument, or run this in a terminal and it asks for it.
XEX_IN="${1:-}"
if [ -z "$XEX_IN" ]; then
  [ -t 0 ] || { echo "usage: ci-build.sh /path/to/default.xex" >&2; exit 2; }
  read -r -p "Path to your default.xex: " XEX_IN
fi
XEX_IN="${XEX_IN%\"}"; XEX_IN="${XEX_IN#\"}"
[ -f "$XEX_IN" ] || { echo "No such file: $XEX_IN" >&2; exit 2; }
XEX="$(readlink -f "$XEX_IN")"
WANT=aebdf3c14d1e4e4ed2c130d94bc1a0e97bd6d5bf39bdfdef33c3909c9cc4ba4c
GOT="$(sha256sum "$XEX" | cut -d' ' -f1)"
[ "$GOT" = "$WANT" ] || { echo "default.xex does not match the supported USA (NTSC-U) build (SHA-256 $GOT)." >&2; exit 2; }
SRC="$(cd "$(dirname "$0")/.." && pwd)"
sudo mkdir -p /home/claude
sudo chown "$(id -u):$(id -g)" /home/claude
ln -sfn "$SRC" /home/claude/speedbreaker
export JOBS="${JOBS:-$(nproc)}"
sudo -E bash "$SRC/win-build/rebuild.sh" "$XEX"
mkdir -p "$SRC/dist"
cp /home/claude/SpeedBreaker.exe "$SRC/dist/SpeedBreaker.exe"
sha256sum "$SRC/dist/SpeedBreaker.exe" | sed 's# .*/# #' | tee "$SRC/dist/SpeedBreaker.exe.sha256"
