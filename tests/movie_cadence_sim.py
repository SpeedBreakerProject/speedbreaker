#!/usr/bin/env python3
# Movie frames against the guest vblank (game/movie_pacing.cpp), simulated.
#   python3 tests/movie_cadence_sim.py
#
# Model (ms): frame n's swap reaches the command processor at
# n * F + scatter[n], no sooner than 4 ms after the last one went out (D3D
# holds the game thread until then, and it renders for ~4 ms). The scatter is
# the XMV player's, measured: 4 s of the attract movie (Steam Machine, nested
# gamescope at 60 Hz, the diag timeline), each kick's distance from a straight
# line, replayed in random blocks of 10. A swap goes out at the first vblank
# after it (interval ONE), or, held, a vblank later when it would go out one
# vblank after the last one and came less than G before that vblank.
# Prints, per movie rate against vblank rate and guard, the frames shown for
# 1 and 3 vblanks, adjacent 1-3 pairs, and how long after its ideal time a
# frame goes out on average. The right answer is only the 3s (a slow movie)
# or 1s (a fast one) the two rates force.
import math, random, statistics

SCATTER = [
    -4.1, -3.5, -3.2, -2.3, -2.5, 2.0, 2.2, 2.5, 3.8, 3.3, 1.9, 3.0, 2.8, 2.7, 3.3, -2.6, -1.1, -1.6, -1.4, -0.0,
    -1.9, -2.2, -0.9, -1.2, -0.6, -1.1, -1.2, -0.2, 0.2, -1.3, 0.2, 0.3, -0.2, 0.8, 0.3, 1.6, 0.9, 1.4, 1.7, -4.7,
    -4.7, -4.1, -3.4, -4.0, -2.6, 2.3, 3.5, 3.8, 3.1, 2.6, 2.0, 2.3, 3.1, 3.4, -2.2, -2.0, -2.5, -1.9, -1.0, -1.6,
    -0.6, 0.2, -0.3, 1.1, 0.4, -0.4, 0.1, -0.6, 0.7, 0.8, -1.1, 0.1, 0.1, 0.4, 1.3, 1.0, 1.7, 1.8, 2.1, 1.6,
    1.9, 2.0, 3.0, 3.1, -2.6, -3.3, -2.9, -1.4, -2.0, -1.6, 4.7, 4.4, 4.1, 5.8, -1.8, -0.2, -0.4, -0.9, 0.6, 1.0,
    -1.9, -0.3, -0.3, -0.4, 0.2, -0.8, 0.8, 1.2, 0.6, -5.2, -4.9, -5.6, -4.0, -3.9, 0.8, 1.0, 1.3, 1.5, 2.0, 1.1,
]
WORK_MS = 4.0


def run(frame_ms, vblank_ms, guard_ms, frames=20000, seed=1):
    rng = random.Random(seed)
    scatter = []
    while len(scatter) < frames:
        i = rng.randrange(len(SCATTER) - 10)
        scatter += SCATTER[i:i + 10]
    last = None  # vblank index the last frame went out at
    spans, delays = [], []
    for n in range(frames):
        ideal = 1000.0 + n * frame_ms
        arrival = ideal + scatter[n]
        if last is not None:
            arrival = max(arrival, last * vblank_ms + WORK_MS)
        out = math.floor(arrival / vblank_ms) + 1
        if last is not None:
            if guard_ms > 0 and out == last + 1 and arrival >= out * vblank_ms - guard_ms:
                out += 1
            spans.append(out - last)
        delays.append(out * vblank_ms - ideal)
        last = out
    ones = sum(1 for s in spans if s == 1) / len(spans)
    threes = sum(1 for s in spans if s >= 3) / len(spans)
    pairs = sum(1 for a, b in zip(spans, spans[1:]) if {a, b} == {1, 3})
    return ones, threes, pairs, statistics.mean(delays)


for name, fps, hz in [("29.97 fps at 60 Hz", 29.97, 60.0), ("29.97 fps at 60.16 Hz (Deck panel)", 29.97, 60.16),
                      ("30.3 fps at 60 Hz (headless disk audio)", 30.3, 60.0), ("30 fps at 60 Hz", 30.0, 60.0)]:
    frame_ms, vblank_ms = 1000.0 / fps, 1000.0 / hz
    need = frame_ms / vblank_ms - 2
    print(f"{name}: the rates force {100 * need if need > 0 else 0:.2f}% 3s, {-100 * need if need < 0 else 0:.2f}% 1s")
    for guard in (0, 4, 6, 8, 10, 12):
        r = [run(frame_ms, vblank_ms, guard, seed=s) for s in range(4)]
        ones, threes, pairs, delay = (statistics.mean(x[i] for x in r) for i in range(4))
        print(f"  guard {guard:2} ms: 1s {100 * ones:5.2f}%  3s {100 * threes:5.2f}%  1-3 pairs {pairs:6.1f} per 20000"
              f"  out {delay:5.1f} ms after the ideal time")
