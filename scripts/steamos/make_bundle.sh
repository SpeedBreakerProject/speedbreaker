#!/bin/bash
# SpeedBreaker: a self-contained SteamOS build (for a Steam Deck, a Steam
# Machine or a Steam Frame) that needs no distrobox and no compiler. Run it
# inside the nfsmw-build distrobox after building, or on the Steam Frame
# itself (README.md here):
#
#   scripts/steamos/make_bundle.sh [build dir] [output dir]
#
# (defaults build/main and build/bundle). It makes <output>/SpeedBreaker/ and
# the release download <output>/SpeedBreaker-steamos-<x86_64 or arm64>.tar.xz
# (plus .sha256), with a copy named by the version,
# SpeedBreaker-<version>-steamos-<arch>.tar.xz (a hard link; its own .sha256):
# the game, the libraries it needs that SteamOS doesn't provide at the
# versions it was built against, the launcher (bundle/speedbreaker.sh), the
# player's README (bundle/README.md, or bundle/README-steam-frame.md for
# arm64) and licenses. Never any game data: players install from their own
# disc image on the first run.
#
# The launcher is the only program at the top of the folder: the game is
# lib/SpeedBreaker, beside its libraries (since v0.1.1; v0.1.0 had it at the
# top, where a player ran it directly and got "GLIBC_2.44 not found", since
# only the launcher picks the C library it needs). A v0.1.1 folder unpacked
# over a v0.1.0 one still holds v0.1.0's SpeedBreaker at the top: the
# launcher removes it at its first start.
#
# The game's binary has its debug information stripped (its symbol table
# stays: crash reports name functions with it, and the recompiled ones with
# the game's own table either way). The full symbols go to
# <output>/symbols/SpeedBreaker-<version>-<commit>-steamos-<arch>.debug, for
# us: never published, they map a player's report back to the exact build.
#
# Which libraries go in (ldd of the binary, sorted by soname):
# - From the system, never bundled: the Vulkan loader, which must find the
#   system's GPU drivers and Steam's and gamescope's layers, and everything
#   SDL loads at run time (X11, Wayland, PipeWire, PulseAudio, ALSA, udev,
#   D-Bus), which must match the system's display and audio servers. SteamOS
#   has them all; in a distrobox this script checks the host's copies
#   (/run/host) and says which are missing.
# - x86-64: the C and C++ runtimes, in lib/glibc and lib/cxx. The distrobox is
#   rolling Arch: its builds need glibc 2.44 (sinh and cosh got new symbol
#   versions; SDL 3.4 needs 2.43) and GLIBCXX_3.4.35, where SteamOS 3.8 has
#   glibc 2.41 and GLIBCXX_3.4.34. Building against an older baseline would
#   need another toolchain and SDL 3.4 rebuilt; instead the launcher runs the
#   game through the bundled loader, each runtime only while the system's is
#   older.
# - arm64 (built on the Steam Frame, against its own SteamOS): no C or C++
#   runtime. The system's are the ones it was built against, and the check
#   below holds the build to them.
# - Everything else (SDL3, glslang, SPIRV-Tools, and whatever the binary links
#   later) in lib/: SteamOS lacks glslang and ships SDL 3.2 and another
#   release of SPIRV-Tools, whose soname carries no version.
set -euo pipefail
# x86-64 in the nfsmw-build distrobox (Steam Deck, Steam Machine), or arm64 on
# the Steam Frame itself (its SteamOS has the compilers; see README.md).
case "$(uname -s)-$(uname -m)" in
    Linux-x86_64) ARCH=x86_64 LOADER=ld-linux-x86-64.so.2 HOST_LOADER=/lib64/ld-linux-x86-64.so.2 RUNTIMES=bundled README=README.md ;;
    Linux-aarch64) ARCH=arm64 LOADER=ld-linux-aarch64.so.1 HOST_LOADER=/lib/ld-linux-aarch64.so.1 RUNTIMES=system README=README-steam-frame.md ;;
    *) echo "make_bundle.sh: x86-64 or arm64 Linux only" >&2; exit 1 ;;
esac
SCRIPTS="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
ROOT="$(cd "$SCRIPTS/../.." && pwd)"
BUILD="$(readlink -f "${1:-$ROOT/build/main}")"
mkdir -p "${2:-$ROOT/build/bundle}"
OUT="$(readlink -f "${2:-$ROOT/build/bundle}")"
BIN="$BUILD/runtime/SpeedBreaker"
NAME=SpeedBreaker
STAGE="$OUT/$NAME"

[ -x "$BIN" ] || { echo "make_bundle.sh: no $BIN; build first (scripts/steamos/README.md)" >&2; exit 1; }
# The sources the binary was built from (licenses, version): the build's own.
SRC="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$BUILD/CMakeCache.txt" 2>/dev/null || true)"
[ -d "$SRC" ] || SRC="$ROOT"
TYPE="$(sed -n 's/^CMAKE_BUILD_TYPE:STRING=//p' "$BUILD/CMakeCache.txt" 2>/dev/null || true)"
[ "$TYPE" = Release ] || echo "make_bundle.sh: warning: build type '${TYPE:-unknown}', not Release" >&2
# The version and commit the build compiled in (runtime/report/build_info.cmake),
# not the checkout's state now.
INFO="$BUILD/runtime/generated/build_info.inc"
define() { sed -n "s/^#define NFSMW_BUILD_$1 \"\{0,1\}\([^\"]*\)\"\{0,1\}\$/\1/p" "$INFO" 2>/dev/null; }
VERSION="$(define VERSION)"
[ -n "$VERSION" ] || VERSION="$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "$BUILD/CMakeCache.txt" 2>/dev/null || true)"
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "make_bundle.sh: no version in $BUILD (project() in CMakeLists.txt)" >&2; exit 1; }
COMMIT="$(define COMMIT)"
[ "$(define DIRTY)" = 1 ] && COMMIT="$COMMIT-dirty"
case "$COMMIT" in
    "" | unknown*) echo "make_bundle.sh: warning: the build doesn't know its commit" >&2 ;;
    *-dirty) echo "make_bundle.sh: warning: the build is $COMMIT (uncommitted changes): not for release" >&2 ;;
esac

# The stage is deleted and rebuilt, so it must be one of ours: with ~ as the
# output folder it could be a folder of the player's (or a checkout of the
# repo). A bundle folder has the launcher and bundle-info.txt
# (an arm64 one has no lib/glibc).
if [ -e "$STAGE" ] && { [ -e "$STAGE/.git" ] || [ ! -f "$STAGE/speedbreaker.sh" ] || [ ! -f "$STAGE/bundle-info.txt" ]; }; then
    echo "make_bundle.sh: $STAGE exists and isn't a bundle folder; not replacing it (give another output folder)" >&2
    exit 1
fi
rm -rf "$STAGE"
mkdir -p "$STAGE/lib" "$STAGE/licenses"
[ "$RUNTIMES" = bundled ] && mkdir -p "$STAGE/lib/cxx" "$STAGE/lib/glibc"

# The binary without its debug information; the full symbols beside the bundle.
SYMBOLS="$OUT/symbols"
mkdir -p "$SYMBOLS"
OBJCOPY="$(command -v objcopy || command -v llvm-objcopy || true)"
[ -n "$OBJCOPY" ] || { echo "make_bundle.sh: no objcopy (binutils) to strip the binary" >&2; exit 1; }
DEBUG="SpeedBreaker-$VERSION-$COMMIT-steamos-$ARCH.debug"
"$OBJCOPY" --only-keep-debug "$BIN" "$SYMBOLS/$DEBUG"
GAME="$STAGE/lib/SpeedBreaker"
"$OBJCOPY" --strip-debug --add-gnu-debuglink="$SYMBOLS/$DEBUG" "$BIN" "$GAME"
# A build-time library path (a hand-built SDL3 in ~/deps) has no use in the
# bundle, which passes the loader its own lib/.
if readelf -d "$GAME" | grep -qE '\((RUNPATH|RPATH)\)'; then
    if command -v patchelf > /dev/null; then
        patchelf --remove-rpath "$GAME"
    else
        echo "make_bundle.sh: warning: the binary keeps its build's library path ($(readelf -d "$GAME" |
            sed -n 's/.*(R[UN]*PATH).*\[\(.*\)\]/\1/p')): harmless (the launcher's lib/ comes first), but install patchelf to drop it" >&2
    fi
fi
cp "$SCRIPTS/bundle/speedbreaker.sh" "$STAGE/"
cp "$SCRIPTS/bundle/$README" "$STAGE/README.md"
chmod 755 "$GAME" "$STAGE/speedbreaker.sh"
echo "SpeedBreaker v$VERSION ($COMMIT, built $(date -u -r "$BIN" +%F)), SteamOS $ARCH" > "$STAGE/VERSION"

# Sorted by soname; `ldd` lists the whole closure. A library the build found
# outside the system's folders (CMake's <Package>_DIR, such as the Frame's
# SDL3 in ~/deps/sdl3/lib/cmake/SDL3) is looked up in that package's lib/: the
# binary carries no RUNPATH to it (-DCMAKE_SKIP_BUILD_RPATH=ON, see the folder
# check below).
libdirs="$(sed -n 's|^[A-Za-z0-9_-]*_DIR:[A-Z]*=\(/.*/lib\(64\)\{0,1\}\)/cmake/[^/]*$|\1|p' "$BUILD/CMakeCache.txt" 2>/dev/null |
    grep -v -x -e /usr/lib -e /usr/lib64 -e /lib -e /lib64 | sort -u | paste -sd: - || true)"
ldd() {
    if [ -n "$libdirs" ]; then LD_LIBRARY_PATH="$libdirs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" command ldd "$@"
    else command ldd "$@"; fi
}
system=() bundled=()
LIBC=""
while read -r soname arrow path _; do
    [ "$arrow" = "=>" ] || continue  # the vdso
    if [ "$path" = not ]; then echo "make_bundle.sh: $soname not found" >&2; exit 1; fi
    case "$soname" in
        */ld-linux* | ld-linux*) ;;  # the loader (by path): with glibc below
        libvulkan.so.* | libX11* | libXext* | libxcb* | libwayland-* | libdecor-* | libdrm* | libgbm* | libGL* | libEGL* | \
        libpipewire-* | libpulse* | libasound* | libudev* | libdbus-1* | libsystemd*)
            system+=("$soname") ;;
        libc.so.*) LIBC="$path" ;;
        libm.so.* | libmvec.so.* | libpthread.so.* | libdl.so.* | librt.so.* | libutil.so.* | libanl.so.* | libresolv.so.*) ;;
        libstdc++.so.* | libgcc_s.so.*)
            if [ "$RUNTIMES" = bundled ]; then
                cp -L "$path" "$STAGE/lib/cxx/$soname"; bundled+=("cxx/$soname:$path")
            else
                system+=("$soname")
            fi ;;
        *) cp -L "$path" "$STAGE/lib/$soname"; bundled+=("$soname:$path") ;;
    esac
done < <(ldd "$BIN")
[ -n "$LIBC" ] || { echo "make_bundle.sh: no libc.so.6 in ldd's list" >&2; exit 1; }
if [ "$RUNTIMES" = bundled ]; then
    # All of glibc, not only what the binary names: system libraries loaded
    # into the game (Mesa, PipeWire, Steam's overlay) may still name
    # libpthread.so.0, libdl.so.2 or librt.so.1, and those must be this
    # glibc's, not the system's.
    GLIBC_DIR="$(dirname "$(readlink -f "$LIBC")")"
    for soname in $LOADER libc.so.6 libm.so.6 libmvec.so.1 libpthread.so.0 libdl.so.2 librt.so.1 libutil.so.1 \
        libanl.so.1 libresolv.so.2 libnss_files.so.2 libnss_dns.so.2 libnss_compat.so.2; do
        if [ -e "$GLIBC_DIR/$soname" ]; then
            cp -L "$GLIBC_DIR/$soname" "$STAGE/lib/glibc/$soname"
            bundled+=("glibc/$soname:$GLIBC_DIR/$soname")
        fi
    done
    chmod 755 "$STAGE/lib/glibc/$LOADER"
fi

# The newest symbol versions everything here asks for, against what the
# runtimes it runs on give (a check that nothing needs more than they have):
# the bundled ones on x86-64, the system's on arm64.
newest() {  # newest <prefix> <files...>: from each file's needs, not its definitions
    local prefix=$1; shift
    for f in "$@"; do readelf -V --wide "$f" 2>/dev/null | sed -n '/\.gnu\.version_r/,$p'; done |
        grep -oE "Name: ${prefix}_[0-9.]+" | awk '{ print $2 }' | sort -uV | tail -1
}
provides() { grep -ao "${1}_[0-9.]*[0-9]" "$2" | sort -uV | tail -1; }
shopt -s nullglob
elves=("$GAME" "$STAGE"/lib/*.so* "$STAGE"/lib/cxx/*)
shopt -u nullglob
need_glibc="$(newest GLIBC "${elves[@]}")"
need_cxx="$(newest GLIBCXX "${elves[@]}")"
if [ "$RUNTIMES" = bundled ]; then
    have_glibc="$("$STAGE/lib/glibc/$LOADER" --version | sed -n '1s/.* version \([0-9][0-9.]*[0-9]\).*/\1/p')"
    have_cxx="$(provides GLIBCXX "$STAGE/lib/cxx/libstdc++.so.6")"
    runtimes="bundled"
else
    have_glibc="$("$HOST_LOADER" --version | sed -n '1s/.* version \([0-9][0-9.]*[0-9]\).*/\1/p')"
    cxx_path="$(ldd "$BIN" | sed -n 's/^\s*libstdc++\.so\.6 => \(\S*\).*/\1/p')"
    have_cxx="$(provides GLIBCXX "${cxx_path:-/usr/lib/libstdc++.so.6}")"
    runtimes="the system's"
fi
if [ -z "$have_glibc" ] || [ -z "$have_cxx" ] ||
    [ "$(printf '%s\n%s\n' "${need_glibc#GLIBC_}" "$have_glibc" | sort -V | tail -1)" != "$have_glibc" ] ||
    [ "$(printf '%s\n%s\n' "$need_cxx" "$have_cxx" | sort -V | tail -1)" != "$have_cxx" ]; then
    echo "make_bundle.sh: needs $need_glibc and $need_cxx; $runtimes: glibc ${have_glibc:-?} and ${have_cxx:-?}" >&2
    exit 1
fi

# Licenses: the game's notices (COPYING, NOTICE, THIRD_PARTY_NOTICES.md and
# LICENSES/, which cover what is compiled into it), and each bundled
# library's own: its package's (Arch and SteamOS keep them in
# /usr/share/licenses/<package>), or for one built by hand (the Frame's SDL3
# in ~/deps/sdl3), the license files its install prefix holds. A bundled
# library without one stops the bundle: the README promises them all.
for f in COPYING NOTICE THIRD_PARTY_NOTICES.md; do
    [ -f "$SRC/$f" ] || { echo "make_bundle.sh: no $f in $SRC" >&2; exit 1; }
    cp "$SRC/$f" "$STAGE/licenses/"
done
cp -R "$SRC/LICENSES" "$STAGE/licenses/LICENSES"
DISTRO="$(. /etc/os-release 2>/dev/null; echo "${NAME:-the build system}")"
{
    echo "SpeedBreaker is free software under the GPL-3.0-or-later (COPYING, NOTICE). What is"
    echo "compiled into it, and each license, is in THIRD_PARTY_NOTICES.md and LICENSES/."
    echo
    echo "Bundled libraries (lib/), from $DISTRO packages unless noted:"
} > "$STAGE/licenses/README.txt"
declare -A packages=()
unlicensed=()
for entry in "${bundled[@]}"; do
    pkg=""
    command -v pacman > /dev/null && pkg="$(pacman -Qqo "${entry#*:}" 2>/dev/null || true)"
    if [ -n "$pkg" ]; then
        echo "  ${entry%%:*}: $pkg$(pacman -Qi "$pkg" | sed -n 's/^Version *: / /p; s/^Licenses *: /, /p' | tr -d '\n')" >> "$STAGE/licenses/README.txt"
        packages[$pkg]=1
        continue
    fi
    # Built by hand: <prefix>/lib/<file>, its license under <prefix>/share.
    prefix="$(dirname "$(dirname "$(readlink -f "${entry#*:}")")")"
    name="$(basename "${entry%%:*}" | sed 's/^lib//; s/\.so.*//')"
    found=()
    while IFS= read -r f; do found+=("$f"); done < <(find "$prefix/share/licenses" "$prefix/share/doc" -maxdepth 3 -type f \
        \( -iname 'LICENSE*' -o -iname 'COPYING*' \) 2>/dev/null | sort)
    if [ ${#found[@]} -eq 0 ]; then
        unlicensed+=("${entry%%:*} (${entry#*:})")
        continue
    fi
    mkdir -p "$STAGE/licenses/$name"
    cp "${found[@]}" "$STAGE/licenses/$name/"
    echo "  ${entry%%:*}: built from source (not a package); license: $name/$(cd "$STAGE/licenses/$name" && ls | tr '\n' ' ')" >> "$STAGE/licenses/README.txt"
done
if [ ${#unlicensed[@]} -gt 0 ]; then
    echo "make_bundle.sh: no license found for: ${unlicensed[*]}" >&2
    exit 1
fi
for pkg in "${!packages[@]}"; do
    if [ -d "/usr/share/licenses/$pkg" ]; then
        mkdir -p "$STAGE/licenses/$pkg"
        cp -rL "/usr/share/licenses/$pkg/." "$STAGE/licenses/$pkg/"
    fi
    for id in $(pacman -Qi "$pkg" | sed -n 's/^Licenses *: //p' | sed 's/ WITH [^ ]*//g'); do
        [ -f "/usr/share/licenses/spdx/$id.txt" ] && mkdir -p "$STAGE/licenses/$pkg" && cp "/usr/share/licenses/spdx/$id.txt" "$STAGE/licenses/$pkg/"
    done
done

# Never game data (the installer brings it from the player's own disc).
if find "$STAGE" \( -iname '*.xex' -o -iname '*.iso' -o -iname '*.bin' -o -iname '*.xmv' -o -iname '*.big' \) | grep -q .; then
    echo "make_bundle.sh: game data in $STAGE" >&2
    exit 1
fi

# No folder of the build machine in what's published (the sources are named
# from the repository root, CMakeLists.txt's -ffile-prefix-map; FFmpeg-XMA is
# built without debug information, scripts/build_ffmpeg_xma.sh). tools/ and
# ppc/ may be links into another checkout. The home folder too: a removed
# RUNPATH (patchelf above) leaves its text behind, so a library folder under
# ~ (a hand-built SDL3 in ~/deps) is kept out at configure time instead
# (-DCMAKE_SKIP_BUILD_RPATH=ON, as the README's Steam Frame line does).
folders=(-e "$SRC" -e "$BUILD")
for d in tools ppc; do [ -L "$SRC/$d" ] && folders+=(-e "$(dirname "$(readlink -f "$SRC/$d")")"); done
[ "${#HOME}" -gt 1 ] && folders+=(-e "$HOME/")
leaks="$(grep -aoF "${folders[@]}" "$GAME" | sort | uniq -c || true)"
if [ -n "$leaks" ]; then
    echo "make_bundle.sh: the binary names the build machine's folders (for a library folder, configure with -DCMAKE_SKIP_BUILD_RPATH=ON):" >&2
    echo "$leaks" >&2
    [ "${SB_ALLOW_PATHS:-0}" = 1 ] || exit 1
fi

# The top of the folder: the launcher is the only program there (lib/ holds the
# game), so nothing else can be run by mistake.
top="$(cd "$STAGE" && find . -mindepth 1 -maxdepth 1 -type f -perm -u+x ! -name speedbreaker.sh | sed 's|^\./||')"
if [ -n "$top" ]; then
    echo "make_bundle.sh: programs at the top of $STAGE besides speedbreaker.sh: $top" >&2
    exit 1
fi

{
    echo "SpeedBreaker v$VERSION ($COMMIT), $TYPE build for SteamOS $ARCH, bundled $(date -u '+%F %T UTC')"
    echo "Needs: $need_glibc and $need_cxx ($runtimes: glibc $have_glibc, $have_cxx)"
    echo "From the system: ${system[*]} (and what SDL loads at run time)"
} > "$STAGE/bundle-info.txt"
(cd "$STAGE" && find . -type f ! -name SHA256SUMS | sort | sed 's|^\./||' | xargs sha256sum > SHA256SUMS)

# The host's side, when this runs in a distrobox: what the launcher will find.
if [ -d /run/host/usr/lib ]; then
    # (The loader is self-contained, so the host's runs here.)
    host_glibc="$(/run/host/usr/lib/$LOADER --version 2>/dev/null | sed -n '1s/.* version \([0-9][0-9.]*[0-9]\).*/\1/p' || true)"
    echo "host: glibc ${host_glibc:-?}, $(provides GLIBCXX /run/host/usr/lib/libstdc++.so.6 2>/dev/null || echo 'no libstdc++')"
    for soname in "${system[@]}" libX11.so.6 libXext.so.6 libXcursor.so.1 libXi.so.6 libXfixes.so.3 libXrandr.so.2 libXss.so.1 \
        libxkbcommon.so.0 libwayland-client.so.0 libdecor-0.so.0 libpipewire-0.3.so.0 libpulse.so.0 libasound.so.2 libudev.so.1 libdbus-1.so.3; do
        [ -e "/run/host/usr/lib/$soname" ] || echo "host: warning: no $soname (the system's copy is used)"
    done
fi

# The release download, by the name the website links to, and a copy named by
# the version (a hard link). Each .sha256 names its own file.
TAR="$OUT/SpeedBreaker-steamos-$ARCH.tar.xz"
VERSIONED="$OUT/SpeedBreaker-$VERSION-steamos-$ARCH.tar.xz"
tar -C "$OUT" --sort=name --owner=0 --group=0 --numeric-owner --mtime="@$(stat -c %Y "$BIN")" -cf - "$NAME" |
    xz -T0 -9 > "$TAR.partial"
mv "$TAR.partial" "$TAR"
rm -f "$VERSIONED"
ln "$TAR" "$VERSIONED" 2>/dev/null || cp "$TAR" "$VERSIONED"
for f in "$TAR" "$VERSIONED"; do
    (cd "$OUT" && sha256sum "$(basename "$f")" > "$(basename "$f").sha256")
done
echo "$(du -sh "$STAGE" | cut -f1) in $STAGE; $(du -h "$TAR" | cut -f1) $TAR (and $(basename "$VERSIONED")); symbols: $SYMBOLS/$DEBUG"
echo "v$VERSION ($COMMIT); needs $need_glibc and $need_cxx ($runtimes); bundled: ${bundled[*]%%:*}"
