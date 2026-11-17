// Tests for gpu::cube_aa (runtime/gpu/cube_aa.h): NFSMW_CUBE_AA's names,
// which target is the car's environment cube's, the size of its surface as
// the console lays it out in EDRAM, the occlusion weight with more than one
// host sample a pixel, the pixels each mode shades, and what the resolve's
// average of a 2x2 block gives an edge (issue #89: the rear-view mirror's
// stair steps); renderer.cpp is read to check it applies the rules.
// From the repo root:
//   clang++ -std=c++20 -O2 -Iruntime tests/cube_aa_test.cpp -o build/cube_aa_test
//   build/cube_aa_test runtime/gpu/renderer.cpp
#include <gpu/cube_aa.h>
#include <gpu/render_scale.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

using namespace gpu::cube_aa;

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void TestParse()
{
    struct Case { const char* text; bool ok; Mode mode; };
    for (const Case& c : { Case{ nullptr, true, Mode::Off }, Case{ "", true, Mode::Off }, Case{ "0", true, Mode::Off },
             Case{ "off", true, Mode::Off }, Case{ "ss", true, Mode::Supersample }, Case{ "msaa", true, Mode::Msaa } })
    {
        // (From every earlier value: the result doesn't depend on it.)
        for (Mode before : { Mode::Off, Mode::Supersample, Mode::Msaa })
        {
            Mode mode = before;
            CHECK(ParseMode(c.text, mode) == c.ok && mode == c.mode, "'%s' gives %s", c.text ? c.text : "(unset)", ModeName(mode));
        }
    }
    // Anything else: refused, and the mode left as it was (the default: off).
    for (const char* text : { "1", "on", "2", "4", "MSAA", "ss ", "msaa4", "supersample", "2x" })
    {
        Mode mode = Mode::Off;
        CHECK(!ParseMode(text, mode) && mode == Mode::Off, "'%s' is refused", text);
        mode = Mode::Msaa;
        CHECK(!ParseMode(text, mode) && mode == Mode::Msaa, "'%s' leaves the mode", text);
    }
    for (Mode mode : { Mode::Off, Mode::Supersample, Mode::Msaa })
    {
        Mode back = Mode::Off;
        CHECK(ParseMode(ModeName(mode), back) && back == mode, "%s round-trips", ModeName(mode));
    }
}

static void TestApplies()
{
    // Off: no target, so every target is made as before.
    for (uint32_t pitch : { 32u, 256u, 280u, 320u, 640u, 1280u, 1600u })
        for (bool msaa : { false, true })
            CHECK(!Applies(Mode::Off, pitch, msaa), "off applies to pitch %u", pitch);
    for (Mode mode : { Mode::Supersample, Mode::Msaa })
    {
        CHECK(Applies(mode, 280, true), "%s: the cube's surface", ModeName(mode));
        // The same pitch without guest MSAA is another target.
        CHECK(!Applies(mode, 280, false), "%s: pitch 280 without MSAA", ModeName(mode));
        // The scene (1280), its half and quarter sizes, the bloom (320), the
        // sun's shadow map (1600), the reflection (256).
        for (uint32_t pitch : { 32u, 64u, 256u, 279u, 281u, 288u, 320u, 640u, 1280u, 1600u })
            CHECK(!Applies(mode, pitch, true), "%s applies to pitch %u", ModeName(mode), pitch);
    }
    CHECK(kPitch == 280 && kFace == 256 && kSamples == 4 && kFactor * kFactor == kSamples, "the cube's constants");
}

static void TestSurfaceRows()
{
    // The cube as the game lays it out: 280 pixels at 4x are 560 samples,
    // 7 tiles a row; depth at tile 1824 has 224 tiles, 32 rows of them, 512
    // sample rows: 256 pixel rows, exactly a face.
    CHECK(SurfaceRows(1824, 280, 2) == 256, "the cube's depth has %u rows", SurfaceRows(1824, 280, 2));
    // Its colour at tile 0: 2048 / 7 = 292 tile rows = 2336 pixel rows,
    // clamped to the renderer's 2048.
    CHECK(SurfaceRows(0, 280, 2) == 2048, "the cube's colour has %u rows", SurfaceRows(0, 280, 2));
    // The scene at 720p 4x (pitch 1280: 32 tiles a row): 64 tile rows at
    // tile 0, 512 pixel rows (the game draws it as three tiles of 256).
    CHECK(SurfaceRows(0, 1280, 2) == 512, "the scene at 4x has %u rows", SurfaceRows(0, 1280, 2));
    // Without MSAA a pixel is one sample each way; 2x doubles only the rows.
    CHECK(SurfaceRows(0, 1280, 0) == 2048, "1280 at 1x: %u", SurfaceRows(0, 1280, 0));
    CHECK(SurfaceRows(1024, 1280, 0) == 1024, "1280 at 1x from tile 1024: %u", SurfaceRows(1024, 1280, 0));
    CHECK(SurfaceRows(1024, 1280, 1) == 512, "1280 at 2x from tile 1024: %u", SurfaceRows(1024, 1280, 1));
    CHECK(SurfaceRows(1824, 280, 0) == 896, "280 at 1x from tile 1824: %u", SurfaceRows(1824, 280, 0));
    CHECK(SurfaceRows(1824, 280, 1) == 448, "280 at 2x from tile 1824: %u", SurfaceRows(1824, 280, 1));
    // 64 bits a pixel: two tiles' worth a sample.
    CHECK(SurfaceRows(1824, 280, 2, true) == 128, "280 at 4x, 64bpp: %u", SurfaceRows(1824, 280, 2, true));
    // Never none, never past the renderer's 2048.
    CHECK(SurfaceRows(2047, 1280, 2) == 16 && SurfaceRows(5000, 280, 2) == 16, "a surface at the end of EDRAM");
    CHECK(SurfaceRows(0, 16, 0) == 2048, "a narrow surface");
    for (uint32_t base = 0; base < 2048; base += 37)
        for (uint32_t pitch : { 80u, 256u, 280u, 320u, 1280u })
            for (uint32_t msaa = 0; msaa < 3; msaa++)
            {
                const uint32_t rows = SurfaceRows(base, pitch, msaa);
                CHECK(rows >= 16 && rows <= 2048, "base %u pitch %u msaa %u: %u rows", base, pitch, msaa, rows);
                // More samples a pixel never leave more rows.
                CHECK(msaa == 0 || rows <= SurfaceRows(base, pitch, msaa - 1), "base %u pitch %u: rows grow with MSAA", base, pitch);
                // What the rows hold fits the tiles left (but for the floor of 16).
                const uint32_t tilesPerRow = (pitch * (msaa == 2 ? 2 : 1) + 79) / 80;
                CHECK(rows == 16 || (rows * (msaa ? 2 : 1) + 15) / 16 * tilesPerRow <= 2048 - base,
                    "base %u pitch %u msaa %u: %u rows overrun EDRAM", base, pitch, msaa, rows);
            }
}

static void TestTargetSize()
{
    // The renderer's own row count takes the pitch for samples (896 rows at
    // base 1824, 2048 at 0); the mode sizes the target as the surface.
    const Size depth = TargetSize(1824, 280, 2, false, 896);
    CHECK(depth.width == 280 && depth.height == 256, "the cube's depth target is %ux%u", depth.width, depth.height);
    const Size color = TargetSize(0, 280, 2, false, 2048);
    CHECK(color.width == 280 && color.height == 2048, "the cube's colour target is %ux%u", color.width, color.height);
    // Never taller than the renderer's count.
    CHECK(TargetSize(0, 280, 2, false, 100).height == 100, "the renderer's rows bound it");
    // A face fits both, and a pass on both (their common size) is 280x256:
    // a face and nothing of a neighbour.
    CHECK(kFace <= depth.width && kFace <= depth.height && kFace <= color.width && kFace <= color.height, "a face fits");
    CHECK(std::min(depth.height, color.height) == kFace, "the pass is a face tall");
    // Host images with each mode against the 1280-wide ones from before
    // (colour 1280x2048, depth 1280x896, one sample): no more samples.
    const gpu::AxisScale two{ kFactor, 1 };
    CHECK(two.Size(depth.width) == 560 && two.Size(depth.height) == 512 && two.Size(color.height) == 4096, "the supersampled sizes");
    const uint64_t before = 1280ull * 2048 + 1280ull * 896;
    const uint64_t ss = uint64_t(two.Size(color.width)) * two.Size(color.height) + uint64_t(two.Size(depth.width)) * two.Size(depth.height);
    const uint64_t msaa = (uint64_t(color.width) * color.height + uint64_t(depth.width) * depth.height) * (1 + kSamples);
    CHECK(ss < before && msaa <= before + before / 16, "samples held: %llu before, %llu ss, %llu msaa", (unsigned long long)before,
        (unsigned long long)ss, (unsigned long long)msaa);
    // A face's host rectangle at the cube's factor: whole pixels, inside.
    CHECK(two.EdgeAt(0) == 0 && two.EdgeAt(kFace) == 512 && two.Exact(kFace) && two.Exact(0), "a face at 2x is 512 host pixels");
}

static void TestOcclusionWeight()
{
    // One host sample a pixel: the pair the renderer passed before, as it
    // was (unreduced), at every MSAA and scale.
    struct Scale { uint32_t num, den; };
    for (uint32_t msaa = 0; msaa < 3; msaa++)
        for (const Scale& s : { Scale{ 1, 1 }, Scale{ 4, 1 }, Scale{ 9, 4 }, Scale{ 9, 1 }, Scale{ 10, 9 }, Scale{ 16, 1 } })
        {
            const Weight w = OcclusionWeight(msaa, 1, s.num, s.den);
            CHECK(w.num == (1u << msaa) * s.den && w.den == s.num, "msaa %u at %u/%u: %u/%u", msaa, s.num, s.den, w.num, w.den);
        }
    // The cube with four host samples a pixel under the game's 4x: a sample
    // counted is a sample the game expects.
    Weight w = OcclusionWeight(2, 4, 1, 1);
    CHECK(w.num == 1 && w.den == 1, "4x over 4 samples: %u/%u", w.num, w.den);
    // And were its surface scaled too (NFSMW_SCALE_PITCH): 2x2 pixels of
    // four samples each for a guest pixel of four.
    w = OcclusionWeight(2, 4, 4, 1);
    CHECK(w.num == 1 && w.den == 4, "4x over 4 samples at 2x: %u/%u", w.num, w.den);
    w = OcclusionWeight(2, 4, 9, 4);
    CHECK(w.num == 4 && w.den == 9, "4x over 4 samples at 1.5x: %u/%u", w.num, w.den);
    w = OcclusionWeight(1, 4, 1, 1);
    CHECK(w.num == 1 && w.den == 2, "2x over 4 samples: %u/%u", w.num, w.den);
    // The supersampled cube (one sample a host pixel, 2x2 of them): the
    // scaled scene's weight, which also comes to a guest sample each.
    w = OcclusionWeight(2, HostSamples(Mode::Supersample), kFactor * kFactor, 1);
    CHECK(w.num == 4 && w.den == 4, "4x over 2x2 pixels: %u/%u", w.num, w.den);
    // Whatever the mode, a covered guest pixel counts the game's samples.
    for (Mode mode : { Mode::Off, Mode::Supersample, Mode::Msaa })
    {
        const uint32_t pixels = mode == Mode::Supersample ? kFactor * kFactor : 1;
        const Weight m = OcclusionWeight(2, HostSamples(mode), pixels, 1);
        const uint32_t counted = pixels * HostSamples(mode);
        CHECK(counted * m.num / m.den == 4, "%s: a pixel counts %u", ModeName(mode), counted * m.num / m.den);
    }
    CHECK(HostSamples(Mode::Off) == 1 && HostSamples(Mode::Supersample) == 1 && HostSamples(Mode::Msaa) == 4, "host samples");
}

static void TestCost()
{
    // Six 256x256 faces a frame.
    CHECK(ShadedPixels(Mode::Off) == 393216 && StoredSamples(Mode::Off) == 393216, "off: %llu", (unsigned long long)ShadedPixels(Mode::Off));
    CHECK(ShadedPixels(Mode::Supersample) == 1572864 && StoredSamples(Mode::Supersample) == 1572864, "ss: %llu",
        (unsigned long long)ShadedPixels(Mode::Supersample));
    CHECK(ShadedPixels(Mode::Msaa) == 393216 && StoredSamples(Mode::Msaa) == 1572864, "msaa: %llu shaded, %llu stored",
        (unsigned long long)ShadedPixels(Mode::Msaa), (unsigned long long)StoredSamples(Mode::Msaa));
    CHECK(ShadedPixels(Mode::Supersample) - ShadedPixels(Mode::Off) == 1179648, "ss shades 1,179,648 more");
    CHECK(ShadedPixels(Mode::Supersample, 1) == 512 * 512 && ShadedPixels(Mode::Msaa, 1) == 256 * 256, "one face");
    // Against a 1280x720 frame at 1x (921,600 pixels): 1.28 frames' worth more.
    CHECK((ShadedPixels(Mode::Supersample) - ShadedPixels(Mode::Off)) * 100 / (1280 * 720) == 128, "ss against a 720p frame");
}

// What the resolve's average gives: a face supersampled 2x2 whose resolve
// averages each guest pixel's block (the resolve shader's px() at a whole
// scale), against one sample at the pixel's centre. A straight edge through
// the face: one sample gives 0 or 1 only (the stair steps of issue #89), the
// 2x2 grid levels between (of 0, 1/4 .. 1), each no further from the edge's
// true coverage than one sample was, or within a quarter of it; pixels the edge misses are the same either way.
static void TestBoxAverage()
{
    // Inside where a*x + b*y < c (pixel units; several slopes).
    struct Edge { double a, b, c; };
    for (const Edge& e : { Edge{ 1.0, 0.31, 9.7 }, Edge{ 0.17, 1.0, 6.2 }, Edge{ 1.0, 1.0, 15.9 }, Edge{ 1.0, -0.6, 3.3 } })
    {
        auto inside = [&](double x, double y) { return e.a * x + e.b * y < e.c ? 1 : 0; };
        std::set<int> levels;
        int changed = 0, intermediate = 0;
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++)
            {
                const int one = inside(x + 0.5, y + 0.5);
                int four = 0;
                for (uint32_t j = 0; j < kFactor; j++)
                    for (uint32_t i = 0; i < kFactor; i++)
                        four += inside(x + (i + 0.5) / kFactor, y + (j + 0.5) / kFactor);
                levels.insert(four);
                // Corners all on one side: the edge misses the pixel, and
                // both give the same.
                const int corners = inside(x, y) + inside(x + 1, y) + inside(x, y + 1) + inside(x + 1, y + 1);
                if (corners == 0 || corners == 4)
                    CHECK(four == one * 4, "a pixel the edge misses changed at %d,%d (%d of 4, was %d)", x, y, four, one);
                // The true coverage, by 32x32 samples.
                int fine = 0;
                for (int j = 0; j < 32; j++)
                    for (int i = 0; i < 32; i++)
                        fine += inside(x + (i + 0.5) / 32, y + (j + 0.5) / 32);
                const double truth = fine / 1024.0;
                CHECK(std::abs(four / 4.0 - truth) <= std::abs(one - truth) + 1e-9 || std::abs(four / 4.0 - truth) <= 0.25,
                    "%d,%d: the average (%d/4) is further from the coverage (%.3f) than one sample (%d)", x, y, four, truth, one);
                changed += four != one * 4;
                intermediate += four > 0 && four < 4;
            }
        CHECK(intermediate > 0 && changed == intermediate, "an edge gives %d in-between pixels (%d changed)", intermediate, changed);
        // (An ordered grid: a diagonal passes through fewer of its levels.)
        CHECK(levels.size() >= 3 && levels.count(0) && levels.count(4), "%zu levels along the edge", levels.size());
    }
}

// renderer.cpp applies the rules where its targets, passes, pipelines,
// draws and resolves are made.
static void TestRenderer(const char* path)
{
    std::ifstream f(path);
    CHECK(f.good(), "can't read %s", path);
    if (!f.good())
        return;
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();
    auto has = [&](const char* what) { return text.find(what) != std::string::npos; };
    // The switch, ss unless set (operator 2026-11-13), and its names.
    CHECK(has("gpu::cube_aa::Mode s_cubeAA = gpu::cube_aa::Mode::Supersample;"), "the default isn't ss");
    // The default takes the same checks as a set value: the untiled
    // fallback and the start line run whether NFSMW_CUBE_AA is set or not.
    CHECK(has("            }\n            // (Set or not: the default is ss.)\n            {\n                if (s_cubeAA == gpu::cube_aa::Mode::Msaa || s_cubeAAAlt)"),
        "the checks only run when NFSMW_CUBE_AA is set");
    CHECK(has("std::getenv(\"NFSMW_CUBE_AA\")") && has("gpu::cube_aa::ParseMode(v, s_cubeAA)"), "NFSMW_CUBE_AA isn't read");
    // Which target, its size, its factor.
    CHECK(has("rt->cube = gpu::cube_aa::Applies(s_cubeAA, pitch, wide);"), "CreateTarget doesn't use Applies");
    CHECK(has("gpu::cube_aa::TargetSize(base, pitch, msaaLog2, !depth && Is64bpp(format), rows)"), "CreateTarget doesn't use TargetSize");
    CHECK(has("rt->scale.x = rt->scale.y = gpu::AxisScale{ gpu::cube_aa::kFactor, 1 };"), "the supersample factor isn't the cube's");
    CHECK(has("VK_SAMPLE_COUNT_4_BIT);") && has("if (rt.msImage.image)"), "the 4-sample image isn't made and freed");
    CHECK(has("m.targets += rt->image.bytes + rt->msImage.bytes;") && has("own(rt->msImage);"),
        "the [perf] line's target memory leaves the 4-sample image out");
    // A pass and its draws agree on the samples; the colour is resolved.
    CHECK(has("const bool multisampled = PassSamples(color, depth) > 1;") && has("const uint32_t hostSamples = PassSamples(color, depth);"),
        "BeginPass and Draw don't share PassSamples");
    CHECK(has("colors[i].resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;") && has("colors[i].resolveImageView = color[i]->image.view;"),
        "the pass doesn't resolve the 4-sample colour");
    CHECK(has("ms.rasterizationSamples = (key.pad >> 5) & 1 ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_1_BIT;"),
        "the pipeline's samples don't follow key.pad bit 5");
    CHECK(has("const uint32_t samplesBit = hostSamples > 1 ? 1u << 5 : 0u;") && has("key.pad = samplesBit;"),
        "a pending variant's fallback loses the samples bit");
    // The occlusion count, the textures a cube draw samples, the resolve.
    CHECK(has("gpu::cube_aa::OcclusionWeight(uint32_t(surface.msaa_samples), hostSamples, scale.PixelsNum(), scale.PixelsDen())"),
        "Draw doesn't use OcclusionWeight");
    CHECK(has("if (rt && !rt->cube && !rt->scale.IsOne())"), "a supersampled cube draw would sample scaled textures");
    CHECK(has("!directShadowResolve && !source->cube && !source->scale.IsOne() && source->scale == s_renderScale"),
        "the cube's resolve would keep scaled memory");
    CHECK(has("if (s_cubeAA == gpu::cube_aa::Mode::Supersample || s_cubeAAAlt)\n            {\n                // (At Internal Resolution 1x"),
        "the cube's resolve pipeline isn't made at start");
    // NFSMW_CUBE_AA=alt (a test aid): switched with the [perf] window, the
    // targets made again; never a default.
    CHECK(has("bool s_cubeAAAlt = false;") && has("if (s_vk && s_cubeAAAlt && frame % 120 == 0)\n            CubeAAAlt(frame);"),
        "alt isn't off by default, or doesn't switch with the [perf] window");
    CHECK(has("if (!(was.wide && was.pitch == gpu::cube_aa::kPitch))\n                        continue;"),
        "alt replaces targets that aren't the cube's");
}

int main(int argc, char** argv)
{
    TestParse();
    TestApplies();
    TestSurfaceRows();
    TestTargetSize();
    TestOcclusionWeight();
    TestCost();
    TestBoxAverage();
    if (argc > 1)
        TestRenderer(argv[1]);
    if (g_failures)
    {
        printf("cube_aa_test: %d failures\n", g_failures);
        return 1;
    }
    printf("cube_aa_test: all passed\n");
    return 0;
}
