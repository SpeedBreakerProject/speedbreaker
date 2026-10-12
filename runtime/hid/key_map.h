// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The keyboard as an Xbox 360 controller (Settings > Controls): the 24
// controls a key can drive, in the page's order, each with the two settings
// that hold its keys (user/settings.h, the [keys] table of settings.toml),
// and how a set of held keys becomes buttons, triggers and stick axes.
// Header only and SDL-free: hid/input.cpp samples the keys and feeds the
// result to the pad; ui/settings_menu.cpp draws one row per entry;
// tests/key_map_test.cpp checks the rules.
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

#include <user/settings.h>

namespace hid::keymap
{
    enum class Control : uint8_t
    {
        RightTrigger, LeftTrigger,
        LeftStickLeft, LeftStickRight, LeftStickUp, LeftStickDown,
        A, B, X, Y, LB, RB,
        DpadUp, DpadDown, DpadLeft, DpadRight,
        Start, Back,
        RightStickUp, RightStickDown, RightStickLeft, RightStickRight,
        LeftStickClick, RightStickClick,
        Count
    };
    constexpr size_t kControls = size_t(Control::Count);

    enum class Role : uint8_t { Button, LeftTrigger, RightTrigger, AxisNeg, AxisPos };
    enum class Axis : uint8_t { LX, LY, RX, RY, Count };

    // XInput's button bits, as the game reads them (xbox.h's
    // XAMINPUT_GAMEPAD_*; input.cpp asserts they agree). Literals here so
    // this header needs no kernel header.
    namespace button
    {
        constexpr uint16_t DpadUp = 0x0001, DpadDown = 0x0002, DpadLeft = 0x0004, DpadRight = 0x0008;
        constexpr uint16_t Start = 0x0010, Back = 0x0020, LeftThumb = 0x0040, RightThumb = 0x0080;
        constexpr uint16_t LeftShoulder = 0x0100, RightShoulder = 0x0200;
        constexpr uint16_t A = 0x1000, B = 0x2000, X = 0x4000, Y = 0x8000;
    }

    struct Entry
    {
        Control control;
        settings::Id id;       // the option holding its first key
        settings::Id id2;      // ...and its second (always id + 1)
        Role role;
        uint16_t button = 0;   // Role::Button
        Axis axis = Axis::LX;  // Role::AxisNeg, AxisPos
    };

    constexpr Entry kEntries[kControls] = {
        { Control::RightTrigger, settings::Id::KeyRightTrigger, settings::Id::KeyRightTrigger2, Role::RightTrigger },
        { Control::LeftTrigger, settings::Id::KeyLeftTrigger, settings::Id::KeyLeftTrigger2, Role::LeftTrigger },
        { Control::LeftStickLeft, settings::Id::KeyLeftStickLeft, settings::Id::KeyLeftStickLeft2, Role::AxisNeg, 0, Axis::LX },
        { Control::LeftStickRight, settings::Id::KeyLeftStickRight, settings::Id::KeyLeftStickRight2, Role::AxisPos, 0, Axis::LX },
        { Control::LeftStickUp, settings::Id::KeyLeftStickUp, settings::Id::KeyLeftStickUp2, Role::AxisPos, 0, Axis::LY },
        { Control::LeftStickDown, settings::Id::KeyLeftStickDown, settings::Id::KeyLeftStickDown2, Role::AxisNeg, 0, Axis::LY },
        { Control::A, settings::Id::KeyA, settings::Id::KeyA2, Role::Button, button::A },
        { Control::B, settings::Id::KeyB, settings::Id::KeyB2, Role::Button, button::B },
        { Control::X, settings::Id::KeyX, settings::Id::KeyX2, Role::Button, button::X },
        { Control::Y, settings::Id::KeyY, settings::Id::KeyY2, Role::Button, button::Y },
        { Control::LB, settings::Id::KeyLB, settings::Id::KeyLB2, Role::Button, button::LeftShoulder },
        { Control::RB, settings::Id::KeyRB, settings::Id::KeyRB2, Role::Button, button::RightShoulder },
        { Control::DpadUp, settings::Id::KeyDpadUp, settings::Id::KeyDpadUp2, Role::Button, button::DpadUp },
        { Control::DpadDown, settings::Id::KeyDpadDown, settings::Id::KeyDpadDown2, Role::Button, button::DpadDown },
        { Control::DpadLeft, settings::Id::KeyDpadLeft, settings::Id::KeyDpadLeft2, Role::Button, button::DpadLeft },
        { Control::DpadRight, settings::Id::KeyDpadRight, settings::Id::KeyDpadRight2, Role::Button, button::DpadRight },
        { Control::Start, settings::Id::KeyStart, settings::Id::KeyStart2, Role::Button, button::Start },
        { Control::Back, settings::Id::KeyBack, settings::Id::KeyBack2, Role::Button, button::Back },
        { Control::RightStickUp, settings::Id::KeyRightStickUp, settings::Id::KeyRightStickUp2, Role::AxisPos, 0, Axis::RY },
        { Control::RightStickDown, settings::Id::KeyRightStickDown, settings::Id::KeyRightStickDown2, Role::AxisNeg, 0, Axis::RY },
        { Control::RightStickLeft, settings::Id::KeyRightStickLeft, settings::Id::KeyRightStickLeft2, Role::AxisNeg, 0, Axis::RX },
        { Control::RightStickRight, settings::Id::KeyRightStickRight, settings::Id::KeyRightStickRight2, Role::AxisPos, 0, Axis::RX },
        { Control::LeftStickClick, settings::Id::KeyLeftStickClick, settings::Id::KeyLeftStickClick2, Role::Button, button::LeftThumb },
        { Control::RightStickClick, settings::Id::KeyRightStickClick, settings::Id::KeyRightStickClick2, Role::Button, button::RightThumb },
    };

    consteval bool EntriesInOrder()
    {
        for (size_t i = 0; i < kControls; i++)
            if (size_t(kEntries[i].control) != i || size_t(kEntries[i].id) != size_t(settings::Id::KeyRightTrigger) + 2 * i ||
                size_t(kEntries[i].id2) != size_t(kEntries[i].id) + 1)
                return false;
        return true;
    }
    static_assert(EntriesInOrder(), "one Entry per Control, in Control order, over the Key options in pairs");

    // The control either of a Key option's keys belongs to.
    constexpr const Entry& EntryOf(settings::Id id)
    {
        return kEntries[(size_t(id) - size_t(settings::Id::KeyRightTrigger)) / 2];
    }

    // What the held keys amount to.
    struct Resolved
    {
        uint16_t buttons = 0;
        bool leftTrigger = false, rightTrigger = false;  // a trigger key is down: that trigger at full
        bool axisSet[size_t(Axis::Count)] = {};          // either key of the axis is down: the keyboard has the axis
        float axis[size_t(Axis::Count)] = {};            // +1, 0 (both keys) or -1 while set; up is positive, as XInput has it
    };

    // pressed[i]: either key bound to Control(i) is down. Buttons OR in; an axis
    // is its positive key minus its negative one, and belongs to the
    // keyboard while either is held (both held: centred).
    constexpr Resolved Resolve(std::span<const bool, kControls> pressed)
    {
        Resolved r;
        for (size_t i = 0; i < kControls; i++)
        {
            if (!pressed[i])
                continue;
            const Entry& e = kEntries[i];
            switch (e.role)
            {
            case Role::Button:
                r.buttons |= e.button;
                break;
            case Role::LeftTrigger:
                r.leftTrigger = true;
                break;
            case Role::RightTrigger:
                r.rightTrigger = true;
                break;
            case Role::AxisNeg:
                r.axisSet[size_t(e.axis)] = true;
                r.axis[size_t(e.axis)] -= 1.0f;
                break;
            case Role::AxisPos:
                r.axisSet[size_t(e.axis)] = true;
                r.axis[size_t(e.axis)] += 1.0f;
                break;
            }
        }
        return r;
    }
}
