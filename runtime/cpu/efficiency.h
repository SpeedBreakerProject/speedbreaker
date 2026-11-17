// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The efficiency changes of item #71 (iPhone heat and power): ways of waiting
// that cost less CPU and fewer wake-ups for the same frames, each with a
// switch so it can be measured against the old way and turned off:
//   - kCpSpin: on iOS, and on Linux (not Android), the command processor
//     polls for new work with yields for 0.5 ms, not 2 ms, after its last
//     work and at the start of a WAIT_REG_MEM before it backs off to 100 us
//     sleeps (macOS has had 0.5 ms since v0.2.2; on Android and Windows
//     nothing changes). NFSMW_CP_IDLE_SPIN_US still sets it outright on any
//     platform.
//   - kGpuWait: D3D's GPU busy-waits (sub_825A5B20's hook) past their 20 us
//     of spin wait for the command processor's next step (a write made
//     visible to the game, or a batch finished; at most 1 ms) instead of
//     sleeping 50 us a poll: one wake-up where there were dozens, and no wait
//     that ends up to 50 us after what it waited for (backoff::GpuWaitPoll).
//     Windows has waited this way since its wait fix.
//   - kSleep0: the game's Sleep(0) polls (KeDelayExecutionThread with no
//     time; the guest thread g2 above all) yield for 0.25 ms, not 1 ms, of
//     back-to-back polls before each poll sleeps 100 us (backoff::ZeroDelay):
//     the 1 ms of yields at the start of each of a few hundred polls a second
//     was the busiest spin left (M1 Pro race: g2 0.24 cores, 250 thousand
//     yields a second).
//   - kLateGuard: the back-off's guard against stretched timers (a 100 us
//     sleep that ends 2 ms or more late makes that thread yield, i.e. spin,
//     for the next second) needs 2 late sleeps of the thread's last 8, not
//     1: App Nap's stretched timers make every sleep late and still start it
//     at once, but a thread preempted once no longer spins for a second (an
//     iPhone 18 Pro Max at nominal, run AB-all-1: 19 such seconds on g2 in
//     7 minutes, 57 million yields).
// On by default where measured (cpu/efficiency_choice.h): all four on Apple
// (macOS and iOS); cpspin and lateguard on a Steam Deck, where gpuwait and
// sleep0 cost frames (#71); cpspin, sleep0 and lateguard
// on the Steam Machine, where gpuwait cost frames and saved no CPU
// (#71); elsewhere (the Steam Frame, other Linux, Android,
// Windows) each waits as before unless named.
// NFSMW_EFFICIENCY=0 turns them all off (each waits the old way: the "base"
// of the measurements); NFSMW_EFFICIENCY=<names> turns on only those
// ("cpspin,gpuwait,sleep0,lateguard", on any system); unset: the defaults.
// NFSMW_EFFICIENCY_AB=<names> (with
// NFSMW_CPU_LOG) turns those on and off in turn at every [cpu] line, starting
// off, so one run measures them against the old way at the same heat, clocks
// and scene; each [cpu] line names the state it covered.
#pragma once
#include <atomic>
#include <string>

namespace efficiency
{
    enum Change
    {
        kCpSpin,
        kGpuWait,
        kSleep0,
        kLateGuard,
        kChanges
    };

    namespace detail
    {
        extern std::atomic<bool> g_on[kChanges];
        void Init();
    }

    // Any thread, live (NFSMW_EFFICIENCY_AB changes it mid-run).
    inline bool On(Change c)
    {
        detail::Init();
        return detail::g_on[c].load(std::memory_order_relaxed);
    }

    // NFSMW_EFFICIENCY_AB: flips the listed changes; returns what the period
    // before ran with ("efficiency ab off: cpspin gpuwait"), or "" without it.
    std::string AlternateForAb();

    // "cpspin on, gpuwait on": the state now, for the log.
    std::string Describe();
}
