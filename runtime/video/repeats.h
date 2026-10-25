// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The [perf] line's "repeated" (video::PresentStats), as the Windows
// presenter counts it: refreshes a game frame stayed on screen past the time
// its guest vblanks gave it, in the display's own refreshes. The game runs
// at 60 fps whatever the panel does, so on a panel that isn't 60 Hz frames
// show for whole refreshes of another length:
//
//   panel     a frame shows for              counted (one each)
//   60 Hz     1 refresh (16.7 ms)            a frame kept 2 refreshes (a repeat)
//   120 Hz    2 (16.7), or 1 and 3 in pairs  the 3s (25.0 ms)
//   144 Hz    2 or 3 (13.9 / 20.8 ms)        the 3s, ~40% of frames: the judder
//   165 Hz    3 or 2 (18.2 / 12.1 ms)        only a 4: a 3 is a quarter refresh
//                                            long (the short 2s are the uneven ones)
//   240 Hz    4 (16.7), 3 or 5 with jitter   the 5s (20.8 ms)
//   G-SYNC    ~16.7 ms, as delivered         frames half a refresh late (3.5 ms at 144)
//
// The Windows port's work notes (144HZ-NOTE.md) have the model behind it.
// Header only: tests/repeats_test.cpp checks it.
#pragma once
#include <cmath>
#include <cstdint>

namespace video
{
    // A game frame given `vblanks` guest vblanks of `vblankNs` each was on
    // screen for `shownNs` (its present's completion to the next one's), on
    // a display refreshing every `refreshNs`: the refreshes it stayed past
    // its vblanks' time, to the nearest. A gap of more than half a second is
    // a pause, not a repeat; an unknown refresh (0) counts nothing.
    inline int64_t ExtraRefreshes(double shownNs, int64_t vblanks, double vblankNs, double refreshNs)
    {
        if (refreshNs <= 0.0 || shownNs > 500e6)
            return 0;
        int64_t extra = std::llround((shownNs - double(vblanks) * vblankNs) / refreshNs);
        return extra > 0 ? extra : 0;
    }
}
