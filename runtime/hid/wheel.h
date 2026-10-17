// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// A USB steering wheel (the Logitech G29 and its family, and any other
// wheel SDL sees) as the Xbox 360 controller the game reads: the device
// itself, its force feedback, and what it amounts to each frame.
// hid/wheel_map.h holds the rules this follows; hid/input.cpp puts a
// Reading in the pad, ui/ui.cpp asks ButtonFor for the settings chord and
// the menu's navigation, and ui/settings_menu.cpp draws Settings > Wheel.
//
//   - A wheel is not a gamepad: SDL has no gamepad mapping for one, so it
//     arrives as a plain joystick. The first one connected is used, and a
//     wheel is preferred to a controller while both are plugged in. One
//     connecting or going mid-game is picked up (Initialize is not needed
//     again).
//   - Force feedback goes through SDL's haptics, which need the system to
//     drive the wheel's motors: that is the maker's driver on Windows and
//     the kernel's on Linux (a G29 on SteamOS). macOS has no such driver
//     for these wheels, so the wheel stays slack there and the log says so,
//     while steering, the pedals and the buttons all work.
#pragma once
#include <cstdint>
#include <string>

#include <SDL3/SDL_events.h>

namespace hid::wheel
{
    // Main thread, after SDL's gamepad (and so joystick) subsystem is up.
    // Not called at all while NFSMW_VIRTUAL_PAD's test controller is
    // attached: that run has its own idea of what is plugged in.
    void Initialize();
    // Main thread: every SDL event, for a wheel coming or going.
    void HandleEvent(const SDL_Event& event);
    // Main thread: a wheel is plugged in and open (Settings > Wheel's page
    // shows itself while one is).
    bool Found();
    // Main thread: ...and Settings > Wheel has it on, so the game plays
    // from it (a controller is then left alone). With it off the wheel
    // still works the menus, so the page that switched it off can switch it
    // back on.
    bool Playing();
    // Main thread: this joystick is the open wheel, so the gamepad path
    // must leave it be.
    bool Owns(SDL_JoystickID joystick);
    // Main thread: the open wheel's name, "" with none.
    std::string Name();
    // Main thread: the wheel's force feedback can be driven on this system
    // (Settings > Wheel says so where it can't).
    bool HasForceFeedback();

    // The wheel as the pad, before the deadzones and the keyboard
    // (hid/input.cpp): the buttons as XInput's bits, with Back among them,
    // and the axes as fractions, Settings > Wheel already applied.
    struct Reading
    {
        uint16_t buttons = 0;
        float steer = 0.0f;     // -1 (full left) to 1: the left stick's X
        float throttle = 0.0f;  // 0 to 1: the right trigger
        float brake = 0.0f;     // 0 to 1: the left trigger
        float clutch = 0.0f;    // 0 to 1: nothing on the pad; Settings > Wheel shows it
        int16_t rightY = 0;     // the paddles as the right stick, which is how the game shifts
        bool back = false, start = false;  // as the device reports them (the settings chord)
    };
    // Main thread, once a frame. False, with `out` left alone, when no
    // wheel is in play; the device is still read, so the page's readings
    // stay live with the wheel switched off.
    bool Sample(Reading& out);
    // Main thread: what the last Sample read, for Settings > Wheel to show
    // (the page is drawn between two samples). All zero with no wheel.
    Reading Last();

    // Main thread: the pad buttons the wheel is holding down now (ui/ui.cpp
    // feeds them to ImGui, which knows only gamepads). 0 with no wheel.
    uint16_t HeldButtons();
    // Main thread: which pad button this joystick's button is, for the Back
    // + Start chord (ui/ui.cpp). 0: none, or another device's.
    uint16_t ButtonFor(SDL_JoystickID joystick, int button);

    // Main thread: the rumble the game asked for, as Settings > Wheel >
    // Force Feedback and Settings > Input > Vibration allow. Called when
    // the request changes, as the gamepad's is.
    void Rumble(uint16_t low, uint16_t high, bool vibration);
    // Main thread, once a frame: keeps the wheel's centring force as
    // Settings > Wheel asks.
    void Update();
    // Main thread: the game is suspended, or stopping. Suspend stops what
    // the wheel is playing without letting the device go.
    void Suspend();
    void Shutdown();

    // Settings > Wheel > Pedals (ui/settings_menu.cpp). Assign watches the
    // axes and gives the one that moves furthest to a pedal, which is the
    // only way to tell three identical springs apart on a wheel we don't
    // know.
    enum class Pedal : uint8_t { Throttle, Brake, Clutch };
    // Main thread: start watching. The axes' places are remembered as they
    // are now, so a pedal already held down isn't taken for one being
    // pressed.
    void BeginAssign(Pedal pedal);
    // Main thread, once a frame while assigning: the axis that has moved
    // furthest since BeginAssign, once it has moved far enough to mean it
    // (true, and the assignment is stored and in force). False while
    // nothing has.
    bool PollAssign();
    // Main thread: give up on the assignment under way (B, or the menu
    // closing). Nothing if none is.
    void CancelAssign();
    bool Assigning();
    // Main thread: the axis each role is reading, as the device numbers
    // them; -1: none (shown in Settings > Wheel's Pedals row).
    struct Assignment { int steer = -1, throttle = -1, brake = -1, clutch = -1; };
    Assignment Axes();
    // Main thread: forget what Settings > Wheel > Pedals assigned and work
    // the axes out from the device again (the page's Reset).
    void ResetAxes();
}
