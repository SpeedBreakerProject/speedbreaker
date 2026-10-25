// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See input.h.
// A steering wheel's own side of this is hid/wheel.cpp.
#include <stdafx.h>
#include "input.h"
#include "key_map.h"
#include "player_slots.h"
#include "rumble_note.h"
#include "wheel.h"
#include <cpu/guest_time.h>
#include <game/split_screen.h>
#include <kernel/function.h>
#include <ui/ui.h>
#include <user/settings.h>
#include <user/key_names.h>

#include <SDL3/SDL.h>

// key_map.h names XInput's bits as literals (it has no kernel header): keep
// them the bits the game reads.
static_assert(hid::keymap::button::DpadUp == XAMINPUT_GAMEPAD_DPAD_UP && hid::keymap::button::DpadDown == XAMINPUT_GAMEPAD_DPAD_DOWN &&
    hid::keymap::button::DpadLeft == XAMINPUT_GAMEPAD_DPAD_LEFT && hid::keymap::button::DpadRight == XAMINPUT_GAMEPAD_DPAD_RIGHT &&
    hid::keymap::button::Start == XAMINPUT_GAMEPAD_START && hid::keymap::button::Back == XAMINPUT_GAMEPAD_BACK &&
    hid::keymap::button::LeftThumb == XAMINPUT_GAMEPAD_LEFT_THUMB && hid::keymap::button::RightThumb == XAMINPUT_GAMEPAD_RIGHT_THUMB &&
    hid::keymap::button::LeftShoulder == XAMINPUT_GAMEPAD_LEFT_SHOULDER && hid::keymap::button::RightShoulder == XAMINPUT_GAMEPAD_RIGHT_SHOULDER &&
    hid::keymap::button::A == XAMINPUT_GAMEPAD_A && hid::keymap::button::B == XAMINPUT_GAMEPAD_B &&
    hid::keymap::button::X == XAMINPUT_GAMEPAD_X && hid::keymap::button::Y == XAMINPUT_GAMEPAD_Y,
    "key_map.h's button bits are XInput's");
// The key codes of user/key_names.h are SDL's scancodes (USB HID usages);
// tests/key_names_test.cpp checks every row, this pins a few.
static_assert(settings::keynames::Code("a") == SDL_SCANCODE_A && settings::keynames::Code("keypad_1") == SDL_SCANCODE_KP_1 &&
              settings::keynames::Code("right_alt") == SDL_SCANCODE_RALT && settings::keynames::Code("escape") == SDL_SCANCODE_ESCAPE);

namespace
{
#ifndef ERROR_SUCCESS  // (<windows.h> has these, with the same values)
    constexpr uint32_t ERROR_SUCCESS = 0;
    constexpr uint32_t ERROR_DEVICE_NOT_CONNECTED = 0x48F;
#endif

    struct Pad
    {
        uint16_t buttons = 0;
        uint8_t leftTrigger = 0, rightTrigger = 0;
        int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    };

    std::mutex s_mutex;
    Pad s_pad;                      // latest sample (devices only)
    bool s_deviceConnected = false; // a gamepad, or the keyboard of our window
    uint32_t s_packet = 0;
    uint16_t s_rumbleLow = 0, s_rumbleHigh = 0;
    bool s_rumbleDirty = false;

    SDL_Gamepad* s_gamepad = nullptr;
    std::atomic<bool> s_blocked{ false };

    // Split screen (game/split_screen.h, hid/player_slots.h): XInput users
    // 1-3 are the other players' controllers. Under s_mutex, written by
    // Update and by a controller's removal; [0] unused (user 0 is s_pad).
    struct OtherUser
    {
        bool connected = false;
        Pad pad;
        uint32_t packet = 0;
        Pad last;  // the state packet counted last
        uint16_t rumbleLow = 0, rumbleHigh = 0;
        bool rumbleDirty = false;
        bool told = false;  // connected, as the game last saw it (logged when it changes)
    };
    OtherUser s_users[hid::PlayerSlots::kUsers];
    // Main thread: the controllers other than s_gamepad, open only while the
    // game is in split screen, and which player each is.
    struct OtherPad
    {
        SDL_JoystickID id = 0;
        SDL_Gamepad* gamepad = nullptr;
        uint16_t latched = 0;  // held when a menu closed (s_latchedButtons' rule)
        int user = 0;          // as last logged
    };
    std::vector<OtherPad> s_others;
    hid::PlayerSlots s_slots;
    // NFSMW_OTHER_PADS=always, a diagnostic: the other controllers are users
    // 1-3 in single player too (what a second pad would do there).
    const bool s_otherPadsAlways = [] {
        const char* v = std::getenv("NFSMW_OTHER_PADS");
        const bool always = v && strcmp(v, "always") == 0;
        if (always)
            fprintf(stderr, "[input] NFSMW_OTHER_PADS=always: other controllers are XInput users 1-3 outside split screen too\n");
        return always;
    }();

    // NFSMW_TEST_SPLIT="35-45,60-70" (tests): the input side of split screen
    // (other controllers as users 1-3, the notices) on over those seconds of
    // guest time too, whatever the game says; the renderer is not affected.
    std::vector<std::pair<double, double>> ParseTestSplit()
    {
        std::vector<std::pair<double, double>> ranges;
        const char* v = std::getenv("NFSMW_TEST_SPLIT");
        for (std::string_view rest = v ? v : ""; !rest.empty();)
        {
            const size_t comma = rest.find(',');
            const std::string item(rest.substr(0, comma));
            rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
            const size_t dash = item.find('-');
            if (dash != std::string::npos)
                ranges.push_back({ std::atof(item.c_str()), std::atof(item.c_str() + dash + 1) });
        }
        if (!ranges.empty())
            fprintf(stderr, "[input] NFSMW_TEST_SPLIT: %zu ranges of input-side split screen\n", ranges.size());
        return ranges;
    }
    const std::vector<std::pair<double, double>> s_testSplit = ParseTestSplit();

    // Main thread: the notices for a player whose controller went in split
    // screen (bit 0: player 1's, bits 1-3: users 1-3's), as shown now.
    // The game itself only pauses under a bare PAUSE banner (X360_Pause.fng,
    // the screen it also shows under the 360's Guide): on a 360 the console's
    // own overlay asked for the controller. Its leftover ControllerUnplugged.fng
    // ("Please reconnect the controller to controller port %d and press START
    // to continue.") is never pushed. Player 1 keeps the keyboard, so the game
    // never sees user 0 go: a notice only.
    uint32_t s_noticesShown = 0;
    bool s_p1Lost = false;  // player 1's controller went in split screen, none since
    std::string MissingNotice(int user)
    {
        if (user == 0)
            return "Player 1's controller disconnected. Reconnect it, or play on with the keyboard.";
        return "Player " + std::to_string(user + 1) + "'s controller disconnected. Reconnect it and press START.";
    }

    // SDL's buttons as XInput's.
    const std::pair<SDL_GamepadButton, uint16_t> kButtonMap[] = {
        { SDL_GAMEPAD_BUTTON_SOUTH, XAMINPUT_GAMEPAD_A }, { SDL_GAMEPAD_BUTTON_EAST, XAMINPUT_GAMEPAD_B },
        { SDL_GAMEPAD_BUTTON_WEST, XAMINPUT_GAMEPAD_X }, { SDL_GAMEPAD_BUTTON_NORTH, XAMINPUT_GAMEPAD_Y },
        { SDL_GAMEPAD_BUTTON_BACK, XAMINPUT_GAMEPAD_BACK }, { SDL_GAMEPAD_BUTTON_START, XAMINPUT_GAMEPAD_START },
        { SDL_GAMEPAD_BUTTON_LEFT_STICK, XAMINPUT_GAMEPAD_LEFT_THUMB },
        { SDL_GAMEPAD_BUTTON_RIGHT_STICK, XAMINPUT_GAMEPAD_RIGHT_THUMB },
        { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, XAMINPUT_GAMEPAD_LEFT_SHOULDER },
        { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, XAMINPUT_GAMEPAD_RIGHT_SHOULDER },
        { SDL_GAMEPAD_BUTTON_DPAD_UP, XAMINPUT_GAMEPAD_DPAD_UP }, { SDL_GAMEPAD_BUTTON_DPAD_DOWN, XAMINPUT_GAMEPAD_DPAD_DOWN },
        { SDL_GAMEPAD_BUTTON_DPAD_LEFT, XAMINPUT_GAMEPAD_DPAD_LEFT }, { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, XAMINPUT_GAMEPAD_DPAD_RIGHT },
    };
    // Main thread: buttons held when a menu closed, hidden from the game
    // until each is released (the B, Start or A that closed it).
    uint16_t s_latchedButtons = 0;
    bool s_wasBlocked = false;
    bool s_scriptSuppressed = false;  // under s_mutex: no scripted input while a menu is open
    std::chrono::steady_clock::time_point s_lastInput;  // main thread: a control last in use (SecondsSinceInput)
    bool s_inputSeen = false;
    hid::wheelmap::WheelStill s_wheelStill;  // main thread: a wheel's steering counts when it turns (wheel_map.h)

    // The controller's Back is held back from the game until it's clear it
    // isn't the start of the Back + Start settings chord: released (then
    // replayed as a short tap), or held 250 ms without Start. Main thread.
    // In guest time, so a suspension can't eat the replayed tap.
    enum class BackState { Idle, Pending, Passing, Replay, Swallow };
    BackState s_backState = BackState::Idle;
    int64_t s_backSince = 0;  // guest ns

    bool PadBackForGame(bool back, bool start)
    {
        int64_t now = guesttime::NowNs();
        switch (s_backState)
        {
        case BackState::Idle:
            if (back)
            {
                s_backState = start ? BackState::Swallow : BackState::Pending;
                s_backSince = now;
            }
            return false;
        case BackState::Pending:
            if (start)
            {
                s_backState = BackState::Swallow;  // the chord: the UI has it
                return false;
            }
            if (!back)
            {
                s_backState = BackState::Replay;
                s_backSince = now;
                return true;
            }
            if (now - s_backSince >= 250'000'000)
            {
                s_backState = BackState::Passing;
                return true;
            }
            return false;
        case BackState::Passing:
            if (!back)
                s_backState = BackState::Idle;
            return back;
        case BackState::Replay:
            if (now - s_backSince < 100'000'000)
                return true;
            s_backState = BackState::Idle;
            return false;
        case BackState::Swallow:
            if (!back && !start)
                s_backState = BackState::Idle;
            return false;
        }
        return false;
    }

    // A scripted input: buttons (or analog: RT/LT triggers, LL/LR/LU/LD left
    // stick) held over [time, time + duration).
    // `steps`: time and duration in world steps (the stepper's count, which
    // restarts at each load) instead of seconds: "w6668~w7148:RT".
    struct ScriptTap { double time; uint16_t buttons; int16_t lx, ly; double duration = 0.15; uint8_t lt = 0, rt = 0; bool steps = false; };
    std::vector<ScriptTap> s_script;
    // Guest ns at launch: the script and the virtual pad run on guest time,
    // so a press due while the game is suspended comes after it instead of
    // being lost.
    const int64_t s_start = guesttime::NowNs();

    double SecondsSinceLaunch()
    {
        return double(guesttime::NowNs() - s_start) / 1e9;
    }

    // NFSMW_INPUT_STEPS_AFTER=<s>: world-step taps ("w..." times) count from
    // this many seconds after launch (past the load whose steps they count),
    // so automated runs press at the same world step whatever the loads took
    // (the A/B tests of game/hfr.h: a wall-clock press lands on another step
    // when a load is shorter).
    const double s_stepsAfter = [] { const char* v = std::getenv("NFSMW_INPUT_STEPS_AFTER"); return v ? std::atof(v) : 0.0; }();

    // The world stepper's count (game/hfr.cpp, frame_time.cpp: [0x82A2CB6C] + 40); -1 without one.
    double WorldSteps()
    {
        uint8_t* base = g_memory.base;
        if (!base)
            return -1;
        const uint32_t stepper = PPC_LOAD_U32(0x82A2CB6C);
        return stepper ? double(PPC_LOAD_U32(stepper + 40)) : -1.0;
    }

    int16_t ToShort(float v)
    {
        return int16_t(std::clamp(v * 32767.0f, -32768.0f, 32767.0f));
    }

    // Radial deadzone with rescale: |v| < dz -> 0, else (|v|-dz)/(1-dz).
    void ApplyDeadzone(float& x, float& y, float dz)
    {
        float m = std::sqrt(x * x + y * y);
        if (m <= dz)
        {
            x = y = 0.0f;
            return;
        }
        float scaled = std::min(1.0f, (m - dz) / (1.0f - dz));
        x = x / m * scaled;
        y = y / m * scaled;
    }

    uint8_t Trigger(float v, float dz)
    {
        if (v <= dz)
            return 0;
        return uint8_t(std::clamp((v - dz) / (1.0f - dz), 0.0f, 1.0f) * 255.0f + 0.5f);
    }

    uint16_t ButtonByName(const std::string& n)
    {
        static const std::pair<const char*, uint16_t> names[] = {
            { "UP", XAMINPUT_GAMEPAD_DPAD_UP }, { "DOWN", XAMINPUT_GAMEPAD_DPAD_DOWN },
            { "LEFT", XAMINPUT_GAMEPAD_DPAD_LEFT }, { "RIGHT", XAMINPUT_GAMEPAD_DPAD_RIGHT },
            { "START", XAMINPUT_GAMEPAD_START }, { "BACK", XAMINPUT_GAMEPAD_BACK },
            { "LS", XAMINPUT_GAMEPAD_LEFT_THUMB }, { "RS", XAMINPUT_GAMEPAD_RIGHT_THUMB },
            { "LB", XAMINPUT_GAMEPAD_LEFT_SHOULDER }, { "RB", XAMINPUT_GAMEPAD_RIGHT_SHOULDER },
            { "A", XAMINPUT_GAMEPAD_A }, { "B", XAMINPUT_GAMEPAD_B }, { "X", XAMINPUT_GAMEPAD_X }, { "Y", XAMINPUT_GAMEPAD_Y },
        };
        for (auto& [name, bit] : names)
            if (n == name)
                return bit;
        return 0;
    }

    void ParseScript()
    {
        const char* s = std::getenv("NFSMW_INPUT_SCRIPT");
        if (!s)
            return;
        std::string all = s;
        size_t pos = 0;
        while (pos < all.size())
        {
            size_t end = all.find(',', pos);
            std::string item = all.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            pos = end == std::string::npos ? all.size() : end + 1;
            size_t colon = item.find(':');
            if (colon == std::string::npos)
                continue;
            // "sec:BUTTON", "from-to/step:BUTTON" (a tap every `step` s) or
            // "from~to:BUTTON" (held).
            std::string when = item.substr(0, colon);
            std::string name = item.substr(colon + 1);
            ScriptTap proto{ 0, ButtonByName(name), 0, 0 };
            // World steps: "w<step>" (a tap of 9 steps) or "w<from>~w<to>" (held).
            if (!when.empty() && when[0] == 'w')
            {
                proto.steps = true;
                proto.duration = 9;
                when.erase(std::remove(when.begin(), when.end(), 'w'), when.end());
            }
            if (name == "RT") proto.rt = 255;
            else if (name == "LT") proto.lt = 255;
            else if (name == "LL") proto.lx = -32767;
            else if (name == "LR") proto.lx = 32767;
            else if (name == "LU") proto.ly = 32767;
            else if (name == "LD") proto.ly = -32767;
            else if (proto.buttons == 0)
            {
                fprintf(stderr, "[input] script: unknown button in \"%s\"\n", item.c_str());
                continue;
            }
            uint16_t button = proto.buttons;
            if (size_t tilde = when.find('~'); tilde != std::string::npos)
            {
                ScriptTap hold = proto;
                hold.time = std::atof(when.substr(0, tilde).c_str());
                hold.duration = std::atof(when.substr(tilde + 1).c_str()) - hold.time;
                s_script.push_back(hold);
                continue;
            }
            size_t dash = when.find('-'), slash = when.find('/');
            if (dash != std::string::npos && slash != std::string::npos && slash > dash)
            {
                double from = std::atof(when.substr(0, dash).c_str());
                double to = std::atof(when.substr(dash + 1, slash - dash - 1).c_str());
                double step = std::max(0.2, std::atof(when.substr(slash + 1).c_str()));
                for (double t = from; t <= to; t += step)
                {
                    ScriptTap tap = proto;
                    tap.time = t;
                    s_script.push_back(tap);
                }
            }
            else
            {
                ScriptTap tap = proto;
                tap.time = std::atof(when.c_str());
                s_script.push_back(tap);
            }
        }
        fprintf(stderr, "[input] script: %zu taps\n", s_script.size());
    }

    // Apply the scripted inputs active now on top of the device state.
    void ApplyScript(Pad& p)
    {
        if (s_script.empty())
            return;
        double t = SecondsSinceLaunch();
        const double steps = t >= s_stepsAfter ? WorldSteps() : -1.0;
        // The first world-step tap that applies: a line saying when (checks the clock).
        static bool said = false;
        if (!said && steps >= 0)
            for (const auto& tap : s_script)
                if (tap.steps && steps >= tap.time && steps < tap.time + tap.duration)
                {
                    said = true;
                    fprintf(stderr, "[input] script: first world-step tap (w%.0f) at world step %.0f, %.3f s after launch\n", tap.time, steps, t);
                    break;
                }
        for (const auto& tap : s_script)
            if (tap.steps ? steps >= 0 && steps >= tap.time && steps < tap.time + tap.duration
                          : t >= tap.time && t < tap.time + tap.duration)
            {
                p.buttons |= tap.buttons;
                p.leftTrigger = std::max(p.leftTrigger, tap.lt);
                p.rightTrigger = std::max(p.rightTrigger, tap.rt);
                if (tap.lx) p.lx = tap.lx;
                if (tap.ly) p.ly = tap.ly;
            }
    }

    const bool s_scriptParsed = (ParseScript(), true);

    // NFSMW_VIRTUAL_PAD="2:LX=0.05,4:LX=0.2,6:RT=0.01": an SDL virtual
    // controller, the only one opened, whose axes move to those values at
    // seconds since launch (sticks LX/LY/RX/RY -1..1, up positive; triggers
    // LT/RT 0..1) and hold them. Its buttons too: "5:BACK=1" holds one down,
    // "5.2:BACK=0" lets it go, "6:DOWN" taps it (0.15 s); A B X Y BACK START
    // LB RB LS RS and the D-pad's UP DOWN LEFT RIGHT. Being a real SDL
    // controller, it drives the menus (Back + Start, the D-pad, A, B) as
    // well as the game. A button changes once a frame at most, so a tap due
    // whole within one long frame (a capture's stall) still shows a frame
    // pressed. "12:SHOT" saves the next presented frame (ui::RequestShot), on
    // the pad's clock. NFSMW_INPUT_SCRIPT feeds the game after the
    // device path; this goes through it, so the deadzones and Vibration
    // reach it as they would a real controller, for tests without one. Each
    // step logs what the game then gets; the rumble the game asks for and
    // the rumble the pad receives are logged.
    // NFSMW_VIRTUAL_PAD_2 (and _3, _4), the same syntax: more virtual
    // controllers, for split-screen tests (the other players' pads); with
    // any of them set, only the virtual ones are opened. Two more steps for
    // them: "40:DETACH" unplugs the pad (SDL removes it, as a real one
    // going), "50:ATTACH" plugs it in again (a new SDL id).
    struct PadStep { double time; SDL_GamepadAxis axis; float value; std::string text; int button = -1; bool down = false; };
    constexpr int kShotStep = -2;    // PadStep::button: a capture, not a button
    constexpr int kDetachStep = -3;  // unplug
    constexpr int kAttachStep = -4;  // plug in again
    struct VirtualPad
    {
        std::vector<PadStep> steps;
        size_t next = 0;
        SDL_JoystickID id = 0;          // 0: not attached
        SDL_Joystick* joystick = nullptr;
        int number = 1;                 // 1 = NFSMW_VIRTUAL_PAD
    };
    VirtualPad s_vpads[hid::PlayerSlots::kUsers];
    bool s_virtualOnly = false;  // a virtual pad was asked for: real controllers are left alone

    bool SDLCALL VirtualPadRumble(void* userdata, Uint16 low, Uint16 high)
    {
        const int number = int(reinterpret_cast<intptr_t>(userdata));
        if (number == 1)
            fprintf(stderr, "[input] virtual pad: rumble low %u, high %u\n", unsigned(low), unsigned(high));
        else
            fprintf(stderr, "[input] virtual pad %d: rumble low %u, high %u\n", number, unsigned(low), unsigned(high));
        return true;
    }

    bool IsVirtualPad(SDL_JoystickID id)
    {
        for (const VirtualPad& v : s_vpads)
            if (v.id && v.id == id)
                return true;
        return false;
    }

    bool PlugVirtualPad(VirtualPad& vp)
    {
        const std::string name = vp.number == 1 ? "SpeedBreaker virtual pad" : "SpeedBreaker virtual pad " + std::to_string(vp.number);
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.name = name.c_str();
        if (vp.number > 1)
            desc.product_id = Uint16(vp.number);  // its own SDL mapping, so its own name (pad 1's descriptor as before)
        desc.Rumble = VirtualPadRumble;
        desc.userdata = reinterpret_cast<void*>(intptr_t(vp.number));
        vp.id = SDL_AttachVirtualJoystick(&desc);
        vp.joystick = vp.id ? SDL_OpenJoystick(vp.id) : nullptr;
        if (!vp.joystick)
        {
            fprintf(stderr, "[input] %s: no virtual controller: %s\n", vp.number == 1 ? "NFSMW_VIRTUAL_PAD" : name.c_str(), SDL_GetError());
            if (vp.id)
                SDL_DetachVirtualJoystick(vp.id);
            vp.id = 0;
            return false;
        }
        return true;
    }

    void ParseVirtualPad(const char* v, VirtualPad& vp)
    {
        static const std::pair<const char*, SDL_GamepadAxis> axes[] = {
            { "LX", SDL_GAMEPAD_AXIS_LEFTX }, { "LY", SDL_GAMEPAD_AXIS_LEFTY }, { "RX", SDL_GAMEPAD_AXIS_RIGHTX },
            { "RY", SDL_GAMEPAD_AXIS_RIGHTY }, { "LT", SDL_GAMEPAD_AXIS_LEFT_TRIGGER }, { "RT", SDL_GAMEPAD_AXIS_RIGHT_TRIGGER },
        };
        const char* var = vp.number == 1 ? "NFSMW_VIRTUAL_PAD" : vp.number == 2 ? "NFSMW_VIRTUAL_PAD_2" : vp.number == 3 ? "NFSMW_VIRTUAL_PAD_3" : "NFSMW_VIRTUAL_PAD_4";
        for (std::string_view rest = v; !rest.empty();)
        {
            size_t comma = rest.find(',');
            std::string item(rest.substr(0, comma));
            rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
            size_t colon = item.find(':'), equals = item.find('=');
            if (colon == std::string::npos || (equals != std::string::npos && equals < colon))
                continue;
            std::string name = item.substr(colon + 1, equals == std::string::npos ? std::string::npos : equals - colon - 1);
            static const std::pair<const char*, SDL_GamepadButton> buttons[] = {
                { "A", SDL_GAMEPAD_BUTTON_SOUTH }, { "B", SDL_GAMEPAD_BUTTON_EAST }, { "X", SDL_GAMEPAD_BUTTON_WEST },
                { "Y", SDL_GAMEPAD_BUTTON_NORTH }, { "BACK", SDL_GAMEPAD_BUTTON_BACK }, { "START", SDL_GAMEPAD_BUTTON_START },
                { "LB", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER }, { "RB", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER },
                { "LS", SDL_GAMEPAD_BUTTON_LEFT_STICK }, { "RS", SDL_GAMEPAD_BUTTON_RIGHT_STICK },
                { "UP", SDL_GAMEPAD_BUTTON_DPAD_UP }, { "DOWN", SDL_GAMEPAD_BUTTON_DPAD_DOWN },
                { "LEFT", SDL_GAMEPAD_BUTTON_DPAD_LEFT }, { "RIGHT", SDL_GAMEPAD_BUTTON_DPAD_RIGHT },
            };
            if (name == "SHOT")
            {
                vp.steps.push_back({ std::atof(item.c_str()), SDL_GAMEPAD_AXIS_INVALID, 0.0f, name, kShotStep, false });
                continue;
            }
            if (name == "DETACH" || name == "ATTACH")
            {
                vp.steps.push_back({ std::atof(item.c_str()), SDL_GAMEPAD_AXIS_INVALID, 0.0f, name, name == "DETACH" ? kDetachStep : kAttachStep, false });
                continue;
            }
            bool found = false;
            for (auto& [n, button] : buttons)
                if (name == n)
                {
                    const double at = std::atof(item.c_str());
                    if (equals == std::string::npos)
                    {
                        // A tap: down, and up again 0.15 s later.
                        vp.steps.push_back({ at, SDL_GAMEPAD_AXIS_INVALID, 0.0f, name + " down", int(button), true });
                        vp.steps.push_back({ at + 0.15, SDL_GAMEPAD_AXIS_INVALID, 0.0f, name + " up", int(button), false });
                    }
                    else
                    {
                        const bool down = std::atof(item.c_str() + equals + 1) != 0.0;
                        vp.steps.push_back({ at, SDL_GAMEPAD_AXIS_INVALID, 0.0f, name + (down ? " down" : " up"), int(button), down });
                    }
                    found = true;
                }
            if (found)
                continue;
            if (equals == std::string::npos)
            {
                fprintf(stderr, "[input] %s: unknown button in \"%s\"\n", var, item.c_str());
                continue;
            }
            for (auto& [n, axis] : axes)
                if (name == n)
                {
                    vp.steps.push_back({ std::atof(item.c_str()), axis, float(std::atof(item.c_str() + equals + 1)), item.substr(colon + 1) });
                    found = true;
                }
            if (!found)
                fprintf(stderr, "[input] %s: unknown axis in \"%s\"\n", var, item.c_str());
        }
        std::stable_sort(vp.steps.begin(), vp.steps.end(), [](const PadStep& a, const PadStep& b) { return a.time < b.time; });
    }

    void AttachVirtualPad()
    {
        static const char* const vars[] = { "NFSMW_VIRTUAL_PAD", "NFSMW_VIRTUAL_PAD_2", "NFSMW_VIRTUAL_PAD_3", "NFSMW_VIRTUAL_PAD_4" };
        for (int n = 0; n < hid::PlayerSlots::kUsers; n++)
        {
            VirtualPad& vp = s_vpads[n];
            vp.number = n + 1;
            const char* v = std::getenv(vars[n]);
            if (!v || !*v)
                continue;
            // A test's window rarely has the keyboard focus (on the Mac it's
            // started from a shell behind other windows), and without it SDL
            // drops stick and trigger changes: the pad kept its first value.
            SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
            ParseVirtualPad(v, vp);
            if (!PlugVirtualPad(vp))
                continue;
            s_virtualOnly = true;
            if (n == 0)
                fprintf(stderr, "[input] virtual pad attached, %zu steps\n", vp.steps.size());
            else
                fprintf(stderr, "[input] virtual pad %d attached, %zu steps\n", vp.number, vp.steps.size());
        }
    }

    // Moves each virtual pad's axes and buttons due by now. taken[n]: pad
    // n + 1's steps taken ("" if none).
    void StepVirtualPads(std::string taken[hid::PlayerSlots::kUsers])
    {
        double t = SecondsSinceLaunch();
        bool any = false;
        for (int n = 0; n < hid::PlayerSlots::kUsers; n++)
        {
            VirtualPad& vp = s_vpads[n];
            uint32_t changed = 0;  // buttons changed this frame
            for (; vp.next < vp.steps.size() && vp.steps[vp.next].time <= t; vp.next++)
            {
                const PadStep& step = vp.steps[vp.next];
                if (step.button == kShotStep)
                {
                    ui::RequestShot();
                    taken[n] += (taken[n].empty() ? "" : " ") + step.text;
                    continue;
                }
                if (step.button == kDetachStep || step.button == kAttachStep)
                {
                    if (step.button == kDetachStep && vp.joystick)
                    {
                        SDL_CloseJoystick(vp.joystick);
                        vp.joystick = nullptr;
                        SDL_DetachVirtualJoystick(vp.id);
                        vp.id = 0;
                    }
                    else if (step.button == kAttachStep && !vp.joystick)
                        PlugVirtualPad(vp);
                    taken[n] += (taken[n].empty() ? "" : " ") + step.text;
                    any = true;
                    continue;
                }
                if (!vp.joystick)
                    continue;  // unplugged: its steps pass
                if (step.button >= 0)
                {
                    if (changed & (1u << step.button))
                        break;  // its next change waits a frame (the steps stay in order)
                    changed |= 1u << step.button;
                    SDL_SetJoystickVirtualButton(vp.joystick, step.button, step.down);
                    taken[n] += (taken[n].empty() ? "" : " ") + step.text;
                    any = true;
                    continue;
                }
                float value = step.value;
                bool trigger = step.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || step.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
                if (step.axis == SDL_GAMEPAD_AXIS_LEFTY || step.axis == SDL_GAMEPAD_AXIS_RIGHTY)
                    value = -value;  // SDL: down is positive
                // A virtual gamepad's triggers span the whole axis (-32768 is released).
                float raw = trigger ? std::clamp(value, 0.0f, 1.0f) * 65535.0f - 32768.0f : std::clamp(value, -1.0f, 1.0f) * 32767.0f;
                SDL_SetJoystickVirtualAxis(vp.joystick, int(step.axis), Sint16(std::lround(std::clamp(raw, -32768.0f, 32767.0f))));
                taken[n] += (taken[n].empty() ? "" : " ") + step.text;
                any = true;
            }
        }
        if (any)
            SDL_UpdateJoysticks();  // read back this frame
    }

    // The "no rumble over USB" note (hid/rumble_note.h): player 1's
    // controller as SDL reaches it, judged by Update once the note can show.
    struct RumbleNotePad
    {
        SDL_Gamepad* gamepad = nullptr;  // pending for this pad; null when decided
        char driver = 0;
        hid::RumbleNote::Connection connection = hid::RumbleNote::Connection::Unknown;
        uint16_t vendor = 0;
        std::string name;
    };
    RumbleNotePad s_rumbleNotePad;

    // Main thread, from Update: the note, shown or skipped, with a line
    // either way; it waits (silently) for the UI, no other panel, and the
    // controls idle 3 s (not mid-race), as the refresh-rate notice does.
    void MaybeRumbleNote(bool vibration)
    {
        using Note = hid::RumbleNote;
        RumbleNotePad& pad = s_rumbleNotePad;
        if (!pad.gamepad)
            return;
        if (pad.gamepad != s_gamepad)
        {
            pad.gamepad = nullptr;  // gone before it was judged (the next one is judged on its own)
            return;
        }
        Note::Inputs in;
#if defined(__APPLE__) && !TARGET_OS_IOS
        in.platform = Note::Platform::Mac;
#endif
        in.driver = pad.driver;
        in.connection = pad.connection;
        in.vendor = pad.vendor;
        in.name = pad.name;
        in.vibration = vibration;
        in.shownBefore = settings::RumbleNoteShown();
        const Note::Decision d = Note::Decide(in);
        if (!d.show)
        {
            pad.gamepad = nullptr;
            fprintf(stderr, "[input] rumble note for %s: not shown: %s\n", pad.name.c_str(), d.reason);
            return;
        }
        if (!ui::Ready() || ui::HasModal() || ui::SettingsOpen() || hid::SecondsSinceInput() < 3.0)
            return;
        if (!ui::ShowNotice(Note::kTitle, Note::kText, [](const char*) {
                if (settings::RecordRumbleNote())
                    fprintf(stderr, "[input] rumble note: recorded in settings.toml (not shown again)\n");
            }))
            return;
        pad.gamepad = nullptr;
        fprintf(stderr, "[input] rumble note for %s: shown (%s)\n", pad.name.c_str(), d.reason);
    }

    void OpenFirstGamepad()
    {
        if (s_gamepad)
            return;
        if (s_virtualOnly)
        {
            // Only the test's pad: a real controller nearby must not rumble.
            if (s_vpads[0].id)
                s_gamepad = SDL_OpenGamepad(s_vpads[0].id);
            if (s_gamepad)
                fprintf(stderr, "[input] gamepad: %s\n", SDL_GetGamepadName(s_gamepad));
            return;
        }
        int count = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&count);
        for (int i = 0; ids && i < count; i++)
        {
            // In split screen, never another player's controller (one that
            // isn't a player yet may take over player 1's).
            if (s_slots.UserOf(ids[i]) != 0)
                continue;
            // A wheel SDL happens to have a gamepad mapping for belongs to the
            // wheel, not to this path: one device, one owner (hid/wheel.h).
            if (hid::wheel::Owns(ids[i]))
                continue;
            auto other = std::find_if(s_others.begin(), s_others.end(), [&](const OtherPad& o) { return o.id == ids[i]; });
            if (other != s_others.end())
            {
                s_gamepad = other->gamepad;  // already open: player 1's now
                s_others.erase(other);
                s_slots.Remove(ids[i]);
            }
            else
                s_gamepad = SDL_OpenGamepad(ids[i]);
            if (s_gamepad)
            {
                // What it can do: a pad without motors takes rumble and ignores it.
                SDL_PropertiesID props = SDL_GetGamepadProperties(s_gamepad);
                // How SDL reaches it (a wired Xbox Series pad on a Mac takes no rumble while the
                // same pad over Bluetooth does): the GUID's driver byte ('h' HIDAPI, 'm' Apple's GameController,
                // 0 the system's HID), the connection and the device path.
                SDL_GUID guid = SDL_GetJoystickGUID(SDL_GetGamepadJoystick(s_gamepad));
                char guidText[33];
                SDL_GUIDToString(guid, guidText, sizeof(guidText));
                const char* path = SDL_GetGamepadPath(s_gamepad);
                SDL_JoystickConnectionState conn = SDL_GetGamepadConnectionState(s_gamepad);
                fprintf(stderr, "[input] gamepad: %s (rumble %s, trigger rumble %s; %s, driver '%c', guid %s, path %s)\n",
                    SDL_GetGamepadName(s_gamepad),
                    SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false) ? "yes" : "no",
                    SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, false) ? "yes" : "no",
                    conn == SDL_JOYSTICK_CONNECTION_WIRED ? "wired" : conn == SDL_JOYSTICK_CONNECTION_WIRELESS ? "wireless" : "connection unknown",
                    guid.data[14] ? char(guid.data[14]) : '0', guidText, path ? path : "none");
                const char* name = SDL_GetGamepadName(s_gamepad);
                s_rumbleNotePad = { s_gamepad, char(guid.data[14]),
                    conn == SDL_JOYSTICK_CONNECTION_WIRED ? hid::RumbleNote::Connection::Wired
                        : conn == SDL_JOYSTICK_CONNECTION_WIRELESS ? hid::RumbleNote::Connection::Wireless
                        : hid::RumbleNote::Connection::Unknown,
                    SDL_GetGamepadVendor(s_gamepad), name ? name : "" };
            }
            break;
        }
        SDL_free(ids);
    }

    // Split screen: open a controller as a possible other player (not player
    // 1's, not one already open; with virtual pads, only theirs).
    void OpenOtherPad(SDL_JoystickID id)
    {
        if (!id || hid::wheel::Owns(id) || (s_gamepad && SDL_GetGamepadID(s_gamepad) == id) || (s_virtualOnly && (!IsVirtualPad(id) || id == s_vpads[0].id)))
            return;
        if (std::any_of(s_others.begin(), s_others.end(), [&](const OtherPad& o) { return o.id == id; }))
            return;
        SDL_Gamepad* g = SDL_OpenGamepad(id);
        if (!g)
        {
            fprintf(stderr, "[input] split screen: controller %u didn't open: %s\n", unsigned(id), SDL_GetError());
            return;
        }
        s_others.push_back(OtherPad{ id, g });
        SDL_PropertiesID props = SDL_GetGamepadProperties(g);
        fprintf(stderr, "[input] split screen: %s open (rumble %s), a player once it's used\n", SDL_GetGamepadName(g),
            SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false) ? "yes" : "no");
    }

    void OpenOtherPads()
    {
        int count = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&count);
        for (int i = 0; ids && i < count; i++)
            OpenOtherPad(ids[i]);
        SDL_free(ids);
    }

    // A controller of s_others closed (removed, or split screen over): its
    // user, if it was one, reads not connected from now on.
    void CloseOtherPad(SDL_JoystickID id, const char* why)
    {
        auto it = std::find_if(s_others.begin(), s_others.end(), [&](const OtherPad& o) { return o.id == id; });
        if (it == s_others.end())
            return;
        const std::string name = SDL_GetGamepadName(it->gamepad) ? SDL_GetGamepadName(it->gamepad) : "a controller";
        SDL_RumbleGamepad(it->gamepad, 0, 0, 0);
        SDL_CloseGamepad(it->gamepad);
        s_others.erase(it);
        const int user = s_slots.Remove(id);
        if (user)
        {
            {
                std::lock_guard lock(s_mutex);
                s_users[user].connected = false;
                s_users[user].pad = Pad{};
            }
            fprintf(stderr, "[input] player %d's controller (%s, XInput user %d) %s: the game sees it disconnected\n", user + 1,
                name.c_str(), user, why);
        }
    }

    void CloseOtherPads(const char* why)
    {
        while (!s_others.empty())
            CloseOtherPad(s_others.back().id, why);
    }

#if defined(__APPLE__) && TARGET_OS_IOS
    // A phone or tablet has nothing to play the game with but a controller
    // (or a keyboard attached to it, which counts as on desktop), and a
    // player who has just installed it may not know. So a notice when there
    // is none: once as the game starts (not over the installer, which a
    // finger works), and again when the last one goes. Each waits a moment
    // and looks again, so a controller that iOS reports a little late (a
    // Bluetooth pad woken by its button after launch takes a few seconds),
    // or one that drops out and straight back, brings up nothing; and one
    // that turns up while the notice shows takes it down. Main thread;
    // guest ns, 0: none due.
    constexpr const char* kNeedsController = "The game needs a controller: an Xbox or PlayStation one over Bluetooth, or a Backbone.";
    constexpr const char* kControllerGone = "Controller disconnected: the game needs one (Xbox or PlayStation over Bluetooth, or a Backbone).";
    int64_t s_noControllerAt = 0;
    const char* s_noControllerText = nullptr;

    void NoControllerNoticeIn(int64_t ns, const char* text)
    {
        s_noControllerAt = guesttime::NowNs() + ns;
        s_noControllerText = text;
    }

    void NoControllerNotice()
    {
        if (!s_noControllerAt || guesttime::NowNs() < s_noControllerAt)
            return;
        s_noControllerAt = 0;
        // (NFSMW_INPUT_SCRIPT is the controller of an automated run.)
        if (s_gamepad || hid::wheel::Playing() || SDL_HasKeyboard() || !s_script.empty())
            return;
        fprintf(stderr, "[input] no controller: %s\n", s_noControllerText);
        ui::Toast(s_noControllerText, 10.0);
    }

    // Something to play with turned up: a notice still showing (or waiting
    // its turn) no longer holds.
    void WithdrawNoControllerNotice()
    {
        ui::WithdrawToast(kNeedsController);
        ui::WithdrawToast(kControllerGone);
    }
#endif
}

namespace
{
    const bool s_inputLog = std::getenv("NFSMW_INPUT_LOG") != nullptr;

    // NFSMW_INPUT_LOG=1: once a second, the left stick as the device reports
    // it (last value and range over the second) and as the game receives it.
    // A stick at rest must reach the game as exactly 0 (the drift check).
    // The triggers too (a worn one that never reads 0 holds the brake).
    void LogStick(float rawX, float rawY, float x, float y, float rawLt, float rawRt, uint8_t lt, uint8_t rt)
    {
        if (!s_inputLog)
            return;
        static float minX = 1, maxX = -1, minY = 1, maxY = -1;
        static auto last = std::chrono::steady_clock::now();
        minX = std::min(minX, rawX); maxX = std::max(maxX, rawX);
        minY = std::min(minY, rawY); maxY = std::max(maxY, rawY);
        auto now = std::chrono::steady_clock::now();
        if (now - last < std::chrono::seconds(1))
            return;
        // ...and how long the controls have been idle (the refresh-rate notice waits for 3 s of it).
        const double idle = s_inputSeen ? std::chrono::duration<double>(now - s_lastInput).count() : 0.0;
        fprintf(stderr, "[input] left stick raw (%+.4f, %+.4f) range x[%+.4f, %+.4f] y[%+.4f, %+.4f] -> game (%d, %d); "
                        "triggers raw (%.3f, %.3f) -> game (%u, %u); idle %.1f s\n",
            rawX, rawY, minX, maxX, minY, maxY, int(ToShort(x)), int(ToShort(y)), rawLt, rawRt, unsigned(lt), unsigned(rt), idle);
        minX = minY = 1;
        maxX = maxY = -1;
        last = now;
    }
}

namespace
{
    // Split screen, once a frame from Update (main thread): the mode (the
    // game's own predicate, hid/player_slots.h), the other controllers as
    // XInput users 1-3, and their rumble. Outside split screen nothing is
    // open and this returns at once: single player is v0.1.7's.
    void UpdateOtherPlayers(bool blocked, bool menuClosed, float deadzone, float triggerDeadzone, bool vibration,
        bool vibrationChanged, const std::string padTaken[hid::PlayerSlots::kUsers])
    {
        constexpr int kUsers = hid::PlayerSlots::kUsers;
        const int64_t now = guesttime::NowNs();
        const double t = SecondsSinceLaunch();
        const bool testSplit = std::any_of(s_testSplit.begin(), s_testSplit.end(), [&](const auto& r) { return t >= r.first && t < r.second; });
        const bool said = game::split_screen::Enabled() && (s_otherPadsAlways || testSplit || game::split_screen::GameSaysSplit());
        const bool split = s_slots.NoteSplit(said, now);
        if (s_slots.Changed())
        {
            if (split)
            {
                OpenOtherPads();
                fprintf(stderr, "[input] split screen%s: player 1 is the keyboard and %s; another controller is player 2 once it's "
                                "used (%zu other open)\n",
                    s_otherPadsAlways ? " (NFSMW_OTHER_PADS=always)" : " on (the game's Quick Race > Split Screen)",
                    s_gamepad && SDL_GetGamepadName(s_gamepad) ? SDL_GetGamepadName(s_gamepad) : "no controller", s_others.size());
            }
            else
            {
                CloseOtherPads("closed: split screen is over");
                {
                    // (The players were forgotten before their pads closed:
                    // every user reads not connected from here, explicitly.)
                    std::lock_guard lock(s_mutex);
                    for (int u = 1; u < kUsers; u++)
                    {
                        s_users[u].connected = false;
                        s_users[u].pad = Pad{};
                        s_users[u].rumbleLow = s_users[u].rumbleHigh = 0;
                    }
                }
                fprintf(stderr, "[input] split screen off: one controller again (XInput users 1-3 not connected)\n");
            }
        }
        // The notices: a player whose controller went (split screen only),
        // up until a controller takes its place again.
        if (s_gamepad || !split)
            s_p1Lost = false;
        uint32_t wanted = s_p1Lost ? 1u : 0u;
        for (int u = 1; u < kUsers; u++)
            if (s_slots.Missing(u))
                wanted |= 1u << u;
        for (int u = 0; u < kUsers; u++)
        {
            const uint32_t bit = 1u << u;
            if ((wanted & bit) == (s_noticesShown & bit))
                continue;
            const std::string text = MissingNotice(u);
            if (wanted & bit)
            {
                ui::Toast(text, 24.0 * 3600.0);  // until withdrawn
                fprintf(stderr, "[input] notice shown: %s\n", text.c_str());
            }
            else
            {
                ui::WithdrawToast(text);
                fprintf(stderr, "[input] notice withdrawn (player %d %s): %s\n", u + 1,
                    split ? "is back" : "gone with split screen", text.c_str());
            }
        }
        s_noticesShown = wanted;
        for (int n = 1; n < kUsers; n++)
            if (!padTaken[n].empty() && std::none_of(s_others.begin(), s_others.end(), [&](const OtherPad& o) { return o.id == s_vpads[n].id; }))
                fprintf(stderr, "[input] virtual pad %d %s (%s)\n", n + 1, padTaken[n].c_str(),
                    !s_vpads[n].id ? "unplugged" : split ? "not open" : "not open: not split screen");
        if (!split && s_others.empty())
            return;

        Pad staged[kUsers];
        for (OtherPad& o : s_others)
        {
            Pad p;
            for (auto& [button, bit] : kButtonMap)
                if (SDL_GetGamepadButton(o.gamepad, button))
                    p.buttons |= bit;  // (Back straight through: our menu's chord is player 1's)
            auto axis = [&](SDL_GamepadAxis a) { return float(SDL_GetGamepadAxis(o.gamepad, a)) / 32767.0f; };
            float lx = axis(SDL_GAMEPAD_AXIS_LEFTX), ly = -axis(SDL_GAMEPAD_AXIS_LEFTY);  // XInput: up is positive
            float rx = axis(SDL_GAMEPAD_AXIS_RIGHTX), ry = -axis(SDL_GAMEPAD_AXIS_RIGHTY);
            const float lt = axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER), rt = axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
            ApplyDeadzone(lx, ly, deadzone);
            ApplyDeadzone(rx, ry, deadzone);
            p.lx = ToShort(lx);
            p.ly = ToShort(ly);
            p.rx = ToShort(rx);
            p.ry = ToShort(ry);
            p.leftTrigger = Trigger(lt, triggerDeadzone);
            p.rightTrigger = Trigger(rt, triggerDeadzone);
            const int user = s_slots.NoteInput(o.id, p.buttons, lt > 0.5f || rt > 0.5f, now);
            if (user != o.user)
            {
                fprintf(stderr, "[input] player %d: %s is XInput user %d\n", user + 1,
                    SDL_GetGamepadName(o.gamepad) ? SDL_GetGamepadName(o.gamepad) : "a controller", user);
                SDL_SetGamepadPlayerIndex(o.gamepad, user);  // a DualSense's player lights
                o.user = user;
            }
            if (user && (p.buttons || p.leftTrigger || p.rightTrigger || p.lx || p.ly || p.rx || p.ry))
            {
                s_lastInput = std::chrono::steady_clock::now();
                s_inputSeen = true;
            }
            // A menu of ours open: the game sees idle pads, and after it
            // closes each button held stays hidden until released (Update's
            // rule for player 1).
            if (menuClosed)
                o.latched = p.buttons;
            o.latched &= p.buttons;
            if (blocked)
                p = Pad{};
            else
                p.buttons &= ~o.latched;
            if (user && s_slots.Arriving(user, now))
                p = Pad{};
            else if (user && !blocked)
                p.buttons = s_slots.Shown(user, p.buttons, now);  // (the press that made it a player, replayed whole)
            if (user)
                staged[user] = p;
            for (int n = 1; n < kUsers; n++)
                if (!padTaken[n].empty() && s_vpads[n].id == o.id)
                    fprintf(stderr, "[input] virtual pad %d %s: %s%s, the game gets buttons %04X, left stick (%d, %d), triggers (%u, %u)%s\n",
                        n + 1, padTaken[n].c_str(), user ? "XInput user " : "not a player yet", user ? std::to_string(user).c_str() : "",
                        unsigned(p.buttons), int(p.lx), int(p.ly), unsigned(p.leftTrigger), unsigned(p.rightTrigger),
                        blocked ? " (a menu is open)" : user && s_slots.Arriving(user, now) ? " (arriving: idle for now)" : "");
        }

        uint16_t low[kUsers] = {}, high[kUsers] = {};
        bool dirty[kUsers] = {};
        {
            std::lock_guard lock(s_mutex);
            for (int u = 1; u < kUsers; u++)
            {
                OtherUser& ou = s_users[u];
                ou.connected = s_slots.Connected(u);
                ou.pad = ou.connected ? staged[u] : Pad{};
                if (!ou.connected)
                    ou.rumbleLow = ou.rumbleHigh = 0;  // the next player's starts still
                dirty[u] = ou.rumbleDirty || vibrationChanged;
                ou.rumbleDirty = false;
                low[u] = ou.rumbleLow;
                high[u] = ou.rumbleHigh;
            }
        }
        for (OtherPad& o : s_others)
            if (o.user && dirty[o.user])
            {
                const uint16_t l = vibration ? low[o.user] : 0, h = vibration ? high[o.user] : 0;
                if (s_virtualOnly)
                {
                    static uint64_t logged[kUsers] = { ~0ull, ~0ull, ~0ull, ~0ull };
                    const uint64_t key = uint64_t(low[o.user]) << 17 | uint64_t(high[o.user]) << 1 | (vibration ? 1u : 0u);
                    if (key != logged[o.user])
                    {
                        logged[o.user] = key;
                        fprintf(stderr, "[input] rumble: user %d: game low %u, high %u; Vibration %s\n", o.user, unsigned(low[o.user]),
                            unsigned(high[o.user]), vibration ? "on" : "off (the controller gets 0)");
                    }
                }
                SDL_RumbleGamepad(o.gamepad, l, h, l || h ? 5000 : 0);
            }
    }
}

namespace hid
{
    void Initialize()
    {
        if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD))
            fprintf(stderr, "[input] SDL gamepad init failed: %s\n", SDL_GetError());
        fprintf(stderr, "[input] stick deadzone %.2f, trigger deadzone %.2f\n", settings::GetFloat(settings::Id::StickDeadzone),
            settings::GetFloat(settings::Id::TriggerDeadzone));
        AttachVirtualPad();
        // A steering wheel (hid/wheel.h), unless this run has the test's
        // own controller: then that is the only device.
        if (!s_virtualOnly)
            wheel::Initialize();
        OpenFirstGamepad();
        std::lock_guard lock(s_mutex);
        s_deviceConnected = true;  // the keyboard always counts as controller 0
    }

    void NoteGameStarting()
    {
#if defined(__APPLE__) && TARGET_OS_IOS
        NoControllerNoticeIn(5'000'000'000, kNeedsController);
#endif
    }

    void HandleEvent(const SDL_Event& e)
    {
        wheel::HandleEvent(e);  // a wheel coming or going
        if (e.type == SDL_EVENT_GAMEPAD_ADDED)
        {
            OpenFirstGamepad();
            if (s_slots.Split())
                OpenOtherPad(e.gdevice.which);  // (unless it just became player 1's)
#if defined(__APPLE__) && TARGET_OS_IOS
            if (s_gamepad || wheel::Playing())
                WithdrawNoControllerNotice();
#endif
        }
        else if (e.type == SDL_EVENT_GAMEPAD_REMOVED && s_gamepad && e.gdevice.which == SDL_GetGamepadID(s_gamepad))
        {
            SDL_CloseGamepad(s_gamepad);
            s_gamepad = nullptr;
            fprintf(stderr, "[input] gamepad removed\n");
            OpenFirstGamepad();
            if (!s_gamepad && s_slots.Split())
                s_p1Lost = true;  // (Update's notices)
#if defined(__APPLE__) && TARGET_OS_IOS
            if (!s_gamepad && !wheel::Playing())
                NoControllerNoticeIn(1'000'000'000, kControllerGone);
#endif
        }
        else if (e.type == SDL_EVENT_GAMEPAD_REMOVED)
            CloseOtherPad(e.gdevice.which, "removed");  // (split screen: another player's)
#if defined(__APPLE__) && TARGET_OS_IOS
        else if (e.type == SDL_EVENT_KEYBOARD_ADDED)
            WithdrawNoControllerNotice();  // (a keyboard counts as a controller here)
#endif
    }

    void Shutdown()
    {
        wheel::Shutdown();
        if (s_gamepad)
        {
            SDL_RumbleGamepad(s_gamepad, 0, 0, 0);
            SDL_CloseGamepad(s_gamepad);
            s_gamepad = nullptr;
        }
        for (OtherPad& o : s_others)
        {
            SDL_RumbleGamepad(o.gamepad, 0, 0, 0);
            SDL_CloseGamepad(o.gamepad);
        }
        s_others.clear();
    }

    bool IsOtherPlayersPad(SDL_JoystickID id)
    {
        return s_slots.UserOf(id) != 0;
    }

    void Suspend()
    {
        // Update sends a rumble for 5 s at a time and doesn't run meanwhile:
        // stop it now rather than buzz on for a game that stands still.
        if (s_gamepad)
            SDL_RumbleGamepad(s_gamepad, 0, 0, 0);
        for (OtherPad& o : s_others)
            SDL_RumbleGamepad(o.gamepad, 0, 0, 0);
        wheel::Suspend();
    }

    void Resume()
    {
        // The next Update sends the game's last request again, as the
        // Vibration setting allows.
        std::lock_guard lock(s_mutex);
        s_rumbleDirty = true;
        for (OtherUser& ou : s_users)
            ou.rumbleDirty = true;
    }

    double SecondsSinceInput()
    {
        if (!s_inputSeen)
            return 0.0;
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - s_lastInput).count();
    }

    void SetBlocked(bool blocked)
    {
        s_blocked.store(blocked, std::memory_order_relaxed);
    }

    void Update()
    {
#if defined(__APPLE__) && TARGET_OS_IOS
        NoControllerNotice();
#endif
        Pad p;
        float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
        bool padBack = false;  // the controller's Back, as the device reports it
        float rawX = 0, rawY = 0;  // the left stick before its deadzone (NFSMW_INPUT_LOG)
        std::string padTaken[hid::PlayerSlots::kUsers];
        StepVirtualPads(padTaken);
        const std::string& padSteps = padTaken[0];
        const float deadzone = settings::GetFloat(settings::Id::StickDeadzone);
        const float triggerDeadzone = settings::GetFloat(settings::Id::TriggerDeadzone);
        // A steering wheel first, while one is plugged in and Settings >
        // Wheel has it on: it brings its own steering curve and its pedals
        // their own calibration (hid/wheel_map.h), so the stick's radial
        // deadzone has nothing to do here. Its paddles push the right stick
        // up and down, which is how the game shifts gears.
        wheel::Reading wheelRead;
        const bool onWheel = wheel::Sample(wheelRead);
        bool lxFromWheel = onWheel;  // ...unless the keyboard sets it below
        if (onWheel)
        {
            p.buttons = uint16_t(wheelRead.buttons & ~uint16_t(XAMINPUT_GAMEPAD_BACK));
            padBack = wheelRead.back;
            if (PadBackForGame(padBack, wheelRead.start))
                p.buttons |= XAMINPUT_GAMEPAD_BACK;
            lx = wheelRead.stick;  // past the game's own steering deadzone (wheel_map.h)
            ry = float(wheelRead.rightY) / 32767.0f;
            lt = wheelRead.brake;
            rt = wheelRead.throttle;
            rawX = wheelRead.steer;  // NFSMW_INPUT_LOG: the wheel's steering, then the stick the game gets
            rawY = ly;
        }
        else if (s_gamepad)
        {
            for (auto& [button, bit] : kButtonMap)
                if (bit != XAMINPUT_GAMEPAD_BACK && SDL_GetGamepadButton(s_gamepad, button))
                    p.buttons |= bit;
            padBack = SDL_GetGamepadButton(s_gamepad, SDL_GAMEPAD_BUTTON_BACK);
            if (PadBackForGame(padBack, SDL_GetGamepadButton(s_gamepad, SDL_GAMEPAD_BUTTON_START)))
                p.buttons |= XAMINPUT_GAMEPAD_BACK;
            auto axis = [](SDL_Gamepad* g, SDL_GamepadAxis a) { return float(SDL_GetGamepadAxis(g, a)) / 32767.0f; };
            lx = axis(s_gamepad, SDL_GAMEPAD_AXIS_LEFTX);
            ly = -axis(s_gamepad, SDL_GAMEPAD_AXIS_LEFTY);  // XInput: up is positive
            rx = axis(s_gamepad, SDL_GAMEPAD_AXIS_RIGHTX);
            ry = -axis(s_gamepad, SDL_GAMEPAD_AXIS_RIGHTY);
            lt = axis(s_gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
            rt = axis(s_gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
            rawX = lx;
            rawY = ly;
            ApplyDeadzone(lx, ly, deadzone);
            ApplyDeadzone(rx, ry, deadzone);
        }
        const float rawLt = lt, rawRt = rt;

        // Keyboard (digital; replaces the controller's value for what it
        // touches): each control's key is Settings > Controls (the Key
        // options), resolved by key_map.h. A key is a scancode SDL's state
        // array is indexed by; an out-of-range value (a hand edit beat
        // Load's checks) counts as not held.
        if (const bool* k = SDL_GetKeyboardState(nullptr))
        {
            bool pressed[keymap::kControls] = {};
            auto down = [&](settings::Id id) {
                int code = settings::GetInt(id);  // 0: no key
                return code > 0 && code < SDL_SCANCODE_COUNT && k[code];
            };
            for (size_t i = 0; i < keymap::kControls; i++)
                pressed[i] = down(keymap::kEntries[i].id) || down(keymap::kEntries[i].id2);
            keymap::Resolved r = keymap::Resolve(pressed);
            p.buttons |= r.buttons;
            if (r.rightTrigger)
                rt = 1.0f;
            if (r.leftTrigger)
                lt = 1.0f;
            float* axes[size_t(keymap::Axis::Count)] = { &lx, &ly, &rx, &ry };  // Axis order: LX, LY, RX, RY
            for (size_t a = 0; a < size_t(keymap::Axis::Count); a++)
                if (r.axisSet[a])
                    *axes[a] = r.axis[a];
            if (r.axisSet[size_t(keymap::Axis::LX)])
                lxFromWheel = false;
        }
        p.lx = ToShort(lx);
        p.ly = ToShort(ly);
        p.rx = ToShort(rx);
        p.ry = ToShort(ry);
        p.leftTrigger = Trigger(lt, triggerDeadzone);
        p.rightTrigger = Trigger(rt, triggerDeadzone);
        if (s_gamepad || onWheel)
            LogStick(rawX, rawY, lx, ly, rawLt, rawRt, p.leftTrigger, p.rightTrigger);
        if (!padSteps.empty())
            fprintf(stderr, "[input] virtual pad %s: left stick raw (%+.4f, %+.4f) -> game (%d, %d), right (%d, %d); "
                            "triggers raw (%.3f, %.3f) -> game (%u, %u); deadzones: stick %.2f, trigger %.2f%s\n",
                padSteps.c_str(), rawX, rawY, int(p.lx), int(p.ly), int(p.rx), int(p.ry), rawLt, rawRt,
                unsigned(p.leftTrigger), unsigned(p.rightTrigger), deadzone, triggerDeadzone,
                s_blocked.load(std::memory_order_relaxed) ? " (a menu is open: the game sees an idle pad)" : "");

        // The last time anything was pressed or moved, the script's taps
        // included (SecondsSinceInput).
        {
            Pad scripted = p;
            ApplyScript(scripted);
            // A wheel's steering counts when it turns, not for resting a little
            // off its centre: without a deadzone it is rarely exactly 0, and the
            // controls never read idle with a wheel plugged in.
            const bool wheelTurned = onWheel && s_wheelStill.Turned(wheelRead.steer);
            if (lxFromWheel && !wheelTurned && scripted.lx == p.lx)
                scripted.lx = 0;
            if (scripted.buttons || scripted.leftTrigger || scripted.rightTrigger || scripted.lx || scripted.ly || scripted.rx ||
                scripted.ry || !s_inputSeen)
            {
                s_lastInput = std::chrono::steady_clock::now();
                s_inputSeen = true;
            }
        }

        // While a menu has the input the game sees an idle pad; after it
        // closes, each button still held stays hidden until released, so the
        // press that closed the menu (B, Start, Esc) doesn't reach the game.
        bool blocked = s_blocked.load(std::memory_order_relaxed);
        const bool menuClosed = s_wasBlocked && !blocked;
        if (s_wasBlocked && !blocked)
            s_latchedButtons = p.buttons;
        // A Back pressed in the menu (or the one that closed it) is never replayed.
        if ((blocked || s_wasBlocked) && padBack)
            s_backState = BackState::Swallow;
        if (blocked != s_wasBlocked)
            wheel::NoteMenu(blocked);  // a wheel's state in the log at every menu (nothing without one)
        s_wasBlocked = blocked;
        s_latchedButtons &= p.buttons;
        if (blocked)
            p = Pad{};
        else
            p.buttons &= ~s_latchedButtons;

        uint16_t low, high;
        bool rumble;
        {
            std::lock_guard lock(s_mutex);
            s_pad = p;
            s_scriptSuppressed = blocked;
            rumble = s_rumbleDirty;
            s_rumbleDirty = false;
            low = s_rumbleLow;
            high = s_rumbleHigh;
        }
        static bool vibration = true;
        bool vibrationChanged = false;
        if (bool on = settings::GetBool(settings::Id::Vibration); on != vibration)
        {
            vibration = on;
            rumble = true;  // stop (or restart) a rumble under way
            vibrationChanged = true;
        }
        if (rumble && s_vpads[0].joystick)
        {
            // What the game asked for and what the controller gets, on
            // change. Only with the test's pad: while driving the game
            // changes its request every frame, which would be a line a frame
            // in a player's last-run.log (speedbreaker.sh sets NFSMW_INPUT_LOG).
            static uint64_t logged = ~0ull;
            uint64_t now = uint64_t(low) << 17 | uint64_t(high) << 1 | (vibration ? 1u : 0u);
            if (now != logged)
            {
                logged = now;
                fprintf(stderr, "[input] rumble: game low %u, high %u; Vibration %s%s\n", unsigned(low), unsigned(high),
                    vibration ? "on" : "off (the controller gets 0)", s_gamepad ? "" : "; no controller");
            }
        }
        MaybeRumbleNote(vibration);
        if (!vibration)
            low = high = 0;
        if (rumble && s_gamepad && !SDL_RumbleGamepad(s_gamepad, low, high, low || high ? 5000 : 0))
        {
            // A pad that says it has motors but refuses them: once per session, with SDL's reason.
            static bool told = false;
            if (!told)
            {
                told = true;
                fprintf(stderr, "[input] rumble refused by %s: %s\n", SDL_GetGamepadName(s_gamepad), SDL_GetError());
            }
        }
        // The wheel's own force feedback: every frame, since Settings >
        // Wheel > Force Feedback and Centering Force can change under it
        // without the game asking for anything new.
        wheel::Rumble(low, high, vibration);
        wheel::Update();

        UpdateOtherPlayers(blocked, menuClosed, deadzone, triggerDeadzone, vibration, vibrationChanged, padTaken);
    }
}

namespace
{
    // The pad as the game sees it: devices plus scripted taps. Caller holds s_mutex.
    Pad CurrentPadLocked()
    {
        Pad p = s_pad;
        if (!s_scriptSuppressed)
            ApplyScript(p);
        return p;
    }

    // Keystrokes (XamInputGetKeystroke[Ex]): edges of buttons, triggers and
    // stick directions as XInput VK_PAD_* codes, with auto-repeat for the
    // navigation keys, generated on demand from the same state as GetState.
    struct KeyDef { uint16_t vk; bool repeats; bool (*down)(const Pad&); };
    const KeyDef kKeys[] = {
        { 0x5800, false, [](const Pad& p) { return (p.buttons & XAMINPUT_GAMEPAD_A) != 0; } },
        { 0x5801, false, [](const Pad& p) { return (p.buttons & XAMINPUT_GAMEPAD_B) != 0; } },
        { 0x5802, false, [](const Pad& p) { return (p.buttons & XAMINPUT_GAMEPAD_X) != 0; } },
        { 0x5803, false, [](const Pad& p) { return (p.buttons & XAMINPUT_GAMEPAD_Y) != 0; } },
        { 0x5804, false, [](const Pad& p) { return (p.buttons & XAMINPUT_GAMEPAD_RIGHT_SHOULDER) != 0; } },
        { 0x5805, false, [](const Pad& p) { return (p.buttons & XAMINPUT_GAMEPAD_LEFT_SHOULDER) != 0; } },
        { 0x5806, false, [](const Pad& p) { return p.leftTrigger > 30; } },
        { 0x5807, false, [](const Pad& p) { return p.rightTrigger > 30; } },
        { 0x5810, true, [](const Pad& p) { return (p.buttons & XAMINPUT_GAMEPAD_DPAD_UP) != 0; } },
        { 0x5811, true, [](const Pad& p) { return (p.buttons & XAMINPUT_GAMEPAD_DPAD_DOWN) != 0; } },
        { 0x5812, true, [](const Pad& p) { return (p.buttons & XAMINPUT_GAMEPAD_DPAD_LEFT) != 0; } },
        { 0x5813, true, [](const Pad& p) { return (p.buttons & XAMINPUT_GAMEPAD_DPAD_RIGHT) != 0; } },
        { 0x5814, false, [](const Pad& p) { return (p.buttons & XAMINPUT_GAMEPAD_START) != 0; } },
        { 0x5815, false, [](const Pad& p) { return (p.buttons & XAMINPUT_GAMEPAD_BACK) != 0; } },
        { 0x5816, false, [](const Pad& p) { return (p.buttons & XAMINPUT_GAMEPAD_LEFT_THUMB) != 0; } },
        { 0x5817, false, [](const Pad& p) { return (p.buttons & XAMINPUT_GAMEPAD_RIGHT_THUMB) != 0; } },
        { 0x5820, true, [](const Pad& p) { return p.ly > 16384; } },
        { 0x5821, true, [](const Pad& p) { return p.ly < -16384; } },
        { 0x5822, true, [](const Pad& p) { return p.lx > 16384; } },
        { 0x5823, true, [](const Pad& p) { return p.lx < -16384; } },
        { 0x5830, true, [](const Pad& p) { return p.ry > 16384; } },
        { 0x5831, true, [](const Pad& p) { return p.ry < -16384; } },
        { 0x5832, true, [](const Pad& p) { return p.rx > 16384; } },
        { 0x5833, true, [](const Pad& p) { return p.rx < -16384; } },
    };
    constexpr size_t kKeyCount = sizeof(kKeys) / sizeof(kKeys[0]);
    bool s_keyDown[kKeyCount] = {};
    // Guest ns: a key held through a suspension doesn't repeat as soon as it's back.
    int64_t s_keyRepeatAt[kKeyCount] = {};

    struct Keystroke
    {
        be<uint16_t> virtualKey;
        be<uint16_t> unicode;
        be<uint16_t> flags;   // 1 key down, 2 key up, 4 repeat
        uint8_t userIndex;
        uint8_t hidCode;
    };
    static_assert(sizeof(Keystroke) == 8);

#ifndef ERROR_EMPTY
    constexpr uint32_t ERROR_EMPTY = 0x10D2;
#endif

    uint32_t NextKeystroke(Keystroke* out)
    {
        std::lock_guard lock(s_mutex);
        if (!(s_deviceConnected || !s_script.empty()))
            return ERROR_DEVICE_NOT_CONNECTED;
        Pad p = CurrentPadLocked();
        int64_t now = guesttime::NowNs();
        auto emit = [&](size_t i, uint16_t flags) {
            out->virtualKey = kKeys[i].vk;
            out->unicode = 0;
            out->flags = flags;
            out->userIndex = 0;
            out->hidCode = 0;
            return ERROR_SUCCESS;
        };
        for (size_t i = 0; i < kKeyCount; i++)
        {
            bool down = kKeys[i].down(p);
            if (down != s_keyDown[i])
            {
                s_keyDown[i] = down;
                s_keyRepeatAt[i] = now + 400'000'000;
                return emit(i, down ? 1 : 2);
            }
        }
        for (size_t i = 0; i < kKeyCount; i++)
            if (s_keyDown[i] && kKeys[i].repeats && now >= s_keyRepeatAt[i])
            {
                s_keyRepeatAt[i] = now + 100'000'000;
                return emit(i, 1 | 4);
            }
        return ERROR_EMPTY;
    }
}

// Users 1-3: the other players' controllers, connected only in split screen
// (hid/player_slots.h); user 0 and 0xFF ("any"): player 1, as before.
namespace
{
    bool OtherUserConnected(uint32_t userIndex)
    {
        std::lock_guard lock(s_mutex);
        return s_users[userIndex].connected;
    }
}

uint32_t XamInputGetCapabilities(uint32_t userIndex, uint32_t flags, XAMINPUT_CAPABILITIES* caps)
{
    bool scripted = !s_script.empty();
    if (userIndex >= 1 && userIndex < uint32_t(hid::PlayerSlots::kUsers))
    {
        if (!OtherUserConnected(userIndex))
            return ERROR_DEVICE_NOT_CONNECTED;
    }
    else
    {
        std::lock_guard lock(s_mutex);
        if ((userIndex != 0 && userIndex != 0xFF) || !(s_deviceConnected || scripted))
            return ERROR_DEVICE_NOT_CONNECTED;
    }
    memset(caps, 0, sizeof(*caps));
    caps->Type = XAMINPUT_DEVTYPE_GAMEPAD;
    caps->SubType = XAMINPUT_DEVSUBTYPE_GAMEPAD;
    caps->Gamepad.wButtons = ByteSwap(uint16_t(0xF3FF));
    caps->Gamepad.bLeftTrigger = 0xFF;
    caps->Gamepad.bRightTrigger = 0xFF;
    for (int16_t* v : { &caps->Gamepad.sThumbLX, &caps->Gamepad.sThumbLY, &caps->Gamepad.sThumbRX, &caps->Gamepad.sThumbRY })
        *v = ByteSwap(int16_t(0xFFC0));
    caps->Vibration.wLeftMotorSpeed = 0xFFFF;
    caps->Vibration.wRightMotorSpeed = 0xFFFF;
    return ERROR_SUCCESS;
}

uint32_t XamInputGetState(uint32_t userIndex, uint32_t flags, XAMINPUT_STATE* state)
{
    Pad p;
    uint32_t packet;
    if (userIndex >= 1 && userIndex < uint32_t(hid::PlayerSlots::kUsers))
    {
        std::lock_guard lock(s_mutex);
        OtherUser& ou = s_users[userIndex];
        if (ou.connected != ou.told)
        {
            ou.told = ou.connected;
            fprintf(stderr, "[input] the game sees XInput user %u %s\n", userIndex, ou.connected ? "connected" : "not connected");
        }
        if (!ou.connected)
            return ERROR_DEVICE_NOT_CONNECTED;
        p = ou.pad;
        if (memcmp(&p, &ou.last, sizeof(p)) != 0)
        {
            ou.packet++;
            ou.last = p;
        }
        packet = ou.packet;
    }
    else
    {
        std::lock_guard lock(s_mutex);
        if ((userIndex != 0 && userIndex != 0xFF) || !(s_deviceConnected || !s_script.empty()))
            return ERROR_DEVICE_NOT_CONNECTED;
        p = CurrentPadLocked();
        static Pad last;
        if (memcmp(&p, &last, sizeof(p)) != 0)
        {
            s_packet++;
            last = p;
        }
        packet = s_packet;
    }
    state->dwPacketNumber = ByteSwap(packet);
    state->Gamepad.wButtons = ByteSwap(p.buttons);
    state->Gamepad.bLeftTrigger = p.leftTrigger;
    state->Gamepad.bRightTrigger = p.rightTrigger;
    state->Gamepad.sThumbLX = ByteSwap(p.lx);
    state->Gamepad.sThumbLY = ByteSwap(p.ly);
    state->Gamepad.sThumbRX = ByteSwap(p.rx);
    state->Gamepad.sThumbRY = ByteSwap(p.ry);
    return ERROR_SUCCESS;
}

uint32_t XamInputSetState(uint32_t userIndex, uint32_t flags, XAMINPUT_VIBRATION* vibration)
{
    std::lock_guard lock(s_mutex);
    if (userIndex >= 1 && userIndex < uint32_t(hid::PlayerSlots::kUsers))
    {
        OtherUser& ou = s_users[userIndex];
        if (!ou.connected)
            return ERROR_DEVICE_NOT_CONNECTED;
        ou.rumbleLow = ByteSwap(vibration->wLeftMotorSpeed);
        ou.rumbleHigh = ByteSwap(vibration->wRightMotorSpeed);
        ou.rumbleDirty = true;
        return ERROR_SUCCESS;
    }
    if ((userIndex != 0 && userIndex != 0xFF) || !(s_deviceConnected || !s_script.empty()))
        return ERROR_DEVICE_NOT_CONNECTED;
    s_rumbleLow = ByteSwap(vibration->wLeftMotorSpeed);
    s_rumbleHigh = ByteSwap(vibration->wRightMotorSpeed);
    s_rumbleDirty = true;
    return ERROR_SUCCESS;
}

// Xenia's semantics: user 0xFF ("any") is pinned to user 0; queries for
// non-gamepad devices report none connected.
uint32_t XamInputGetKeystroke(uint32_t userIndex, uint32_t flags, Keystroke* keystroke)
{
    if (!keystroke)
        return 0xA0;  // ERROR_BAD_ARGUMENTS
    if ((flags & 0xFF) && (flags & 1) == 0)
        return ERROR_DEVICE_NOT_CONNECTED;
    if ((userIndex & 0xFF) != 0xFF && userIndex != 0 && !(flags & 0x40000000))
        return ERROR_DEVICE_NOT_CONNECTED;
    return NextKeystroke(keystroke);
}

uint32_t XamInputGetKeystrokeEx(be<uint32_t>* userIndex, uint32_t flags, Keystroke* keystroke)
{
    uint32_t result = XamInputGetKeystroke(userIndex ? uint32_t(*userIndex) : 0xFF, flags, keystroke);
    if (result == ERROR_SUCCESS && userIndex)
        *userIndex = 0;
    return result;
}

GUEST_FUNCTION_HOOK(__imp__XamInputGetCapabilities, XamInputGetCapabilities);
GUEST_FUNCTION_HOOK(__imp__XamInputGetState, XamInputGetState);
GUEST_FUNCTION_HOOK(__imp__XamInputSetState, XamInputSetState);
GUEST_FUNCTION_HOOK(__imp__XamInputGetKeystrokeEx, XamInputGetKeystrokeEx);
