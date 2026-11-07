// SpeedBreaker tests. GPL-3.0-or-later (see COPYING).
//
// Quick Race's Cops row (game/quick_race_cops.h, header part only): where a
// race type's cops byte is, next to the bytes the game's own rows edit, and
// when the options screen offers the row.
//
// Build by hand: c++ -std=c++20 -O2 -I runtime tests/quick_race_cops_test.cpp
#include <game/quick_race_cops.h>

#include <cstdint>
#include <cstdio>

using namespace game::quick_race_cops;

namespace
{
    int s_failures = 0;

#define CHECK(cond, ...)                                              \
    do                                                                \
    {                                                                 \
        if (!(cond))                                                  \
        {                                                             \
            s_failures++;                                             \
            fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
            fprintf(stderr, __VA_ARGS__);                             \
            fprintf(stderr, "\n");                                    \
        }                                                             \
    } while (0)

    constexpr uint32_t kDb = 0x40001000;  // any FEDatabase address

    // The game's rows address a race type's settings as the database plus
    // 24 x type, then a fixed offset: Track Direction's Act and Draw
    // (sub_825616E8, sub_825617E8) byte 33 (+5), Catch Up's (sub_82561070,
    // sub_82561118) byte 39 (+11), FillCustomRace (sub_822C4FB8) +12 for
    // cops and +9 for the heat level. Cops is the byte after Catch Up's.
    void CopsByteBesideTheGamesRows()
    {
        for (uint32_t type = 0; type < 11; type++)
        {
            const uint32_t settings = kDb + 28 + 24 * type;
            const uint32_t cops = CopsByte(kDb, type);
            CHECK(cops == settings + 12, "type %u: %#x, settings at %#x", type, cops, settings);
            CHECK(cops - 1 == kDb + 39 + 24 * type, "type %u: not right after Catch Up's byte", type);
            CHECK(cops - kSettingsCops + 5 == kDb + 33 + 24 * type, "type %u: Track Direction's byte", type);
            CHECK(cops - kSettingsCops + kSettingsHeat == settings + 9, "type %u: heat byte", type);
        }
        // Circuit (type 1) and the database's own layout.
        CHECK(CopsByte(kDb, 1) == kDb + 64, "circuit: %#x", CopsByte(kDb, 1));
        static_assert(CopsByte(0, 0) == 40);
    }

    // Offered: single player only. The game's split-screen test is two
    // players in a quick race (game/split_screen.h); LAN and online games
    // build their own rows (sub_82561C40).
    void OfferedInSinglePlayerOnly()
    {
        CHECK(Offered(1, kFlagQuickRace), "single player quick race");
        CHECK(Offered(1, 0), "single player, no flags");
        CHECK(!Offered(2, kFlagQuickRace), "split screen");
        CHECK(!Offered(2, kFlagQuickRace | 0x100), "split screen, other flags");
        CHECK(Offered(2, 0), "two players outside a quick race isn't split screen");
        CHECK(!Offered(1, kFlagLan), "LAN");
        CHECK(!Offered(1, kFlagOnline), "online");
        CHECK(!Offered(1, kFlagQuickRace | kFlagLan), "LAN quick race");
        CHECK(!Offered(2, kFlagQuickRace | kFlagOnline), "online, two players");
        // The bits the game tests (rlwinm ...,29,29 / 28,28 / 25,25).
        static_assert(kFlagQuickRace == 1u << (31 - 29));
        static_assert(kFlagLan == 1u << (31 - 28));
        static_assert(kFlagOnline == 1u << (31 - 25));
    }
}

int main()
{
    CopsByteBesideTheGamesRows();
    OfferedInSinglePlayerOnly();
    if (s_failures)
    {
        fprintf(stderr, "%d failure(s)\n", s_failures);
        return 1;
    }
    printf("quick_race_cops_test: all passed\n");
    return 0;
}
