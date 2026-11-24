#!/bin/bash
# The SteamOS launcher's Turnip part (scripts/steamos/bundle/speedbreaker.sh:
# turnip_sysmem, run alone). Registered by tests/CMakeLists.txt, or:
#   bash tests/launcher_turnip_test.sh scripts/steamos/bundle/speedbreaker.sh
# Each case: the machine (uname -m) and what the user set; then whether
# TU_DEBUG reaches the game and what the log says. arm64 (the Steam Frame)
# gets TU_DEBUG=sysmem; x86-64 gets nothing and no line; a TU_DEBUG the user
# set (even empty) is left as it is; NFSMW_TURNIP_SYSMEM=0 turns it off.
set -u
LAUNCHER=${1:?usage: launcher_turnip_test.sh <speedbreaker.sh>}
[ -f "$LAUNCHER" ] || { echo "no $LAUNCHER"; exit 1; }
# The function, as the launcher has it (from "turnip_sysmem() {" to its "}").
FUNCS="$(awk '/^turnip_sysmem\(\) \{$/ { on = 1 } on { print } on && /^\}$/ { on = 0 }' "$LAUNCHER")"
case "$FUNCS" in *"turnip_sysmem() {"*) ;; *) echo "FAIL: turnip_sysmem not found in $LAUNCHER"; exit 1 ;; esac
grep -qx 'turnip_sysmem' "$LAUNCHER" || { echo "FAIL: $LAUNCHER never calls turnip_sysmem"; exit 1; }

failures=0
cases=0

# run <name> <uname -m> [VAR=value ...]: the function in a clean environment (no TU_* or NFSMW_* variables but
# those given). Sets OUT (the log) and ENVS (TU_DEBUG as the game would get it).
run() {
    local name=$1 arch=$2; shift 2
    OUT="$(env -i PATH="$PATH" ARCH="$arch" "$@" "$BASH" -c '
        say() { echo "[launcher] $*"; }
        uname() { [ "${1:-}" = -m ] && echo "$ARCH" || command uname "$@"; }
        eval "$1"
        turnip_sysmem
        echo "ENV TU_DEBUG=${TU_DEBUG-<unset>}"
    ' bash "$FUNCS" 2>&1)"
    ENVS="$(echo "$OUT" | sed -n 's/^ENV //p')"
    OUT="$(echo "$OUT" | grep -v '^ENV ')"
    cases=$((cases + 1))
    printf '%s\n  log: %s\n  env: %s\n' "$name" "${OUT:-(none)}" "$ENVS"
}
check() {  # check <description> <shell test...>
    local what=$1; shift
    if ! "$@"; then failures=$((failures + 1)); echo "  FAIL: $what"; fi
}

# 1. The Steam Frame (arm64): sysmem, one line.
run "Steam Frame (arm64)" aarch64
check "sysmem" [ "$ENVS" = "TU_DEBUG=sysmem" ]
check "the line" [ "$OUT" = "[launcher] graphics: TU_DEBUG=sysmem (Turnip draws straight to memory, not through its tile memory; NFSMW_TURNIP_SYSMEM=0 turns this off)" ]
run "Steam Frame, NFSMW_TURNIP_SYSMEM=1" aarch64 NFSMW_TURNIP_SYSMEM=1
check "sysmem" [ "$ENVS" = "TU_DEBUG=sysmem" ]

# 2. x86-64 (Deck, Steam Machine, PCs): nothing set, no line, whatever is set.
run "Steam Deck / Steam Machine (x86-64)" x86_64
check "nothing set" [ "$ENVS" = "TU_DEBUG=<unset>" ]; check "no line" [ -z "$OUT" ]
run "x86-64, NFSMW_TURNIP_SYSMEM=0" x86_64 NFSMW_TURNIP_SYSMEM=0
check "nothing set" [ "$ENVS" = "TU_DEBUG=<unset>" ]; check "no line" [ -z "$OUT" ]
run "x86-64, TU_DEBUG=gmem set" x86_64 TU_DEBUG=gmem
check "kept" [ "$ENVS" = "TU_DEBUG=gmem" ]; check "no line" [ -z "$OUT" ]

# 3. The off switch and the user's own values win.
run "Steam Frame, NFSMW_TURNIP_SYSMEM=0" aarch64 NFSMW_TURNIP_SYSMEM=0
check "nothing set" [ "$ENVS" = "TU_DEBUG=<unset>" ]
check "says so" [ "$OUT" = "[launcher] graphics: NFSMW_TURNIP_SYSMEM=0: Turnip chooses between its tile memory and memory itself" ]
run "Steam Frame, TU_DEBUG=gmem set" aarch64 TU_DEBUG=gmem
check "kept" [ "$ENVS" = "TU_DEBUG=gmem" ]
check "says so" [ "$OUT" = "[launcher] graphics: TU_DEBUG=gmem was already set: left as it is" ]
run "Steam Frame, TU_DEBUG= (empty) set" aarch64 TU_DEBUG=
check "kept empty" [ "$ENVS" = "TU_DEBUG=" ]
check "says so" [ "$OUT" = "[launcher] graphics: TU_DEBUG= was already set: left as it is" ]
run "Steam Frame, TU_DEBUG=perf,sysmem set" aarch64 TU_DEBUG=perf,sysmem
check "kept" [ "$ENVS" = "TU_DEBUG=perf,sysmem" ]

echo
if [ $failures = 0 ]; then echo "all passed ($cases cases)"; else echo "$failures failure(s) in $cases cases"; fi
[ $failures = 0 ]
