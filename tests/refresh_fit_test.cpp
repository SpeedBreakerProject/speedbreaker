// Synthetic-timestamp tests for video::RefreshFit (runtime/video/refresh_fit.cpp)
// and which displays it is told are a whole multiple of 60 Hz
// (runtime/video/refresh_multiple.h). Sections 1-8 are the 60 Hz fit's (as in
// v0.1.6); 9-15 the multiples (120, 180, 240 Hz), the rates that aren't
// (143.9, 90 Hz), jitter, the policy, and planted wrong configurations that
// must each fail a check. From the repo root:
//   clang++ -std=c++20 -O2 -Iruntime tests/refresh_fit_test.cpp runtime/video/refresh_fit.cpp -o build/refresh_fit_test
//   build/refresh_fit_test
#include <video/refresh_fit.h>
#include <video/refresh_multiple.h>

#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
#include <string>

using video::RefreshFit;
using video::RefreshEstimate;

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct Display
{
    double hz;
    double phaseNs = 3'141'592;  // a refresh at this time
    double Refresh(int64_t k) const { return phaseNs + double(k) * 1e9 / hz; }
};

// Feeds completions for refreshes [k0, k1) where `present(k)` says a present
// completes on that refresh; `noise(k)` is the wake-up delay added.
struct Feed
{
    RefreshFit fit;
    int64_t last = 0;
    void Run(const Display& d, int64_t k0, int64_t k1, const std::function<bool(int64_t)>& present,
        const std::function<double(int64_t)>& noise, const std::function<void(int64_t, const RefreshEstimate&)>& each = nullptr)
    {
        for (int64_t k = k0; k < k1; k++)
            if (present(k))
            {
                last = int64_t(d.Refresh(k) + noise(k));
                fit.Add(last);
                if (each)
                    each(k, fit.Estimate());
            }
    }
};

// How far the fitted grid is from the true one (ns, wrapped to a period).
static double PhaseError(const Display& d, const RefreshEstimate& e)
{
    double p = 1e9 / d.hz;
    double k = std::round((double(e.gridNs) - d.phaseNs) / p);
    return double(e.gridNs) - d.Refresh(int64_t(k));
}

int main()
{
    std::mt19937_64 rng(1234);
    auto gauss = [&](double sigma) { return std::normal_distribution<double>(0.0, sigma)(rng); };
    auto uniform = [&]() { return std::uniform_real_distribution<double>(0.0, 1.0)(rng); };
    const double kWake = 150'000;  // mean wake-up delay after the refresh

    printf("1. clean 59.972616 Hz, 0.2 ms jitter, every refresh, 30 s\n");
    {
        Display d{ 59.972616 };
        Feed f;
        int64_t firstValid = -1;
        f.Run(d, 0, 1800, [](int64_t) { return true; }, [&](int64_t) { return kWake + gauss(200'000); },
            [&](int64_t k, const RefreshEstimate& e) { if (e.valid && firstValid < 0) firstValid = k; });
        const auto& e = f.fit.Estimate();
        double hz = 1e9 / e.periodNs;
        printf("   %.6f Hz (error %.2e Hz), rms %.0f us, phase error %.0f us, valid after %.1f s\n", hz, hz - d.hz, e.rmsNs / 1e3,
            (PhaseError(d, e) - kWake) / 1e3, firstValid / d.hz);
        CHECK(e.valid, "not valid");
        CHECK(std::abs(hz - d.hz) < 2e-4, "rate %.6f", hz);
        CHECK(std::abs(PhaseError(d, e) - kWake) < 100'000, "phase %.0f", PhaseError(d, e));
        CHECK(firstValid > 0 && firstValid / d.hz < 10.5, "valid after %.1f s", firstValid / d.hz);
    }

    printf("2. 30%% of refreshes without a present (gaps of 2-4), 1%% late wake-ups of 2-6 ms\n");
    {
        Display d{ 59.972616 };
        Feed f;
        f.Run(d, 0, 2400, [&](int64_t) { return uniform() > 0.3; },
            [&](int64_t) { return kWake + gauss(200'000) + (uniform() < 0.01 ? 2e6 + 4e6 * uniform() : 0.0); });
        const auto& e = f.fit.Estimate();
        double hz = 1e9 / e.periodNs;
        printf("   %.6f Hz (error %.2e Hz), rms %.0f us, %u of %u set aside, phase error %.0f us\n", hz, hz - d.hz, e.rmsNs / 1e3,
            e.rejected, e.samples, (PhaseError(d, e) - kWake) / 1e3);
        CHECK(e.valid, "not valid");
        CHECK(std::abs(hz - d.hz) < 3e-4, "rate %.6f", hz);
        CHECK(e.rejected > 0, "no outliers found");
    }

    printf("3. a 30 s gap (loading) then presents again: the estimate holds and the phase follows\n");
    {
        Display d{ 59.972616 };
        Feed f;
        f.Run(d, 0, 900, [](int64_t) { return true; }, [&](int64_t) { return kWake + gauss(200'000); });
        CHECK(f.fit.Estimate().valid, "not valid before the gap");
        bool validAfter = true;
        f.Run(d, 900 + 1800, 900 + 1800 + 120, [](int64_t) { return true; }, [&](int64_t) { return kWake + gauss(200'000); },
            [&](int64_t, const RefreshEstimate& e) { validAfter = validAfter && e.valid; });
        const auto& e = f.fit.Estimate();
        printf("   valid throughout: %s, %.6f Hz, phase error %.0f us\n", validAfter ? "yes" : "no", 1e9 / e.periodNs,
            (PhaseError(d, e) - kWake) / 1e3);
        CHECK(validAfter, "lost the estimate after the gap");
        CHECK(std::abs(PhaseError(d, e) - kWake) < 150'000, "phase %.0f", PhaseError(d, e));
    }

    printf("4. the rate changes from 59.972616 to 61 Hz\n");
    {
        Display a{ 59.972616 }, b{ 61.0, 7'000'000 };
        Feed f;
        f.Run(a, 0, 1800, [](int64_t) { return true; }, [&](int64_t) { return kWake + gauss(200'000); });
        CHECK(f.fit.Estimate().valid, "not valid on the first rate");
        // b's refreshes after a's last.
        int64_t start = int64_t(std::ceil((a.Refresh(1800) - b.phaseNs) * b.hz / 1e9));
        int64_t invalidAt = -1, validAt = -1;
        f.Run(b, start, start + 1800, [](int64_t) { return true; }, [&](int64_t) { return kWake + gauss(200'000); },
            [&](int64_t k, const RefreshEstimate& e) {
                if (!e.valid && invalidAt < 0) invalidAt = k - start;
                if (!e.valid) validAt = -1; else if (validAt < 0 && invalidAt >= 0) validAt = k - start;
            });
        const auto& e = f.fit.Estimate();
        printf("   invalid after %.2f s, valid again after %.1f s at %.6f Hz\n", invalidAt / b.hz, validAt / b.hz, 1e9 / e.periodNs);
        CHECK(invalidAt >= 0 && invalidAt / b.hz < 2.5, "old rate kept %.2f s", invalidAt / b.hz);
        CHECK(validAt > 0 && validAt / b.hz < 14.0, "new rate after %.1f s", validAt / b.hz);
        CHECK(e.valid && std::abs(1e9 / e.periodNs - 61.0) < 2e-4, "rate %.6f", 1e9 / e.periodNs);
    }

    printf("5. rates outside 58.5-61.5 Hz are measured but not accepted (50, 75 Hz)\n");
    for (double hz : { 50.0, 75.0 })
    {
        Display d{ hz };
        Feed f;
        f.Run(d, 0, int64_t(hz * 20), [](int64_t) { return true; }, [&](int64_t) { return kWake + gauss(200'000); });
        printf("   %.0f Hz display: valid %s\n", hz, f.fit.Estimate().valid ? "yes" : "no");
        CHECK(!f.fit.Estimate().valid, "accepted %.0f Hz", hz);
    }

    printf("5b. the nested gamescope test rates, 59 and 61 Hz, are accepted\n");
    for (double hz : { 59.0, 61.0 })
    {
        Display d{ hz };
        Feed f;
        f.Run(d, 0, int64_t(hz * 15), [](int64_t) { return true; }, [&](int64_t) { return kWake + gauss(200'000); });
        printf("   %.0f Hz display: valid %s, %.5f Hz\n", hz, f.fit.Estimate().valid ? "yes" : "no", 1e9 / f.fit.Estimate().periodNs);
        CHECK(f.fit.Estimate().valid, "rejected %.0f Hz", hz);
    }

    printf("6. a 120 Hz display with a present on alternate refreshes and 10%% odd ones: not accepted\n");
    {
        Display d{ 120.0 };
        Feed f;
        f.Run(d, 0, 2400, [&](int64_t k) { return k % 2 == 0 || uniform() < 0.1; }, [&](int64_t) { return kWake + gauss(200'000); });
        printf("   valid %s, rms %.0f us\n", f.fit.Estimate().valid ? "yes" : "no", f.fit.Estimate().rmsNs / 1e3);
        CHECK(!f.fit.Estimate().valid, "accepted a mixed 120 Hz cadence");
    }

    printf("7. heavy jitter (1.5 ms wake-ups): not accepted\n");
    {
        Display d{ 59.972616 };
        Feed f;
        f.Run(d, 0, 1800, [](int64_t) { return true; }, [&](int64_t) { return kWake + std::abs(gauss(1'500'000)); });
        printf("   valid %s, rms %.0f us\n", f.fit.Estimate().valid ? "yes" : "no", f.fit.Estimate().rmsNs / 1e3);
        CHECK(!f.fit.Estimate().valid, "accepted a 1.5 ms jitter stream");
    }

    printf("8. the rate the old guest vblank and gamescope's CVT mode would give are told apart\n");
    for (double hz : { 59.9988, 59.9356 })
    {
        Display d{ hz };
        Feed f;
        f.Run(d, 0, 1200, [](int64_t) { return true; }, [&](int64_t) { return kWake + gauss(300'000); });
        double got = 1e9 / f.fit.Estimate().periodNs;
        printf("   %.4f Hz measured as %.5f Hz\n", hz, got);
        CHECK(std::abs(got - hz) < 5e-4 && std::abs(got - 59.972616) > 0.02, "%.5f", got);
    }

    // ---- Whole multiples of 60 Hz (refresh_multiple.h, vblank_lock.h) ----
    // A 60 fps game on a display of `multiple` x 60 Hz: a new frame every
    // `multiple` refreshes, `odd` of them a refresh early or late (gaps of
    // multiple -+ 1: the 1-and-3 pairs at 120 Hz), plus wake-up jitter.
    struct MultipleRun
    {
        bool valid = false;
        double hz = 0, rmsUs = 0, phaseUs = 0, validAfterS = -1;
        int multiple = 0;
    };
    auto runMultiple = [&](double hz, int multiple, double seconds, double odd, double jitterNs, bool absJitter = false) {
        Display d{ hz };
        Feed f;
        f.fit.SetMultiple(multiple);
        const int every = std::max(1, int(std::lround(hz / 60.0)));
        int64_t next = 0, firstValid = -1;
        f.Run(d, 0, int64_t(hz * seconds),
            [&](int64_t k) {
                if (k < next)
                    return false;
                // The next frame `every` refreshes on, or one refresh either side.
                next = k + every + (uniform() < odd ? (uniform() < 0.5 ? -1 : 1) : 0);
                return true;
            },
            [&](int64_t) { return kWake + (absJitter ? std::abs(gauss(jitterNs)) : gauss(jitterNs)); },
            [&](int64_t k, const RefreshEstimate& e) { if (e.valid && firstValid < 0) firstValid = k; });
        const RefreshEstimate& e = f.fit.Estimate();
        MultipleRun r;
        r.valid = e.valid;
        r.hz = e.periodNs > 0 ? 1e9 / e.periodNs : 0;
        r.rmsUs = e.rmsNs / 1e3;
        r.phaseUs = (PhaseError(d, e) - kWake) / 1e3;
        r.validAfterS = firstValid >= 0 ? firstValid / hz : -1;
        r.multiple = e.multiple;
        return r;
    };

    printf("9. 60, 120, 180 and 240 Hz (and 119.88, 239.76) told their multiple: a frame every 1-4 refreshes, 5%% a refresh early or late, "
           "0.2 ms jitter, 30 s\n");
    for (double hz : { 60.0, 59.94, 120.0, 119.88, 180.0, 240.0, 239.76 })
    {
        const int m = RefreshFit::MultipleOf(hz);
        MultipleRun r = runMultiple(hz, m, 30, 0.05, 200'000);
        printf("   %7.2f Hz, multiple %d: valid %s after %.1f s, %.5f Hz (error %.1e), rms %.0f us, phase error %.0f us\n", hz, m,
            r.valid ? "yes" : "no", r.validAfterS, r.hz, r.hz - hz, r.rmsUs, r.phaseUs);
        CHECK(r.valid && r.multiple == m, "%.2f Hz not accepted as %d", hz, m);
        CHECK(std::abs(r.hz - hz) < 2e-4 * m, "%.2f Hz measured %.5f", hz, r.hz);
        CHECK(std::abs(r.phaseUs) < 100, "%.2f Hz phase %.0f us", hz, r.phaseUs);
        CHECK(r.validAfterS > 0 && r.validAfterS < 10.5, "%.2f Hz valid after %.1f s", hz, r.validAfterS);
    }

    printf("9b. a free-running 59.9988 Hz guest on a 119.88 Hz display (before the lock): frames shown on the first refresh after "
           "vblank + 2-14 ms, so gaps of 1, 2 and 3 refreshes, all on the grid\n");
    {
        Display d{ 119.88 };
        RefreshFit fit;
        fit.SetMultiple(2);
        const double p = 1e9 / d.hz;
        int64_t lastShown = INT64_MIN;
        for (int64_t i = 0; i < 60 * 30; i++)
        {
            const double ready = double(i) * 16'667'000.0 + 2e6 + 12e6 * uniform();
            int64_t k = int64_t(std::ceil((ready - d.phaseNs) / p));
            k = std::max(k, lastShown + 1);  // one frame a refresh at most (FIFO)
            lastShown = k;
            fit.Add(int64_t(d.Refresh(k) + kWake + gauss(200'000)));
        }
        const RefreshEstimate& e = fit.Estimate();
        printf("   valid %s, %.5f Hz, rms %.0f us\n", e.valid ? "yes" : "no", 1e9 / e.periodNs, e.rmsNs / 1e3);
        CHECK(e.valid && std::abs(1e9 / e.periodNs - d.hz) < 4e-4, "%.5f Hz", 1e9 / e.periodNs);
    }

    printf("10. no whole multiple: 143.9, 144 and 165 Hz (a 60 fps frame every 2 or 3 refreshes) and 90 Hz (1 or 2): MultipleOf 0, "
           "the fit stays at 1 and doesn't accept them\n");
    for (double hz : { 143.9, 144.0, 165.0, 90.0 })
    {
        CHECK(RefreshFit::MultipleOf(hz) == 0, "%.1f Hz has multiple %d", hz, RefreshFit::MultipleOf(hz));
        const video::RefreshMultiple policy;
        CHECK(policy.For(hz, false) == 1, "%.1f Hz: For %d", hz, policy.For(hz, false));
        Display d{ hz };
        Feed f;
        f.fit.SetMultiple(policy.For(hz, false));
        const double p = 1e9 / hz;
        int64_t lastShown = INT64_MIN;
        bool everValid = false;
        for (int64_t i = 0; i < 60 * 30; i++)
        {
            const double ready = d.phaseNs + double(i) * 1e9 / 60.0 + 3e6 + gauss(500'000);
            int64_t k = std::max(int64_t(std::ceil((ready - d.phaseNs) / p)), lastShown + 1);
            lastShown = k;
            f.fit.Add(int64_t(d.Refresh(k) + kWake + gauss(200'000)));
            everValid |= f.fit.Estimate().valid;
        }
        printf("   %6.1f Hz: valid %s (ever: %s), fitted %.3f Hz, rms %.0f us\n", hz, f.fit.Estimate().valid ? "yes" : "no",
            everValid ? "yes" : "no", 1e9 / f.fit.Estimate().periodNs, f.fit.Estimate().rmsNs / 1e3);
        CHECK(!everValid, "%.1f Hz accepted", hz);
        CHECK(policy.Describe(hz, 1, false).find("no whole multiple") != std::string::npos, "%.1f Hz: no line saying why", hz);
    }

    printf("11. jitter at 120 and 240 Hz: 0.3 ms accepted, 1.5 ms wake-ups not\n");
    for (double hz : { 120.0, 240.0 })
    {
        MultipleRun ok = runMultiple(hz, RefreshFit::MultipleOf(hz), 30, 0.02, 300'000);
        MultipleRun bad = runMultiple(hz, RefreshFit::MultipleOf(hz), 30, 0.02, 1'500'000, true);
        printf("   %.0f Hz: 0.3 ms valid %s (rms %.0f us), 1.5 ms valid %s (rms %.0f us)\n", hz, ok.valid ? "yes" : "no", ok.rmsUs,
            bad.valid ? "yes" : "no", bad.rmsUs);
        CHECK(ok.valid && std::abs(ok.hz - hz) < 5e-4 * RefreshFit::MultipleOf(hz), "%.0f Hz at 0.3 ms: %.5f", hz, ok.hz);
        CHECK(!bad.valid, "%.0f Hz accepted a 1.5 ms jitter stream", hz);
    }

    printf("12. MultipleOf at and around the limits\n");
    {
        const struct { double hz; int m; } rows[] = { { 0, 0 }, { -1, 0 }, { 30, 0 }, { 50, 0 }, { 58.49, 0 }, { 58.5, 1 }, { 59.94, 1 },
            { 61.5, 1 }, { 61.51, 0 }, { 75, 0 }, { 90, 0 }, { 100, 0 }, { 116.9, 0 }, { 117, 2 }, { 119.88, 2 }, { 120, 2 }, { 123, 2 },
            { 123.1, 0 }, { 143.9, 0 }, { 144, 0 }, { 165, 0 }, { 175.4, 0 }, { 175.5, 3 }, { 180, 3 }, { 184.5, 3 }, { 200, 0 },
            { 233.9, 0 }, { 234, 4 }, { 239.76, 4 }, { 240, 4 }, { 246, 4 }, { 246.1, 0 }, { 300, 0 }, { 360, 0 } };
        int bad = 0;
        for (const auto& r : rows)
            if (RefreshFit::MultipleOf(r.hz) != r.m)
            {
                bad++;
                CHECK(false, "MultipleOf(%.2f) = %d, want %d", r.hz, RefreshFit::MultipleOf(r.hz), r.m);
            }
        printf("   %zu rates, %d wrong\n", std::size(rows), bad);
    }

    printf("13. a new multiple starts the fit over: valid at 60 Hz, told 2 (the window moved to a 120 Hz display), valid again at 120\n");
    {
        Display a{ 59.972616 };
        Feed f;
        f.Run(a, 0, 900, [](int64_t) { return true; }, [&](int64_t) { return kWake + gauss(200'000); });
        CHECK(f.fit.Estimate().valid && f.fit.Estimate().multiple == 1, "not valid at 60 Hz");
        f.fit.SetMultiple(2);
        CHECK(!f.fit.Estimate().valid && f.fit.Estimate().multiple == 2 && f.fit.Estimate().samples == 0, "the old estimate kept");
        f.fit.SetMultiple(2);  // the same again: nothing to start over
        Display b{ 120.0, 2'000'000 };
        int64_t start = int64_t(std::ceil((a.Refresh(900) - b.phaseNs) * b.hz / 1e9)), validAt = -1;
        f.Run(b, start, start + 120 * 15, [](int64_t k) { return k % 2 == 0; }, [&](int64_t) { return kWake + gauss(200'000); },
            [&](int64_t k, const RefreshEstimate& e) { if (e.valid && validAt < 0) validAt = k - start; });
        const RefreshEstimate& e = f.fit.Estimate();
        printf("   valid again after %.1f s at %.5f Hz (multiple %d)\n", validAt / b.hz, 1e9 / e.periodNs, e.multiple);
        // 15 s of completions at 0.2 ms jitter: within 1e-3 Hz (8 ppm; libc++ and libstdc++ draw
        // other noise: 120.00033 and 120.00042 Hz).
        CHECK(e.valid && std::abs(1e9 / e.periodNs - 120.0) < 1e-3 && validAt / b.hz < 10.5, "%.5f Hz", 1e9 / e.periodNs);
        f.fit.SetMultiple(9);  // out of range: 1
        CHECK(f.fit.Multiple() == 1 && !f.fit.Estimate().valid, "multiple %d", f.fit.Multiple());
    }

    printf("14. the policy (NFSMW_REFRESH_MULTIPLE, the platform, the mode's rate) and its log line\n");
    {
        using video::RefreshMultiple;
        const RefreshMultiple autoMode = RefreshMultiple::FromEnv(nullptr), off = RefreshMultiple::FromEnv("0"),
                              on = RefreshMultiple::FromEnv("1"), three = RefreshMultiple::FromEnv("3"),
                              junk = RefreshMultiple::FromEnv("7"), empty = RefreshMultiple::FromEnv("");
        CHECK(autoMode.mode == RefreshMultiple::Mode::Auto && empty.mode == RefreshMultiple::Mode::Auto &&
                  junk.mode == RefreshMultiple::Mode::Auto, "parsing");
        CHECK(off.mode == RefreshMultiple::Mode::Off && on.mode == RefreshMultiple::Mode::On &&
                  three.mode == RefreshMultiple::Mode::Forced && three.forced == 3, "parsing");
        struct Row { const RefreshMultiple* p; const char* name; double hz; bool apple; int want; };
        const Row rows[] = {
            { &autoMode, "auto", 59.94, false, 1 }, { &autoMode, "auto", 119.88, false, 2 }, { &autoMode, "auto", 180, false, 3 },
            { &autoMode, "auto", 240, false, 4 }, { &autoMode, "auto", 144, false, 1 }, { &autoMode, "auto", 90, false, 1 },
            { &autoMode, "auto", 0, false, 1 }, { &autoMode, "auto", 120, true, 1 }, { &autoMode, "auto", 60, true, 1 },
            { &on, "on", 120, true, 2 }, { &on, "on", 144, true, 1 }, { &off, "off", 120, false, 1 }, { &off, "off", 240, false, 1 },
            { &three, "3", 60, false, 3 }, { &three, "3", 144, true, 3 },
        };
        for (const Row& r : rows)
            CHECK(r.p->For(r.hz, r.apple) == r.want, "%s at %.2f Hz%s: %d, want %d", r.name, r.hz, r.apple ? " (Apple)" : "",
                r.p->For(r.hz, r.apple), r.want);
        // Lines: none at 60 Hz, at an unknown rate, off, or Auto on Apple's systems (v0.1.6 there).
        CHECK(autoMode.Describe(59.94, 1, false).empty() && autoMode.Describe(0, 1, false).empty(), "a line at 60 Hz or unknown");
        CHECK(autoMode.Describe(120, 1, true).empty() && autoMode.Describe(144, 1, true).empty(), "a line on Apple's systems");
        CHECK(off.Describe(144, 1, false).empty() && off.Describe(120, 1, false).empty(), "a line with NFSMW_REFRESH_MULTIPLE=0");
        const std::string l120 = autoMode.Describe(119.88, 2, false), l240 = autoMode.Describe(240, 4, false),
                          l90 = autoMode.Describe(90, 1, false), l50 = autoMode.Describe(50, 1, false), l300 = autoMode.Describe(300, 1, false);
        printf("   119.88: %s\n   90:     %s\n   50:     %s\n   300:    %s\n", l120.c_str(), l90.c_str(), l50.c_str(), l300.c_str());
        CHECK(l120.find("every 2nd refresh") != std::string::npos && l240.find("every 4th refresh") != std::string::npos, "multiple lines");
        CHECK(l90.find("1 or 2 refreshes") != std::string::npos && l90.find("1.50") != std::string::npos, "90 Hz: %s", l90.c_str());
        CHECK(autoMode.Describe(144, 1, false).find("2 or 3 refreshes (2.40") != std::string::npos, "144 Hz");
        CHECK(l50.find("slower than 60 Hz") != std::string::npos && l300.find("faster than") != std::string::npos, "50 / 300 Hz");
        CHECK(three.Describe(60, 3, false).find("(NFSMW_REFRESH_MULTIPLE)") != std::string::npos, "forced");
    }

    printf("14b. Smooth Motion's rates on SteamOS and Linux (RefreshMultiple::SmoothMotionRate)\n");
    {
        using video::RefreshMultiple;
        const RefreshMultiple autoMode = RefreshMultiple::FromEnv(nullptr), off = RefreshMultiple::FromEnv("0");
        struct Row { double hz; const RefreshMultiple* p; bool want; };
        const Row rows[] = {
            { 119.88, &autoMode, true }, { 120, &autoMode, true }, { 89.89, &autoMode, true }, { 90, &autoMode, true },
            { 91, &autoMode, true },
            { 180, &autoMode, false }, { 240, &autoMode, false }, { 143.9, &autoMode, false }, { 144, &autoMode, false },
            { 165, &autoMode, false }, { 59.94, &autoMode, false }, { 60, &autoMode, false }, { 50, &autoMode, false },
            { 75, &autoMode, false }, { 100, &autoMode, false }, { 0, &autoMode, false }, { 300, &autoMode, false },
            { 120, &off, false }, { 240, &off, false }, { 90, &off, true },
        };
        for (const Row& r : rows)
        {
            const int m = r.p->For(r.hz, false);
            CHECK(RefreshMultiple::SmoothMotionRate(r.hz, m) == r.want, "%.2f Hz (multiple %d): %d, want %d", r.hz, m,
                RefreshMultiple::SmoothMotionRate(r.hz, m), r.want);
        }
        // A multiple the mode doesn't have (forced): not a rate the lock holds.
        CHECK(!RefreshMultiple::SmoothMotionRate(144, 3) && !RefreshMultiple::SmoothMotionRate(60, 2) &&
                  !RefreshMultiple::SmoothMotionRate(180, 3), "a forced multiple, or 180 Hz's");
    }

    printf("15. planted wrong configurations, each must fail a check above\n");
    {
        int caught = 0, plants = 0;
        auto plant = [&](const char* name, bool failed) {
            plants++;
            caught += failed;
            printf("   plant: %-62s %s\n", name, failed ? "caught" : "NOT caught");
            CHECK(failed, "planted \"%s\" passed", name);
        };
        // 9's check (accepted, at the display's own rate) against:
        auto passes9 = [&](double hz, int multiple) {
            MultipleRun r = runMultiple(hz, multiple, 30, 0.05, 200'000);
            return r.valid && std::abs(r.hz - hz) < 2e-4 * std::max(multiple, 1);
        };
        plant("v0.1.6's fit (multiple 1) on a 119.88 Hz display", !passes9(119.88, 1));
        plant("multiple 2 on a 240 Hz display (fits every 2nd refresh)", !passes9(240.0, 2));
        plant("multiple 3 on a 240 Hz display", !passes9(240.0, 3));
        plant("multiple 4 on a 180 Hz display", !passes9(180.0, 4));
        // 10's check (never accepted) against a 143.9 Hz display told 2 or 3.
        for (int m : { 2, 3 })
        {
            Display d{ 143.9 };
            RefreshFit fit;
            fit.SetMultiple(m);
            bool ever = false;
            int64_t lastShown = INT64_MIN;
            for (int64_t i = 0; i < 60 * 30; i++)
            {
                int64_t k = std::max(int64_t(std::ceil((double(i) * 1e9 / 60.0 + 3e6 + gauss(500'000)) * d.hz / 1e9)), lastShown + 1);
                lastShown = k;
                fit.Add(int64_t(d.Refresh(k) + kWake + gauss(200'000)));
                ever |= fit.Estimate().valid;
            }
            plant(m == 2 ? "143.9 Hz told multiple 2" : "143.9 Hz told multiple 3", !ever);
        }
        printf("   %d of %d caught\n", caught, plants);
    }

    printf(g_failures ? "\n%d FAILED\n" : "\nall passed\n", g_failures);
    return g_failures ? 1 : 0;
}
