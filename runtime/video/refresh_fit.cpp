// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See refresh_fit.h.
#include "refresh_fit.h"

#include <algorithm>
#include <cmath>

namespace video
{
    void RefreshFit::Add(int64_t t)
    {
        Sample s{ t, 0, m_segment };
        if (!m_samples.empty())
        {
            const Sample& last = m_samples.back();
            double period = m_estimate.valid ? m_estimate.periodNs : kNominalNs;
            int64_t n = std::llround(double(t - last.t) / period);
            if (n < 1)
                return;  // within half a refresh of the last: a late wake-up, not a refresh
            if (n > kMaxGap)
                s.segment = ++m_segment;
            else
                s.k = last.k + n;
        }
        m_samples.push_back(s);
        while (m_samples.size() > kKeepSamples && m_samples.front().t < t - kWindowNs)
            m_samples.pop_front();

        Line line = Fit(0);
        if (!Acceptable(line, false))
        {
            // A new rate: the last few seconds agree with each other but not
            // with the rest. Keep only them.
            size_t recent = m_samples.size();
            while (recent > 0 && m_samples[recent - 1].t >= t - kRecentNs)
                recent--;
            if (recent > 0 && m_samples.size() - recent >= 60)
            {
                Line tail = Fit(recent);
                if (Acceptable(tail, false))
                {
                    m_samples.erase(m_samples.begin(), m_samples.begin() + recent);
                    line = tail;
                }
            }
        }
        m_estimate.valid = Acceptable(line, true);
        m_estimate.samples = uint32_t(m_samples.size());
        m_estimate.rejected = line.rejected;
        m_estimate.rmsNs = line.rmsNs;
        m_estimate.spanNs = line.spanNs;
        if (line.ok)
        {
            m_estimate.periodNs = line.periodNs;
            m_estimate.gridNs = line.gridNs;
        }
    }

    bool RefreshFit::Acceptable(const Line& line, bool needSpan) const
    {
        if (!line.ok || line.rmsNs > kMaxRmsNs || line.rejected > kMaxRejected * double(line.used + line.rejected))
            return false;
        double hz = 1e9 / line.periodNs;
        return hz >= kMinHz && hz <= kMaxHz && (!needSpan || line.spanNs >= kMinSpanNs);
    }

    RefreshFit::Line RefreshFit::Fit(size_t first) const
    {
        Line line;
        const size_t count = m_samples.size() - first;
        if (count < 3)
            return line;
        // Common slope, one offset per segment: centre each segment on its
        // own means, so the slope is sum(Sxy) / sum(Sxx) over segments.
        auto solve = [&](double threshold, double& period, uint32_t& used) {
            double sxy = 0, sxx = 0;
            used = 0;
            size_t i = first;
            while (i < m_samples.size())
            {
                size_t j = i;
                double sk = 0, st = 0;
                uint32_t n = 0;
                const Sample& origin = m_samples[i];
                for (; j < m_samples.size() && m_samples[j].segment == origin.segment; j++)
                    if (m_scratch[j - first] <= threshold)
                    {
                        sk += double(m_samples[j].k - origin.k);
                        st += double(m_samples[j].t - origin.t);
                        n++;
                    }
                if (n)
                {
                    double mk = sk / n, mt = st / n;
                    for (size_t m = i; m < j; m++)
                        if (m_scratch[m - first] <= threshold)
                        {
                            double dk = double(m_samples[m].k - origin.k) - mk;
                            sxy += dk * (double(m_samples[m].t - origin.t) - mt);
                            sxx += dk * dk;
                        }
                    used += n;
                }
                i = j;
            }
            if (sxx <= 0)
                return false;
            period = sxy / sxx;
            return period > 0;
        };
        // Residuals from a line with this period, each segment through its
        // kept samples' means; returns the newest sample's fitted time.
        auto residuals = [&](double period, double threshold, std::vector<double>& out, double& rms, double& span) {
            out.assign(count, 0);
            rms = 0;
            span = 0;
            uint32_t kept = 0;
            int64_t grid = 0;
            size_t i = first;
            while (i < m_samples.size())
            {
                size_t j = i;
                double sum = 0;
                uint32_t n = 0;
                const Sample& origin = m_samples[i];
                for (; j < m_samples.size() && m_samples[j].segment == origin.segment; j++)
                    if (m_scratch[j - first] <= threshold)
                    {
                        sum += double(m_samples[j].t - origin.t) - period * double(m_samples[j].k - origin.k);
                        n++;
                    }
                double offset = n ? sum / n : 0.0;
                for (size_t m = i; m < j; m++)
                {
                    double r = double(m_samples[m].t - origin.t) - period * double(m_samples[m].k - origin.k) - offset;
                    out[m - first] = std::abs(r);
                    if (m_scratch[m - first] <= threshold)
                    {
                        rms += r * r;
                        kept++;
                    }
                }
                span += period * double(m_samples[j - 1].k - origin.k);
                grid = origin.t + int64_t(std::llround(offset + period * double(m_samples[j - 1].k - origin.k)));
                i = j;
            }
            rms = kept ? std::sqrt(rms / kept) : 0.0;
            return grid;
        };

        m_scratch.assign(count, 0.0);
        constexpr double kAll = 1e300;
        double period = 0;
        uint32_t used = 0;
        if (!solve(kAll, period, used))
            return line;
        std::vector<double> dev;
        double rms, span;
        residuals(period, kAll, dev, rms, span);
        // Outliers: beyond 4 robust standard deviations (1.4826 MAD), with a
        // floor so a clean stream keeps its jitter.
        std::vector<double> sorted = dev;
        std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
        double threshold = std::max(4.0 * 1.4826 * sorted[sorted.size() / 2], 200'000.0);
        m_scratch = dev;
        if (!solve(threshold, period, used))
            return line;
        line.gridNs = residuals(period, threshold, dev, rms, span);
        line.ok = true;
        line.periodNs = period;
        line.rmsNs = rms;
        line.spanNs = span;
        line.used = used;
        line.rejected = uint32_t(count) - used;
        return line;
    }
}
