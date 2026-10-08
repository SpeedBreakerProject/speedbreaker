// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Frame Rate's Auto: when to run the game at 30 instead of 60. The switch
// itself is the setting's own 60/30 (video/frame_rate.h feeds
// video::GuestVblankDivider), so the game keeps its speed. Pure, for
// tests/frame_rate_policy_test.cpp.
//
//   - 60 -> 30 when the device is hot (thermal serious or critical: iOS
//     throttles the CPU and GPU from there), or when it can't hold 60: over
//     the last kWindowSec of game time most of the frames ran under kDropFps
//     (the median sample: a long hitch or two doesn't count), and those slow
//     samples cost kDropCostMs or more a frame on the command processor or
//     the GPU (they were slow for want of time: not the game's own 30 fps
//     menus and movies, under 1 ms CP and 3-6 ms GPU, nor loading).
//   - 30 -> 60 once thermal has been fair or better for kCoolSec (counted
//     from the drop, or from the last time it was hot), and either the last
//     kWindowSec of gameplay at 30 cost little enough for 60 (kRiseCpMs and
//     kRiseGpuMs a frame: a 2-3 ms band under the drop's cost), or 30 has
//     lasted kTrySec with the CP within kRiseCpMs (the GPU unproven: costs
//     measured at 30 run on lower clocks and read high, so a try is how 60
//     comes back after a cautious no). A try that fails drops again
//     kDwellSec later. Only gameplay counts at 30 (kGameplayDraws and
//     kGameplayCpMs a frame, at least): menus, loading screens and movies
//     (under 1100 draws, most under 1 ms CP; the pause menu over a race
//     1000-1120 draws at 2-4 ms) are cheap at any rate and say nothing about
//     the race that comes next.
//   - Never two switches within kDwellSec. A drop within kSettleSec of a
//     rise (that return didn't settle: a scene, or a device that heats up
//     again in a few minutes) doubles the cooldown and the try's wait (up
//     to 8x); kSettleSec at 60 resets them. So a scene that can't hold 60
//     costs a few short tries, then fewer.
//
// Numbers from the iPhone and iPad [perf] lines (2026-10-02 to 10-04),
// replayed (tests/frame_rate_policy_test.cpp): the iPhone 15 Pro Max player's
// free roam at 60 drops at 88 s (a median of 53.6 fps, its slow frames at
// CP 13 and GPU busy 19-21 ms), as it was throttled. Not on any iPad 1x run
// or iPad play session: their worst 10 s read 54.2-58.9 fps by the median,
// where a mean (53.5-53.9) caught 5 s bursts of 50 fps near a race's start.
// Nor on a menu (60 fps at a GPU busy of 16.7 ms: the queue's occupancy, not
// work) followed by a load, which a mean of the window's costs took for a
// GPU that can't hold 60. The player's 13:39 session (races at 36-53 fps
// after a drop at 339 s) came back to 60 three times in 15 minutes while
// menus and loading screens counted (twice by headroom on them, once by a
// try as a race ended); on gameplay alone, twice, both tries.
// The rise's thresholds are the drop's costs less a margin: no iOS device
// has run at 30 yet to measure them there.
#pragma once
#include <platform/thermal.h>

#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace video
{
    // The frames the game swapped since the last sample: the game time they
    // took (guest time: a suspension isn't part of it), and per frame, the
    // command processor's busy time, the GPU's (gpu/command_processor.cpp's
    // overlay numbers: executing time where it's timed, else busy) and the
    // draws.
    struct FrameSample
    {
        double guestSeconds = 0;
        int frames = 0;
        float cpMs = 0, gpuMs = 0;
        float draws = 0;
    };

    enum class FrameRateReason : uint8_t
    {
        None,
        Hot,         // 60 -> 30: thermal serious or critical
        CantHold60,  // 60 -> 30: mostly under kDropFps, at the cost of a full 60 fps frame
        Headroom,    // 30 -> 60: cool, and 30's frames cost little enough for 60
        Try,         // 30 -> 60: cool, and at 30 long enough to try 60 again
    };

    struct FrameRateDecision
    {
        bool changed = false;
        int rate = 60;  // after this update
        FrameRateReason reason = FrameRateReason::None;  // why it changed
        // The window this update judged (all samples since the last switch,
        // at most the newest kWindowSec): its game time, frame rate (all its
        // frames over that time, and the median sample's), per-frame costs,
        // and those of its samples under kDropFps.
        double windowSeconds = 0;
        float fps = 0, medianFps = 0, cpMs = 0, gpuMs = 0, slowCpMs = 0, slowGpuMs = 0;
        double rateSeconds = 0;  // at the rate before this update for this long (since the last switch)
        double coolSeconds = 0;  // at 30: thermal fair or better for this long (since the drop)
        int backoff = 0;         // the cooldown and the try's wait are 2^backoff times theirs
    };

    class AutoFrameRatePolicy
    {
    public:
        // 60, no switch yet: a device that is already hot drops at the first
        // update.
        void Reset();
        // `now`: seconds on a steady clock (the log's). Every half second or
        // so (every 30 swaps).
        FrameRateDecision Update(double now, platform::thermal::State thermal, const FrameSample& sample);
        int Rate() const { return m_rate; }

        static constexpr double kWindowSec = 10;     // of game time
        static constexpr double kDropFps = 54;
        static constexpr double kDropCostMs = 15;    // the heavier of CP and GPU, a frame: 90% of a 60 fps frame
        static constexpr double kRiseCpMs = 12;
        static constexpr double kRiseGpuMs = 13;
        static constexpr double kGameplayDraws = 1500;  // a frame, at least: gameplay (iPhone races and free roam 2000-6400)
        static constexpr double kGameplayCpMs = 1;
        static constexpr double kDwellSec = 30;      // at least, between two switches
        static constexpr double kCoolSec = 60;       // fair or better before 60 again (x 2^backoff)
        static constexpr double kTrySec = 180;       // at 30 before trying 60 unproven (x 2^backoff)
        static constexpr double kSettleSec = 600;    // at 60 this long: settled (a drop sooner backs off; this long, forgotten)
        static constexpr int kBackoffMax = 3;

    private:
        static constexpr double kLongAgo = -1e18;

        std::deque<FrameSample> m_window;  // since the last switch (at 30, gameplay only), trimmed to the newest kWindowSec
        std::vector<std::pair<double, int>> m_sorted;  // scratch: the window's samples by frame rate
        int m_rate = 60;
        double m_lastSwitch = kLongAgo;
        double m_lastRise = kLongAgo;
        double m_lastHot = kLongAgo;  // the last update that saw thermal serious or critical
        int m_backoff = 0;
    };
}
