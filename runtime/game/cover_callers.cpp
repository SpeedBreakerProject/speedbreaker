// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See cover_callers.h.
#include <stdafx.h>
#include <region/region.h>
#include "cover_callers.h"

#include <kernel/function.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <format>
#include <mutex>
#include <vector>

namespace game::covers
{
    namespace
    {
        constexpr uint32_t kSlotBytes = 128;       // four 32-byte ePolys
        constexpr uint32_t kEntries = 1u << 16;

        struct Entry
        {
            std::atomic<uint64_t> physical{ 0 };
            uint32_t lr[8];
            uint32_t n;
        };
        Entry s_entries[kEntries];

        uint32_t Index(uint64_t key)
        {
            return uint32_t(key ^ (key >> 29)) & (kEntries - 1);
        }

    }

    std::atomic<uint64_t> s_noted{ 0 };

    uint64_t Noted()
    {
        return s_noted.load(std::memory_order_relaxed);
    }

    void Note(uint64_t physical, const Chain& c)
    {
        s_noted.fetch_add(1, std::memory_order_relaxed);
        {
            Entry& e = s_entries[Index(physical)];
            e.physical.store(0, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_release);
            std::copy(c.lr, c.lr + 8, e.lr);
            e.n = c.n;
            e.physical.store(physical, std::memory_order_release);
        }
    }

    uint64_t QuadKey(const uint32_t words[16])
    {
        uint64_t h = 1469598103934665603ull;  // FNV-1a over the words
        for (uint32_t i = 0; i < 16; i++)
        {
            h ^= words[i];
            h *= 1099511628211ull;
        }
        return h | 1;  // never 0 (an empty entry)
    }

    bool Enabled()
    {
        static const bool on = [] { const char* v = std::getenv("NFSMW_LOG_COVERS"); return v && v[0] == '1'; }();
        return on;
    }

    bool CallerOf(uint64_t physical, Chain& out)
    {
        const Entry& e = s_entries[Index(physical)];
        if (e.physical.load(std::memory_order_acquire) != physical)
            return false;
        Chain c;
        std::copy(e.lr, e.lr + 8, c.lr);
        c.n = std::min<uint32_t>(e.n, 8);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (e.physical.load(std::memory_order_relaxed) != physical)
            return false;
        out = c;
        return true;
    }

    std::string Describe(const Chain& chain)
    {
        // The recompiled functions' guest addresses, sorted (once).
        static const std::vector<uint32_t> starts = [] {
            std::vector<uint32_t> v;
            const PPCFuncMapping* const mappings = region::FuncMappings();
            for (size_t i = 0; mappings[i].guest != 0; i++)
                v.push_back(mappings[i].guest);
            std::sort(v.begin(), v.end());
            return v;
        }();
        std::string s;
        for (uint32_t i = 0; i < chain.n; i++)
        {
            const uint32_t lr = chain.lr[i];
            auto it = std::upper_bound(starts.begin(), starts.end(), lr - 4);
            if (!s.empty())
                s += " <- ";
            if (it == starts.begin())
                s += std::format("{:08X}", lr);
            else
                s += std::format("sub_{:08X}+0x{:X}", *(it - 1), lr - *(it - 1));
        }
        return s.empty() ? "unknown" : s;
    }
}

namespace
{
    // The chain above the caller: its return address (ctx.lr), then up the
    // stack's back chain (each function here saves its LR at its entry SP - 8).
    game::covers::Chain CallChain(PPCContext& ctx, uint8_t* base)
    {
        game::covers::Chain c;
        c.lr[c.n++] = uint32_t(ctx.lr);
        uint32_t sp = ctx.r1.u32;
        for (int k = 0; k < 7; k++)
        {
            const uint32_t up = PPC_LOAD_U32(sp);
            if (up <= sp || up - sp > 0x10000u)
                break;
            const uint32_t lr = PPC_LOAD_U32(up - 8);
            if (lr < 0x82000000u || lr >= 0x82D00000u)
                break;
            c.lr[c.n++] = lr;
            sp = up;
        }
        return c;
    }
}

extern "C" PPC_FUNC(__imp__sub_824457B0);

// The ePoly vertex writer (cover_callers.h): with NFSMW_LOG_COVERS=1, the
// quad it wrote (at r3) and the return addresses above the call.
SB_HOOK(sub_824457B0)
{
    if (!game::covers::Enabled() || !ctx.r3.u32)
    {
        SB_ORIGINAL(sub_824457B0)(ctx, base);
        return;
    }
    const uint32_t dest = ctx.r3.u32;
    const game::covers::Chain chain = CallChain(ctx, base);
    SB_ORIGINAL(sub_824457B0)(ctx, base);
    uint32_t words[16];
    for (uint32_t v = 0; v < 4; v++)
        for (uint32_t j = 0; j < 4; j++)
            words[v * 4 + j] = PPC_LOAD_U32(dest + v * 32 + j * 4);
    game::covers::Note(game::covers::QuadKey(words), chain);
}
