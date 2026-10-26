// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See backoff.h.
#include <stdafx.h>
#include "backoff.h"
#include "host_cpu.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>

#if defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#include <time.h>
#endif

namespace backoff
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        const char* const kNames[kKinds] = { "sleep0", "yield", "gpu wait" };
        const char* const kSwitches[kKinds] = { "NFSMW_SLEEP0_BACKOFF", nullptr, "NFSMW_GPU_WAIT_BACKOFF" };

        // A sleep this much later than asked is a stretched one (coalesced).
        constexpr auto kLate = std::chrono::milliseconds(2);
        // After one, the thread yields for this long.
        constexpr auto kYieldAfterLate = std::chrono::seconds(1);

        struct Counters
        {
            std::atomic<uint64_t> calls{ 0 }, sleeps{ 0 }, sleptNs{ 0 }, maxNs{ 0 }, late{ 0 }, yieldedAfterLate{ 0 };
        };
        Counters s_counters[kKinds];

        bool LogEnabled()
        {
            static const bool log = [] { const char* v = std::getenv("NFSMW_BACKOFF_LOG"); return v && v[0] == '1'; }();
            return log;
        }

        // A [backoff] line every 10 s, from whichever thread notices first.
        void MaybeLog(Clock::time_point now)
        {
            static std::atomic<int64_t> s_next{ 0 };
            static const Clock::time_point s_start = now;
            int64_t t = std::chrono::duration_cast<std::chrono::milliseconds>(now - s_start).count();
            int64_t next = s_next.load(std::memory_order_relaxed);
            if (next == 0)
            {
                s_next.compare_exchange_strong(next, t + 10000);
                return;
            }
            if (t < next || !s_next.compare_exchange_strong(next, t + 10000))
                return;
            char line[640];
            int n = snprintf(line, sizeof(line), "[backoff] %.0f s:", t / 1000.0);
            for (int k = 0; k < kKinds && n < int(sizeof(line)); k++)
            {
                Counters& c = s_counters[k];
                uint64_t calls = c.calls.exchange(0), sleeps = c.sleeps.exchange(0), ns = c.sleptNs.exchange(0);
                uint64_t maxNs = c.maxNs.exchange(0), late = c.late.exchange(0), yielded = c.yieldedAfterLate.exchange(0);
                n += snprintf(line + n, sizeof(line) - n, "%s %s %llu/s, slept %llu/s (avg %.0f us, max %.0f us), late %llu, yielded after late %llu",
                    k ? ";" : "", kNames[k], (unsigned long long)(calls / 10), (unsigned long long)(sleeps / 10),
                    sleeps ? ns / 1000.0 / sleeps : 0.0, maxNs / 1000.0, (unsigned long long)late, (unsigned long long)yielded);
            }
            fprintf(stderr, "%s\n", line);
        }

        // Once per thread that sleeps (with the log): its name and, on Apple,
        // its QoS class, which decides how much macOS may stretch its timers.
        void NoteThread()
        {
            thread_local bool noted = false;
            if (noted)
                return;
            noted = true;
#if defined(__APPLE__)
            char name[64] = {};
            pthread_getname_np(pthread_self(), name, sizeof(name));
            qos_class_t qos = QOS_CLASS_UNSPECIFIED;
            int relative = 0;
            pthread_get_qos_class_np(pthread_self(), &qos, &relative);
            fprintf(stderr, "[backoff] thread %s sleeps (QoS class 0x%x)\n", name[0] ? name : "?", unsigned(qos));
#endif
        }
    }

    bool Enabled(Kind kind)
    {
#if defined(__linux__) || defined(__APPLE__)
        static const bool enabled[kKinds] = {
            [] { const char* v = std::getenv(kSwitches[kSleep0]); return !v || v[0] != '0'; }(),
            false,  // NtYieldExecution: counted only
            [] { const char* v = std::getenv(kSwitches[kGpuWait]); return !v || v[0] != '0'; }(),
        };
        return enabled[kind];
#else
        (void)kind;
        return false;
#endif
    }

    void Sleep(Kind kind, std::chrono::microseconds time)
    {
        thread_local Clock::time_point yieldUntil{};
        const bool log = LogEnabled();
        auto start = Clock::now();
        if (start < yieldUntil)
        {
            if (log)
                s_counters[kind].yieldedAfterLate.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
            return;
        }
#if defined(__APPLE__)
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(time).count();
        timespec request{ time_t(ns / 1000000000), long(ns % 1000000000) };
        nanosleep(&request, nullptr);
#else
        hostcpu::TightenTimerSlack();
        std::this_thread::sleep_for(time);
#endif
        auto end = Clock::now();
        bool late = end - start > time + kLate;
        if (late)
            yieldUntil = end + kYieldAfterLate;
        if (log)
        {
            NoteThread();
            Counters& c = s_counters[kind];
            uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
            c.sleeps.fetch_add(1, std::memory_order_relaxed);
            c.sleptNs.fetch_add(ns, std::memory_order_relaxed);
            uint64_t max = c.maxNs.load(std::memory_order_relaxed);
            while (ns > max && !c.maxNs.compare_exchange_weak(max, ns, std::memory_order_relaxed))
            {
            }
            if (late)
                c.late.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void ZeroDelay(Kind kind)
    {
        if (LogEnabled())
        {
            s_counters[kind].calls.fetch_add(1, std::memory_order_relaxed);
            MaybeLog(Clock::now());
        }
        if (Enabled(kind))
        {
            thread_local Clock::time_point firstZero[kKinds]{}, lastZero[kKinds]{};
            auto now = Clock::now();
            if (now - lastZero[kind] > std::chrono::microseconds(300))
                firstZero[kind] = now;
            lastZero[kind] = now;
            if (now - firstZero[kind] > std::chrono::milliseconds(1))
            {
                Sleep(kind, std::chrono::microseconds(100));
                lastZero[kind] = Clock::now();
                return;
            }
        }
        std::this_thread::yield();
    }

    void CountGpuWaitPoll()
    {
        if (LogEnabled())
        {
            s_counters[kGpuWait].calls.fetch_add(1, std::memory_order_relaxed);
            MaybeLog(Clock::now());
        }
    }

    void CountYield()
    {
        if (LogEnabled())
        {
            s_counters[kYield].calls.fetch_add(1, std::memory_order_relaxed);
            MaybeLog(Clock::now());
        }
    }
}
