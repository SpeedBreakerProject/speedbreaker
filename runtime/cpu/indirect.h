// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Force-included into the recompiled code: indirect calls (bctrl, bctr to a
// function pointer) go through a checked lookup that reports a bad target as
// a guest address instead of jumping to a wild host pointer.
#pragma once
#include <cstdint>

struct PPCContext;
void PPCCallIndirect(PPCContext& ctx, uint8_t* base, uint32_t target);

#define PPC_CALL_INDIRECT_FUNC(x) PPCCallIndirect(ctx, base, (x))
