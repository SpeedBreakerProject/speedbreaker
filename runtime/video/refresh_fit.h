// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The display's refresh period and phase, measured from the times presents
// were seen on screen (VK_KHR_present_wait, video/presenter.cpp). Neither the
// display mode nor the windowing system can be asked: under gamescope, SDL
// and XRandR report gamescope's own CVT mode (59.9356 Hz for a monitor that
// runs at 59.9726 Hz).
//
// Completions land on the display's refresh grid, one refresh or more apart
// (a present that missed, or none to show). Each gets an integer refresh
// index from its gap to the one before, so missed refreshes don't bend the
// fit, and a least-squares line through (index, time) gives the period; gaps
// longer than kMaxGap refreshes start a new segment, which shares the period
// but has its own offset. The window is the last 20 s (at least the newest
// 720 completions, so a long load keeps the estimate). Completions far off
// the line (a late wake-up) are set aside and the line refitted without them.
// A new display rate shows as a window that fits badly while its last 3 s fit
// well: the older completions are dropped and the window refills.
#pragma once
#include <cstdint>
#include <deque>
#include <vector>

namespace video
{
    struct RefreshEstimate
    {
        bool valid = false;     // accepted: 58.5-61.5 Hz, at least 10 s of refreshes, small residuals
        double periodNs = 0;    // one refresh
        int64_t gridNs = 0;     // a refresh (the fitted time of the newest completion)
        double rmsNs = 0;       // of the completions kept
        uint32_t samples = 0;   // completions in the window
        uint32_t rejected = 0;  // of which set aside as off the line
        double spanNs = 0;      // refresh time the window covers (its segments together)
    };

    class RefreshFit
    {
    public:
        // `t`, ns on the steady clock: a present was seen complete. In order.
        void Add(int64_t t);
        const RefreshEstimate& Estimate() const { return m_estimate; }

        // Acceptance limits: a 60 Hz display, give or take 2.5% (the guest
        // runs at the display's rate, and its game time with it).
        static constexpr double kMinHz = 58.5, kMaxHz = 61.5;
        static constexpr double kMaxRmsNs = 500'000;      // 0.5 ms
        static constexpr double kMinSpanNs = 10e9;        // 10 s
        static constexpr double kMaxRejected = 0.1;       // of the window

    private:
        struct Sample
        {
            int64_t t;
            int64_t k;         // refresh index within its segment
            uint32_t segment;
        };
        struct Line
        {
            bool ok = false;
            double periodNs = 0, rmsNs = 0, spanNs = 0;
            int64_t gridNs = 0;
            uint32_t used = 0, rejected = 0;
        };
        // Fits samples [first, end): two passes, the second without outliers.
        Line Fit(size_t first) const;
        bool Acceptable(const Line& line, bool needSpan) const;

        static constexpr int64_t kWindowNs = 20'000'000'000;
        static constexpr size_t kKeepSamples = 720;
        static constexpr int64_t kRecentNs = 3'000'000'000;
        static constexpr int64_t kMaxGap = 8;  // refreshes; longer gaps start a segment
        static constexpr double kNominalNs = 1e9 / 60.0;

        std::deque<Sample> m_samples;
        uint32_t m_segment = 0;
        RefreshEstimate m_estimate;
        mutable std::vector<double> m_scratch;
    };
}
