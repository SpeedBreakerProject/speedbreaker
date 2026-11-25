// Tests for game::map_cursor (runtime/game/map_cursor.h): how far and how
// smoothly the World Map's cursor moves a second, with the hook and without,
// at Frame Rate 60 and 30, mixed frames and slow ones (GitHub issue #14).
// From the repo root:
//   clang++ -std=c++20 -O2 -Iruntime tests/map_cursor_test.cpp -o build/map_cursor_test
//   build/map_cursor_test
// The game's side is simulated as the guest does it: each frame the main loop
// hands the frame its game time (whole vblanks, as 16.16 ms), the front end's
// per-frame message runs the cursor step, which sets the velocity to the
// stick's value times 14 (single precision, sub_822530A8) and hands it to
// MoveCursor (sub_822534F0), which adds it to the cursor's position. The
// hook's part is the scale on that step.
#include <game/map_cursor.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

namespace
{
    using namespace game::map_cursor;

    // A frame of `vblanks` as the main loop hands it: ms in 16.16
    // (frame_time.cpp: llround(given * divider * 1000/60 * 65536)).
    double GivenMs(int vblanks)
    {
        return double(std::llround(double(vblanks) * (1000.0 / 60.0) * 65536.0)) / 65536.0;
    }

    struct Run
    {
        double distance = 0;    // front end units the cursor moved
        double vblanks = 0;     // game time, vblanks
        int frames = 0, moved = 0;
        float largest = 0;      // the largest step of one frame
    };

    // The stick held at `value` (0..1, the action's value) for frames of the
    // given vblank counts, the cursor never at the map's edge.
    Run Hold(const std::vector<int>& frames, float value, bool hooked)
    {
        Run r;
        float x = 320.0f;
        const float velocity = value * kGameStep;  // fmuls f0,f0,f11 (sub_822530A8)
        for (int vb : frames)
        {
            const double ms = GivenMs(vb);
            float step = velocity;
            if (hooked)
                step = float(double(velocity) * StepScale(ms));  // the hook: f1 = float(f1 * scale)
            x += step;                                            // MoveCursor: fadds
            r.frames++;
            r.vblanks += vb;
            if (step != 0.0f)
                r.moved++;
            if (std::abs(step) > r.largest)
                r.largest = std::abs(step);
        }
        r.distance = double(x) - 320.0;
        return r;
    }

    std::vector<int> Frames(int count, int vblanks) { return std::vector<int>(size_t(count), vblanks); }

    // Units a second (60 vblanks).
    double Speed(const Run& r) { return r.distance / r.vblanks * 60.0; }

    void Scales()
    {
        CHECK(StepScale(GivenMs(1)) == 0.5, "1 vblank (Frame Rate 60): %.9f", StepScale(GivenMs(1)));
        CHECK(StepScale(GivenMs(2)) == 1.0, "2 vblanks (Frame Rate 30): %.9f", StepScale(GivenMs(2)));
        CHECK(StepScale(1000.0 / 60.0) == 0.5, "exact 1/60 s: %.9f", StepScale(1000.0 / 60.0));
        CHECK(StepScale(2000.0 / 60.0) == 1.0, "exact 2/60 s: %.9f", StepScale(2000.0 / 60.0));
        for (int vb = 3; vb <= 6; vb++)
            CHECK(StepScale(GivenMs(vb)) == 1.0, "%d vblanks: %.9f (the 360's whole step at most)", vb, StepScale(GivenMs(vb)));
        CHECK(StepScale(500.0) == 1.0, "a hitch of 500 ms: %.9f", StepScale(500.0));
        CHECK(StepScale(0.0) == 1.0, "no frame time yet: %.9f", StepScale(0.0));
        CHECK(StepScale(-5.0) == 1.0, "negative: %.9f", StepScale(-5.0));
        CHECK(StepScale(std::nan("")) == 1.0, "NaN: %.9f", StepScale(std::nan("")));
        // Measured times (NFSMW_GAME_TIME=0, or a display far from 60 Hz) in
        // proportion, off whole vblanks.
        CHECK(std::abs(StepScale(20.0) - 0.6) < 1e-12, "20 ms: %.9f", StepScale(20.0));
        CHECK(std::abs(StepScale(8.0) - 0.24) < 1e-12, "8 ms: %.9f", StepScale(8.0));
        CHECK(std::abs(StepScale(16.6) - 16.6 / kConsoleFrameMs) < 1e-12, "16.6 ms (0.067 off a vblank, not snapped): %.9f",
            StepScale(16.6));
    }

    // The report: the game's step every frame is twice as fast at 60 as at 30.
    void WithoutHookTwiceAsFast()
    {
        const Run r60 = Hold(Frames(60, 1), 1.0f, false), r30 = Hold(Frames(30, 2), 1.0f, false);
        CHECK(std::abs(Speed(r60) - 840.0) < 1e-3, "unhooked 60 fps: %.4f units/s, want 840", Speed(r60));
        CHECK(std::abs(Speed(r30) - 420.0) < 1e-3, "unhooked 30 fps: %.4f units/s, want 420", Speed(r30));
        CHECK(std::abs(Speed(r60) / Speed(r30) - 2.0) < 1e-6, "unhooked ratio %.6f, want 2", Speed(r60) / Speed(r30));
    }

    // With the hook: the 360's 420 units a second at both, and at 60 the
    // cursor still moves every frame by half the step (not 30 steps of 14).
    void SixtyAsThirty()
    {
        for (float value : { 1.0f, 0.5f, 0.27f })
        {
            const Run r60 = Hold(Frames(600, 1), value, true), r30 = Hold(Frames(300, 2), value, true);
            const double want = 420.0 * value;
            CHECK(std::abs(Speed(r60) - want) / want < 1e-5, "stick %.2f, 60 fps: %.4f units/s, want %.4f", value, Speed(r60), want);
            CHECK(std::abs(Speed(r30) - want) / want < 1e-5, "stick %.2f, 30 fps: %.4f units/s, want %.4f", value, Speed(r30), want);
            CHECK(r60.moved == r60.frames, "stick %.2f, 60 fps: moved in %d of %d frames", value, r60.moved, r60.frames);
            CHECK(r60.largest == float(double(value * kGameStep) * 0.5), "stick %.2f, 60 fps: largest step %.6f, want half the game's",
                value, r60.largest);
        }
    }

    // Frame Rate 30 is the game's own step to the bit: the hook changes nothing.
    void ThirtyUnchanged()
    {
        for (float value : { 1.0f, 0.73f, 0.1f, 0.0f })
        {
            const Run hooked = Hold(Frames(90, 2), value, true), game = Hold(Frames(90, 2), value, false);
            CHECK(hooked.distance == game.distance, "stick %.2f: hooked %.9f, game %.9f", value, hooked.distance, game.distance);
            CHECK(hooked.largest == game.largest, "stick %.2f: largest %.9f, game %.9f", value, hooked.largest, game.largest);
        }
    }

    // Frames of 1 and 2 vblanks mixed (a 60 fps target that drops some, or
    // Frame Rate Auto switching): the speed a second is still the 360's.
    void MixedFrames()
    {
        std::vector<int> frames;
        for (int i = 0; i < 400; i++)
            frames.push_back(i % 3 == 0 ? 2 : 1);
        const Run r = Hold(frames, 1.0f, true);
        CHECK(std::abs(Speed(r) - 420.0) < 1e-3, "mixed 1/2-vblank frames: %.4f units/s, want 420", Speed(r));
        std::vector<int> sw = Frames(120, 1), thirty = Frames(60, 2);
        sw.insert(sw.end(), thirty.begin(), thirty.end());
        const Run s = Hold(sw, 1.0f, true);
        CHECK(std::abs(Speed(s) - 420.0) < 1e-3, "60 then 30: %.4f units/s, want 420", Speed(s));
    }

    // A frame of 3 vblanks or more moves the whole step, as the 360's frame
    // does at that length (it doesn't make up for the time either): the
    // hooked cursor at 60 fps and the 360's at 30 match frame for frame.
    void SlowFramesAsThe360()
    {
        const std::vector<int> frames = { 1, 1, 3, 1, 4, 1, 1, 6, 2, 1 };
        const Run hooked = Hold(frames, 1.0f, true);
        double want = 0;
        for (int vb : frames)
            want += vb >= 2 ? 14.0 : 7.0;
        CHECK(std::abs(hooked.distance - want) < 1e-4, "slow frames: %.4f units, want %.4f", hooked.distance, want);
        CHECK(hooked.largest == kGameStep, "slow frames: largest step %.4f, want the game's %.1f", hooked.largest, kGameStep);
    }
}

int main()
{
    Scales();
    WithoutHookTwiceAsFast();
    SixtyAsThirty();
    ThirtyUnchanged();
    MixedFrames();
    SlowFramesAsThe360();
    if (g_failures)
    {
        printf("%d FAILED\n", g_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
