// SpeedBreaker Windows port: the POSIX odds and ends the runtime uses that
// mingw-w64 lacks, as stubs or thin wrappers. Force-included (-include) into
// every runtime source on Windows. Deliberately does NOT include <windows.h>:
// its macros (FILE_OPEN, DUPLICATE_CLOSE_SOURCE, RtlCompareMemory, ...)
// collide with the Xbox 360 kernel names the runtime implements.
// GPL-3.0-or-later.
#pragma once
#ifdef _WIN32
// XenonUtils/xbox.h includes <windows.h> on Windows, whose macros (FILE_OPEN,
// DUPLICATE_CLOSE_SOURCE, RtlCompareMemory, ...) collide with the kernel names
// the runtime implements. Claim mingw's include guard so it stays out;
// compat/win/win_compat.cpp, which needs the real thing, undefines it first.
#ifndef _INC_WINDOWS
#define _INC_WINDOWS 1
#define SB_FAKE_WINDOWS_GUARD 1
#endif
#ifdef __cplusplus
#include <csignal>
#include <cerrno>
#include <ctime>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#endif
#include <fcntl.h>
#include <io.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef SIGBUS
#define SIGBUS 7
#endif
#ifndef SIGHUP
#define SIGHUP 1
#endif
#ifndef SIGTRAP
#define SIGTRAP 5
#endif
#ifndef SA_SIGINFO
#define SA_SIGINFO 4
#define SA_RESTART 0x10000000
#define SA_RESETHAND 0x80000000
#endif
#ifndef SIG_BLOCK
#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2
#endif
#ifndef ESRCH
#define ESRCH 3
#endif

#ifndef O_NONBLOCK
#define O_NONBLOCK 0
#endif
#ifndef EDQUOT
#define EDQUOT 122
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif
#ifndef SIGPIPE
#define SIGPIPE 13
#endif
#ifndef SIGURG
#define SIGURG 23
#endif
#define F_SETFD 2
#define FD_CLOEXEC 1
#define F_DUPFD_CLOEXEC 1030

// 64-bit file sizes: mingw's plain struct stat has a 32-bit st_size, which
// can't describe a disc image.
#define stat _stat64
#define fstat _fstat64

#ifdef __cplusplus
#include <cstdarg>
#include <cwchar>
#include <string>
extern "C++" {
#ifndef _NTDEF_
typedef long NTSTATUS;
#endif
typedef unsigned long long sigset_t;
typedef struct siginfo_t { void* si_addr; int si_code; int si_signo; } siginfo_t;
struct sigaction
{
    union { void (*sa_handler)(int); void (*sa_sigaction)(int, siginfo_t*, void*); };
    int sa_flags;
    sigset_t sa_mask;
};
inline int sigemptyset(sigset_t* s) { *s = 0; return 0; }
inline int sigaddset(sigset_t*, int) { return 0; }
inline int sigaction(int, const struct sigaction*, struct sigaction* old)
{
    if (old) { old->sa_handler = SIG_DFL; old->sa_flags = 0; old->sa_mask = 0; }
    return 0;
}
inline int kill(int, int) { errno = ESRCH; return -1; }
inline int fsync(int fd) { return _commit(fd); }
inline int pipe(int fds[2]) { return _pipe(fds, 1 << 16, _O_BINARY); }
inline int fcntl(int fd, int cmd, ...)
{
    if (cmd == F_DUPFD_CLOEXEC) return dup(fd);
    return 0;
}
// Windows paths are wide: std::filesystem::path::c_str() is a wchar_t*.
inline int _stat64(const wchar_t* p, struct _stat64* st) { return _wstat64(p, st); }
inline int open(const wchar_t* p, int flags, int mode = 0) { return _wopen(p, flags | _O_BINARY, mode); }
inline int open(const char* p, int flags, int mode = 0) { return _open(p, flags | _O_BINARY, mode); }
inline FILE* fopen(const wchar_t* p, const char* mode)
{
    std::wstring w;
    for (const char* c = mode; *c; ++c) w.push_back(wchar_t(*c));
    return _wfopen(p, w.c_str());
}
inline int access(const wchar_t* p, int mode) { return _waccess(p, mode & 6); }
inline int mkdir(const wchar_t* p, int) { return _wmkdir(p); }
inline int unlink(const wchar_t* p) { return _wunlink(p); }
inline int rename(const wchar_t* a, const wchar_t* b) { return _wrename(a, b); }
inline int remove(const wchar_t* p) { return _wremove(p); }
ssize_t pread(int fd, void* buf, size_t count, long long offset);
ssize_t pwrite(int fd, const void* buf, size_t count, long long offset);
#define LOCK_EX 2
#define LOCK_NB 4
inline int flock(int, int) { return 0; }
struct ucontext_t { int unused; };
inline struct tm* localtime_r(const time_t* t, struct tm* out) { return localtime_s(out, t) == 0 ? out : nullptr; }
}
#endif
#endif
