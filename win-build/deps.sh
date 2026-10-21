#!/bin/bash
set -x
S=/home/claude/deps/src; B=/home/claude/deps/build; P=/home/claude/deps/install
TC="-DCMAKE_TOOLCHAIN_FILE=/home/claude/mingw-toolchain.cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$P"
mkdir -p $B $P
# Vulkan headers
[ -f $P/include/vulkan/vulkan.h ] || { cmake -S $S/Vulkan-Headers -B $B/vkh $TC -G Ninja && cmake --install $B/vkh; } || exit 1
# Vulkan import library
if [ ! -f $P/lib/libvulkan-1.a ]; then
  mkdir -p $P/lib
  x86_64-w64-mingw32-dlltool -d $S/Vulkan-Loader/loader/vulkan-1.def -l $P/lib/libvulkan-1.a -D vulkan-1.dll || exit 1
fi
# SDL3
[ -f $P/lib/libSDL3.a ] || { cmake -S $S/SDL3 -B $B/sdl $TC -G Ninja -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_INSTALL_TESTS=OFF -DSDL_VULKAN=ON && ninja -C $B/sdl && cmake --install $B/sdl; } || { echo SDL_FAIL > /home/claude/deps/FAIL; exit 1; }
echo SDL_OK > /home/claude/deps/sdl_ok
# glslang (no SPIRV-Tools optimizer)
[ -f $P/lib/libglslang.a ] || { cmake -S $S/glslang -B $B/gl $TC -G Ninja -DBUILD_SHARED_LIBS=OFF -DENABLE_OPT=OFF -DENABLE_GLSLANG_BINARIES=OFF -DGLSLANG_TESTS=OFF -DENABLE_SPVREMAPPER=OFF -DENABLE_HLSL=OFF && ninja -C $B/gl && cmake --install $B/gl; } || { echo GLSLANG_FAIL > /home/claude/deps/FAIL; exit 1; }
echo GL_OK > /home/claude/deps/gl_ok
# FFmpeg XMA
if [ ! -f /home/claude/deps/ffmpeg-xma/lib/libavcodec.a ]; then
  cd $S/FFmpeg && for p in /home/claude/speedbreaker/patches/ffmpeg/*.patch; do patch -s -p1 < $p; done
  ./configure --disable-everything --disable-programs --disable-doc --disable-debug --disable-network --disable-autodetect --disable-avdevice --disable-avformat --disable-swscale --disable-swresample --disable-postproc --disable-avfilter --enable-decoder=xma1,xma2,wmapro --enable-static --disable-shared --enable-cross-compile --target-os=mingw32 --arch=x86_64 --cross-prefix=x86_64-w64-mingw32- --cc="clang --target=x86_64-w64-mingw32" --prefix=/home/claude/deps/ffmpeg-xma --disable-x86asm > /home/claude/deps/ffconf.log 2>&1 || { echo FFCONF_FAIL > /home/claude/deps/FAIL; exit 1; }
  make -j"${JOBS:-1}" && make install || { echo FF_FAIL > /home/claude/deps/FAIL; exit 1; }
fi
echo ALL_OK > /home/claude/deps/all_ok
