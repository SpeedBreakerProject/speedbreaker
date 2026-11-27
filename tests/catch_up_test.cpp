// Tests for the AI's catch-up (game/catch_up.h): the weight scaling, which
// leaves the game's own weights at 1 and halves the spread at 0.5, the
// menu's other choice. Header only; no game files.
//
// The strength itself is a settings option (Settings > Game > Catch Up,
// settings::Id::CatchUp, NFSMW_CATCHUP as its override): its range, clamping
// and the file's format are user/settings.cpp's, and tests/settings_test.cpp's.
// From the repo root:
//   clang++ -std=c++20 -O2 -Iruntime tests/catch_up_test.cpp -o build/catch_up_test && build/catch_up_test
#include <game/catch_up.h>

#include <cmath>
#include <cstdio>
#include <initializer_list>

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

namespace
{
    using namespace game::catch_up;

    bool Near(float a, float b)
    {
        return std::fabs(a - b) <= 1e-6f;
    }

    void TestWeight()
    {
        // At 1 the game's own weights, bit for bit.
        for (float w : { 0.0f, 0.5f, 1.0f })
            CHECK(ScaleWeight(w, 1.0f) == w, "strength 1 leaves %g", double(w));
        // At 0.5, the menu's other choice, the table's ends come halfway in.
        CHECK(Near(ScaleWeight(1.0f, 0.5f), 0.75f), "1 at half: %g", double(ScaleWeight(1.0f, 0.5f)));
        CHECK(Near(ScaleWeight(0.0f, 0.5f), 0.25f), "0 at half: %g", double(ScaleWeight(0.0f, 0.5f)));
        // The middle never moves, at any strength.
        for (float k : { 0.5f, 0.75f, 1.0f })
            CHECK(ScaleWeight(0.5f, k) == 0.5f, "middle at strength %g", double(k));
        // 1 is the only strength the hook lets through unscaled.
        CHECK(!Scaled(1.0f), "1: the hook does nothing");
        for (float k : { 0.5f, 0.75f, 0.95f })
            CHECK(Scaled(k), "strength %g scales", double(k));
    }
}

int main()
{
    TestWeight();

    if (g_failures)
    {
        printf("catch_up_test: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("catch_up_test: all passed\n");
    return 0;
}
