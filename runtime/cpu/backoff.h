// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Short sleeps for the game's polling loops. On the 360 a hardware thread that
// spun while it waited cost nothing; here each one spins a host core. The
// game's zero delays (Sleep(0), KeDelayExecutionThread with no time) and D3D's
// GPU busy-waits back off to short sleeps once a poll has gone on for a while
// (see their callers for how long). NtYieldExecution is only counted.
//
// A sleep the host stretches far past its time (macOS coalesces the timers of
// some threads to ~10 ms: an app in the background, App Nap) would turn a poll
// into a hitch, so each sleep is timed: one that ends 2 ms or more late makes
// that thread yield again, as before, for the next second (with
// cpu/efficiency.h's kLateGuard: two late sleeps of its last 8).
//
// Linux and Apple. Windows, whose sleeps end on its 1 ms timer at the
// earliest, always yields. NFSMW_SLEEP0_BACKOFF=0 and NFSMW_GPU_WAIT_BACKOFF=0
// turn each kind off (it yields or spins as v0.1.9 did). NFSMW_BACKOFF_LOG=1
// logs a [backoff] line every 10 s: per kind the calls, the sleeps, how long
// they took, and the late ones.
#pragma once

#include <chrono>

namespace backoff
{
    enum Kind
    {
        kSleep0,   // zero delays (KeDelayExecutionThread)
        kYield,    // NtYieldExecution (counted only)
        kGpuWait,  // D3D's busy-waits (sub_825A5B20)
        kKinds
    };

    // Whether this kind backs off at all (its environment switch; false on
    // Windows).
    bool Enabled(Kind kind);

    // One poll of a zero delay or a yield: after 1 ms of back-to-back polls
    // (each less than 300 us after the last; 0.25 ms with cpu/efficiency.h's
    // kSleep0) it sleeps 100 us, otherwise it yields. A new poll starts exact.
    void ZeroDelay(Kind kind);

    // Sleeps the calling thread for `time` (or yields, for the second after a
    // late sleep). Counted under `kind`.
    void Sleep(Kind kind, std::chrono::microseconds time);

    // Count one poll of a GPU busy-wait, or one NtYieldExecution, for the log.
    void CountGpuWaitPoll();
    void CountYield();

    // A D3D GPU busy-wait's poll past its 20 us of spin. The old way (and
    // with cpu/efficiency.h's kGpuWait off): Sleep `time`. With kGpuWait (item
    // #71): wait for the command processor's next step (NoteGpuProgress: a
    // write it made visible to the game, or a batch it finished), at most
    // 1 ms, and return at once when there was one since this thread's last
    // poll, so the game looks again. One wake-up where 50 us sleeps made
    // dozens, and the wait ends when what it waits for may have come, not up
    // to 50 us after. (Windows' wait fix waits this way too.)
    void GpuWaitPoll(std::chrono::microseconds time);

    // The command processor (and the GPU's completion thread) made progress
    // a D3D GPU wait may be waiting for: a fence or other write made visible
    // to the guest, or a batch of commands finished. An atomic add, and a
    // notify only while a GPU wait waits.
    void NoteGpuProgress();
}
