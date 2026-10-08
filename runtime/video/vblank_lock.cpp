// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See vblank_lock.h.
#include "vblank_lock.h"

#include <algorithm>
#include <cmath>

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
        }
        if (!locked)
            return prev + m_fallbackNs;
        const double period = m_display.periodNs;
        m_leadNs = period - double(kAfterRefreshNs);
        double candidate = double(prev) + period;
        if (m_slipLeftNs > 0)
        {
            double step = std::min(double(kMaxStepNs), m_slipLeftNs);
            m_slipLeftNs -= step;
            return int64_t(std::llround(candidate + step));
        }
        // The wanted vblanks are grid + k * period - lead; the nearest one.
        double x = (candidate + m_leadNs - double(m_display.gridNs)) / period;
        m_errorNs = (std::round(x) - x) * period;
        double step = std::clamp(m_errorNs, -double(kMaxStepNs), double(kMaxStepNs));
        m_steadyVblanks = std::abs(m_errorNs) <= double(kMaxStepNs) ? m_steadyVblanks + 1 : 0;
        m_chasingVblanks = std::abs(m_errorNs) > double(kMaxStepNs) ? m_chasingVblanks + 1 : 0;
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
    }

    void VblankLock::SetDisplay(const RefreshEstimate& estimate)
    {
        std::lock_guard lock(m_mutex);
        m_display = estimate;
    }

    void VblankLock::Observe(int64_t vblank, int64_t presented, int64_t onScreen)
    {
        std::lock_guard lock(m_mutex);
        uint32_t slot = m_count++ % kHistory;
        m_ready[slot] = float(presented - vblank);
        m_latency[slot] = float(onScreen - vblank);
        m_observed++;
        if (!m_locked)
        {
            m_late[slot] = 0;
            return;
        }
        const double period = m_display.periodNs;
        m_late[slot] = int32_t(std::llround((double(onScreen - vblank) - m_leadNs) / period));
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

    bool VblankLock::Locked() const
    {
        std::lock_guard lock(m_mutex);
        return m_locked;
    }

    double VblankLock::PeriodNs() const
    {
        std::lock_guard lock(m_mutex);
        return m_locked ? m_display.periodNs : double(m_fallbackNs);
    }

    VblankLock::Status VblankLock::GetStatus() const
    {
        std::lock_guard lock(m_mutex);
        Status s;
        s.locked = m_locked;
        s.periodNs = m_locked ? m_display.periodNs : double(m_fallbackNs);
        s.leadNs = m_leadNs;
        s.errorNs = m_errorNs;
        s.slips = m_slips;
        s.follows = m_follows;
        s.observed = m_observed;
        s.display = m_display;
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
