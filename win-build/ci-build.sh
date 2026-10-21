#!/bin/bash
# CI entry point (GitHub Actions, ubuntu-24.04): the helper scripts want the tree at
# /home/claude/speedbreaker, so link the checkout there, then run rebuild.sh as root.
# usage: win-build/ci-build.sh /path/to/default.xex   ->  dist/SpeedBreaker.exe
set -euo pipefail
XEX="$(readlink -f "${1:?usage: ci-build.sh /path/to/default.xex}")"
SRC="$(cd "$(dirname "$0")/.." && pwd)"
sudo mkdir -p /home/claude
sudo chown "$(id -u):$(id -g)" /home/claude
ln -sfn "$SRC" /home/claude/speedbreaker
export JOBS="${JOBS:-$(nproc)}"
sudo -E bash "$SRC/win-build/rebuild.sh" "$XEX"
mkdir -p "$SRC/dist"
cp /home/claude/SpeedBreaker.exe "$SRC/dist/SpeedBreaker.exe"
sha256sum "$SRC/dist/SpeedBreaker.exe" | sed 's# .*/# #' | tee "$SRC/dist/SpeedBreaker.exe.sha256"
