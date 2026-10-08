#!/bin/bash
# Repeatable in-race benchmark: career -> first race -> pause menu (the full
# 3D scene keeps rendering behind it), then average the [perf] lines logged
# between BENCH_FROM and BENCH_TO seconds. Extra env vars pass through.
#   scripts/bench.sh [label]
cd "$(dirname "$0")/.."
FROM=${BENCH_FROM:-175}
TO=${BENCH_TO:-205}
LOG=build/bench_${1:-run}.log
NFSMW_MUTE=1 NFSMW_INPUT_SCRIPT="8-26/2:START,30-130/4:DOWN,31-131/4:A,32.5-132.5/4:START,145:A,165:START" \
  build/main/runtime/SpeedBreaker > "$LOG" 2>&1 &
PID=$!
START=$(date +%s)
: > "$LOG.perf"
while [ $(( $(date +%s) - START )) -lt "$TO" ]; do
  sleep 1
  if [ $(( $(date +%s) - START )) -ge "$FROM" ]; then
    grep "\[perf\]" "$LOG" | tail -1 >> "$LOG.perf"
  fi
done
kill $PID 2>/dev/null; wait $PID 2>/dev/null
sort -u "$LOG.perf" | python3 -c '
import sys,re
v=[(float(m.group(1)),float(n.group(1)),float(g.group(1))) for l in sys.stdin
   for m in [re.search(r"([\d.]+) fps",l)] for n in [re.search(r"per frame: (\d+) draws",l)] for g in [re.search(r"GPU busy ([\d.]+)",l)] if m and n and g]
if not v: print("no samples"); sys.exit()
print("bench: %.1f fps (min %.1f, max %.1f) | %.0f draws | GPU busy %.1f ms | %d samples" % (
  sum(x[0] for x in v)/len(v), min(x[0] for x in v), max(x[0] for x in v), sum(x[1] for x in v)/len(v), sum(x[2] for x in v)/len(v), len(v)))'
