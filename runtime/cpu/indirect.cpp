// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
#include <stdafx.h>
#include "indirect.h"

void PPCCallIndirect(PPCContext& ctx, uint8_t* base, uint32_t target)
{
    PPCFunc* fn = nullptr;
    if (target >= PPC_CODE_BASE && target < PPC_CODE_BASE + PPC_CODE_SIZE && (target & 3) == 0)
        fn = PPC_LOOKUP_FUNC(base, target);
    if (fn == nullptr)
    {
        fprintf(stderr, "[cpu] indirect call to %08X, which is not a recompiled function (lr=%08X r3=%08X r4=%08X)\n",
            target, uint32_t(ctx.lr), ctx.r3.u32, ctx.r4.u32);
        fflush(stderr);
        std::abort();
    }
    fn(ctx, base);
}
