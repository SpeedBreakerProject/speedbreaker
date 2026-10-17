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
#include <format>
#include <string>
#include <vector>

#include <SDL3/SDL_haptic.h>
#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_timer.h>

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
        wheelmap::Steering s_steering;  // the wheel's two ends, measured or assumed
        wheelmap::Pedal s_pedals[3];  // in Step order: Throttle, Brake, Clutch
        int s_fullTurn = 0;           // degrees lock to lock, 0: unknown
        // The axes' resting places are read again on the first frame after
        // the device was opened: a joystick SDL has not polled yet reads 0
        // on every axis, and a pedal taken to rest at 0 when it really
        // rests at one end would hold the throttle down.
        bool s_restAgain = false;
        // Which axes SDL has reported a value for. It sends an axis only
        // when it moves, so everything reads 0 until something does: a
        // reading from an axis that has never spoken means nothing.
        std::vector<bool> s_axisSeen;

        SDL_Haptic* s_haptic = nullptr;
        bool s_canAutocentre = false;  // the wheel takes an autocentre level
        bool s_canSpring = false;      // ...or a spring effect
        bool s_canHapticRumble = false;
        bool s_canJoystickRumble = false;
        bool s_onlyOneEffect = false;  // a rumble would stop the centring: the centring wins
        SDL_HapticEffectID s_spring = -1;
        int s_centering = -1;          // the level the wheel is holding; -1: none yet
        Uint64 s_centeringSaid = 0;    // when it was last told (SDL ticks)
        float s_rumble = 0.0f;         // what it was last told to play

        const bool s_log = std::getenv("NFSMW_WHEEL_LOG") != nullptr;

        // A pedal step's place in s_pedals.
        int Slot(Step step) { return int(step) - int(Step::Throttle); }

        settings::Id AxisOption(Step step)
        {
            switch (step)
            {
            case Step::Throttle: return settings::Id::WheelThrottleAxis;
            case Step::Brake: return settings::Id::WheelBrakeAxis;
            case Step::Clutch: return settings::Id::WheelClutchAxis;
            default: return settings::Id::WheelSteerAxis;
            }
        }

        settings::Id RestOption(Step step)
        {
            switch (step)
            {
            case Step::Brake: return settings::Id::BrakeRest;
            case Step::Clutch: return settings::Id::ClutchRest;
            default: return settings::Id::ThrottleRest;
            }
        }

        settings::Id FullOption(Step step)
        {
            switch (step)
            {
            case Step::Brake: return settings::Id::BrakeFull;
            case Step::Clutch: return settings::Id::ClutchFull;
            default: return settings::Id::ThrottleFull;
            }
        }

        // The setting holding the button given to a pad control; they run in
        // Control order from ButtonA, as the static_assert below checks.
        settings::Id ButtonOption(wheelmap::Control control)
        {
            return settings::Id(size_t(settings::Id::ButtonA) + size_t(control));
        }
        static_assert(size_t(settings::Id::ButtonShiftDown) - size_t(settings::Id::ButtonA) + 1 == wheelmap::kControlCount,
            "one Button option per wheel_map.h Control, in Control order");

        int AxisOf(Step step)
        {
            switch (step)
            {
            case Step::Throttle: return s_axes.throttle;
            case Step::Brake: return s_axes.brake;
            case Step::Clutch: return s_axes.clutch;
            default: return s_axes.steer;
            }
        }

        // What Calibrate recorded for each control, in Control order.
        std::array<int, wheelmap::kControlCount> StoredButtons()
        {
            std::array<int, wheelmap::kControlCount> out{};
            for (size_t i = 0; i < wheelmap::kControlCount; i++)
                out[i] = settings::GetInt(ButtonOption(wheelmap::Control(i)));
            return out;
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
            bool anySeen = false;
            for (size_t a = 0; a < s_axisSeen.size(); a++)
                anySeen = anySeen || s_axisSeen[a];
            const Axes stored = StoredAxes();
            Axes preferred = s_model->axes;
            if (stored.steer >= 0) preferred.steer = stored.steer;
            if (stored.throttle >= 0) preferred.throttle = stored.throttle;
            if (stored.brake >= 0) preferred.brake = stored.brake;
            if (stored.clutch >= 0) preferred.clutch = stored.clutch;
            // Where the axes rest only means something once they have
            // reported: before that, the model's own layout is all there is.
            s_axes = anySeen ? wheelmap::Detect(rest, preferred) : preferred;
            // The player's assignments are not guesses: they stand.
            if (stored.steer >= 0) s_axes.steer = stored.steer;
            if (stored.throttle >= 0) s_axes.throttle = stored.throttle;
            if (stored.brake >= 0) s_axes.brake = stored.brake;
            if (stored.clutch >= 0) s_axes.clutch = stored.clutch;
            // The wheel's two ends: both as Calibrate measured them, else
            // the axis as it comes. Both have to be there - one alone would
            // put the centre halfway to nowhere.
            const int16_t left = int16_t(settings::GetInt(settings::Id::SteerLeft));
            const int16_t right = int16_t(settings::GetInt(settings::Id::SteerRight));
            if (left != right && left != 0 && right != 0)
                s_steering.Start(left, right);
            else
                s_steering.Start(-wheelmap::kAxisMax, wheelmap::kAxisMax);
            // Each pedal's axis and the span it runs over: what Calibrate
            // measured, else what this wheel's model says its pedals do,
            // else the axis itself once it has reported (its far end being
            // as far as it can go). Without any of those it reads nothing,
            // rather than reading as pressed.
            struct Plan { int axis = -1; int16_t rest = 0, full = 0; bool known = false; };
            Plan plan[3];
            for (Step step : { Step::Throttle, Step::Brake, Step::Clutch })
            {
                Plan& it = plan[Slot(step)];
                it.axis = AxisOf(step);
                if (it.axis < 0 || size_t(it.axis) >= rest.size())
                {
                    it.axis = -1;
                    continue;
                }
                const int16_t storedRest = int16_t(settings::GetInt(RestOption(step)));
                const int16_t storedFull = int16_t(settings::GetInt(FullOption(step)));
                if (storedRest != storedFull)
                    it = { it.axis, storedRest, storedFull, true };
                else if (s_model->pedalRest != s_model->pedalFull)
                    it = { it.axis, s_model->pedalRest, s_model->pedalFull, true };
                else if (size_t(it.axis) < s_axisSeen.size() && s_axisSeen[size_t(it.axis)])
                {
                    const int16_t at = rest[size_t(it.axis)];
                    it = { it.axis, at, int16_t(at >= 0 ? -32768 : 32767), true };
                }
            }
            // No two controls may read one axis the same way: that is how a
            // calibration gone wrong has one pedal working two of the
            // game's, while the other pedal does nothing at all. Two pedals
            // sharing an axis is allowed only where their spans run
            // opposite ways, which is how a wheel with both on one axis
            // reports them. The wheel's own axis is never a pedal.
            for (int a = 0; a < 3; a++)
            {
                if (plan[a].axis >= 0 && plan[a].axis == s_axes.steer)
                {
                    fprintf(stderr, "[wheel] %s is on the wheel's own axis %d: ignored\n", StepName(Step(a + 1)), plan[a].axis);
                    plan[a] = {};
                }
                for (int b = a + 1; b < 3; b++)
                {
                    if (plan[a].axis < 0 || plan[a].axis != plan[b].axis)
                        continue;
                    const bool opposite = plan[a].known && plan[b].known &&
                        (plan[a].full > plan[a].rest) != (plan[b].full > plan[b].rest);
                    if (opposite)
                        continue;  // one axis, one each way: both are real
                    fprintf(stderr, "[wheel] %s and %s both read axis %d the same way: %s dropped (calibrate it)\n",
                        StepName(Step(a + 1)), StepName(Step(b + 1)), plan[a].axis, StepName(Step(b + 1)));
                    plan[b] = {};
                    settings::Set(AxisOption(Step(b + 1)), -1);  // so it doesn't come back next run
                }
            }
            for (Step step : { Step::Throttle, Step::Brake, Step::Clutch })
            {
                const Plan& it = plan[Slot(step)];
                s_pedals[Slot(step)] = {};  // not ready: reads nothing
                if (it.axis >= 0 && it.known)
                    s_pedals[Slot(step)].Start(it.rest, it.full);
            }
            s_axes.throttle = plan[0].axis;
            s_axes.brake = plan[1].axis;
            s_axes.clutch = plan[2].axis;
            // Each pedal's span as well as its axis: a pedal reading
            // nothing, or reading backwards, shows here before anyone has
            // to guess at it.
            std::string pedals;
            for (Step step : { Step::Throttle, Step::Brake, Step::Clutch })
            {
                const wheelmap::Pedal& it = s_pedals[Slot(step)];
                pedals += std::format(", {} {}", StepName(step), AxisOf(step));
                pedals += it.ready ? std::format(" ({:+d} to {:+d})", int(it.idle), int(it.full)) : std::string(" (not placed)");
            }
            fprintf(stderr, "[wheel] axes: steering %d%s (of %zu; rest", s_axes.steer, pedals.c_str(), rest.size());
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
        // `again`: say it even if it has been said before. A wheel does its
        // own power-on sweep and needn't have been listening when it was
        // first told, and a one-shot call that misses leaves the player's
        // centring force doing nothing until they nudge the row - which is
        // just how that looks from the outside.
        void ApplyCentering(bool again = false)
        {
            if (!s_haptic)
                return;
            const int percent = std::clamp(settings::GetInt(settings::Id::CenteringForce), 0, 100);
            if (percent == s_centering && !again)
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
        // drive them: the kernel's driver on Linux (lg4ff for a G29 on
        // SteamOS), and on macOS Apple's Force Feedback framework, which
        // takes a G29 as it is (a measured G29 offers a constant force, a
        // sine, a spring, an autocentre level and gain, 16 effects at
        // once).
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
            s_axisSeen.clear();
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
            // The model's buttons, with whatever Calibrate recorded on top.
            const std::array<int, wheelmap::kControlCount> assigned = StoredButtons();
            s_bindings = wheelmap::WithAssigned(s_model->buttons, assigned);
            const char* text = std::getenv("NFSMW_WHEEL_BUTTONS");
            if (!text || !*text)
                return;
            std::string error;
            s_bindings = wheelmap::ParseBindings(text, s_bindings, error);
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
                s_axisSeen.assign(size_t(std::max(0, SDL_GetNumJoystickAxes(s_joystick))), false);
                Calibrate(true);
                s_restAgain = true;
                OpenHaptics();
                ApplyCentering();
            }
            SDL_free(ids);
        }

        // Settings > Wheel > Calibrate: the step under way. The furthest
        // each axis has moved since it began is kept, so letting a pedal up
        // doesn't lose the reading and the biggest mover wins however long
        // the player takes.
        bool s_assigning = false;
        Step s_assignStep = Step::SteerLeft;
        std::vector<int16_t> s_assignFrom;
        // Per axis, the range it has covered since the step began, and
        // whether it has been worth tracking at all: an axis SDL has not
        // reported reads 0, which is not where it is, so it is left out
        // until it speaks. That also keeps a pedal reporting for the first
        // time out of another step's reckoning.
        std::vector<int32_t> s_assignLo, s_assignHi;
        std::vector<bool> s_assignTracked;
        // A button step takes a button that goes down during the step, not
        // one already held: the A that confirmed the step before is still
        // down as the next one begins.
        std::array<bool, kMaxButtons> s_assignButtonsFrom{};

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

        // How far a pedal is down, or nothing at all while its axis has yet
        // to report: SDL gives an axis only when it moves, and the 0 it
        // reads until then is not where the pedal is. On a wheel whose
        // pedals run +32767 to -32768 that 0 is halfway, so without this a
        // resting pedal reads half pressed.
        float PedalValue(Step step)
        {
            const int axis = AxisOf(step);
            if (axis < 0 || size_t(axis) >= s_axisSeen.size() || !s_axisSeen[size_t(axis)])
                return 0.0f;
            return s_pedals[Slot(step)].Value(AxisNow(axis));
        }

        bool InPlay()
        {
            return s_joystick != nullptr && settings::GetBool(settings::Id::Wheel);
        }

        Reading s_last;  // what Sample last read (Last)
    }

    bool StepIsButton(Step step)
    {
        return step >= Step::ButtonA && step < Step::Count;
    }

    const char* StepName(Step step)
    {
        switch (step)
        {
        case Step::SteerLeft: return "Full left";
        case Step::SteerRight: return "Full right";
        case Step::Throttle: return "Throttle";
        case Step::Brake: return "Brake";
        case Step::Clutch: return "Clutch";
        default: break;
        }
        if (StepIsButton(step))
            return wheelmap::kControls[size_t(step) - size_t(Step::ButtonA)].name;
        return "?";
    }

    const char* StepAsk(Step step)
    {
        switch (step)
        {
        case Step::SteerLeft: return "Turn the wheel fully left";
        case Step::SteerRight: return "Turn the wheel fully right";
        case Step::Throttle: return "Press the throttle to the floor";
        case Step::Brake: return "Press the brake to the floor";
        case Step::Clutch: return "Press the clutch to the floor";
        default: break;
        }
        return StepIsButton(step) ? "Press the button you want for this" : "?";
    }

    void Initialize()
    {
        s_started = true;
        // Every Wheel setting as it is read, so a run says for itself what
        // it is playing by: a value here that differs from settings.toml
        // means the file didn't reach the game.
        fprintf(stderr, "[wheel] settings: wheel %s, range %s, invert %s, deadzone %.2f, response %.2f, "
                        "force feedback %d%%, centring %d%%\n",
            settings::GetBool(settings::Id::Wheel) ? "on" : "off",
            settings::FormatValue(settings::Id::SteeringRange, settings::GetValue(settings::Id::SteeringRange)).c_str(),
            settings::GetBool(settings::Id::InvertSteering) ? "yes" : "no",
            double(settings::GetFloat(settings::Id::SteeringDeadzone)), double(settings::GetFloat(settings::Id::SteeringResponse)),
            settings::GetInt(settings::Id::ForceFeedback), settings::GetInt(settings::Id::CenteringForce));
        Open();
    }

    void HandleEvent(const SDL_Event& e)
    {
        if (!s_started)
            return;
        if (e.type == SDL_EVENT_JOYSTICK_AXIS_MOTION && s_joystick && e.jaxis.which == s_id)
        {
            const int a = e.jaxis.axis;
            if (a >= 0 && size_t(a) < s_axisSeen.size() && !s_axisSeen[size_t(a)])
            {
                s_axisSeen[size_t(a)] = true;
                // It is certainly awake now, whatever it was doing when it
                // was first told what to hold. The centring force is said
                // again, and the one other thing asked of the device as it
                // opened - whether it can be made to knock - is asked
                // again if it said no, for the same reason: a wheel busy
                // with its own power-on sweep can refuse and then be
                // perfectly able a moment later.
                s_centering = -1;
                if (s_haptic && !s_canHapticRumble && SDL_InitHapticRumble(s_haptic))
                {
                    s_canHapticRumble = true;
                    fprintf(stderr, "[wheel] force feedback: knocks yes after all (the wheel was busy at first)\n");
                }
                // A pedal on this axis that had no resting place to work
                // from takes the value it was at as it started to move,
                // which is near enough where it rests. A wheel we know
                // never gets here: its model said so at the open.
                for (Step step : { Step::Throttle, Step::Brake, Step::Clutch })
                    if (AxisOf(step) == a && !s_pedals[Slot(step)].ready)
                    {
                        // Where it was as it started to move is near enough
                        // where it rests, and it can go as far as its axis.
                        const int16_t at = e.jaxis.value;
                        s_pedals[Slot(step)].Start(at, int16_t(at >= 0 ? -32768 : 32767));
                        fprintf(stderr, "[wheel] %s: axis %d first reported %+d, taken as where it rests\n", StepName(step),
                            a, int(at));
                    }
            }
        }
        else if (e.type == SDL_EVENT_JOYSTICK_ADDED)
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
        {
            float turn = s_steering.Fraction(AxisNow(s_axes.steer));
            if (settings::GetBool(settings::Id::InvertSteering))
                turn = -turn;
            r.steer = wheelmap::Steer(turn, settings::GetInt(settings::Id::SteeringRange), s_fullTurn,
                settings::GetFloat(settings::Id::SteeringDeadzone), settings::GetFloat(settings::Id::SteeringResponse));
        }
        r.throttle = PedalValue(Step::Throttle);
        r.brake = PedalValue(Step::Brake);
        r.clutch = PedalValue(Step::Clutch);
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
        if (!s_joystick)
            return;
        // An autocentre level is one cheap call, so it is said again every
        // second: whenever the wheel became ready, it ends up holding what
        // the setting says. A spring effect is not re-made that way - that
        // would mean tearing one down and building another every second -
        // so it is only (re)made when the setting itself changes, or when
        // the device first speaks (HandleEvent).
        const Uint64 now = SDL_GetTicks();
        const bool again = s_canAutocentre && now - s_centeringSaid >= 1000;
        if (again)
            s_centeringSaid = now;
        ApplyCentering(again);
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

    void BeginAssign(Step step)
    {
        if (!s_joystick)
            return;
        s_assigning = true;
        s_assignStep = step;
        s_assignFrom = AxisStates(false);
        s_assignLo.assign(s_assignFrom.size(), 0);
        s_assignHi.assign(s_assignFrom.size(), 0);
        s_assignTracked.assign(s_assignFrom.size(), false);
        s_assignButtonsFrom = ButtonsNow();
    }

    Candidate Watch()
    {
        if (!s_assigning || !s_joystick)
            return {};
        if (StepIsButton(s_assignStep))
        {
            // Whatever has gone down since the step began.
            const std::array<bool, kMaxButtons> held = ButtonsNow();
            Candidate c;
            for (int b = 0; b < kMaxButtons; b++)
                if (held[size_t(b)] && !s_assignButtonsFrom[size_t(b)])
                {
                    c.found = true;
                    c.button = b;
                }
            return c;
        }
        // Fold in where everything is now, keeping the range each axis has
        // covered. An axis that has never reported is left alone.
        for (size_t a = 0; a < s_assignFrom.size(); a++)
        {
            if (a >= s_axisSeen.size() || !s_axisSeen[a])
                continue;
            const int32_t v = AxisNow(int(a));
            if (!s_assignTracked[a])
            {
                s_assignLo[a] = s_assignHi[a] = v;
                s_assignTracked[a] = true;
                continue;
            }
            s_assignLo[a] = std::min(s_assignLo[a], v);
            s_assignHi[a] = std::max(s_assignHi[a], v);
        }
        // The axis that has covered the most ground is the one being moved.
        size_t best = 0;
        int32_t widest = -1;
        for (size_t a = 0; a < s_assignLo.size(); a++)
        {
            if (!s_assignTracked[a])
                continue;
            const int32_t span = s_assignHi[a] - s_assignLo[a];
            if (span > widest)
            {
                widest = span;
                best = a;
            }
        }
        if (widest < kAssignMoved)
            return {};
        Candidate c;
        c.found = true;
        c.axis = int(best);
        // Where it is being held, and the end of the range furthest from
        // that, which is where it was before the player touched it.
        const int32_t held = AxisNow(int(best));
        const int32_t far = std::abs(held - s_assignLo[best]) > std::abs(held - s_assignHi[best]) ? s_assignLo[best]
                                                                                                 : s_assignHi[best];
        c.full = int16_t(std::clamp(held, -32768, 32767));
        c.rest = int16_t(std::clamp(far, -32768, 32767));
        const float span = wheelmap::RestsCentred(c.rest) ? float(wheelmap::kAxisMax) : float(2 * wheelmap::kAxisMax);
        c.moved = std::clamp(float(widest) / span, 0.0f, 1.0f);
        // Which way round the wheel is needs no deciding here: the two ends
        // say it between them (wheel_map.h's Steering).
        return c;
    }

    bool ConfirmAssign()
    {
        if (!s_assigning)
            return false;
        const Candidate c = Watch();
        if (!c.found)
            return false;  // nothing done yet: the step goes on
        s_assigning = false;
        if (StepIsButton(s_assignStep))
        {
            const wheelmap::Control control = wheelmap::Control(size_t(s_assignStep) - size_t(Step::ButtonA));
            settings::Set(ButtonOption(control), double(c.button));
            TakeBindings();
            fprintf(stderr, "[wheel] %s: button %d\n", StepName(s_assignStep), c.button);
            return true;
        }
        fprintf(stderr, "[wheel] %s: axis %d, held at %+d (it moved %.0f%% of its travel)\n", StepName(s_assignStep), c.axis,
            int(c.full), double(c.moved) * 100.0);
        switch (s_assignStep)
        {
        case Step::SteerLeft:
            settings::Set(settings::Id::WheelSteerAxis, double(c.axis));
            settings::Set(settings::Id::SteerLeft, double(c.full));
            // The ends carry which way round the wheel is, so the manual
            // flip starts clear: whoever wants it can still set it.
            settings::Set(settings::Id::InvertSteering, 0.0);
            break;
        case Step::SteerRight:
            settings::Set(settings::Id::WheelSteerAxis, double(c.axis));
            settings::Set(settings::Id::SteerRight, double(c.full));
            break;
        default:
            // Both ends, kept for the next run too: a pedal that stops
            // short of its axis still reads as floored, and one sharing an
            // axis with another keeps to its own half.
            settings::Set(AxisOption(s_assignStep), double(c.axis));
            settings::Set(RestOption(s_assignStep), double(c.rest));
            settings::Set(FullOption(s_assignStep), double(c.full));
            break;
        }
        Calibrate(false);
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

    Step AssigningStep()
    {
        return s_assignStep;
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
        for (settings::Id id : { settings::Id::InvertSteering, settings::Id::ThrottleRest, settings::Id::ThrottleFull,
                                 settings::Id::BrakeRest, settings::Id::BrakeFull, settings::Id::ClutchRest,
                                 settings::Id::ClutchFull, settings::Id::SteerLeft, settings::Id::SteerRight })
            settings::Reset(id);
        for (size_t i = 0; i < wheelmap::kControlCount; i++)
            settings::Reset(ButtonOption(wheelmap::Control(i)));
        if (s_joystick)
            TakeBindings();
        if (s_joystick)
            Calibrate(false);
    }
}
