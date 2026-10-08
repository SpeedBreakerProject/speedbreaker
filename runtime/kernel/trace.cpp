// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
#include <stdafx.h>
#include "function.h"
#include <cpu/guest_thread.h>

bool ImportTraceEnabled()
{
    static const bool enabled = [] {
        const char* v = std::getenv("NFSMW_TRACE");
        return v != nullptr && v[0] == '1';
    }();
    return enabled;
}

void ImportTraceCall(const char* name, const PPCContext& ctx)
{
    fprintf(stderr, "[trace t%u] %s(%08X, %08X, %08X, %08X) lr=%08X\n", GuestThread::GetCurrentThreadId(), name,
        ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, uint32_t(ctx.lr));
}

void ImportTraceReturn(const char* name, const PPCContext& ctx)
{
    fprintf(stderr, "[trace t%u]   -> %08X\n", GuestThread::GetCurrentThreadId(), ctx.r3.u32);
}
