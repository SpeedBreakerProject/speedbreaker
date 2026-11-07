// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Quick Race > Custom Race: a "Cops" On/Off option in the race options
// (laps, traffic, opponents, difficulty, catch up, cops).
//
// The game already has everything but the menu row. Each race type's quick
// race settings (24 bytes at FEDatabase + 28 + 24 * type; the FEDatabase
// pointer is at 0x82A2C684, game/split_screen.h) hold, among the bytes the
// menu edits, two it never shows: +9 the heat level (1 by default) and +12
// "cops in race" (0 by default, sub_822C4268). The function that turns the
// settings into the race (cFrontendDatabase::FillCustomRace, sub_822C4FB8)
// already passes +12 to the race's attribute 0x3918E889 (cops enabled) and,
// when it is set, +9 to the race's forced heat level (GRaceCustom + 32). The
// PC release is the same code, which is how PC mods (NFSMW Extra Options'
// "Show More Race Options", NFSMW Race Fixes' heat level option) put cops in
// quick races.
//
// The options screen builds its rows per race type (UIQRTrackOptions,
// sub_82561910 switches on the race type into sub_825621A8 sprint,
// sub_82561F00 circuit and the default, sub_82562408 drag, sub_82562620 lap
// knockout, sub_825629B8 speedtrap, sub_82562C48 tollbooth), each row a
// 92-byte FEToggleWidget (malloc sub_8215CB08, constructor sub_822E46B8,
// UIWidgetMenu::AddToggleOption sub_822E23D0) with the row's vtable. One row
// is built but never shown: Track Direction (vtable 0x820A5BE0, Act
// sub_825616E8, Draw sub_825617E8), added only for a track that can be
// reversed (sub_8233A830), and none can.
//
// So the runtime:
//   - after each of the hooked setups (not drag: a drag race is steered for
//     you, lane by lane), in single player only (not split screen, not LAN
//     or online), adds one more row: a Track Direction widget whose vtable is
//     a copy of the game's (in guest heap memory), so it is told apart;
//   - for that copy only, Act toggles byte +12 on left/right (as Catch Up's
//     Act does byte +11) and Draw shows "COPS" with "ON"/"OFF" (the game's
//     own strings: language hashes 0x0013FF94, COMMON_ON, COMMON_OFF);
//   - keeps cops out of split screen, where the row isn't offered: while
//     FillCustomRace runs for a split-screen race, byte +12 reads 0.
//
// NFSMW_QUICK_RACE_COPS=0: none of it (the game as before; a race whose
// settings already say cops still gets them, as the game itself would).
#pragma once
#include <cstdint>

namespace game::quick_race_cops
{
    // Language hashes (bStringHash of the string's name).
    constexpr uint32_t kLabelCops = 0x0013FF94;   // "COPS"
    constexpr uint32_t kValueOn = 0x417B2604;     // "COMMON_ON"
    constexpr uint32_t kValueOff = 0x70DFE5C2;    // "COMMON_OFF"

    // The toggle's left and right messages (as every option row's Act).
    constexpr uint32_t kMessageLeft = 0x9120409E;
    constexpr uint32_t kMessageRight = 0xB5971BF1;

    // FEDatabase fields.
    constexpr uint32_t kDatabasePointer = 0x82A2C684;
    constexpr uint32_t kRaceTypeOffset = 24;       // word: the selected race type
    constexpr uint32_t kSettingsOffset = 28;       // quick race settings, per type
    constexpr uint32_t kSettingsStride = 24;
    constexpr uint32_t kFlagsOffset = 300;         // word
    constexpr uint32_t kSettingsCops = 12;         // byte in a type's settings
    constexpr uint32_t kSettingsHeat = 9;          // byte

    constexpr uint32_t kFlagQuickRace = 4;
    constexpr uint32_t kFlagLan = 8;
    constexpr uint32_t kFlagOnline = 0x40;

    // Where a race type's cops byte is, from the database.
    constexpr uint32_t CopsByte(uint32_t database, uint32_t raceType)
    {
        return database + kSettingsOffset + kSettingsStride * raceType + kSettingsCops;
    }

    // The options screen offers the row: single player, not LAN or online.
    constexpr bool Offered(uint8_t players, uint32_t flags)
    {
        if (flags & (kFlagLan | kFlagOnline))
            return false;
        return !(players == 2 && (flags & kFlagQuickRace));
    }

    // NFSMW_QUICK_RACE_COPS != 0 (default on).
    bool Enabled();
}
