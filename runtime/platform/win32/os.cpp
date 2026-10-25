// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
// The POSIX calls platform/win32/posix.h declares, on Windows.
#include <platform/os.h>

#include <algorithm>
#include <atomic>
#include <cstdio>

int mprotect(void* address, size_t length, int prot)
{
    DWORD protect = (prot & PROT_WRITE) ? PAGE_READWRITE : (prot & PROT_READ) ? PAGE_READONLY : PAGE_NOACCESS;
    DWORD old;
    if (VirtualProtect(address, length, protect, &old))
        return 0;
    // The write watch protects each mirror separately because a range that
    // crosses into another view fails (487, ERROR_INVALID_ADDRESS): a
    // failure here means a page stays as it was, so say so (a few times).
    static std::atomic<int> logged{ 0 };
    if (logged.fetch_add(1, std::memory_order_relaxed) < 20)
        fprintf(stderr, "[memory] VirtualProtect(%p, 0x%zX, %d) failed: error %lu\n", address, length, prot, GetLastError());
    errno = EACCES;
    return -1;
}

long sysconf(int name)
{
    if (name != _SC_PAGESIZE)
    {
        errno = EINVAL;
        return -1;
    }
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return long(info.dwPageSize);
}

namespace
{
    int64_t QpcFrequency()
    {
        static const int64_t frequency = [] {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            return int64_t(f.QuadPart);
        }();
        return frequency;
    }

    int64_t NowNs()
    {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        int64_t f = QpcFrequency();
        return int64_t(t.QuadPart / f) * 1000000000 + int64_t(t.QuadPart % f) * 1000000000 / f;
    }
}

int nanosleep(const struct timespec* request, struct timespec* remaining)
{
    if (!request || request->tv_nsec < 0 || request->tv_nsec >= 1000000000 || request->tv_sec < 0)
    {
        errno = EINVAL;
        return -1;
    }
    int64_t ns = int64_t(request->tv_sec) * 1000000000 + request->tv_nsec;
    int64_t deadline = NowNs() + ns;
    if (ns >= 1000000)
        Sleep(DWORD(std::min<int64_t>(ns / 1000000, INFINITE - 1)));
    while (NowNs() < deadline)
        Sleep(0);  // gives the core to a ready thread, if any
    if (remaining)
        *remaining = {};
    return 0;
}

int clock_gettime(int clock, struct timespec* time)
{
    int64_t ns;
    if (clock == CLOCK_MONOTONIC)
        ns = NowNs();
    else if (clock == CLOCK_REALTIME)
    {
        FILETIME ft;
        GetSystemTimePreciseAsFileTime(&ft);
        constexpr int64_t kEpochDelta = 116444736000000000;  // 1601 to 1970 in 100 ns
        ns = ((int64_t(ft.dwHighDateTime) << 32 | ft.dwLowDateTime) - kEpochDelta) * 100;
    }
    else if (clock == CLOCK_THREAD_CPUTIME_ID)
    {
        FILETIME created, exited, kernel, user;
        if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
        {
            errno = EINVAL;
            return -1;
        }
        ns = ((int64_t(kernel.dwHighDateTime) << 32 | kernel.dwLowDateTime) + (int64_t(user.dwHighDateTime) << 32 | user.dwLowDateTime)) * 100;
    }
    else
    {
        errno = EINVAL;
        return -1;
    }
    time->tv_sec = time_t(ns / 1000000000);
    time->tv_nsec = long(ns % 1000000000);
    return 0;
}
