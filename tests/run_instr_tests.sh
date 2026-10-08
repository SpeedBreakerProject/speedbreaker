#!/usr/bin/env bash
# Instruction tests for our XenonRecomp branch.
#
#   tests/run_instr_tests.sh [test-name ...]
#
# Assembles Xenia's PPC instruction tests (fetched into tools/xenia-tests) plus
# our own (tests/instr/*.s, same format) with LLVM, recompiles them with
# XenonRecomp's test mode, builds XenonTests natively, and runs it. Any line
# the test binary prints is a failed register check.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [ -z "${LLVM:-}" ]; then
  if [ -x /opt/homebrew/opt/llvm/bin/clang ]; then LLVM=/opt/homebrew/opt/llvm/bin
  else LLVM="$(dirname "$(command -v clang)")"; fi
fi
# The linker: LLD=, else LLVM's own, else the one on PATH (Homebrew's lld
# formula puts it in /opt/homebrew/bin; Linux distributions in /usr/bin).
if [ -z "${LLD:-}" ]; then
  if [ -x "$LLVM/ld.lld" ]; then LLD="$LLVM/ld.lld"
  elif command -v ld.lld > /dev/null; then LLD="$(command -v ld.lld)"
  else LLD=/opt/homebrew/bin/ld.lld; fi
fi
for t in "$LLVM/clang" "$LLVM/llvm-mc" "$LLVM/llvm-objdump" "$LLD"; do
  [ -n "$t" ] && [ -x "$t" ] || { echo "missing ${t:-ld.lld}: needs LLVM's clang, llvm-mc and llvm-objdump (LLVM=<bin dir>) and lld (LLD=<ld.lld>)"; exit 1; }
done
XR="$ROOT/tools/XenonRecomp"
WORK="$ROOT/tools/instr-tests"
rm -rf "$WORK" && mkdir -p "$WORK/bin"

# Always test the current source: rebuild just the tools (not XenonTests, whose
# generated sources come and go), and stop if that fails.
ninja -C "$XR/build" XenonRecomp XenonAnalyse > "$WORK.build.log" 2>&1 || { cat "$WORK.build.log"; echo "XenonRecomp build failed"; exit 1; }

# Xenia's tests use gas syntax; one directory holds both sources, bin/ the objects.
cp "$ROOT"/tools/xenia-tests/*.s "$WORK"/ 2>/dev/null || true
cp "$ROOT"/tests/instr/*.s "$WORK"/ 2>/dev/null || true
for s in "$WORK"/*.s; do
  [ -s "$s" ] || { echo "EMPTY TEST FILE: $(basename "$s") (rerun scripts/setup_tools.sh)"; exit 1; }
done
if [ $# -gt 0 ]; then
  for f in "$WORK"/*.s; do
    keep=0; for t in "$@"; do [ "$(basename "$f" .s)" = "$t" ] && keep=1; done
    [ $keep = 1 ] || rm "$f"
  done
fi

fail_asm=0
for s in "$WORK"/*.s; do
  n=$(basename "$s" .s)
  # LLVM wants %-prefixed register names; Xenia's tests use bare ones.
  perl -pe 'next if /^\s*#/; s/\b(r|v|f|cr)(\d+)\b/%$1$2/g unless /:\s*$/' "$s" > "$WORK/bin/$n.llvm.s"
  # XenonUtils' ELF loader wants a *linked* 32-bit big-endian image (like
  # Xenia's own test build), so assemble for ppc32 and link low at 0x10000 (the harness masks addresses to 20 bits).
  if ! "$LLVM"/llvm-mc -triple=powerpc-unknown-unknown -mcpu=pwr7 -mattr=+altivec \
       -filetype=obj -o "$WORK/bin/$n.obj" "$WORK/bin/$n.llvm.s" 2> "$WORK/bin/$n.asm.log"; then
    echo "ASSEMBLE FAILED: $n ($(head -1 "$WORK/bin/$n.asm.log"))"; rm -f "$s"; fail_asm=1; continue
  fi
  # The harness only picks up *.o, so the linked executable takes that name.
  "$LLD" -o "$WORK/bin/$n.o" -e 0 --image-base=0 -Ttext=0x10000 "$WORK/bin/$n.obj"
  "$LLVM"/llvm-objdump -d "$WORK/bin/$n.o" > "$WORK/bin/$n.dis"
done

# Recompile into the XenonTests project directory (it globs *.cpp).
rm -f "$XR"/XenonTests/*.cpp
"$XR"/build/XenonRecomp/XenonRecomp "$WORK/bin" "$XR/XenonTests" | grep -v "^Recompiling" || true

cmake -Wno-deprecated -S "$XR" -B "$XR/build-tests" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$LLVM/clang" -DCMAKE_CXX_COMPILER="$LLVM/clang++" \
  "-DCMAKE_CXX_FLAGS=-include cstdlib" > /dev/null
ninja -C "$XR/build-tests" XenonTests 2>&1 | grep -E "error|FAILED" | head -20 || true
echo "---- running ----"
"$XR/build-tests/XenonTests/XenonTests" | tee "$WORK/results.txt"
echo "---- $(wc -l < "$WORK/results.txt" | tr -d ' ') failed checks ----"
