#!/bin/bash
# The whole game freezes while the iOS app is inactive (video::SuspendGame):
# a Mac run with no device and no input. NFSMW_TEST_SUSPEND makes the app
# go inactive on time during the attract movie (the same app events, each
# delivered twice, as UIKit does), the process is SIGSTOPped inside the
# long suspension as iOS stops a backgrounded app, and the log is checked
# against the pass criteria. Extra env vars pass through.
#   scripts/test_suspend.sh [label]
#   SUSPEND="30+0.2,40+6,55+12b"  the windows: <at>+<s>[b] ("b": in the
#                                 background too). SUSPEND= (empty): none, a
#                                 plain run (each run is its own yardstick)
#   MAX_SECS=130                  how long to wait for the attract movie's
#                                 [movie] line
#   MOVIE_FRAMES=900              the fewest frames that line has (the intro
#                                 movies are shorter)
#   NFSMW_TEST_SUSPEND_GPU_ONLY=1 the red baseline: only the GPU held, as
#                                 before 2026-10-02 (criteria 4 and 6 fail)
#   NFSMW_MUTE=0                  opens SDL's dummy audio device (criterion 7
#                                 and the audio queue need one)
#   SDL_VIDEO_DRIVER=             a real window instead of SDL's offscreen one
# Only a run with a window has the GPU gate the bug came through: under
# NFSMW_HEADLESS=1 the switch calls SuspendGame/ResumeGame itself, which
# checks the clocks alone (on today's tree nothing stalls headless).
# Output in build/suspend_<label>/: run.log (stdout and stderr), session.log
# (the same with session times), gametime.csv, threads_{a,b}.txt.
cd "$(dirname "$0")/.." || exit 1
LABEL=${1:-run}
OUT=build/suspend_$LABEL
LOG=$OUT/run.log
SUSPEND=${SUSPEND-30+0.2,40+6,55+12b}
MAX_SECS=${MAX_SECS:-130}
MOVIE_FRAMES=${MOVIE_FRAMES:-900}  # the attract movie's [movie] line (the intro movies are shorter)
BIN=${BIN:-build/main/runtime/SpeedBreaker}
# One sandbox HOME for every run, so the shader caches stay warm between the
# baseline and the test (a cold cache has long frames of its own).
HOME_DIR=${SUSPEND_HOME:-build/suspend_home}
mkdir -p "$HOME_DIR" && HOME_DIR=$(cd "$HOME_DIR" && pwd)
rm -rf "$OUT" && mkdir -p "$OUT" && OUT=$(cd "$OUT" && pwd) && LOG=$OUT/run.log
touch "$OUT/start.stamp"

# Per-thread CPU time (ns, user + system, with the thread's name), from
# libproc: what each thread spent between two samples, not ps's decaying %CPU.
sample_threads() {
  python3 - "$1" > "$2" <<'EOF'
import ctypes, ctypes.util, sys, time
lib = ctypes.CDLL(ctypes.util.find_library('proc') or '/usr/lib/libSystem.B.dylib', use_errno=True)
lib.proc_pidinfo.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_int]
class Info(ctypes.Structure):  # struct proc_threadinfo
    _fields_ = [('user', ctypes.c_uint64), ('system', ctypes.c_uint64)] + \
        [(n, ctypes.c_int32) for n in ('cpu', 'policy', 'state', 'flags', 'sleep', 'curpri', 'pri', 'maxpri')] + \
        [('name', ctypes.c_char * 64)]
pid = int(sys.argv[1])
handles = (ctypes.c_uint64 * 4096)()
n = lib.proc_pidinfo(pid, 6, 0, handles, ctypes.sizeof(handles))  # PROC_PIDLISTTHREADS
if n <= 0:
    sys.exit('proc_pidinfo(PROC_PIDLISTTHREADS): errno %d' % ctypes.get_errno())
print('%.6f' % time.monotonic())
for h in handles[:n // 8]:
    info = Info()
    if lib.proc_pidinfo(pid, 5, h, ctypes.byref(info), ctypes.sizeof(info)) == ctypes.sizeof(info):  # PROC_PIDTHREADINFO
        print('%d\t%d\t%s' % (h, info.user + info.system, info.name.decode(errors='replace') or '?'))
EOF
}

# The windows, sorted by start: "<at> <secs>" per line.
WINDOWS=$(echo "$SUSPEND" | tr ',' '\n' | sed -n 's/^ *\([0-9.]*\)+\([0-9.]*\)b\{0,1\} *$/\1 \2/p' | sort -n)
COUNT=$(echo -n "$WINDOWS" | grep -c .)
# The longest one hosts the CPU samples and the SIGSTOP: which suspension it is, and how long.
read -r LONG_INDEX LONG_SECS <<< "$(echo "$WINDOWS" | awk 'NF { i++; if ($2 > best) { best = $2; at = i } } END { print at + 0, best + 0 }')"
LAST_END=$(echo "$WINDOWS" | awk 'NF { e = $1 + $2; if (e > m) m = e } END { print int(m) + 1 }')

ARGS=()
[ -n "$SUSPEND" ] && ARGS+=(NFSMW_TEST_SUSPEND="$SUSPEND")
# Its own HOME (and XDG folders, which Linux uses first): never the
# player's settings, saves or caches.
env HOME="$HOME_DIR" XDG_DATA_HOME="$HOME_DIR/.local/share" XDG_CACHE_HOME="$HOME_DIR/.cache" \
  XDG_CONFIG_HOME="$HOME_DIR/.config" NFSMW_GAME_DIR="${NFSMW_GAME_DIR:-$PWD/game/files}" NFSMW_MUTE="${NFSMW_MUTE:-1}" \
  SDL_VIDEO_DRIVER="${SDL_VIDEO_DRIVER-offscreen}" SDL_AUDIO_DRIVER="${SDL_AUDIO_DRIVER:-dummy}" \
  NFSMW_HANG_SECS="${NFSMW_HANG_SECS:-4,3}" NFSMW_AUDIO_LOG=1 NFSMW_GAME_TIME_LOG="$OUT/gametime.csv" "${ARGS[@]}" \
  "$BIN" > "$LOG" 2>&1 &
PID=$!
# Interrupted (Ctrl-C), the game goes too, and isn't left stopped.
trap 'kill -CONT "$PID" 2>/dev/null; kill -9 "$PID" 2>/dev/null' EXIT
trap 'exit 130' INT TERM
START=$SECONDS
elapsed() { echo $(( SECONDS - START )); }
alive() { kill -0 "$PID" 2>/dev/null; }
# Until the log has `count` lines matching `pattern`, the game ends or MAX_SECS.
wait_for() {
  while alive && [ "$(elapsed)" -lt "$MAX_SECS" ]; do
    [ "$(grep -c -- "$1" "$LOG")" -ge "$2" ] && return 0
    sleep 0.5
  done
  return 1
}

STEPS=""
if [ "$COUNT" -gt 0 ] && awk "BEGIN { exit !($LONG_SECS >= 11) }"; then
  # Inside the longest suspension: two samples 4 s apart once it has
  # settled, then 4 s stopped (under the watchdog's 5 s tick-gap rule).
  if wait_for '\[suspend\] \(suspended:\|GPU only: held\)' "$LONG_INDEX"; then
    sleep 1.5
    sample_threads "$PID" "$OUT/threads_a.txt"
    sleep 4
    sample_threads "$PID" "$OUT/threads_b.txt"
    kill -STOP "$PID"; sleep 4; kill -CONT "$PID"
    STEPS="SIGSTOP for 4 s inside suspension $LONG_INDEX"
  else
    STEPS="suspension $LONG_INDEX never started: no CPU samples, no SIGSTOP"
  fi
elif [ "$COUNT" -gt 0 ]; then
  STEPS="no suspension of 11 s or more: no CPU samples, no SIGSTOP"
fi

# Then until the attract movie's [movie] line (logged at the first frame
# after it), and the last audit 2 s after the last resume.
while alive && [ "$(elapsed)" -lt "$MAX_SECS" ]; do
  if grep -oE '\[movie\] [0-9]+ frames' "$LOG" | awk -v min="$MOVIE_FRAMES" '$2 + 0 >= min { f = 1 } END { exit !f }' &&
    [ "$(elapsed)" -ge $(( LAST_END + 3 )) ]; then
    sleep 1
    break
  fi
  sleep 1
done
ALIVE=1
alive || ALIVE=0
{
  kill "$PID"
  for _ in 1 2 3 4 5 6 7 8 9 10; do alive || break; sleep 0.3; done
  kill -9 "$PID"
  wait "$PID"
} 2>/dev/null
SESSION=$(find "$HOME_DIR" -path '*/logs/*.log' -newer "$OUT/start.stamp" 2>/dev/null | head -1)
[ -n "$SESSION" ] && cp "$SESSION" "$OUT/session.log"

trap - EXIT
OUT="$OUT" LOG="$LOG" SUSPEND="$SUSPEND" MOVIE_FRAMES="$MOVIE_FRAMES" ALIVE="$ALIVE" \
STEPS="$STEPS" ELAPSED="$(elapsed)" python3 - <<'EOF'
import os, re, sys
env = os.environ
lines = open(env['LOG'], errors='replace').read().splitlines()
results = []
def report(name, status, detail):
    results.append(status)
    print('%-4s %s: %s' % (status, name, detail))
def first(pattern):
    return next((i for i, l in enumerate(lines) if re.search(pattern, l)), None)
def count(pattern):
    return sum(1 for l in lines if re.search(pattern, l))

windows = []
for w in filter(None, env['SUSPEND'].split(',')):
    m = re.fullmatch(r'\s*([\d.]+)\+([\d.]+)(b?)\s*', w)
    if m and float(m.group(2)) > 0:
        windows.append((float(m.group(1)), float(m.group(2)), m.group(3) == 'b'))
windows.sort()
arm = next((l for l in lines if 'NFSMW_TEST_SUSPEND=' in l), '')
headless = '; headless' in arm
windowed = first(r'\[video\] .*, swapchain \d+x\d+') is not None
print('suspend test %s: %d window(s) %s, %s, %s; ran %s s%s' % (os.path.basename(env['OUT']), len(windows), env['SUSPEND'] or '(baseline)',
    'headless (SuspendGame/ResumeGame called directly)' if headless else 'app events through the main loop',
    'windowed' if windowed else 'no window', env['ELAPSED'], '; ' + env['STEPS'] if env['STEPS'] else ''))

report('0 ran', 'PASS' if env['ALIVE'] == '1' else 'FAIL',
       'still running when stopped' if env['ALIVE'] == '1' else 'the game had exited before the end (see run.log)')

# 1. Every event delivered twice, each handled once.
suspended = [l for l in lines if '[suspend] suspended:' in l]
resumed = [l for l in lines if '[suspend] resumed after' in l]
if not windows:
    report('1 deliveries', 'SKIP', 'no NFSMW_TEST_SUSPEND')
else:
    if headless:
        got, want, what = count(r'NFSMW_TEST_SUSPEND: call '), 4 * len(windows), 'calls'
    else:
        got, want, what = count(r'NFSMW_TEST_SUSPEND: deliver '), sum(2 * (4 if b else 2) for _, _, b in windows), 'deliveries'
    ok = got == want and len(suspended) == len(windows) and len(resumed) == len(windows)
    report('1 deliveries', 'PASS' if ok else 'FAIL', '%d %s (%d expected), %d suspended, %d resumed (%d each expected)' % (
        got, what, want, len(suspended), len(resumed), len(windows)))

# 2. Nothing moved while suspended.
rx = re.compile(r'resumed after ([\d.]+) s: guest time ([-+][\d.]+) s, mftb ([-+]\d+) ticks, system time ([-+][\d.]+) s; '
                r'meanwhile (\d+) vblanks, (\d+) audio frames, (\d+) timer firings')
if not resumed:
    report('2 frozen', 'SKIP' if not windows else 'FAIL', 'no [suspend] resumed lines')
for i, l in enumerate(resumed):
    m = rx.search(l)
    if not m:
        report('2 frozen', 'FAIL', 'unparsed: ' + l.strip())
        continue
    after, guest, mftb, system, vb, af, tf = float(m[1]), float(m[2]), int(m[3]), float(m[4]), int(m[5]), int(m[6]), int(m[7])
    ok = abs(guest) <= 0.001 and abs(system) <= 0.001 and vb <= 1 and af <= 1 and tf <= 1
    report('2 frozen #%d' % (i + 1), 'PASS' if ok else 'FAIL', 'after %.3f s: guest %+.6f s, system %+.6f s, mftb %+d ticks; '
           '%d vblanks, %d audio frames, %d timer firings (<= 1 each)' % (after, guest, system, mftb, vb, af, tf))

# 3. 2 s after each resume: guest time runs with the host's, no catch-up.
rx = re.compile(r'([\d.]+) s after resuming: guest ([\d.]+) s, mftb ([\d.]+) s \(host ([\d.]+) s\), (\d+) vblanks \(([\d.]+) expected\), '
                r'(\d+) audio frames \(([\d.]+) expected\), audio queued ([\d.]+) ms')
audits = [l for l in lines if 's after resuming:' in l]
# Audio frames per guest second: between the first two suspensions (both
# after the render client registered, which boot delays by 0.3-1.2 s), else
# from the session's start to the first.
stops = [(float(m[1]), int(m[2])) for m in (re.search(r'clocks stop at guest ([\d.]+) s \(vblank \d+, audio frame (\d+)\)', l)
                                             for l in lines if '[suspend] suspended:' in l) if m]
if len(stops) >= 2 and stops[1][0] - stops[0][0] > 1:
    audio_rate = (stops[1][1] - stops[0][1]) / (stops[1][0] - stops[0][0])
elif stops and stops[0][0] > 0:
    audio_rate = stops[0][1] / stops[0][0]
else:
    audio_rate = None
want = sum(1 for i, (at, secs, _) in enumerate(windows) if i + 1 == len(windows) or windows[i + 1][0] - (at + secs) > 2.2)
if not windows:
    report('3 audit', 'SKIP', 'no NFSMW_TEST_SUSPEND')
elif len(audits) < want:
    report('3 audit', 'FAIL', '%d audit lines, %d expected (a suspension within 2 s of a resume skips its audit)' % (len(audits), want))
for i, l in enumerate(audits):
    m = rx.search(l)
    if not m:
        report('3 audit', 'FAIL', 'unparsed: ' + l.strip())
        continue
    guest, mftb, host, vb, vbx, af, afx, queued = float(m[2]), float(m[3]), float(m[4]), int(m[5]), float(m[6]), int(m[7]), float(m[8]), float(m[9])
    # The audio cadence follows the device's clock (SDL's dummy device runs
    # ~3% slow): expect the rate this run had before its first suspension.
    if audio_rate:
        afx = audio_rate * guest
    # Vblanks +-2: a timer tick late on a busy machine moves one across the
    # audit's 2 s edge (119 for 120.1 seen once); a catch-up adds dozens.
    ok = abs(guest - host) <= 0.010 and abs(mftb - host) <= 0.010 and abs(vb - vbx) <= 2 and abs(af - afx) <= 4 + 0.01 * afx
    report('3 audit #%d' % (i + 1), 'PASS' if ok else 'FAIL', 'host %.3f s: guest %.3f s, mftb %.3f s (+-0.010); %d vblanks (%.1f, +-2), '
           '%d audio frames (%.1f at this run\'s rate, +-4+1%%), queued %.1f ms' % (host, guest, mftb, vb, vbx, af, afx, queued))

# 4. The attract movie isn't caught up after a resume: its frames on one
# vblank in the 180 vblanks (3 s) after each resume, against this run's own
# rate in the movie before its first suspension. Run to run that rate varies
# a lot (10-77 of 2296 frames in two baselines), so no other run is the
# yardstick. The positions are guest vblanks: gametime.csv's running sum of
# its vblanks column, and the vblank each suspension stopped at (in the red
# baseline, NFSMW_TEST_SUSPEND_GPU_ONLY=1, the "GPU only" lines).
def attract():
    best = None
    for i, l in enumerate(lines):
        m = re.search(r'\[movie\] (\d+) frames: (\d+) on 1 vblank', l)
        if m and int(m[1]) >= int(env['MOVIE_FRAMES']) and (best is None or int(m[1]) > best[0]):
            best = (int(m[1]), int(m[2]), i)
    return best
movie = attract()
csv = os.path.join(env['OUT'], 'gametime.csv')
frames = []  # (guest vblank after the frame, its vblanks)
if os.path.exists(csv):
    total = 0
    for r in open(csv).read().splitlines()[1:]:
        f = r.split(',')
        if len(f) > 1 and f[1].isdigit():
            total += int(f[1])
            frames.append((total, int(f[1])))
held = [int(m[1]) for m in (re.search(r'\[suspend\] (?:suspended: .*\(vblank|GPU only: held at vblank) (\d+)', l) for l in lines) if m]
released = [int(m[1]) for m in (re.search(r'\[suspend\] GPU only: released at vblank (\d+)', l) for l in lines) if m]
resumes = released or held  # the clocks frozen: a resume is at the vblank its suspension stopped at
if not movie:
    report('4 movie', 'FAIL' if windows else 'SKIP', 'no [movie] line of %s+ frames (the attract movie; MAX_SECS to wait longer)' % env['MOVIE_FRAMES'])
elif not windows or not resumes or not frames:
    report('4 movie', 'SKIP', '%d frames, %d on 1 vblank (no suspension, or no gametime.csv)' % movie[:2])
else:
    ones = lambda a, b: sum(1 for v, n in frames if a <= v < b and n == 1)
    pre_end = held[0] if held else resumes[0]
    pre_start = pre_end - 600  # the 10 s of movie before the first suspension (it starts at ~20 s, the first window at 30 s)
    rate = ones(pre_start, pre_end) / 600 * 180
    limit = 2.5 * rate + 6
    for k, at in enumerate(resumes):
        n = ones(at, at + 180)
        report('4 movie #%d' % (k + 1), 'PASS' if n <= limit else 'FAIL', '%d frames on 1 vblank in the 3 s after the resume at vblank %d; '
               'before the first suspension %.1f per 3 s (at most %.1f)' % (n, at, rate, limit))

# 5. No false hang, stall or long frame once the first suspension starts
# (a cold start has long frames of its own).
start = first(r'\[suspend\] suspended:')
if start is None:
    report('5 no stalls', 'SKIP', 'no suspension')
else:
    bad = [l.strip() for l in lines[start:] if re.search(r'\[hang\]|without a new frame|WAIT_REG_MEM stuck|vblanks skipped', l)
           or re.search(r'\[hitch\] frame .* took [0-9]{4,}\.', l)]
    report('5 no stalls', 'FAIL' if bad else 'PASS', '; '.join(b[:160] for b in bad[:3]) if bad else
           'no [hang], "without a new frame", "WAIT_REG_MEM stuck" or [hitch] of 1 s+ after the first suspension')

# 6. No game frame spans a suspension's vblanks (the first row counts from boot).
csv = os.path.join(env['OUT'], 'gametime.csv')
rows = open(csv).read().splitlines()[2:] if os.path.exists(csv) else []
if not rows:
    report('6 frames', 'SKIP', 'no gametime.csv rows')
else:
    long = [(n + 3, r) for n, r in enumerate(rows) if len(r.split(',')) > 1 and r.split(',')[1].isdigit() and int(r.split(',')[1]) > 120]
    report('6 frames', 'FAIL' if long else 'PASS', ('rows over 120 vblanks: ' + ', '.join('line %d (%s)' % x for x in long[:5])) if long else
           '%d frames, none over 120 vblanks' % len(rows))

# 7. No audio gap after the first resume.
start = first(r'\[suspend\] resumed after')
audio = [l for l in lines[start:]] if start is not None else []
gaps = [re.search(r'(\d+) gaps > 50 ms', l) for l in audio if '[audio] frames:' in l]
gaps = [int(m[1]) for m in gaps if m]
if start is None or not gaps:
    report('7 audio', 'SKIP', 'no [audio] frames lines after a resume (NFSMW_MUTE=1 opens no device: NFSMW_MUTE=0 uses SDL\'s dummy one)')
else:
    report('7 audio', 'PASS' if not any(gaps) else 'FAIL', '%d [audio] lines after the first resume, gaps > 50 ms: %s' % (len(gaps), gaps))

# 8. The first [perf] after each resume inside the attract movie runs at the movie's rate.
if not movie or not (resumed or any('[suspend] GPU only: released' in l for l in lines)):
    report('8 movie fps', 'SKIP', 'no attract movie line or no resume')
else:
    prev = max((i for i, l in enumerate(lines[:movie[2]]) if '[movie] ' in l and ' frames:' in l), default=-1)
    # The movie's clock is the audio played, so its rate follows the audio
    # device's (SDL's dummy device runs ~3% slow): 29.97 fps scaled by this
    # run's audio rate before its first suspension (256-sample frames at 48 kHz).
    pre_fps = 29.97 * audio_rate / 187.5 if audio_rate else 29.97
    checked = 0
    for i, l in enumerate(lines):
        if not ('[suspend] resumed after' in l or '[suspend] GPU only: released' in l) or not prev < i < movie[2]:
            continue
        perf = next((p for p in lines[i + 1:movie[2]] if '[perf]' in p), None)
        m = perf and re.search(r'\[perf\] ([\d.]+) fps', perf)
        if not m:
            continue
        checked += 1
        fps = float(m[1])
        lo, hi = pre_fps - 1.5, pre_fps + 1.5
        report('8 movie fps', 'PASS' if lo <= fps <= hi else 'FAIL', 'first [perf] after the resume at line %d: %.1f fps '
               '(the movie at this run\'s audio rate: %.1f, +-1.5)' % (i + 1, fps, pre_fps))
    if not checked:
        report('8 movie fps', 'SKIP', 'no resume with a [perf] line before the attract movie ended')

# 9. Nothing spins while suspended: per-thread CPU between two samples.
a, b = os.path.join(env['OUT'], 'threads_a.txt'), os.path.join(env['OUT'], 'threads_b.txt')
def load(path):
    text = open(path).read().splitlines() if os.path.exists(path) else []
    if not text:
        return None, {}
    threads = {}
    for t in text[1:]:
        h, ns, name = t.split('\t', 2)
        threads[h] = (int(ns), name)
    return float(text[0]), threads
ta, sa = load(a)
tb, sb = load(b)
if ta is None or tb is None:
    report('9 cpu', 'SKIP', 'no thread samples (%s)' % (env['STEPS'] or 'no suspension'))
else:
    wall = (tb - ta) * 1e9
    use = sorted(((sb[h][0] - sa[h][0]) / wall * 100, sb[h][1]) for h in sb if h in sa)[::-1]
    busy = [u for u in use if u[0] > 5.0]
    report('9 cpu', 'FAIL' if busy else 'PASS', 'over %.1f s, %d threads; busiest: %s' % (
        wall / 1e9, len(use), ', '.join('%s %.1f%%' % (n, p) for p, n in use[:5])))

failed, skipped = results.count('FAIL'), results.count('SKIP')
print('suspend test: %d passed, %d failed, %d skipped (log: %s)' % (results.count('PASS'), failed, skipped, env['LOG']))
sys.exit(1 if failed else 0)
EOF
