// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See frame_rate_policy.h.
#include "frame_rate_policy.h"

#include <algorithm>
#include <utility>

namespace video
{
    void AutoFrameRatePolicy::Reset()
    {
        *this = AutoFrameRatePolicy();
    }

    FrameRateDecision AutoFrameRatePolicy::Update(double now, platform::thermal::State thermal, const FrameSample& sample)
    {
        using platform::thermal::State;
        // Unknown (no reading on this system) counts as fair.
        const bool hot = thermal == State::Serious || thermal == State::Critical;
        if (hot)
            m_lastHot = now;

        // At 30, gameplay only: a menu or a load after a race would read as
        // room for 60 the next race doesn't have.
        const bool gameplay = sample.draws >= kGameplayDraws && sample.cpMs >= kGameplayCpMs;
        if (sample.frames > 0 && sample.guestSeconds > 0 && (m_rate == 60 || gameplay))
            m_window.push_back(sample);
        double seconds = 0;
        for (const FrameSample& s : m_window)
            seconds += s.guestSeconds;
        // The newest kWindowSec: the oldest sample goes once the rest cover it.
        while (m_window.size() > 1 && seconds - m_window.front().guestSeconds >= kWindowSec)
        {
            seconds -= m_window.front().guestSeconds;
            m_window.pop_front();
        }

        FrameRateDecision d;
        d.windowSeconds = seconds;
        {
            int frames = 0, slowFrames = 0;
            double cp = 0, gpu = 0, slowCp = 0, slowGpu = 0;
            m_sorted.clear();
            for (const FrameSample& s : m_window)
            {
                const double fps = s.frames / s.guestSeconds;
                frames += s.frames;
                cp += double(s.cpMs) * s.frames;
                gpu += double(s.gpuMs) * s.frames;
                if (fps < kDropFps)
                {
                    slowFrames += s.frames;
                    slowCp += double(s.cpMs) * s.frames;
                    slowGpu += double(s.gpuMs) * s.frames;
                }
                m_sorted.push_back({ fps, s.frames });
            }
            if (frames > 0)
            {
                d.fps = float(frames / seconds);
                d.cpMs = float(cp / frames);
                d.gpuMs = float(gpu / frames);
                // The frame rate half the window's frames ran at or under.
                std::sort(m_sorted.begin(), m_sorted.end());
                int below = 0;
                for (const auto& [fps, n] : m_sorted)
                    if ((below += n) * 2 >= frames)
                    {
                        d.medianFps = float(fps);
                        break;
                    }
            }
            if (slowFrames > 0)
            {
                d.slowCpMs = float(slowCp / slowFrames);
                d.slowGpuMs = float(slowGpu / slowFrames);
            }
        }
        d.rateSeconds = m_lastSwitch == kLongAgo ? 0.0 : now - m_lastSwitch;
        const bool full = d.windowSeconds >= kWindowSec;
        const bool dwelt = now - m_lastSwitch >= kDwellSec;
        const double scale = double(1 << m_backoff);

        if (m_rate == 60)
        {
            const bool cantHold = full && d.medianFps < kDropFps && std::max(d.slowCpMs, d.slowGpuMs) >= kDropCostMs;
            if (dwelt && (hot || cantHold))
            {
                // Dropped again before the return settled: wait longer next
                // time (a phone that heats up again 2-4 minutes after a
                // return backs off as surely as a scene that can't hold 60).
                if (now - m_lastRise < kSettleSec)
                    m_backoff = std::min(m_backoff + 1, kBackoffMax);
                d.changed = true;
                d.reason = hot ? FrameRateReason::Hot : FrameRateReason::CantHold60;
            }
            else if (now - m_lastSwitch >= kSettleSec)
                m_backoff = 0;
        }
        else
        {
            d.coolSeconds = hot ? 0.0 : now - std::max(m_lastSwitch, m_lastHot);
            const bool cool = !hot && d.coolSeconds >= kCoolSec * scale;
            const bool headroom = full && d.cpMs <= kRiseCpMs && d.gpuMs <= kRiseGpuMs;
            const bool tryIt = full && now - m_lastSwitch >= kTrySec * scale && d.cpMs <= kRiseCpMs;
            if (dwelt && cool && (headroom || tryIt))
            {
                d.changed = true;
                d.reason = headroom ? FrameRateReason::Headroom : FrameRateReason::Try;
                m_lastRise = now;
            }
        }

        if (d.changed)
        {
            m_rate = m_rate == 60 ? 30 : 60;
            m_lastSwitch = now;
            m_window.clear();  // the other rate's frames say nothing about this one's
        }
        d.rate = m_rate;
        d.backoff = m_backoff;
        return d;
    }
}
