// SpeedBreaker tests. GPL-3.0-or-later (see COPYING).
//
// Shadow Resolution's Auto (gpu/shadow_auto.h, GitHub #90): while the scene
// renders at 2x or more, the wider map (9/4 x 1, 3600x1600) on Apple GPUs and
// the 2x map (3200x3200) on every other GPU, 1x otherwise, at every screen
// shape; 1x on a Steam Deck or Steam Frame at any output
// (video/steam_device.h); the setting's own square 1x / 1.5x / 2x unchanged
// (ShadowScale's); with the renderer's own scales (video::NativeShape for an
// Internal Resolution step, video::AutoNativeScale for its Auto at each
// device's budget: renderer.cpp ReadRenderScale); Auto as the option's
// default; and gpu/renderer.cpp applying it with the current render scale,
// one Apple-GPU flag (kAppleGpu) and again after a scale change.
// Shadow Edges' Auto (ShadowEdgesSmooth): Classic on a Steam Deck, Smooth
// everywhere else, Smooth and Classic as chosen; the renderer reading it at
// start and live, the receivers' Classic variant translated without PCF and
// taken only by them; its Auto entry and help text.
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
    using gpu::AutoShadowScale;
    using gpu::AxisFromHalves;
    using gpu::RenderScale;
    using video::AutoNativeScale;
    using video::NativeShape;
    static_assert(settings::kShadowResolutionDefault == settings::kShadowResolutionAuto, "Auto is the default");
    // Auto's maps: 1x (1600x1600); at 2x or more the wider one on Apple GPUs,
    // 9/4 across the view and 1 along it (3600x1600: 2.25 times the 1x map's
    // texels, as the setting's 1.5x), and the 2x map elsewhere (3200x3200: 4
    // times, the setting's own 2x).
    const RenderScale kOne = RenderScale::Whole(1, 1);
    const RenderScale kTwo = RenderScale::Whole(2, 2);
    const RenderScale kWide{ { 9, 4 }, { 1, 1 } };
    CHECK(gpu::kAutoShadowWide == kWide, "the wider map is 9/4 x 1");
    CHECK(kWide.x.Size(1600) == 3600 && kWide.y.Size(1600) == 1600, "a 3600x1600 map");
    CHECK(kWide.Exact(1600, 1600) && kWide.x.Edge(1600) == 3600 && kWide.y.Edge(1600) == 1600, "every guest texel has whole host ones");
    CHECK(uint64_t(kWide.x.Size(1600)) * kWide.y.Size(1600) * 4 == uint64_t(1600) * 1600 * 9, "2.25 times the 1x map's texels");
    // Apple GPUs (appleGpu true): the wider map at 2x or more.
    CHECK(AutoShadowScale(RenderScale::Whole(1, 1), true) == kOne && AutoShadowScale(RenderScale::Whole(2, 2), true) == kWide,
        "Apple GPU: 1x -> 1x, 2x -> the wider map");
    CHECK(AutoShadowScale(RenderScale::Whole(3, 3), true) == kWide && AutoShadowScale(RenderScale::Whole(3, 2), true) == kWide,
        "Apple GPU: 3x, 3x2: the wider map");
    CHECK(AutoShadowScale(RenderScale::Whole(2, 1), true) == kOne && AutoShadowScale(RenderScale::Whole(1, 2), true) == kOne, "Apple GPU: 2x1, 1x2: 1x");
    // Every other GPU: the 2x map at 2x or more, as v0.2.4 and v0.2.5 picked.
    CHECK(AutoShadowScale(RenderScale::Whole(1, 1), false) == kOne && AutoShadowScale(RenderScale::Whole(2, 2), false) == kTwo,
        "other GPUs: 1x -> 1x, 2x -> the 2x map");
    CHECK(AutoShadowScale(RenderScale::Whole(3, 3), false) == kTwo && AutoShadowScale(RenderScale::Whole(3, 2), false) == kTwo,
        "other GPUs: 3x, 3x2: the 2x map");
    CHECK(AutoShadowScale(RenderScale::Whole(2, 1), false) == kOne && AutoShadowScale(RenderScale::Whole(1, 2), false) == kOne, "other GPUs: 2x1, 1x2: 1x");
    // The setting's own steps are as they were; the 2x map is the setting's
    // 2x, and the wider map none of its steps.
    CHECK(gpu::ShadowScale(2) == kOne && (gpu::ShadowScale(3) == RenderScale{ { 3, 2 }, { 3, 2 } }) && gpu::ShadowScale(4) == kTwo,
        "1x, 1.5x, 2x: 1600x1600, 2400x2400, 3200x3200");
    CHECK(!(kWide == gpu::ShadowScale(3)) && !(kWide == gpu::ShadowScale(4)), "Auto's wider map isn't a setting's step");
    // The pool row for it (render_scale.h): 12 KB a guest page, the 2x row's pages.
    CHECK(kWide.SlotWords() == 3072 && gpu::ShadowPoolMegabytes(kWide) == 288, "3072 words a page, 288 MB");
    CHECK((gpu::ShadowPoolMegabytes(kWide) << 20) / (4ull * kWide.SlotWords()) == (gpu::ShadowPoolMegabytes(RenderScale::Whole(2, 2)) << 20) / (4ull * 4096),
        "as many pages as the 2x row");

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
        for (const bool apple : { true, false })
        {
            const RenderScale above = apple ? kWide : kTwo;
            CHECK(AutoShadowScale(NativeShape(AxisFromHalves(2), s.w, s.h), apple) == kOne, s.name);
            CHECK(AutoShadowScale(NativeShape(AxisFromHalves(3), s.w, s.h), apple) == kOne, s.name);
            CHECK(AutoShadowScale(NativeShape(AxisFromHalves(4), s.w, s.h), apple) == above, s.name);
            CHECK(AutoShadowScale(NativeShape(AxisFromHalves(6), s.w, s.h), apple) == above, s.name);
        }

    // Internal Resolution's Auto, device by device (the budgets
    // renderer.cpp's ReadRenderScale gives), at the screens they meet.
    struct Case
    {
        uint32_t w, h, budget;
        bool apple;  // an Apple GPU (renderer.cpp's kAppleGpu: every Apple build)
        bool above;  // above 1x: the wider map on Apple GPUs, the 2x map elsewhere
        const char* name;
    };
    const Case cases[] = {
        // A Mac (integrated: 4; an M3 or later Max or Ultra: 9).
        { 1280, 720, 4, true, false, "Mac at 1280x720: 1x" },
        { 1280, 800, 4, true, false, "Mac at 1280x800 (16:10): 1x" },
        { 2560, 1440, 4, true, true, "Mac, a 1280x720-point window on Retina (2560x1440): the wider map" },
        { 2560, 1600, 4, true, true, "Mac at 2560x1600: the wider map" },
        { 3024, 1964, 4, true, true, "MacBook Pro 14 filled (3024x1964): the wider map" },
        { 3456, 2234, 4, true, true, "MacBook Pro 16 filled: the wider map" },
        { 3840, 2160, 4, true, true, "Mac at 4K (2x2 within budget 4): the wider map" },
        { 3840, 2160, 9, true, true, "M3 Max at 4K (3x3): the wider map" },
        // Steam Machine (discrete: 6).
        { 3440, 1440, 6, false, true, "Steam Machine at 3440x1440 (3x2): the 2x map" },
        { 3840, 2160, 6, false, true, "Steam Machine at 4K (2x3): the 2x map" },
        { 1920, 1080, 6, false, true, "Steam Machine at 1080p (2x2): the 2x map" },
        // Windows: discrete 6, other integrated 4, Intel integrated 1.
        { 1920, 1080, 6, false, true, "Windows discrete GPU at 1080p (2x2): the 2x map" },
        { 1280, 720, 6, false, false, "Windows discrete GPU at 720p: 1x" },
        { 1920, 1080, 4, false, true, "Windows non-Intel integrated GPU at 1080p (2x2): the 2x map" },
        { 1920, 1080, 1, false, false, "Windows Intel UHD 620 at 1080p (Auto renders 1280x720): 1x" },
        { 1920, 991, 1, false, false, "Windows Intel iGPU, maximized 1920x991 window: 1x" },
        { 3840, 2160, 1, false, false, "Windows Intel iGPU at 4K: 1x" },
        // iPhone and iPad (video::MobileScaleBudget: 1 on every chip).
        { 2868, 1320, video::MobileScaleBudget("Apple A17 Pro GPU").pixels, true, false, "iPhone 15 Pro Max: 1x" },
        { 2868, 1320, video::MobileScaleBudget("Apple A20 Pro GPU").pixels, true, false, "iPhone 18 Pro Max: 1x" },
        { 2732, 2048, video::MobileScaleBudget("Apple M2 GPU").pixels, true, false, "iPad Pro M2: 1x" },
    };
    for (const Case& c : cases)
        CHECK(AutoShadowScale(AutoNativeScale(c.w, c.h, c.budget), c.apple) == (c.above ? (c.apple ? kWide : kTwo) : kOne), c.name);

    // Steam Deck and Steam Frame (integrated: 4): 1x at every output, though
    // Internal Resolution's Auto renders 2x2 docked at 1080p (the 2x map cost
    // the Deck ~12 fps there, #90; the wider one still costs frames in its
    // heaviest city scene); anything else on the same scales keeps the rule.
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
        for (const bool apple : { false, true })
        {
            CHECK(AutoShadowScale(scene, apple, SteamDevice::Deck) == kOne, c.name);
            CHECK(AutoShadowScale(scene, apple, SteamDevice::Frame) == kOne, c.name);
        }
    }
    CHECK(AutoShadowScale(AutoNativeScale(1920, 1080, 4), false) == kTwo, "Deck-sized GPU elsewhere at 1080p (2x2): the 2x map");
    CHECK(AutoShadowScale(AutoNativeScale(1920, 1080, 4), false, SteamDevice::Other) == kTwo, "Other: the rule");
    CHECK(AutoShadowScale(RenderScale::Whole(3, 3), false, SteamDevice::Deck) == kOne, "a Deck at a fixed 3x: still 1x");
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
    CHECK(AutoShadowScale(AutoNativeScale(2560, 1440, 4 / 2), true) == kOne, "Smooth Motion at 1440p on a Mac: 1x");

    // Shadow Edges (user/settings.h ShadowEdgesMode: 0 Auto, 1 Smooth, 2 Classic).
    CHECK(!gpu::ShadowEdgesSmooth(0, SteamDevice::Deck), "Auto on a Steam Deck: Classic");
    CHECK(gpu::ShadowEdgesSmooth(0, SteamDevice::Frame) && gpu::ShadowEdgesSmooth(0, SteamDevice::Other) && gpu::ShadowEdgesSmooth(0),
        "Auto on a Steam Frame and everywhere else: Smooth");
    CHECK(gpu::ShadowEdgesSmooth(1, SteamDevice::Deck) && gpu::ShadowEdgesSmooth(1, SteamDevice::Other), "Smooth as chosen, a Deck too");
    CHECK(!gpu::ShadowEdgesSmooth(2, SteamDevice::Other) && !gpu::ShadowEdgesSmooth(2, SteamDevice::Frame), "Classic as chosen");

    if (argc > 1)
    {
        const std::string r = ReadText(argv[1]);
        CHECK(r.find("return setting == settings::kShadowResolutionAuto ? gpu::AutoShadowScale(s_renderScale, kAppleGpu, video::ThisSteamDevice()) : "
                     "gpu::ShadowScale(uint32_t(setting));") != std::string::npos,
            "the renderer resolves Auto with the current render scale, this GPU and this device, a chosen step as before");
        CHECK(r.find("return gpu::AutoShadowScale(scene, kAppleGpu, video::ThisSteamDevice());") != std::string::npos, "the menu's Auto label asks the same rule");
        {
            const char* flag = "#ifdef __APPLE__\n        constexpr bool kAppleGpu = true;\n#else\n        constexpr bool kAppleGpu = false;\n#endif";
            size_t definitions = 0;
            for (size_t at = r.find("constexpr bool kAppleGpu"); at != std::string::npos; at = r.find("constexpr bool kAppleGpu", at + 1))
                definitions++;
            CHECK(r.find(flag) != std::string::npos && definitions == 2, "one Apple-GPU flag: every Apple build, as HostScaledResolves takes it");
            CHECK(r.find("gpu::HostScaledResolves(s, kAppleGpu)") != std::string::npos, "the same flag there");
        }
        CHECK(r.find("AutoShadowHalves") == std::string::npos, "no second rule left");
        CHECK(r.find("return std::format(\"Auto: {} (a {}x{} map; {}; the scene at {})\"") != std::string::npos,
            "the log names the map Auto built and says why it is 1x on a Deck or Frame");
        CHECK(r.find("return std::format(\"Auto: {} (a {}x{} map; the scene at {})\", gpu::ScaleName(s_shadowScale), gpu::shadow_ab::MapWidth(s_shadowScale),\n"
                     "                gpu::shadow_ab::MapHeight(s_shadowScale), gpu::ScaleName(s_renderScale));") != std::string::npos,
            "the log names the map Auto built");
        CHECK(r.find("s_shadowScale = WantedShadowScale(s_shadowResolutionSetting);") != std::string::npos, "at start, after the render scale");
        CHECK(r.find("UpdateShadowResolution();  // Auto follows the new scale") != std::string::npos, "again after a render scale change");
        CHECK(r.find("s_shadowEdgesSmooth = gpu::ShadowEdgesSmooth(setting, video::ThisSteamDevice());") != std::string::npos,
            "Shadow Edges resolved for this device");
        CHECK(r.find("UpdateShadowResolution();\n            UpdateShadowEdges();") != std::string::npos, "Shadow Edges read live");
        CHECK(r.find("ShadowResolutionName().c_str());\n            UpdateShadowEdges();") != std::string::npos, "and at start");
        CHECK(r.find("options.shadowPcf = 0;") != std::string::npos && r.find("glsl = &ps.classicGlsl;") != std::string::npos,
            "the Classic variant: the receiver translated without PCF");
        CHECK(r.find("if (!s_shadowEdgesSmooth && s_ps->info.shadowPcfGroups && !(key.pad & 3))\n            key.pad |= 1u << 4;") != std::string::npos,
            "taken by the receivers only, outside Sharper UI's draws");
        CHECK(r.find("(key.pad >> 4) & 1);") != std::string::npos, "the pipeline passes it to its pixel shader variant");
    }
    if (argc > 2)
    {
        const std::string s = ReadText(argv[2]);
        CHECK(s.find("{ kShadowResolutionAuto, \"auto\", \"Auto\" },") != std::string::npos, "an Auto entry, saved as \"auto\"");
        CHECK(s.find("always 1x on Steam Deck and Steam Frame.") != std::string::npos, "the help text says so");
        CHECK(s.find("{ int32_t(ShadowEdgesMode::Auto), \"auto\", \"Auto\" },") != std::string::npos, "Shadow Edges: an Auto entry");
        CHECK(s.find("Auto: Classic on a Steam Deck, Smooth everywhere else.") != std::string::npos, "its help text says what Auto picks");
    }
    if (s_failures)
    {
        printf("%d check(s) FAILED\n", s_failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
