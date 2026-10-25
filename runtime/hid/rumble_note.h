// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The one-time note that an Xbox controller connected by USB to a Mac takes
// no rumble. macOS hands a wired Xbox One/Series pad to SDL through Apple's
// GameController framework (the GUID's driver byte 'm', connection unknown),
// whose haptics accept every call and buzz nothing (a standalone CoreHaptics
// test: every call "ok", no motor); over Bluetooth the same pad goes through
// SDL's HIDAPI ('h') and rumbles, and a DualSense rumbles either way.
// Nothing the game can do about it, so it says so once. When it shows;
// hid/input.cpp feeds it and ui::ShowNotice draws it. Header only:
// tests/rumble_note_test.cpp checks it.
//
// Shown when, all at once:
//   - on macOS (not iOS, Windows or Linux);
//   - player 1's controller is an Xbox one (USB vendor 0x045E, Microsoft, or
//     "Xbox" in its name: a licensed pad from another maker);
//   - SDL reaches it through Apple's GameController framework (driver 'm');
//   - it isn't reported wireless (wired or unknown: GameController reports
//     no connection);
//   - Settings > Input > Vibration is on (off, there is nothing to miss);
//   - not dismissed before (settings.toml's [app] xbox_usb_rumble_note).
// The game pauses under it, as for the refresh-rate notice; A, B, Start, Esc
// or OK closes it, and it is recorded then.
#pragma once
#include <cctype>
#include <cstdint>
#include <string_view>

namespace hid
{
    struct RumbleNote
    {
        enum class Platform
        {
            Mac,
            Other,  // iOS, Windows, Linux
        };
        enum class Connection
        {
            Unknown,
            Wired,
            Wireless,
        };

        struct Inputs
        {
            Platform platform = Platform::Other;
            char driver = 0;  // the SDL GUID's driver byte: 'h' HIDAPI, 'm' GameController, 0 the system's HID
            Connection connection = Connection::Unknown;
            uint16_t vendor = 0;  // the USB vendor id (0 unknown)
            std::string_view name;
            bool vibration = true;     // Settings > Input > Vibration
            bool shownBefore = false;  // settings.toml's [app] xbox_usb_rumble_note
        };

        struct Decision
        {
            bool show;
            const char* reason;  // why (not), for the log
        };

        static constexpr uint16_t kMicrosoft = 0x045E;
        static constexpr const char* kTitle = "No rumble over USB";
        static constexpr const char* kText =
            "Rumble doesn't work with Xbox controllers connected by USB on a Mac. "
            "Pair it over Bluetooth for rumble.";

        static bool IsXbox(uint16_t vendor, std::string_view name)
        {
            if (vendor == kMicrosoft)
                return true;
            constexpr std::string_view xbox = "xbox";
            for (size_t i = 0; i + xbox.size() <= name.size(); i++)
            {
                size_t k = 0;
                while (k < xbox.size() && std::tolower(uint8_t(name[i + k])) == xbox[k])
                    k++;
                if (k == xbox.size())
                    return true;
            }
            return false;
        }

        static Decision Decide(const Inputs& in)
        {
            if (in.platform != Platform::Mac)
                return { false, "not macOS" };
            if (!IsXbox(in.vendor, in.name))
                return { false, "not an Xbox controller" };
            if (in.driver != 'm')
                return { false, "not through Apple's GameController (this route rumbles)" };
            if (in.connection == Connection::Wireless)
                return { false, "wireless" };
            if (!in.vibration)
                return { false, "Vibration is off" };
            if (in.shownBefore)
                return { false, "dismissed before (settings.toml [app] xbox_usb_rumble_note)" };
            return { true, "an Xbox controller by USB through Apple's GameController, Vibration on" };
        }
    };
}
