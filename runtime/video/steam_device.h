// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Which Valve handheld or headset this is, by the rules the presenter already
// used: a Steam Deck is DMI vendor "Valve" with product "Jupiter" (LCD) or
// "Galileo" (OLED) (Smooth Motion's opt-out, video/presenter.cpp); the Steam
// Frame is arm64 Linux (video/refresh_notice.h's Excluded platform). The
// checks are pure for tests/shadow_auto_test.cpp; ThisSteamDevice() reads
// this machine once.
#pragma once
#include <cstdio>
#include <string>
#include <string_view>

namespace video
{
    enum class SteamDevice
    {
        Other,
        Deck,   // Steam Deck, LCD or OLED
        Frame,  // Steam Frame (arm64 Linux)
    };

    inline bool IsSteamDeckDmi(std::string_view vendor, std::string_view product)
    {
        return vendor == "Valve" && (product == "Jupiter" || product == "Galileo");
    }

    // The device from what the machine says: arm64 Linux is the Frame,
    // otherwise Linux with the Deck's DMI names is a Deck.
    inline SteamDevice SteamDeviceFrom(bool linuxArm64, std::string_view vendor, std::string_view product)
    {
        if (linuxArm64)
            return SteamDevice::Frame;
        return IsSteamDeckDmi(vendor, product) ? SteamDevice::Deck : SteamDevice::Other;
    }

    inline const char* SteamDeviceName(SteamDevice d)
    {
        switch (d)
        {
        case SteamDevice::Deck: return "Steam Deck";
        case SteamDevice::Frame: return "Steam Frame";
        default: return "";
        }
    }

    // One line of a /sys/class/dmi/id file, trailing newline and spaces cut;
    // "" when it can't be read.
    inline std::string ReadDmiLine(const char* path)
    {
        std::string v;
        if (FILE* f = fopen(path, "r"))
        {
            char buf[64] = {};
            if (fgets(buf, sizeof buf, f))
                v = buf;
            fclose(f);
        }
        while (!v.empty() && (v.back() == '\n' || v.back() == ' '))
            v.pop_back();
        return v;
    }

    inline SteamDevice ThisSteamDevice()
    {
#if defined(__linux__) && !defined(__ANDROID__)
        static const SteamDevice d = SteamDeviceFrom(
#if defined(__aarch64__)
            true,
#else
            false,
#endif
            ReadDmiLine("/sys/class/dmi/id/sys_vendor"), ReadDmiLine("/sys/class/dmi/id/product_name"));
        return d;
#else
        return SteamDevice::Other;
#endif
    }
}
