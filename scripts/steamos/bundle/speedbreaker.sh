#!/bin/bash
# SpeedBreaker on SteamOS without a distrobox (a Steam Deck, a Steam Machine,
# a Steam Frame): add this script to Steam as a non-Steam game (README.md next
# to it). It runs SpeedBreaker from this folder with the libraries in lib/
# and logs to last-run.log here;
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
BIN="$HERE/SpeedBreaker"

# The log: here, or in the data folder if this one isn't writable (or /tmp,
# the last place a log can go). On a terminal it's shown as well
# (`speedbreaker.sh --install ...`).
LOG_DIR="$HERE"
if [ ! -w "$LOG_DIR" ]; then
    LOG_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/speedbreaker"
    mkdir -p "$LOG_DIR" 2>/dev/null
    [ -w "$LOG_DIR" ] || LOG_DIR="${TMPDIR:-/tmp}"
fi
[ -f "$LOG_DIR/last-run.log" ] && mv -f "$LOG_DIR/last-run.log" "$LOG_DIR/previous-run.log"
if [ -t 2 ]; then
    exec > >(tee "$LOG_DIR/last-run.log") 2>&1
else
    exec > "$LOG_DIR/last-run.log" 2>&1
fi

say() { echo "[launcher] $*"; }
say "$(cat "$HERE/VERSION" 2>/dev/null || echo "SpeedBreaker (no VERSION file)"), started $(date '+%F %T %z')"
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

# Game Mode (gamescope, from Steam's environment): the game runs fullscreen
# at the resolution Steam sets for it (Properties > General > Game
# Resolution). SDL uses X11 (gamescope's Xwayland), which is how gamescope
# matches the game's window to the Steam shortcut that launched it; the same
# in Desktop Mode, as the distrobox launcher (scripts/steamos/speedbreaker.sh) does.
# Hybrid graphics on Linux PCs (e.g. Intel/AMD CPU with an NVIDIA GPU):
# route Vulkan to the discrete NVIDIA GPU so the game runs on the dedicated
# graphics card and avoids Intel Mesa shared-memory crashes.
if [ -d /proc/driver/nvidia ] && [ -e /usr/share/vulkan/icd.d/nvidia_icd.json ]; then
    export __NV_PRIME_RENDER_OFFLOAD="${__NV_PRIME_RENDER_OFFLOAD:-1}"
    export __GLX_VENDOR_LIBRARY_NAME="${__GLX_VENDOR_LIBRARY_NAME:-nvidia}"
    export VK_DRIVER_FILES="${VK_DRIVER_FILES:-/usr/share/vulkan/icd.d/nvidia_icd.json}"
fi

GAME_MODE=0
if [ -n "$GAMESCOPE_WAYLAND_DISPLAY" ] || [ "$XDG_CURRENT_DESKTOP" = gamescope ]; then GAME_MODE=1; fi
export NFSMW_GAME_MODE="${NFSMW_GAME_MODE:-$GAME_MODE}"
export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-x11}"
# Stick readings once a second (drift reports) and the [latency] lines.
export NFSMW_INPUT_LOG="${NFSMW_INPUT_LOG:-1}" NFSMW_LOG_LATENCY="${NFSMW_LOG_LATENCY:-1}"
say "Game Mode $NFSMW_GAME_MODE, video $SDL_VIDEODRIVER, display ${DISPLAY:-none}${GAMESCOPE_WAYLAND_DISPLAY:+, gamescope $GAMESCOPE_WAYLAND_DISPLAY}"

# exec: Steam's Exit Game stops the game itself (no lifeline needed).
exec "$LOADER" --library-path "$LIBS" --argv0 "$BIN" "$BIN" "$@"
