// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See efficiency.h.
#include "efficiency.h"
#include "efficiency_choice.h"

#include <video/steam_device.h>

#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace efficiency
{
    namespace
    {
        static_assert(kChanges == kChoiceCount, "efficiency_choice.h lists the changes in Change order");
        const char* const* const kNames = kChoiceNames;

        // "cpspin,gpuwait" -> which are named; "0" none, "1"/"all" all.
        void Parse(const char* list, bool out[kChanges])
        {
            ParseNames(list, out);
        }

        // The machine whose defaults apply (efficiency_choice.h). On Linux a
        // Steam Deck by its DMI names (video/steam_device.h: read from
        // /sys/class/dmi/id once, at the first call; nothing else has to run
        // first), the Steam Machine by its own (Valve "Fremont", read here,
        // once: Init calls this once); the Steam Frame and other Linux are
        // Other.
        Machine ThisMachine()
        {
#if defined(__APPLE__)
            return Machine::Apple;
#elif defined(__linux__) && !defined(__ANDROID__)
            if (video::ThisSteamDevice() == video::SteamDevice::Deck)
                return Machine::SteamDeck;
            return video::IsSteamMachineDmi(video::ReadDmiLine("/sys/class/dmi/id/sys_vendor"), video::ReadDmiLine("/sys/class/dmi/id/product_name"))
                ? Machine::SteamMachine : Machine::Other;
#else
            return Machine::Other;
#endif
        }

        bool s_ab[kChanges] = {};
        bool s_abAny = false;
    }

    namespace detail
    {
        std::atomic<bool> g_on[kChanges];

        void Init()
        {
            static const bool once = [] {
                bool on[kChanges];
                Choose(std::getenv("NFSMW_EFFICIENCY"), ThisMachine(), on);
                if (const char* v = std::getenv("NFSMW_EFFICIENCY_AB"))
                {
                    Parse(v, s_ab);
                    for (int c = 0; c < kChanges; c++)
                        if (s_ab[c])
                        {
                            on[c] = false;  // the first period runs the old way
                            s_abAny = true;
                        }
                }
                for (int c = 0; c < kChanges; c++)
                    g_on[c].store(on[c], std::memory_order_relaxed);
                fprintf(stderr, "[efficiency] %s%s%s\n", Describe().c_str(), s_abAny ? "; NFSMW_EFFICIENCY_AB: " : "",
                    s_abAny ? "turned on and off in turn at each [cpu] line" : "");
                return true;
            }();
            (void)once;
        }
    }

    std::string AlternateForAb()
    {
        detail::Init();
        if (!s_abAny)
            return "";
        std::string ran;
        bool wasOn = false;
        for (int c = 0; c < kChanges; c++)
            if (s_ab[c])
            {
                wasOn = detail::g_on[c].load(std::memory_order_relaxed);
                ran += std::string(" ") + kNames[c];
                detail::g_on[c].store(!wasOn, std::memory_order_relaxed);
            }
        return std::string("efficiency ab ") + (wasOn ? "on:" : "off:") + ran;
    }

    std::string Describe()
    {
        std::string text;
        for (int c = 0; c < kChanges; c++)
            text += std::string(c ? ", " : "") + kNames[c] + (detail::g_on[c].load(std::memory_order_relaxed) ? " on" : " off");
        return text;
    }
}
