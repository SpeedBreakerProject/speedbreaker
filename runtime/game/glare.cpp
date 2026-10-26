// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See glare.h.
//
// Two function overrides (the generated functions are weak; no ppc/
// regeneration):
//   - sub_82225730, the sun code (projects the sun, counts its occlusion,
//     draws its glow): keeps its view argument for the glow. r4 is the view's
//     entry in the per-view table (0x82C3F590 + id * 384; logged: view 1 at
//     0x82C3F710, view 4 at 0x82C3FB90), its id at +0, which the code itself
//     tests for the reflection views 4 and 5.
//   - sub_822251C8, the quad builder: after the game builds the glow's quad
//     (called from the sun code, ctx.lr = glare::kGlowCall) in a view the
//     projection hook widens, its corners are fitted to the view scales.
// NFSMW_GLARE_FIT=0 leaves the glow as the game builds it (an A/B switch).
#include <stdafx.h>
#include <region/region.h>
#include "glare.h"
#include "ultrawide.h"

#include <kernel/function.h>

extern "C" PPC_FUNC(__imp__sub_82225730);
extern "C" PPC_FUNC(__imp__sub_822251C8);

namespace
{
    const bool s_fit = [] {
        const char* v = std::getenv("NFSMW_GLARE_FIT");
        const bool on = !(v && v[0] == '0');
        if (!on)
            fprintf(stderr, "[glare] NFSMW_GLARE_FIT=0: the sun's glow is drawn as the game builds it (stretched off 16:9)\n");
        return on;
    }();

    // The view the sun code is drawing for (its r4), on the thread running
    // it; 0 outside it.
    thread_local uint32_t t_sunView = 0;

    float LoadFloat(uint8_t* base, uint32_t address)
    {
        const uint32_t bits = PPC_LOAD_U32(address);
        float f;
        memcpy(&f, &bits, sizeof(f));
        return f;
    }

    void StoreFloat(uint8_t* base, uint32_t address, float f)
    {
        uint32_t bits;
        memcpy(&bits, &f, sizeof(bits));
        PPC_STORE_U32(address, bits);
    }
}

SB_HOOK(sub_82225730)
{
    const uint32_t outer = t_sunView;
    t_sunView = ctx.r4.u32;
    SB_ORIGINAL(sub_82225730)(ctx, base);
    t_sunView = outer;
}

SB_HOOK(sub_822251C8)
{
    const uint32_t lr = static_cast<uint32_t>(ctx.lr), verts = ctx.r5.u32;
    SB_ORIGINAL(sub_822251C8)(ctx, base);
    if (lr != region::Addr(game::glare::kGlowCall) || !t_sunView)
        return;
    const float sx = game::AspectScaleX(), sy = game::AspectScaleY();
    const uint32_t id = PPC_LOAD_U32(t_sunView);
    const bool fit = s_fit && (sx != 1.0f || sy != 1.0f) && game::glare::WidenedViewId(id);
    // Each view's first glow (and again when the scales change), in every
    // log: the view, its corners as built, and what was done.
    {
        static std::mutex mutex;
        static std::unordered_map<uint32_t, uint64_t> logged;
        uint32_t sxBits, syBits;
        memcpy(&sxBits, &sx, 4);
        memcpy(&syBits, &sy, 4);
        const uint64_t key = (uint64_t(sxBits) << 32) ^ syBits ^ (fit ? 1ull << 63 : 0);
        std::lock_guard lock(mutex);
        if (logged.size() < 64)
        {
            auto [it, added] = logged.try_emplace(id, ~key);
            if (it->second != key)
            {
                it->second = key;
                fprintf(stderr, "[glare] sun glow in view %u: corners (%.1f,%.1f) (%.1f,%.1f) (%.1f,%.1f) (%.1f,%.1f); view scale x %.4f y %.4f: %s\n",
                    id, LoadFloat(base, verts), LoadFloat(base, verts + 4), LoadFloat(base, verts + 16),
                    LoadFloat(base, verts + 20), LoadFloat(base, verts + 32), LoadFloat(base, verts + 36), LoadFloat(base, verts + 48),
                    LoadFloat(base, verts + 52), sx, sy,
                    fit ? "fitted to the view scale" : !s_fit ? "left as built (NFSMW_GLARE_FIT=0)"
                        : (sx == 1.0f && sy == 1.0f) ? "left as built (16:9)" : "left as built (a view the projection hook doesn't widen)");
            }
        }
    }
    if (!fit)
        return;
    // The corners: x, y at +0 and +4 of four 16-byte vertices.
    float x[4], y[4];
    for (uint32_t i = 0; i < 4; i++)
    {
        x[i] = LoadFloat(base, verts + i * 16);
        y[i] = LoadFloat(base, verts + i * 16 + 4);
    }
    game::glare::FitSprite(x, y, sx, sy);
    for (uint32_t i = 0; i < 4; i++)
    {
        StoreFloat(base, verts + i * 16, x[i]);
        StoreFloat(base, verts + i * 16 + 4, y[i]);
    }
}
