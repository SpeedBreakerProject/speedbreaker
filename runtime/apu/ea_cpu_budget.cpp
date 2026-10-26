// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// EA SND's CPU budget on a host: never expel voices for time (see
// ea_cpu_budget.h; GitHub issue #35).
#include <stdafx.h>
#include <region/region.h>
#include "ea_cpu_budget.h"
#include <kernel/function.h>
#include <report/report.h>

extern "C" PPC_FUNC(__imp__sub_82601788);

namespace
{
    const bool s_eaExpels = [] {
        const bool on = apu::ea_budget::Expels(std::getenv("NFSMW_EA_CPU_BUDGET"));
        if (on)
            fprintf(stderr, "[audio] NFSMW_EA_CPU_BUDGET=1: EA's mixer drops sounds when a mix takes too long, as on the console\n");
        return on;
    }();

    // EA's mixer (sub_82601528 runs on its thread, one block at a time).
    struct
    {
        uint64_t times = 0, sinceLog = 0;
        double worstMs = 0, lastLog = -1e9;
    } s_skipped;
}

// sub_82601528's over-budget step: f1 = how far over, in EA's units (r3 =
// mixer + 68). EA expels the lowest-priority voices until it fits.
SB_HOOK(sub_82601788)
{
    if (s_eaExpels)
    {
        SB_ORIGINAL(sub_82601788)(ctx, base);
        return;
    }
    // Nothing is expelled. Say so, at most once a minute.
    auto& s = s_skipped;
    const double overMs = apu::ea_budget::ToMs(ctx.f1.f64);
    uint32_t bits = PPC_LOAD_U32(PPC_LOAD_U32(ctx.r3.u32) + 16);  // the config's budget, %
    float percent;
    memcpy(&percent, &bits, 4);
    s.times++;
    s.sinceLog++;
    s.worstMs = std::max(s.worstMs, overMs);
    const double now = report::SessionSeconds();
    if (now - s.lastLog >= 60.0)
    {
        fprintf(stderr, "[audio] the game's sound mixer took too long by its own clock (%.1f ms over its %.1f ms a block%s), "
            "so EA's code would have dropped sounds (the engine's, say) until the next gear change; kept them "
            "(%llu time%s so far)\n", s.worstMs, apu::ea_budget::ToMs(apu::ea_budget::Budget(percent)),
            s.sinceLog > 1 ? std::format(" at worst, {} times since the last line", s.sinceLog).c_str() : "",
            (unsigned long long)s.times, s.times == 1 ? "" : "s");
        s.lastLog = now;
        s.sinceLog = 0;
        s.worstMs = 0;
    }
}
