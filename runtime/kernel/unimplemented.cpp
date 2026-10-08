// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
#include "../stdafx.h"
#include "unimplemented.h"
#include <report/report.h>

#include <cstdlib>

void UnimplementedImport(const char* name, PPCContext& ctx, uint8_t* base)
{
    static const bool s_continue = [] {
        const char* v = std::getenv("NFSMW_STUB_CONTINUE");
        return v != nullptr && v[0] == '1';
    }();

    fprintf(stderr,
        "[import] UNIMPLEMENTED %s  lr=%08X  r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X\n",
        name, uint32_t(ctx.lr), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32);

    if (!s_continue)
    {
        fflush(stderr);
        report::FlushLog();  // the line above, into the session log, before _Exit
        std::_Exit(3);
    }

    ctx.r3.u64 = 0;
}
