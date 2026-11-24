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

    // The game time the main loop has handed its frames since launch, in ms:
    // each frame's measured time, or the whole vblanks GameFrameTime gives it
    // (1/60 s each). It moves once a frame, at the frame's start, paused or
    // not; the game draws the frame after (the nitrous trails, game/light_trails.h).
    double FrameClockMs();

    // The game time the main loop handed the frame now running, in ms (the
    // last FrameClockMs step: 1/60 s a frame at Frame Rate 60, 2/60 s at 30,
    // more for a slow frame); 0 before the first frame. For steps the game
    // takes once a frame, which the 360 took 30 times a second
    // (game/map_cursor.h).
    double CurrentFrameMs();
}
