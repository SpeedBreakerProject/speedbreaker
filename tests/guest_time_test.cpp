// Tests for the game's clocks (runtime/cpu/guest_time.cpp), which stand still
// while the game is suspended. From the repo root:
//   clang++ -std=c++20 -O2 -pthread -Iruntime tests/guest_time_test.cpp runtime/cpu/guest_time.cpp -o build/guest_time_test
//   build/guest_time_test
// About 10 s. Each guest clock is checked in its own units against the host
// clock under it (guesttime::Raw*): timebase ticks (49.875 MHz), ns, and the
// system time's 100 ns. A suspension is bracketed by host readings taken
// around the Suspend and Resume calls, so the checks hold however the
// machine schedules the threads.
#include <cpu/guest_time.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <random>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

static std::atomic<int> g_failures{ 0 };
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

// The three clocks: the recompiled code's mftb (through its macro, as ppc/
// reads it), steady ns, and the system time.
constexpr int kClocks = 3;
constexpr const char* kClockName[kClocks] = { "timebase", "ns", "system time" };
constexpr double kPerUs[kClocks] = { 49.875, 1000.0, 10.0 };

// Each transition moves guest time on by a margin, so that a reader's
// unordered counter read can't see it go back (1 us per transition, see
// guest_time.cpp): a suspension may leave guest time up to 2 us ahead.
constexpr double kGainUs = 2.0;

static int64_t Units(int c, double us)
{
    return int64_t(std::ceil(us * kPerUs[c]));
}

static double Ms(int c, int64_t v)
{
    return double(v) / (kPerUs[c] * 1000.0);
}

struct Reading
{
    int64_t v[kClocks];
};

static Reading Guest()
{
    return { { int64_t(PPC_QUERY_TIMEBASE()), guesttime::NowNs(), guesttime::SystemTime100ns() } };
}

static Reading Host()
{
    return { { guesttime::RawTimebase(), guesttime::RawNs(), guesttime::RawSystem() } };
}

// Waits `us` microseconds of host time: spinning for the shortest, so a
// window can be next to nothing, and sleeping otherwise.
static void Pause(int64_t us)
{
    if (us >= 20)
    {
        std::this_thread::sleep_for(std::chrono::microseconds(us));
        return;
    }
    const int64_t end = guesttime::RawNs() + us * 1000;
    while (guesttime::RawNs() < end)
    {
    }
}

// Polls `done` for up to a second of host time.
static bool WaitFor(const std::function<bool()>& done)
{
    const auto end = std::chrono::steady_clock::now() + 1s;
    while (!done())
    {
        if (std::chrono::steady_clock::now() > end)
            return done();
        std::this_thread::sleep_for(20us);
    }
    return true;
}

// The value a clock froze at in the latest suspension (guest_time.h's state).
static int64_t Frozen(int c)
{
    const guesttime::Domain* domain[kClocks] = { &guesttime::g_state.timebase, &guesttime::g_state.ns, &guesttime::g_state.system };
    return domain[c]->frozen.load(std::memory_order_relaxed);
}

// One thread's readings, which must never go back on any clock: game code
// subtracts mftb and the tick count unsigned, so one tick back reads as 2^64.
// A step back onto the frozen value means the last reading before a
// suspension was later than the time it froze at.
struct alignas(128) Monotonic
{
    Reading last{};
    bool any = false;
    uint64_t reads = 0;
    uint64_t backwards[kClocks] = {}, intoSuspension[kClocks] = {};
    int64_t worst[kClocks] = {};  // the largest step back

    void Add(const Reading& r)
    {
        for (int c = 0; any && c < kClocks; c++)
            if (r.v[c] < last.v[c])
            {
                backwards[c]++;
                intoSuspension[c] += r.v[c] == Frozen(c);
                worst[c] = std::max(worst[c], last.v[c] - r.v[c]);
            }
        last = r;
        any = true;
        reads++;
    }

    uint64_t Backwards() const { return backwards[0] + backwards[1] + backwards[2]; }
};

// Suspensions made from this thread, each call made twice as UIKit
// delivers each transition twice. The time suspended is at least from just
// after Suspend returned to just before Resume was called, and at most from
// just before the one to just after the other.
struct Suspensions
{
    int64_t least[kClocks] = {}, most[kClocks] = {};
    uint64_t count = 0, notIdempotent = 0, notFrozen = 0;
    Monotonic reads;  // this thread's own

    void Run(int64_t us)
    {
        const Reading s1 = Host();
        const bool suspended = guesttime::Suspend();
        const Reading s2 = Host();
        const bool suspendedAgain = guesttime::Suspend();
        const Reading frozen = Guest();
        Pause(us);
        const Reading still = Guest();
        const Reading r1 = Host();
        const bool resumed = guesttime::Resume();
        const Reading r2 = Host();
        const bool resumedAgain = guesttime::Resume();
        reads.Add(frozen);
        reads.Add(still);
        reads.Add(Guest());
        notIdempotent += !(suspended && !suspendedAgain && resumed && !resumedAgain);
        for (int c = 0; c < kClocks; c++)
        {
            notFrozen += frozen.v[c] != still.v[c];
            least[c] += r1.v[c] - s2.v[c];
            most[c] += r2.v[c] - s1.v[c];
        }
        count++;
    }
};

// SleepUntil, `ns` of guest time from now, on a thread of its own.
struct Sleeper
{
    const int64_t guestStart = guesttime::NowNs(), hostStart = guesttime::RawNs();
    int64_t guestEnd = 0, hostEnd = 0;
    std::atomic<bool> done{ false };
    std::thread thread;

    explicit Sleeper(int64_t ns)
        : thread([this, deadline = guestStart + ns] {
              guesttime::SleepUntil(deadline);
              hostEnd = guesttime::RawNs();
              guestEnd = guesttime::NowNs();
              done = true;
          })
    {
    }
    void Join() { thread.join(); }
};

// The resume hook, standing in for dispatcher::WakeAfterResume: it takes the
// lock a kernel-style waiter parks under and wakes it.
namespace hook
{
    std::thread::id caller;  // the thread calling Resume
    std::atomic<uint64_t> calls{ 0 }, onCaller{ 0 }, whileSuspended{ 0 };
    std::atomic<int64_t> lastResume{ 0 };  // LastResumeNs() as the last call saw it
    std::mutex mutex;
    std::condition_variable cv;

    void Run()
    {
        if (std::this_thread::get_id() == caller)
            onCaller++;
        if (guesttime::Suspended())
            whileSuspended++;
        lastResume = guesttime::LastResumeNs();
        {
            std::lock_guard lock(mutex);
            cv.notify_all();
        }
        calls++;
    }
}

int main()
{
    // A thread left parked would hang the joins below: give up instead.
    std::thread([] {
        std::this_thread::sleep_for(120s);
        printf("  FAIL: still running after 120 s (a thread left parked?)\n");
        std::fflush(stdout);
        std::_Exit(2);
    }).detach();

    std::mt19937_64 rng(1234);
    auto gap = [&] { return int64_t(std::uniform_int_distribution<int>(0, 200)(rng)); };  // us
    // Long enough to measure: timer coalescing (a background process on
    // macOS) ends a 5 ms sleep up to 5 ms late, and any sleep up to ~18 ms
    // late. Restarting the sleep at a resume would still end 50 ms late.
    const int64_t kSleepNs = 100'000'000, kLateNs = 30'000'000;

    printf("1. never suspended, every guest clock reads its host clock as before\n");
    {
        CHECK(!guesttime::Suspended() && guesttime::Epoch() == 0 && guesttime::PausedNs() == 0 && guesttime::LastResumeNs() == 0,
            "a fresh process isn't at rest");
        uint64_t outside[kClocks] = {};
        for (int i = 0; i < 100'000; i++)
        {
            const Reading before = Host(), guest = Guest(), after = Host();
            for (int c = 0; c < kClocks; c++)
                outside[c] += guest.v[c] < before.v[c] || guest.v[c] > after.v[c];
        }
        for (int c = 0; c < kClocks; c++)
            CHECK(outside[c] == 0, "%s: %llu of 100000 readings outside the host readings around them", kClockName[c],
                (unsigned long long)outside[c]);
    }

    printf("2. Suspend and Resume twice each: the second call does nothing, and the hook runs once after each resume, on a thread of its own\n");
    {
        hook::caller = std::this_thread::get_id();
        guesttime::SetResumeHook(hook::Run);
        CHECK(!guesttime::Resume(), "Resume while running changed something");
        std::this_thread::sleep_for(20ms);
        CHECK(hook::calls == 0, "the hook ran with no resume");

        const int kCycles = 200;
        Suspensions s;
        int badEpoch = 0, noHook = 0, staleHook = 0, badLastResume = 0;
        for (int i = 0; i < kCycles; i++)
        {
            // As Suspensions::Run, with the epoch and LastResumeNs checked
            // in between; then nothing suspends until the hook has run.
            const bool suspended = guesttime::Suspend(), suspendedAgain = guesttime::Suspend();
            badEpoch += guesttime::Epoch() != uint64_t(i + 1) || !guesttime::Suspended();
            const Reading frozen = Guest();
            Pause(gap());
            const Reading still = Guest();
            const int64_t r1 = guesttime::RawNs();
            const bool resumed = guesttime::Resume();
            const int64_t r2 = guesttime::RawNs();
            const bool resumedAgain = guesttime::Resume();
            s.notIdempotent += !(suspended && !suspendedAgain && resumed && !resumedAgain);
            for (int c = 0; c < kClocks; c++)
                s.notFrozen += frozen.v[c] != still.v[c];
            const int64_t last = guesttime::LastResumeNs();
            badLastResume += last < r1 || last > r2 || guesttime::Suspended();
            if (!WaitFor([&] { return hook::calls == uint64_t(i + 1); }))
                noHook++;
            else if (hook::lastResume != last)
                staleHook++;
        }
        std::this_thread::sleep_for(20ms);
        printf("   %d cycles: hook ran %llu times\n", kCycles, (unsigned long long)hook::calls.load());
        CHECK(s.notIdempotent == 0, "%llu cycles where a second call changed something or a first didn't",
            (unsigned long long)s.notIdempotent);
        CHECK(s.notFrozen == 0, "%llu readings moved while suspended", (unsigned long long)s.notFrozen);
        CHECK(badEpoch == 0, "%d suspensions with the wrong Epoch() or not Suspended()", badEpoch);
        CHECK(badLastResume == 0, "%d resumes whose LastResumeNs() isn't the host time of the call", badLastResume);
        CHECK(noHook == 0 && hook::calls == uint64_t(kCycles), "%d resumes without a hook call, %llu calls for %d resumes", noHook,
            (unsigned long long)hook::calls.load(), kCycles);
        CHECK(staleHook == 0, "%d hook calls that didn't see their resume", staleHook);
        CHECK(hook::onCaller == 0, "%llu hook calls on Resume's own thread", (unsigned long long)hook::onCaller.load());
        CHECK(hook::whileSuspended == 0, "%llu hook calls before the resume took effect", (unsigned long long)hook::whileSuspended.load());
    }

    printf("3. SleepUntil(now + 100 ms) sleeps 100 ms of guest time: on the host, 100 ms plus the time suspended\n");
    {
        auto report = [&](const char* name, const Sleeper& sleeper, int64_t leastSuspended, int64_t mostSuspended, uint64_t suspensions) {
            const int64_t host = sleeper.hostEnd - sleeper.hostStart, guest = sleeper.guestEnd - sleeper.guestStart;
            const int64_t least = kSleepNs + leastSuspended - Units(1, kGainUs) * int64_t(suspensions);
            const int64_t most = kSleepNs + mostSuspended + kLateNs;
            printf("   %s: %.3f ms (%.3f-%.3f ms suspended), guest %.3f ms\n", name, host / 1e6, leastSuspended / 1e6,
                mostSuspended / 1e6, guest / 1e6);
            CHECK(host >= least && host <= most, "%s: slept %.3f ms of host time, not %.3f-%.3f", name, host / 1e6, least / 1e6,
                most / 1e6);
            CHECK(guest >= kSleepNs && guest <= kSleepNs + kLateNs, "%s: slept %.3f ms of guest time", name, guest / 1e6);
        };

        // Suspensions of `suspendMs` while it sleeps, each after `runMs` of
        // host time; tried again when the sleep ended before one began (a
        // busy machine), which proves nothing.
        auto across = [&](const char* name, std::initializer_list<std::pair<int, int>> windows) {
            for (int attempt = 0; attempt < 3; attempt++)
            {
                Suspensions s;
                Sleeper sleeper(kSleepNs);
                bool straddled = true;
                for (auto [runMs, suspendMs] : windows)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(runMs));
                    straddled = straddled && !sleeper.done;
                    s.Run(suspendMs * 1000);
                }
                sleeper.Join();
                CHECK(s.notIdempotent == 0 && s.notFrozen == 0 && s.reads.Backwards() == 0, "%s: the suspensions themselves", name);
                if (!straddled)
                    continue;
                report(name, sleeper, s.least[1], s.most[1], s.count);
                return;
            }
            CHECK(false, "%s: the sleep ended before a suspension three times (a busy machine?)", name);
        };
        across("50 ms suspended 60 ms in", { { 60, 50 } });
        across("30 ms suspended 25 ms in, and again 25 ms later", { { 25, 30 }, { 25, 30 } });

        {
            // Called while suspended: the whole 100 ms runs after the resume.
            guesttime::Suspend();
            Sleeper sleeper(kSleepNs);
            std::this_thread::sleep_for(50ms);
            const bool parked = !sleeper.done;
            const int64_t r1 = guesttime::RawNs();
            guesttime::Resume();
            const int64_t r2 = guesttime::RawNs();
            sleeper.Join();
            CHECK(parked, "called while suspended, it returned before the resume");
            report("called while suspended for 50 ms", sleeper, r1 - sleeper.hostStart, r2 - sleeper.hostStart, 1);
        }
        {
            // A deadline already passed still waits for the resume: the
            // game's threads park in Sleep rather than spin.
            guesttime::Suspend();
            Sleeper sleeper(-1);
            std::this_thread::sleep_for(10ms);
            const bool parked = !sleeper.done;
            const int64_t r1 = guesttime::RawNs();
            guesttime::Resume();
            const int64_t r2 = guesttime::RawNs();
            sleeper.Join();
            printf("   a passed deadline, called while suspended: returned %.3f ms after Resume was called\n", (sleeper.hostEnd - r1) / 1e6);
            CHECK(parked, "a passed deadline returned while suspended");
            CHECK(sleeper.hostEnd >= r1 && sleeper.hostEnd <= r2 + kLateNs, "returned %.3f ms after Resume was called",
                (sleeper.hostEnd - r1) / 1e6);
        }
    }

    const int kCycles = 20'000, kReaders = 6, kWaiters = 4;
    printf("4. %d suspensions of 0-200 us, 0-200 us apart, each call made twice, under %d readers and %d waiters\n", kCycles,
        kReaders, kWaiters);
    {
        std::atomic<bool> stop{ false };
        std::vector<Monotonic> readers(kReaders);
        struct alignas(128) Waiter
        {
            std::atomic<uint64_t> passes{ 0 }, parks{ 0 };
        };
        Waiter waiters[kWaiters], kernel;
        std::vector<std::thread> threads;

        // Readers: every clock, as fast as they can.
        for (int i = 0; i < kReaders; i++)
            threads.emplace_back([&, i] {
                Monotonic& m = readers[i];
                while (!stop.load(std::memory_order_relaxed))
                    m.Add(Guest());
            });
        // Threads that park while suspended, as the game's do in D3D's
        // waits, Sleep and yield.
        for (int i = 0; i < kWaiters; i++)
            threads.emplace_back([&, i] {
                std::mt19937 r{ uint32_t(i) };
                while (!stop.load(std::memory_order_relaxed))
                {
                    if (guesttime::Suspended())
                        waiters[i].parks++;
                    guesttime::WaitWhileSuspended();
                    waiters[i].passes++;
                    Pause(int64_t(r() % 100));
                }
            });
        // A kernel-style wait (kernel/dispatcher.cpp): it tests Suspended()
        // under its own lock and parks there without a timeout, so only the
        // hook, which takes that lock after the resume, can wake it.
        threads.emplace_back([&] {
            std::unique_lock lock(hook::mutex);
            while (!stop.load(std::memory_order_relaxed))
            {
                if (guesttime::Suspended())
                {
                    kernel.parks++;
                    hook::cv.wait(lock);
                }
                else
                    hook::cv.wait_for(lock, 50us);
                kernel.passes++;
            }
        });

        // After a resume, with nothing suspending meanwhile, every waiter gets
        // past its wait and the hook has seen the resume. A lost wakeup would
        // leave a waiter parked only until the next resume, so this is
        // checked after random ones as well as the last.
        int checkpoints = 0, leftBehind = 0, kernelLeftBehind = 0, hookMissed = 0, firstMiss = -1;
        auto checkpoint = [&](int cycle) {
            uint64_t before[kWaiters];
            for (int w = 0; w < kWaiters; w++)
                before[w] = waiters[w].passes;
            const uint64_t kernelBefore = kernel.passes;
            const int64_t resumed = guesttime::LastResumeNs();
            auto waitersPassed = [&] {
                for (int w = 0; w < kWaiters; w++)
                    if (waiters[w].passes == before[w])
                        return false;
                return true;
            };
            auto kernelPassed = [&] { return kernel.passes != kernelBefore; };
            auto hookRan = [&] { return hook::lastResume == resumed; };
            if (!WaitFor([&] { return waitersPassed() && kernelPassed() && hookRan(); }))
            {
                leftBehind += !waitersPassed();
                kernelLeftBehind += !kernelPassed();
                hookMissed += !hookRan();
                firstMiss = firstMiss < 0 ? cycle : firstMiss;
            }
            checkpoints++;
        };

        Suspensions s;
        const uint64_t epoch0 = guesttime::Epoch(), hookCalls0 = hook::calls;
        const Reading g0 = Guest(), h0 = Host();
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < kCycles; i++)
        {
            Pause(gap());
            s.Run(gap());
            if (rng() % 50 == 0 || i == kCycles - 1)
                checkpoint(i);
        }
        const Reading g1 = Guest(), h1 = Host();
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const int64_t r0 = guesttime::RawNs(), now = guesttime::NowNs(), r1 = guesttime::RawNs(), paused = guesttime::PausedNs();
        stop = true;
        for (std::thread& t : threads)
            t.join();

        // Every thread's readings, the suspending one's too.
        uint64_t reads = 0, backwards[kClocks] = {}, intoSuspension[kClocks] = {}, parks = 0;
        int64_t worst[kClocks] = {};
        readers.push_back(s.reads);
        for (const Monotonic& m : readers)
        {
            reads += m.reads;
            for (int c = 0; c < kClocks; c++)
            {
                backwards[c] += m.backwards[c];
                intoSuspension[c] += m.intoSuspension[c];
                worst[c] = std::max(worst[c], m.worst[c]);
            }
        }
        for (const Waiter& w : waiters)
            parks += w.parks;
        printf("   %.1f s; %llu readings; %llu waits parked, %llu kernel-style; hook ran %llu times for %d resumes; %d checkpoints\n",
            seconds, (unsigned long long)reads, (unsigned long long)parks, (unsigned long long)kernel.parks.load(),
            (unsigned long long)(hook::calls - hookCalls0), kCycles, checkpoints);
        for (int c = 0; c < kClocks; c++)
            CHECK(backwards[c] == 0, "%s went back %llu times (%llu from the last reading before a suspension to the frozen value), "
                  "up to %lld (%.2f us)", kClockName[c], (unsigned long long)backwards[c], (unsigned long long)intoSuspension[c],
                (long long)worst[c], double(worst[c]) / kPerUs[c]);
        CHECK(s.notIdempotent == 0, "%llu cycles where a second call changed something or a first didn't",
            (unsigned long long)s.notIdempotent);
        CHECK(s.notFrozen == 0, "%llu readings moved while suspended", (unsigned long long)s.notFrozen);
        CHECK(guesttime::Epoch() - epoch0 == uint64_t(kCycles), "Epoch() moved %llu for %d suspensions",
            (unsigned long long)(guesttime::Epoch() - epoch0), kCycles);
        CHECK(parks > 0 && kernel.parks > 0, "no wait parked (%llu, kernel-style %llu): the slow paths went untested",
            (unsigned long long)parks, (unsigned long long)kernel.parks.load());
        CHECK(leftBehind == 0 && kernelLeftBehind == 0, "a waiter left parked after %d resumes, kernel-style after %d (first at cycle %d)",
            leftBehind, kernelLeftBehind, firstMiss);
        CHECK(hookMissed == 0, "no hook call after %d resumes (first at cycle %d)", hookMissed, firstMiss);
        CHECK(hook::onCaller == 0, "%llu hook calls on Resume's own thread", (unsigned long long)hook::onCaller.load());

        // Guest time = host time - time suspended, give or take the margins
        // (at most 2 us a suspension) and 1 ms.
        for (int c = 0; c < kClocks; c++)
        {
            const int64_t lost = (h1.v[c] - h0.v[c]) - (g1.v[c] - g0.v[c]);
            const int64_t least = s.least[c] - Units(c, kGainUs) * int64_t(s.count) - Units(c, 1000);
            const int64_t most = s.most[c] + Units(c, 1000);
            printf("   %s: guest time %.3f ms behind the host's after %.3f-%.3f ms suspended\n", kClockName[c], Ms(c, lost),
                Ms(c, s.least[c]), Ms(c, s.most[c]));
            CHECK(lost >= least && lost <= most, "%s: %.3f ms behind, not %.3f-%.3f", kClockName[c], Ms(c, lost), Ms(c, least),
                Ms(c, most));
        }
        // PausedNs() is exactly how far the guest's ns are behind the host's.
        CHECK(paused >= r0 - now - 1000 && paused <= r1 - now + 1000, "PausedNs() %lld, host - guest %lld-%lld", (long long)paused,
            (long long)(r0 - now), (long long)(r1 - now));
        CHECK(!guesttime::Suspended(), "left suspended");
    }

    printf(g_failures ? "\n%d FAILED\n" : "\nall passed\n", g_failures.load());
    return g_failures ? 1 : 0;
}
