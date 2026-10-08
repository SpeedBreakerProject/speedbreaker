// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// How GameFrameTime (frame_time.cpp) gives game frames whole guest vblanks
// of time, apart so tests/whole_vblanks_test.cpp can check it.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace game
{
    // Game time follows the guest vblanks that fire: over any run of frames
    // it adds up to them, to within a vblank, and each frame that saw any
    // gets whole ones (1 to 6), so the fixed 1/60 s world step runs a whole
    // number of times a frame.
    //   - A frame the vblank paces (60 fps held, or 30 at Frame Rate 30) saw
    //     one and gets one.
    //   - A frame it doesn't (GPU-bound) starts anywhere between two, so its
    //     count is its length rounded down or up: a 44 ms frame (2.64
    //     refreshes) sees 2 or 3, and gets them.
    //   - A frame no vblank fired in (a vblank thread waking late, as the
    //     Mac's does several ms at a time when its GPU is busy; or two frames
    //     between two vblanks) keeps its measured time, and is then ahead of
    //     the vblanks by that much: the next frames give it back, one whole
    //     vblank fewer when it adds up to one, never fewer than one.
    // The rule until 2026-10-07 gave a frame its count only when its
    // measured time was within half a vblank of it, else the measured time:
    // GPU-bound 44 ms frames got 3 vblanks or their own 2.64, so the game ran
    // fast (3-6% on an Intel laptop GPU), and at Frame Rate 30 (44-49 ms
    // frames, 1 or 2 guest vblanks of 33.3 ms) 9-15% slow; a late vblank
    // thread's 0 and 2 counts around 18 ms frames (the Mac at 3x3) ran it
    // 6-9% slow.
    struct WholeVblanks
    {
        // Game time given beyond the vblanks that fired, in vblanks: at most
        // kMaxAhead (a held vblank, as a GPU-only suspension's, isn't
        // replayed: what can't be given back is forgotten).
        double ahead = 0.0;
        static constexpr double kMaxAhead = 6.0;

        // A frame the main loop measured at `measuredMs`, during which
        // `vblanks` guest vblanks fired (from its start to the next frame's),
        // each `vblankMs` long: the vblanks to give it, or 0 to keep its
        // measured time. More than 6 (loading) keeps it too, and starts over.
        uint64_t Give(double measuredMs, uint64_t vblanks, double vblankMs)
        {
            if (vblanks > 6 || !(vblankMs > 0.0))
            {
                ahead = 0.0;
                return 0;
            }
            if (vblanks == 0)
            {
                ahead = std::min(ahead + std::max(measuredMs, 0.0) / vblankMs, kMaxAhead);
                return 0;
            }
            double owed = double(vblanks) - ahead;
            int64_t give = std::clamp<int64_t>(std::llround(owed), 1, 6);
            ahead = double(give) - owed;
            return uint64_t(give);
        }

        // Game time measured again (NFSMW_GAME_TIME=0, a rate far from 60).
        void Reset() { ahead = 0.0; }
    };
}
