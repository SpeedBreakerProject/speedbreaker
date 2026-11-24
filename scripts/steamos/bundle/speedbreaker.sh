#!/bin/bash
# SpeedBreaker on SteamOS without a distrobox (a Steam Deck, a Steam Machine,
# a Steam Frame): add this script to Steam as a non-Steam game (README.md next
# to it). It is the only program at the top of this folder: it runs the game,
# lib/SpeedBreaker, with the libraries in lib/ (the game on its own can't pick
# them: started directly it fails, e.g. "GLIBC_2.44 not found"), and logs to
# last-run.log here;
# the run before stays as previous-run.log, so a relaunch after a crash
# doesn't lose it. SpeedBreaker's own options pass through
# (`speedbreaker.sh --install <image>`), and NFSMW_* settings can go in Steam's
# launch options, e.g. `NFSMW_DEADZONE=0.12 %command%`.
#
# The x86-64 game is built in a rolling Arch distrobox
# (scripts/steamos/make_bundle.sh made this folder), newer than SteamOS, so:
#   lib/        SDL3, glslang, SPIRV-Tools: always these, never the system's.
#   lib/cxx/    libstdc++ and libgcc_s: used when newer than the system's.
#   lib/glibc/  the C library and its loader: used when the system's is older
#               than the one the game was built against. The game then starts
#               through that loader, and the system's own libraries (the
#               Vulkan loader, the GPU driver, X11, PipeWire) load against
#               this newer C library, which runs them as the system's would:
#               glibc keeps old symbol versions. A later SteamOS with a C
#               library as new runs the game on its own.
# The arm64 game (the Steam Frame's) is built on SteamOS itself, against its
# own C and C++ runtimes: its bundle has lib/ only, and runs on the system's.
# The Vulkan loader and GPU drivers always come from the system. The loader
# is given the library path as an option, not LD_LIBRARY_PATH, so programs the
# game starts (a file dialog) never see these libraries.
# NFSMW_BUNDLED_GLIBC=1|0 and NFSMW_BUNDLED_CXX=1|0 force a choice (debugging).
HERE="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
BIN="$HERE/lib/SpeedBreaker"

# Portable mode (README.md): a portable.txt here keeps the game's settings,
# saves, logs and caches in this folder (data/ and cache/); the game itself
# looks for the file, this script only reports it and keeps its own log out
# of the home folder then.
PORTABLE=0
[ -f "$HERE/portable.txt" ] && PORTABLE=1

# The log: here, or in the data folder if this one isn't writable (or /tmp,
# the last place a log can go; in portable mode, straight there). On a
# terminal it's shown as well (`speedbreaker.sh --install ...`).
LOG_DIR="$HERE"
if [ ! -w "$LOG_DIR" ]; then
    if [ "$PORTABLE" = 1 ]; then
        LOG_DIR="${TMPDIR:-/tmp}"
    else
        LOG_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/speedbreaker"
        mkdir -p "$LOG_DIR" 2>/dev/null
        [ -w "$LOG_DIR" ] || LOG_DIR="${TMPDIR:-/tmp}"
    fi
fi
[ -f "$LOG_DIR/last-run.log" ] && mv -f "$LOG_DIR/last-run.log" "$LOG_DIR/previous-run.log"
if [ -t 2 ]; then
    exec > >(tee "$LOG_DIR/last-run.log") 2>&1
else
    exec > "$LOG_DIR/last-run.log" 2>&1
fi

say() { echo "[launcher] $*"; }
say "$(cat "$HERE/VERSION" 2>/dev/null || echo "SpeedBreaker (no VERSION file)"), started $(date '+%F %T %z')"
[ "$PORTABLE" = 1 ] && say "portable mode: portable.txt in $HERE (data/ and cache/ beside it)"

# Up to v0.1.0 the game was here at the top, beside this script; since v0.1.1
# it is lib/SpeedBreaker. A newer release unpacked over an older folder (the
# README's way to update) leaves the old program behind: an old version that
# runs if started directly. With the new one in lib/, it goes (or, if it
# can't be removed, it's renamed aside), once, and the log says so.
OLD="$HERE/SpeedBreaker"
if [ -f "$BIN" ] && [ -f "$OLD" ] && [ ! -L "$OLD" ]; then
    if rm -f "$OLD" 2>/dev/null && [ ! -e "$OLD" ]; then
        say "removed SpeedBreaker at the top of this folder: an earlier release's program, left by unpacking this one over it (the game is lib/SpeedBreaker now)"
    elif mv -f "$OLD" "$OLD.old" 2>/dev/null; then
        say "renamed SpeedBreaker at the top of this folder to SpeedBreaker.old: an earlier release's program (the game is lib/SpeedBreaker now)"
    else
        say "warning: can't remove SpeedBreaker at the top of this folder, an earlier release's program (the game is lib/SpeedBreaker now): delete it, and start the game with speedbreaker.sh"
    fi
fi
if [ ! -f "$BIN" ]; then
    say "no lib/SpeedBreaker in $HERE: unpack the whole download again"
    exit 1
fi
say "system: $(. /etc/os-release 2>/dev/null; echo "${NAME:-unknown} ${VERSION_ID:-}${BUILD_ID:+ build $BUILD_ID}${VARIANT_ID:+ ($VARIANT_ID)}"), kernel $(uname -r)," \
    "CPU$(grep -m1 '^model name' /proc/cpuinfo | cut -d: -f2-), $(nproc) threads, $(awk '/^MemTotal/ { printf "%.1f GB", $2 / 1048576 }' /proc/meminfo)"

# The game is compiled for x86-64-v3 (AVX2, FMA, BMI2, MOVBE: Intel Haswell,
# AMD Zen and later; every Steam Deck). On an older CPU it would die at its
# first such instruction with SIGILL, which says nothing to a player.
# (The arm64 build, the Steam Frame's, needs nothing beyond ARMv8.)
flags=" $(grep -m1 '^flags' /proc/cpuinfo | cut -d: -f2-) "
missing=""
if [ "$(uname -m)" = x86_64 ]; then
    for f in avx2 fma bmi2 movbe; do
        case "$flags" in *" $f "*) ;; *) missing="$missing $f" ;; esac
    done
fi
if [ -n "$missing" ]; then
    say "this CPU lacks$missing: the game needs an x86-64-v3 CPU (Intel Haswell / AMD Zen or later)"
    exit 1
fi
# The loader's library path splits at ':' and ';' and expands $ORIGIN, $LIB
# and $PLATFORM, so in a folder named "Need for Speed: Most Wanted" the game
# wouldn't find its libraries ("libglslang.so.16: cannot open shared object
# file"). Say so instead.
case "$HERE" in
    *[:\;\$]*) say "the folder's path has a ':', ';' or '\$' ($HERE): move it or rename that folder"; exit 1 ;;
esac

# Newer wins, separately for the C and the C++ runtime.
case "$(uname -m)" in
    aarch64) LOADER_NAME=ld-linux-aarch64.so.1 HOST_LOADER=/lib/ld-linux-aarch64.so.1 ;;
    *) LOADER_NAME=ld-linux-x86-64.so.2 HOST_LOADER=/lib64/ld-linux-x86-64.so.2 ;;
esac
BUNDLED_LOADER="$HERE/lib/glibc/$LOADER_NAME"
loader_version() { "$1" --version 2>/dev/null | sed -n '1s/.* version \([0-9][0-9.]*[0-9]\).*/\1/p'; }
cxx_version() { grep -ao 'GLIBCXX_3\.4\.[0-9]*' "$1" 2>/dev/null | sort -uV | tail -1; }
older() { [ "$1" != "$2" ] && [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | head -1)" = "$1" ]; }  # $1 < $2

host_glibc=""
[ -x "$HOST_LOADER" ] && host_glibc="$(loader_version "$HOST_LOADER")"
bundled_glibc="$(loader_version "$BUNDLED_LOADER")"
# Unpacked on a card formatted for Windows (files without the execute
# permission) or a drive mounted noexec, nothing here can run: the loader
# fails first, and the system's then refuses the game's newer glibc needs.
# (Only where there is a bundled loader: the arm64 bundle has none.)
if [ -e "$BUNDLED_LOADER" ] && [ -z "$bundled_glibc" ]; then
    say "can't run lib/glibc/$LOADER_NAME: is this folder on a card formatted for Windows (FAT32, exFAT, NTFS), or a drive mounted noexec? (README.md, 1. Unpack)"
fi
case "${NFSMW_BUNDLED_GLIBC:-}" in
    1) use_glibc=1 ;;
    0) use_glibc=0 ;;
    *) if [ -n "$bundled_glibc" ] && { [ -z "$host_glibc" ] || older "$host_glibc" "$bundled_glibc"; }; then use_glibc=1; else use_glibc=0; fi ;;
esac
# No bundled C library (the arm64 bundle): the system's, whatever was asked.
[ -e "$BUNDLED_LOADER" ] || use_glibc=0

host_cxx=""
for f in /usr/lib/libstdc++.so.6 /usr/lib64/libstdc++.so.6 /usr/lib/x86_64-linux-gnu/libstdc++.so.6 /lib/x86_64-linux-gnu/libstdc++.so.6 \
    /usr/lib/aarch64-linux-gnu/libstdc++.so.6; do
    if [ -e "$f" ]; then host_cxx="$(cxx_version "$f")"; break; fi
done
bundled_cxx="$(cxx_version "$HERE/lib/cxx/libstdc++.so.6")"
case "${NFSMW_BUNDLED_CXX:-}" in
    1) use_cxx=1 ;;
    0) use_cxx=0 ;;
    *) if [ -n "$bundled_cxx" ] && { [ -z "$host_cxx" ] || older "$host_cxx" "$bundled_cxx"; }; then use_cxx=1; else use_cxx=0; fi ;;
esac
[ -e "$HERE/lib/cxx/libstdc++.so.6" ] || use_cxx=0

LIBS="$HERE/lib"
[ "$use_cxx" = 1 ] && LIBS="$LIBS:$HERE/lib/cxx"
LOADER="$HOST_LOADER"
if [ "$use_glibc" = 1 ]; then
    LIBS="$LIBS:$HERE/lib/glibc"
    LOADER="$BUNDLED_LOADER"
fi
say "C library: $([ "$use_glibc" = 1 ] && echo bundled || echo "the system's") (system ${host_glibc:-none}, bundled ${bundled_glibc:-none});" \
    "C++ library: $([ "$use_cxx" = 1 ] && echo bundled || echo "the system's") (system ${host_cxx:-none}, bundled ${bundled_cxx:-none})"

# Laptops with two GPUs: an Intel or AMD one in the processor, beside an
# NVIDIA graphics card. The game itself draws with the discrete GPU (its
# log's "[video] Vulkan devices:" line lists them all; NFSMW_GPU=<index or
# part of a name> picks another). With NVIDIA's own driver, this also turns
# on NVIDIA's PRIME render offload for the game, as NVIDIA documents for
# Vulkan programs: __NV_PRIME_RENDER_OFFLOAD=1 enables NVIDIA's Vulkan layer,
# which lists the NVIDIA GPU first. The idea and its first version: GitHub
# PR #6 by lydacious (tested on an i9-13900H laptop with an RTX 3050: the game
# had run on the Intel GPU, and ran at 60 fps on the NVIDIA one).
# Only when a GPU is driven by NVIDIA's kernel driver, another GPU is in use
# beside it, and NVIDIA's Vulkan driver is where the Vulkan loader looks for
# drivers; never over a value already set (__NV_PRIME_RENDER_OFFLOAD=0 in
# Steam's launch options turns it off). Left out on purpose:
#   VK_DRIVER_FILES=<NVIDIA's driver only>: the other GPU would vanish, so a
#     broken NVIDIA driver (updated, not yet rebooted) would leave the game no
#     GPU instead of the integrated one, and NFSMW_GPU couldn't pick it; the
#     game prefers the NVIDIA GPU without it.
#   __GLX_VENDOR_LIBRARY_NAME=nvidia: OpenGL only, which the game doesn't
#     use; it would only move programs the game starts (a file dialog, the
#     web browser) onto the NVIDIA GPU.
# One log line names the PCI GPUs (none on the Steam Frame, whose GPU isn't
# on PCI: no line) and what was done. tests/launcher_prime_test.sh runs these
# two functions on made-up /sys, /proc and Vulkan folders (<root>).

# nvidia_icd [root]: NVIDIA's Vulkan driver manifest, where the Vulkan loader
# looks (its folders, in its order), or nothing.
nvidia_icd() {
    local root="${1:-}" IFS=: dir name
    for dir in ${XDG_CONFIG_HOME:-$HOME/.config} ${XDG_CONFIG_DIRS:-/etc/xdg} /etc \
        ${XDG_DATA_HOME:-$HOME/.local/share} ${XDG_DATA_DIRS:-/usr/local/share:/usr/share} /usr/local/share /usr/share; do
        [ -n "$dir" ] || continue
        for name in nvidia_icd.json "nvidia_icd.$(uname -m).json"; do
            if [ -f "$root$dir/vulkan/icd.d/$name" ]; then
                echo "$dir/vulkan/icd.d/$name"
                return 0
            fi
        done
    done
    return 1
}

# prime_offload [root]: the above.
prime_offload() {
    local root="${1:-}" d class vendor device driver kind version gpus="" nvidia=0 others=0 icd
    for d in "$root"/sys/bus/pci/devices/*; do
        class="$(cat "$d/class" 2>/dev/null)" || continue
        case "$class" in 0x03*) ;; *) continue ;; esac  # display controllers: VGA, 3D, other
        vendor="$(cat "$d/vendor" 2>/dev/null)"
        device="$(cat "$d/device" 2>/dev/null)"
        driver="$(readlink "$d/driver" 2>/dev/null)"
        driver="${driver##*/}"
        case "$vendor" in
            0x10de) kind=NVIDIA ;; 0x8086) kind=Intel ;; 0x1002) kind=AMD ;; *) kind=PCI ;;
        esac
        gpus="$gpus${gpus:+, }$kind ${vendor#0x}:${device#0x} (${driver:-no driver})"
        if [ "$vendor" = 0x10de ]; then
            [ "$driver" = nvidia ] && nvidia=$((nvidia + 1))
        else
            case "$driver" in ""|vfio-pci|pci-stub) ;; *) others=$((others + 1)) ;; esac  # not passed to a VM
        fi
    done
    [ -n "$gpus" ] || return 0
    if [ "$nvidia" = 0 ] || [ "$others" = 0 ] || [ ! -d "$root/proc/driver/nvidia" ]; then
        say "graphics: $gpus"
        return 0
    fi
    version="$(sed -n '1s/.*  \([0-9][0-9.]*[0-9]\)  .*/\1/p' "$root/proc/driver/nvidia/version" 2>/dev/null)"
    gpus="$gpus; NVIDIA driver${version:+ $version} beside another GPU"
    if ! icd="$(nvidia_icd "$root")"; then
        say "graphics: $gpus, but no NVIDIA Vulkan driver (nvidia_icd.json) where the Vulkan loader looks: no PRIME render offload"
    elif [ -n "${__NV_PRIME_RENDER_OFFLOAD+set}" ]; then
        say "graphics: $gpus; NVIDIA's Vulkan driver $icd; __NV_PRIME_RENDER_OFFLOAD=$__NV_PRIME_RENDER_OFFLOAD was already set: left as it is"
    else
        export __NV_PRIME_RENDER_OFFLOAD=1
        say "graphics: $gpus; NVIDIA's Vulkan driver $icd; __NV_PRIME_RENDER_OFFLOAD=1 (PRIME render offload: NVIDIA's layer lists its GPU first)"
    fi
}
prime_offload

# The Steam Frame's Adreno GPU and Mesa's Turnip driver: every render pass is
# drawn straight to memory (TU_DEBUG=sysmem) instead of through the GPU's tile
# memory. For this game the tiled path costs more than it saves: before the
# main scene and the sun's shadow map are drawn tile by tile, Turnip runs
# every draw once more to sort them into tiles, and its choice between the
# two paths doesn't count that pass. On a Steam Frame (2026-11-22, the same
# picture both ways): about 1 ms less GPU time a frame at 1x, 3 ms at 2x
# (31 -> 35 fps in the city). Only on arm64: TU_DEBUG is read by Turnip
# alone, and the x86-64 machines have AMD, Intel or NVIDIA GPUs. Never over
# a value already set (TU_DEBUG= in Steam's launch options leaves Turnip its
# own choice); NFSMW_TURNIP_SYSMEM=0 turns it off.
# tests/launcher_turnip_test.sh runs this function alone.
turnip_sysmem() {
    [ "$(uname -m)" = aarch64 ] || return 0
    if [ "${NFSMW_TURNIP_SYSMEM:-1}" = 0 ]; then
        say "graphics: NFSMW_TURNIP_SYSMEM=0: Turnip chooses between its tile memory and memory itself"
    elif [ -n "${TU_DEBUG+set}" ]; then
        say "graphics: TU_DEBUG=$TU_DEBUG was already set: left as it is"
    else
        export TU_DEBUG=sysmem
        say "graphics: TU_DEBUG=sysmem (Turnip draws straight to memory, not through its tile memory; NFSMW_TURNIP_SYSMEM=0 turns this off)"
    fi
}
turnip_sysmem

# Game Mode (gamescope, from Steam's environment): the game runs fullscreen
# at the resolution Steam sets for it (Properties > General > Game
# Resolution). SDL uses X11 (gamescope's Xwayland), which is how gamescope
# matches the game's window to the Steam shortcut that launched it; the same
# in Desktop Mode, as the distrobox launcher (scripts/steamos/speedbreaker.sh) does.
GAME_MODE=0
if [ -n "$GAMESCOPE_WAYLAND_DISPLAY" ] || [ "$XDG_CURRENT_DESKTOP" = gamescope ]; then GAME_MODE=1; fi
export NFSMW_GAME_MODE="${NFSMW_GAME_MODE:-$GAME_MODE}"
export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-x11}"
# Stick readings once a second (drift reports) and the [latency] lines.
export NFSMW_INPUT_LOG="${NFSMW_INPUT_LOG:-1}" NFSMW_LOG_LATENCY="${NFSMW_LOG_LATENCY:-1}"
say "Game Mode $NFSMW_GAME_MODE, video $SDL_VIDEODRIVER, display ${DISPLAY:-none}${GAMESCOPE_WAYLAND_DISPLAY:+, gamescope $GAMESCOPE_WAYLAND_DISPLAY}"

# exec: Steam's Exit Game stops the game itself (no lifeline needed).
exec "$LOADER" --library-path "$LIBS" --argv0 "$BIN" "$BIN" "$@"
