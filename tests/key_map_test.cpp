// Tests for hid/key_map.h: the 24 keyboard-driven controls, their two keys each, and how held keys
// become buttons, triggers and stick axes. Header only; from the repo root:
//   clang++ -std=c++20 -O2 -Iruntime tests/key_map_test.cpp -o build/key_map_test && build/key_map_test
#include <hid/key_map.h>

#include <cstdio>
#include <initializer_list>
#include <set>

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

using namespace hid::keymap;

static Resolved Press(std::initializer_list<Control> held)
{
    bool pressed[kControls] = {};
    for (Control c : held)
        pressed[size_t(c)] = true;
    return Resolve(pressed);
}

static bool NoAxis(const Resolved& r)
{
    for (size_t a = 0; a < size_t(Axis::Count); a++)
        if (r.axisSet[a] || r.axis[a] != 0.0f)
            return false;
    return true;
}

int main()
{
    // The table: every control once, in order, each with its own setting
    // among the Key options.
    CHECK(kControls == 24, "%zu controls", kControls);
    std::set<settings::Id> ids;
    for (size_t i = 0; i < kControls; i++)
    {
        CHECK(size_t(kEntries[i].control) == i, "entry %zu is its control", i);
        CHECK(ids.insert(kEntries[i].id).second && ids.insert(kEntries[i].id2).second, "entry %zu: its own two settings", i);
        CHECK(kEntries[i].id == settings::Id(size_t(settings::Id::KeyRightTrigger) + 2 * i), "entry %zu: in the options' order", i);
        CHECK(kEntries[i].id2 == settings::Id(size_t(kEntries[i].id) + 1), "entry %zu: its second key is the next option", i);
        CHECK(kEntries[i].id2 <= settings::Id::KeyRightStickClick2, "entry %zu: a Key option", i);
        if (kEntries[i].role == Role::Button)
            CHECK(kEntries[i].button != 0 && (kEntries[i].button & (kEntries[i].button - 1)) == 0, "entry %zu: one bit", i);
        else
            CHECK(kEntries[i].button == 0, "entry %zu: no bit", i);
    }
    CHECK(kEntries[size_t(Control::RightTrigger)].id == settings::Id::KeyRightTrigger &&
        kEntries[size_t(Control::RightStickClick)].id2 == settings::Id::KeyRightStickClick2, "first and last");
    // Either key's option finds its control.
    CHECK(&EntryOf(settings::Id::KeyRightTrigger) == &kEntries[0] && &EntryOf(settings::Id::KeyRightTrigger2) == &kEntries[0] &&
        &EntryOf(settings::Id::KeyLeftStickLeft2) == &kEntries[size_t(Control::LeftStickLeft)] &&
        &EntryOf(settings::Id::KeyRightStickClick2) == &kEntries[kControls - 1], "EntryOf");
    // The bits are XInput's (xbox.h's XAMINPUT_GAMEPAD_*; input.cpp asserts the same).
    CHECK(button::DpadUp == 0x0001 && button::DpadDown == 0x0002 && button::DpadLeft == 0x0004 && button::DpadRight == 0x0008 &&
        button::Start == 0x0010 && button::Back == 0x0020 && button::LeftThumb == 0x0040 && button::RightThumb == 0x0080 &&
        button::LeftShoulder == 0x0100 && button::RightShoulder == 0x0200 &&
        button::A == 0x1000 && button::B == 0x2000 && button::X == 0x4000 && button::Y == 0x8000, "XInput's bits");

    // Nothing held: nothing.
    Resolved r = Press({});
    CHECK(r.buttons == 0 && !r.leftTrigger && !r.rightTrigger && NoAxis(r), "idle");

    // A button key sets its bit and nothing else.
    r = Press({ Control::A });
    CHECK(r.buttons == button::A && !r.leftTrigger && !r.rightTrigger && NoAxis(r), "A: 0x%04x", r.buttons);
    CHECK(Press({ Control::B }).buttons == button::B && Press({ Control::X }).buttons == button::X && Press({ Control::Y }).buttons == button::Y, "B X Y");
    CHECK(Press({ Control::LB }).buttons == button::LeftShoulder && Press({ Control::RB }).buttons == button::RightShoulder, "LB RB");
    CHECK(Press({ Control::DpadUp }).buttons == button::DpadUp && Press({ Control::DpadDown }).buttons == button::DpadDown &&
        Press({ Control::DpadLeft }).buttons == button::DpadLeft && Press({ Control::DpadRight }).buttons == button::DpadRight, "D-pad");
    CHECK(Press({ Control::Start }).buttons == button::Start && Press({ Control::Back }).buttons == button::Back, "Start Back");
    CHECK(Press({ Control::LeftStickClick }).buttons == button::LeftThumb && Press({ Control::RightStickClick }).buttons == button::RightThumb, "clicks");
    r = Press({ Control::DpadLeft, Control::Back, Control::X });
    CHECK(r.buttons == (button::DpadLeft | button::Back | button::X), "several: 0x%04x", r.buttons);

    // A trigger key is that trigger at full.
    r = Press({ Control::RightTrigger });
    CHECK(r.rightTrigger && !r.leftTrigger && r.buttons == 0 && NoAxis(r), "right trigger");
    r = Press({ Control::LeftTrigger });
    CHECK(r.leftTrigger && !r.rightTrigger, "left trigger");

    // An axis is the difference of its keys, set while either is down, so
    // both keys centre the stick and no key leaves the controller's value.
    const size_t LX = size_t(Axis::LX), LY = size_t(Axis::LY), RX = size_t(Axis::RX), RY = size_t(Axis::RY);
    r = Press({ Control::LeftStickRight });
    CHECK(r.axisSet[LX] && r.axis[LX] == 1.0f && !r.axisSet[LY] && !r.axisSet[RX] && !r.axisSet[RY] && r.buttons == 0, "right: +1");
    r = Press({ Control::LeftStickLeft });
    CHECK(r.axisSet[LX] && r.axis[LX] == -1.0f, "left: -1");
    r = Press({ Control::LeftStickLeft, Control::LeftStickRight });
    CHECK(r.axisSet[LX] && r.axis[LX] == 0.0f, "both: set, centred");
    r = Press({ Control::LeftStickUp });
    CHECK(r.axisSet[LY] && r.axis[LY] == 1.0f && !r.axisSet[LX], "up is positive");
    r = Press({ Control::LeftStickDown });
    CHECK(r.axisSet[LY] && r.axis[LY] == -1.0f, "down is negative");
    r = Press({ Control::RightStickUp });
    CHECK(r.axisSet[RY] && r.axis[RY] == 1.0f && !r.axisSet[RX] && !r.axisSet[LY], "right stick up");
    r = Press({ Control::RightStickDown });
    CHECK(r.axisSet[RY] && r.axis[RY] == -1.0f, "right stick down");
    r = Press({ Control::RightStickLeft });
    CHECK(r.axisSet[RX] && r.axis[RX] == -1.0f && !r.axisSet[RY], "right stick left");
    r = Press({ Control::RightStickRight });
    CHECK(r.axisSet[RX] && r.axis[RX] == 1.0f, "right stick right");

    // One key on a button and an axis (the default Up): both fire.
    r = Press({ Control::DpadUp, Control::LeftStickUp });
    CHECK(r.buttons == button::DpadUp && r.axisSet[LY] && r.axis[LY] == 1.0f, "Up: D-pad and stick");

    // Everything at once is still each thing.
    bool all[kControls];
    for (bool& b : all)
        b = true;
    r = Resolve(all);
    CHECK(r.buttons == 0xF3FF && r.leftTrigger && r.rightTrigger, "all buttons: 0x%04x", r.buttons);
    for (size_t a = 0; a < size_t(Axis::Count); a++)
        CHECK(r.axisSet[a] && r.axis[a] == 0.0f, "axis %zu: both keys, centred", a);

    // Usable at compile time (constexpr), so the table is checked when it's built.
    constexpr bool kNone[kControls] = {};
    static_assert(Resolve(kNone).buttons == 0, "constexpr");

    printf("%s (%d failures)\n", g_failures ? "FAILED" : "passed", g_failures);
    return g_failures ? 1 : 0;
}
