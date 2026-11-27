// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The AI's catch-up, the "rubber band". The USA disc's own code, recompiled
// and surveyed (default.xex aebdf3c1...):
//   - cFrontendDatabase::FillCustomRace (sub_822C4FB8) hands a quick race
//     type's Catch Up byte (+11) to the race as attribute 0x10DB04E6, a bool
//     (sub_823563B0), beside the cops byte (+12 -> 0x3918E889) and heat (+9 ->
//     race + 32).
//   - sub_82338A90(race) reads it back (else the zeroed default at
//     0x82C5F590); only two functions call it: sub_823328C0, the AI's speed
//     step, and sub_82105E08, which stores a skill value at +2208.
//   - In sub_823328C0 with the flag on, f31 = 0x82061C90[level], level being
//     sub_8233A6A0's bucket (attribute 0x88A7E3BE, 34 / 67) and the table
//     { 0, 0.5, 1 }: the weight the AI's speed is scaled by. With the flag off
//     that weight is computed from the AI's own curves instead (0x82332BC0),
//     and sub_82105E08 takes the 0.8 row rather than 1.0.
//
// So the weight is what to scale. 0.5 is the table's middle row, the weight
// every bucket gets when there is no spread; Settings > Game > Catch Up
// (settings::Id::CatchUp, which NFSMW_CATCHUP overrides for one run) keeps it
// and scales the deviations from it: 100% is the game bit for bit, and 50%,
// the menu's other choice, halves the help and the lift. 0, which pins every
// bucket to the middle row, was tried and felt wrong, so the menu does not
// offer it (a file or the environment may hold anything from 0.5 to 1).
// The hook is one instruction wide (0x823329D0, f31 loaded at 0x823329CC).
// sub_823328C0 also builds f31 from the AI's own curve at r31+17764 for a
// race type of 2 and merges there, so a race with the flag off can reach it
// too. The strength's range and clamping are user/settings.cpp's.
#pragma once

namespace game::catch_up
{
    // The table's middle row: the weight between no help (0) and all of it (1).
    constexpr float kNeutral = 0.5f;
    constexpr float kFull = 1.0f;

    // The game's weight with its deviation from the middle scaled: at 0 every
    // bucket is kNeutral, at 1 the game's own.
    constexpr float ScaleWeight(float weight, float strength)
    {
        return kNeutral + (weight - kNeutral) * strength;
    }

    constexpr bool Scaled(float strength)
    {
        return strength < kFull;
    }
}
