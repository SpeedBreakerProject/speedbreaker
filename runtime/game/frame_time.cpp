// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See frame_time.h.
//
// NFSMW times itself with mftb. The main loop (sub_823B0008) measures each
// frame and hands it, in milliseconds as 16.16 fixed point, to the frame
// (sub_823AFE20). That quantises it to 1/3600 s (sub_823C6500; the frame's
// time for its real-time users, [0x82A39660]), advances RealTimeFrames and a
// 4000 Hz tick counter at 0x82A399AC (AdvanceRealTime, 0x823C66C8), and runs
// the world simulation in fixed 1/60 s steps from an accumulator over those
// ticks (sub_823A2348: floor(elapsed / step) steps, the remainder kept; the
// step is stored once, at boot, and tasks copy it). There is no interpolation:
// a frame's world state is its steps'.
//
// So a frame that isn't 1/60 s long shows it. With the vblank locked to the
// player's 59.9726 Hz monitor, a frame lasts 16.674 ms and every 36.6 s one
// runs two steps: the car jumps, where the presenter used to drop a frame.
// And at any rate a frame's measured length jitters (the main thread's
// wake-up, the 250 us ticks), so while the accumulator's remainder sits near
// a step boundary, frames alternate between no step and two.
//
// Each guest vblank is made one step instead (NFSMW_GAME_TIME=0 turns this
// off). A frame is given k/60 s for the k guest vblanks that fired during it
// (counted from one frame's start to the next, 1 to 6), as the console's
// 60 Hz vblank would have given it; the stepper's elapsed time, then k steps
// give or take a tick, is rounded to k. The main loop starts a frame when D3D
// lets it, after the vblank that released the last one, so its start can
// jitter by most of a vblank without changing the count; a frame slower than
// the vblank starts anywhere between two, and its count is its length
// rounded either way. A frame no vblank fired in keeps its measured time,
// which later frames give back (game/whole_vblanks.h): game time adds up to
// the vblanks that fired. It runs at the display's rate over 60: 0.044% slow
// on a 59.97 Hz monitor, at most 2.5% (the lock's range). Frames of more
// than 6 vblanks (loading) keep their measured time. At Frame Rate 30
// (video::GuestVblankDivider) a guest vblank comes every other refresh and
// is given 2/60 s: two steps a frame, the game at its speed.
#include <stdafx.h>
#include "frame_time.h"
#include "whole_vblanks.h"

#include <kernel/function.h>
#include <video/presenter.h>
#include <video/vblank_lock.h>

namespace
{
    const bool s_enabled = [] {
        const char* v = std::getenv("NFSMW_GAME_TIME");
        bool on = !v || v[0] != '0';
        if (!on)
            fprintf(stderr, "[game] game time as measured (NFSMW_GAME_TIME=0)\n");
        return on;
    }();
    std::atomic<uint64_t> s_frames{ 0 }, s_snapped{ 0 }, s_noStep{ 0 }, s_multiStep{ 0 };
    // NFSMW_GAME_TIME_LOG=<file>: one line per frame, written when the next
    // one starts: its measured ms, guest vblanks, whether it was given whole
    // vblanks, the world steps it ran (-1: no world), how long after the last
    // vblank it started (us), and the guest vblanks it was given (0: its
    // measured time).
    FILE* const s_log = [] {
        const char* path = std::getenv("NFSMW_GAME_TIME_LOG");
        FILE* f = path ? fopen(path, "w") : nullptr;
        if (f)
            fprintf(f, "measured_ms,vblanks,snapped,steps,after_vblank_us,given\n");
        return f;
    }();
    struct LogLine
    {
        double measuredMs = 0, afterVblankUs = 0;
        uint64_t vblanks = 0, given = 0;
        int steps = -1;
        bool snapped = false, pending = false;
    } s_line;  // main thread only
    game::WholeVblanks s_whole;  // main thread only

    // Whole vblanks are game time only at the lock's rates: NFSMW_VBLANK_HZ
    // far from 60 keeps game time as measured, in the stepper too.
    bool NearSixty(double vblankMs)
    {
        return vblankMs >= 1000.0 / video::RefreshFit::kMaxHz && vblankMs <= 1000.0 / video::RefreshFit::kMinHz;
    }
}

namespace game
{
    FrameTimeStats GetFrameTimeStats()
    {
        return { s_frames.load(std::memory_order_relaxed), s_noStep.load(std::memory_order_relaxed),
            s_multiStep.load(std::memory_order_relaxed), s_snapped.load(std::memory_order_relaxed) };
    }
}

// GameFrameTime, before `bl 0x823AFE20` at 0x823B0148 in the main loop: r3 is
// the frame's measured time, milliseconds in 16.16 fixed point.
void GameFrameTime(PPCRegister& r3)
{
    static uint64_t last = 0;
    uint64_t now = video::GuestVblanks(), vblanks = now - last;
    last = now;
    s_frames.fetch_add(1, std::memory_order_relaxed);
    double measuredMs = double(int32_t(r3.u32)) / 65536.0;
    if (s_log)
    {
        if (s_line.pending)
            fprintf(s_log, "%.3f,%llu,%d,%d,%.0f,%llu\n", s_line.measuredMs, (unsigned long long)s_line.vblanks, s_line.snapped,
                s_line.steps, s_line.afterVblankUs, (unsigned long long)s_line.given);
        int64_t since = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() -
            video::LastGuestVblank();
        s_line = { measuredMs, double(since) / 1e3, vblanks, 0, -1, false, true };
    }
    if (!s_enabled)
        return;
    double refreshMs = video::GuestVblank().PeriodNs() / 1e6;
    if (!NearSixty(refreshMs))
    {
        s_whole.Reset();
        return;
    }
    // At Frame Rate 30 a guest vblank is two refreshes, and two 1/60 s steps.
    int divider = video::GuestVblankDivider();
    uint64_t given = s_whole.Give(measuredMs, vblanks, refreshMs * divider);
    if (!given)
        return;
    r3.u64 = uint32_t(std::llround(double(given * divider) * (1000.0 / 60.0) * 65536.0));
    s_snapped.fetch_add(1, std::memory_order_relaxed);
    s_line.snapped = true;
    s_line.given = given;
}

// WorldStepTime, before `fadds f0,f10,f9` at 0x823A23E8 in the stepper: f10 is
// the elapsed time in steps (f9 the remainder it joins).
void WorldStepTime(PPCRegister& f10)
{
    if (!s_enabled || !NearSixty(video::GuestVblank().PeriodNs() / 1e6))
        return;
    double k = std::round(f10.f64);
    if (k >= 1 && k <= 6 && std::abs(f10.f64 - k) <= 0.05)
        f10.f64 = k;
}

// WorldSteps, at `mr r4,r30` (0x823A2424) in the stepper: r30 is the number of
// steps this frame runs, before the step runner's cap.
void WorldSteps(PPCRegister& r30)
{
    s_line.steps = int(r30.u32);
    if (r30.u32 == 0)
        s_noStep.fetch_add(1, std::memory_order_relaxed);
    else if (r30.u32 >= 2)
        s_multiStep.fetch_add(1, std::memory_order_relaxed);
}
