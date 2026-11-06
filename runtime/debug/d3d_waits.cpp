// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The game's Direct3D GPU waits. sub_825A5B20 is D3D's GPU hang check,
// called once per poll by each of its eight busy-wait loops (fence, ring
// space, idle: 0x82596364, 0x82596908, 0x82597208, 0x82597534, 0x825A995C,
// 0x825AA0E8, 0x825AAF08, 0x825AB234). It is overridden here to back those
// waits off to short sleeps, to park them while the game is suspended, and
// to report, with NFSMW_LOG_D3D_WAIT=1, which counter they poll and how often.
#include <stdafx.h>
#include <region/region.h>
#include <kernel/function.h>
#include <cpu/guest_time.h>
#include <cpu/backoff.h>
#include "timeline.h"
#include "d3d_waits.h"

//DEBUGGING
#include <debug/wake_stats.h>

extern "C" PPC_FUNC(__imp__sub_825A5B20);

namespace d3dwaits
{
    BinarySemaphore waiting_semaphore;

    void Block()
    {
        SB_WAKE("D3DWait blocking");
        waiting_semaphore.acquire_with_timeout(std::chrono::milliseconds(20)); 
    }
}

SB_HOOK(sub_825A5B20)
{
    // While the game is suspended the GPU work these loops wait for is held
    // back (Metal refuses it), and no guest time passes for the hang check:
    // sleep through it instead of spinning a core.
    guesttime::WaitWhileSuspended();
    if (timeline::g_enabled && !timeline::t_waitTargetMarked)
    {
        timeline::t_waitTargetMarked = true;
        uint32_t device = PPC_LOAD_U32(ctx.r3.u32 + 0);
        timeline::Mark(timeline::GameWaitTarget, PPC_LOAD_U32(device + 10396) - PPC_LOAD_U32(ctx.r3.u32 + 8));
    }
    static const bool log = std::getenv("NFSMW_LOG_D3D_WAIT") != nullptr;
    if (log)
    {
        static uint64_t polls = 0;
        static auto last = std::chrono::steady_clock::now();
        polls++;
        auto now = std::chrono::steady_clock::now();
        if (now - last > std::chrono::seconds(1))
        {
            uint32_t args = ctx.r3.u32;
            uint32_t device = PPC_LOAD_U32(args + 0);
            uint32_t counterPtr = PPC_LOAD_U32(device + 10384);
            fprintf(stderr, "[d3dwait] %llu polls/s | counter at %08X = %08X, issued %08X, target %08X\n",
                (unsigned long long)polls, counterPtr, PPC_LOAD_U32(counterPtr), PPC_LOAD_U32(device + 10396),
                PPC_LOAD_U32(args + 8));
            polls = 0;
            last = now;
        }
    }
    SB_WAKE("D3DWait called");
    if (std::getenv("NFSMW_SYNCHRONIZE_D3D_WAITS") != nullptr){
        uint32_t device = PPC_LOAD_U32(ctx.r3.u32);
        d3dwaits::fence_page.store(PPC_LOAD_U32(device + 10384) & 0x0FFFFF00, std::memory_order_relaxed);
        d3dwaits::Block();
    }
    SB_ORIGINAL(sub_825A5B20)(ctx, base);
}
