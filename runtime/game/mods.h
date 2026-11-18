// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Settings > Mods (game/mods.cpp): what the rest of the game code calls.
#pragma once

#include <cstdint>

struct PPCContext;

namespace mods
{
    // Controls and loading screens, once they're built (r3 =
    // screen): hides the "Xbox Guide" text under the controller
    // picture for non-360 prompt sets. Called from the Modern
    // Gamepad Layouts hooks for those screens (fixes.cpp).
#ifndef SB_NO_MODS
    void HideGuideText(PPCContext& ctx, uint8_t* base, uint32_t screen);
#else
    // Not built with SB_NO_MODS (mods::kAvailable).
    inline void HideGuideText(PPCContext&, uint8_t*, uint32_t) {}
#endif
}
