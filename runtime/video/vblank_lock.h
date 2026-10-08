// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The guest vblank, locked to the display. NFSMW shows one frame per vblank
// (D3D's swap throttle), so the vblank interrupt (gpu/command_processor.cpp's
// Vsync thread) is the game's frame clock. Free-running at 60 Hz it beats
// against the display: at 59.9726 Hz (a measured monitor) the guest makes
// one frame too many every 38 s, which the presenter drops, and until then
// its FIFO queue stays full (the most latency).
//
// Locked, each vblank fires `lead` before a display refresh, on the refresh
// grid measured from present completions (video/refresh_fit.h): the period
// is the display's, and each vblank moves by at most kMaxStepNs toward the
// wanted phase. The lead is nearly a whole refresh, so the guest vblank lands
// just after the display's own: a frame has a refresh to reach the screen
// and shows on the next one, as on the console. When frames show a refresh
// later than that for 2 s in a row (queued behind others, e.g. from before
// the lock), the vblank slips one refresh later, a step at a time, which
// drains one queued frame without showing any twice. If a slip doesn't bring
// frames earlier they weren't queued but slow to present, and that lateness
// is accepted from then on (one frame was shown twice to learn it).
//
// Without a valid measurement (no present wait, a display outside 58.5-61.5 Hz,
// noisy completions) the vblank free-runs at `fallbackNs`, as before. So it
// does, for the rest of the run, when the refreshes move with the vblank: a
// display that shows each present when it comes (variable refresh, or an
// offscreen surface, whose completions fit the guest's own 60 Hz nearly well
// enough to pass) has no grid, and chasing it would drag the vblank, and
// game time with it, down to 58.5 Hz.
#pragma once
#include "refresh_fit.h"

#include <cstdint>
#include <mutex>

namespace video
{
    class VblankLock
    {
    public:
        struct Status
        {
            bool locked = false;
            double periodNs = 0;     // the vblank period in use
            double leadNs = 0;       // locked: before the refresh each vblank targets
            double errorNs = 0;      // locked: the last vblank's distance from its wanted time
            uint64_t slips = 0;      // refreshes slipped to drain queued frames
            bool follows = false;    // the display follows the vblank: never locked again
            uint64_t observed = 0;   // game frames timed on screen
            double latencyNs = 0;    // median vblank -> on screen, recent frames
            double readyNs = 0;      // 99th percentile vblank -> presented, recent frames
            RefreshEstimate display; // the measurement
        };

        VblankLock(int64_t fallbackNs, bool enabled) : m_fallbackNs(fallbackNs), m_enabled(enabled) {}

        // Vsync thread: when the vblank after the one scheduled at `prev` fires.
        int64_t Next(int64_t prev);
        // Vsync thread: the game was suspended and the vblank moved on by
        // exactly the time suspended, so it may be up to half a refresh off
        // the display's grid. Next walks it back; the walk starts a fresh
        // chase count (not one that adds up to "the display follows the
        // vblank"), and frames from before the gap aren't judged with the
        // ones after it.
        void Resumed();
        // The display's refresh grid (from the presenter's wait thread).
        void SetDisplay(const RefreshEstimate& estimate);
        // A game frame, released by the vblank at `vblank`, was presented at
        // `presented` and seen on screen at `onScreen` (ns, steady clock).
        void Observe(int64_t vblank, int64_t presented, int64_t onScreen);
        Status GetStatus() const;
        bool Locked() const;
        double PeriodNs() const;  // the vblank period now

        static constexpr int64_t kMaxStepNs = 50'000;       // phase moved per vblank
        static constexpr int64_t kAfterRefreshNs = 1'000'000; // vblank this long after the refresh

    private:
        mutable std::mutex m_mutex;
        const int64_t m_fallbackNs;
        const bool m_enabled;
        RefreshEstimate m_display;
        bool m_locked = false;
        double m_leadNs = 0;
        double m_errorNs = 0;
        double m_slipLeftNs = 0;
        uint64_t m_slips = 0;
        uint32_t m_steadyVblanks = 0;  // since the phase error last exceeded a step
        // Locked vblanks a whole step from the wanted phase since it was
        // last reached. Reaching it takes at most half a refresh of steps; a
        // whole refresh means the refreshes move with the vblank. Counted
        // across locks: chased, such a display soon stops fitting a line and
        // the lock comes and goes.
        uint32_t m_chasingVblanks = 0;
        bool m_follows = false;
        // Recent game frames: which refresh each showed on (0: the one its
        // vblank targets) and vblank -> presented.
        static constexpr uint32_t kHistory = 120;
        int32_t m_late[kHistory] = {};
        float m_ready[kHistory] = {}, m_latency[kHistory] = {};
        uint32_t m_count = 0;
        uint64_t m_observed = 0;
        uint64_t m_lastSlipObserved = 0;
        // A slip that didn't bring frames earlier: they need that refresh
        // (a long present path), so lateness up to it is accepted.
        int32_t m_acceptedLate = 0, m_lateAtSlip = 0;
        bool m_slipJudged = true;
    };
}
