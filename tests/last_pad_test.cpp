// Tests for hid/last_pad.h (player 1 follows the controller last pressed, GitHub #66); hid/input.cpp is read (argv[1])
// to check it follows the rule. Header only; from the repo root:
//   clang++ -std=c++20 -O2 -Iruntime tests/last_pad_test.cpp -o build/last_pad_test &&
//   build/last_pad_test runtime/hid/input.cpp
#include <hid/last_pad.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

using hid::LastPad;
using Sample = LastPad::Sample;
constexpr uint16_t kStart = 0x0010, kA = 0x1000;  // XInput's START and A

// One frame: each pad's sample, then the pick (as input.cpp's FollowLastPressed does it).
static uint32_t Frame(LastPad& l, uint32_t current, bool split, std::initializer_list<std::pair<uint32_t, Sample>> pads)
{
    for (auto& [pad, s] : pads)
        l.Note(pad, s);
    return l.Choose(current, split);
}

static Sample Buttons(uint16_t b) { Sample s; s.buttons = b; return s; }
static Sample Left(float x, float y) { Sample s; s.lx = x; s.ly = y; return s; }
static Sample Right(float x, float y) { Sample s; s.rx = x; s.ry = y; return s; }
static Sample Triggers(float lt, float rt) { Sample s; s.lt = lt; s.rt = rt; return s; }

int main(int argc, char** argv)
{
    const Sample idle;

    // A docked Steam Deck in Desktop Mode: SDL lists the Deck's own controls (1) first; the pad in hand (2) takes
    // player 1 with its first press, and that press is the frame's.
    {
        LastPad l;
        uint32_t p1 = 1;
        p1 = Frame(l, p1, false, { { 1, idle }, { 2, idle } });
        CHECK(p1 == 1, "nobody pressed: SDL's first stays player 1");
        for (int i = 0; i < 100; i++)
            p1 = Frame(l, p1, false, { { 1, idle }, { 2, idle } });
        CHECK(p1 == 1, "still, idle");
        p1 = Frame(l, p1, false, { { 1, idle }, { 2, Buttons(kStart) } });
        CHECK(p1 == 2, "START on the pad in hand: player 1's");
        p1 = Frame(l, p1, false, { { 1, idle }, { 2, Buttons(kStart) } });
        p1 = Frame(l, p1, false, { { 1, idle }, { 2, idle } });
        CHECK(p1 == 2, "held, released: stays");
        p1 = Frame(l, p1, false, { { 1, Buttons(kA) }, { 2, idle } });
        CHECK(p1 == 1, "a press on the Deck's own controls: back to it");
    }

    // Sticks: a press past half its travel counts, drift and rest don't; a stick held off centre counts once.
    {
        LastPad l;
        uint32_t p1 = 1;
        p1 = Frame(l, p1, false, { { 1, idle }, { 2, idle } });
        for (int i = 0; i < 300; i++)
        {
            const float drift = 0.2f * float(i % 7) / 6.0f;  // a worn stick wandering up to 0.2, both sticks
            Sample s = Left(drift, -drift);
            s.rx = drift;
            p1 = Frame(l, p1, false, { { 1, idle }, { 2, s } });
        }
        CHECK(p1 == 1, "drift up to 0.2 never takes player 1");
        p1 = Frame(l, p1, false, { { 1, idle }, { 2, Left(0.45f, 0.0f) } });
        CHECK(p1 == 1, "0.45: not yet");
        p1 = Frame(l, p1, false, { { 1, idle }, { 2, Left(0.0f, 0.6f) } });
        CHECK(p1 == 2, "pushed up past half: a press");
        for (int i = 0; i < 50; i++)
            p1 = Frame(l, p1, false, { { 1, idle }, { 2, Left(0.0f, 0.6f) } });
        CHECK(p1 == 2, "held there: stays");
    }
    {
        // The radial rule exactly: a diagonal past half counts.
        LastPad l;
        uint32_t p1 = 2;
        p1 = Frame(l, p1, false, { { 1, idle }, { 2, idle } });
        p1 = Frame(l, p1, false, { { 1, Right(-0.4f, -0.4f) }, { 2, idle } });
        CHECK(p1 == 1, "a diagonal at 0.57 of the travel is a press");
        // Held there: no new press; pad 2 takes it back with a button.
        p1 = Frame(l, p1, false, { { 1, Right(-0.4f, -0.4f) }, { 2, Buttons(kA) } });
        CHECK(p1 == 2, "pad 2's A");
        for (int i = 0; i < 50; i++)
            p1 = Frame(l, p1, false, { { 1, Right(-0.4f, -0.4f) }, { 2, idle } });
        CHECK(p1 == 2, "pad 1's stick still held off centre: not another press");
        p1 = Frame(l, p1, false, { { 1, Right(-0.25f, -0.0f) }, { 2, idle } });  // 0.25: not back yet
        p1 = Frame(l, p1, false, { { 1, Right(-0.6f, 0.0f) }, { 2, idle } });
        CHECK(p1 == 2, "eased to a quarter, pushed again: not back below a quarter, so no press");
        p1 = Frame(l, p1, false, { { 1, Right(-0.1f, 0.0f) }, { 2, idle } });
        p1 = Frame(l, p1, false, { { 1, Right(-0.6f, 0.0f) }, { 2, idle } });
        CHECK(p1 == 1, "back near the centre, pushed again: a press");
    }

    // Triggers: past half is a press; a trigger resting a little in (a worn one) isn't.
    {
        LastPad l;
        uint32_t p1 = 1;
        p1 = Frame(l, p1, false, { { 1, idle }, { 2, idle } });
        for (int i = 0; i < 50; i++)
            p1 = Frame(l, p1, false, { { 1, idle }, { 2, Triggers(0.12f, 0.3f) } });
        CHECK(p1 == 1, "triggers resting in: no press");
        p1 = Frame(l, p1, false, { { 1, idle }, { 2, Triggers(0.12f, 0.9f) } });
        CHECK(p1 == 2, "RT pressed in: a press");
        p1 = Frame(l, p1, false, { { 1, Triggers(0.7f, 0.0f) }, { 2, Triggers(0.12f, 0.9f) } });
        CHECK(p1 == 1, "pad 1's LT");
    }

    // What a controller reads as it is first seen (held while it connected, a stick stuck off centre) is no press.
    {
        LastPad l;
        uint32_t p1 = 1;
        p1 = Frame(l, p1, false, { { 1, idle } });
        Sample stuck = Buttons(kA);
        stuck.lx = 0.9f;
        stuck.rt = 1.0f;
        p1 = Frame(l, p1, false, { { 1, idle }, { 3, stuck } });
        CHECK(p1 == 1, "a new pad holding A, a stick and RT: not a press");
        for (int i = 0; i < 50; i++)
            p1 = Frame(l, p1, false, { { 1, idle }, { 3, stuck } });
        CHECK(p1 == 1, "still held: still not");
        p1 = Frame(l, p1, false, { { 1, idle }, { 3, Buttons(kA | kStart) } });
        CHECK(p1 == 3, "then another button: a press");
    }

    // Both pressed in the same frame: player 1 keeps its own. Two others: the one noted last.
    {
        LastPad l;
        uint32_t p1 = 1;
        p1 = Frame(l, p1, false, { { 1, idle }, { 2, idle }, { 3, idle } });
        p1 = Frame(l, p1, false, { { 1, Buttons(kA) }, { 2, Buttons(kA) } });
        CHECK(p1 == 1, "player 1's own press beside another's: no change");
        p1 = Frame(l, p1, false, { { 1, Buttons(kA) }, { 2, idle }, { 3, idle } });
        p1 = Frame(l, p1, false, { { 1, Buttons(kA) }, { 2, Buttons(kA) }, { 3, Buttons(kA) } });
        CHECK(p1 == 3 || p1 == 2, "two others at once: one of them");
    }

    // Split screen: nothing changes hands; player 1 is the pad pressed last before it began.
    {
        LastPad l;
        uint32_t p1 = 1;
        p1 = Frame(l, p1, false, { { 1, idle }, { 2, idle } });
        p1 = Frame(l, p1, false, { { 1, idle }, { 2, Buttons(kA) } });
        CHECK(p1 == 2, "pad 2 chose Quick Race > Split Screen");
        p1 = Frame(l, p1, true, { { 2, idle } });
        p1 = Frame(l, p1, true, { { 2, Buttons(kStart) } });
        CHECK(p1 == 2, "split screen: still pad 2");
        // (In split screen input.cpp notes only player 1's pad; a press from any other one must not change it.)
        p1 = Frame(l, p1, true, { { 1, Buttons(kStart) }, { 2, idle } });
        CHECK(p1 == 2, "split screen: another pad's START is player 2's, not player 1's");
        p1 = Frame(l, p1, false, { { 1, idle }, { 2, idle } });
        CHECK(p1 == 2, "split screen over: still pad 2 until another is pressed");
        p1 = Frame(l, p1, false, { { 1, Buttons(kA) }, { 2, idle } });
        CHECK(p1 == 1, "then single player follows the press again");
    }

    // Player 1's controller removed: the one pressed last of those left, else the first listed; none, 0.
    {
        LastPad l;
        uint32_t p1 = 1;
        p1 = Frame(l, p1, false, { { 1, idle }, { 2, idle }, { 3, idle }, { 4, idle } });
        CHECK(l.Fallback({ 2, 3, 4 }) == 2, "none pressed: SDL's first");
        p1 = Frame(l, p1, false, { { 1, idle }, { 2, idle }, { 3, Buttons(kA) }, { 4, idle } });
        p1 = Frame(l, p1, false, { { 1, idle }, { 2, idle }, { 3, idle }, { 4, Buttons(kA) } });
        CHECK(p1 == 4, "pad 4 pressed last");
        l.Remove(4);
        CHECK(l.Fallback({ 1, 2, 3 }) == 3, "pad 4 gone: pad 3, pressed before it");
        l.Remove(3);
        CHECK(l.Fallback({ 1, 2 }) == 1, "pads 3 and 4 gone, 1 and 2 never pressed: SDL's first");
        CHECK(l.Fallback({}) == 0, "none left");
        // A pad that comes back (a new SDL id) starts afresh: its first sight is no press.
        p1 = Frame(l, 1, false, { { 1, idle }, { 5, Buttons(kA) } });
        CHECK(p1 == 1, "pad back with A held as it connects: no press");
        p1 = Frame(l, p1, false, { { 1, idle }, { 5, idle } });
        p1 = Frame(l, p1, false, { { 1, idle }, { 5, Buttons(kA) } });
        CHECK(p1 == 5, "then pressed: player 1's");
        // A press noted, then the pad removed before Choose: not picked.
        l.Note(2, idle);
        l.Note(2, Buttons(kA));
        l.Remove(2);
        CHECK(l.Choose(5, false) == 5, "a removed pad's press is forgotten");
    }

    // input.cpp: the rule's use.
    if (argc > 1)
    {
        std::ifstream f(argv[1]);
        std::stringstream ss;
        ss << f.rdbuf();
        const std::string src = ss.str();
        CHECK(!src.empty(), "read %s", argv[1]);
        auto has = [&](const char* text) { return src.find(text) != std::string::npos; };
        CHECK(has("const SDL_JoystickID pick = s_lastPad.Choose(current, s_slots.Split());"), "split screen locks player 1");
        CHECK(has("const SDL_JoystickID id = s_lastPad.Fallback(candidates);"), "removal falls back by the rule");
        CHECK(has("FollowLastPressed();  // (outside split screen"), "followed before player 1's pad is read");
        CHECK(src.find("FollowLastPressed();  // (outside") < src.find("SDL_GetGamepadButton(s_gamepad, SDL_GAMEPAD_BUTTON_BACK)"),
            "the press that switched reaches the game the same frame");
        CHECK(has("SDL_RumbleGamepad(s_gamepad, 0, 0, 0);\n            SDL_SetGamepadPlayerIndex(s_gamepad, -1);"),
            "the last pad stops rumbling and shows no player");
        CHECK(has("s_rumbleDirty = true;  // the game's last request, on this pad now"), "rumble follows player 1");
        CHECK(has("OpenOtherPads();\n                CloseIdlePads();"), "split screen: idle pads handed to s_others, open across");
        CHECK(has("OpenIdlePads();  // (before their split-screen opens close"), "split screen over: open again first");
        CHECK(has("CloseIdlePads(e.gdevice.which);\n            s_lastPad.Remove(e.gdevice.which);"), "a removed idle pad forgotten");
    }
    else
    {
        printf("  (no input.cpp given: its use not checked)\n");
    }

    if (g_failures)
    {
        printf("last_pad_test: %d FAILED\n", g_failures);
        return 1;
    }
    printf("last_pad_test: all passed\n");
    return 0;
}
