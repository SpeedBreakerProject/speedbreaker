#!/bin/bash
# Cross-compiles the generated PowerPC translation (ppc/*.cpp) to objects.
# JOBS=N compiles N files at a time (default 1). Finished objects are kept, so a re-run resumes.
cd /home/claude/speedbreaker
mkdir -p /home/claude/ppcobj
compile() {
  f=$1
  o=/home/claude/ppcobj/$(basename "$f" .cpp).o
  [ -s "$o" ] && return 0
  clang++ --target=x86_64-w64-mingw32 -std=c++20 -O2 -march=x86-64-v3 -fno-strict-aliasing -Wno-unused-label -Wno-unused-variable -Wno-everything -include runtime/cpu/indirect.h -include runtime/cpu/guest_time.h -Ippc -Itools/XenonRecomp/XenonUtils -Itools/XenonRecomp/thirdparty/simde -c "$f" -o "$o.tmp" && mv "$o.tmp" "$o" || echo "FAIL $f" >> /home/claude/ppc_fail.log
}
export -f compile
printf '%s\n' ppc/*.cpp | xargs -P "${JOBS:-1}" -I{} bash -c 'compile {}'
echo DONE > /home/claude/ppc_done
