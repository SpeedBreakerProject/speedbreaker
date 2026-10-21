#!/bin/bash
cd /home/claude/speedbreaker
mkdir -p /home/claude/ppcobj
for f in ppc/*.cpp; do
  o=/home/claude/ppcobj/$(basename $f .cpp).o
  [ -s $o ] && continue
  clang++ --target=x86_64-w64-mingw32 -std=c++20 -O2 -march=x86-64-v3 -fno-strict-aliasing -Wno-unused-label -Wno-unused-variable -Wno-everything -include runtime/cpu/indirect.h -include runtime/cpu/guest_time.h -Ippc -Itools/XenonRecomp/XenonUtils -Itools/XenonRecomp/thirdparty/simde -c $f -o $o.tmp && mv $o.tmp $o || echo "FAIL $f" >> /home/claude/ppc_fail.log
done
echo DONE > /home/claude/ppc_done
