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
// On a display at a whole multiple of 60 Hz (120, 180, 240: RefreshEstimate's
// `multiple`, from video/refresh_fit.h) a guest vblank is `multiple`
// refreshes: the vblank lands 1 ms after a refresh (any of them: the phase is
// kept to the refresh grid), and its frame targets the refresh `multiple`
// later, the one before the next vblank, as at 60 Hz (the same 15.7 ms from
// vblank to screen). FIFO there shows a present on the first refresh after it,
// though, with nothing queued (the display has spare refreshes): a frame ready
// in under one refresh would show early and the one after it late, a
// 1-and-3 pair at 120 Hz. So the presenter holds each new frame until just
// after the refresh before its target (PresentAt), and every frame stays on
// screen for exactly `multiple` refreshes, or one more when it misses its
// refresh (with the next one shown on time, a refresh short). The target
// starts at the refresh right after the vblank's (vblank to screen 7.3 ms at
// 120 Hz, where it is 15.7 at 60): a frame ready within that refresh needs no
// hold. Frames that miss it aren't queued as at 60 Hz (where FIFO's queue
// then holds every later frame a refresh back, evenly, and a slip drains it):
// when several of the last 2 s missed their refresh, the target moves a
// refresh later (AdaptHold), up to two vblanks past the 60 Hz one, and a
// refresh earlier again after 10 s of frames all ready half a refresh before
// that earlier one (twice as long after each earlier target that missed
// within 6 s, up to 160 s). There are no slips; the
// "follows" chase counts in refreshes, as at 60 Hz. A display that follows
// the vblank there (variable refresh in a 120 Hz mode, say) would show held
// frames as soon as they are presented and so fit any grid the hold makes:
// held frames seen on screen less than half a refresh after their present
// (two thirds of the last 30-120 of them, across locks) are that, and the
// lock lets go for the run, as at 60 Hz (on a fixed display a held frame
// waits a refresh, so this can only take a fixed display for one where the
// present waits return more than half a refresh late: it then free-runs,
// as in v0.1.6). At the first target frames aren't held: there the lock's
// steps adding up to more than a refresh and a half one way, without 10 s
// held at the grid between, are that display (a fixed one's grid is
// reached in half a refresh of steps).
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
            double periodNs = 0;     // the vblank period in use (locked: `display.multiple` refreshes)
            double leadNs = 0;       // locked: before the refresh each vblank targets
            double errorNs = 0;      // locked: the last vblank's distance from its wanted time
            uint64_t slips = 0;      // refreshes slipped to drain queued frames
            bool follows = false;    // the display follows the vblank: never locked again
            uint64_t observed = 0;   // game frames timed on screen
            double latencyNs = 0;    // median vblank -> on screen, recent frames
            double readyNs = 0;      // 99th percentile vblank -> presented, recent frames (ready: before any hold)
            RefreshEstimate display; // the measurement
            // Locked at a multiple: new frames by refreshes on screen against
            // the vblanks' (since the lock): as many, fewer (shown early or
            // after a late one), more (late, or repeated). 0 at 60 Hz.
            uint64_t even = 0, shorter = 0, longer = 0;
            // Locked at a multiple: new frames target the refresh `multiple`
            // + extra after the last one before their vblank (AdaptHold;
            // extra starts at 1 - multiple: the next refresh), its changes,
            // and the misses in the last 2 s that made it later (0: earlier).
            int32_t extra = 0;
            uint64_t extraChanges = 0;
            uint32_t extraMissed = 0;
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
        // `presented` and seen on screen at `onScreen` (ns, steady clock);
        // `ready`: when it was ready to present, if it was held for its
        // refresh before that (PresentAt; 0: = presented).
        void Observe(int64_t vblank, int64_t presented, int64_t onScreen, int64_t ready = 0);
        Status GetStatus() const;
        bool Locked() const;
        // Refreshes a vblank is while locked at a multiple of 60 Hz with new
        // frames held for their refresh (PresentAt); 1 otherwise.
        int HoldMultiple() const;
        // How many times AdaptHold has moved new frames to a later or an
        // earlier refresh (Status::extraChanges, without the rest).
        uint64_t HoldChanges() const;
        double PeriodNs() const;  // the vblank period now (locked at a multiple: that many refreshes)
        // Main thread, before presenting a new game frame released by the
        // vblank that fired at `vblank`: not before the time returned (ns,
        // steady clock; 0: at once). Locked at a multiple only: just after the
        // refresh before the one the frame targets (the refresh `multiple`
        // after the last refresh before `vblank`, plus any lateness the lock
        // has accepted), so FIFO shows it on its target and no sooner.
        int64_t PresentAt(int64_t vblank) const { return PresentAt(vblank, 0.0f); }
        // An in-between picture (game/hfr.h) presented after the vblank at
        // `vblank` and before the next one's frame: `between` (its fraction of
        // the step) of the vblank's refreshes after the target of a frame
        // released by `vblank` (one refresh at 120 Hz for 0.5).
        int64_t PresentAt(int64_t vblank, float between) const;
        // NFSMW_PRESENT_HOLD=0: frames are presented when ready at a multiple
        // too (the lock alone, for comparison).
        void SetHold(bool on);

        static constexpr int64_t kMaxStepNs = 50'000;       // phase moved per vblank
        static constexpr int64_t kAfterRefreshNs = 1'000'000; // vblank this long after the refresh
        // A held frame is presented this long after the refresh before its
        // target (the fitted grid: a present seen on screen, a little after
        // the refresh itself), so the compositor latches it for the target.
        static constexpr int64_t kHoldAfterRefreshNs = 250'000;
        // AdaptHold: the target a refresh later when this many of the last
        // kHistory missed their refresh (one late frame, a stall's, doesn't),
        // at most kMaxExtraVblanks vblanks past the 60 Hz one; a refresh
        // earlier after kEarlyToHoldShorter frames in a row (times 2 to the
        // backoff: an earlier target that missed within 6 s doubles it, up
        // to kMaxEarlierBackoff) ready half a refresh and kHoldMarginNs
        // before that earlier target (the compositor's latch and the GPU's
        // last work fit in that).
        static constexpr uint32_t kMissedToHoldLonger = 2;
        static constexpr int32_t kMaxExtraVblanks = 2;
        static constexpr uint32_t kEarlyToHoldShorter = 600;  // 10 s
        static constexpr uint32_t kEarlyExceptions = 0;
        static constexpr uint32_t kMaxEarlierBackoff = 4;  // 160 s
        static constexpr int64_t kHoldMarginNs = 500'000;
        static constexpr uint32_t kQuickToFollow = 30;  // held frames seen before judging
        static constexpr uint32_t kSteadyToForget = 600;  // vblanks at the grid that forget the steps' sum

    private:
        void AdaptHold(int64_t vblank, int64_t ready, int64_t presented, int64_t onScreen);

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
        bool m_hold = true;
        // Locked at a multiple: the last observed frame (cadence counts), and
        // AdaptHold's state: refreshes held past the target, which of the
        // last kHistory frames missed theirs, frames since a change, and
        // frames in a row ready in time for one refresh less.
        int64_t m_lastOnScreen = 0, m_lastVblank = 0;
        uint64_t m_even = 0, m_shorter = 0, m_longer = 0;
        int32_t m_extra = 0;
        bool m_missed[kHistory] = {};
        uint32_t m_missedNext = 0, m_sinceExtra = 0, m_earlyRun = 0, m_earlyLate = 0, m_extraMissed = 0, m_earlierBackoff = 0;
        bool m_lastEarlier = false;
        double m_chaseSumNs = 0;  // at a multiple: the lock's steps since 10 s held (variable refresh)
        // Held frames seen on screen within half a refresh of their present,
        // of the last kHistory held ones (variable refresh).
        bool m_quick[kHistory] = {};
        uint32_t m_quickNext = 0, m_heldSeen = 0;
        uint64_t m_extraChanges = 0;
    };
}
