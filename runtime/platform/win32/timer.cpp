// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See platform/timer.h.
#include <platform/timer.h>
#include <platform/os.h>

#include <timeapi.h>

#include <chrono>
#include <cstdio>
#include <thread>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace platform
{
    namespace
    {
        int64_t SteadyNs()
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }
    }

    void SleepUntil(int64_t steadyNs)
    {
        // One timer per thread that waits (the main thread's holds and the
        // audio thread's ticks), made at its first wait; INVALID: the system can't make one.
        thread_local HANDLE timer = [] {
            HANDLE h = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            if (!h)
                fprintf(stderr, "[timer] no high-resolution waitable timer (error %lu): holds sleep to the 1 ms tick\n", GetLastError());
            return h ? h : INVALID_HANDLE_VALUE;
        }();
        constexpr int64_t kYieldNs = 50'000;  // the last stretch, yielded (the timer may wake a little early on the QPC clock)
        for (int64_t left = steadyNs - SteadyNs(); left > 0; left = steadyNs - SteadyNs())
        {
            if (left <= kYieldNs)
                std::this_thread::yield();
            else if (timer == INVALID_HANDLE_VALUE)
                std::this_thread::sleep_until(std::chrono::steady_clock::time_point(std::chrono::nanoseconds(steadyNs)));
            else
            {
                LARGE_INTEGER due;
                due.QuadPart = -((left - kYieldNs / 2) / 100);  // relative, in 100 ns units
                if (!SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0) ||
                    WaitForSingleObject(timer, INFINITE) != WAIT_OBJECT_0)
                    std::this_thread::sleep_until(std::chrono::steady_clock::time_point(std::chrono::nanoseconds(steadyNs)));
            }
        }
    }

    const char* RaiseAudioThreadPriority()
    {
        // avrt.dll loaded, not linked (the exe needn't import it).
        if (HMODULE avrt = LoadLibraryW(L"avrt.dll"))
        {
            using SetFn = HANDLE(WINAPI*)(LPCWSTR, LPDWORD);
            if (auto set = reinterpret_cast<SetFn>(reinterpret_cast<void*>(GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW"))))
            {
                DWORD task = 0;
                if (set(L"Pro Audio", &task))
                    return "MMCSS Pro Audio";
                task = 0;
                if (set(L"Audio", &task))
                    return "MMCSS Audio";
            }
        }
        if (SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST))
            return "thread priority highest (no MMCSS)";
        return "normal thread priority (no MMCSS)";
    }

    void RaiseTimerResolution()
    {
        // Honoured even in a windowless process over SSH (the probe measured
        // the global resolution at 1.0 ms afterwards).
        if (timeBeginPeriod(1) != TIMERR_NOERROR)
            fprintf(stderr, "[timer] timeBeginPeriod(1) failed: sleeps are rounded to the 15.6 ms tick\n");
    }
}
