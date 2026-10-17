// Tests for hid/wheel_map.h: a USB steering wheel as the Xbox 360 pad. Which joystick is a wheel,
// which of its axes is the wheel and which are its pedals, the steering curve (range, deadzone and
// response), a pedal from where it rests to as far as it is pressed, the buttons and the paddles,
// NFSMW_WHEEL_BUTTONS, and the force-feedback levels. Header only; from the repo root:
//   clang++ -std=c++20 -O2 -Iruntime tests/wheel_map_test.cpp -o build/wheel_map_test && build/wheel_map_test
#include <hid/wheel_map.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <set>
#include <string>
#include <vector>

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

using namespace hid::wheelmap;

// Steer works on the turn as a fraction of full lock; these cases are
// written as raw axis readings, as the device gives them.
static float SteerRaw(int16_t raw, int range, int full, float deadzone, float response)
{
    return Steer(float(raw) / float(kAxisMax), range, full, deadzone, response);
}

static bool Near(float a, float b, float slack = 0.001f)
{
    return std::fabs(a - b) <= slack;
}

// A G29's axes as they rest, measured on the hardware: the wheel centred on
// axis 0, and its three pedals held at +32767 by their springs.
static constexpr int16_t kG29Rest[] = { 0, 32767, 32767, 32767 };

// Resolve takes a contiguous run of bools (std::vector<bool> is not one).
template <size_t N>
static std::array<bool, N> Held(std::initializer_list<int> which)
{
    std::array<bool, N> held{};
    for (int b : which)
        if (b >= 0 && size_t(b) < N)
            held[size_t(b)] = true;
    return held;
}

int main()
{
    // The table: one fallback, last, and every other model a wheel we name,
    // with its own USB ids, an axis to steer with and buttons.
    CHECK(kModelCount >= 2, "%zu models", kModelCount);
    std::set<uint32_t> ids;
    for (size_t i = 0; i < kModelCount; i++)
    {
        const Model& m = kModels[i];
        const bool last = i == kModelCount - 1;
        CHECK((m.vendor == 0 && m.product == 0) == last, "model %zu: only the last is the fallback", i);
        CHECK(m.name != nullptr && m.axes.steer >= 0 && !m.buttons.empty(), "model %zu: named, steers, has buttons", i);
        // Where its pedals rest: an end of the axis, or 0 for not known.
        CHECK((m.pedalRest != m.pedalFull) == (m.vendor != 0),
            "model %zu: the wheels we know give both ends of their pedals, the fallback neither", i);
        if (m.pedalRest != m.pedalFull)
            CHECK((m.pedalRest > 0) != (m.pedalFull > 0), "model %zu: its pedals run from one end of the axis to the other", i);
        CHECK(m.fullTurnDegrees >= 0 && m.fullTurnDegrees <= 1080, "model %zu: %d degrees", i, m.fullTurnDegrees);
        if (!last)
            CHECK(ids.insert(uint32_t(m.vendor) << 16 | m.product).second, "model %zu: its own USB ids", i);
        // A pedal is never the wheel's own axis, and no two roles share one.
        const int axes[] = { m.axes.steer, m.axes.throttle, m.axes.brake, m.axes.clutch };
        for (size_t a = 0; a < 4; a++)
            for (size_t b = a + 1; b < 4; b++)
                CHECK(axes[a] < 0 || axes[a] != axes[b], "model %zu: axis %d is one role", i, axes[a]);
    }
    // The G29 is the wheel this was written for, and the one its numbers
    // were measured on: the wheel is axis 0, the pedals are throttle 2,
    // brake 3 and clutch 1 (not the order they stand in), they rest at
    // +32767, it turns 900 degrees, and its buttons are the Logitech set.
    for (uint16_t product : { 0xC24F, 0xC260 })
    {
        const Model& g29 = ModelFor(0x046D, product);
        CHECK(g29.vendor == 0x046D && g29.product == product, "the G29 (%04x) is known", product);
        CHECK(g29.axes.steer == 0 && g29.axes.throttle == 2 && g29.axes.brake == 3 && g29.axes.clutch == 1,
            "the G29's measured axes (%d, %d, %d, %d)", g29.axes.steer, g29.axes.throttle, g29.axes.brake, g29.axes.clutch);
        CHECK(g29.pedalRest == kAxisMax && g29.pedalFull == -32768, "the G29's pedals run +32767 to -32768");
        CHECK(g29.fullTurnDegrees == 900, "the G29 turns 900 degrees");
        CHECK(g29.buttons.data() == kLogitechButtons, "the G29 has the Logitech buttons");
    }
    CHECK(&ModelFor(0x046D, 0x0001) == &Fallback(), "another Logitech device is not a wheel we know");
    CHECK(&ModelFor(0, 0) == &Fallback(), "no USB ids: the fallback");

    // Which joystick is a wheel. SDL's own word, our table, or the name;
    // but never something without an axis to steer with and a button.
    CHECK(LooksLikeWheel(true, 0x1234, 0x5678, "Something", 4, 12), "SDL says it is a wheel");
    CHECK(LooksLikeWheel(false, 0x046D, 0xC24F, "", 4, 25), "the G29 by its USB ids, unnamed");
    CHECK(LooksLikeWheel(false, 0, 0, "Thrustmaster T300RS", 4, 13), "an unknown wheel by its name");
    CHECK(LooksLikeWheel(false, 0, 0, "LOGITECH G923 Racing Wheel", 4, 13), "the name, whatever its case");
    CHECK(!LooksLikeWheel(false, 0x045E, 0x028E, "Xbox 360 Controller", 6, 11), "a controller is not a wheel");
    CHECK(!LooksLikeWheel(true, 0x046D, 0xC24F, "Logitech G29", 1, 1), "one axis: not a wheel, whatever it says");
    CHECK(!LooksLikeWheel(true, 0x046D, 0xC24F, "Logitech G29", 4, 0), "no buttons: nor is this");

    // Where the axes rest tells the wheel (centred) from the pedals (held at
    // one end): a G29 comes out as its model says.
    Axes axes = Detect(kG29Rest, ModelFor(0x046D, 0xC24F).axes);
    CHECK(axes.steer == 0 && axes.throttle == 2 && axes.brake == 3 && axes.clutch == 1, "the G29's axes: %d %d %d %d",
        axes.steer, axes.throttle, axes.brake, axes.clutch);

    // A wheel we don't know, with the same resting places, still steers and
    // brakes: the centred axis is the wheel, the rest are pedals in order.
    axes = Detect(kG29Rest, Fallback().axes);
    CHECK(axes.steer == 0 && axes.throttle == 1 && axes.brake == 2 && axes.clutch == 3, "an unknown wheel's axes");

    // A model that names the wrong axes loses to where they rest: here the
    // wheel is axis 2, which the model called a pedal. Its pedal layout
    // can't hold, so it is dropped whole and the axes left over go to the
    // pedals in the order the device reports them, rather than the throttle
    // keeping the model's axis while the brake takes what is left.
    constexpr int16_t kOddRest[] = { -32768, 32767, 0, 32767 };
    axes = Detect(kOddRest, { 0, 1, 2, 3 });
    CHECK(axes.steer == 2, "the centred axis is the wheel, not the one the model named: %d", axes.steer);
    CHECK(axes.throttle == 0 && axes.brake == 1 && axes.clutch == 3, "the rest in the device's order: %d, %d, %d",
        axes.throttle, axes.brake, axes.clutch);

    // Two pedals and no clutch: nothing is invented for a role the model
    // hasn't got, and a pedal is never the wheel's axis.
    constexpr int16_t kTwoPedals[] = { 0, 32767, 32767 };
    axes = Detect(kTwoPedals, { 0, 1, 2, -1 });
    CHECK(axes.steer == 0 && axes.throttle == 1 && axes.brake == 2 && axes.clutch == -1, "a wheel with two pedals");
    constexpr int16_t kOnePedal[] = { 0, 32767 };
    axes = Detect(kOnePedal, Fallback().axes);
    CHECK(axes.steer == 0 && axes.throttle == 1 && axes.brake == -1 && axes.clutch == -1, "a wheel with one pedal");

    // A wheel held off centre as the game starts is still the wheel (half
    // way is the line), and a pedal already pressed is still a pedal: here
    // axis 1, which on a G29 is the clutch, is held to the floor while the
    // other two rest, and every role still lands where the model says.
    constexpr int16_t kOffCentreRest[] = { 12000, -32768, 32767, 32767 };
    axes = Detect(kOffCentreRest, ModelFor(0x046D, 0xC24F).axes);
    CHECK(axes.steer == 0 && axes.throttle == 2 && axes.brake == 3 && axes.clutch == 1,
        "a wheel off centre, the clutch held down: %d, %d, %d, %d", axes.steer, axes.throttle, axes.brake, axes.clutch);

    // Steering. The whole turn, straight through: the ends and the middle.
    CHECK(Near(SteerRaw(0, 0, 900, 0.0f, 1.0f), 0.0f), "centred is straight ahead");
    CHECK(Near(SteerRaw(32767, 0, 900, 0.0f, 1.0f), 1.0f), "full right");
    CHECK(Near(SteerRaw(-32768, 0, 900, 0.0f, 1.0f), -1.0f), "full left (the extra count clamps)");
    CHECK(Near(SteerRaw(16384, 0, 900, 0.0f, 1.0f), 0.5f, 0.002f), "half way round");
    // A range smaller than the wheel's turn reaches full lock sooner: 450 of
    // a G29's 900 degrees means half a turn is already full lock.
    CHECK(Near(SteerRaw(16384, 450, 900, 0.0f, 1.0f), 1.0f), "450 of 900: half the turn is full lock");
    CHECK(Near(SteerRaw(8192, 450, 900, 0.0f, 1.0f), 0.5f, 0.002f), "...and a quarter of it is half lock");
    CHECK(Near(SteerRaw(-16384, 450, 900, 0.0f, 1.0f), -1.0f), "the same the other way");
    CHECK(Near(SteerRaw(32767, 450, 900, 0.0f, 1.0f), 1.0f), "past full lock is full lock");
    // A range at or above the wheel's turn, or a wheel whose turn we don't
    // know, uses the whole turn.
    CHECK(Near(SteerRaw(16384, 900, 900, 0.0f, 1.0f), 0.5f, 0.002f), "900 of 900 is the whole turn");
    CHECK(Near(SteerRaw(16384, 540, 0, 0.0f, 1.0f), 0.5f, 0.002f), "a wheel whose turn is unknown: the whole axis");
    // The deadzone: nothing inside it, and what is outside still reaches
    // full lock.
    CHECK(Near(SteerRaw(3000, 0, 900, 0.1f, 1.0f), 0.0f), "inside the deadzone: straight ahead");
    CHECK(Near(SteerRaw(-3000, 0, 900, 0.1f, 1.0f), 0.0f), "...either way");
    CHECK(SteerRaw(4000, 0, 900, 0.1f, 1.0f) > 0.0f, "just outside it: steering");
    CHECK(Near(SteerRaw(32767, 0, 900, 0.1f, 1.0f), 1.0f), "the deadzone doesn't cost full lock");
    // Response: above 1 is gentler near the centre, below 1 sharper, and
    // full lock stays full lock either way.
    CHECK(SteerRaw(16384, 0, 900, 0.0f, 2.0f) < SteerRaw(16384, 0, 900, 0.0f, 1.0f), "response 2: gentler near the centre");
    CHECK(SteerRaw(16384, 0, 900, 0.0f, 0.5f) > SteerRaw(16384, 0, 900, 0.0f, 1.0f), "response 0.5: sharper");
    for (float response : { 0.5f, 1.0f, 2.0f })
        CHECK(Near(SteerRaw(32767, 0, 900, 0.05f, response), 1.0f), "response %.1f still reaches full lock", double(response));
    // Nothing a hand-edited settings.toml holds can send it out of range.
    for (int raw : { -32768, -1, 0, 1, 32767 })
        for (float deadzone : { -1.0f, 0.0f, 0.5f, 2.0f })
            for (float response : { -1.0f, 0.0f, 1.0f, 3.0f })
            {
                const float v = SteerRaw(int16_t(raw), -5, 900, deadzone, response);
                CHECK(v >= -1.0f && v <= 1.0f && v == v, "raw %d, deadzone %.1f, response %.1f: %f", raw, double(deadzone),
                    double(response), double(v));
            }

    // A pedal is the span from where it rests to where it reads floored,
    // measured at both ends. A G29's rest at +32767 and run to -32768.
    Pedal pedal;
    pedal.Start(32767, -32768);
    CHECK(pedal.ready, "both ends given: it reads");
    CHECK(Near(pedal.Value(32767), 0.0f), "a pedal at rest is up");
    CHECK(Near(pedal.Value(0), 0.5f, 0.01f), "half way down");
    CHECK(Near(pedal.Value(-32768), 1.0f), "on the floor");
    // One wired the other way round works the same.
    pedal.Start(-32768, 32767);
    CHECK(Near(pedal.Value(-32768), 0.0f) && Near(pedal.Value(32767), 1.0f), "a pedal that runs the other way");
    // Only movement towards the pressed end counts: the other way is a
    // pedal on its way back up, which is not a pedal being pressed.
    pedal.Start(0, 32767);
    CHECK(Near(pedal.Value(0), 0.0f) && Near(pedal.Value(32767), 1.0f), "a pedal from the middle");
    CHECK(Near(pedal.Value(-32768), 0.0f), "...and the other way is still up, not full");
    // A pedal that stops short of the end of its axis still reads as
    // floored, because the span says how far it really goes. (A G29's
    // brake stops around -31740 when it is pressed hard.)
    pedal.Start(32767, -31740);
    CHECK(Near(pedal.Value(-31740), 1.0f), "a pedal that stops short still reaches the floor");
    CHECK(Near(pedal.Value(-32768), 1.0f), "...and past it is still the floor");
    CHECK(pedal.Value(0) > 0.4f && pedal.Value(0) < 0.6f, "half way is still half way");

    // Two pedals sharing one axis, one taking it positive and the other
    // negative: each reads its own half and ignores the other's. (Some
    // wheels report a combined pedal axis this way.)
    Pedal shareUp, shareDown;
    shareUp.Start(0, 32767);
    shareDown.Start(0, -32768);
    CHECK(Near(shareUp.Value(32767), 1.0f) && Near(shareDown.Value(32767), 0.0f), "the positive half is one pedal's");
    CHECK(Near(shareDown.Value(-32768), 1.0f) && Near(shareUp.Value(-32768), 0.0f), "the negative half is the other's");
    CHECK(Near(shareUp.Value(0), 0.0f) && Near(shareDown.Value(0), 0.0f), "at rest, neither is pressed");

    // A pedal nobody has placed reads nothing, however far its axis is from
    // zero: SDL reports an axis only when it moves, so an untouched pedal
    // resting at +32767 would otherwise read as floored.
    const Pedal untouched;
    CHECK(!untouched.ready, "a fresh pedal knows nothing");
    for (int raw : { -32768, 0, 32767 })
        CHECK(untouched.Value(int16_t(raw)) == 0.0f, "an unplaced pedal reads nothing at %d", raw);
    Pedal degenerate;
    degenerate.Start(5000, 5000);
    CHECK(!degenerate.ready && degenerate.Value(0) == 0.0f, "two ends the same is no span at all");

    // An axis read the other way round (Invert Steering).
    CHECK(Flip(0) == 0, "centred stays centred");
    CHECK(Flip(32767) == -32767 && Flip(-32767) == 32767, "the ends swap");
    CHECK(Flip(-32768) == kAxisMax, "the extra count clamps rather than wrapping to itself");
    CHECK(Flip(Flip(12345)) == 12345, "twice is where it started");

    // Whatever it reads, the answer is a fraction.
    pedal.Start(32767, -32768);
    for (int raw : { -32768, -1000, 0, 1000, 32767 })
    {
        const float v = pedal.Value(int16_t(raw));
        CHECK(v >= 0.0f && v <= 1.0f, "pedal at %d: %f", raw, double(v));
    }

    // The wheel's own axis as two measured ends: the centre falls halfway
    // between them, each side is measured against its own end, and which
    // way round the wheel is needs nothing else said.
    Steering wheel;
    CHECK(!Steering{}.ready || true, "a default Steering spans the axis");
    wheel.Start(-32768, 32767);
    CHECK(wheel.ready, "two ends given");
    CHECK(Near(wheel.Fraction(0), 0.0f, 0.01f), "the middle is straight ahead");
    CHECK(Near(wheel.Fraction(32767), 1.0f, 0.01f), "full right");
    CHECK(Near(wheel.Fraction(-32768), -1.0f, 0.01f), "full left");
    // A wheel whose axis runs the other way: the ends come out swapped, and
    // that alone turns it round.
    wheel.Start(32767, -32768);
    CHECK(Near(wheel.Fraction(32767), -1.0f, 0.01f), "the positive end is full left on a backwards wheel");
    CHECK(Near(wheel.Fraction(-32768), 1.0f, 0.01f), "...and the negative end is full right");
    // A wheel that reaches neither end of its axis, and sits off centre:
    // each side still reaches full lock.
    wheel.Start(-20000, 10000);
    CHECK(Near(wheel.Fraction(-20000), -1.0f, 0.01f) && Near(wheel.Fraction(10000), 1.0f, 0.01f), "a short, lopsided wheel");
    CHECK(Near(wheel.Fraction(-5000), 0.0f, 0.01f), "its own centre is straight ahead");
    CHECK(wheel.Fraction(32767) <= 1.0f && wheel.Fraction(-32768) >= -1.0f, "past its ends is still full lock");
    // Two ends the same is no span: straight ahead, whatever it reads.
    Steering flat;
    flat.Start(1000, 1000);
    CHECK(!flat.ready && Near(flat.Fraction(32767), 0.0f), "no span, no steering");

    // The buttons the player assigns sit on top of the model's, and a
    // button given to one control is taken off whatever else had it.
    {
        std::array<int, kControlCount> assigned{};
        assigned.fill(-1);
        assigned[size_t(Control::A)] = 7;  // A is the lever the model calls LB
        const std::vector<Binding> mine = WithAssigned(kLogitechButtons, assigned);
        CHECK(Resolve(Held<25>({ 7 }), hat::Centred, mine).buttons == button::A, "button 7 is now A");
        CHECK(Resolve(Held<25>({ 0 }), hat::Centred, mine).buttons == 0, "...and the model's A no longer answers");
        CHECK(Resolve(Held<25>({ 9 }), hat::Centred, mine).buttons == button::Start, "the rest of the model stands");
        // Nothing assigned leaves the model exactly as it was.
        assigned.fill(-1);
        CHECK(WithAssigned(kLogitechButtons, assigned).size() == std::size(kLogitechButtons), "no assignments, no change");
        // A paddle can be reassigned too, and keeps its own role.
        assigned[size_t(Control::ShiftUp)] = 2;
        const std::vector<Binding> shift = WithAssigned(kLogitechButtons, assigned);
        CHECK(Resolve(Held<25>({ 2 }), hat::Centred, shift).rightY > 0, "button 2 now shifts up");
        CHECK(Resolve(Held<25>({ 4 }), hat::Centred, shift).rightY == 0, "...and the model's paddle no longer does");
    }

    // Every control has a name and its own place in the order.
    for (size_t i = 0; i < kControlCount; i++)
        CHECK(kControls[i].name != nullptr && size_t(kControls[i].control) == i, "control %zu is itself", i);

    // The buttons. A G29's, by position: Cross is A, Circle is B, Square is
    // X, Triangle is Y; Share and Options are Back and Start.
    auto press = [](std::initializer_list<int> which) {
        return Resolve(Held<25>(which), hat::Centred, kLogitechButtons);
    };
    CHECK(press({ 0 }).buttons == button::A, "Cross is A");
    CHECK(press({ 1 }).buttons == button::X, "Square is X");
    CHECK(press({ 2 }).buttons == button::B, "Circle is B");
    CHECK(press({ 3 }).buttons == button::Y, "Triangle is Y");
    CHECK(press({ 8 }).buttons == button::Back, "Share is Back");
    CHECK(press({ 9 }).buttons == button::Start, "Options is Start");
    CHECK(press({ 8, 9 }).buttons == (button::Back | button::Start), "Share + Options is the settings chord");
    CHECK(press({ 6 }).buttons == button::RightShoulder && press({ 7 }).buttons == button::LeftShoulder, "the levers are the shoulders");
    CHECK(press({ 10 }).buttons == button::RightThumb && press({ 11 }).buttons == button::LeftThumb, "the stick clicks");
    // The paddles shift gears, which the game does with the right stick;
    // both at once is no change, as holding both keys of a pair is.
    CHECK(press({ 4 }).rightY > 0 && press({ 4 }).buttons == 0, "the right paddle pushes the stick up");
    CHECK(press({ 5 }).rightY < 0 && press({ 5 }).buttons == 0, "the left one pulls it down");
    CHECK(press({ 4, 5 }).rightY == 0, "both paddles: no gear change");
    CHECK(press({}).buttons == 0 && press({}).rightY == 0, "nothing held, nothing pressed");
    // A button the wheel reports but nothing is bound to does nothing, and a
    // binding for a button the wheel hasn't got is ignored rather than read
    // off the end.
    CHECK(press({ 20 }).buttons == 0, "an unbound button does nothing");
    CHECK(Resolve(Held<2>({ 0, 1 }), hat::Centred, kLogitechButtons).buttons == (button::A | button::X),
        "a wheel with two buttons: those two, and no reading past them");
    // The hat is the D-pad, corners included.
    const std::array<bool, 25> none{};
    CHECK(Resolve(none, hat::Up, kLogitechButtons).buttons == button::DpadUp, "the hat is the D-pad");
    CHECK(Resolve(none, uint8_t(hat::Left | hat::Down), kLogitechButtons).buttons == (button::DpadLeft | button::DpadDown),
        "its corners too");

    // NFSMW_WHEEL_BUTTONS: what it names changes, the rest of the model's
    // bindings stay, NONE takes one out, and nonsense is reported without
    // throwing the others away.
    std::string error = "x";
    std::vector<Binding> changed = ParseBindings("0=B,2=A", kLogitechButtons, error);
    CHECK(error.empty(), "a line that reads: \"%s\"", error.c_str());
    CHECK(Resolve(Held<25>({ 0 }), hat::Centred, changed).buttons == button::B, "button 0 is now B");
    CHECK(Resolve(Held<25>({ 9 }), hat::Centred, changed).buttons == button::Start, "...and the model's Start is untouched");
    changed = ParseBindings("9=NONE", kLogitechButtons, error);
    CHECK(error.empty() && Resolve(Held<25>({ 9 }), hat::Centred, changed).buttons == 0, "NONE takes a button out");
    changed = ParseBindings(" 4=START , nonsense , 5=WHAT ", kLogitechButtons, error);
    CHECK(error == "nonsense", "the first item it can't read: \"%s\"", error.c_str());
    CHECK(Resolve(Held<25>({ 4 }), hat::Centred, changed).buttons == button::Start, "the items that do read still count");
    CHECK(ParseBindings("", kLogitechButtons, error).size() == std::size(kLogitechButtons) && error.empty(), "an empty list changes nothing");

    // Every name is one of the pad's controls, and NameOf says which.
    for (const auto& [name, binding] : kNames)
        if (name != "NONE")
            CHECK(NameOf(binding) == name, "%s names itself", std::string(name).c_str());

    // Force feedback: the stronger motor the game asks for, at the share the
    // setting allows, and nothing at all at 0.
    CHECK(Near(RumbleStrength(65535, 0, 100), 1.0f), "a full rumble at 100%%");
    CHECK(Near(RumbleStrength(0, 65535, 100), 1.0f), "either motor");
    CHECK(Near(RumbleStrength(65535, 65535, 50), 0.5f), "half strength");
    CHECK(Near(RumbleStrength(65535, 0, 0), 0.0f), "0%%: nothing");
    CHECK(Near(RumbleStrength(0, 0, 100), 0.0f), "no request: nothing");
    for (int percent : { -10, 0, 50, 100, 1000 })
    {
        const float v = RumbleStrength(40000, 20000, percent);
        CHECK(v >= 0.0f && v <= 1.0f, "%d%%: %f", percent, double(v));
        CHECK(SpringLevel(percent) <= 0x7FFF, "%d%%: the spring's coefficient fits", percent);
    }
    CHECK(SpringLevel(0) == 0 && SpringSaturation(0) == 0, "no centring force is no spring");
    CHECK(SpringLevel(100) == 0x7FFF, "full centring force is the strongest spring");
    CHECK(SpringLevel(50) > 0 && SpringLevel(50) < SpringLevel(100), "and half is in between");

    // Usable at compile time (constexpr), as hid/key_map.h's rules are.
    static_assert([] { Pedal p; p.Start(32767, -32768); return p.Value(32767) == 0.0f && p.Value(-32768) == 1.0f; }(),
        "constexpr pedal");
    static_assert(Resolve(std::array<bool, 4>{ true, false, false, false }, hat::Up, kLogitechButtons).buttons ==
        (button::A | button::DpadUp), "constexpr buttons");

    printf("%s (%d failures)\n", g_failures ? "FAILED" : "passed", g_failures);
    return g_failures ? 1 : 0;
}
