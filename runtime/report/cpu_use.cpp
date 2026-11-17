// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See cpu_use.h.
#include "cpu_use.h"

#include <cpu/efficiency.h>
#include <report/report.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#define SB_CPU_USE_APPLE 1
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#elif defined(__linux__) && !defined(__ANDROID__)
#define SB_CPU_USE_LINUX 1
#include <report/proc_stat.h>

#include <dirent.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace cpuuse
{
    std::atomic<uint64_t> g_counts[kCounters];

    namespace
    {
        // NFSMW_CPU_LOG=<seconds>: the period; 0 (unset) is off.
        int Period()
        {
            static const int period = [] {
                const char* v = std::getenv("NFSMW_CPU_LOG");
                int n = v ? std::atoi(v) : 0;
                return n > 0 ? n : 0;
            }();
            return period;
        }

#if defined(SB_CPU_USE_APPLE) || defined(SB_CPU_USE_LINUX)
        // Same-named threads merged (several unnamed ones, or a name used
        // twice), the busiest first; `total` is their sum.
        std::vector<std::pair<uint64_t, std::string>> ByTime(std::vector<std::pair<std::string, uint64_t>> threads, uint64_t& total)
        {
            std::sort(threads.begin(), threads.end());
            std::vector<std::pair<uint64_t, std::string>> byTime;
            total = 0;
            for (size_t i = 0; i < threads.size();)
            {
                size_t j = i;
                uint64_t ns = 0;
                while (j < threads.size() && threads[j].first == threads[i].first)
                    ns += threads[j++].second;
                byTime.push_back({ ns, threads[i].first });
                total += ns;
                i = j;
            }
            std::sort(byTime.begin(), byTime.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            return byTime;
        }

        // "; threads: g2-8284FE40 0.104, ..., 16 others 0.025; polls a
        // second: ..." (the polls' counts start again).
        std::string ThreadsAndPolls(const std::vector<std::pair<uint64_t, std::string>>& byTime, double wallNs, double seconds)
        {
            std::string line = "; threads:";
            size_t shown = 0;
            uint64_t rest = 0;
            size_t restCount = 0;
            for (const auto& [ns, name] : byTime)
            {
                const double cores = double(ns) / wallNs;
                if (shown < 14 && cores >= 0.005)
                {
                    line += std::format(" {} {:.3f},", name, cores);
                    shown++;
                }
                else
                {
                    rest += ns;
                    restCount++;
                }
            }
            line += std::format(" {} others {:.3f}", restCount, double(rest) / wallNs);
            static const char* const kNames[kCounters] = { "CP idle yields", "CP idle sleeps", "CP wait yields", "CP wait sleeps",
                "main delays" };
            line += "; polls a second:";
            for (int c = 0; c < kCounters; c++)
                line += std::format("{} {} {:.0f}", c ? "," : "", kNames[c], double(g_counts[c].exchange(0, std::memory_order_relaxed)) / seconds);
            return line;
        }
#endif

#if defined(SB_CPU_USE_APPLE)
        struct Thread
        {
            uint64_t ns = 0;      // user + system time so far
            uint64_t round = 0;   // the last sample that saw it
            std::string name;
        };

        struct Task
        {
            uint64_t cpu = 0;       // user + system, Mach absolute time (all threads, ended ones too)
            uint64_t ptime = 0;     // of it on performance cores
            uint64_t energy = 0;    // nJ (Apple silicon only)
            uint64_t gpu = 0;       // the GPU's time for this task (ns)
            uint64_t idleWakeups = 0, interruptWakeups = 0, timerWakeups = 0;
            bool haveEnergy = false;
        };

        std::unordered_map<uint64_t, Thread> s_threads;  // by thread id
        uint64_t s_round = 0;
        uint64_t s_mainThread = 0;
        double s_last = -1;
        Task s_task;

        bool ReadTask(Task& t)
        {
            task_power_info_v2_data_t info{};
            mach_msg_type_number_t count = TASK_POWER_INFO_V2_COUNT;
            if (task_info(mach_task_self(), TASK_POWER_INFO_V2, task_info_t(&info), &count) != KERN_SUCCESS)
                return false;
            t.cpu = info.cpu_energy.total_user + info.cpu_energy.total_system;
            t.idleWakeups = info.cpu_energy.task_platform_idle_wakeups;
            t.interruptWakeups = info.cpu_energy.task_interrupt_wakeups;
            t.timerWakeups = info.cpu_energy.task_timer_wakeups_bin_1 + info.cpu_energy.task_timer_wakeups_bin_2;
            t.gpu = info.gpu_energy.task_gpu_utilisation;
            t.haveEnergy = count >= TASK_POWER_INFO_V2_COUNT;
            if (t.haveEnergy)
            {
                t.ptime = info.task_ptime;
#if defined(__arm64__) || defined(__arm__)
                t.energy = info.task_energy;
#else
                t.haveEnergy = false;
#endif
            }
            return true;
        }

        // Each live thread's CPU time since the last sample (a thread that
        // started meanwhile: all of its time), by name; threads that ended
        // meanwhile are gone from the list (their last stretch isn't counted).
        std::vector<std::pair<std::string, uint64_t>> ReadThreads()
        {
            std::vector<std::pair<std::string, uint64_t>> spent;
            thread_act_array_t list = nullptr;
            mach_msg_type_number_t n = 0;
            if (task_threads(mach_task_self(), &list, &n) != KERN_SUCCESS)
                return spent;
            const uint64_t round = ++s_round;
            for (mach_msg_type_number_t i = 0; i < n; i++)
            {
                thread_identifier_info_data_t id{};
                mach_msg_type_number_t count = THREAD_IDENTIFIER_INFO_COUNT;
                thread_extended_info_data_t ext{};
                mach_msg_type_number_t extCount = THREAD_EXTENDED_INFO_COUNT;
                if (thread_info(list[i], THREAD_IDENTIFIER_INFO, thread_info_t(&id), &count) == KERN_SUCCESS &&
                    thread_info(list[i], THREAD_EXTENDED_INFO, thread_info_t(&ext), &extCount) == KERN_SUCCESS)
                {
                    const uint64_t ns = ext.pth_user_time + ext.pth_system_time;
                    auto [it, born] = s_threads.try_emplace(id.thread_id);
                    Thread& t = it->second;
                    const uint64_t delta = born ? (round == 1 ? 0 : ns) : ns - std::min(ns, t.ns);
                    t.ns = ns;
                    t.round = round;
                    if (id.thread_id == s_mainThread)
                        t.name = "host-main";
                    else if (ext.pth_name[0])
                        t.name.assign(ext.pth_name, strnlen(ext.pth_name, sizeof(ext.pth_name)));
                    else if (t.name.empty())
                        t.name = std::format("thread-{}", id.thread_id);
                    if (round > 1)
                        spent.push_back({ t.name, delta });
                }
                mach_port_deallocate(mach_task_self(), list[i]);
            }
            vm_deallocate(mach_task_self(), vm_address_t(list), n * sizeof(thread_act_t));
            for (auto it = s_threads.begin(); it != s_threads.end();)
                it = it->second.round == round ? std::next(it) : s_threads.erase(it);
            return spent;
        }

        double AbsToNs(uint64_t abs)
        {
            static const double scale = [] {
                mach_timebase_info_data_t tb{};
                mach_timebase_info(&tb);
                return tb.denom ? double(tb.numer) / double(tb.denom) : 1.0;
            }();
            return double(abs) * scale;
        }

        void Sample(double now, const std::string& ab)
        {
            Task task;
            const bool haveTask = ReadTask(task);
            std::vector<std::pair<std::string, uint64_t>> threads = ReadThreads();
            const double seconds = now - s_last;
            const bool first = s_last < 0;
            const Task before = s_task;
            s_task = task;
            s_last = now;
            if (first || seconds <= 0)
                return;
            uint64_t total = 0;
            const std::vector<std::pair<uint64_t, std::string>> byTime = ByTime(std::move(threads), total);
            const double wallNs = seconds * 1e9;
            std::string line = std::format("[cpu] {:.1f} s, over {:.1f} s{}: {:.2f} cores", now, seconds, ab.empty() ? "" : " (" + ab + ")",
                double(total) / wallNs);
            if (haveTask)
            {
                const uint64_t cpu = task.cpu - std::min(task.cpu, before.cpu);
                line += std::format(" (task counter {:.2f}", AbsToNs(cpu) / wallNs);
                if (task.haveEnergy && cpu > 0)
                    line += std::format("; {:.0f}% on P cores", 100.0 * double(task.ptime - std::min(task.ptime, before.ptime)) / double(cpu));
                line += ")";
                if (task.haveEnergy)
                    line += std::format(", CPU energy {:.3f} W", double(task.energy - std::min(task.energy, before.energy)) / 1e9 / seconds);
                line += std::format(", GPU {:.0f} ms/s", double(task.gpu - std::min(task.gpu, before.gpu)) / 1e6 / seconds);
                line += std::format(", wake-ups {:.0f}/s from idle, {:.0f}/s by interrupts, timers {:.0f}/s",
                    double(task.idleWakeups - std::min(task.idleWakeups, before.idleWakeups)) / seconds,
                    double(task.interruptWakeups - std::min(task.interruptWakeups, before.interruptWakeups)) / seconds,
                    double(task.timerWakeups - std::min(task.timerWakeups, before.timerWakeups)) / seconds);
            }
            line += ThreadsAndPolls(byTime, wallNs, seconds);
            fprintf(stderr, "%s\n", line.c_str());
        }
#elif defined(SB_CPU_USE_LINUX)
        // Linux (SteamOS; item #71 on the Deck): /proc/self/task/*/stat for
        // the threads (user + system time in clock ticks, the name as comm
        // holds it: 15 bytes at most), getrusage for the whole process (its
        // ended threads too) and its context switches. A voluntary switch is
        // a thread that blocked (a sleep, a wait); a yield that hands the CPU
        // to another thread counts as an involuntary one, as does preemption.
        // These stand where Apple's line has its wake-ups; there is no CPU
        // energy for a process here (the Deck's APU power is read from
        // outside: gpu_metrics).
        struct Thread
        {
            uint64_t ticks = 0;   // user + system time so far
            uint64_t round = 0;   // the last sample that saw it
            std::string name;
        };

        struct Task
        {
            uint64_t cpuUs = 0;   // user + system, all threads (ended ones too)
            uint64_t voluntary = 0, involuntary = 0;
        };

        std::unordered_map<long, Thread> s_threads;  // by thread id
        uint64_t s_round = 0;
        long s_mainThread = 0;
        double s_last = -1;
        Task s_task;

        bool ReadTask(Task& t)
        {
            rusage u{};
            if (getrusage(RUSAGE_SELF, &u) != 0)
                return false;
            t.cpuUs = uint64_t(u.ru_utime.tv_sec + u.ru_stime.tv_sec) * 1000000u + uint64_t(u.ru_utime.tv_usec + u.ru_stime.tv_usec);
            t.voluntary = uint64_t(u.ru_nvcsw);
            t.involuntary = uint64_t(u.ru_nivcsw);
            return true;
        }

        // A small file at once (stat is one line of a few hundred bytes).
        bool ReadSmall(const char* path, std::string& out)
        {
            const int fd = open(path, O_RDONLY | O_CLOEXEC);
            if (fd < 0)
                return false;
            char buf[1024];
            const ssize_t n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n <= 0)
                return false;
            out.assign(buf, size_t(n));
            return true;
        }

        // Each live thread's CPU time since the last sample (in ns), by name,
        // as the Apple version does (a thread that started meanwhile: all of
        // its time; threads that ended meanwhile are gone from the list).
        std::vector<std::pair<std::string, uint64_t>> ReadThreads()
        {
            static const uint64_t tick = [] {
                const long t = sysconf(_SC_CLK_TCK);
                return uint64_t(1000000000 / (t > 0 ? t : 100));
            }();
            std::vector<std::pair<std::string, uint64_t>> spent;
            DIR* dir = opendir("/proc/self/task");
            if (!dir)
                return spent;
            const uint64_t round = ++s_round;
            std::string text;
            char path[64];
            while (const dirent* e = readdir(dir))
            {
                char* end = nullptr;
                const long tid = std::strtol(e->d_name, &end, 10);
                if (tid <= 0 || !end || *end != 0)
                    continue;
                snprintf(path, sizeof(path), "/proc/self/task/%ld/stat", tid);
                procstat::TaskStat st;
                if (!ReadSmall(path, text) || !procstat::ParseTaskStat(text, st))
                    continue;
                const uint64_t ticks = st.utime + st.stime;
                auto [it, born] = s_threads.try_emplace(tid);
                Thread& t = it->second;
                const uint64_t delta = born ? (round == 1 ? 0 : ticks) : ticks - std::min(ticks, t.ticks);
                t.ticks = ticks;
                t.round = round;
                if (tid == s_mainThread)
                    t.name = "host-main";
                else if (!st.name.empty())
                    t.name = std::move(st.name);
                else if (t.name.empty())
                    t.name = std::format("thread-{}", tid);
                if (round > 1)
                    spent.push_back({ t.name, delta * tick });
            }
            closedir(dir);
            for (auto it = s_threads.begin(); it != s_threads.end();)
                it = it->second.round == round ? std::next(it) : s_threads.erase(it);
            return spent;
        }

        void Sample(double now, const std::string& ab)
        {
            Task task;
            const bool haveTask = ReadTask(task);
            std::vector<std::pair<std::string, uint64_t>> threads = ReadThreads();
            const double seconds = now - s_last;
            const bool first = s_last < 0;
            const Task before = s_task;
            s_task = task;
            s_last = now;
            if (first || seconds <= 0)
                return;
            uint64_t total = 0;
            const std::vector<std::pair<uint64_t, std::string>> byTime = ByTime(std::move(threads), total);
            const double wallNs = seconds * 1e9;
            std::string line = std::format("[cpu] {:.1f} s, over {:.1f} s{}: {:.2f} cores", now, seconds, ab.empty() ? "" : " (" + ab + ")",
                double(total) / wallNs);
            if (haveTask)
            {
                line += std::format(" (task counter {:.2f})", double(task.cpuUs - std::min(task.cpuUs, before.cpuUs)) * 1000.0 / wallNs);
                line += std::format(", context switches {:.0f}/s voluntary, {:.0f}/s involuntary",
                    double(task.voluntary - std::min(task.voluntary, before.voluntary)) / seconds,
                    double(task.involuntary - std::min(task.involuntary, before.involuntary)) / seconds);
            }
            line += ThreadsAndPolls(byTime, wallNs, seconds);
            fprintf(stderr, "%s\n", line.c_str());
        }
#endif
    }

    bool On()
    {
#if defined(SB_CPU_USE_APPLE) || defined(SB_CPU_USE_LINUX)
        return Period() > 0;
#else
        return false;
#endif
    }

    void Poll()
    {
#if defined(SB_CPU_USE_APPLE) || defined(SB_CPU_USE_LINUX)
        const int period = Period();
        if (period <= 0)
            return;
        static double s_next = 0;
        const double now = report::SessionSeconds();
        if (now < s_next)
            return;
        if (s_mainThread == 0)
        {
#if defined(SB_CPU_USE_APPLE)
            pthread_threadid_np(nullptr, &s_mainThread);
#else
            s_mainThread = long(syscall(SYS_gettid));
#endif
            fprintf(stderr, "[cpu] NFSMW_CPU_LOG: a [cpu] line every %d s\n", period);
        }
        s_next = now + period;
        // NFSMW_EFFICIENCY_AB: the period that ends here ran with what this
        // flips; the next runs with the other.
        const std::string ab = s_last < 0 ? std::string() : efficiency::AlternateForAb();
        Sample(now, ab);
#endif
    }
}
