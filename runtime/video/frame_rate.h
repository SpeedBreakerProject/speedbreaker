// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The game's frame rate: Settings > Display > Frame Rate's 60 or 30, or what
// Auto chose (video/frame_rate_policy.h: 60, or 30 while the device is hot or
// can't hold 60). Auto uses the setting's own mechanism (GuestVblankDivider:
// one guest vblank every other refresh, two world steps a frame), so a
// switch mid-race keeps the game's speed. Each switch is logged ("[frame
// rate] auto 60 -> 30 at 86.9 s: ...") and shown in a toast.
// NFSMW_AUTO_FPS_LOG=1 logs every evaluation (window, costs, thermal, timers).
#pragma once
#include "frame_rate_policy.h"

#include <string>

namespace video
{
    // Any thread, lock-free: 60 or 30.
    int FrameRate();
    // The command processor, every 30 swaps: those swaps' game time and
    // per-frame costs. Drives Auto while Frame Rate is Auto.
    void NoteFrameCosts(const FrameSample& sample);
    // For the menu's help line while Frame Rate is Auto: "Now 30 fps: the
    // device is warm."; "" when it isn't Auto. Any thread.
    std::string AutoFrameRateNote();
    // For the [perf] line: " | auto 30 fps" while Frame Rate is Auto, else "".
    std::string AutoFrameRatePerfText();
}
