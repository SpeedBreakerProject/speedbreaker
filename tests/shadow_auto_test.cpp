// SpeedBreaker tests. GPL-3.0-or-later (see COPYING).
//
// Shadow Resolution's Auto (gpu/shadow_auto.h, GitHub #90): 2x while the
// scene renders at 2x or more, 1x otherwise, at every screen shape; 1x on a
// Steam Deck or Steam Frame at any output (video/steam_device.h); with the
// renderer's own scales (video::NativeShape for an Internal Resolution step,
// video::AutoNativeScale for its Auto at each device's budget: renderer.cpp
// ReadRenderScale); Auto as the option's default; and gpu/renderer.cpp
// applying it with the current render scale and again after a scale change.
// Arguments: runtime/gpu/renderer.cpp, runtime/user/settings.cpp.
//
// Build by hand: c++ -std=c++20 -O2 -UNDEBUG -I runtime tests/shadow_auto_test.cpp runtime/video/picture_fit.cpp runtime/video/scale_budget.cpp -o build/shadow_auto_test
#include <gpu/shadow_auto.h>
#include <user/settings.h>
#include <video/picture_fit.h>
#include <video/scale_budget.h>

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

static std::string ReadText(const char* path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

int main(int argc, char** argv)
{
    using gpu::AutoShadowHalves;
    using gpu::AxisFromHalves;
    using gpu::RenderScale;
    using video::AutoNativeScale;
    using video::NativeShape;
    static_assert(settings::kShadowResolutionDefault == settings::kShadowResolutionAuto, "Auto is the default");
    CHECK(AutoShadowHalves(RenderScale::Whole(1, 1)) == 2 && AutoShadowHalves(RenderScale::Whole(2, 2)) == 4, "1x -> 1x, 2x -> 2x");
    CHECK(gpu::ShadowScale(AutoShadowHalves(RenderScale::Whole(2, 2))) == RenderScale::Whole(2, 2), "Auto at 2x: the 3200x3200 map");
    CHECK(gpu::ShadowScale(AutoShadowHalves(RenderScale::Whole(1, 1))) == RenderScale::Whole(1, 1), "Auto at 1x: the 1600x1600 map");
    CHECK(AutoShadowHalves(RenderScale::Whole(3, 3)) == 4 && AutoShadowHalves(RenderScale::Whole(3, 2)) == 4, "3x, 3x2: 2x");
    CHECK(AutoShadowHalves(RenderScale::Whole(2, 1)) == 2 && AutoShadowHalves(RenderScale::Whole(1, 2)) == 2, "2x1, 1x2: 1x");

    // Internal Resolution's 1x / 1.5x / 2x / 3x (half steps 2 / 3 / 4 / 6)
    // on each screen shape the picture can take (filled).
    struct Screen
    {
        uint32_t w, h;
        const char* name;
    };
    const Screen screens[] = {
        { 1280, 720, "16:9" }, { 1280, 800, "16:10 (MacBook, Deck)" }, { 1512, 982, "MacBook Pro 14 (1.54)" },
        { 1440, 960, "3:2" }, { 1024, 768, "4:3" }, { 1720, 720, "21:9" }, { 2560, 1080, "21:9 1080p" },
        { 2560, 720, "32:9" }, { 3840, 1080, "32:9 1080p" }, { 2868, 1320, "iPhone (Hor+)" },
    };
    for (const Screen& s : screens)
    {
        CHECK(AutoShadowHalves(NativeShape(AxisFromHalves(2), s.w, s.h)) == 2, s.name);
        CHECK(AutoShadowHalves(NativeShape(AxisFromHalves(3), s.w, s.h)) == 2, s.name);
        CHECK(AutoShadowHalves(NativeShape(AxisFromHalves(4), s.w, s.h)) == 4, s.name);
        CHECK(AutoShadowHalves(NativeShape(AxisFromHalves(6), s.w, s.h)) == 4, s.name);
    }

    // Internal Resolution's Auto, device by device (the budgets
    // renderer.cpp's ReadRenderScale gives), at the screens they meet.
    struct Case
    {
        uint32_t w, h, budget, halves;
        const char* name;
    };
    const Case cases[] = {
        // A Mac (integrated: 4; an M3 or later Max or Ultra: 9).
        { 1280, 720, 4, 2, "Mac at 1280x720: 1x" },
        { 1280, 800, 4, 2, "Mac at 1280x800 (16:10): 1x" },
        { 2560, 1440, 4, 4, "Mac, a 1280x720-point window on Retina (2560x1440): 2x" },
        { 2560, 1600, 4, 4, "Mac at 2560x1600: 2x" },
        { 3024, 1964, 4, 4, "MacBook Pro 14 filled (3024x1964): 2x" },
        { 3456, 2234, 4, 4, "MacBook Pro 16 filled: 2x" },
        { 3840, 2160, 4, 4, "Mac at 4K: 2x (2x2 within budget 4)" },
        { 3840, 2160, 9, 4, "M3 Max at 4K: 3x3, 2x" },
        // Steam Machine (discrete: 6).
        { 3440, 1440, 6, 4, "Steam Machine at 3440x1440: 3x2, 2x" },
        { 3840, 2160, 6, 4, "Steam Machine at 4K: 2x3, 2x" },
        { 1920, 1080, 6, 4, "Steam Machine at 1080p: 2x" },
        // Windows: discrete 6, other integrated 4, Intel integrated 1.
        { 1920, 1080, 6, 4, "Windows discrete GPU at 1080p: 2x" },
        { 1280, 720, 6, 2, "Windows discrete GPU at 720p: 1x" },
        { 1920, 1080, 4, 4, "Windows non-Intel integrated GPU at 1080p: 2x" },
        { 1920, 1080, 1, 2, "Windows Intel UHD 620 at 1080p (Auto renders 1280x720): 1x" },
        { 1920, 991, 1, 2, "Windows Intel iGPU, maximized 1920x991 window: 1x" },
        { 3840, 2160, 1, 2, "Windows Intel iGPU at 4K: 1x" },
        // iPhone and iPad (video::MobileScaleBudget: 1 on every chip).
        { 2868, 1320, video::MobileScaleBudget("Apple A17 Pro GPU").pixels, 2, "iPhone 15 Pro Max: 1x" },
        { 2868, 1320, video::MobileScaleBudget("Apple A20 Pro GPU").pixels, 2, "iPhone 18 Pro Max: 1x" },
        { 2732, 2048, video::MobileScaleBudget("Apple M2 GPU").pixels, 2, "iPad Pro M2: 1x" },
    };
    for (const Case& c : cases)
        CHECK(AutoShadowHalves(AutoNativeScale(c.w, c.h, c.budget)) == c.halves, c.name);

    // Steam Deck and Steam Frame (integrated: 4): 1x at every output, though
    // Internal Resolution's Auto renders 2x2 docked at 1080p (2x shadows cost
    // the Deck ~12 fps there, issues/90/v024/dev/deck); anything else on the
    // same scales keeps the rule.
    using video::SteamDevice;
    struct DeviceCase
    {
        uint32_t w, h;
        const char* name;
    };
    const DeviceCase handheld[] = {
        { 1280, 800, "Deck's own 1280x800" }, { 1920, 1080, "docked at 1080p (2x2)" }, { 2560, 1440, "docked at 1440p" },
        { 3840, 2160, "docked at 4K" }, { 1280, 720, "Frame at 1280x720" }, { 1920, 1080, "Frame at 1920x1080 (2x2)" },
    };
    for (const DeviceCase& c : handheld)
    {
        const gpu::RenderScale scene = AutoNativeScale(c.w, c.h, 4);
        CHECK(AutoShadowHalves(scene, SteamDevice::Deck) == 2, c.name);
        CHECK(AutoShadowHalves(scene, SteamDevice::Frame) == 2, c.name);
    }
    CHECK(AutoShadowHalves(AutoNativeScale(1920, 1080, 4)) == 4, "Deck-sized GPU elsewhere at 1080p (2x2): 2x");
    CHECK(AutoShadowHalves(AutoNativeScale(1920, 1080, 4), SteamDevice::Other) == 4, "Other: the rule");
    CHECK(AutoShadowHalves(RenderScale::Whole(3, 3), SteamDevice::Deck) == 2, "a Deck at a fixed 3x: still 1x");
    // The device, from what the machine says (presenter.cpp's DMI names, refresh_notice.h's arm64 Linux rule).
    CHECK(video::SteamDeviceFrom(false, "Valve", "Jupiter") == SteamDevice::Deck, "Deck LCD");
    CHECK(video::SteamDeviceFrom(false, "Valve", "Galileo") == SteamDevice::Deck, "Deck OLED");
    CHECK(video::SteamDeviceFrom(true, "", "") == SteamDevice::Frame, "arm64 Linux: the Frame");
    CHECK(video::SteamDeviceFrom(false, "Valve", "Fremont") == SteamDevice::Other, "another Valve product name");
    CHECK(video::SteamDeviceFrom(false, "LENOVO", "Jupiter") == SteamDevice::Other, "not Valve");
    CHECK(video::SteamDeviceFrom(false, "", "") == SteamDevice::Other, "no DMI (Mac, Windows)");
    CHECK(std::string(video::SteamDeviceName(SteamDevice::Deck)) == "Steam Deck" && std::string(video::SteamDeviceName(SteamDevice::Frame)) == "Steam Frame",
        "names for the log");
    // Smooth Motion halves the budget (renderer.cpp WantedRenderScale): a
    // Mac at 1440p then renders at most 2 host pixels per guest pixel, and
    // Auto keeps 1x.
    CHECK(AutoShadowHalves(AutoNativeScale(2560, 1440, 4 / 2)) == 2, "Smooth Motion at 1440p on a Mac: 1x");

    if (argc > 1)
    {
        const std::string r = ReadText(argv[1]);
        CHECK(r.find("return gpu::ShadowScale(setting == settings::kShadowResolutionAuto ? gpu::AutoShadowHalves(s_renderScale, video::ThisSteamDevice()) : uint32_t(setting));")
                != std::string::npos,
            "the renderer resolves Auto with the current render scale and this device");
        CHECK(r.find("return std::format(\"Auto: {} ({}; the scene at {})\"") != std::string::npos, "the log says why Auto is 1x on a Deck or Frame");
        CHECK(r.find("s_shadowScale = WantedShadowScale(s_shadowResolutionSetting);") != std::string::npos, "at start, after the render scale");
        CHECK(r.find("UpdateShadowResolution();  // Auto follows the new scale") != std::string::npos, "again after a render scale change");
    }
    if (argc > 2)
    {
        const std::string s = ReadText(argv[2]);
        CHECK(s.find("{ kShadowResolutionAuto, \"auto\", \"Auto\" },") != std::string::npos, "an Auto entry, saved as \"auto\"");
        CHECK(s.find("always 1x on Steam Deck and Steam Frame.") != std::string::npos, "the help text says so");
    }
    if (s_failures)
    {
        printf("%d check(s) FAILED\n", s_failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
