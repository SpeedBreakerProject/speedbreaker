// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See input.h.
#include <stdafx.h>
#include "input.h"
#include <cpu/guest_time.h>
#include <kernel/function.h>
#include <ui/ui.h>
#include <user/settings.h>

#include <SDL3/SDL.h>

namespace
{
    constexpr uint32_t ERROR_SUCCESS = 0;
    constexpr uint32_t ERROR_DEVICE_NOT_CONNECTED = 0x48F;

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
    // Main thread: buttons held when a menu closed, hidden from the game
    // until each is released (the B, Start or A that closed it).
    uint16_t s_latchedButtons = 0;
    bool s_wasBlocked = false;
    bool s_scriptSuppressed = false;  // under s_mutex: no scripted input while a menu is open

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
    struct ScriptTap { double time; uint16_t buttons; int16_t lx, ly; double duration = 0.15; uint8_t lt = 0, rt = 0; };
    std::vector<ScriptTap> s_script;
    // Guest ns at launch: the script and the virtual pad run on guest time,
    // so a press due while the game is suspended comes after it instead of
    // being lost.
    const int64_t s_start = guesttime::NowNs();

    double SecondsSinceLaunch()
    {
        return double(guesttime::NowNs() - s_start) / 1e9;
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
        for (const auto& tap : s_script)
            if (t >= tap.time && t < tap.time + tap.duration)
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
    // LT/RT 0..1) and hold them. NFSMW_INPUT_SCRIPT feeds the game after the
    // device path; this goes through it, so the deadzones and Vibration
    // reach it as they would a real controller, for tests without one. Each
    // step logs what the game then gets; the rumble the game asks for and
    // the rumble the pad receives are logged.
    struct PadStep { double time; SDL_GamepadAxis axis; float value; std::string text; };
    std::vector<PadStep> s_padSteps;
    size_t s_padNext = 0;
    SDL_JoystickID s_virtualPadId = 0;
    SDL_Joystick* s_virtualPad = nullptr;

    bool SDLCALL VirtualPadRumble(void*, Uint16 low, Uint16 high)
    {
        fprintf(stderr, "[input] virtual pad: rumble low %u, high %u\n", unsigned(low), unsigned(high));
        return true;
    }

    void AttachVirtualPad()
    {
        const char* v = std::getenv("NFSMW_VIRTUAL_PAD");
        if (!v || !*v)
            return;
        // A test's window rarely has the keyboard focus (on the Mac it's
        // started from a shell behind other windows), and without it SDL
        // drops stick and trigger changes: the pad kept its first value.
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        static const std::pair<const char*, SDL_GamepadAxis> axes[] = {
            { "LX", SDL_GAMEPAD_AXIS_LEFTX }, { "LY", SDL_GAMEPAD_AXIS_LEFTY }, { "RX", SDL_GAMEPAD_AXIS_RIGHTX },
            { "RY", SDL_GAMEPAD_AXIS_RIGHTY }, { "LT", SDL_GAMEPAD_AXIS_LEFT_TRIGGER }, { "RT", SDL_GAMEPAD_AXIS_RIGHT_TRIGGER },
        };
        for (std::string_view rest = v; !rest.empty();)
        {
            size_t comma = rest.find(',');
            std::string item(rest.substr(0, comma));
            rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
            size_t colon = item.find(':'), equals = item.find('=');
            if (colon == std::string::npos || equals == std::string::npos || equals < colon)
                continue;
            std::string name = item.substr(colon + 1, equals - colon - 1);
            bool found = false;
            for (auto& [n, axis] : axes)
                if (name == n)
                {
                    s_padSteps.push_back({ std::atof(item.c_str()), axis, float(std::atof(item.c_str() + equals + 1)), item.substr(colon + 1) });
                    found = true;
                }
            if (!found)
                fprintf(stderr, "[input] NFSMW_VIRTUAL_PAD: unknown axis in \"%s\"\n", item.c_str());
        }
        std::stable_sort(s_padSteps.begin(), s_padSteps.end(), [](const PadStep& a, const PadStep& b) { return a.time < b.time; });
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.name = "SpeedBreaker virtual pad";
        desc.Rumble = VirtualPadRumble;
        s_virtualPadId = SDL_AttachVirtualJoystick(&desc);
        s_virtualPad = s_virtualPadId ? SDL_OpenJoystick(s_virtualPadId) : nullptr;
        if (!s_virtualPad)
        {
            fprintf(stderr, "[input] NFSMW_VIRTUAL_PAD: no virtual controller: %s\n", SDL_GetError());
            return;
        }
        fprintf(stderr, "[input] virtual pad attached, %zu steps\n", s_padSteps.size());
    }

    // Moves the virtual pad's axes due by now. Returns the steps taken ("" if none).
    std::string StepVirtualPad()
    {
        if (!s_virtualPad || s_padNext >= s_padSteps.size())
            return {};
        double t = SecondsSinceLaunch();
        std::string taken;
        for (; s_padNext < s_padSteps.size() && s_padSteps[s_padNext].time <= t; s_padNext++)
        {
            const PadStep& step = s_padSteps[s_padNext];
            float value = step.value;
            bool trigger = step.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || step.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
            if (step.axis == SDL_GAMEPAD_AXIS_LEFTY || step.axis == SDL_GAMEPAD_AXIS_RIGHTY)
                value = -value;  // SDL: down is positive
            // A virtual gamepad's triggers span the whole axis (-32768 is released).
            float raw = trigger ? std::clamp(value, 0.0f, 1.0f) * 65535.0f - 32768.0f : std::clamp(value, -1.0f, 1.0f) * 32767.0f;
            SDL_SetJoystickVirtualAxis(s_virtualPad, int(step.axis), Sint16(std::lround(std::clamp(raw, -32768.0f, 32767.0f))));
            taken += (taken.empty() ? "" : " ") + step.text;
        }
        if (!taken.empty())
            SDL_UpdateJoysticks();  // read back this frame
        return taken;
    }

    void OpenFirstGamepad()
    {
        if (s_gamepad)
            return;
        if (s_virtualPad)
        {
            // Only the test's pad: a real controller nearby must not rumble.
            s_gamepad = SDL_OpenGamepad(s_virtualPadId);
            if (s_gamepad)
                fprintf(stderr, "[input] gamepad: %s\n", SDL_GetGamepadName(s_gamepad));
            return;
        }
        int count = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&count);
        if (ids && count > 0)
        {
            s_gamepad = SDL_OpenGamepad(ids[0]);
            if (s_gamepad)
            {
                // What it can do: a pad without motors takes rumble and ignores it.
                SDL_PropertiesID props = SDL_GetGamepadProperties(s_gamepad);
                fprintf(stderr, "[input] gamepad: %s (rumble %s, trigger rumble %s)\n", SDL_GetGamepadName(s_gamepad),
                    SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false) ? "yes" : "no",
                    SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, false) ? "yes" : "no");
            }
        }
        SDL_free(ids);
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
        if (s_gamepad || SDL_HasKeyboard() || !s_script.empty())
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
        fprintf(stderr, "[input] left stick raw (%+.4f, %+.4f) range x[%+.4f, %+.4f] y[%+.4f, %+.4f] -> game (%d, %d); "
                        "triggers raw (%.3f, %.3f) -> game (%u, %u)\n",
            rawX, rawY, minX, maxX, minY, maxY, int(ToShort(x)), int(ToShort(y)), rawLt, rawRt, unsigned(lt), unsigned(rt));
        minX = minY = 1;
        maxX = maxY = -1;
        last = now;
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
        if (e.type == SDL_EVENT_GAMEPAD_ADDED)
        {
            OpenFirstGamepad();
#if defined(__APPLE__) && TARGET_OS_IOS
            if (s_gamepad)
                WithdrawNoControllerNotice();
#endif
        }
        else if (e.type == SDL_EVENT_GAMEPAD_REMOVED && s_gamepad && e.gdevice.which == SDL_GetGamepadID(s_gamepad))
        {
            SDL_CloseGamepad(s_gamepad);
            s_gamepad = nullptr;
            fprintf(stderr, "[input] gamepad removed\n");
            OpenFirstGamepad();
#if defined(__APPLE__) && TARGET_OS_IOS
            if (!s_gamepad)
                NoControllerNoticeIn(1'000'000'000, kControllerGone);
#endif
        }
#if defined(__APPLE__) && TARGET_OS_IOS
        else if (e.type == SDL_EVENT_KEYBOARD_ADDED)
            WithdrawNoControllerNotice();  // (a keyboard counts as a controller here)
#endif
    }

    void Shutdown()
    {
        if (s_gamepad)
        {
            SDL_RumbleGamepad(s_gamepad, 0, 0, 0);
            SDL_CloseGamepad(s_gamepad);
            s_gamepad = nullptr;
        }
    }

    void Suspend()
    {
        // Update sends a rumble for 5 s at a time and doesn't run meanwhile:
        // stop it now rather than buzz on for a game that stands still.
        if (s_gamepad)
            SDL_RumbleGamepad(s_gamepad, 0, 0, 0);
    }

    void Resume()
    {
        // The next Update sends the game's last request again, as the
        // Vibration setting allows.
        std::lock_guard lock(s_mutex);
        s_rumbleDirty = true;
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
        const std::string padSteps = StepVirtualPad();
        const float deadzone = settings::GetFloat(settings::Id::StickDeadzone);
        const float triggerDeadzone = settings::GetFloat(settings::Id::TriggerDeadzone);
        if (s_gamepad)
        {
            static const std::pair<SDL_GamepadButton, uint16_t> map[] = {
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
            for (auto& [button, bit] : map)
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

        // Keyboard (digital; overrides the analog values it touches).
        const bool* k = SDL_GetKeyboardState(nullptr);
        if (k)
        {
            static const std::pair<SDL_Scancode, uint16_t> keys[] = {
                { SDL_SCANCODE_UP, XAMINPUT_GAMEPAD_DPAD_UP }, { SDL_SCANCODE_DOWN, XAMINPUT_GAMEPAD_DPAD_DOWN },
                { SDL_SCANCODE_LEFT, XAMINPUT_GAMEPAD_DPAD_LEFT }, { SDL_SCANCODE_RIGHT, XAMINPUT_GAMEPAD_DPAD_RIGHT },
                { SDL_SCANCODE_SPACE, XAMINPUT_GAMEPAD_A }, { SDL_SCANCODE_BACKSPACE, XAMINPUT_GAMEPAD_B },
                { SDL_SCANCODE_LSHIFT, XAMINPUT_GAMEPAD_X }, { SDL_SCANCODE_LCTRL, XAMINPUT_GAMEPAD_Y },
                { SDL_SCANCODE_Q, XAMINPUT_GAMEPAD_LEFT_SHOULDER }, { SDL_SCANCODE_E, XAMINPUT_GAMEPAD_RIGHT_SHOULDER },
                { SDL_SCANCODE_RETURN, XAMINPUT_GAMEPAD_START }, { SDL_SCANCODE_ESCAPE, XAMINPUT_GAMEPAD_BACK },
                { SDL_SCANCODE_C, XAMINPUT_GAMEPAD_RIGHT_THUMB },
            };
            for (auto& [key, bit] : keys)
                if (k[key])
                    p.buttons |= bit;
            float kx = float(k[SDL_SCANCODE_RIGHT] || k[SDL_SCANCODE_D]) - float(k[SDL_SCANCODE_LEFT] || k[SDL_SCANCODE_A]);
            float ky = float(k[SDL_SCANCODE_UP]) - float(k[SDL_SCANCODE_DOWN]);
            if (kx != 0.0f)
                lx = kx;
            if (ky != 0.0f)
                ly = ky;
            if (k[SDL_SCANCODE_W])
                rt = 1.0f;
            if (k[SDL_SCANCODE_S])
                lt = 1.0f;
        }
        p.lx = ToShort(lx);
        p.ly = ToShort(ly);
        p.rx = ToShort(rx);
        p.ry = ToShort(ry);
        p.leftTrigger = Trigger(lt, triggerDeadzone);
        p.rightTrigger = Trigger(rt, triggerDeadzone);
        if (s_gamepad)
            LogStick(rawX, rawY, lx, ly, rawLt, rawRt, p.leftTrigger, p.rightTrigger);
        if (!padSteps.empty())
            fprintf(stderr, "[input] virtual pad %s: left stick raw (%+.4f, %+.4f) -> game (%d, %d), right (%d, %d); "
                            "triggers raw (%.3f, %.3f) -> game (%u, %u); deadzones: stick %.2f, trigger %.2f%s\n",
                padSteps.c_str(), rawX, rawY, int(p.lx), int(p.ly), int(p.rx), int(p.ry), rawLt, rawRt,
                unsigned(p.leftTrigger), unsigned(p.rightTrigger), deadzone, triggerDeadzone,
                s_blocked.load(std::memory_order_relaxed) ? " (a menu is open: the game sees an idle pad)" : "");

        // While a menu has the input the game sees an idle pad; after it
        // closes, each button still held stays hidden until released, so the
        // press that closed the menu (B, Start, Esc) doesn't reach the game.
        bool blocked = s_blocked.load(std::memory_order_relaxed);
        if (s_wasBlocked && !blocked)
            s_latchedButtons = p.buttons;
        // A Back pressed in the menu (or the one that closed it) is never replayed.
        if ((blocked || s_wasBlocked) && padBack)
            s_backState = BackState::Swallow;
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
        if (bool on = settings::GetBool(settings::Id::Vibration); on != vibration)
        {
            vibration = on;
            rumble = true;  // stop (or restart) a rumble under way
        }
        if (rumble && s_virtualPad)
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
        if (!vibration)
            low = high = 0;
        if (rumble && s_gamepad)
            SDL_RumbleGamepad(s_gamepad, low, high, low || high ? 5000 : 0);
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

    constexpr uint32_t ERROR_EMPTY = 0x10D2;

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

uint32_t XamInputGetCapabilities(uint32_t userIndex, uint32_t flags, XAMINPUT_CAPABILITIES* caps)
{
    bool scripted = !s_script.empty();
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
