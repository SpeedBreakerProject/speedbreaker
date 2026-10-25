// video::ExtraRefreshes (runtime/video/repeats.h): the [perf] line's
// "repeated" as the Windows presenter counts it, in the panel's own
// refreshes. Header only; built by tests/CMakeLists.txt (-DSB_TESTS=ON).
//
// 1. Single frames at 60, 120, 144, 165 and 240 Hz, under G-SYNC, at Frame
//    Rate 30, a pause, an unknown rate.
// 2. A FIFO display model: a steady 60 fps (frames presented 3 ms after each
//    vblank, with jitter) shown at the first refresh after each present. Two
//    seconds (120 frames, one [perf] window) at 144 Hz count the ~48 frames
//    shown for 3 refreshes; at 60 Hz and under steady G-SYNC, none. The
//    count as it was before, against 1/60 s, sees nothing at 144 Hz: why it
//    changed.
#include <video/repeats.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

using video::ExtraRefreshes;

namespace
{
    constexpr double kVblank = 1e9 / 60.0;

    double Period(double hz)
    {
        return 1e9 / hz;
    }

    // The count before this change (presenter.cpp's OnScreen without a
    // valid fit): whole periods of 1/60 s past the vblanks.
    int64_t OldCount(double shownNs, int64_t vblanks)
    {
        int64_t shownFor = std::llround(shownNs / (1e9 / 60.0));
        return shownFor > vblanks && shownFor <= 30 ? shownFor - vblanks : 0;
    }

    struct Window
    {
        int64_t extra = 0, old = 0;
        std::vector<int> refreshes;  // each gap in whole refreshes
    };

    // 120 game frames at 60 fps, each presented `delayNs` (+ jitter) after
    // its vblank and shown at the first refresh of a `hz` grid after that
    // (hz 0: variable refresh, shown as presented, 0.5 ms later).
    Window Simulate(double hz, double delayNs, double jitterNs, uint32_t seed)
    {
        std::mt19937 rng(seed);
        std::normal_distribution<double> jitter(0.0, jitterNs);
        const double period = hz > 0 ? Period(hz) : Period(144.0);  // the rate SDL reports under G-SYNC
        const double phase = 0.37 * period;
        Window w;
        double last = -1;
        for (int k = 0; k < 121; k++)
        {
            double presented = k * kVblank + delayNs + (jitterNs > 0 ? std::max(-delayNs + 0.1e6, jitter(rng)) : 0.0);
            double shown = hz > 0 ? phase + std::ceil((presented - phase) / period) * period : presented + 0.5e6;
            if (last >= 0)
            {
                double gap = shown - last;
                w.extra += ExtraRefreshes(gap, 1, kVblank, period);
                w.old += OldCount(gap, 1);
                w.refreshes.push_back(int(std::lround(gap / period)));
            }
            last = shown;
        }
        return w;
    }
}

int main()
{
    printf("1. single frames\n");
    // 60 Hz: one refresh is the frame's own; a second one is a repeat.
    CHECK(ExtraRefreshes(Period(60), 1, kVblank, Period(60)) == 0, "60 Hz, 1 refresh");
    CHECK(ExtraRefreshes(2 * Period(60), 1, kVblank, Period(60)) == 1, "60 Hz, 2 refreshes");
    CHECK(ExtraRefreshes(3 * Period(60), 1, kVblank, Period(60)) == 2, "60 Hz, 3 refreshes");
    // A 59.94 Hz panel against the free-running 60 Hz vblank: the same.
    CHECK(ExtraRefreshes(Period(59.94), 1, kVblank, Period(59.94)) == 0, "59.94 Hz, 1 refresh");
    CHECK(ExtraRefreshes(2 * Period(59.94), 1, kVblank, Period(59.94)) == 1, "59.94 Hz, 2 refreshes");
    // 120 Hz: 2 refreshes are a frame's own; the 3 of a 1-and-3 pair counts.
    CHECK(ExtraRefreshes(2 * Period(120), 1, kVblank, Period(120)) == 0, "120 Hz, 2");
    CHECK(ExtraRefreshes(1 * Period(120), 1, kVblank, Period(120)) == 0, "120 Hz, 1");
    CHECK(ExtraRefreshes(3 * Period(120), 1, kVblank, Period(120)) == 1, "120 Hz, 3");
    // 144 Hz: 2 or 3 refreshes (13.9 / 20.8 ms); the 3s count.
    CHECK(ExtraRefreshes(2 * Period(144), 1, kVblank, Period(144)) == 0, "144 Hz, 2");
    CHECK(ExtraRefreshes(3 * Period(144), 1, kVblank, Period(144)) == 1, "144 Hz, 3");
    CHECK(ExtraRefreshes(4 * Period(144), 1, kVblank, Period(144)) == 2, "144 Hz, 4");
    CHECK(OldCount(3 * Period(144), 1) == 0, "the old count saw a 3 at 144 Hz");
    // 165 Hz: a 3 is a quarter refresh long, not counted; a 4 is.
    CHECK(ExtraRefreshes(3 * Period(165), 1, kVblank, Period(165)) == 0, "165 Hz, 3");
    CHECK(ExtraRefreshes(4 * Period(165), 1, kVblank, Period(165)) == 1, "165 Hz, 4");
    // 240 Hz: 4 is the frame's own; 5 counts.
    CHECK(ExtraRefreshes(4 * Period(240), 1, kVblank, Period(240)) == 0, "240 Hz, 4");
    CHECK(ExtraRefreshes(5 * Period(240), 1, kVblank, Period(240)) == 1, "240 Hz, 5");
    // G-SYNC: SDL reports the panel's top rate; a frame ~1 ms late is not a
    // repeat, one half a refresh late is.
    CHECK(ExtraRefreshes(kVblank + 1e6, 1, kVblank, Period(144)) == 0, "VRR, 1 ms late");
    CHECK(ExtraRefreshes(kVblank + 3.6e6, 1, kVblank, Period(144)) == 1, "VRR, 3.6 ms late");
    // Frame Rate 30: two vblanks a frame.
    CHECK(ExtraRefreshes(2 * kVblank, 2, kVblank, Period(60)) == 0, "Frame Rate 30, 2 refreshes");
    CHECK(ExtraRefreshes(3 * kVblank, 2, kVblank, Period(60)) == 1, "Frame Rate 30, 3 refreshes");
    CHECK(ExtraRefreshes(5 * Period(144), 2, kVblank, Period(144)) == 0, "Frame Rate 30 at 144 Hz, 5");
    CHECK(ExtraRefreshes(6 * Period(144), 2, kVblank, Period(144)) == 1, "Frame Rate 30 at 144 Hz, 6");
    // A pause, an unknown rate, a frame shown early.
    CHECK(ExtraRefreshes(600e6, 1, kVblank, Period(60)) == 0, "a pause");
    CHECK(ExtraRefreshes(3 * kVblank, 1, kVblank, 0.0) == 0, "unknown rate");
    CHECK(ExtraRefreshes(0.5 * kVblank, 1, kVblank, Period(60)) == 0, "a short frame");

    printf("2. two seconds of 60 fps (one [perf] window)\n");
    {
        Window w = Simulate(144.0, 3e6, 0.0, 1);
        int threes = 0;
        for (int r : w.refreshes)
            threes += r == 3;
        printf("   144 Hz: %lld repeated (%d of %zu frames shown for 3 refreshes); before: %lld\n", (long long)w.extra, threes,
            w.refreshes.size(), (long long)w.old);
        CHECK(w.extra == threes && threes >= 45 && threes <= 51, "144 Hz: %lld, %d threes", (long long)w.extra, threes);
        CHECK(w.old == 0, "the old count at 144 Hz: %lld", (long long)w.old);
    }
    {
        Window w = Simulate(144.0, 7e6, 1.5e6, 2);
        printf("   144 Hz, jittery delivery: %lld repeated\n", (long long)w.extra);
        CHECK(w.extra >= 40 && w.extra <= 60, "144 Hz jittery: %lld", (long long)w.extra);
    }
    {
        Window w = Simulate(60.0, 3e6, 0.0, 3);
        printf("   60 Hz: %lld repeated (before: %lld)\n", (long long)w.extra, (long long)w.old);
        CHECK(w.extra == 0 && w.old == 0, "60 Hz: %lld / %lld", (long long)w.extra, (long long)w.old);
    }
    {
        Window w = Simulate(120.0, 3e6, 0.0, 4);
        printf("   120 Hz, presents clear of the refresh: %lld repeated\n", (long long)w.extra);
        CHECK(w.extra == 0, "120 Hz steady: %lld", (long long)w.extra);
    }
    {
        // Presents right at a refresh: jitter splits them over two, 1-and-3.
        Window w = Simulate(120.0, 0.37 * Period(120.0) + 2 * Period(120.0), 0.4e6, 5);
        int threes = 0;
        for (int r : w.refreshes)
            threes += r == 3;
        printf("   120 Hz, presents at a refresh: %lld repeated (%d frames shown for 3)\n", (long long)w.extra, threes);
        CHECK(w.extra == threes && threes > 10, "120 Hz at the latch: %lld, %d", (long long)w.extra, threes);
    }
    {
        // Under G-SYNC only delivery jitter reaches the screen: a steady
        // stream counts nothing, a jittery one only its late frames (half a
        // 144 Hz refresh, 3.5 ms), far from the fixed 144 Hz's ~48.
        Window steady = Simulate(0.0, 7e6, 0.5e6, 6), jittery = Simulate(0.0, 7e6, 1.0e6, 6);
        printf("   G-SYNC (144 Hz top rate): %lld repeated with 0.5 ms of jitter, %lld with 1 ms\n", (long long)steady.extra,
            (long long)jittery.extra);
        CHECK(steady.extra == 0, "VRR, steady: %lld", (long long)steady.extra);
        CHECK(jittery.extra < 10, "VRR, jittery: %lld", (long long)jittery.extra);
    }

    printf(g_failures ? "%d failure(s)\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
