#!/usr/bin/env bash
# SpeedBreaker. GPL-3.0-or-later (see COPYING).
#
#   scripts/macos/build_deps.sh <prefix> [<downloads folder>]
#
# The libraries the Mac app bundles (scripts/macos/make_app.py), built from
# their official sources for the oldest macOS the game runs on, arm64, into
# <prefix>. That macOS is the top-level CMakeLists.txt's deployment target
# (MACOSX_DEPLOYMENT_TARGET in the environment overrides it). Homebrew's
# bottles can't be bundled for it: a bottle is built for the macOS it was made
# on (26.0 for macOS 26), and macOS refuses to load a library built for a
# newer macOS than its own.
#
#   Vulkan-Headers 1.4.357.0, SPIRV-Headers 1.4.357.0   headers (the game, the
#                                                       loader, SPIRV-Tools)
#   SPIRV-Tools 1.4.357.0, glslang 16.6.0               shared libraries
#   Vulkan-Loader 1.4.357.0                             libvulkan.1.dylib
#   SDL3 3.4.16                                         libSDL3.0.dylib
#   FFmpeg 7.1.1 (scripts/build_ffmpeg_xma.sh)          <prefix>/ffmpeg-xma,
#                                                       static, in the game
#
# The same versions v0.1.0 bundled from Homebrew, built the way Homebrew
# builds them, so they run exactly as fast: its formulas' CMake options,
# -Os -DNDEBUG, Apple's clang from the command line tools, and no -mcpu or
# -march (the compiler's default for arm64 Macs, apple-m1, which the
# deployment target doesn't change). -Os, not CMake's Release default -O3:
# Homebrew's bottles are -Os. Measured (2026-10-08, command line tools 26.6,
# the clang Homebrew's bottles were built with): this script with -Os and
# target 26.0 reproduces Homebrew's libSDL3.0.dylib byte for byte, and the
# loader's and SPIRV-Tools' code to the instruction (only build dates and
# the loader's search folders differ); with -O3, SDL's code is 29% larger.
# What differs from Homebrew's:
#   - the deployment target;
#   - the Vulkan loader's search folders are upstream's defaults
#     (/usr/local/etc, /etc, /etc/xdg, /usr/local/share, /usr/share), not
#     Homebrew's own (/opt/homebrew/etc and /opt/homebrew/share before
#     those). The app points the loader at its own MoltenVK either way;
#   - the command-line tools of SPIRV-Tools and glslang aren't built (the
#     game uses only the libraries).
#
# Each source archive is checked against its SHA-256 (the archives Homebrew's
# formulas pin, from the projects' own GitHub releases and tags; FFmpeg's from
# ffmpeg.org, the one scripts/setup_tools.sh pins) and kept in the downloads
# folder (default <prefix>/downloads). An archive that's already there, or in
# tools/ffmpeg-src for FFmpeg, is used without downloading it. Each project's
# license files go to <prefix>/share/licenses/<project>/, and
# <prefix>/share/speedbreaker-deps.json records what was built, from what and
# how; make_app.py copies both into the app. Nothing of the machine it's built
# on ends up in the libraries (the sources are mapped to "." as Homebrew does),
# and nothing of when or where: SOURCE_DATE_EPOCH is each source's newest file,
# as Homebrew sets it (SPIRV-Tools' version string carries it), and each library
# is linked with the rpath it is installed with (CMAKE_BUILD_WITH_INSTALL_RPATH),
# never one naming the work folder or the prefix: the linker lays the code out
# after the load commands and hashes the file into its LC_UUID, and CMake's
# install step only deletes such an rpath afterwards, so its length moved
# libglslang's code and it changed libSPIRV-Tools-opt's UUID on every build.
# So a rebuild gives the same libraries, byte for byte (every .dylib, and
# FFmpeg-XMA's archives), whatever the prefix's path and the (temporary) work
# folder's; what differs is only what names the prefix (the .pc files) and
# the dates in libSDL3_test.a, neither in the app. Every compiler is the one
# recorded: Apple's clang from xcrun (FFmpeg's configure too, which would
# otherwise take the first `gcc` on PATH), with the cmake and ninja versions
# next to it.
#
# Nothing newer than the target is used unchecked: a library that calls a macOS
# API newer than the target without an @available check compiles with only a
# warning (clang's -Wunguarded-availability-new, on by default) and links it
# weakly, and on the older macOS that call is to null (an Objective-C method
# isn't even an import, so scripts/macos/check_app.py can't see it). Each
# project's build fails here on any such warning in its log, and on any
# compile command that silences them; a control compile first checks the
# warning's wording is still the one looked for. Not -Werror in the flags: that
# would also reach CMake's configure checks, and a feature test failing on it
# would quietly turn a feature off. MoltenVK, the one library the app takes
# from Homebrew (built for macOS 12.0), is not built here, so not checked
# here: its calls to newer Metal APIs rest on its own runtime checks.
#
# Then build the game against it (the README's cmake line, plus two options):
#   cmake -S . -B build/main -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm/bin/clang \
#         -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ \
#         -DCMAKE_PREFIX_PATH=<prefix> -DFFMPEG_XMA=<prefix>/ffmpeg-xma
#   ninja -C build/main SpeedBreaker && scripts/macos/make_app.py --dmg
#
# Needs: cmake 3.23+, ninja, curl, python3 and Apple's command line tools. No
# Homebrew library is used: Homebrew's prefixes are kept out of every search.
# A <prefix> made by this script before is rebuilt (its downloads stay);
# any other non-empty folder is refused.
set -euo pipefail
[ $# -ge 1 ] || { echo "usage: $0 <prefix> [<downloads folder>]" >&2; exit 2; }
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
mkdir -p "$1"
PREFIX="$(cd "$1" && pwd -P)"
DOWNLOADS="${2:-$PREFIX/downloads}"
mkdir -p "$DOWNLOADS"
DOWNLOADS="$(cd "$DOWNLOADS" && pwd -P)"

TARGET="${MACOSX_DEPLOYMENT_TARGET:-$(sed -n 's/^set(SB_OSX_DEFAULT_TARGET "\([0-9.]*\)").*/\1/p' "$ROOT/CMakeLists.txt" | head -1)}"
[[ "$TARGET" =~ ^[0-9]+(\.[0-9]+)*$ ]] || { echo "build_deps.sh: no deployment target in $ROOT/CMakeLists.txt" >&2; exit 1; }
export MACOSX_DEPLOYMENT_TARGET="$TARGET"
CC="$(xcrun --find clang)"
CXX="$(xcrun --find clang++)"
SDK="$(xcrun --sdk macosx --show-sdk-path)"
JOBS="$(sysctl -n hw.ncpu)"
OPT="-Os -DNDEBUG"   # Homebrew's optimisation (see the top)

# A prefix of ours (its marker says so, even from a run that failed) is
# emptied but for its downloads; anything else must be empty.
MARKER="$PREFIX/.speedbreaker-deps"
if [ -f "$MARKER" ]; then
  find "$PREFIX" -mindepth 1 -maxdepth 1 ! -path "$DOWNLOADS" ! -name downloads -exec rm -rf {} +
elif [ -n "$(find "$PREFIX" -mindepth 1 -maxdepth 1 ! -path "$DOWNLOADS" ! -name downloads | head -1)" ]; then
  echo "build_deps.sh: $PREFIX isn't empty and wasn't made by this script" >&2
  exit 1
fi
echo "made by SpeedBreaker's scripts/macos/build_deps.sh (it may empty this folder to rebuild it)" > "$MARKER"
LOGS="$PREFIX/build-logs"
mkdir -p "$LOGS" "$PREFIX/share/licenses"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/sb-deps.XXXXXX")"
WORK="$(cd "$WORK" && pwd -P)"
trap 'rm -rf "$WORK"' EXIT

# name | version | URL | SHA-256 | archive name
SOURCES=(
  "Vulkan-Headers|1.4.357.0|https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.357.0.tar.gz|e87dce08116151f6b6d7de6b6faf41498e87e6cf848ff16fa3bd5402190ad4a3|Vulkan-Headers-vulkan-sdk-1.4.357.0.tar.gz"
  "SPIRV-Headers|1.4.357.0|https://github.com/KhronosGroup/SPIRV-Headers/archive/refs/tags/vulkan-sdk-1.4.357.0.tar.gz|4d703067a7e06331ccb37bdfed3f9b7879cc61969a2689ae95c95db34a47ff07|SPIRV-Headers-vulkan-sdk-1.4.357.0.tar.gz"
  "SPIRV-Tools|1.4.357.0|https://github.com/KhronosGroup/SPIRV-Tools/archive/refs/tags/vulkan-sdk-1.4.357.0.tar.gz|d31e7109b6ef3559067e53e520870eafed7c9534d00db9728814b6df03fa4a5e|SPIRV-Tools-vulkan-sdk-1.4.357.0.tar.gz"
  "glslang|16.6.0|https://github.com/KhronosGroup/glslang/archive/refs/tags/16.6.0.tar.gz|9c09b901149c729df745057dafa815278aaa101b84d2b6e14f16a42de52f97f2|glslang-16.6.0.tar.gz"
  "Vulkan-Loader|1.4.357.0|https://github.com/KhronosGroup/Vulkan-Loader/archive/refs/tags/vulkan-sdk-1.4.357.0.tar.gz|54f2537df22313768da0317dda2abdaaab7711b4081c48c869a79db343d0ae70|Vulkan-Loader-vulkan-sdk-1.4.357.0.tar.gz"
  "SDL3|3.4.16|https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-3.4.16.tar.gz|7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68|SDL3-3.4.16.tar.gz"
  "FFmpeg|7.1.1|https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz|733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1|ffmpeg-7.1.1.tar.xz"
)
field() { local IFS='|'; local a=($1); echo "${a[$2]}"; }
source_of() { local s; for s in "${SOURCES[@]}"; do [ "$(field "$s" 0)" = "$1" ] && { echo "$s"; return; }; done; return 1; }

# fetch <name>: the archive's path, downloaded (into a temporary file first)
# unless it's already in the downloads folder; checked either way.
fetch() {
  local s url sum file
  s="$(source_of "$1")"; url="$(field "$s" 2)"; sum="$(field "$s" 3)"; file="$DOWNLOADS/$(field "$s" 4)"
  if [ ! -f "$file" ] && [ "$1" = FFmpeg ] && [ -f "$ROOT/tools/ffmpeg-src/$(field "$s" 4)" ]; then
    cp "$ROOT/tools/ffmpeg-src/$(field "$s" 4)" "$file.part" && mv "$file.part" "$file"
  fi
  if [ ! -f "$file" ]; then
    echo "downloading $url" >&2
    # (errexit doesn't reach inside $(fetch): check here, and never keep a
    # partial download under the archive's name)
    if ! curl -fsSL -o "$file.part" "$url"; then
      rm -f "$file.part"; echo "build_deps.sh: couldn't download $url" >&2; exit 1
    fi
    mv "$file.part" "$file"
  fi
  if ! echo "$sum  $file" | shasum -a 256 -c - > /dev/null; then
    echo "build_deps.sh: $file isn't the expected archive (SHA-256 $sum)" >&2
    exit 1
  fi
  echo "$file"
}

# unpack <name>: the project's source folder (the archive's one top-level
# folder) in the work folder. Its newest file's time is kept in
# <folder>.epoch, before the build writes anything into it.
unpack() {
  local archive dest="$WORK/src/$1"
  archive="$(fetch "$1")" || exit 1
  mkdir -p "$dest"
  tar -xf "$archive" -C "$dest"
  set -- "$dest"/*
  [ $# -eq 1 ] && [ -d "$1" ] || { echo "build_deps.sh: $archive doesn't hold one folder" >&2; exit 1; }
  find "$1" -type f -exec stat -f %m {} + | sort -n | tail -1 > "$1.epoch"
  [ -s "$1.epoch" ] || { echo "build_deps.sh: no files in $archive" >&2; exit 1; }
  echo "$1"
}

# licenses <name> <source folder> <file...>: the project's license files,
# kept at their paths in the source (each must exist).
licenses() {
  local name=$1 src=$2 f
  shift 2
  for f in "$@"; do
    [ -f "$src/$f" ] || { echo "build_deps.sh: $name has no $f" >&2; exit 1; }
    mkdir -p "$PREFIX/share/licenses/$name/$(dirname "$f")"
    cp "$src/$f" "$PREFIX/share/licenses/$name/$f"
  done
}

# cmake_project <name> <source folder> [cmake options...]: configure, build
# and install, as Homebrew's formulas do (cmake -S . -B build, with its
# standard options), with this build's target and the sources mapped to ".".
cmake_project() {
  local name=$1 src=$2
  shift 2
  local map="-ffile-prefix-map=$src=."
  local log="$LOGS/$name.log"
  echo "== $name" >&2
  (cd "$src" && export SOURCE_DATE_EPOCH="$(cat "$src.epoch")" && cmake -S . -B build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_INSTALL_LIBDIR=lib \
      -DCMAKE_FIND_FRAMEWORK=LAST -DBUILD_TESTING=OFF -Wno-dev \
      -DCMAKE_OSX_DEPLOYMENT_TARGET="$TARGET" -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_SYSROOT="$SDK" \
      -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" -DCMAKE_OBJC_COMPILER="$CC" \
      "-DCMAKE_C_FLAGS=$map" "-DCMAKE_CXX_FLAGS=$map" "-DCMAKE_OBJC_FLAGS=$map" "-DCMAKE_ASM_FLAGS=$map" \
      "-DCMAKE_C_FLAGS_RELEASE=$OPT" "-DCMAKE_CXX_FLAGS_RELEASE=$OPT" "-DCMAKE_OBJC_FLAGS_RELEASE=$OPT" \
      -DCMAKE_SHARED_LINKER_FLAGS=-Wl,-headerpad_max_install_names \
      -DCMAKE_MODULE_LINKER_FLAGS=-Wl,-headerpad_max_install_names \
      -DCMAKE_EXE_LINKER_FLAGS=-Wl,-headerpad_max_install_names \
      -DCMAKE_PREFIX_PATH="$PREFIX" "-DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew;/usr/local" \
      -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
      "$@" && cmake --build build -j "$JOBS" && cmake --install build) > "$log" 2>&1 \
    || { echo "build_deps.sh: $name failed; see $log" >&2; tail -20 "$log" >&2; exit 1; }
  cp "$src/build/install_manifest.txt" "$LOGS/$name.installed.txt"
  availability "$name" "$log" "$src/build/compile_commands.json"
}

# availability <name> <log> [compile_commands.json]: the project's build used
# nothing newer than the target unchecked (see the top): no availability
# warning in its log, and no compile command that silences them.
AVAILABILITY_WARNING='is only available on macOS [0-9.]+ or newer|-W(unguarded|partial)-availability'
availability() {
  local found
  found="$(grep -a -E "$AVAILABILITY_WARNING" "$2" | sort -u || true)"
  if [ -n "$found" ]; then
    echo "build_deps.sh: $1 uses an API newer than macOS $TARGET without an @available check (see $2):" >&2
    echo "$found" | head -20 >&2
    exit 1
  fi
  [ -n "${3:-}" ] || return 0
  if [ ! -f "$3" ]; then   # (CMake writes none for a project that compiles nothing: the headers)
    ! grep -q -a -E 'Building (C|CXX|OBJC|OBJCXX|ASM) object' "$2" && return 0
    echo "build_deps.sh: $1 compiled code but wrote no $3" >&2; exit 1
  fi
  found="$(python3 -I -c '
import json, shlex, sys
for c in json.load(open(sys.argv[1])):
    args = c.get("arguments") or shlex.split(c["command"])
    bad = sorted({a for a in args if a in ("-w", "-Wno-everything") or a.startswith((
        "-Wno-unguarded-availability", "-Wno-partial-availability", "-Wno-availability",
        "-Wno-error=unguarded-availability", "-Wno-error=partial-availability"))})
    if bad:
        print(c["file"] + ": " + " ".join(bad))
' "$3")" || { echo "build_deps.sh: couldn't read $3" >&2; exit 1; }
  if [ -n "$found" ]; then
    echo "build_deps.sh: $1 compiles with availability warnings silenced, so they can't be checked:" >&2
    echo "$found" | head -20 >&2
    exit 1
  fi
}

# The control: the compiler's warning for a newer API used unchecked must still
# read the way `availability` looks for (it needs an API newer than the target).
CONTROL_API=timingsafe_enable_if_supported CONTROL_HEADER=timingsafe.h CONTROL_MACOS=15.2
if [ "$TARGET" != "$CONTROL_MACOS" ] && [ "$(printf '%s\n' "$TARGET" "$CONTROL_MACOS" | sort -V | head -1)" = "$TARGET" ]; then
  mkdir -p "$WORK/control"
  printf '#include <%s>\nint control(void) { return %s(); }\n' "$CONTROL_HEADER" "$CONTROL_API" > "$WORK/control/control.c"
  "$CC" -c -arch arm64 -isysroot "$SDK" -mmacosx-version-min="$TARGET" "$WORK/control/control.c" -o "$WORK/control/control.o" \
    > "$WORK/control/control.log" 2>&1 || { cat "$WORK/control/control.log" >&2; exit 1; }
  grep -q -E "$AVAILABILITY_WARNING" "$WORK/control/control.log" \
    || { echo "build_deps.sh: the control ($CONTROL_API, macOS $CONTROL_MACOS) gave no warning that reads as expected:" >&2
         cat "$WORK/control/control.log" >&2; exit 1; }
  echo "control: $CC warns on $CONTROL_API (macOS $CONTROL_MACOS) built for $TARGET, as looked for" > "$LOGS/availability-control.txt"
else
  echo "control: skipped ($CONTROL_API is macOS $CONTROL_MACOS, not newer than $TARGET)" > "$LOGS/availability-control.txt"
fi

export PKG_CONFIG_LIBDIR="$WORK/no-pkgconfig"   # no .pc file of Homebrew's or anyone's
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"

src="$(unpack Vulkan-Headers)"
cmake_project Vulkan-Headers "$src"
licenses Vulkan-Headers "$src" LICENSE.md LICENSES/Apache-2.0.txt LICENSES/MIT.txt

src="$(unpack SPIRV-Headers)"
cmake_project SPIRV-Headers "$src"
licenses SPIRV-Headers "$src" LICENSE

src="$(unpack SPIRV-Tools)"
cmake_project SPIRV-Tools "$src" -DCMAKE_INSTALL_RPATH=@loader_path/../lib -DBUILD_SHARED_LIBS=ON \
  -DPython3_EXECUTABLE="$(command -v python3)" -DSPIRV-Headers_SOURCE_DIR="$PREFIX" \
  -DSPIRV_SKIP_TESTS=ON -DSPIRV_TOOLS_BUILD_STATIC=OFF -DSPIRV_SKIP_EXECUTABLES=ON
licenses SPIRV-Tools "$src" LICENSE

src="$(unpack glslang)"
cmake_project glslang "$src" -DBUILD_EXTERNAL=OFF -DALLOW_EXTERNAL_SPIRV_TOOLS=ON -DBUILD_SHARED_LIBS=ON \
  -DENABLE_CTEST=OFF -DENABLE_OPT=ON -DCMAKE_INSTALL_RPATH=@loader_path/../lib -DENABLE_GLSLANG_BINARIES=OFF -DGLSLANG_TESTS=OFF \
  -DPython3_EXECUTABLE="$(command -v python3)"
licenses glslang "$src" LICENSE.txt

src="$(unpack Vulkan-Loader)"
cmake_project Vulkan-Loader "$src" -DVULKAN_HEADERS_INSTALL_DIR="$PREFIX" -DCMAKE_INSTALL_SYSCONFDIR=/usr/local/etc
licenses Vulkan-Loader "$src" LICENSE.txt
# cJSON (MIT) is compiled into the loader; its notice is the top of its source.
sed -n '1,/\*\//p' "$src/loader/cJSON.c" > "$PREFIX/share/licenses/Vulkan-Loader/cJSON-LICENSE.txt"
grep -q "Permission is hereby granted" "$PREFIX/share/licenses/Vulkan-Loader/cJSON-LICENSE.txt" \
  || { echo "build_deps.sh: cJSON's notice not found in the loader's loader/cJSON.c" >&2; exit 1; }

src="$(unpack SDL3)"
cmake_project SDL3 "$src" -DSDL_TESTS=OFF -DSDL_X11_XTEST=OFF
# SDL3 itself, and the two parts it includes with their own license files:
# HIDAPI (controllers; SDL takes it under one of its three licenses) and
# yuv2rgb (video colour conversion).
licenses SDL3 "$src" LICENSE.txt src/hidapi/LICENSE.txt src/hidapi/LICENSE-bsd.txt src/hidapi/LICENSE-gpl3.txt \
  src/hidapi/LICENSE-orig.txt src/video/yuv2rgb/LICENSE

echo "== FFmpeg" >&2
ffmpeg_archive="$(fetch FFmpeg)" || exit 1
# Its configure takes the compiler by name (it records the command line in
# libavutil): `clang` from the folder of the one used above, first on PATH,
# with the same SDK (SDKROOT, which xcrun's /usr/bin shims would set).
SDKROOT="$SDK" PATH="$(dirname "$CC"):$PATH" "$ROOT/scripts/build_ffmpeg_xma.sh" "$ffmpeg_archive" "$PREFIX/ffmpeg-xma" \
  --cc=clang > "$LOGS/FFmpeg.log" 2>&1 \
  || { echo "build_deps.sh: FFmpeg failed; see $LOGS/FFmpeg.log" >&2; tail -20 "$LOGS/FFmpeg.log" >&2; exit 1; }
availability FFmpeg "$LOGS/FFmpeg.log"   # (its configure adds -Werror=partial-availability on macOS too)
FFMPEG_CC="$(PATH="$(dirname "$CC"):$PATH" clang --version | head -1)"
[ "$FFMPEG_CC" = "$("$CC" --version | head -1)" ] || { echo "build_deps.sh: FFmpeg's clang ($FFMPEG_CC) isn't $CC" >&2; exit 1; }
mkdir -p "$WORK/ffmpeg-licenses"
tar -xf "$ffmpeg_archive" -C "$WORK/ffmpeg-licenses" ffmpeg-7.1.1/COPYING.LGPLv2.1 ffmpeg-7.1.1/LICENSE.md
licenses FFmpeg "$WORK/ffmpeg-licenses/ffmpeg-7.1.1" COPYING.LGPLv2.1 LICENSE.md
cp "$ROOT"/patches/ffmpeg/*.patch "$PREFIX/share/licenses/FFmpeg/"

# The check: every library is arm64 only and built for exactly this macOS.
minos() { otool -l "$1" | awk '/LC_BUILD_VERSION/{b=1} b&&/minos/{print $2; b=0}' | sort -u; }
bad=0
for f in "$PREFIX"/lib/*.dylib "$PREFIX"/ffmpeg-xma/lib/*.a; do
  [ -L "$f" ] && continue
  m="$(minos "$f" | tr '\n' ' ' | sed 's/ $//')"
  a="$(lipo -archs "$f" 2>/dev/null || echo '?')"
  if [ "$m" != "$TARGET" ] || [ "$a" != arm64 ]; then
    echo "build_deps.sh: $f is $a for macOS $m, not arm64 for $TARGET" >&2; bad=1
  fi
done
[ $bad -eq 0 ] || exit 1

# The record make_app.py reads: each project, its source and the libraries it
# installed.
python3 -I - "$PREFIX" "$TARGET" "$("$CC" --version | head -1) ($CC)" "$(xcrun --sdk macosx --show-sdk-version)" \
  "$(cmake --version | head -1)" "ninja $(ninja --version)" "${SOURCES[@]}" <<'EOF'
import json, os, sys
prefix, target, compiler, sdk, cmake, ninja = sys.argv[1:7]
projects = {}
for line in sys.argv[7:]:
    name, version, url, sha256, archive = line.split('|')
    installed = os.path.join(prefix, 'build-logs', name + '.installed.txt')
    files = open(installed).read().split() if os.path.exists(installed) else []
    licenses = os.path.join(prefix, 'share/licenses', name)
    projects[name] = {
        'version': version, 'url': url, 'sha256': sha256,
        'libraries': sorted(os.path.basename(f) for f in files if f.endswith('.dylib')),
        'licenses': sorted(os.path.relpath(os.path.join(d, f), licenses)
                           for d, _, fs in os.walk(licenses) for f in fs),
    }
projects['FFmpeg']['libraries'] = ['ffmpeg-xma/lib/libavcodec.a', 'ffmpeg-xma/lib/libavutil.a']
record = {'deployment_target': target, 'architecture': 'arm64', 'compiler': compiler, 'sdk': sdk,
          'tools': {'cmake': cmake, 'ninja': ninja},
          'flags': 'CMake Release with -Os -DNDEBUG (as Homebrew), no -mcpu/-march; FFmpeg: its configure defaults '
                   '(-O3, as v0.1.0) with --cc=clang, the compiler above; SOURCE_DATE_EPOCH: each source\'s newest file',
          'projects': projects}
with open(os.path.join(prefix, 'share/speedbreaker-deps.json'), 'w') as f:
    json.dump(record, f, indent=2)
    f.write('\n')
EOF
echo "libraries for macOS $TARGET+ (arm64) in $PREFIX:"
ls "$PREFIX/lib" | grep '\.dylib$' | tr '\n' ' '; echo
echo "FFmpeg-XMA in $PREFIX/ffmpeg-xma; licenses in $PREFIX/share/licenses; record in $PREFIX/share/speedbreaker-deps.json"
