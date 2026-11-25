// Tests for gpu::bloom (runtime/gpu/bloom.h): NFSMW_BLOOM's names and alt
// order, auto's rule by Internal Resolution, the combines the filter covers,
// which surfaces the scale applies to, which reads the filter and the
// fill take, the fetch rewrite, the B-spline weights the filter's GLSL
// computes, the texels the first pass covers with and without fill, and the
// memory and pixel arithmetic (GitHub #106); renderer.cpp is read to check it
// applies the rules.
// From the repo root:
//   clang++ -std=c++20 -O2 -Iruntime tests/bloom_test.cpp -o build/bloom_test
//   build/bloom_test runtime/gpu/renderer.cpp
#include <gpu/bloom.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using namespace gpu::bloom;

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void TestParse()
{
    // Every value sets all three: a mode's name, alt and auto each turn the
    // other two off, whatever they were.
    struct Case { const char* text; uint32_t mode; bool alt; bool autoMode = false; };
    for (const Case& c : { Case{ nullptr, kOff, false }, Case{ "", kOff, false }, Case{ "0", kOff, false }, Case{ "off", kOff, false },
             Case{ "scale", kScale, false }, Case{ "scalefull", kScale | kSceneHost, false }, Case{ "filter", kFilter, false },
             Case{ "both", kScale | kFilter, false }, Case{ "bothfull", kScale | kSceneHost | kFilter, false }, Case{ "fill", kFill, false },
             Case{ "fillfilter", kFill | kFilter, false }, Case{ "fillscale", kFill | kScale, false },
             Case{ "all", kFill | kScale | kFilter, false }, Case{ "alt", kOff, true }, Case{ "auto", kOff, false, true } })
        for (uint32_t before : { uint32_t(kOff), uint32_t(kScale | kFilter) })
            for (bool altBefore : { false, true })
                for (bool autoBefore : { false, true })
                {
                    uint32_t mode = before;
                    bool alt = altBefore, autoMode = autoBefore;
                    CHECK(ParseMode(c.text, mode, alt, autoMode) && mode == c.mode && alt == c.alt && autoMode == c.autoMode,
                        "'%s' gives %s%s%s", c.text ? c.text : "(unset)", ModeName(mode), alt ? " alt" : "", autoMode ? " auto" : "");
                }
    // Anything else: refused, and the mode, alt and auto left as they were.
    for (const char* text : { "1", "on", "2", "2x", "SCALE", "scale ", "bilinear", "bicubic", "full", "fillfull", "fill+scale", "ALL", "AUTO",
             "auto ", "Auto", "automatic", "aut", "a", "al", "default" })
        for (bool autoBefore : { false, true })
        {
            uint32_t mode = kFilter;
            bool alt = true, autoMode = autoBefore;
            CHECK(!ParseMode(text, mode, alt, autoMode) && mode == kFilter && alt && autoMode == autoBefore, "'%s' is refused", text);
        }
    // Every mode's name round-trips.
    for (uint32_t m : kAltOrder)
    {
        uint32_t back = 99;
        bool alt = true, autoMode = true;
        CHECK(ParseMode(ModeName(m), back, alt, autoMode) && back == m && !alt && !autoMode, "%s round-trips", ModeName(m));
    }
}

// NFSMW_BLOOM=auto: fillfilter where Internal Resolution is 2x or more, fill
// below; fill at any scale on a Steam Deck.
static void TestAuto()
{
    // Today's rule, pinned here and nowhere else in this test (the cases
    // below follow the header's constants): unset is auto, the filter from
    // 2x, fill below. Making off the default again, or giving 1x the filter,
    // is the header's one line and this one.
    CHECK(kAutoByDefault && kAutoFilterFrom == 2 && kAutoFrom == (kFill | kFilter) && kAutoBelow == kFill, "the rule's constants");
    // Neither mode scales the chain (a change of scale while the game runs
    // needs no target made again for the mode), both fill, and from the
    // threshold on the filter is on.
    CHECK(!((kAutoFrom | kAutoBelow) & (kScale | kSceneHost)) && (kAutoFrom & kFill) && (kAutoBelow & kFill) && (kAutoFrom & kFilter),
        "auto's two modes");
    static_assert(AutoMode(gpu::RenderScale{ { kAutoFilterFrom, 1 }, { kAutoFilterFrom, 1 } }) == kAutoFrom);
    static_assert(AutoMode(gpu::RenderScale{ { 1, 1 }, { 1, 1 } }) == kAutoBelow);
    const uint32_t from = kAutoFrom, below = kAutoBelow;
    struct Case { const char* scale; uint32_t mode; const char* what; };
    for (const Case& c : {
             // The setting's steps on a 16:9 picture.
             Case{ "1", below, "1x" }, Case{ "1.5", below, "1.5x: below 2x" }, Case{ "2", from, "2x" }, Case{ "3", from, "3x" },
             Case{ "4", from, "4x (NFSMW_RENDER_SCALE)" },
             // A Vert+ or Hor+ picture's own rows or columns: the setting's
             // number is the smaller axis.
             Case{ "1x10/9", below, "the Deck's 1280x800 at 1x" }, Case{ "1.5x5/3", below, "1920x1200 at 1.5x" },
             Case{ "2x20/9", from, "2560x1600 at 2x" }, Case{ "43/16x2", from, "3440x1440 at 2x (2.6875x2)" },
             Case{ "43/32x1", below, "an ultrawide at 1x" }, Case{ "3x10/3", from, "3x on 16:10" },
             // Auto's whole scales for a wide or tall picture: both axes count.
             Case{ "3x2", from, "Auto on 3440x1440" }, Case{ "2x3", from, "2x3" }, Case{ "2x1", below, "one axis at 1x" },
             Case{ "1x2", below, "one axis at 1x" }, Case{ "3x1", below, "one axis at 1x" }, Case{ "2x1.5", below, "one axis at 1.5x" },
             Case{ "1.5x2", below, "one axis at 1.5x" }, Case{ "1.5x3", below, "one axis at 1.5x" },
             // Just under and just over.
             Case{ "199/100x2", below, "1.99x across" }, Case{ "2x199/100", below, "1.99x down" }, Case{ "201/100x201/100", from, "2.01x" } })
    {
        gpu::RenderScale s;
        CHECK(gpu::ParseRenderScale(c.scale, s), "'%s' is a scale", c.scale);
        CHECK(AutoMode(s) == c.mode, "%s (%s) gives %s", c.scale, c.what, ModeName(AutoMode(s)));
    }
    // The setting's own values (half steps: 2 is 1x, 3 is 1.5x, 4 is 2x, 6 is 3x).
    for (uint32_t halves = 2; halves <= 8; halves++)
    {
        const gpu::AxisScale a = gpu::AxisFromHalves(halves);
        CHECK(AutoMode({ a, a }) == (halves >= 2 * kAutoFilterFrom ? from : below), "%u half steps give %s", halves, ModeName(AutoMode({ a, a })));
    }
    // An unreduced ratio counts by its value (4/2 is 2x, 3/2 is not).
    CHECK(AutoMode({ { 4, 2 }, { 6, 3 } }) == from && AutoMode({ { 4, 2 }, { 3, 2 } }) == below, "ratios by value");
    // A Steam Deck: fill at any scale (its own screen's 1x10/9, 1920x1080's
    // 2x2, a 4K screen's 3x3); the Steam Frame and every other machine (the
    // Steam Machine is Other) by the scale alone.
    using video::SteamDevice;
    static_assert(AutoMode(gpu::RenderScale{ { 2, 1 }, { 2, 1 } }, SteamDevice::Deck) == kAutoBelow);
    for (const char* scale : { "1", "1x10/9", "1.5", "2", "2x20/9", "3", "3x2", "4" })
    {
        gpu::RenderScale s;
        CHECK(gpu::ParseRenderScale(scale, s), "'%s' is a scale", scale);
        CHECK(AutoMode(s, SteamDevice::Deck) == below, "a Steam Deck at %s gives %s", scale, ModeName(AutoMode(s, SteamDevice::Deck)));
        CHECK(AutoMode(s, SteamDevice::Frame) == AutoMode(s) && AutoMode(s, SteamDevice::Other) == AutoMode(s),
            "%s: the Frame and other machines go by the scale", scale);
    }
    CHECK(video::SteamDeviceFrom(false, "Valve", "Galileo") == SteamDevice::Deck && video::SteamDeviceFrom(false, "Valve", "Jupiter") == SteamDevice::Deck &&
              video::SteamDeviceFrom(false, "Valve", "Fremont") == SteamDevice::Other, "the Deck's two names; the Steam Machine isn't one");
}

// The combines the filter covers unless NFSMW_BLOOM_PS names others.
static void TestCombines()
{
    CHECK(kCombineCount == 2 && kCombines[0] == kCombinePs && kCombines[1] == kShotCombinePs, "the default list");
    CHECK(kCombinePs == 0xd52a5eb6c17f9598ull && kShotCombinePs == 0x74c891970bd63fcaull && kCombinePs != kDownsamplePs &&
              kShotCombinePs != kDownsamplePs, "the two hashes (neither the first pass: the variant bits are shared)");
    // The default is what naming both gives.
    uint64_t v[8];
    uint32_t n = 0;
    CHECK(ParseShaderList("ps_d52a5eb6c17f9598,ps_74c891970bd63fca", v, n) && n == kCombineCount && v[0] == kCombines[0] && v[1] == kCombines[1],
        "NFSMW_BLOOM_PS with both names");
    // The camera shots' combine reads six textures (0-5): the chain's two
    // sit where the race combine's do (slot 1, 320 wide; slot 3, 160 wide),
    // and its other slots are neither the filter's nor the fill's: the scene
    // (0), the vignette (2), the depth of field's half-size picture (4: 640
    // wide, at the scene's scale) and the depth (5).
    CHECK(FilterSlot(1, 320, true, 1280) && FilterSlot(3, 160, true, 1280), "the chain's two textures");
    for (uint32_t slot : { 0u, 2u, 4u, 5u })
        for (uint32_t width : { 1280u, 256u, 640u })
            CHECK(!FilterSlot(slot, width, true, 1280) && !FillSlot(slot, width, true, 1280), "slot %u (%u wide) is taken", slot, width);
    // Its bloom read sits behind a flag; the rewrite takes it there too and
    // leaves slots 4 and 5 alone.
    std::string glsl = "    if (boolConst(129u) == true) {\n        {\n            vec4 tdata = texture(t2d_4, vec2(r[0].x, r[0].y), fetchLodBias(4u));\n"
                       "        }\n    }\n    {\n        vec4 tdata = texture(t2d_1, vec2(r[0].x, r[0].y), fetchLodBias(1u));\n    }\n"
                       "    {\n        vec4 tdata = texture(t2d_5, vec2(r[0].x, r[0].y), fetchLodBias(5u));\n    }\n"
                       "    if (boolConst(128u) == true) {\n        {\n            vec4 tdata = texture(t2d_3, vec2(r[0].x, r[0].y), fetchLodBias(3u));\n"
                       "        }\n    }\n";
    CHECK(RewriteFetches(glsl, 0b01010) == 2, "two reads rewritten");
    CHECK(glsl.find("sbBloomTex(t2d_1, vec2(r[0].x, r[0].y), fetchLodBias(1u))") != std::string::npos &&
              glsl.find("            vec4 tdata = sbBloomTex(t2d_3, vec2(r[0].x, r[0].y), fetchLodBias(3u));") != std::string::npos &&
              glsl.find("texture(t2d_4, ") != std::string::npos && glsl.find("texture(t2d_5, ") != std::string::npos &&
              glsl.find("texture(t2d_1, ") == std::string::npos && glsl.find("texture(t2d_3, ") == std::string::npos,
        "slots 1 and 3 through the filter, 4 and 5 as they were");
}

static void TestAlt()
{
    // Off first (the run starts as today), each mode a whole [perf] window, then round again.
    CHECK(AltMode(0) == kOff && AltMode(119) == kOff, "frames 0-119: off");
    CHECK(AltMode(120) == kScale && AltMode(240) == kFilter && AltMode(360) == (kScale | kFilter) && AltMode(480) == kFill &&
              AltMode(600) == (kFill | kFilter) && AltMode(720) == (kFill | kScale) && AltMode(840) == (kFill | kScale | kFilter),
        "the order: off, scale, filter, both, fill, fillfilter, fillscale, all");
    CHECK(kAltCount == 8 && AltMode(960) == kOff && AltMode(960 + 239) == kScale, "round again after eight windows");
    CHECK(kAltWindow == 120, "a window is the [perf] line's 120 frames");
    // NFSMW_BLOOM_ALT_FRAMES: another window (0 never divides).
    CHECK(AltMode(0, 2) == kOff && AltMode(1, 2) == kOff && AltMode(2, 2) == kScale && AltMode(15, 2) == (kFill | kScale | kFilter) &&
              AltMode(16, 2) == kOff, "a window of 2 frames");
    CHECK(AltMode(3, 0) == AltMode(3, 1), "a window of 0 is taken as 1");
}

static void TestPitches()
{
    // Off, filter and fill: no surface changes.
    for (uint32_t mode : { uint32_t(kOff), uint32_t(kFilter), uint32_t(kFill), uint32_t(kFill | kFilter) })
        for (uint32_t pitch : { 80u, 160u, 280u, 320u, 640u, 1280u })
            CHECK(!ScaledPitch(mode, pitch, false), "%s scales pitch %u", ModeName(mode), pitch);
    for (uint32_t mode : { uint32_t(kScale), uint32_t(kScale | kSceneHost), uint32_t(kScale | kFilter), uint32_t(kFill | kScale),
             uint32_t(kFill | kScale | kFilter) })
    {
        CHECK(ScaledPitch(mode, 320, false) && ScaledPitch(mode, 160, false), "%s: the chain's 320 and 160", ModeName(mode));
        // The luminance (80: read back by the CPU), the cube (280), the
        // reflection (256), the shadow map (1600), and any depth: not the chain's.
        for (uint32_t pitch : { 32u, 64u, 80u, 256u, 280u, 288u, 319u, 321u, 640u, 1280u, 1600u })
            CHECK(!ScaledPitch(mode, pitch, false), "%s scales pitch %u", ModeName(mode), pitch);
        CHECK(!ScaledPitch(mode, 320, true) && !ScaledPitch(mode, 160, true), "%s scales a depth surface", ModeName(mode));
    }
    // The first pass's read of the scene: 1x under scale, host under scalefull;
    // the chain's own textures (320 and narrower) always as drawn.
    CHECK(SceneReadAt1x(kScale, true, 1280) && SceneReadAt1x(kScale | kFilter, true, 1280), "scale: the scene at 1x");
    CHECK(SceneReadAt1x(kFill | kScale, true, 1280) && SceneReadAt1x(kFill | kScale | kFilter, true, 1280),
        "fill with scale: the scene at 1x (the box steps by its texels)");
    CHECK(!SceneReadAt1x(kScale | kSceneHost, true, 1280), "scalefull: the scene at host resolution");
    CHECK(!SceneReadAt1x(kScale, true, 320) && !SceneReadAt1x(kScale, true, 160), "scale: the chain's own textures as drawn");
    CHECK(!SceneReadAt1x(kScale, false, 1280) && !SceneReadAt1x(kOff, true, 1280) && !SceneReadAt1x(kFilter, true, 1280),
        "only draws into the chain's scaled surfaces");
}

static void TestShaderList()
{
    uint64_t v[8];
    uint32_t n = 99;
    CHECK(ParseShaderList(nullptr, v, n) && n == 0, "unset: none (the renderer keeps its default)");
    CHECK(ParseShaderList("ps_d52a5eb6c17f9598", v, n) && n == 1 && v[0] == kCombinePs, "one, with ps_");
    CHECK(ParseShaderList("d52a5eb6c17f9598,PS_0000000000000001", v, n) == false, "PS_ (capitals) is not the prefix");
    CHECK(ParseShaderList("d52a5eb6c17f9598,ps_00000000000000ff", v, n) && n == 2 && v[1] == 0xff, "two");
    CHECK(!ParseShaderList("d52a5eb6c17f95980", v, n), "17 digits");
    CHECK(!ParseShaderList("d52a,,1", v, n), "an empty entry");
    CHECK(!ParseShaderList("xyz", v, n), "not hex");
    CHECK(!ParseShaderList("1,2,3,4,5,6,7,8,9", v, n), "more than 8");
}

static void TestFilterSlot()
{
    // The combine at 1280: the chain's 320x180 and 160x90 textures, linear.
    CHECK(FilterSlot(3, 160, true, 1280) && FilterSlot(1, 320, true, 1280), "the chain's textures");
    CHECK(!FilterSlot(0, 1280, true, 1280), "the scene");
    CHECK(!FilterSlot(2, 256, true, 1280) && !FilterSlot(2, 64, true, 1280), "the vignette (256x256), a small texture of another size");
    CHECK(!FilterSlot(3, 160, false, 1280), "a point-filtered texture (the B-spline is made of bilinear taps)");
    CHECK(!FilterSlot(3, 160, true, 320), "a draw into the chain itself");
    CHECK(!FilterSlot(5, 160, true, 1280) && kSlotBits == 5, "slots past 4");
}

static void TestFillSlot()
{
    // The first pass: the scene (1280 wide, linear) into a 320-wide surface.
    CHECK(FillSlot(0, 1280, true, 320), "the scene, into the chain's 320");
    CHECK(!FillSlot(0, 320, true, 320) && !FillSlot(0, 160, true, 320), "the chain's own textures (the blurs)");
    CHECK(!FillSlot(0, 1280, true, 160) && !FillSlot(0, 1280, true, 1280) && !FillSlot(0, 1280, true, 80), "another target");
    CHECK(!FillSlot(0, 1280, false, 320), "a point-filtered read");
    CHECK(!FillSlot(5, 1280, true, 320), "slots past 4");
    CHECK(kDownsamplePs == 0x387140822b5b9e76ull && kDownsamplePs != kCombinePs, "the first pass's shader");
}

// What the first pass covers, one row of it: its taps sit 2 and 6 scene
// texels either side of the pixel's centre (a texel corner, the 320 grid
// being a quarter of the scene's), each a bilinear read. The game's: 2 texels
// of every 4. With fill (each tap two reads a texel either side, the GLSL's
// diagonal in one dimension): all 16, each a sixteenth.
static void TestFillCover()
{
    const int n = 64;
    auto cover = [&](bool fill, double centre, double* weight) {
        for (int i = 0; i < n; i++)
            weight[i] = 0.0;
        auto bilinear = [&](double u, double w) {
            const double f = u - 0.5;
            const int i = int(std::floor(f));
            const double a = f - i;
            weight[i] += w * (1.0 - a);
            weight[i + 1] += w * a;
        };
        for (double tap : { -6.0, -2.0, 2.0, 6.0 })
            if (fill)
            {
                bilinear(centre + tap - 1.0, 0.125);
                bilinear(centre + tap + 1.0, 0.125);
            }
            else
                bilinear(centre + tap, 0.25);
    };
    double w[64];
    const double centre = 34.0;  // pixel 8 of the 320 grid: scene texels 32-35, its centre the corner between 33 and 34
    cover(false, centre, w);
    int used = 0;
    for (int i = 0; i < n; i++)
        used += w[i] > 0.0;
    CHECK(used == 8, "the game's taps read 8 of the row's 16 texels (%d)", used);
    for (int i = 26; i < 42; i++)
        CHECK(std::fabs(w[i] - ((i & 3) == 3 || (i & 3) == 0 ? 0.125 : 0.0)) < 1e-12, "the game's weight of texel %d: %g", i, w[i]);
    cover(true, centre, w);
    double sum = 0.0;
    for (int i = 0; i < n; i++)
    {
        sum += w[i];
        CHECK(std::fabs(w[i] - (i >= 26 && i < 42 ? 1.0 / 16.0 : 0.0)) < 1e-12, "fill's weight of texel %d: %g", i, w[i]);
    }
    CHECK(std::fabs(sum - 1.0) < 1e-12, "fill keeps the sum (%g)", sum);
    // (The span grows by a texel each side, 14 to 16 of the scene's: the
    // glow's size is the blurs', 30 times wider.)
    CHECK(std::string(FillGlsl()).find("vec4 sbBloomBox(sampler2D s, vec2 uv, float bias)") == 0, "the box's signature");
    CHECK(std::string(FillGlsl()).find("0.25 * (texture(s, uv - d, bias)") != std::string::npos, "four reads, a quarter each");
}

static void TestRewrite()
{
    std::string glsl = "a texture(t2d_0, vec2(r[0].x, r[0].y), fetchLodBias(0u)); texture(t2d_3, uv, fetchLodBias(3u)); "
                       "texture(t2d_3, uv2, b); texture(t2d_1, x, y); texture(t3d_3, u, v); texture(t2d_30, z, w);";
    std::string a = glsl;
    CHECK(RewriteFetches(a, 0) == 0 && a == glsl, "mask 0: unchanged");
    a = glsl;
    CHECK(RewriteFetches(a, 1u << 3) == 2, "slot 3: two reads");
    CHECK(a.find("sbBloomTex(t2d_3, uv, fetchLodBias(3u))") != std::string::npos && a.find("sbBloomTex(t2d_3, uv2, b)") != std::string::npos,
        "slot 3 rewritten with its arguments");
    CHECK(a.find("texture(t2d_0, ") != std::string::npos && a.find("texture(t2d_1, ") != std::string::npos &&
              a.find("texture(t3d_3, ") != std::string::npos && a.find("texture(t2d_30, ") != std::string::npos,
        "other slots, the 3D slot 3 and slot 30 unchanged");
    a = glsl;
    CHECK(RewriteFetches(a, (1u << 1) | (1u << 3)) == 3, "slots 1 and 3: three reads");
    CHECK(std::string(FilterGlsl()).find("vec4 sbBloomTex(sampler2D s, vec2 uv, float bias)") == 0, "the function's signature");
    a = glsl;
    CHECK(RewriteFetches(a, 1u, "sbBloomBox") == 1 && a.find("sbBloomBox(t2d_0, vec2(r[0].x, r[0].y), fetchLodBias(0u))") != std::string::npos,
        "the first pass's read through the box");
}

// The filter's arithmetic, in C++ (the GLSL's lines one for one) against the
// B-spline evaluated directly over the 4x4 texels: a 1D texture of 12 texels,
// bilinear with clamp to edge.
static double Bilinear(const double* t, int n, double uv)
{
    const double p = uv * n - 0.5;
    const int i = int(std::floor(p));
    const double f = p - i;
    auto at = [&](int k) { return t[std::min(std::max(k, 0), n - 1)]; };
    return at(i) * (1 - f) + at(i + 1) * f;
}
static double FilterLikeGlsl(const double* t, int n, double uv)
{
    const double size = n, p = uv * size - 0.5, i = std::floor(p), f = p - i, f2 = f * f, f3 = f2 * f;
    const double w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0, w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
    const double w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0, w3 = f3 / 6.0;
    const double g0 = w0 + w1, g1 = w2 + w3, h0 = (i - 0.5 + w1 / g0) / size, h1 = (i + 1.5 + w3 / g1) / size;
    return g0 * Bilinear(t, n, h0) + g1 * Bilinear(t, n, h1);
}
static double BSplineDirect(const double* t, int n, double uv)
{
    const double p = uv * n - 0.5;
    const int i = int(std::floor(p));
    const double f = p - i;
    auto at = [&](int k) { return t[std::min(std::max(k, 0), n - 1)]; };
    auto b = [](double x) {
        x = std::fabs(x);
        return x < 1 ? (4 - 6 * x * x + 3 * x * x * x) / 6 : x < 2 ? (2 - x) * (2 - x) * (2 - x) / 6 : 0.0;
    };
    double s = 0;
    for (int k = -1; k <= 2; k++)
        s += at(i + k) * b(f - k);
    return s;
}

static void TestBSpline()
{
    const double t[12] = { 0, 0, 0, 0, 1, 1, 0, 0, 0.25, 0.5, 0.75, 1 };
    double worst = 0, sum = 0;
    for (int k = 0; k <= 1200; k++)
    {
        const double uv = 0.05 + 0.9 * k / 1200.0;  // away from the clamped ends
        worst = std::max(worst, std::fabs(FilterLikeGlsl(t, 12, uv) - BSplineDirect(t, 12, uv)));
    }
    CHECK(worst < 1e-12, "the four bilinear taps are the B-spline (worst %.3g)", worst);
    // Weights sum to one: a flat texture reads flat.
    const double flat[12] = { 0.6, 0.6, 0.6, 0.6, 0.6, 0.6, 0.6, 0.6, 0.6, 0.6, 0.6, 0.6 };
    for (int k = 0; k <= 100; k++)
        CHECK(std::fabs(FilterLikeGlsl(flat, 12, k / 100.0) - 0.6) < 1e-12, "flat at %d", k);
    // Its kernel's variance: 1/3 texel^2 against bilinear's 1/6 (the extra
    // blur the header states: sqrt(1/6) = 0.41 texels).
    const double cubicVar = 1.0 / 3.0, tentVar = 1.0 / 6.0;
    CHECK(std::fabs(std::sqrt(cubicVar - tentVar) - 0.408) < 0.001, "sigma added %.3f", std::sqrt(cubicVar - tentVar));
    // A single bright texel keeps its energy (the glow's brightness): the
    // integral over the texture of either read is one texel.
    const double spike[12] = { 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0 };
    double eb = 0, ef = 0;
    for (int k = 0; k < 12000; k++)
    {
        const double uv = (k + 0.5) / 12000.0;
        eb += Bilinear(spike, 12, uv) / 1000.0;
        ef += FilterLikeGlsl(spike, 12, uv) / 1000.0;
    }
    CHECK(std::fabs(eb - 1) < 1e-6 && std::fabs(ef - 1) < 1e-6, "energy: bilinear %.6f, filter %.6f", eb, ef);
    // C1 (no kink at a texel centre, where bilinear's slope jumps): the slope
    // either side of the centre of texel 5 of the spike.
    const double c = 5.5 / 12.0, e = 1e-6;
    const double sb = (Bilinear(spike, 12, c + e) - Bilinear(spike, 12, c)) - (Bilinear(spike, 12, c) - Bilinear(spike, 12, c - e));
    const double sf = (FilterLikeGlsl(spike, 12, c + e) - FilterLikeGlsl(spike, 12, c)) - (FilterLikeGlsl(spike, 12, c) - FilterLikeGlsl(spike, 12, c - e));
    CHECK(std::fabs(sb) > 1e-6 && std::fabs(sf) < 1e-9, "bilinear kinks (%.3g), the filter doesn't (%.3g)", sb, sf);
    (void)sum;
}

static void TestCost()
{
    // The chain's surfaces as the renderer makes them: 2048 rows from bases 0 and 48 (pitch 320) and 0 (pitch 160).
    CHECK(SurfaceBytes(320, 2048, 1, 1) == 2621440 && SurfaceBytes(160, 2048, 1, 1) == 1310720, "1x");
    CHECK(SurfaceBytes(320, 2048, 2, 1) == 4 * SurfaceBytes(320, 2048, 1, 1), "2x: four times");
    CHECK(SurfaceBytes(320, 2048, 3, 2) == 480ull * 3072 * 4, "1.5x");
    CHECK(SurfaceBytes(320, 2048, 3, 1) == 9 * SurfaceBytes(320, 2048, 1, 1), "3x: nine times");
    // The race frame's chain at 1x: 3 x 57,600 + 3 x 14,400 = 216,000 pixels.
    CHECK(ChainPixels(1, 1) == 216000 && ChainPixels(2, 1) == 864000 && ChainPixels(3, 1) == 1944000, "pixels %llu",
        (unsigned long long)ChainPixels(1, 1));
}

static std::string ReadFile(const char* path)
{
    std::ifstream f(path, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

static void TestRenderer(const char* path)
{
    const std::string src = ReadFile(path);
    CHECK(!src.empty(), "read %s", path);
    auto has = [&](const char* text) { return src.find(text) != std::string::npos; };
    // The start: the mode off and alt off, auto the header's default (its
    // mode once the scale is read, below), and the switch read once.
    CHECK(has("uint32_t s_bloom = gpu::bloom::kOff;") && has("bool s_bloomAlt = false;") &&
              has("bool s_bloomAuto = gpu::bloom::kAutoByDefault;"), "the switch doesn't start from the header's default");
    CHECK(has("gpu::bloom::ParseMode(v, s_bloom, s_bloomAlt, s_bloomAuto)"), "NFSMW_BLOOM isn't parsed with the header's rule");
    // auto: the header's rule on the scene's scale, where the switch is read
    // (after the scale is) and after every change of the scale.
    CHECK(has("const video::SteamDevice device = video::ThisSteamDevice();\n            s_bloom = gpu::bloom::AutoMode(s_renderScale, device);"),
        "auto isn't the header's rule on the scene's scale and this machine");
    const size_t scaleRead = src.find("s_renderScale = WantedRenderScale(setting, regionW, regionH);");
    const size_t parsed = src.find("gpu::bloom::ParseMode(v, s_bloom, s_bloomAlt, s_bloomAuto)");
    CHECK(scaleRead != std::string::npos && parsed != std::string::npos && scaleRead < parsed &&
              src.find("BloomAuto();", parsed) < src.find("NFSMW_BLOOM_ALT_FRAMES", parsed),
        "auto isn't resolved once the scale and the switch are read");
    CHECK(has("            s_renderScale = scale;\n            BloomAuto();"), "a change of Internal Resolution leaves auto's mode as it was");
    // The filter's combines: a race frame's and the camera shots' unless told.
    CHECK(has("uint64_t s_bloomPs[8] = { gpu::bloom::kCombines[0], gpu::bloom::kCombines[1] };") &&
              has("uint32_t s_bloomPsCount = gpu::bloom::kCombineCount;"), "the filter's default combines aren't the header's two");
    CHECK(has("std::find(s_bloomPs, s_bloomPs + s_bloomPsCount, s_ps->hash) != s_bloomPs + s_bloomPsCount"),
        "the filter isn't applied by the list");
    // The scale: the chain's pitches at the scene's scale, made again with it.
    CHECK(has("if (ScaledPitch(pitch) || gpu::bloom::ScaledPitch(s_bloom, pitch, depth))\n                rt->scale = s_renderScale;"),
        "CreateTarget doesn't give the chain the scene's scale");
    CHECK(has("return ScaledPitch(target.pitch) || (target.depth && target.pitch == 1600) ||\n                gpu::bloom::ScaledPitch(s_bloom, target.pitch, target.depth);"),
        "ScaledTarget leaves the chain out (an Internal Resolution change wouldn't remake it)");
    // The first pass's scene read at 1x under scale.
    CHECK(has("gpu::bloom::SceneReadAt1x(s_bloom, s_drawIntoBloomChain, width)"), "the chain's first pass reads the scene at host resolution");
    // The filter: a variant of the combine, its slots by the header's rule.
    CHECK(has("gpu::bloom::FilterSlot(n, (d2 & 0x1FFF) + 1,"), "the filter's slots aren't the header's");
    CHECK(has("gpu::bloom::RewriteFetches(text, bloomMask, box ? \"sbBloomBox\" : \"sbBloomTex\")"), "the variant isn't the rewritten translation");
    // The fill: a variant of the first pass alone, its slot by the header's rule.
    CHECK(has("(s_bloom & gpu::bloom::kFill) && !(key.pad & 0x1F) && color[0] && s_ps->hash == gpu::bloom::kDownsamplePs"),
        "the fill isn't the first pass's alone");
    CHECK(has("gpu::bloom::FillSlot(n, (d2 & 0x1FFF) + 1,"), "the fill's slot isn't the header's");
    CHECK(has("const bool box = ps.hash == gpu::bloom::kDownsamplePs;"), "the first pass doesn't take the box");
    // alt: switched with the [perf] window unless told, never a default.
    CHECK(has("uint32_t s_bloomAltWindow = gpu::bloom::kAltWindow;"), "alt's window isn't the [perf] line's by default");
    CHECK(has("if (s_vk && s_bloomAlt && frame % s_bloomAltWindow == 0)\n            BloomAlt(frame);"),
        "alt doesn't switch with its window");
}

int main(int argc, char** argv)
{
    TestParse();
    TestAuto();
    TestCombines();
    TestAlt();
    TestPitches();
    TestShaderList();
    TestFilterSlot();
    TestFillSlot();
    TestFillCover();
    TestRewrite();
    TestBSpline();
    TestCost();
    if (argc > 1)
        TestRenderer(argv[1]);
    if (g_failures)
    {
        printf("bloom_test: %d failures\n", g_failures);
        return 1;
    }
    printf("bloom_test: all passed\n");
    return 0;
}
