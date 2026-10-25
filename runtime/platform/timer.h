// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Timing the operating system decides. Windows schedules sleeps on a timer
// that ticks every 15.6 ms unless a process asks for more: there Sleep(1),
// a 100 us sleep_for and a 5.33 ms sleep_until all took about 16 ms (the
// P0 probe, laptop.md item d), and after timeBeginPeriod(1) about 2 ms.
// macOS and Linux sleep to the microsecond as it is: nothing to do there.
// (Sub-millisecond waits on Windows, a high-resolution waitable timer plus a
// short spin for the vblank, audio and tick cadences, come in PLAN.md P4.)
#pragma once
#include <chrono>
#include <cstdint>
#include <thread>

namespace platform
{
    // Once, first thing in main: the finest timer resolution (Windows: 1 ms,
    // for the life of the process). Elsewhere nothing.
    void RaiseTimerResolution();

    // Sleeps until `steadyNs` (std::chrono::steady_clock's time since its
    // epoch, in ns), never returning before it: the presenter's hold of a new
    // frame for its refresh (video/vblank_lock.h's PresentAt). Windows: a
    // high-resolution waitable timer (Windows 10 1803 and later; a 1 ms
    // sleep_until, about 1.3 ms late there, where it can't be made), which
    // wakes within a few tenths of a millisecond, with the rest of the way
    // yielded; at 240 Hz a refresh is 4.2 ms. Also the audio thread's 5.33 ms
    // ticks (apu/audio.cpp). Elsewhere sleep_until.
    void SleepUntil(int64_t steadyNs);

    // For the audio thread (apu/audio.cpp), on it: Windows: the thread joins
    // MMCSS's "Pro Audio" task (else "Audio", else the highest normal
    // priority), as WASAPI's own threads do, so a busy core doesn't hold its
    // 5.33 ms ticks back. Returns what it got, for the log. Elsewhere nothing
    // (Apple sets its QoS class in audio.cpp) and nullptr.
    const char* RaiseAudioThreadPriority();

#ifndef _WIN32
    inline const char* RaiseAudioThreadPriority()
    {
        return nullptr;
    }

    inline void RaiseTimerResolution()
    {
    }

    inline void SleepUntil(int64_t steadyNs)
    {
        std::this_thread::sleep_until(std::chrono::steady_clock::time_point(std::chrono::nanoseconds(steadyNs)));
    }
#endif
}
