#!/usr/bin/env bash
# Rebuild the toolchain from scratch into tools/ (git-ignored).
#
#   scripts/setup_tools.sh
#
# - XenonRecomp at a pinned upstream commit + patches/xenonrecomp/*.patch
#   (branch "nfsmw"), built with Clang.
# - extract-xiso (to unpack the disc image).
# - Xenia's PPC instruction tests for the instructions we added.
#
# Needs: git, cmake, ninja, clang 18+ (Homebrew LLVM on macOS, the system
# clang elsewhere; override with LLVM=<bin dir>), lld, make, curl; gh for the
# instruction tests (skipped without it, or when it isn't logged in).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TOOLS="$ROOT/tools"
if [ -z "${LLVM:-}" ]; then
  if [ -x /opt/homebrew/opt/llvm/bin/clang ]; then LLVM=/opt/homebrew/opt/llvm/bin
  else LLVM="$(dirname "$(command -v clang)")"; fi
fi
sha256() { if command -v sha256sum >/dev/null; then sha256sum -c -; else shasum -a 256 -c -; fi; }
XENONRECOMP_BASE=ddd128bcca99fe8bfbb99bea583c972351fa6ace   # hedge-dev/XenonRecomp main, 2025

mkdir -p "$TOOLS"

if [ ! -d "$TOOLS/XenonRecomp" ]; then
  git clone --recursive https://github.com/hedge-dev/XenonRecomp.git "$TOOLS/XenonRecomp"
fi
cd "$TOOLS/XenonRecomp"
if ! git rev-parse -q --verify nfsmw >/dev/null; then
  git checkout -q -b nfsmw "$XENONRECOMP_BASE"
  git submodule update --init --recursive
  # The patches carry their own authors; the committer needs any name (a fresh
  # machine or a CI runner has none configured).
  git -c user.name="SpeedBreaker setup" -c user.email=setup@speedbreaker.invalid \
    am -q "$ROOT"/patches/xenonrecomp/*.patch
fi
# The bundled fmt predates libc++ dropping transitive <cstdlib>.
cmake -Wno-deprecated -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$LLVM/clang" -DCMAKE_CXX_COMPILER="$LLVM/clang++" \
  "-DCMAKE_CXX_FLAGS=-include cstdlib" > /dev/null
ninja -C build XenonRecomp XenonAnalyse

if [ ! -x "$TOOLS/extract-xiso/build/extract-xiso" ]; then
  [ -d "$TOOLS/extract-xiso" ] || git clone -q --depth 1 https://github.com/XboxDev/extract-xiso.git "$TOOLS/extract-xiso"
  cmake -Wno-deprecated -S "$TOOLS/extract-xiso" -B "$TOOLS/extract-xiso/build" -G Ninja -DCMAKE_BUILD_TYPE=Release > /dev/null
  ninja -C "$TOOLS/extract-xiso/build"
fi

# FFmpeg for XMA audio: a minimal static libavcodec (XMA1/XMA2/WMAPro
# decoders only) with patches/ffmpeg applied (scripts/build_ffmpeg_xma.sh).
FFMPEG_VERSION=7.1.1
FFMPEG_SHA256=733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1
if [ ! -f "$TOOLS/ffmpeg-xma/lib/libavcodec.a" ]; then
  mkdir -p "$TOOLS/ffmpeg-src"
  cd "$TOOLS/ffmpeg-src"
  [ -f "ffmpeg-$FFMPEG_VERSION.tar.xz" ] || curl -sfLO "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz"
  echo "$FFMPEG_SHA256  ffmpeg-$FFMPEG_VERSION.tar.xz" | sha256
  "$ROOT/scripts/build_ffmpeg_xma.sh" "$TOOLS/ffmpeg-src/ffmpeg-$FFMPEG_VERSION.tar.xz" "$TOOLS/ffmpeg-xma"
  # The license texts, for the SteamOS bundle's licenses/ (make_bundle.sh).
  tar -xf "ffmpeg-$FFMPEG_VERSION.tar.xz" "ffmpeg-$FFMPEG_VERSION/COPYING.LGPLv2.1" "ffmpeg-$FFMPEG_VERSION/LICENSE.md"
  cd "$ROOT"
fi

mkdir -p "$TOOLS/xenia-tests"
missing=()
for t in vslw vand vsel mulhw vslh vsrah vsubshs vspltish vmaxsh vandc vctuxs vpkswss \
         vminsh vavguh vpkswus vsrh vrlh vpkshss vpkuhus mulhd mulhdu; do
  [ -s "$TOOLS/xenia-tests/instr_$t.s" ] || missing+=("$t")
done
# They come through gh. Without it, or logged out, everything else is ready:
# only tests/run_instr_tests.sh lacks its inputs, so that isn't a failure.
if [ ${#missing[@]} -gt 0 ]; then
  if ! command -v gh >/dev/null; then
    echo "gh not found: skipping Xenia's instruction tests (only tests/run_instr_tests.sh needs them)"; missing=()
  elif ! gh auth status >/dev/null 2>&1; then
    echo "gh isn't logged in (gh auth login): skipping Xenia's instruction tests (only tests/run_instr_tests.sh needs them)"; missing=()
  fi
fi
for t in ${missing[@]+"${missing[@]}"}; do
  f="$TOOLS/xenia-tests/instr_$t.s"
  # Into a temporary file first: a failed fetch must not leave an empty test
  # that later runs would skip as already fetched.
  if gh api "repos/xenia-project/xenia/contents/src/xenia/cpu/ppc/testing/instr_$t.s" \
       -H "Accept: application/vnd.github.raw" > "$f.part" && [ -s "$f.part" ]; then
    mv "$f.part" "$f"
  else
    rm -f "$f.part"; echo "couldn't fetch Xenia's instr_$t.s"; exit 1
  fi
done

echo "tools ready: $(git -C "$TOOLS/XenonRecomp" log --oneline -1)"
