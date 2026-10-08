// Tests for game::WholeVblanks (runtime/game/whole_vblanks.h): the guest
// vblanks GameFrameTime gives each frame as its game time. From the repo root:
//   clang++ -std=c++20 -O2 -Iruntime tests/whole_vblanks_test.cpp -o build/whole_vblanks_test
//   build/whole_vblanks_test [frames.csv[@30] ...]
// Without arguments: synthetic frames against vblanks. With NFSMW_GAME_TIME_LOG
// files (measured_ms,vblanks,...; "@30" after a path: Frame Rate 30): also the
// game time each rule gives their frames (those of 0-6 vblanks), against the
// measured time and the vblanks that fired.
#include <game/whole_vblanks.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <random>
#include <string>
#include <vector>

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

namespace
{
    constexpr double kRefreshMs = 1000.0 / 60.0;

    // The rule until 2026-10-07: the count when the measured time is within
    // half a vblank of it, else the measured time.
    uint64_t HalfVblankRule(double measuredMs, uint64_t vblanks, double vblankMs)
    {
        if (vblanks < 1 || vblanks > 6)
            return 0;
        return std::abs(measuredMs / vblankMs - double(vblanks)) > 0.5 ? 0 : vblanks;
    }

    struct Frame
    {
        double ms;
        uint64_t vblanks;
    };

    struct Totals
    {
        double measuredMs = 0, vblankMs = 0, newMs = 0, oldMs = 0;
        uint64_t frames = 0, given = 0, paced = 0, pacedGivenOne = 0;
        int maxSteps = 0;
    };

    // Each rule's game time for a run of frames.
    Totals Play(const std::vector<Frame>& frames, double vblankMs)
    {
        game::WholeVblanks rule;
        Totals t;
        for (const Frame& f : frames)
        {
            uint64_t k = rule.Give(f.ms, f.vblanks, vblankMs), old = HalfVblankRule(f.ms, f.vblanks, vblankMs);
            t.measuredMs += f.ms;
            t.vblankMs += double(f.vblanks) * vblankMs;
            t.newMs += k ? double(k) * vblankMs : f.ms;
            t.oldMs += old ? double(old) * vblankMs : f.ms;
            t.frames++;
            t.given += k != 0;
            t.maxSteps = std::max(t.maxSteps, int(k));
            if (f.vblanks == 1 && std::abs(f.ms / vblankMs - 1.0) <= 0.5)
            {
                t.paced++;
                t.pacedGivenOne += k == 1;
            }
        }
        return t;
    }

    // `count` frames against vblanks due every `vblankMs`, each firing 0 to
    // `lateMs` late (a vblank thread's wake-up; it catches up: one fires
    // before the next is due). Paced: frame i starts 0 to `jitterMs` after
    // vblank i fired, which released it (60 fps held). Else frames last
    // `meanMs` +- `jitterMs` (GPU-bound), so their starts wander between
    // vblanks. A frame's count: the vblanks that fired after its start, up
    // to the next one's (GameFrameTime reads the count at each frame's start).
    std::vector<Frame> Frames(double meanMs, double jitterMs, double vblankMs, double lateMs, bool paced, int count, uint32_t seed)
    {
        std::mt19937 rng(seed);
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        std::vector<double> fired;
        double last = paced ? double(count + 2) * vblankMs : double(count + 2) * (meanMs + jitterMs);
        for (double due = 0.0; due <= last + vblankMs; due += vblankMs)
            fired.push_back(due + unit(rng) * std::min(lateMs, vblankMs * 0.95));
        std::vector<double> starts;
        double t = unit(rng) * vblankMs;
        for (int i = 0; i <= count; i++)
        {
            starts.push_back(paced ? fired[size_t(i)] + 0.05 + unit(rng) * jitterMs : t);
            t += meanMs + (2 * unit(rng) - 1) * jitterMs;
        }
        std::vector<Frame> out;
        size_t v = 0;
        for (int i = 0; i < count; i++)
        {
            uint64_t n = 0;
            while (v < fired.size() && fired[v] <= starts[i])
                v++;
            while (v < fired.size() && fired[v] <= starts[i + 1])
                v++, n++;
            out.push_back({ starts[i + 1] - starts[i], n });
        }
        return out;
    }

    void Synthetic()
    {
        // Frame by frame, from a fresh start.
        {
            game::WholeVblanks r;
            CHECK(r.Give(16.6, 1, kRefreshMs) == 1, "a 60 fps frame: 1");
            CHECK(r.Give(44.0, 3, kRefreshMs) == 3, "44 ms, 3 fired (2.64 rounded up): 3");
            CHECK(r.Give(44.0, 2, kRefreshMs) == 2, "44 ms, 2 fired (2.64 rounded down): 2; the half-vblank rule kept 44 ms");
            CHECK(HalfVblankRule(44.0, 2, kRefreshMs) == 0, "the old rule's miss");
            CHECK(r.ahead == 0.0, "nothing ahead");
            CHECK(r.Give(44.0, 1, 2 * kRefreshMs) == 1 && r.Give(44.0, 2, 2 * kRefreshMs) == 2,
                "Frame Rate 30: 44 ms is 1.32 guest vblanks of 33.3 ms; 1 or 2 fired, given");
            // A late vblank: an 18.7 ms frame sees none (its measured time),
            // the next sees 2 and is given 1; then 1 each.
            CHECK(r.Give(18.7, 0, kRefreshMs) == 0, "none fired: measured time");
            CHECK(std::abs(r.ahead - 18.7 / kRefreshMs) < 1e-9, "ahead by the measured time (%.3f)", r.ahead);
            CHECK(r.Give(18.7, 2, kRefreshMs) == 1, "the catch-up gives back the vblank");
            CHECK(r.Give(18.7, 1, kRefreshMs) == 1, "then one each");
            CHECK(std::abs(r.ahead - (18.7 / kRefreshMs - 1.0)) < 1e-9, "the fraction stays (%.3f)", r.ahead);
            // Ahead by more than half a vblank: a frame of 1 still gets 1.
            CHECK(r.Give(16.0, 0, kRefreshMs) == 0 && r.ahead > 1.0, "ahead by more than a vblank");
            CHECK(r.Give(16.6, 1, kRefreshMs) == 1, "never fewer than one");
            CHECK(r.Give(120.0, 7, kRefreshMs) == 0 && r.ahead == 0.0, "7 vblanks (loading): measured time, and a fresh start");
            for (int i = 0; i < 20; i++)
                r.Give(30.0, 0, kRefreshMs);
            CHECK(r.ahead == game::WholeVblanks::kMaxAhead, "ahead at most %.0f vblanks", game::WholeVblanks::kMaxAhead);
            r.Reset();
            CHECK(r.ahead == 0.0 && r.Give(16.6, 1, 0.0) == 0, "no vblank period: measured time");
        }

        struct Case
        {
            const char* name;
            double meanMs, jitterMs, vblankMs, lateMs;
            bool paced;
            double oldMin, oldMax;  // the half-vblank rule's game time over the vblanks', expected range
        };
        const Case cases[] = {
            { "60 fps held, starts 0-3 ms late", kRefreshMs, 3.0, kRefreshMs, 0.5, true, 0.9999, 1.0001 },
            { "30 fps held, Frame Rate 30", 2 * kRefreshMs, 3.0, 2 * kRefreshMs, 0.5, true, 0.9999, 1.0001 },
            { "60 fps held, vblanks up to 6 ms late", kRefreshMs, 1.0, kRefreshMs, 6.0, true, 0.98, 1.02 },
            { "GPU-bound 44 ms +-2, Frame Rate 60", 44.0, 2.0, kRefreshMs, 0.5, false, 1.03, 1.2 },
            { "GPU-bound 49 ms +-2, Frame Rate 60", 49.0, 2.0, kRefreshMs, 0.5, false, 1.005, 1.1 },
            { "GPU-bound 44 ms +-2, Frame Rate 30", 44.0, 2.0, 2 * kRefreshMs, 0.5, false, 0.7, 0.95 },
            { "GPU-bound 49 ms +-2, Frame Rate 30", 49.0, 2.0, 2 * kRefreshMs, 0.5, false, 0.7, 0.95 },
            { "GPU-bound 18.7 ms +-1, vblanks up to 8 ms late", 18.7, 1.0, kRefreshMs, 8.0, false, 0.8, 0.97 },
            { "GPU-bound 26 ms +-7, Frame Rate 60", 26.0, 7.0, kRefreshMs, 0.5, false, 0.9, 1.1 },
        };
        for (const Case& c : cases)
        {
            double worst = 0, oldMean = 0;
            uint64_t paced = 0, pacedOne = 0, frames = 0, given = 0;
            int maxSteps = 0;
            const int runs = 12;
            for (int p = 0; p < runs; p++)
            {
                Totals t = Play(Frames(c.meanMs, c.jitterMs, c.vblankMs, c.lateMs, c.paced, 4000, 1234u + uint32_t(p)), c.vblankMs);
                worst = std::max(worst, std::abs(t.newMs - t.vblankMs) / c.vblankMs);
                oldMean += t.oldMs / t.vblankMs / runs;
                paced += t.paced;
                pacedOne += t.pacedGivenOne;
                frames += t.frames;
                given += t.given;
                maxSteps = std::max(maxSteps, t.maxSteps);
            }
            printf("  %-48s new rule within %.2f vblanks of the vblanks' time; half-vblank rule x%.4f; %llu of %llu frames given whole vblanks\n",
                c.name, worst, oldMean, (unsigned long long)given, (unsigned long long)frames);
            // Game time against the vblanks that fired, over 4000 frames:
            // within what can be ahead at the end (half a vblank, and a last
            // frame that saw none), whatever the frames did.
            CHECK(worst <= 2.0, "%s: the new rule within 2 vblanks of the vblanks (%.2f)", c.name, worst);
            CHECK(oldMean >= c.oldMin && oldMean <= c.oldMax, "%s: the half-vblank rule's known bias (x%.4f)", c.name, oldMean);
            if (c.paced)
                CHECK(pacedOne == paced && given == frames, "%s: every paced frame given one vblank (%llu of %llu)", c.name,
                    (unsigned long long)pacedOne, (unsigned long long)paced);
            CHECK(maxSteps <= 6, "%s: at most 6 vblanks a frame", c.name);
        }
    }

    // NFSMW_GAME_TIME_LOG rows: measured_ms,vblanks,... (the frame's own; the
    // rule used then doesn't change them).
    void Replay(const std::string& arg)
    {
        std::string path = arg;
        double vblankMs = kRefreshMs;
        if (arg.size() > 3 && arg.substr(arg.size() - 3) == "@30")
        {
            path = arg.substr(0, arg.size() - 3);
            vblankMs = 2 * kRefreshMs;
        }
        std::ifstream in(path);
        if (!in)
        {
            printf("  %s: can't read\n", path.c_str());
            g_failures++;
            return;
        }
        std::string line;
        std::getline(in, line);
        std::vector<Frame> frames;
        while (std::getline(in, line))
        {
            double ms;
            unsigned long long k;
            if (sscanf(line.c_str(), "%lf,%llu", &ms, &k) == 2 && k <= 6)
                frames.push_back({ ms, k });
        }
        if (frames.empty())
        {
            printf("  %s: no frames of 0-6 vblanks\n", path.c_str());
            return;
        }
        Totals t = Play(frames, vblankMs);
        printf("  %s: %llu frames of 0-6 vblanks, %.1f s measured; game time over measured: new rule %.4f, half-vblank rule %.4f, "
            "vblanks %.4f\n", path.c_str(), (unsigned long long)t.frames, t.measuredMs / 1000, t.newMs / t.measuredMs, t.oldMs / t.measuredMs,
            t.vblankMs / t.measuredMs);
    }
}

int main(int argc, char** argv)
{
    printf("synthetic frames:\n");
    Synthetic();
    if (argc > 1)
        printf("logs:\n");
    for (int i = 1; i < argc; i++)
        Replay(argv[i]);
    printf("%s (%d failures)\n", g_failures ? "FAILED" : "passed", g_failures);
    return g_failures ? 1 : 0;
}
