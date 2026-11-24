// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Black Edition (GitHub #100): the content of the game's Black Edition
// release, which the retail Xbox 360 disc already holds, switched off by one
// byte of the game's own (0x82A2CE06, in .data; the USA and the European
// Spanish discs have 0 there; the German disc has not been seen). tuhana's
// Xenia patch for title 454107D9, "Black Edition content", sets it (a
// big-endian word at 0x82A2CE04 = 0x00000100: the bytes around it are 0
// already). Nothing in the game writes it (no store to it in either release's
// recompiled code).
//
// Who reads it (the USA recompiled code; the European (PAL, Spanish) disc's
// code reads the same address in the same 11 functions, relocated):
//   - sub_8226A548, sub_822C5CC0, sub_822C62D8 (returns it), sub_822E8D48,
//     sub_822EA0E8, sub_822EAA60, sub_822EAB88, sub_822EAC88,
//     sub_824EE670, sub_82568690, sub_8257A420: with it on, Quick Race and
//     the Challenge Series list more events and the bonus cars list ten
//     more (the 2026-11-09 probe's counts);
//   - the USA disc only: its online product code, "SLUS_21351" with it on
//     and "SLUS_21267" off (sub_823AF000; sub_82259408 and sub_8253C5F8,
//     the online lobby's SKU fields). The PAL disc's has one product code.
//
// Settings > Game > Black Edition ([game] black_edition in settings.toml,
// NFSMW_BLACK_EDITION for one run; off by default) sets it once, after the
// image is loaded and the release is chosen and before any guest code runs:
// a Restart option, so a change in the menu waits for the next launch and the
// game never sees the flag change under it. Off, nothing is written (the byte
// stays as the disc has it). Saves are the same bytes either way (a profile
// made, saved, or raced in a Black Edition car, on or off, checked for GitHub
// issue #100); with it off, the content is hidden again.
#pragma once
#include <cstdint>

namespace game::black_edition
{
    // The flag's USA address (region::Addr gives the running release's).
    constexpr uint32_t kFlag = 0x82A2CE06;

    // On: the flag set to 1 (returns true). Off: nothing written.
    inline bool Apply(uint8_t* flag, bool on)
    {
        if (!on)
            return false;
        *flag = 1;
        return true;
    }

    // main.cpp: after the image is loaded and region::Select, before the
    // guest thread starts.
    void ApplyAtLaunch();
}
