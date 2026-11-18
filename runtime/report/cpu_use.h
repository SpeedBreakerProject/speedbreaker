// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Where the CPU time and the power go, for measuring (item #71, iPhone heat):
// NFSMW_CPU_LOG=<seconds> logs a [cpu] line every that many seconds (10 is a
// good period; off by default) with, over that period:
//   - the process's CPU in cores (all its threads' user + system time over
//     the wall time), the share of it on performance cores, and the CPU
//     energy the kernel counts for the process (Apple silicon: its energy
//     model per thread, in watts here);
//   - the GPU time the kernel counts for the process (ms a second);
//   - the wake-ups from idle and from interrupts (a second): what keeps cores
//     out of their deep idle states even when the threads do little;
//   - the busiest threads by name, in cores (a guest thread is
//     "g<id>-<entry>", the host's main thread "host-main").
// Apple (macOS and iOS: Mach's task_info TASK_POWER_INFO_V2 and thread_info
// for the process's own task, no entitlement). Linux (not Android; SteamOS,
// item #71 on the Deck): the cores, the task counter (getrusage, ended
// threads too) and the threads by name (/proc/self/task/*/stat, the name as
// comm holds it, 15 bytes at most; report/proc_stat.h), and in place of the
// wake-ups the process's context switches a second, voluntary (a thread
// blocked: a sleep, a wait) and involuntary (preempted, or a yield that gave
// the CPU to another thread); no energy or GPU time (Linux keeps neither
// per process; the Deck's APU power, for the whole machine, is read from
// outside). Elsewhere the line isn't written. Main
// thread: Poll() from the frame loop (once a second at most it does more
// than read a clock).
#pragma once
#include <atomic>
#include <cstdint>

namespace cpuuse
{
    // Main thread, every frame.
    void Poll();

    // Whether NFSMW_CPU_LOG is set (the counters below count only then).
    bool On();

    // Polls of the loops that wait for work by waking again and again, a
    // second in the [cpu] line: the command processor's idle loop and its
    // WAIT_REG_MEM (yields, short sleeps), and the host's main thread waiting
    // for the game's next picture (its short delays).
    enum Counter
    {
        kCpIdleYield,
        kCpIdleSleep,
        kCpWaitYield,
        kCpWaitSleep,
        kMainDelay,
        kCounters
    };
    extern std::atomic<uint64_t> g_counts[kCounters];
    inline void Count(Counter c)
    {
        if (On())
            g_counts[c].fetch_add(1, std::memory_order_relaxed);
    }
}
