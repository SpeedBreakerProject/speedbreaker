// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Force-included into the recompiled code on Windows only (after indirect.h
// and guest_time.h; recomp/CMakeLists.txt). The game's setjmp and longjmp
// become the host's, with the host jmp_buf inside the guest's (XenonRecomp:
// setjmp(*reinterpret_cast<jmp_buf*>(base + r3))). The CRT's _setjmp on x64
// stores XMM6-15 with aligned moves, so a buffer that is only 8-byte aligned
// crashes in _setjmp itself (the P0 probe, laptop.md item f). Four of the
// game's seven setjmp sites take buffers of unknown alignment
// (ppc_recomp.184.cpp, ppc_recomp.186.cpp), so both calls use the guest
// buffer's first 16-byte boundary instead: setjmp and longjmp get the same
// guest pointer, so they agree. It fits, as the guest's own setjmp
// (sub_826BF180) fills 316 bytes or more and the host's jmp_buf is 256:
// 15 + 256 <= 316. __builtin_setjmp works at any alignment but doesn't keep
// XMM6-15, which Win64 code expects to survive the call.
#pragma once

// Everything the generated code includes comes first, so the macros below
// reach only the generated code itself.
#include <ppc_context.h>

#include <csetjmp>
#include <cstdint>

namespace sb_win32
{
    inline void* AlignJmpBuf(void* guestBuffer)
    {
        return reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(guestBuffer) + 15) & ~uintptr_t(15));
    }
}

static_assert(sizeof(jmp_buf) + 15 <= 316, "the host jmp_buf must fit in the guest's after alignment");

#undef setjmp
#define setjmp(buffer) _setjmp(*reinterpret_cast<jmp_buf*>(::sb_win32::AlignJmpBuf(&(buffer))))
#define longjmp(buffer, value) ::longjmp(*reinterpret_cast<jmp_buf*>(::sb_win32::AlignJmpBuf(&(buffer))), (value))
