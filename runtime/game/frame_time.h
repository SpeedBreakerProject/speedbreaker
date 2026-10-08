// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Game time in whole vblanks (frame_time.cpp): each guest vblank is one world
// step, at any display rate the vblank locks to.
#pragma once
#include <cstdint>

namespace game
{
    // Since launch, for the [perf] line: game frames (main loop iterations),
    // those whose world ran no step or two and more, and those given whole
    // vblanks of time.
    struct FrameTimeStats
    {
        uint64_t frames, noStep, multiStep, snapped;
    };
    FrameTimeStats GetFrameTimeStats();
}
