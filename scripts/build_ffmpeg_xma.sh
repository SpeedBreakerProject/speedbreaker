#!/usr/bin/env bash
# SpeedBreaker. GPL-3.0-or-later (see COPYING).
#
#   scripts/build_ffmpeg_xma.sh <ffmpeg-7.1.1.tar.xz> <install prefix> [extra configure options...]
#
# FFmpeg for XMA audio: a minimal static libavcodec and libavutil (the
# XMA1/XMA2/WMA Pro decoders only, LGPL-2.1-or-later: no GPL parts) with
# patches/ffmpeg applied, installed into <install prefix>/{include,lib}.
# scripts/setup_tools.sh calls it for tools/ffmpeg-xma; extra options are
# for a cross build (--enable-cross-compile --target-os=... --cc=...).
#
# Nothing of the machine it was built on ends up in the libraries, so none
# reaches a published game binary:
#   - no debug information (--disable-debug: it named every source file by
#     its absolute path);
#   - configure records its command line in the libraries (avutil and
#     avcodec_configuration()): it runs with the default prefix and the
#     result is installed with DESTDIR, so no folder of ours is on it.
set -euo pipefail
[ $# -ge 2 ] || { echo "usage: $0 <ffmpeg-<version>.tar.xz> <install prefix> [configure options...]" >&2; exit 2; }
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TARBALL="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
PREFIX="$2"
shift 2
mkdir -p "$PREFIX"
PREFIX="$(cd "$PREFIX" && pwd)"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/ffmpeg-xma.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

tar -xf "$TARBALL" -C "$WORK"
SRC="$(find "$WORK" -mindepth 1 -maxdepth 1 -type d -name 'ffmpeg-*' | head -1)"
[ -n "$SRC" ] || { echo "build_ffmpeg_xma.sh: no ffmpeg-* folder in $TARBALL" >&2; exit 1; }
cd "$SRC"
for p in "$ROOT"/patches/ffmpeg/*.patch; do patch -s -p1 < "$p"; done
./configure --disable-everything --disable-programs --disable-doc --disable-debug \
    --disable-network --disable-autodetect --disable-avdevice --disable-avformat --disable-swscale \
    --disable-swresample --disable-postproc --disable-avfilter \
    --enable-decoder=xma1,xma2,wmapro --enable-static --disable-shared --enable-pic "$@" > "$WORK/configure.log"
make -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)" > "$WORK/make.log"
make install DESTDIR="$WORK/stage" > "$WORK/install.log"
# The default prefix (/usr/local) inside the stage, moved to its place.
rm -rf "$PREFIX/include" "$PREFIX/lib"
mv "$WORK/stage/usr/local/include" "$WORK/stage/usr/local/lib" "$PREFIX/"
# The check: no folder of this machine in the libraries.
for lib in "$PREFIX"/lib/libavcodec.a "$PREFIX"/lib/libavutil.a; do
    if strings -a "$lib" | grep -qF -e "$WORK" -e "$ROOT" -e "$PREFIX" ${HOME:+-e "$HOME"}; then
        echo "build_ffmpeg_xma.sh: $lib names a folder of this machine" >&2
        exit 1
    fi
done
echo "FFmpeg-XMA $(basename "$SRC") in $PREFIX"
