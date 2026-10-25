// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Windows only (platform/os.h): <windows.h>, the C runtime's POSIX-named
// headers (<io.h>: read, write, close, dup, dup2, isatty, fileno; <process.h>:
// getpid), and the few POSIX calls the shared code makes that the C runtime
// lacks, each a thin wrapper (platform/win32/os.cpp). Descriptors are the C
// runtime's; platform/file_io.h opens files (binary, shared, wide paths).
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>

using ssize_t = ptrdiff_t;
using pid_t = int;

// open() flags. O_CLOEXEC: not inherited by child processes.
#ifndef O_CLOEXEC
#define O_CLOEXEC _O_NOINHERIT
#endif

// No FIFOs in Windows' file systems, so nothing for O_NONBLOCK to guard.
#ifndef O_NONBLOCK
#define O_NONBLOCK 0
#endif

// Quotas end a write as a full disk does.
#ifndef EDQUOT
#define EDQUOT ENOSPC
#endif

#ifndef STDIN_FILENO
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#endif

#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)
#define S_ISREG(mode) (((mode) & _S_IFMT) == _S_IFREG)
#endif

// Page protection: mprotect's values, applied with VirtualProtect. The
// result is checked: a failure (a range that crosses a mapped view: error
// 487) is logged, rate-limited, and returns -1 with errno EACCES.
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
int mprotect(void* address, size_t length, int prot);

// sysconf(_SC_PAGESIZE): the page size (4096 on x64).
#define _SC_PAGESIZE 30
long sysconf(int name);

// nanosleep: Sleep() for whole milliseconds; shorter waits spin with yields
// (Sleep(0)), since even after timeBeginPeriod(1) a 100 us sleep takes
// about 2 ms here (laptop.md, item d). Never interrupted: 0.
int nanosleep(const struct timespec* request, struct timespec* remaining);

// clock_gettime: CLOCK_MONOTONIC (QueryPerformanceCounter), CLOCK_REALTIME
// (the system time, 100 ns), CLOCK_THREAD_CPUTIME_ID (the thread's user and
// kernel time, 100 ns units).
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#define CLOCK_THREAD_CPUTIME_ID 3
int clock_gettime(int clock, struct timespec* time);

inline struct tm* localtime_r(const time_t* time, struct tm* out)
{
    return localtime_s(out, time) == 0 ? out : nullptr;
}

inline struct tm* gmtime_r(const time_t* time, struct tm* out)
{
    return gmtime_s(out, time) == 0 ? out : nullptr;
}

inline int strcasecmp(const char* a, const char* b)
{
    return _stricmp(a, b);
}

inline int strncasecmp(const char* a, const char* b, size_t n)
{
    return _strnicmp(a, b, n);
}

inline int setenv(const char* name, const char* value, int overwrite)
{
    if (!overwrite && std::getenv(name))
        return 0;
    return _putenv_s(name, value) == 0 ? 0 : -1;
}

// FlushFileBuffers on the descriptor's handle.
inline int fsync(int fd)
{
    return _commit(fd);
}
