// SpeedBreaker tests. GPL-3.0-or-later (see COPYING).
//
// Anisotropic Filtering's Auto (gpu/anisotropy_auto.h): 16x on a Mac at 1x
// Internal Resolution, at every screen shape (16:9, a 16:10 MacBook's
// 1280x800, an ultrawide), the device's own value everywhere else; and
// gpu/renderer.cpp's GetSampler applying it with the current render scale.
// The scales are the renderer's own (video::NativeShape for a setting,
// video::AutoNativeScale for Auto). Argument: runtime/gpu/renderer.cpp.
//
// Build by hand: c++ -std=c++20 -O2 -UNDEBUG -I runtime tests/anisotropy_auto_test.cpp runtime/video/picture_fit.cpp -o build/anisotropy_auto_test
#include <gpu/anisotropy_auto.h>
#include <video/picture_fit.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

static int s_failures = 0;
#define CHECK(cond, what)                                              \
    do                                                                 \
    {                                                                  \
        if (!(cond))                                                   \
        {                                                              \
            s_failures++;                                              \
            fprintf(stderr, "FAIL %s:%d: %s: %s\n", __FILE__, __LINE__, #cond, what); \
        }                                                              \
    } while (0)

int main(int argc, char** argv)
{
    using gpu::AutoAnisotropy;
    CHECK(AutoAnisotropy(4, true, true) == 16, "a Mac at 1x: 16x");
    CHECK(AutoAnisotropy(4, true, false) == 4, "a Mac above 1x: its own 4x");
    CHECK(AutoAnisotropy(4, false, true) == 4, "not a Mac (the Deck, an iPhone) at 1x: unchanged");
    CHECK(AutoAnisotropy(2, false, true) == 2, "Windows' Intel iGPU at 1x: unchanged");
    CHECK(AutoAnisotropy(16, false, false) == 16 && AutoAnisotropy(16, true, true) == 16, "16x stays 16x");

    // Internal Resolution's 1x / 1.5x / 2x / 3x (setting 2 / 3 / 4 / 6 half
    // steps) and Auto, on each screen shape the picture can take (filled).
    using gpu::AxisFromHalves;
    using gpu::SceneAtOneX;
    using video::AutoNativeScale;
    using video::NativeShape;
    struct Screen
    {
        uint32_t w, h;
        const char* name;
    };
    const Screen screens[] = {
        { 1280, 720, "16:9" }, { 1280, 800, "16:10 (MacBook, Deck)" }, { 1512, 982, "MacBook Pro 14 (1.54)" },
        { 1440, 960, "3:2" }, { 1024, 768, "4:3" }, { 1720, 720, "21:9" }, { 2560, 1080, "21:9 1080p" },
        { 2560, 720, "32:9" }, { 3840, 1080, "32:9 1080p" },
    };
    for (const Screen& s : screens)
    {
        const gpu::RenderScale one = NativeShape(AxisFromHalves(2), s.w, s.h);
        CHECK(SceneAtOneX(one), s.name);
        CHECK(AutoAnisotropy(4, true, SceneAtOneX(one)) == 16, s.name);
        for (uint32_t halves : { 3u, 4u, 6u })
        {
            const gpu::RenderScale above = NativeShape(AxisFromHalves(halves), s.w, s.h);
            CHECK(!SceneAtOneX(above), s.name);
            CHECK(AutoAnisotropy(4, true, SceneAtOneX(above)) == 4, s.name);
        }
    }
    // Vert+ and Hor+ at 1x: one axis 1, the other stretched (IsOne false).
    CHECK(NativeShape(AxisFromHalves(2), 1280, 800) == (gpu::RenderScale{ { 1, 1 }, { 10, 9 } }), "16:10 at 1x is 1x10/9");
    CHECK(!NativeShape(AxisFromHalves(2), 1280, 800).IsOne() && SceneAtOneX(NativeShape(AxisFromHalves(2), 1280, 800)),
        "16:10 at 1x: not IsOne, but 1x");
    CHECK(NativeShape(AxisFromHalves(2), 3440, 1440) == (gpu::RenderScale{ { 43, 32 }, { 1, 1 } }), "21:9 at 1x is 1.34375x1");
    CHECK(NativeShape(AxisFromHalves(4), 2560, 1600) == (gpu::RenderScale{ { 2, 1 }, { 20, 9 } }), "16:10 at 2x is 2x2.222");
    // Auto (a Mac's budget 4; 9 on an M3 Max or later): 1x where the
    // picture is up to 1280 x 810 (16:9's 720 rows and Vert+'s 1.125),
    // above 1x on a bigger one.
    for (uint32_t budget : { 4u, 9u })
    {
        CHECK(SceneAtOneX(AutoNativeScale(1280, 720, budget)), "Auto at 1280x720 (16:9): 1x");
        CHECK(SceneAtOneX(AutoNativeScale(1280, 800, budget)), "Auto at 1280x800 (16:10): 1x");
        CHECK(SceneAtOneX(AutoNativeScale(1720, 720, budget)), "Auto at 1720x720 (21:9): 1x");
        CHECK(SceneAtOneX(AutoNativeScale(2560, 720, budget)), "Auto at 2560x720 (32:9): 1x");
        CHECK(!SceneAtOneX(AutoNativeScale(2560, 1440, budget)), "Auto at 2560x1440 (16:9): 2x");
        CHECK(!SceneAtOneX(AutoNativeScale(2560, 1600, budget)), "Auto at 2560x1600 (16:10): 2x");
        CHECK(!SceneAtOneX(AutoNativeScale(3024, 1964, budget)), "Auto at 3024x1964 (MacBook Pro 14): above 1x");
        CHECK(!SceneAtOneX(AutoNativeScale(3440, 1440, budget)), "Auto at 3440x1440 (21:9): 2x");
        CHECK(!SceneAtOneX(AutoNativeScale(3840, 2160, budget)), "Auto at 3840x2160: above 1x");
    }
    CHECK(AutoAnisotropy(4, true, SceneAtOneX(AutoNativeScale(1280, 800, 4))) == 16, "a 16:10 Mac at Auto's 1x: 16x");
    CHECK(AutoAnisotropy(4, true, SceneAtOneX(AutoNativeScale(2560, 1600, 4))) == 4, "a 16:10 Mac at Auto's 2x: its own 4x");
    // Not a Mac: the Deck (1280x800, 1x10/9) and an iPhone (2868x1320 at
    // 1x: Hor+) keep their own Auto.
    CHECK(AutoAnisotropy(4, false, SceneAtOneX(AutoNativeScale(1280, 800, 4))) == 4, "the Deck at 1x: its own 4x");
    CHECK(AutoAnisotropy(4, false, SceneAtOneX(NativeShape(AxisFromHalves(2), 2868, 1320))) == 4, "an iPhone at 1x: its own 4x");
#if defined(__APPLE__) && !TARGET_OS_IOS
    CHECK(gpu::kAnisotropyMac, "macOS builds are the Mac");
#else
    CHECK(!gpu::kAnisotropyMac, "other builds are not");
#endif
    if (argc > 1)
    {
        std::ifstream in(argv[1], std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        const std::string r = text.str();
        CHECK(r.find("anisotropy = gpu::AutoAnisotropy(s_autoAnisotropy, gpu::kAnisotropyMac, gpu::SceneAtOneX(s_renderScale));") != std::string::npos,
            "GetSampler applies Auto with the current render scale (1x at every screen shape)");
        CHECK(r.find("s_textureCache->generation++;  // lookups decided with the old scale") != std::string::npos,
            "a scale change makes the texture lookups (and so their samplers) again");
    }
    if (s_failures)
    {
        printf("%d check(s) FAILED\n", s_failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
