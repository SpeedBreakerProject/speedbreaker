#!/bin/bash
# SpeedBreaker. GPL-3.0-or-later (see COPYING).
#
# scripts/ios/build_deps.sh <deps folder>: the libraries scripts/ios/build.sh
# links, built for iPhone and iPad (arm64, iOS 17 or later). The folder is
# build.sh's IOS_DEPS afterwards:
#   prefix/     SDL3 3.4.16, glslang 16.6.0 (without its optimizer) and
#               FFmpeg-XMA (FFmpeg 7.1.1's XMA decoders with patches/ffmpeg,
#               scripts/build_ffmpeg_xma.sh), all static
#   MoltenVK/   MoltenVK 1.4.2's static xcframework (its release's
#               MoltenVK-ios.tar; no Vulkan loader on iOS)
# The sources go to <deps folder>/downloads once (about 65 MB), and each is
# checked against its SHA-256 below. FFmpeg's comes from tools/ffmpeg-src when
# scripts/setup_tools.sh has fetched it already. Needs Xcode, CMake, Ninja and
# curl; a few minutes. Run it again to rebuild everything.
set -euo pipefail
[ $# -eq 1 ] || { echo "usage: $0 <deps folder>" >&2; exit 2; }
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
export DEVELOPER_DIR=${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}
mkdir -p "$1"
DEPS=$(cd "$1" && pwd)
DL=$DEPS/downloads
WORK=$DEPS/work
PREFIX=$DEPS/prefix
IOS_MIN=17.0
JOBS=${JOBS:-$(sysctl -n hw.ncpu)}
SDK=$(xcrun -sdk iphoneos --show-sdk-path) || { echo "build_deps.sh: no iOS SDK (install Xcode, or set DEVELOPER_DIR)" >&2; exit 1; }

SDL3=SDL3-3.4.16.tar.gz
SDL3_URL=https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/$SDL3
SDL3_SHA256=7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68
GLSLANG=glslang-16.6.0.tar.gz
GLSLANG_URL=https://github.com/KhronosGroup/glslang/archive/refs/tags/16.6.0.tar.gz
GLSLANG_SHA256=9c09b901149c729df745057dafa815278aaa101b84d2b6e14f16a42de52f97f2
MOLTENVK=MoltenVK-ios.tar
MOLTENVK_URL=https://github.com/KhronosGroup/MoltenVK/releases/download/v1.4.2/$MOLTENVK
MOLTENVK_SHA256=b5d947b1660e6e9fed40b9cd2387e160aaab9e80b775c0cef7e14059405178c1
FFMPEG=ffmpeg-7.1.1.tar.xz
FFMPEG_URL=https://ffmpeg.org/releases/$FFMPEG
FFMPEG_SHA256=733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1

# fetch <file> <url> <sha256>: into downloads/ unless it's there already, then checked.
fetch() {
    local file=$1 url=$2 sum=$3
    if [ ! -f "$DL/$file" ]; then
        curl -fL --retry 3 -o "$DL/$file.part" "$url"
        mv "$DL/$file.part" "$DL/$file"
    fi
    echo "$sum  $DL/$file" | shasum -a 256 -c - > /dev/null ||
        { echo "build_deps.sh: $DL/$file isn't the expected file (SHA-256); delete it and run again" >&2; exit 1; }
}
mkdir -p "$DL"
[ -f "$DL/$FFMPEG" ] || [ ! -f "$ROOT/tools/ffmpeg-src/$FFMPEG" ] || cp "$ROOT/tools/ffmpeg-src/$FFMPEG" "$DL/"
fetch "$SDL3" "$SDL3_URL" "$SDL3_SHA256"
fetch "$GLSLANG" "$GLSLANG_URL" "$GLSLANG_SHA256"
fetch "$MOLTENVK" "$MOLTENVK_URL" "$MOLTENVK_SHA256"
fetch "$FFMPEG" "$FFMPEG_URL" "$FFMPEG_SHA256"

rm -rf "$WORK" "$PREFIX" "$DEPS/MoltenVK"
mkdir -p "$WORK" "$PREFIX"
trap 'echo "build_deps.sh: failed; the logs are in $WORK" >&2' ERR

# CMake projects: a static Release build for iOS (device, arm64) installed into prefix/.
ios_cmake() {  # ios_cmake <source> <build> [options...]
    local src=$1 build=$2; shift 2
    cmake -S "$src" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=$IOS_MIN \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" -DBUILD_SHARED_LIBS=OFF "$@" > "$build.configure.log" 2>&1
    ninja -C "$build" -j"$JOBS" install > "$build.build.log" 2>&1
}

echo "SDL3 3.4.16"
tar -xzf "$DL/$SDL3" -C "$WORK"
ios_cmake "$WORK/SDL3-3.4.16" "$WORK/sdl3" -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF

echo "glslang 16.6.0"
tar -xzf "$DL/$GLSLANG" -C "$WORK"
ios_cmake "$WORK/glslang-16.6.0" "$WORK/glslang" -DENABLE_OPT=OFF -DGLSLANG_TESTS=OFF \
    -DENABLE_GLSLANG_BINARIES=OFF -DGLSLANG_ENABLE_INSTALL=ON

echo "FFmpeg-XMA 7.1.1"
"$ROOT/scripts/build_ffmpeg_xma.sh" "$DL/$FFMPEG" "$WORK/ffmpeg-xma" \
    --enable-cross-compile --target-os=darwin --arch=aarch64 --cc="xcrun -sdk iphoneos clang" --sysroot="$SDK" \
    --extra-cflags="-arch arm64 -miphoneos-version-min=$IOS_MIN" --extra-ldflags="-arch arm64 -miphoneos-version-min=$IOS_MIN"
cp -R "$WORK/ffmpeg-xma/include/." "$PREFIX/include/"
cp "$WORK/ffmpeg-xma/lib/libavcodec.a" "$WORK/ffmpeg-xma/lib/libavutil.a" "$PREFIX/lib/"

echo "MoltenVK 1.4.2"
tar -xf "$DL/$MOLTENVK" -C "$DEPS"
[ -f "$DEPS/MoltenVK/MoltenVK/static/MoltenVK.xcframework/ios-arm64/libMoltenVK.a" ] ||
    { echo "build_deps.sh: $MOLTENVK has no static iOS MoltenVK" >&2; exit 1; }

# Every library is for iOS devices.
for lib in "$PREFIX"/lib/*.a "$DEPS/MoltenVK/MoltenVK/static/MoltenVK.xcframework/ios-arm64/libMoltenVK.a"; do
    platform=$(otool -l "$lib" | awk '/LC_BUILD_VERSION/{f=1} f && $1=="platform" && !p{print $2; p=1}')
    [ "$platform" = 2 ] || { echo "build_deps.sh: $lib isn't built for iOS (platform ${platform:-?})" >&2; exit 1; }
done
rm -rf "$WORK"
echo "done: IOS_DEPS=$DEPS ($(du -sh "$DEPS/prefix" | cut -f1) prefix, sources kept in downloads/)"
