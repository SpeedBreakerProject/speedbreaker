// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The operating system's own headers, chosen in one place (stdafx.h). On
// macOS, iOS and Linux they are the POSIX ones the runtime always used. On
// Windows, platform/win32/posix.h brings <windows.h> and the handful of
// POSIX names the shared code calls (mprotect, sysconf, nanosleep, ...),
// each a thin wrapper over the Windows call; anything with real semantics
// of its own (files, threads, timers, faults, folders) is in the platform
// layer's headers next to this one.
#pragma once

#ifdef _WIN32
#include <platform/win32/posix.h>
#else
#include <sys/mman.h>
#endif
