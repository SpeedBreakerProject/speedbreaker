#!/bin/bash
# Rebuilds SpeedBreaker.exe (Windows x64, cross-compiled on Ubuntu 24.04) from
# this source tree. These are the commands that were run, in order, collected
# into one script. NOTE: it has not been run end-to-end as one script; each
# step was run separately in the original session. Needs ~10 GB free disk,
# several hours on one CPU core (set JOBS=N to use N: the generated-code and FFmpeg
# builds then run N at a time). Paths are absolute (/home/claude) because the
# helper scripts (build_ppc.sh, deps.sh, mingw-toolchain.cmake) use them.
#
# Needs from you: your own USA (NTSC-U) NFS Most Wanted Xbox 360 default.xex
# (3,936,256 bytes, SHA-256 aebdf3c14d1e4e4ed2c130d94bc1a0e97bd6d5bf39bdfdef33c3909c9cc4ba4c)
# passed as $1. It is not included.
set -euo pipefail
XEX="${1:?usage: rebuild.sh /path/to/default.xex}"
W=/home/claude
SRC=$W/speedbreaker
D=$W/deps

# 0. Toolchain (Ubuntu 24.04: clang/lld 18.1.3, cmake 3.28.3, ninja 1.11.1, mingw-w64 gcc 13.2.0 posix)
apt-get update -qq || true
apt-get install -y -qq git curl patch clang lld cmake ninja-build nasm make python3 pkg-config \
  g++-mingw-w64-x86-64-posix gcc-mingw-w64-x86-64-posix mingw-w64-x86-64-dev binutils-mingw-w64-x86-64
update-alternatives --set x86_64-w64-mingw32-g++ /usr/bin/x86_64-w64-mingw32-g++-posix || true
update-alternatives --set x86_64-w64-mingw32-gcc /usr/bin/x86_64-w64-mingw32-gcc-posix || true
# XenonUtils includes "Windows.h" (capital W); mingw's header is lowercase.
ln -sf /usr/x86_64-w64-mingw32/include/windows.h /usr/x86_64-w64-mingw32/include/Windows.h

# 1. Source tree is expected at $SRC (unpacked from the tarball); helper scripts next to it
[ -d $SRC/runtime ] || { echo "unpack the source tarball so that $SRC/runtime exists"; exit 1; }
cp $SRC/win-build/{mingw-toolchain.cmake,deps.sh,build_ppc.sh} $W/
chmod +x $W/deps.sh $W/build_ppc.sh

# 2. XenonRecomp at the pinned commit + the project's patches, built with clang
mkdir -p $SRC/tools && cd $SRC/tools
[ -d XenonRecomp ] || git clone --recursive -q https://github.com/hedge-dev/XenonRecomp.git
cd XenonRecomp
git rev-parse -q --verify nfsmw >/dev/null || {
  git checkout -q -b nfsmw ddd128bcca99fe8bfbb99bea583c972351fa6ace
  git submodule update --init --recursive -q
  git -c user.name=x -c user.email=x@x.invalid am -q $SRC/patches/xenonrecomp/*.patch
}
cmake -Wno-deprecated -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ "-DCMAKE_CXX_FLAGS=-include cstdlib" >/dev/null
ninja -C build XenonRecomp

# 3. Recompile the xex into C++ (writes $SRC/ppc, ~140 MB, ~5M lines, 222 .cpp files; takes seconds)
mkdir -p $SRC/game/files $SRC/ppc
echo "aebdf3c14d1e4e4ed2c130d94bc1a0e97bd6d5bf39bdfdef33c3909c9cc4ba4c  $XEX" | sha256sum -c -
cp "$XEX" $SRC/game/files/default.xex
cd $SRC && tools/XenonRecomp/build/XenonRecomp/XenonRecomp config/nfsmw.toml tools/XenonRecomp/XenonUtils/ppc_context.h

# 4. Cross-compile the generated code (clang --target=x86_64-w64-mingw32 -O2 -march=x86-64-v3); ~30-60 min on one core, JOBS=N divides that
$W/build_ppc.sh
[ -f $W/ppc_fail.log ] && { echo "some ppc files failed:"; cat $W/ppc_fail.log; exit 1; }
x86_64-w64-mingw32-ar rcs $W/libNfsmwRecompLib.a $W/ppcobj/*.o

# 5. Dependencies: download sources, then build (SDL3 static, glslang, FFmpeg XMA decoders, Vulkan import lib)
mkdir -p $D/src && cd $D/src
get() { mkdir -p "$1"; curl -sfL "$2" | tar -xz -C "$1" --strip-components=1; }
get Vulkan-Headers https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.357.0.tar.gz
get SPIRV-Headers  https://github.com/KhronosGroup/SPIRV-Headers/archive/refs/tags/vulkan-sdk-1.4.357.0.tar.gz
get SPIRV-Tools    https://github.com/KhronosGroup/SPIRV-Tools/archive/refs/tags/vulkan-sdk-1.4.357.0.tar.gz
get glslang        https://github.com/KhronosGroup/glslang/archive/refs/tags/16.6.0.tar.gz
get Vulkan-Loader  https://github.com/KhronosGroup/Vulkan-Loader/archive/refs/tags/vulkan-sdk-1.4.357.0.tar.gz
get SDL3           https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-3.4.16.tar.gz
get FFmpeg         https://codeload.github.com/FFmpeg/FFmpeg/tar.gz/refs/tags/n7.1.1
$W/deps.sh

# 6. Configure and build (link uses lld: GNU ld can't resolve the generated code's COFF weak aliases)
cmake -S $SRC -B $W/build -G Ninja -DCMAKE_TOOLCHAIN_FILE=$W/mingw-toolchain.cmake -DCMAKE_BUILD_TYPE=Release \
  -DSB_PPC_PREBUILT=$W/libNfsmwRecompLib.a -DFFMPEG_XMA=$D/ffmpeg-xma \
  -DVulkan_INCLUDE_DIR=$D/install/include -DVulkan_LIBRARY=$D/install/lib/libvulkan-1.a \
  -DCMAKE_PREFIX_PATH=$D/install "-DCMAKE_CXX_FLAGS=-include cstdlib -Wno-everything" "-DCMAKE_C_FLAGS=-Wno-everything"
ninja -C $W/build SpeedBreaker   # the final link of the 45 MB generated-code archive takes several minutes
x86_64-w64-mingw32-strip -o $W/SpeedBreaker.exe $W/build/runtime/SpeedBreaker.exe
echo "built $W/SpeedBreaker.exe"
