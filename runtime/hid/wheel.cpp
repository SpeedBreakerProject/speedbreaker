// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See wheel.h.
#include "wheel.h"
#include "key_map.h"
#include "wheel_map.h"

#include <user/settings.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <SDL3/SDL_haptic.h>
#include <SDL3/SDL_joystick.h>

// wheel_map.h names XInput's bits as literals (it has no kernel header), as
// key_map.h does; input.cpp checks key_map.h's against the bits the game
// reads, so agreeing with it is enough.
static_assert(hid::wheelmap::button::DpadUp == hid::keymap::button::DpadUp &&
    hid::wheelmap::button::DpadDown == hid::keymap::button::DpadDown &&
    hid::wheelmap::button::DpadLeft == hid::keymap::button::DpadLeft &&
    hid::wheelmap::button::DpadRight == hid::keymap::button::DpadRight &&
    hid::wheelmap::button::Start == hid::keymap::button::Start && hid::wheelmap::button::Back == hid::keymap::button::Back &&
    hid::wheelmap::button::LeftThumb == hid::keymap::button::LeftThumb &&
    hid::wheelmap::button::RightThumb == hid::keymap::button::RightThumb &&
    hid::wheelmap::button::LeftShoulder == hid::keymap::button::LeftShoulder &&
    hid::wheelmap::button::RightShoulder == hid::keymap::button::RightShoulder &&
    hid::wheelmap::button::A == hid::keymap::button::A && hid::wheelmap::button::B == hid::keymap::button::B &&
    hid::wheelmap::button::X == hid::keymap::button::X && hid::wheelmap::button::Y == hid::keymap::button::Y,
    "wheel_map.h's button bits are key_map.h's, and so XInput's");
// ...and its hat bits are SDL's.
static_assert(hid::wheelmap::hat::Centred == SDL_HAT_CENTERED && hid::wheelmap::hat::Up == SDL_HAT_UP &&
    hid::wheelmap::hat::Right == SDL_HAT_RIGHT && hid::wheelmap::hat::Down == SDL_HAT_DOWN &&
    hid::wheelmap::hat::Left == SDL_HAT_LEFT, "wheel_map.h's hat bits are SDL's");

namespace hid::wheel
{
    namespace
    {
        using wheelmap::Axes;
        using wheelmap::Binding;

        // A device reports at most this many buttons to us; the wheels we
        // know report 25 (a G29 with a shifter attached).
        constexpr int kMaxButtons = 64;
        // How far a pedal must move to count as the one being assigned
        // (Settings > Wheel > Pedals): a third of its travel, which no
        // amount of jitter reaches and a deliberate press passes at once.
        constexpr int kAssignMoved = wheelmap::kAxisMax / 3;

        bool s_started = false;
        SDL_Joystick* s_joystick = nullptr;
        SDL_JoystickID s_id = 0;
        std::string s_name;
        const wheelmap::Model* s_model = nullptr;
        std::vector<Binding> s_bindings;
        Axes s_axes;
        wheelmap::Pedal s_pedals[3];  // Pedal::Throttle, Brake, Clutch
        int s_fullTurn = 0;           // degrees lock to lock, 0: unknown
        // The axes' resting places are read again on the first frame after
        // the device was opened: a joystick SDL has not polled yet reads 0
        // on every axis, and a pedal taken to rest at 0 when it really
        // rests at one end would hold the throttle down.
        bool s_restAgain = false;

        SDL_Haptic* s_haptic = nullptr;
        bool s_canAutocentre = false;  // the wheel takes an autocentre level
        bool s_canSpring = false;      // ...or a spring effect
        bool s_canHapticRumble = false;
        bool s_canJoystickRumble = false;
        bool s_onlyOneEffect = false;  // a rumble would stop the centring: the centring wins
        SDL_HapticEffectID s_spring = -1;
        int s_centering = -1;          // the level the wheel is holding; -1: none yet
        float s_rumble = 0.0f;         // what it was last told to play

        const bool s_log = std::getenv("NFSMW_WHEEL_LOG") != nullptr;

        // Pedal -> s_pedals, and the setting holding its axis.
        int Slot(Pedal pedal) { return int(pedal); }

        settings::Id AxisOption(Pedal pedal)
        {
            switch (pedal)
            {
            case Pedal::Throttle: return settings::Id::WheelThrottleAxis;
            case Pedal::Brake: return settings::Id::WheelBrakeAxis;
            case Pedal::Clutch: return settings::Id::WheelClutchAxis;
            }
            return settings::Id::WheelThrottleAxis;
        }

        const char* PedalName(Pedal pedal)
        {
            switch (pedal)
            {
            case Pedal::Throttle: return "throttle";
            case Pedal::Brake: return "brake";
            case Pedal::Clutch: return "clutch";
            }
            return "?";
        }

        int AxisOf(Pedal pedal)
        {
            switch (pedal)
            {
            case Pedal::Throttle: return s_axes.throttle;
            case Pedal::Brake: return s_axes.brake;
            case Pedal::Clutch: return s_axes.clutch;
            }
            return -1;
        }

        int16_t AxisNow(int axis)
        {
            if (!s_joystick || axis < 0)
                return 0;
            return SDL_GetJoystickAxis(s_joystick, axis);
        }

        // Where every axis is now, which is where they rest when the device
        // has just been opened.
        std::vector<int16_t> AxisStates(bool initial)
        {
            std::vector<int16_t> values;
            if (!s_joystick)
                return values;
            const int count = std::max(0, SDL_GetNumJoystickAxes(s_joystick));
            values.resize(size_t(count));
            for (int a = 0; a < count; a++)
            {
                Sint16 state = 0;
                // The state SDL saw when the device turned up, which a pedal
                // hasn't had a chance to move yet; failing that, now's.
                if (!initial || !SDL_GetJoystickAxisInitialState(s_joystick, a, &state))
                    state = SDL_GetJoystickAxis(s_joystick, a);
                values[size_t(a)] = state;
            }
            return values;
        }

        // The axes the player assigned (Settings > Wheel > Pedals); -1 each
        // where they haven't.
        Axes StoredAxes()
        {
            return { settings::GetInt(settings::Id::WheelSteerAxis), settings::GetInt(settings::Id::WheelThrottleAxis),
                     settings::GetInt(settings::Id::WheelBrakeAxis), settings::GetInt(settings::Id::WheelClutchAxis) };
        }

        // Which axis is which, and each pedal's resting place: the model's
        // idea, checked against where the axes rest, with the player's own
        // assignments on top of both.
        void Calibrate(bool initial)
        {
            const std::vector<int16_t> rest = AxisStates(initial);
            const Axes stored = StoredAxes();
            Axes preferred = s_model->axes;
            if (stored.steer >= 0) preferred.steer = stored.steer;
            if (stored.throttle >= 0) preferred.throttle = stored.throttle;
            if (stored.brake >= 0) preferred.brake = stored.brake;
            if (stored.clutch >= 0) preferred.clutch = stored.clutch;
            s_axes = wheelmap::Detect(rest, preferred);
            // The player's assignments are not guesses: they stand.
            if (stored.steer >= 0) s_axes.steer = stored.steer;
            if (stored.throttle >= 0) s_axes.throttle = stored.throttle;
            if (stored.brake >= 0) s_axes.brake = stored.brake;
            if (stored.clutch >= 0) s_axes.clutch = stored.clutch;
            const Pedal all[] = { Pedal::Throttle, Pedal::Brake, Pedal::Clutch };
            for (Pedal pedal : all)
            {
                const int axis = AxisOf(pedal);
                s_pedals[Slot(pedal)] = {};
                if (axis >= 0 && size_t(axis) < rest.size())
                    s_pedals[Slot(pedal)].Start(rest[size_t(axis)]);
            }
            fprintf(stderr, "[wheel] axes: steering %d, throttle %d, brake %d, clutch %d (of %zu; rest", s_axes.steer,
                s_axes.throttle, s_axes.brake, s_axes.clutch, rest.size());
            for (int16_t v : rest)
                fprintf(stderr, " %d", int(v));
            fprintf(stderr, ")\n");
        }

        void StopRumble()
        {
            s_rumble = 0.0f;
            if (s_haptic && s_canHapticRumble)
                SDL_StopHapticRumble(s_haptic);
            if (s_joystick && s_canJoystickRumble)
                SDL_RumbleJoystick(s_joystick, 0, 0, 0);
        }

        void StopSpring()
        {
            if (s_haptic && s_spring >= 0)
            {
                SDL_StopHapticEffect(s_haptic, s_spring);
                SDL_DestroyHapticEffect(s_haptic, s_spring);
            }
            s_spring = -1;
        }

        // Settings > Wheel > Centering Force: an autocentre level where the
        // wheel takes one, else a spring effect that pulls it to the middle.
        void ApplyCentering()
        {
            if (!s_haptic)
                return;
            const int percent = std::clamp(settings::GetInt(settings::Id::CenteringForce), 0, 100);
            if (percent == s_centering)
                return;
            s_centering = percent;
            if (s_canAutocentre)
            {
                if (!SDL_SetHapticAutocenter(s_haptic, percent))
                    fprintf(stderr, "[wheel] autocentre %d%% failed: %s\n", percent, SDL_GetError());
                return;
            }
            if (!s_canSpring)
                return;
            if (percent == 0)
            {
                StopSpring();
                return;
            }
            SDL_HapticEffect effect{};
            effect.condition.type = SDL_HAPTIC_SPRING;
            effect.condition.direction.type = SDL_HAPTIC_CARTESIAN;
            effect.condition.direction.dir[0] = 1;
            effect.condition.length = SDL_HAPTIC_INFINITY;
            for (int axis = 0; axis < 3; axis++)
            {
                effect.condition.right_sat[axis] = wheelmap::SpringSaturation(percent);
                effect.condition.left_sat[axis] = wheelmap::SpringSaturation(percent);
                effect.condition.right_coeff[axis] = Sint16(wheelmap::SpringLevel(percent));
                effect.condition.left_coeff[axis] = Sint16(wheelmap::SpringLevel(percent));
            }
            if (s_spring >= 0)
            {
                if (SDL_UpdateHapticEffect(s_haptic, s_spring, &effect))
                    return;
                StopSpring();  // it wouldn't take the change: make it afresh
            }
            s_spring = SDL_CreateHapticEffect(s_haptic, &effect);
            if (s_spring < 0 || !SDL_RunHapticEffect(s_haptic, s_spring, SDL_HAPTIC_INFINITY))
            {
                fprintf(stderr, "[wheel] centring force failed: %s\n", SDL_GetError());
                StopSpring();
                s_canSpring = false;
            }
        }

        // What the wheel can do with its motors, which needs the system to
        // drive them: the maker's driver on Windows, the kernel's on Linux
        // (lg4ff for a G29 on SteamOS). macOS has neither for these wheels.
        void OpenHaptics()
        {
            s_canAutocentre = s_canSpring = s_canHapticRumble = s_onlyOneEffect = false;
            s_canJoystickRumble = false;
            s_centering = -1;
            if (!s_joystick)
                return;
            SDL_PropertiesID props = SDL_GetJoystickProperties(s_joystick);
            s_canJoystickRumble = SDL_GetBooleanProperty(props, SDL_PROP_JOYSTICK_CAP_RUMBLE_BOOLEAN, false);
            if (!SDL_IsJoystickHaptic(s_joystick))
            {
                fprintf(stderr, "[wheel] no force feedback on this system: the wheel stays slack%s\n",
                    s_canJoystickRumble ? " (it does take plain rumble)" : "");
                return;
            }
            s_haptic = SDL_OpenHapticFromJoystick(s_joystick);
            if (!s_haptic)
            {
                fprintf(stderr, "[wheel] force feedback didn't open: %s\n", SDL_GetError());
                return;
            }
            const Uint32 features = SDL_GetHapticFeatures(s_haptic);
            s_canAutocentre = (features & SDL_HAPTIC_AUTOCENTER) != 0;
            s_canSpring = (features & SDL_HAPTIC_SPRING) != 0;
            s_canHapticRumble = SDL_InitHapticRumble(s_haptic);
            // One effect at a time: a knock would take the centring force
            // away, which matters far more while driving.
            const int playing = SDL_GetMaxHapticEffectsPlaying(s_haptic);
            s_onlyOneEffect = playing == 1 && !s_canAutocentre && s_canSpring;
            fprintf(stderr, "[wheel] force feedback: centring %s, knocks %s\n",
                s_canAutocentre ? "autocentre" : s_canSpring ? "spring" : "no",
                s_onlyOneEffect ? "no (one effect at a time: the centring force has it)"
                                : s_canHapticRumble ? "yes" : s_canJoystickRumble ? "rumble" : "no");
        }

        void Close()
        {
            StopRumble();
            StopSpring();
            if (s_haptic)
            {
                if (s_canAutocentre)
                    SDL_SetHapticAutocenter(s_haptic, 0);
                SDL_CloseHaptic(s_haptic);
                s_haptic = nullptr;
            }
            if (s_joystick)
            {
                SDL_CloseJoystick(s_joystick);
                s_joystick = nullptr;
            }
            s_id = 0;
            s_restAgain = false;
            s_name.clear();
            s_bindings.clear();
            s_axes = {};
            s_model = nullptr;
            s_fullTurn = 0;
            s_centering = -1;
        }

        // NFSMW_WHEEL_BUTTONS="0=A,2=B,8=BACK,9=START,4=SHIFTUP": what the
        // wheel's buttons do, for one whose numbering the model has wrong.
        void TakeBindings()
        {
            s_bindings.assign(s_model->buttons.begin(), s_model->buttons.end());
            const char* text = std::getenv("NFSMW_WHEEL_BUTTONS");
            if (!text || !*text)
                return;
            std::string error;
            s_bindings = wheelmap::ParseBindings(text, s_model->buttons, error);
            if (!error.empty())
                fprintf(stderr, "[wheel] NFSMW_WHEEL_BUTTONS: can't read \"%s\"\n", error.c_str());
            std::string shown;
            for (const Binding& b : s_bindings)
                shown += (shown.empty() ? "" : " ") + std::to_string(b.index) + "=" + std::string(wheelmap::NameOf(b));
            fprintf(stderr, "[wheel] buttons: %s\n", shown.c_str());
        }

        void Open()
        {
            if (s_joystick || !s_started)
                return;
            int count = 0;
            SDL_JoystickID* ids = SDL_GetJoysticks(&count);
            if (!ids)
                return;
            for (int i = 0; i < count && !s_joystick; i++)
            {
                const SDL_JoystickID id = ids[i];
                const char* name = SDL_GetJoystickNameForID(id);
                const bool sdlSaysWheel = SDL_GetJoystickTypeForID(id) == SDL_JOYSTICK_TYPE_WHEEL;
                const uint16_t vendor = SDL_GetJoystickVendorForID(id), product = SDL_GetJoystickProductForID(id);
                SDL_Joystick* joystick = SDL_OpenJoystick(id);
                if (!joystick)
                {
                    fprintf(stderr, "[wheel] joystick %u didn't open: %s\n", unsigned(id), SDL_GetError());
                    continue;
                }
                // How many axes and buttons it has takes an open device, and
                // tells a wheel from a gamepad whose name happens to match.
                if (!wheelmap::LooksLikeWheel(sdlSaysWheel, vendor, product, name ? name : "",
                        SDL_GetNumJoystickAxes(joystick), SDL_GetNumJoystickButtons(joystick)))
                {
                    SDL_CloseJoystick(joystick);
                    continue;
                }
                // The device has only just been opened: make sure SDL has
                // read it before its axes are taken for where they rest.
                SDL_UpdateJoysticks();
                s_joystick = joystick;
                s_id = id;
                s_name = name ? name : "Steering wheel";
                s_model = &wheelmap::ModelFor(vendor, product);
                s_fullTurn = s_model->fullTurnDegrees;
                const std::string turn = s_fullTurn ? std::to_string(s_fullTurn) + " degrees" : "an unknown amount";
                fprintf(stderr, "[wheel] %s (USB %04x:%04x, %s, %d axes, %d buttons, %d hats, turns %s)\n", s_name.c_str(),
                    vendor, product, s_model->vendor ? s_model->name : "not one we know",
                    SDL_GetNumJoystickAxes(s_joystick), SDL_GetNumJoystickButtons(s_joystick),
                    SDL_GetNumJoystickHats(s_joystick), turn.c_str());
                TakeBindings();
                Calibrate(true);
                s_restAgain = true;
                OpenHaptics();
                ApplyCentering();
            }
            SDL_free(ids);
        }

        // Settings > Wheel > Pedals: the assignment under way.
        bool s_assigning = false;
        Pedal s_assignTo = Pedal::Throttle;
        std::vector<int16_t> s_assignFrom;

        // NFSMW_WHEEL_LOG=1: every button as it is pressed (which is how to
        // find a wheel's numbering), and the axes once a second.
        void LogSample(const std::array<bool, kMaxButtons>& buttons, const Reading& r)
        {
            if (!s_log)
                return;
            static std::array<bool, kMaxButtons> was{};
            for (int b = 0; b < kMaxButtons; b++)
                if (buttons[size_t(b)] != was[size_t(b)])
                    fprintf(stderr, "[wheel] button %d %s\n", b, buttons[size_t(b)] ? "down" : "up");
            was = buttons;
            static auto last = std::chrono::steady_clock::now();
            const auto now = std::chrono::steady_clock::now();
            if (now - last < std::chrono::seconds(1))
                return;
            last = now;
            fprintf(stderr, "[wheel] steering raw %d -> %+.3f; pedals raw (%d, %d, %d) -> throttle %.2f, brake %.2f, clutch %.2f\n",
                int(AxisNow(s_axes.steer)), r.steer, int(AxisNow(s_axes.throttle)), int(AxisNow(s_axes.brake)),
                int(AxisNow(s_axes.clutch)), r.throttle, r.brake, r.clutch);
        }

        std::array<bool, kMaxButtons> ButtonsNow()
        {
            std::array<bool, kMaxButtons> held{};
            if (!s_joystick)
                return held;
            const int count = std::min(kMaxButtons, std::max(0, SDL_GetNumJoystickButtons(s_joystick)));
            for (int b = 0; b < count; b++)
                held[size_t(b)] = SDL_GetJoystickButton(s_joystick, b);
            return held;
        }

        uint8_t HatNow()
        {
            if (!s_joystick || SDL_GetNumJoystickHats(s_joystick) < 1)
                return wheelmap::hat::Centred;
            return SDL_GetJoystickHat(s_joystick, 0);
        }

        bool InPlay()
        {
            return s_joystick != nullptr && settings::GetBool(settings::Id::Wheel);
        }

        Reading s_last;  // what Sample last read (Last)
    }

    void Initialize()
    {
        s_started = true;
        Open();
    }

    void HandleEvent(const SDL_Event& e)
    {
        if (!s_started)
            return;
        if (e.type == SDL_EVENT_JOYSTICK_ADDED)
        {
            Open();
        }
        else if (e.type == SDL_EVENT_JOYSTICK_REMOVED && s_joystick && e.jdevice.which == s_id)
        {
            fprintf(stderr, "[wheel] %s disconnected\n", s_name.c_str());
            CancelAssign();
            Close();
            Open();  // a second wheel, if one is plugged in
        }
    }

    bool Found()
    {
        return s_joystick != nullptr;
    }

    bool Playing()
    {
        return InPlay();
    }

    bool Owns(SDL_JoystickID joystick)
    {
        return s_joystick != nullptr && joystick == s_id;
    }

    std::string Name()
    {
        return s_joystick ? s_name : std::string();
    }

    bool HasForceFeedback()
    {
        return s_haptic != nullptr && (s_canAutocentre || s_canSpring || s_canHapticRumble);
    }

    bool Sample(Reading& out)
    {
        if (!s_joystick)
            return false;
        if (s_restAgain)
        {
            s_restAgain = false;
            Calibrate(true);  // a frame on, with the device certainly read
        }
        const std::array<bool, kMaxButtons> held = ButtonsNow();
        const wheelmap::Pressed pressed = wheelmap::Resolve(held, HatNow(), s_bindings);
        Reading r;
        r.buttons = pressed.buttons;
        r.rightY = pressed.rightY;
        r.back = (pressed.buttons & wheelmap::button::Back) != 0;
        r.start = (pressed.buttons & wheelmap::button::Start) != 0;
        if (s_axes.steer >= 0)
            r.steer = wheelmap::Steer(AxisNow(s_axes.steer), settings::GetInt(settings::Id::SteeringRange), s_fullTurn,
                settings::GetFloat(settings::Id::SteeringDeadzone), settings::GetFloat(settings::Id::SteeringResponse));
        if (s_axes.throttle >= 0)
            r.throttle = s_pedals[Slot(Pedal::Throttle)].Value(AxisNow(s_axes.throttle));
        if (s_axes.brake >= 0)
            r.brake = s_pedals[Slot(Pedal::Brake)].Value(AxisNow(s_axes.brake));
        if (s_axes.clutch >= 0)
            r.clutch = s_pedals[Slot(Pedal::Clutch)].Value(AxisNow(s_axes.clutch));
        LogSample(held, r);
        s_last = r;
        // Switched off, the wheel plays nothing - but it was still read, so
        // Settings > Wheel keeps showing what it reads, and the menus (and
        // the chord that opens them) keep working from it.
        if (!settings::GetBool(settings::Id::Wheel))
            return false;
        out = r;
        return true;
    }

    Reading Last()
    {
        return s_joystick ? s_last : Reading{};
    }

    uint16_t HeldButtons()
    {
        if (!s_joystick)
            return 0;
        return wheelmap::Resolve(ButtonsNow(), HatNow(), s_bindings).buttons;
    }

    uint16_t ButtonFor(SDL_JoystickID joystick, int button)
    {
        if (!s_joystick || joystick != s_id)
            return 0;
        for (const Binding& b : s_bindings)
            if (b.index == button && b.role == wheelmap::Role::Button)
                return b.button;
        return 0;
    }

    void Rumble(uint16_t low, uint16_t high, bool vibration)
    {
        if (!s_joystick)
            return;
        const int percent = settings::GetInt(settings::Id::ForceFeedback);
        const float strength = vibration ? wheelmap::RumbleStrength(low, high, percent) : 0.0f;
        if (strength == s_rumble)
            return;
        s_rumble = strength;
        if (strength <= 0.0f)
        {
            StopRumble();
            return;
        }
        if (s_onlyOneEffect)
            return;  // the centring force has the wheel's one effect
        if (s_canHapticRumble && SDL_PlayHapticRumble(s_haptic, strength, 5000))
            return;
        if (s_canJoystickRumble)
            SDL_RumbleJoystick(s_joystick, uint16_t(low * percent / 100), uint16_t(high * percent / 100), 5000);
    }

    void Update()
    {
        if (s_joystick)
            ApplyCentering();
    }

    void Suspend()
    {
        StopRumble();
    }

    void Shutdown()
    {
        Close();
        s_started = false;
    }

    void BeginAssign(Pedal pedal)
    {
        if (!s_joystick)
            return;
        s_assigning = true;
        s_assignTo = pedal;
        s_assignFrom = AxisStates(false);
    }

    bool PollAssign()
    {
        if (!s_assigning || !s_joystick)
            return false;
        int best = -1, moved = 0;
        for (size_t a = 0; a < s_assignFrom.size(); a++)
        {
            if (int(a) == s_axes.steer)
                continue;  // the wheel itself is never a pedal
            const int by = std::abs(int(AxisNow(int(a))) - int(s_assignFrom[a]));
            if (by > moved)
            {
                moved = by;
                best = int(a);
            }
        }
        if (best < 0 || moved < kAssignMoved)
            return false;
        s_assigning = false;
        fprintf(stderr, "[wheel] %s: axis %d (it moved %d)\n", PedalName(s_assignTo), best, moved);
        // Stored, so the next run knows it too, and applied at once: the
        // axis rests where it did before the pedal was pressed.
        settings::Set(AxisOption(s_assignTo), double(best));
        Calibrate(false);
        s_pedals[Slot(s_assignTo)].Start(s_assignFrom[size_t(best)]);
        return true;
    }

    void CancelAssign()
    {
        s_assigning = false;
    }

    bool Assigning()
    {
        return s_assigning;
    }

    Assignment Axes()
    {
        return { s_axes.steer, s_axes.throttle, s_axes.brake, s_axes.clutch };
    }

    void ResetAxes()
    {
        CancelAssign();
        for (settings::Id id : { settings::Id::WheelSteerAxis, settings::Id::WheelThrottleAxis, settings::Id::WheelBrakeAxis,
                                 settings::Id::WheelClutchAxis })
            settings::Set(id, -1);
        if (s_joystick)
            Calibrate(false);
    }
}
