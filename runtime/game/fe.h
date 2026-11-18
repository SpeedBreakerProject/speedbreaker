// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Frontend (FE) helpers: menus are packages (.fng) driven by
// a screen object, talking in bStringHash-named messages.
#pragma once

#include <cstdint>
#include <string_view>

struct PPCContext;

namespace game::fe
{
    // bStringHash.
    constexpr uint32_t Hash(std::string_view name)
    {
        uint32_t h = 0xFFFFFFFF;
        for (char c : name)
            h = h * 33 + uint8_t(c);
        return h;
    }

    // Pad messages.
    constexpr uint32_t kPadBack = Hash("PAD_BACK");

    // A package's screen object (0 if none), and its class check by vtable
    // (the running release's address: region::Addr).
    uint32_t ScreenOf(uint32_t package);
    bool IsScreen(uint32_t screen, uint32_t vtable);

    // Pad messages the package doesn't respond to (the engine drops them).
    // Observe only; register before the game starts.
    using UnhandledButton = void (*)(uint32_t message, uint32_t package);
    void OnUnhandledButton(UnhandledButton listener);

    // Opens the game's Yes/No dialog over a screen, from a game-thread hook.
    // text: a string hash; yes/no: messages sent back to the screen (B = no).
    void ShowYesNoDialog(const PPCContext& ctx, uint8_t* base, uint32_t screen,
        uint32_t text, uint32_t yes, uint32_t no);
}
