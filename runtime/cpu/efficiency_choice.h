// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Which of cpu/efficiency.h's changes a run starts with: NFSMW_EFFICIENCY when
// it is set ("0" none, "1"/"all" all, else the names listed), otherwise the
// defaults of the machine the game runs on. Header only and free of the
// environment, so tests/efficiency_choice_test.cpp checks it alone;
// cpu/efficiency.cpp hands it the variable and the machine.
//   - Apple (macOS, iOS): all four (item #71, the iPhone and the Mac).
//   - A Steam Deck (Linux, video/steam_device.h's DMI names): cpspin and
//     lateguard. gpuwait and sleep0 stay off there: on the Deck gpuwait put
//     ~0.9 ms a frame of the woken waits' work on the command processor in
//     the heaviest city scenes, and sleep0 made a one-frame main-menu hitch
//     come in 7 of 9 runs against 12 of 34 (#71).
//   - The Steam Machine (Linux, DMI "Valve" / "Fremont"): cpspin, sleep0 and
//     lateguard. gpuwait stays off there too: it saved no CPU on that machine
//     and put 0.13 ms a frame on the command processor, which limits the
//     heaviest city scenes (#71).
//   - Anything else (the Steam Frame, other Linux, Android, Windows): none,
//     the v0.2.5 waits, until measured there.
#pragma once
#include <string_view>

namespace efficiency
{
    // The changes, in cpu/efficiency.h's Change order.
    inline constexpr int kChoiceCount = 4;
    inline constexpr const char* kChoiceNames[kChoiceCount] = { "cpspin", "gpuwait", "sleep0", "lateguard" };

    enum class Machine
    {
        Apple,
        SteamDeck,
        SteamMachine,
        Other,
    };

    // "cpspin,gpuwait" -> which are named; "0" none, "1"/"all" all.
    inline void ParseNames(std::string_view v, bool out[kChoiceCount])
    {
        const bool all = v == "1" || v == "all";
        for (int c = 0; c < kChoiceCount; c++)
        {
            out[c] = all;
            for (size_t pos = 0; !all && pos <= v.size();)
            {
                size_t comma = v.find(',', pos);
                std::string_view item = v.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos);
                if (item == kChoiceNames[c])
                    out[c] = true;
                if (comma == std::string_view::npos)
                    break;
                pos = comma + 1;
            }
        }
    }

    // The machine's own defaults (NFSMW_EFFICIENCY unset).
    inline void Defaults(Machine m, bool out[kChoiceCount])
    {
        for (int c = 0; c < kChoiceCount; c++)
        {
            const std::string_view name = kChoiceNames[c];
            out[c] = m == Machine::Apple || (m == Machine::SteamDeck && (name == "cpspin" || name == "lateguard")) ||
                     (m == Machine::SteamMachine && (name == "cpspin" || name == "sleep0" || name == "lateguard"));
        }
    }

    // What a run starts with: `setting` is NFSMW_EFFICIENCY (nullptr when unset).
    inline void Choose(const char* setting, Machine m, bool out[kChoiceCount])
    {
        if (setting)
            ParseNames(setting, out);
        else
            Defaults(m, out);
    }
}
