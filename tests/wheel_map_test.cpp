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
        CHECK(m.pedalRest == 0 || m.pedalRest == kAxisMax || m.pedalRest == -kAxisMax,
            "model %zu: pedalRest %d is an end of the axis, or unknown", i, int(m.pedalRest));
        CHECK((m.pedalRest != 0) == (m.vendor != 0), "model %zu: the wheels we know say where their pedals rest", i);
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
        CHECK(g29.pedalRest == kAxisMax, "the G29's pedals rest at +32767, not 0");
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
    CHECK(Near(Steer(0, 0, 900, 0.0f, 1.0f), 0.0f), "centred is straight ahead");
    CHECK(Near(Steer(32767, 0, 900, 0.0f, 1.0f), 1.0f), "full right");
    CHECK(Near(Steer(-32768, 0, 900, 0.0f, 1.0f), -1.0f), "full left (the extra count clamps)");
    CHECK(Near(Steer(16384, 0, 900, 0.0f, 1.0f), 0.5f, 0.002f), "half way round");
    // A range smaller than the wheel's turn reaches full lock sooner: 450 of
    // a G29's 900 degrees means half a turn is already full lock.
    CHECK(Near(Steer(16384, 450, 900, 0.0f, 1.0f), 1.0f), "450 of 900: half the turn is full lock");
    CHECK(Near(Steer(8192, 450, 900, 0.0f, 1.0f), 0.5f, 0.002f), "...and a quarter of it is half lock");
    CHECK(Near(Steer(-16384, 450, 900, 0.0f, 1.0f), -1.0f), "the same the other way");
    CHECK(Near(Steer(32767, 450, 900, 0.0f, 1.0f), 1.0f), "past full lock is full lock");
    // A range at or above the wheel's turn, or a wheel whose turn we don't
    // know, uses the whole turn.
    CHECK(Near(Steer(16384, 900, 900, 0.0f, 1.0f), 0.5f, 0.002f), "900 of 900 is the whole turn");
    CHECK(Near(Steer(16384, 540, 0, 0.0f, 1.0f), 0.5f, 0.002f), "a wheel whose turn is unknown: the whole axis");
    // The deadzone: nothing inside it, and what is outside still reaches
    // full lock.
    CHECK(Near(Steer(3000, 0, 900, 0.1f, 1.0f), 0.0f), "inside the deadzone: straight ahead");
    CHECK(Near(Steer(-3000, 0, 900, 0.1f, 1.0f), 0.0f), "...either way");
    CHECK(Steer(4000, 0, 900, 0.1f, 1.0f) > 0.0f, "just outside it: steering");
    CHECK(Near(Steer(32767, 0, 900, 0.1f, 1.0f), 1.0f), "the deadzone doesn't cost full lock");
    // Response: above 1 is gentler near the centre, below 1 sharper, and
    // full lock stays full lock either way.
    CHECK(Steer(16384, 0, 900, 0.0f, 2.0f) < Steer(16384, 0, 900, 0.0f, 1.0f), "response 2: gentler near the centre");
    CHECK(Steer(16384, 0, 900, 0.0f, 0.5f) > Steer(16384, 0, 900, 0.0f, 1.0f), "response 0.5: sharper");
    for (float response : { 0.5f, 1.0f, 2.0f })
        CHECK(Near(Steer(32767, 0, 900, 0.05f, response), 1.0f), "response %.1f still reaches full lock", double(response));
    // Nothing a hand-edited settings.toml holds can send it out of range.
    for (int raw : { -32768, -1, 0, 1, 32767 })
        for (float deadzone : { -1.0f, 0.0f, 0.5f, 2.0f })
            for (float response : { -1.0f, 0.0f, 1.0f, 3.0f })
            {
                const float v = Steer(int16_t(raw), -5, 900, deadzone, response);
                CHECK(v >= -1.0f && v <= 1.0f && v == v, "raw %d, deadzone %.1f, response %.1f: %f", raw, double(deadzone),
                    double(response), double(v));
            }

    // A pedal resting at one end of its axis runs to the other.
    Pedal pedal;
    pedal.Start(32767);
    CHECK(Near(pedal.Value(32767), 0.0f), "a pedal at rest is up");
    CHECK(Near(pedal.Value(0), 0.5f, 0.01f), "half way down");
    CHECK(Near(pedal.Value(-32768), 1.0f), "on the floor");
    // ...and one wired the other way round works the same.
    pedal.Start(-32768);
    CHECK(Near(pedal.Value(-32768), 0.0f), "a pedal resting at the other end is up");
    CHECK(Near(pedal.Value(32767), 1.0f), "...and is down at the far end");
    // A pedal that rests centred has half the axis to travel.
    pedal.Start(0);
    CHECK(Near(pedal.Value(0), 0.0f), "a centred pedal is up");
    CHECK(Near(pedal.Value(32767), 1.0f), "...and down at the end");
    // The travel is the axis, not the furthest the pedal has been seen to
    // go: a part press reads as a part press however hard the last one was,
    // and a cautious first press is never taken for the floor.
    pedal.Start(32767);
    CHECK(pedal.Value(-20000) < 1.0f && pedal.Value(-20000) > 0.5f, "a part press is a part press");
    CHECK(pedal.Value(-30000) > pedal.Value(-20000) && pedal.Value(-30000) < 1.0f, "a harder one is further down, still not the floor");
    CHECK(Near(pedal.Value(32767), 0.0f), "and it is up again at rest");
    CHECK(Near(pedal.Value(-20000), float(32767 + 20000) / 65534.0f, 0.001f), "the same press reads the same as before");
    // A pedal nobody has placed reads nothing, however far its axis is from
    // zero: SDL reports an axis only when it moves, so an untouched pedal
    // resting at +32767 would otherwise read as floored.
    const Pedal untouched;
    CHECK(!untouched.ready, "a fresh pedal knows nothing");
    for (int raw : { -32768, 0, 32767 })
        CHECK(untouched.Value(int16_t(raw)) == 0.0f, "an unplaced pedal reads nothing at %d", raw);
    Pedal placed;
    placed.Start(32767);
    CHECK(placed.ready && Near(placed.Value(32767), 0.0f) && Near(placed.Value(-32768), 1.0f), "...and reads once placed");

    // An axis read the other way round (Invert Steering).
    CHECK(Flip(0) == 0, "centred stays centred");
    CHECK(Flip(32767) == -32767 && Flip(-32767) == 32767, "the ends swap");
    CHECK(Flip(-32768) == kAxisMax, "the extra count clamps rather than wrapping to itself");
    CHECK(Flip(Flip(12345)) == 12345, "twice is where it started");

    // Whatever it reads, the answer is a fraction.
    pedal.Start(0);
    for (int raw : { -32768, -1000, 0, 1000, 32767 })
    {
        const float v = pedal.Value(int16_t(raw));
        CHECK(v >= 0.0f && v <= 1.0f, "pedal at %d: %f", raw, double(v));
    }

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
    static_assert([] { Pedal p; p.Start(32767); return p.Value(32767) == 0.0f && p.Value(-32768) == 1.0f; }(), "constexpr pedal");
    static_assert(Resolve(std::array<bool, 4>{ true, false, false, false }, hat::Up, kLogitechButtons).buttons ==
        (button::A | button::DpadUp), "constexpr buttons");

    printf("%s (%d failures)\n", g_failures ? "FAILED" : "passed", g_failures);
    return g_failures ? 1 : 0;
}
