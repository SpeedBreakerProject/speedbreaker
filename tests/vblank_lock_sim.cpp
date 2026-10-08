// Closed-loop simulation of the guest vblank lock with the runtime's own
// RefreshFit and VblankLock (video/refresh_fit.cpp, video/vblank_lock.cpp).
// From the repo root:
//   clang++ -std=c++20 -O2 -Iruntime tests/vblank_lock_sim.cpp runtime/video/refresh_fit.cpp \
//     runtime/video/vblank_lock.cpp -o build/vblank_lock_sim
//   build/vblank_lock_sim
//
// Model (times in ns):
//   display   refreshes every P (true rate), phase random; a present made
//             before R_k - latch shows at R_k (FIFO, in order); its
//             completion is seen at R_k + notify (0.2-0.6 ms, sometimes late).
//   guest     each vblank (VblankLock::Next, or free-running 16.667 ms)
//             releases one game frame, which reaches the presenter x later
//             (x drawn per scene). The presenter keeps only the newest
//             pending frame (older ones are dropped), picks it up ~0.5 ms
//             later, and presents at once when a swapchain image is free
//             (3 images: one on screen, at most 2 queued), else waits for
//             the next flip holding it.
//   variable  (the second table) no grid: a present shows as soon as it is
//             latched, no sooner than 1/144 s after the last refresh, and
//             the display refreshes on its own after 1/48 s without one.
// Counted from the 60th second (the lock has converged by then): dropped
// frames, repeats (a refresh with no new frame while frames flow), slips,
// the median vblank -> on screen latency, and the guest vblank's rate.
//   suspended (the last tables) the game is suspended (iOS: Control Center,
//             the app switcher): no vblank fires and its frames stand still,
//             the display shows what was queued, and the next vblank comes
//             exactly the time suspended later, then VblankLock::Resumed
//             (gpu/command_processor.cpp's Vsync thread).
#include <video/refresh_fit.h>
#include <video/vblank_lock.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <iterator>
#include <random>
#include <utility>
#include <vector>

using namespace video;

struct Scene
{
    const char* name;
    double meanMs, sdMs;   // vblank -> presenter, normal
    double spikeP, spikeMs; // occasional slow frames
};

struct Result
{
    double lockAtS = -1;
    uint64_t drops = 0, repeats = 0, frames = 0, slips = 0;
    double latMedMs = 0;
    double hz = 0;
    double followsAtS = -1;  // the lock found the display following the vblank
    double vblankHz = 0;     // counted window
    // After the last suspension: vblanks until the phase was reached again,
    // drops plus repeats from the first frame shown to 5 s after the resume,
    // and whether the lock held.
    int64_t rephaseVblanks = -1;
    uint64_t afterResume = 0;
    bool lockedAtEnd = false;
};

static Result Run(double displayHz, bool lockOn, const Scene& scene, double seconds, uint32_t seed, double countFromS,
    bool variable = false, const std::vector<std::pair<double, double>>& suspensions = {}, bool callResumed = true)
{
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> nd(0, 1);
    std::uniform_real_distribution<double> ud(0, 1);
    const double P = 1e9 / displayHz;
    const int64_t t0 = 1'000'000'000'000;
    const double phase = ud(rng) * P;
    const double latch = 2.0e6;  // compositor latches this long before the refresh

    RefreshFit fit;
    VblankLock lock(16'667'000, lockOn);

    struct Frame
    {
        int64_t vblank, arrive;
    };
    std::deque<Frame> inflight;      // released, not yet at the presenter (sorted by arrive)
    bool pendingFull = false;
    Frame pending{};
    bool held = false;               // main thread holds a frame, waiting for an image
    Frame heldFrame{};
    struct Queued
    {
        Frame f;
        int64_t presented;
    };
    std::deque<Queued> queue;        // presented, not yet on screen
    Result res;
    std::vector<double> lat;

    int64_t vblank = t0;
    int64_t k = 1;
    int64_t refresh = t0 + int64_t(phase);
    int64_t lastRefresh = refresh;
    const int64_t end = t0 + int64_t(seconds * 1e9);
    int64_t firstCounted = 0, lastCounted = 0;
    uint64_t vblanks = 0;
    const int64_t countFrom = t0 + int64_t(countFromS * 1e9);
    bool flowing = false;
    // Suspensions as (start, end), in order; `pause` is the one under way or next.
    std::vector<std::pair<int64_t, int64_t>> pauses;
    for (auto [at, length] : suspensions)
        pauses.push_back({ t0 + int64_t(at * 1e9), t0 + int64_t((at + length) * 1e9) });
    size_t pause = 0;
    bool suspended = false;
    int64_t resumedAt = -1, sinceResume = 0, shownAfterResume = -1;  // the last resume
    auto afterResume = [&](int64_t t) { return shownAfterResume >= 0 && t <= resumedAt + 5'000'000'000; };
    auto present = [&](const Frame& f, int64_t at) {
        queue.push_back({ f, at });
    };
    auto presenterStep = [&](int64_t now) {
        // Main thread: present what it holds when an image is free, then
        // take the pending frame.
        while (true)
        {
            if (held)
            {
                if (queue.size() >= 2)
                    return;
                present(heldFrame, now);
                held = false;
            }
            if (!pendingFull)
                return;
            heldFrame = pending;
            pendingFull = false;
            held = true;
        }
    };
    while (refresh < end)
    {
        if (variable && k > 1)
        {
            const int64_t minGap = int64_t(1e9 / 144), maxGap = int64_t(1e9 / 48);
            refresh = lastRefresh + maxGap;
            if (!queue.empty())
                refresh = std::clamp(queue.front().presented + int64_t(latch), lastRefresh + minGap, lastRefresh + maxGap);
        }
        // Next event: vblank, a frame's arrival, or a flip.
        int64_t nextArrive = inflight.empty() ? INT64_MAX : inflight.front().arrive;
        const int64_t nextEvent = std::min({ vblank, nextArrive, refresh });
        if (pause < pauses.size() && !suspended && nextEvent >= pauses[pause].first)
        {
            // Everything the game does stands still for the suspension; the
            // display flips on through what was already queued.
            suspended = true;
            vblank += pauses[pause].second - pauses[pause].first;
            for (Frame& f : inflight)
                f.arrive += pauses[pause].second - pauses[pause].first;
            continue;
        }
        if (suspended && nextEvent >= pauses[pause].second)
        {
            suspended = false;
            resumedAt = pauses[pause++].second;
            sinceResume = 0;
            shownAfterResume = -1;
            res.rephaseVblanks = -1;
            res.afterResume = 0;
            if (callResumed)
                lock.Resumed();
            presenterStep(resumedAt);
            continue;
        }
        if (vblank <= refresh && vblank <= nextArrive)
        {
            double x = scene.meanMs + scene.sdMs * nd(rng);
            if (ud(rng) < scene.spikeP)
                x = scene.spikeMs + 1.0 * nd(rng);
            x = std::max(1.0, x);
            Frame f{ vblank, vblank + int64_t(x * 1e6 + 500'000) };  // +0.5 ms pickup
            auto it = inflight.end();
            while (it != inflight.begin() && std::prev(it)->arrive > f.arrive)
                --it;
            inflight.insert(it, f);
            if (vblank >= countFrom)
            {
                firstCounted = vblanks++ ? firstCounted : vblank;
                lastCounted = vblank;
            }
            vblank = lock.Next(vblank);
            if (resumedAt >= 0 && res.rephaseVblanks < 0)
            {
                sinceResume++;
                if (std::abs(lock.GetStatus().errorNs) <= double(VblankLock::kMaxStepNs))
                    res.rephaseVblanks = sinceResume;
            }
            continue;
        }
        if (nextArrive <= refresh)
        {
            Frame f = inflight.front();
            inflight.pop_front();
            if (pendingFull && f.vblank > pending.vblank && pending.vblank >= countFrom)
                res.drops++;
            if (pendingFull && f.vblank > pending.vblank && afterResume(f.arrive))
                res.afterResume++;
            if (!pendingFull || f.vblank > pending.vblank)
                pending = f;
            pendingFull = true;
            presenterStep(f.arrive);
            continue;
        }
        // Flip at `refresh`: the oldest present made before the latch shows.
        if (!queue.empty() && queue.front().presented <= refresh - int64_t(latch))
        {
            Queued q = queue.front();
            queue.pop_front();
            double notify = 200'000 + 400'000 * ud(rng);
            if (ud(rng) < 0.01)
                notify += 2e6 * ud(rng);
            int64_t onScreen = refresh + int64_t(notify);
            fit.Add(onScreen);
            lock.SetDisplay(fit.Estimate());
            // The presenter doesn't time a frame released before the resume
            // (video/presenter.cpp's OnScreen).
            if (q.f.vblank >= resumedAt)
                lock.Observe(q.f.vblank, q.presented, onScreen);
            if (resumedAt >= 0 && shownAfterResume < 0)
                shownAfterResume = refresh;
            if (q.f.vblank >= countFrom)
            {
                res.frames++;
                lat.push_back((refresh - q.f.vblank) / 1e6);
            }
            flowing = true;
            if (!suspended)
                presenterStep(refresh);
        }
        else
        {
            if (flowing && refresh >= countFrom)
                res.repeats++;
            if (afterResume(refresh))
                res.afterResume++;
        }
        if (res.lockAtS < 0 && lock.Locked())
            res.lockAtS = (refresh - t0) / 1e9;
        if (res.followsAtS < 0 && lock.GetStatus().follows)
            res.followsAtS = (refresh - t0) / 1e9;
        k++;
        lastRefresh = refresh;
        if (!variable)
            refresh = t0 + int64_t(phase + double(k) * P);
    }
    res.vblankHz = vblanks > 1 ? double(vblanks - 1) / (double(lastCounted - firstCounted) / 1e9) : 0.0;
    auto st = lock.GetStatus();
    res.slips = st.slips;
    res.lockedAtEnd = st.locked;
    res.hz = st.display.periodNs > 0 ? 1e9 / st.display.periodNs : 0;
    if (!lat.empty())
    {
        std::nth_element(lat.begin(), lat.begin() + lat.size() / 2, lat.end());
        res.latMedMs = lat[lat.size() / 2];
    }
    return res;
}

int main()
{
    const Scene scenes[] = {
        { "light (vblank->presenter 7+-1.5 ms)", 7, 1.5, 0.0, 0 },
        { "driving (9+-2 ms, 1% at 20 ms)", 9, 2, 0.01, 20 },
        { "fly-in (14+-1.5 ms, 2% at 20 ms)", 14, 1.5, 0.02, 20 },
        { "heavy (16+-1.5 ms: often a refresh late)", 16, 1.5, 0.0, 0 },
    };
    const double rates[] = { 59.972616, 59.0, 61.0 };
    const double seconds = 300, countFrom = 60;
    printf("%.0f s per run, counted from %.0f s (after the lock converges); 3 seeds each\n", seconds, countFrom);
    printf("%-44s %-10s %5s %8s %8s %8s %6s %8s\n", "scene", "display", "lock", "lock at", "drops", "repeats", "slips", "lat ms");
    bool ok = true;
    for (const Scene& s : scenes)
        for (double hz : rates)
            for (bool on : { false, true })
            {
                Result sum;
                double lockAt = 0, latMed = 0;
                for (uint32_t seed = 1; seed <= 3; seed++)
                {
                    Result r = Run(hz, on, s, seconds, seed * 7919 + uint32_t(hz * 10), countFrom);
                    sum.drops += r.drops;
                    sum.repeats += r.repeats;
                    sum.slips += r.slips;
                    lockAt += r.lockAtS;
                    latMed += r.latMedMs;
                    ok &= !on || (r.lockAtS > 0 && r.followsAtS < 0);  // a real grid is never let go
                }
                printf("%-44s %-10.4f %5s %8.1f %8llu %8llu %6llu %8.1f\n", s.name, hz, on ? "on" : "off", on ? lockAt / 3 : 0.0,
                    (unsigned long long)sum.drops, (unsigned long long)sum.repeats, (unsigned long long)sum.slips, latMed / 3);
            }

    // A display that follows the presents: steady frames fit the guest's own
    // rate well enough to lock, and the lock must let go rather than chase
    // it. Jittery ones never fit. The guest vblank should end at 60 Hz.
    const Scene steady[] = {
        { "steady (vblank->presenter 3+-0.05 ms)", 3, 0.05, 0.0, 0 },
        { "light (vblank->presenter 7+-1.5 ms)", 7, 1.5, 0.0, 0 },
    };
    printf("\nvariable refresh (48-144 Hz), lock on; %.0f s per run, counted from %.0f s\n", seconds, countFrom);
    printf("%-44s %5s %8s %10s %10s %8s %8s\n", "scene", "seed", "lock at", "let go at", "vblank Hz", "drops", "repeats");
    for (const Scene& s : steady)
        for (uint32_t seed = 1; seed <= 3; seed++)
        {
            Result r = Run(60.0, true, s, seconds, seed * 104729, countFrom, true);
            printf("%-44s %5u %8.1f %10.1f %10.4f %8llu %8llu\n", s.name, seed, r.lockAtS, r.followsAtS, r.vblankHz,
                (unsigned long long)r.drops, (unsigned long long)r.repeats);
            ok &= std::abs(r.vblankHz - 1e9 / 16'667'000) < 0.01;
        }
    printf("%s\n", ok ? "fixed rates stay locked; variable refresh ends free-running at 60 Hz: ok" : "FAILED");

    // The vblank comes back up to half a refresh off the grid: the lock walks
    // it back a step a vblank, within ~170 vblanks, without deciding that the
    // display follows the vblank, and shows at most one frame twice or not at
    // all on the way.
    const double suspensions[] = { 0.1, 0.5, 6.0013, 12.5, 30.0 };
    printf("\nsuspended at 90 s, lock on; 3 seeds each, max of the three\n");
    printf("%-44s %-10s %9s %8s %14s %6s\n", "scene", "display", "suspended", "rephase", "drops+repeats", "lock");
    bool resumes = true;
    for (const Scene& s : scenes)
        for (double hz : rates)
            for (double d : suspensions)
            {
                int64_t rephase = 0;
                uint64_t after = 0;
                bool held = true;
                // Half a refresh of steps, and a few more: each error is
                // measured before its step, and the fitted grid moves a little.
                const int64_t limit = int64_t(std::ceil(1e9 / hz / 2 / double(VblankLock::kMaxStepNs))) + 10;
                for (uint32_t seed = 1; seed <= 3; seed++)
                {
                    Result r = Run(hz, true, s, 90 + d + 10, seed * 6151 + uint32_t(hz * 10), countFrom, false, { { 90, d } });
                    rephase = r.rephaseVblanks < 0 ? INT64_MAX : std::max(rephase, r.rephaseVblanks);
                    after = std::max(after, r.afterResume);
                    held &= r.lockedAtEnd && r.followsAtS < 0;
                }
                printf("%-44s %-10.4f %9.4f %8lld %14llu %6s\n", s.name, hz, d, rephase == INT64_MAX ? -1LL : (long long)rephase,
                    (unsigned long long)after, held ? "held" : "LOST");
                resumes &= rephase <= limit && after <= 1 && held;
            }

    // Suspended again while the lock is still walking back (Control Center
    // pulled down a few times in a row). Resumed() starts each walk's chase
    // count afresh; without it the walks add up to "the display follows the
    // vblank", and the lock is gone for the rest of the run.
    printf("\nsuspended 5 times, each 2-2.7 s after the last resume, light scene; 3 seeds each\n");
    printf("%-10s %9s %5s %10s %16s\n", "display", "suspended", "gap", "lock held", "without Resumed");
    for (double hz : rates)
        for (double d : { 0.5, 12.5 })
            for (double gap : { 2.0, 2.7 })
            {
                std::vector<std::pair<double, double>> pauses;
                for (int i = 0; i < 5; i++)
                    pauses.push_back({ 90 + i * (d + gap), d });
                int held = 0, heldWithout = 0;
                for (uint32_t seed = 1; seed <= 3; seed++)
                    for (bool call : { true, false })
                    {
                        Result r = Run(hz, true, scenes[0], 90 + 5 * (d + gap) + 10, seed * 6151 + uint32_t(hz * 10), countFrom, false,
                            pauses, call);
                        (call ? held : heldWithout) += r.lockedAtEnd && r.followsAtS < 0;
                    }
                printf("%-10.4f %9.4f %5.1f %5d of 3 %11d of 3\n", hz, d, gap, held, heldWithout);
                resumes &= held == 3;
            }
    printf("%s\n", resumes ? "after a suspension the lock re-phases within ~170 vblanks, holds, and drops or repeats at most once: ok"
                            : "FAILED");
    return ok && resumes ? 0 : 1;
}
