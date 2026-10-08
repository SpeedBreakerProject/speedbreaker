#!/bin/bash
# SpeedBreaker on SteamOS: add this script to Steam as a non-Steam game.
#
# The development build lives in the "nfsmw-build" distrobox (Arch userspace
# with SDL3 and Mesa RADV; see scripts/steamos/README.md). The game runs from
# the repo root (it finds game/files there) and logs to last-run.log (the
# game also keeps its own session logs, crash and hang reports in
# ~/.local/share/speedbreaker/logs; see docs/TESTING.md).
# SDL uses X11 (gamescope's Xwayland), which is how gamescope matches game
# windows to the Steam app that launched them. Extra NFSMW_* settings can go
# in the Steam launch options, e.g. `NFSMW_DEADZONE=0.12 %command%`.
# The log also gets a [latency] line every few seconds (submissions, GPU
# latency, and time the renderer waited for the presenter's queue lock).
ROOT="$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)"
cd "$ROOT" || exit 1
# Game Mode (gamescope, from Steam's environment): the game runs fullscreen at
# the resolution Steam sets for it (Properties > General > Game Resolution;
# Native for the monitor's own resolution).
GAME_MODE=0
if [ -n "$GAMESCOPE_WAYLAND_DISPLAY" ] || [ "$XDG_CURRENT_DESKTOP" = gamescope ]; then GAME_MODE=1; fi
# The game runs under podman, not as this script's child: Steam stopping the
# script wouldn't reach it, so it watches this PID (exec keeps it) and exits
# when it's gone (NFSMW_LIFELINE_PID).
exec distrobox enter nfsmw-build -- env SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-x11}" \
    NFSMW_INPUT_LOG="${NFSMW_INPUT_LOG:-1}" NFSMW_LOG_LATENCY="${NFSMW_LOG_LATENCY:-1}" NFSMW_LIFELINE_PID=$$ \
    NFSMW_GAME_MODE="${NFSMW_GAME_MODE:-$GAME_MODE}" \
    "$ROOT/build/main/runtime/SpeedBreaker" "$@" > "$ROOT/last-run.log" 2>&1
