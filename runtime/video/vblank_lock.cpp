// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See vblank_lock.h.
#include "vblank_lock.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace video
{
    int64_t VblankLock::Next(int64_t prev)
    {
        std::lock_guard lock(m_mutex);
        bool locked = m_enabled && !m_follows && m_display.valid;
        if (locked != m_locked)
        {
            m_locked = locked;
            m_steadyVblanks = 0;
            m_slipLeftNs = 0;
            // AdaptHold starts over with each lock (another display, maybe),
            // at the refresh right after the vblank's.
            m_extra = locked ? 1 - m_display.multiple : 0;
            m_earlierBackoff = 0;
            m_lastEarlier = false;
            m_sinceExtra = 0;
            m_earlyRun = m_earlyLate = 0;
            std::fill(m_missed, m_missed + kHistory, false);
        }
        if (!locked)
            return prev + m_fallbackNs;
        // One refresh, and a guest vblank's worth of them (1 at 60 Hz).
        const double period = m_display.periodNs;
        const double vblank = period * m_display.multiple;
        m_leadNs = vblank - double(kAfterRefreshNs);
        double candidate = double(prev) + vblank;
        if (m_slipLeftNs > 0)
        {
            double step = std::min(double(kMaxStepNs), m_slipLeftNs);
            m_slipLeftNs -= step;
            return int64_t(std::llround(candidate + step));
        }
        // The wanted vblanks are grid + k * period - lead (k any refresh,
        // also at a multiple); the nearest one.
        double x = (candidate + m_leadNs - double(m_display.gridNs)) / period;
        m_errorNs = (std::round(x) - x) * period;
        double step = std::clamp(m_errorNs, -double(kMaxStepNs), double(kMaxStepNs));
        m_steadyVblanks = std::abs(m_errorNs) <= double(kMaxStepNs) ? m_steadyVblanks + 1 : 0;
        m_chasingVblanks = std::abs(m_errorNs) > double(kMaxStepNs) ? m_chasingVblanks + 1 : 0;
        // At a multiple, also the steps' sum: a display that follows the
        // vblank (its frames shown as they come, unheld at the first
        // target) keeps the lock stepping one way, though the fit's lag lets
        // the error reach a step now and then (which resets the count
        // above). A fixed display's grid is reached within half a refresh
        // of steps and then held; 10 s held forgets the walk.
        if (m_display.multiple >= 2)
        {
            m_chaseSumNs = m_steadyVblanks >= kSteadyToForget ? 0.0 : m_chaseSumNs + step;
            if (std::abs(m_chaseSumNs) > 1.5 * period)
                m_chasingVblanks = std::numeric_limits<uint32_t>::max() / 2;
        }
        if (double(m_chasingVblanks) * double(kMaxStepNs) > period)
        {
            m_follows = true;
            m_locked = false;
            return prev + m_fallbackNs;
        }
        return int64_t(std::llround(candidate + step));
    }

    void VblankLock::Resumed()
    {
        std::lock_guard lock(m_mutex);
        m_steadyVblanks = 0;
        m_chasingVblanks = 0;
        m_count = 0;
        m_lastOnScreen = 0;
        m_chaseSumNs = 0;
    }

    void VblankLock::SetHold(bool on)
    {
        std::lock_guard lock(m_mutex);
        m_hold = on;
    }

    int64_t VblankLock::PresentAt(int64_t vblank, float between) const
    {
        std::lock_guard lock(m_mutex);
        if (!m_locked || m_display.multiple < 2 || !m_hold || !vblank)
            return 0;
        // The last refresh at or before the vblank (it fires kAfterRefreshNs
        // after one, a little later when its thread wakes late), and the
        // target `multiple` + m_extra refreshes on (AdaptHold; 1 at first):
        // rounding instead would take the next refresh for a vblank woken
        // more than half a refresh late.
        const double period = m_display.periodNs;
        const double k = std::floor(double(vblank - m_display.gridNs) / period);
        const double target = double(m_display.gridNs) + (k + m_display.multiple + m_extra + std::round(double(between) * m_display.multiple)) * period;
        return int64_t(std::llround(target - period + double(kHoldAfterRefreshNs)));
    }

    void VblankLock::SetDisplay(const RefreshEstimate& estimate)
    {
        std::lock_guard lock(m_mutex);
        m_display = estimate;
    }

    void VblankLock::Observe(int64_t vblank, int64_t presented, int64_t onScreen, int64_t ready)
    {
        std::lock_guard lock(m_mutex);
        if (!ready)
            ready = presented;
        uint32_t slot = m_count++ % kHistory;
        m_ready[slot] = float(ready - vblank);
        m_latency[slot] = float(onScreen - vblank);
        m_observed++;
        if (!m_locked)
        {
            m_late[slot] = 0;
            m_lastOnScreen = 0;
            return;
        }
        const double period = m_display.periodNs;
        m_late[slot] = int32_t(std::llround((double(onScreen - vblank) - m_leadNs) / period));
        if (m_display.multiple >= 2)
        {
            // Its predecessor's time on screen in refreshes, against the
            // vblanks between them (a gap of more than 0.5 s is a pause).
            if (m_lastOnScreen && onScreen - m_lastOnScreen <= 500'000'000)
            {
                const int64_t shown = std::llround(double(onScreen - m_lastOnScreen) / period);
                const int64_t wanted = std::max<int64_t>(1, std::llround(double(vblank - m_lastVblank) / (period * m_display.multiple))) *
                    m_display.multiple;
                (shown == wanted ? m_even : shown < wanted ? m_shorter : m_longer)++;
            }
            m_lastOnScreen = onScreen;
            m_lastVblank = vblank;
            AdaptHold(vblank, ready, presented, onScreen);
            return;  // nothing queues here (the display has spare refreshes): no slips
        }
        // Every frame of the last 2 s showed at least a refresh after its
        // target, with the phase settled and no slip for as long: a frame is
        // queued. Slip a refresh to drain it.
        const uint64_t slipFrames = uint64_t(period / double(kMaxStepNs)) + kHistory;
        if (m_slipLeftNs > 0 || m_steadyVblanks < kHistory || m_count < kHistory ||
            (m_slips && m_observed - m_lastSlipObserved < slipFrames))
            return;
        int32_t late = *std::min_element(m_late, m_late + kHistory);
        if (!m_slipJudged)
        {
            m_slipJudged = true;
            if (late >= m_lateAtSlip)
                m_acceptedLate = m_lateAtSlip;
        }
        if (late > m_acceptedLate)
        {
            m_slipLeftNs = period;
            m_slips++;
            m_lastSlipObserved = m_observed;
            m_lateAtSlip = late;
            m_slipJudged = false;
        }
    }

    // Observe at a multiple, under the lock: frames held for later refreshes
    // when too many miss theirs, and for earlier ones again when every frame
    // for a while was ready in time for them; and a display that shows held
    // frames at once (variable refresh) let go of. As Observe's arguments.
    void VblankLock::AdaptHold(int64_t vblank, int64_t ready, int64_t presented, int64_t onScreen)
    {
        const double period = m_display.periodNs;
        if (presented > ready)  // held (PresentAt)
        {
            // Counted across locks, as the chase is: such a display soon
            // stops fitting a line, and each lock is short.
            m_quick[m_quickNext] = double(onScreen - presented) < period / 2;
            m_quickNext = (m_quickNext + 1) % kHistory;
            m_heldSeen = std::min(m_heldSeen + 1, kHistory);
            if (m_heldSeen >= kQuickToFollow && uint32_t(std::count(m_quick, m_quick + kHistory, true)) * 3 >= m_heldSeen * 2)
            {
                m_follows = true;
                return;
            }
        }
        // The frame's target with no extra refreshes (the 60 Hz one,
        // PresentAt's at m_extra 0), and whether it was ready half a
        // refresh and kHoldMarginNs before the target one refresh earlier
        // than now.
        // Missed: seen a refresh or more after its held-for one (three
        // quarters of one: a present wait that returns late, as 1% do by up
        // to 2 ms, isn't a frame shown late at 240 Hz).
        const double k = std::floor(double(vblank - m_display.gridNs) / period);
        const double target = double(m_display.gridNs) + (k + m_display.multiple) * period;
        m_missed[m_missedNext] = double(onScreen) - target > (double(m_extra) + 0.75) * period;
        m_missedNext = (m_missedNext + 1) % kHistory;
        const double fewerAt = target + double(m_extra - 1) * period - period / 2 - double(kHoldMarginNs);
        if (m_extra > 1 - m_display.multiple)
        {
            m_earlyLate += double(ready) > fewerAt;
            m_earlyRun++;
            if (m_earlyLate > kEarlyExceptions)
                m_earlyRun = m_earlyLate = 0;
        }
        m_sinceExtra++;
        if (m_sinceExtra < kHistory || m_steadyVblanks < kHistory || m_count < kHistory)
            return;
        const uint32_t missed = uint32_t(std::count(m_missed, m_missed + kHistory, true));
        if (missed >= kMissedToHoldLonger && m_extra < kMaxExtraVblanks * m_display.multiple)
        {
            // An earlier target that didn't last (missed within 6 s): wait
            // twice as long before the next try.
            if (m_lastEarlier && m_sinceExtra < 3 * kHistory)
                m_earlierBackoff = std::min(m_earlierBackoff + 1, kMaxEarlierBackoff);
            m_lastEarlier = false;
            m_extra++;
            m_extraChanges++;
            m_extraMissed = missed;
            m_sinceExtra = 0;
            m_earlyRun = m_earlyLate = 0;
            std::fill(m_missed, m_missed + kHistory, false);
        }
        else if (m_earlyRun >= kEarlyToHoldShorter << m_earlierBackoff)
        {
            m_lastEarlier = true;
            m_extra--;
            m_extraChanges++;
            m_extraMissed = 0;
            m_sinceExtra = 0;
            m_earlyRun = m_earlyLate = 0;
            std::fill(m_missed, m_missed + kHistory, false);
        }
    }

    uint64_t VblankLock::HoldChanges() const
    {
        std::lock_guard lock(m_mutex);
        return m_extraChanges;
    }

    int VblankLock::HoldMultiple() const
    {
        std::lock_guard lock(m_mutex);
        return m_locked && m_hold && m_display.multiple >= 2 ? m_display.multiple : 1;
    }

    bool VblankLock::Locked() const
    {
        std::lock_guard lock(m_mutex);
        return m_locked;
    }

    double VblankLock::PeriodNs() const
    {
        std::lock_guard lock(m_mutex);
        return m_locked ? m_display.periodNs * m_display.multiple : double(m_fallbackNs);
    }

    VblankLock::Status VblankLock::GetStatus() const
    {
        std::lock_guard lock(m_mutex);
        Status s;
        s.locked = m_locked;
        s.periodNs = m_locked ? m_display.periodNs * m_display.multiple : double(m_fallbackNs);
        s.leadNs = m_leadNs;
        s.errorNs = m_errorNs;
        s.slips = m_slips;
        s.follows = m_follows;
        s.observed = m_observed;
        s.display = m_display;
        s.even = m_even;
        s.shorter = m_shorter;
        s.longer = m_longer;
        s.extra = m_extra;
        s.extraChanges = m_extraChanges;
        s.extraMissed = m_extraMissed;
        uint32_t n = std::min(m_count, kHistory);
        if (n)
        {
            float latency[kHistory], ready[kHistory];
            std::copy(m_latency, m_latency + n, latency);
            std::copy(m_ready, m_ready + n, ready);
            std::nth_element(latency, latency + n / 2, latency + n);
            std::nth_element(ready, ready + n * 99 / 100, ready + n);
            s.latencyNs = latency[n / 2];
            s.readyNs = ready[n * 99 / 100];
        }
        return s;
    }
}
