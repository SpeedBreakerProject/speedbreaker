// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// A USB steering wheel as an Xbox 360 controller (Settings > Wheel): which
// of a joystick's axes is the wheel and which are its pedals, how far a
// turn and a pedal press reach, and which of its buttons are the pad's.
// Header only and SDL-free: hid/wheel.cpp samples the device and feeds the
// result to the pad, ui/settings_menu.cpp draws the page, and
// tests/wheel_map_test.cpp checks the rules.
//
// A wheel is not a gamepad: SDL has no gamepad mapping for one, so it
// arrives as a plain joystick whose axes and buttons mean nothing by
// themselves. Three things sort that out, in this order:
//
//   - The model table below, for the wheels we know (the Logitech G29 and
//     its family): which axis is the wheel, which pedal is which, how far
//     the wheel turns, and what its buttons are.
//   - Where the axes rest when the game opens the device, which tells a
//     wheel (centred) from a pedal (held at one end by its spring) whatever
//     the model says, so an unknown wheel still steers and brakes.
//   - The player, through Settings > Wheel > Pedals, whose stored axis
//     numbers beat both (the pedals of a wheel we don't know are the one
//     thing the first two can get wrong: three springs look alike).
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hid::wheelmap
{
    // XInput's button bits, as the game reads them (xbox.h's
    // XAMINPUT_GAMEPAD_*; wheel.cpp asserts they agree), as in key_map.h.
    namespace button
    {
        constexpr uint16_t DpadUp = 0x0001, DpadDown = 0x0002, DpadLeft = 0x0004, DpadRight = 0x0008;
        constexpr uint16_t Start = 0x0010, Back = 0x0020, LeftThumb = 0x0040, RightThumb = 0x0080;
        constexpr uint16_t LeftShoulder = 0x0100, RightShoulder = 0x0200;
        constexpr uint16_t A = 0x1000, B = 0x2000, X = 0x4000, Y = 0x8000;
    }

    // A joystick hat's bits (SDL_HAT_*; wheel.cpp asserts they agree).
    namespace hat
    {
        constexpr uint8_t Centred = 0x00, Up = 0x01, Right = 0x02, Down = 0x04, Left = 0x08;
    }

    // The full deflection of a joystick axis, as SDL reports it. The
    // negative end is one more (-32768), which the readings clamp to this.
    constexpr int kAxisMax = 32767;

    // What one of a wheel's buttons does. The game shifts gears with the
    // right stick (README, Controls), so a wheel's paddles push it up and
    // down: ShiftUp and ShiftDown are not buttons the pad has.
    enum class Role : uint8_t { Button, ShiftUp, ShiftDown };

    struct Binding
    {
        int index = 0;             // the joystick button, as SDL numbers them
        uint16_t button = 0;       // Role::Button: which pad button it is
        Role role = Role::Button;
    };

    // Which axis is which. -1: the device hasn't got it (a wheel without a
    // clutch), or nothing has said which it is yet.
    struct Axes
    {
        int steer = -1, throttle = -1, brake = -1, clutch = -1;
    };

    // A wheel we know by its USB ids.
    struct Model
    {
        const char* name;
        uint16_t vendor = 0, product = 0;  // 0, 0: the fallback for every other wheel
        Axes axes;
        // Where its pedals rest, which SDL cannot be asked for: a joystick
        // reports an axis only when it moves, so a pedal nobody has touched
        // reads 0 whatever it is really sitting at. 0 here: unknown, and
        // then a pedal reads nothing until its axis speaks for itself.
        int16_t pedalRest = 0;
        int fullTurnDegrees = 0;           // lock to lock; 0: unknown (Steering Range then has nothing to go by)
        std::span<const Binding> buttons;
    };

    // The Logitech wheels' buttons, in the PlayStation layout they report
    // over USB (the G29's own numbering, which the G920 and G923 share).
    // Matched to the pad by position, as a PlayStation controller's are
    // (README, Controls): Cross is A, Circle is B, Square is X, Triangle is
    // Y. The paddles shift gears; R2 and L2, the two levers behind them,
    // are the shoulder buttons.
    inline constexpr Binding kLogitechButtons[] = {
        { 0, button::A }, { 1, button::X }, { 2, button::B }, { 3, button::Y },
        { 4, 0, Role::ShiftUp },    // R1: the right paddle
        { 5, 0, Role::ShiftDown },  // L1: the left paddle
        { 6, button::RightShoulder }, { 7, button::LeftShoulder },
        { 8, button::Back },        // Share / Select
        { 9, button::Start },       // Options / Start
        { 10, button::RightThumb }, { 11, button::LeftThumb },
    };

    // Any other wheel: the order nearly every joystick reports its face
    // buttons in, so the menus have A, B and a way out of them. Settings >
    // Wheel's own rows (and NFSMW_WHEEL_BUTTONS) change it.
    inline constexpr Binding kGenericButtons[] = {
        { 0, button::A }, { 1, button::B }, { 2, button::X }, { 3, button::Y },
        { 4, button::LeftShoulder }, { 5, button::RightShoulder },
        { 6, button::Back }, { 7, button::Start },
        { 8, button::LeftThumb }, { 9, button::RightThumb },
    };

    // The wheels we know.
    //
    // The G29's numbers are measured, on a Logitech G29 (046d:c24f) over
    // USB on macOS: the wheel is axis 0 and turning left reads negative, as
    // the pad wants; the pedals are throttle 2, brake 3, clutch 1 - not the
    // order they sit in on the floor - and each rests at +32767 and runs to
    // -32768 as it goes down. Its buttons are kLogitechButtons, confirmed
    // one at a time, and its hat is the D-pad.
    //
    // The rest of the family is taken to report the same way, since they
    // share the G29's report format, but only the G29 has been measured.
    // Nothing here is trusted blindly in any case: Detect() checks the axes
    // against where they rest, and Settings > Wheel > Calibrate overrules
    // the lot.
    inline constexpr Model kModels[] = {
        { "Logitech G29 Driving Force", 0x046D, 0xC24F, { 0, 2, 3, 1 }, kAxisMax, 900, kLogitechButtons },
        { "Logitech G29 Driving Force", 0x046D, 0xC260, { 0, 2, 3, 1 }, kAxisMax, 900, kLogitechButtons },
        { "Logitech G920 Driving Force", 0x046D, 0xC262, { 0, 2, 3, 1 }, kAxisMax, 900, kLogitechButtons },
        { "Logitech G923 Driving Force", 0x046D, 0xC266, { 0, 2, 3, 1 }, kAxisMax, 900, kLogitechButtons },
        { "Logitech G923 Driving Force", 0x046D, 0xC26E, { 0, 2, 3, 1 }, kAxisMax, 900, kLogitechButtons },
        { "Logitech G27 Racing Wheel", 0x046D, 0xC29B, { 0, 2, 3, 1 }, kAxisMax, 900, kLogitechButtons },
        { "Logitech G25 Racing Wheel", 0x046D, 0xC299, { 0, 2, 3, 1 }, kAxisMax, 900, kLogitechButtons },
        { "Logitech Driving Force GT", 0x046D, 0xC29A, { 0, 2, 3, -1 }, kAxisMax, 900, kLogitechButtons },
        { "Logitech Driving Force Pro", 0x046D, 0xC298, { 0, 2, 3, -1 }, kAxisMax, 900, kLogitechButtons },
        { "Steering wheel", 0, 0, { 0, 1, 2, 3 }, 0, 0, kGenericButtons },
    };
    constexpr size_t kModelCount = sizeof(kModels) / sizeof(kModels[0]);
    static_assert(kModels[kModelCount - 1].vendor == 0 && kModels[kModelCount - 1].product == 0,
        "the last model is the fallback for every wheel we don't know");

    inline const Model& Fallback()
    {
        return kModels[kModelCount - 1];
    }

    // The wheel with these USB ids, or the fallback.
    inline const Model& ModelFor(uint16_t vendor, uint16_t product)
    {
        for (const Model& m : kModels)
            if (m.vendor == vendor && m.product == product && m.vendor != 0)
                return m;
        return Fallback();
    }

    inline std::string Lowered(std::string_view text)
    {
        std::string out(text);
        for (char& c : out)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        return out;
    }

    // Is this joystick a wheel? SDL says so itself for the ones it knows
    // (SDL_JOYSTICK_TYPE_WHEEL), our own table for the rest, and failing
    // both the name, which is what a wheel SDL and we have never heard of
    // leaves to go on. `axes` and `buttons` are what the device reports: a
    // wheel has an axis to steer with and at least one pedal, so a name
    // alone never makes a two-axis gamepad into one.
    inline bool LooksLikeWheel(bool sdlSaysWheel, uint16_t vendor, uint16_t product, std::string_view name, int axes, int buttons)
    {
        if (axes < 2 || buttons < 1)
            return false;
        if (sdlSaysWheel)
            return true;
        if (&ModelFor(vendor, product) != &Fallback())
            return true;
        const std::string lowered = Lowered(name);
        for (std::string_view hint : { "wheel", "driving force", "racing", "g29", "g920", "g923", "g27", "g25",
                                       "thrustmaster", "fanatec", "logitech momo" })
            if (lowered.find(hint) != std::string::npos)
                return true;
        return false;
    }

    // Where an axis rests, as a share of full deflection: a wheel's own
    // axis rests centred, a pedal is held at one end by its spring. Half
    // way is nowhere near either, so it splits them with room to spare (a
    // wheel a little off centre at launch, a pedal that rests a touch short
    // of its end).
    constexpr float kRestsCentred = 0.5f;

    constexpr bool RestsCentred(int16_t rest)
    {
        constexpr int limit = int(kRestsCentred * kAxisMax);
        return rest >= -limit && rest <= limit;
    }

    // Which axis is which, for a device whose axes rest here. `preferred`
    // is the model's (or the player's, which wheel.cpp puts in first).
    //
    //   - The wheel's own axis is the preferred one if it rests centred,
    //     else the first axis that does. A wheel always has one and a pedal
    //     never does, so this holds for a wheel we have never heard of.
    //   - The pedals keep their preferred axes, but only if every one of
    //     them the device has is free and resting at an end, as a pedal
    //     under its spring does. If any doesn't check out, the model's
    //     pedal layout is wrong for this device and is dropped whole rather
    //     than half believed: the axes left over go to the throttle, the
    //     brake and the clutch in the order the device reports them, which
    //     is the order nearly every wheel has them in.
    //   - A clutch is looked for only where the model asked for one.
    //
    // So a device with one centred axis and its pedals at their ends steers
    // and brakes whatever we know about it, and a wheel whose pedals come
    // out in the wrong places is the one thing the player has to put right
    // (Settings > Wheel > Pedals, whose axes arrive here as `preferred`,
    // and are not guesses: wheel.cpp keeps them whatever this says).
    inline Axes Detect(std::span<const int16_t> rest, const Axes& preferred)
    {
        const int count = int(rest.size());
        auto has = [&](int axis) { return axis >= 0 && axis < count; };
        Axes out;
        std::vector<bool> taken(size_t(std::max(0, count)), false);
        if (has(preferred.steer) && RestsCentred(rest[size_t(preferred.steer)]))
            out.steer = preferred.steer;
        else
            for (int a = 0; a < count; a++)
                if (RestsCentred(rest[size_t(a)]))
                {
                    out.steer = a;
                    break;
                }
        if (out.steer >= 0)
            taken[size_t(out.steer)] = true;
        int* const pedals[] = { &out.throttle, &out.brake, &out.clutch };
        const int wanted[] = { preferred.throttle, preferred.brake, preferred.clutch };
        // Does the model's pedal layout hold for this device? Its throttle
        // has to be there at all, and every pedal it names has to be free
        // and resting at an end.
        bool layoutHolds = has(wanted[0]);
        for (size_t p = 0; p < 3 && layoutHolds; p++)
            if (has(wanted[p]) && (taken[size_t(wanted[p])] || RestsCentred(rest[size_t(wanted[p])])))
                layoutHolds = false;
        if (layoutHolds)
        {
            for (size_t p = 0; p < 3; p++)
                if (has(wanted[p]))
                {
                    *pedals[p] = wanted[p];
                    taken[size_t(wanted[p])] = true;
                }
            return out;
        }
        // It doesn't: the axes left over, in the order the device reports
        // them.
        for (int next = 0, p = 0; p < 3; p++)
        {
            if (p == 2 && wanted[2] < 0)
                break;  // this wheel has no clutch to find
            while (next < count && taken[size_t(next)])
                next++;
            if (next >= count)
                break;
            *pedals[p] = next;
            taken[size_t(next)] = true;
        }
        return out;
    }

    // A pedal, from where it rests to the floor. A wheel's pedals rest at
    // one end of their axis and run to the other, but which end is which
    // differs between wheels and between one wheel's modes: so the resting
    // place is read when the device is opened (or when the player assigns
    // the pedal), and how far the pedal is down is how far it has moved
    // from there, whichever way that is.
    //
    // Its travel is from that resting place to the far end of the axis, not
    // the furthest it has been seen to go: a cautious first press is then
    // never mistaken for the floor. A pedal that cannot reach the end of
    // its own axis reads short for it, and NFSMW_WHEEL_LOG=1 shows what
    // each one reads, raw and as the game gets it.
    struct Pedal
    {
        int16_t idle = 0;
        int32_t travel = kAxisMax;  // how far from idle is fully pressed
        // Do we know where it rests? SDL reports an axis only when it
        // moves, so one nobody has touched reads 0 whatever it is sitting
        // at: for a pedal resting at +32767 that would be a floored pedal.
        // Until the resting place is known from the wheel's model or from
        // the axis itself, the pedal reads nothing at all, which is the
        // safe way to be wrong.
        bool ready = false;

        // The axis rests here. One resting at an end can travel the whole
        // way to the other; one resting centred, half of it.
        constexpr void Start(int16_t rest)
        {
            idle = rest;
            travel = RestsCentred(rest) ? kAxisMax : kAxisMax + (rest < 0 ? -int(rest) : int(rest));
            ready = true;
        }

        constexpr float Value(int16_t raw) const
        {
            if (!ready || travel <= 0)
                return 0.0f;
            const int32_t from = int32_t(raw) - int32_t(idle);
            const int32_t moved = from < 0 ? -from : from;
            return std::clamp(float(moved) / float(travel), 0.0f, 1.0f);
        }
    };

    // An axis read the other way round (Settings > Wheel > Invert
    // Steering, or a wheel whose axis runs the opposite way): -32768 has no
    // positive twin, so it clamps to the end of the other side instead of
    // wrapping back to itself.
    constexpr int16_t Flip(int16_t raw)
    {
        return int16_t(-std::clamp(int(raw), -kAxisMax, kAxisMax));
    }

    // The wheel's axis as the game's left stick, -1 (full left) to 1.
    //
    //   - `range` degrees of the wheel's turn reach full lock, out of the
    //     `full` degrees it turns altogether (either 0: all of it). We can't
    //     tell the wheel itself to turn less - that needs the maker's own
    //     driver - so a smaller range means full lock comes up before the
    //     wheel's stop does.
    //   - `deadzone` of the turn either side of centre reads as straight
    //     ahead, and the rest is rescaled so it still reaches full lock.
    //   - `response` bends what's left: 1 is straight through, above 1 is
    //     gentler near the centre and sharper near full lock, below 1 the
    //     other way round.
    inline float Steer(int16_t raw, int range, int full, float deadzone, float response)
    {
        float x = std::clamp(float(raw) / float(kAxisMax), -1.0f, 1.0f);
        const float sign = x < 0 ? -1.0f : 1.0f;
        float m = std::abs(x);
        deadzone = std::clamp(deadzone, 0.0f, 0.95f);
        if (m <= deadzone)
            return 0.0f;
        m = (m - deadzone) / (1.0f - deadzone);
        if (range > 0 && full > 0 && range < full)
            m = m * float(full) / float(range);
        m = std::min(1.0f, m);
        if (response > 0.0f && response != 1.0f)
            m = std::pow(m, response);
        return sign * std::clamp(m, 0.0f, 1.0f);
    }

    // What the wheel's buttons and its hat amount to. The hat is the D-pad;
    // the paddles push the right stick up and down, which is how the game
    // shifts gears.
    struct Pressed
    {
        uint16_t buttons = 0;
        int16_t rightY = 0;
    };

    constexpr Pressed Resolve(std::span<const bool> buttons, uint8_t hatBits, std::span<const Binding> map)
    {
        Pressed out;
        bool up = false, down = false;
        for (const Binding& b : map)
        {
            if (b.index < 0 || size_t(b.index) >= buttons.size() || !buttons[size_t(b.index)])
                continue;
            switch (b.role)
            {
            case Role::Button: out.buttons |= b.button; break;
            case Role::ShiftUp: up = true; break;
            case Role::ShiftDown: down = true; break;
            }
        }
        // Both paddles at once is no gear change either way (as holding
        // both keys of a stick's pair centres it: hid/key_map.h).
        out.rightY = int16_t(up == down ? 0 : up ? kAxisMax : -kAxisMax);
        if (hatBits & hat::Up)
            out.buttons |= button::DpadUp;
        if (hatBits & hat::Down)
            out.buttons |= button::DpadDown;
        if (hatBits & hat::Left)
            out.buttons |= button::DpadLeft;
        if (hatBits & hat::Right)
            out.buttons |= button::DpadRight;
        return out;
    }

    // A pad control's name in NFSMW_WHEEL_BUTTONS, and in the log line that
    // says what a wheel's buttons do.
    inline constexpr std::pair<std::string_view, Binding> kNames[] = {
        { "A", { 0, button::A } }, { "B", { 0, button::B } }, { "X", { 0, button::X } }, { "Y", { 0, button::Y } },
        { "LB", { 0, button::LeftShoulder } }, { "RB", { 0, button::RightShoulder } },
        { "BACK", { 0, button::Back } }, { "START", { 0, button::Start } },
        { "LS", { 0, button::LeftThumb } }, { "RS", { 0, button::RightThumb } },
        { "UP", { 0, button::DpadUp } }, { "DOWN", { 0, button::DpadDown } },
        { "LEFT", { 0, button::DpadLeft } }, { "RIGHT", { 0, button::DpadRight } },
        { "SHIFTUP", { 0, 0, Role::ShiftUp } }, { "SHIFTDOWN", { 0, 0, Role::ShiftDown } },
        { "NONE", { 0, 0 } },
    };

    // What a Binding does, for the log.
    inline std::string_view NameOf(const Binding& b)
    {
        for (const auto& [name, binding] : kNames)
            if (binding.role == b.role && (b.role != Role::Button || binding.button == b.button))
                return name;
        return "?";
    }

    // NFSMW_WHEEL_BUTTONS="0=A,2=B,8=BACK,9=START,4=SHIFTUP": what a wheel's
    // buttons do, for a wheel whose numbering we have wrong (the log's
    // "[wheel] button n" lines say which is which). Every button named
    // replaces what the model gave it; NONE takes one out, and the rest of
    // the model's bindings stay. Returns the bindings, and in `error` the
    // first item it couldn't read ("" when all of them were fine).
    inline std::vector<Binding> ParseBindings(std::string_view text, std::span<const Binding> from, std::string& error)
    {
        std::vector<Binding> out(from.begin(), from.end());
        error.clear();
        while (!text.empty())
        {
            const size_t comma = text.find(',');
            std::string_view item = text.substr(0, comma);
            text = comma == std::string_view::npos ? std::string_view() : text.substr(comma + 1);
            while (!item.empty() && std::isspace(static_cast<unsigned char>(item.front())))
                item.remove_prefix(1);
            while (!item.empty() && std::isspace(static_cast<unsigned char>(item.back())))
                item.remove_suffix(1);
            if (item.empty())
                continue;
            const size_t equals = item.find('=');
            if (equals == std::string_view::npos || equals == 0)
            {
                if (error.empty())
                    error = std::string(item);
                continue;
            }
            int index = 0;
            bool digits = true;
            for (char c : item.substr(0, equals))
                if (std::isdigit(static_cast<unsigned char>(c)))
                    index = index * 10 + (c - '0');
                else
                    digits = false;
            std::string name;
            for (char c : item.substr(equals + 1))
                name += char(std::toupper(static_cast<unsigned char>(c)));
            const Binding* found = nullptr;
            for (const auto& [known, binding] : kNames)
                if (known == name)
                    found = &binding;
            if (!digits || index > 255 || !found)
            {
                if (error.empty())
                    error = std::string(item);
                continue;
            }
            std::erase_if(out, [&](const Binding& b) { return b.index == index; });
            if (name != "NONE")
                out.push_back({ index, found->button, found->role });
        }
        std::stable_sort(out.begin(), out.end(), [](const Binding& a, const Binding& b) { return a.index < b.index; });
        return out;
    }

    // Settings > Wheel > Force Feedback: the rumble the game asks for
    // (XInput's two motor speeds) as one strength, 0 to 1, at `percent` of
    // full. The stronger motor has it: the game uses the two together for
    // one knock.
    inline float RumbleStrength(uint16_t low, uint16_t high, int percent)
    {
        const float strength = float(std::max(low, high)) / 65535.0f;
        return std::clamp(strength * float(std::clamp(percent, 0, 100)) / 100.0f, 0.0f, 1.0f);
    }

    // Settings > Wheel > Centering Force as a spring effect's coefficient
    // (how fast the force grows away from the middle, 0 to 0x7FFF) and its
    // saturation (how far it grows, 0 to 0xFFFF), for a wheel whose force
    // feedback takes a spring rather than an autocentre level.
    inline uint16_t SpringLevel(int percent)
    {
        return uint16_t(std::clamp(percent, 0, 100) * 0x7FFF / 100);
    }

    inline uint16_t SpringSaturation(int percent)
    {
        return uint16_t(std::clamp(percent, 0, 100) * 0xFFFF / 100);
    }
}
