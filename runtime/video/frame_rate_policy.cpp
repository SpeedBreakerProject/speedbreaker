// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See frame_rate_policy.h.
#include "frame_rate_policy.h"

#include <algorithm>
#include <utility>

namespace video
{
    void AutoFrameRatePolicy::Reset()
    {
        const bool gentle = m_gentle;
        *this = AutoFrameRatePolicy();
        m_gentle = gentle;
    }

    bool AutoFrameRatePolicy::DropsOnState(platform::thermal::State thermal, bool gentle)
    {
        using platform::thermal::State;
        return thermal == State::Critical || (!gentle && thermal == State::Serious);
    }

    FrameRateDecision AutoFrameRatePolicy::Update(double now, platform::thermal::State thermal, const FrameSample& sample)
    {
        using platform::thermal::State;
        // Unknown (no reading on this system) counts as fair.
        const bool hot = thermal == State::Serious || thermal == State::Critical;
        if (hot)
            m_lastHot = now;
        // What drops at 60 on the state alone, and the median under which
        // 60 isn't held (gentle at serious: a shade under 60).
        const bool dropsOnState = DropsOnState(thermal, m_gentle);
        const double dropFps = m_gentle && thermal == State::Serious ? kWarmDropFps : kDropFps;

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
                if (fps < dropFps)
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
            const bool cantHold = full && d.medianFps < dropFps && std::max(d.slowCpMs, d.slowGpuMs) >= kDropCostMs;
            if (dwelt && (dropsOnState || cantHold))
            {
                // Dropped again before the return settled: wait longer next
                // time (a phone that heats up again 2-4 minutes after a
                // return backs off as surely as a scene that can't hold 60).
                if (now - m_lastRise < kSettleSec)
                    m_backoff = std::min(m_backoff + 1, kBackoffMax);
                d.changed = true;
                d.reason = dropsOnState ? FrameRateReason::Hot : FrameRateReason::CantHold60;
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
            // Gentle: a try may come while still serious (not critical).
            const bool warmTry = m_gentle && !cool && thermal == State::Serious && tryIt;
            if (dwelt && ((cool && (headroom || tryIt)) || warmTry))
            {
                d.changed = true;
                d.reason = cool && headroom ? FrameRateReason::Headroom : FrameRateReason::Try;
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
