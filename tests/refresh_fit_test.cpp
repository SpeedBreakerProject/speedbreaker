// Synthetic-timestamp tests for video::RefreshFit (runtime/video/refresh_fit.cpp).
// From the repo root:
//   clang++ -std=c++20 -O2 -Iruntime tests/refresh_fit_test.cpp runtime/video/refresh_fit.cpp -o build/refresh_fit_test
//   build/refresh_fit_test
#include <video/refresh_fit.h>

#include <cmath>
#include <cstdio>
#include <functional>
#include <random>

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

    printf(g_failures ? "\n%d FAILED\n" : "\nall passed\n", g_failures);
    return g_failures ? 1 : 0;
}
