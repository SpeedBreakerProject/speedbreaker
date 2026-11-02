// runtime/debug/wake_stats.h
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#if defined(__APPLE__) || defined(__linux__)
#include <pthread.h>
#endif

namespace wakestats {
    inline int64_t NowNs() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    inline const int64_t g_startNs = NowNs();

    struct Site {
        const char* name; std::atomic<uint64_t> n; Site* next; uint64_t last = 0;
        Site(const char* nm, Site* nx) : name(nm), n(0), next(nx) {}
    };
    inline std::atomic<Site*> g_sites{ nullptr };

    // ---- history: one slot per event in a ring; the writer publishes `idx` last ----
    struct Slot {
        std::atomic<uint64_t> idx{ 0 };
        std::atomic<int64_t> ns{ 0 };
        std::atomic<const Site*> site{ nullptr };
        std::atomic<uint32_t> tid{ 0 };
    };
    inline constexpr uint64_t kRing = 1u << 16;
    inline Slot g_ring[kRing];
    inline std::atomic<uint64_t> g_next{ 0 };
    inline std::atomic<bool> g_historyOn{ false };
    inline double g_windowMs = 100;
    inline int g_maxLines = 200;
    inline constexpr int64_t kRunGapNs = 2'000'000;   // events of one site further apart than this start a new run

    inline constexpr int kMaxThreads = 256;
    inline char g_threadNames[kMaxThreads][24];
    inline std::atomic<uint32_t> g_threadCount{ 0 };
    inline uint32_t ThreadId() {
        thread_local uint32_t id = [] {
            uint32_t i = std::min<uint32_t>(g_threadCount.fetch_add(1), kMaxThreads - 1);
            g_threadNames[i][0] = 0;
#if defined(__APPLE__) || defined(__linux__)
            pthread_getname_np(pthread_self(), g_threadNames[i], sizeof g_threadNames[i]);
#endif
            if (!g_threadNames[i][0]) snprintf(g_threadNames[i], sizeof g_threadNames[i], "thread %u", i);
            return i;
        }();
        return id;
    }

    inline void Record(const Site& s) {
        const uint64_t i = g_next.fetch_add(1, std::memory_order_relaxed);
        Slot& slot = g_ring[i & (kRing - 1)];
        slot.ns.store(NowNs(), std::memory_order_relaxed);
        slot.site.store(&s, std::memory_order_relaxed);
        slot.tid.store(ThreadId(), std::memory_order_relaxed);
        slot.idx.store(i + 1, std::memory_order_release);
    }
    inline void Hit(Site& s) {
        s.n.fetch_add(1, std::memory_order_relaxed);
        if (g_historyOn.load(std::memory_order_relaxed)) Record(s);
    }

    inline void PrintHistory(double windowMs = -1) {
        if (windowMs < 0) windowMs = g_windowMs;
        const int64_t cutoff = NowNs() - int64_t(windowMs * 1e6);
        struct Ev { int64_t ns; const Site* site; uint32_t tid; };
        const uint64_t next = g_next.load(std::memory_order_acquire);
        std::vector<Ev> evs;
        for (uint64_t back = 1; back <= kRing && back <= next; back++) {
            const uint64_t i = next - back;
            Slot& s = g_ring[i & (kRing - 1)];
            if (s.idx.load(std::memory_order_acquire) != i + 1) continue;
            Ev e{ s.ns.load(std::memory_order_relaxed), s.site.load(std::memory_order_relaxed), s.tid.load(std::memory_order_relaxed) };
            if (s.idx.load(std::memory_order_acquire) != i + 1) continue;   // overwritten meanwhile
            if (e.ns < cutoff) break;
            evs.push_back(e);
        }
        std::sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) { return a.ns < b.ns; });

        struct Run { int64_t first, last; const Site* site; uint32_t tid, count; };
        std::vector<Run> runs;
        for (const Ev& e : evs) {
            if (!runs.empty() && runs.back().site == e.site && e.ns - runs.back().last <= kRunGapNs) {
                runs.back().last = e.ns; runs.back().count++;
            } else {
                runs.push_back({ e.ns, e.ns, e.site, e.tid, 1 });
            }
        }
        const size_t from = runs.size() > size_t(g_maxLines) ? runs.size() - size_t(g_maxLines) : 0;
        fprintf(stderr, "[history] ---- last %.0f ms: %zu events in %zu runs (showing %zu) ----\n",
            g_windowMs, evs.size(), runs.size(), runs.size() - from);
        for (size_t k = from; k < runs.size(); k++) {
            const Run& r = runs[k];
            char span[48] = "";
            if (r.count > 1) snprintf(span, sizeof span, "  (over %.0f us)", double(r.last - r.first) / 1e3);
            fprintf(stderr, "[history] %11.6f %5ux %-34s [%s]%s\n", double(r.first - g_startNs) / 1e9, r.count,
                r.site ? r.site->name : "?", g_threadNames[r.tid], span);
        }
    }

    inline void DumpNow(double windowMs, const char* why) {
        if (!g_historyOn.load()) return;
        static std::atomic<int64_t> next{ 0 };
        const int64_t now = NowNs();
        if (now < next.load()) return;
        next.store(now + 2'000'000'000);
        fprintf(stderr, "[history] ==== %s ====\n", why);
        PrintHistory(windowMs);
    }

    inline void PrintRates() {
        for (Site* s = g_sites.load(); s; s = s->next) {
            uint64_t n = s->n.load(std::memory_order_relaxed);
            fprintf(stderr, "[wakes] %-40s %7.0f/s\n", s->name, double(n - s->last) / 2);
            s->last = n;
        }
    }

    inline bool g_printRates = false;
    inline void ReportThread() {
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            if (g_printRates) PrintRates();
            if (g_historyOn.load()) PrintHistory();
        }
    }

    inline Site& Register(const char* name) {
        static std::once_flag started;
        std::call_once(started, [] {
            g_printRates = std::getenv("NFSMW_LOG_WAKES") != nullptr;
            if (const char* h = std::getenv("NFSMW_WAKE_HISTORY")) {
                g_windowMs = std::max(1.0, std::atof(h));
                if (const char* l = std::getenv("NFSMW_WAKE_HISTORY_LINES")) g_maxLines = std::max(1, std::atoi(l));
                g_historyOn.store(true);
            }
            if (g_printRates || g_historyOn.load()) std::thread(ReportThread).detach();
        });
        Site* s = new Site(name, g_sites.load());
        while (!g_sites.compare_exchange_weak(s->next, s)) {}
        return *s;
    }
    inline Site& RegisterForThread(const char* prefix) {
        char name[32] = {};
#if defined(__APPLE__) || defined(__linux__)
        pthread_getname_np(pthread_self(), name, sizeof name);
#endif
        return Register(strdup((std::string(prefix) + " " + name).c_str()));
    }
}
#define SB_WAKE(name) do { static wakestats::Site& s_ = wakestats::Register(name); wakestats::Hit(s_); } while (0)
#define SB_WAKE_T(prefix) do { thread_local wakestats::Site& s_ = wakestats::RegisterForThread(prefix); wakestats::Hit(s_); } while (0)