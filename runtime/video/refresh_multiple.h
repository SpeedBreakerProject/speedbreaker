// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Which displays the guest vblank locks to every 2nd, 3rd or 4th refresh of
// (video/refresh_fit.h's multiple, video/vblank_lock.h), from the display
// mode's refresh rate (SDL's), and the log line that says so. Header only:
// tests/refresh_fit_test.cpp checks it.
//
// Only the multiple comes from the mode: the period and phase are measured
// from present completions, as at 60 Hz (the mode's rate is gamescope's CVT
// one under gamescope, 59.94 for a 59.97 Hz monitor). At 60 Hz nothing
// changes. A rate that is no whole multiple of 60 (90 Hz, the Steam Deck
// OLED's; 144 Hz; 165 Hz) gets a line saying why the vblank free-runs there.
//
// Auto (NFSMW_REFRESH_MULTIPLE unset) is on everywhere but on Apple's
// systems: there a MacBook Pro's ProMotion panel reports 120 Hz but has no
// fixed refresh grid (frames are shown when their GPU work is done, on a
// 240 Hz timing grid), and new frames are shown at a set time after their
// vblank instead (video/present_schedule.h), measured to work on it and on a
// player's external 120 Hz monitor (GitHub issue #20). NFSMW_REFRESH_MULTIPLE
// =0: never (v0.1.6); =1: from the mode, on Apple's systems too; =2/3/4: that
// multiple whatever the mode says (a display that misreports its rate).
#pragma once
#include "refresh_fit.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace video
{
    struct RefreshMultiple
    {
        enum class Mode
        {
            Auto,    // unset
            Off,     // "0"
            On,      // "1"
            Forced,  // "2" to "4": `forced`
        };
        Mode mode = Mode::Auto;
        int forced = 0;

        static RefreshMultiple FromEnv(const char* value)
        {
            RefreshMultiple r;
            if (!value || !*value)
                return r;
            if (!std::strcmp(value, "0"))
                r.mode = Mode::Off;
            else if (!std::strcmp(value, "1"))
                r.mode = Mode::On;
            else if (value[1] == 0 && value[0] >= '2' && value[0] <= '0' + RefreshFit::kMaxMultiple)
            {
                r.mode = Mode::Forced;
                r.forced = value[0] - '0';
            }
            return r;  // anything else: Auto
        }

        // The multiple RefreshFit should expect on a display whose mode runs
        // at `hz` (0: unknown): 1 (as v0.1.6), or 2-4.
        int For(double hz, bool apple) const
        {
            switch (mode)
            {
            case Mode::Off:
                return 1;
            case Mode::Forced:
                return forced;
            case Mode::Auto:
                if (apple)
                    return 1;
                [[fallthrough]];
            case Mode::On:
                break;
            }
            const int m = RefreshFit::MultipleOf(hz);
            return m >= 2 ? m : 1;
        }

        // Smooth Motion (game/hfr.h) on SteamOS and Linux: whether a display
        // mode of `hz`, its guest vblank expected every `m` refreshes (For's),
        // shows in-between pictures evenly (video::HfrOffered, with present
        // timing): 120 Hz, which the guest vblank locks to every 2nd refresh
        // of (video::HfrPicturesPerStep: 2 pictures a step), and 90 Hz (the
        // Steam Deck OLED's), whose fixed refresh grid the pictures are drawn
        // for (video::HfrGrid, game/hfr_grid.h: 1 or 2 a step). Measured on
        // the Steam Machine in nested gamescope (v020/smooth-steamos,
        // 2026-10-24): 120 and 90 Hz showed a picture every refresh in a race;
        // 144, 165, 180 and 240 Hz drew one for every refresh, but 12-47% of
        // them reached the screen 2-4 refreshes apart (128, 120, 148 and 127
        // a second): not offered there, nor at 60 Hz or below, the rates in
        // between (75, 100 Hz), or with the lock off (NFSMW_REFRESH_MULTIPLE=0).
        static bool SmoothMotionRate(double hz, int m)
        {
            if (m >= 2)
                return m == 2 && RefreshFit::MultipleOf(hz) == 2;
            return RefreshFit::MultipleOf(hz) == 0 && std::fabs(hz - 90.0) <= 1.5;
        }

        // The [vblank] line for a display mode of `hz` given multiple `m`
        // (For's), or "" where there is nothing to say: 60 Hz, an unknown
        // rate, Off, and Auto on Apple's systems (as v0.1.6 there).
        std::string Describe(double hz, int m, bool apple) const
        {
            char line[400];
            if (m >= 2)
            {
                std::snprintf(line, sizeof line, "a %.2f Hz display%s: the guest vblank locks to every %s refresh once presents "
                    "measure it, and new frames are held for their refresh (NFSMW_REFRESH_MULTIPLE=0: as v0.1.6)", hz,
                    mode == Mode::Forced ? " (NFSMW_REFRESH_MULTIPLE)" : "", m == 2 ? "2nd" : m == 3 ? "3rd" : "4th");
                return line;
            }
            if (mode == Mode::Off || (mode == Mode::Auto && apple) || !(hz > 0) || RefreshFit::MultipleOf(hz) >= 1)
                return {};  // 60 Hz (or a multiple held at 1: Forced never is, For never says 1 for one)
            const double perFrame = hz / 60.0;
            if (hz < RefreshFit::kMinHz)
                std::snprintf(line, sizeof line, "a %.2f Hz display is slower than 60 Hz: the guest vblank free-runs at 60 Hz, and "
                    "some frames are never shown (%.2f refreshes a frame)", hz, perFrame);
            else if (hz > RefreshFit::kMaxHz * RefreshFit::kMaxMultiple)
                std::snprintf(line, sizeof line, "a %.2f Hz display is faster than the lock's 240 Hz: the guest vblank free-runs at "
                    "60 Hz, and frames stay on screen for about %.2f refreshes each", hz, perFrame);
            else
                std::snprintf(line, sizeof line, "a %.2f Hz display is no whole multiple of 60 Hz: the guest vblank free-runs at 60 Hz, "
                    "and each frame stays on screen for %d or %d refreshes (%.2f a frame); at 60 or 120 Hz, or with variable refresh, "
                    "frames show evenly", hz, int(std::floor(perFrame)), int(std::floor(perFrame)) + 1, perFrame);
            return line;
        }
    };
}
