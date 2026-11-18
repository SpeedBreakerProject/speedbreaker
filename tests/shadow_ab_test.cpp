// SpeedBreaker tests. GPL-3.0-or-later (see COPYING).
//
// The sun shadow map's diagnostics (gpu/shadow_ab.h, GitHub #90):
//   - NFSMW_SHADOW_SCALE's and map='s scales: a scale per axis ("2x1",
//     "9/4x1", "3x1"), the map's host size and its texels against the 1x
//     map's; malformed ones refused;
//   - NFSMW_SHADOW_AB's plan: variants of Shadow Edges, the fit and the
//     map's scale, with a '.' inside a number kept ("1.5x", "map=1.5x1");
//   - gpu/renderer.cpp taking a forced scale before Shadow Resolution's and
//     leaving the setting's own line as it was, and game/ultrawide.cpp
//     taking the variant's fit.
// Arguments: runtime/gpu/renderer.cpp, runtime/game/ultrawide.cpp.
//
// Build by hand: c++ -std=c++20 -O2 -UNDEBUG -I runtime tests/shadow_ab_test.cpp -o build/shadow_ab_test
#include <gpu/shadow_ab.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

static int s_failures = 0;
#define CHECK(cond, ...)                                                    \
    do                                                                      \
    {                                                                       \
        if (!(cond))                                                        \
        {                                                                   \
            s_failures++;                                                   \
            fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
            fprintf(stderr, __VA_ARGS__);                                   \
            fprintf(stderr, "\n");                                          \
        }                                                                   \
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
    namespace ab = gpu::shadow_ab;
    using gpu::AxisScale;
    using gpu::RenderScale;

    // A scale per axis.
    struct Case { const char* text; bool ok; RenderScale want; uint32_t w, h; double texels; };
    const Case cases[] = {
        { "1", true, RenderScale::Whole(1, 1), 1600, 1600, 1.0 },
        { "2", true, RenderScale::Whole(2, 2), 3200, 3200, 4.0 },
        { "1.5", true, RenderScale{ { 3, 2 }, { 3, 2 } }, 2400, 2400, 2.25 },
        { "2x1", true, RenderScale::Whole(2, 1), 3200, 1600, 2.0 },
        { "9/4x1", true, RenderScale{ { 9, 4 }, { 1, 1 } }, 3600, 1600, 2.25 },
        { "3x1", true, RenderScale::Whole(3, 1), 4800, 1600, 3.0 },
        { "1.5x1", true, RenderScale{ { 3, 2 }, { 1, 1 } }, 2400, 1600, 1.5 },
        { "3/2x1", true, RenderScale{ { 3, 2 }, { 1, 1 } }, 2400, 1600, 1.5 },
        { "1x2", true, RenderScale::Whole(1, 2), 1600, 3200, 2.0 },
        { "4x1", true, RenderScale::Whole(4, 1), 6400, 1600, 4.0 },
        { "2.25x1", false, {}, 0, 0, 0.0 },  // a ratio is spelt as one: 9/4
        { "5/1x1", false, {}, 0, 0, 0.0 },   // above 4
        { "1/2x1", false, {}, 0, 0, 0.0 },   // below 1
        { "2x", false, {}, 0, 0, 0.0 },
        { "x1", false, {}, 0, 0, 0.0 },
        { "2x1x1", false, {}, 0, 0, 0.0 },
        { "", false, {}, 0, 0, 0.0 },
        { "wide", false, {}, 0, 0, 0.0 },
    };
    for (const Case& c : cases)
    {
        RenderScale got = RenderScale::Whole(4, 4);
        const bool ok = ab::ParseMapScale(c.text, got);
        CHECK(ok == c.ok, "\"%s\": %s", c.text, ok ? "accepted" : "refused");
        if (!ok)
        {
            CHECK(got == RenderScale::Whole(4, 4), "\"%s\" refused but the scale changed", c.text);
            continue;
        }
        if (!c.ok)
            continue;
        CHECK(got == c.want, "\"%s\": %s, want %s", c.text, gpu::ScaleName(got).c_str(), gpu::ScaleName(c.want).c_str());
        CHECK(ab::MapWidth(got) == c.w && ab::MapHeight(got) == c.h, "\"%s\": a %ux%u map, want %ux%u", c.text, ab::MapWidth(got),
            ab::MapHeight(got), c.w, c.h);
        CHECK(std::fabs(ab::TexelRatio(got) - c.texels) < 1e-9, "\"%s\": %f of the 1x map's texels, want %f", c.text, ab::TexelRatio(got), c.texels);
    }
    // 9/4 across is the same texels as Shadow Resolution's 1.5x, 2x1 half of 2x's.
    RenderScale wide, half, twice, two;
    CHECK(ab::ParseMapScale("9/4x1", wide) && ab::ParseMapScale("1.5", half) && ab::ParseMapScale("2x1", twice) && ab::ParseMapScale("2", two), "parse");
    CHECK(ab::TexelRatio(wide) == ab::TexelRatio(half) && ab::TexelRatio(twice) * 2.0 == ab::TexelRatio(two), "texel counts");
    CHECK(half == gpu::ShadowScale(3) && two == gpu::ShadowScale(4), "the square ones are Shadow Resolution's");
    // A whole host texel for every guest one on a texel axis (9/4), every edge owned.
    CHECK(wide.x.Texel() && wide.x.Edge(0) == 0 && wide.x.Edge(1600) == 3600 && wide.x.Edge(4) == 9 && wide.Exact(1600, 1600), "9/4: a texel axis");

    // The plan.
    ab::Plan p;
    CHECK(ab::ParsePlan("3@150:1x,map=2x1,map=9/4x1,2x,1x.fit=game,map=3x1.classic", p), "parse");
    CHECK(p.period == 3.0 && p.from == 150.0 && p.variants.size() == 6, "%f %f %zu", p.period, p.from, p.variants.size());
    if (p.variants.size() == 6)
    {
        CHECK(p.variants[0].hasScale && p.variants[0].scale == RenderScale::Whole(1, 1) && p.variants[0].fit == -1 && p.variants[0].smooth == -1, "1x");
        CHECK(p.variants[1].hasScale && p.variants[1].scale == RenderScale::Whole(2, 1), "map=2x1");
        CHECK(p.variants[2].hasScale && (p.variants[2].scale == RenderScale{ { 9, 4 }, { 1, 1 } }), "map=9/4x1");
        CHECK(p.variants[3].hasScale && p.variants[3].scale == RenderScale::Whole(2, 2), "2x");
        CHECK(p.variants[4].scale == RenderScale::Whole(1, 1) && p.variants[4].fit == ab::FitGame, "1x.fit=game");
        CHECK(p.variants[5].scale == RenderScale::Whole(3, 1) && p.variants[5].smooth == 0, "map=3x1.classic");
        CHECK(p.variants[5].name == "map=3x1.classic", "%s", p.variants[5].name.c_str());
    }
    CHECK(ab::ParsePlan("2:smooth,classic,pcf,nopcf,fit=viewh,fit=view,fit=aspect", p) && p.variants.size() == 7, "options");
    if (p.variants.size() == 7)
    {
        CHECK(p.variants[0].smooth == 1 && p.variants[1].smooth == 0 && p.variants[2].smooth == 1 && p.variants[3].smooth == 0, "Shadow Edges");
        CHECK(!p.variants[0].hasScale && p.variants[0].fit == -1, "unset: the run's");
        CHECK(p.variants[4].fit == ab::FitViewH && p.variants[5].fit == ab::FitView && p.variants[6].fit == ab::FitAspect, "fits");
        CHECK(p.from == 0.0, "no @: from the start");
    }
    // A '.' inside a number isn't a separator.
    CHECK(ab::ParsePlan("2:1.5x", p) && (p.variants[0].scale == RenderScale{ { 3, 2 }, { 3, 2 } }), "1.5x alone");
    CHECK(ab::ParsePlan("2:classic.1.5x.fit=game", p) && p.variants[0].smooth == 0 && p.variants[0].fit == ab::FitGame &&
        (p.variants[0].scale == RenderScale{ { 3, 2 }, { 3, 2 } }), "classic.1.5x.fit=game");
    CHECK(ab::ParsePlan("2:map=1.5x1.smooth", p) && p.variants[0].smooth == 1 && (p.variants[0].scale == RenderScale{ { 3, 2 }, { 1, 1 } }),
        "map=1.5x1.smooth");
    CHECK(ab::ParsePlan("2:fit=view.map=1x1.5", p) && (p.variants[0].scale == RenderScale{ { 1, 1 }, { 3, 2 } }), "map=1x1.5 last");
    CHECK(!ab::ParsePlan("3:smooth,bogus", p), "an unknown option");
    CHECK(!ab::ParsePlan("3:map=2.25x1", p) && !ab::ParsePlan("3:map=", p) && !ab::ParsePlan("3:map=5/1x1", p), "a scale the renderer doesn't take");
    // (A whole number above 4 is 4, as NFSMW_RENDER_SCALE's: a 6400-texel axis at most.)
    CHECK(ab::ParsePlan("3:map=5x1", p) && p.variants[0].scale == RenderScale::Whole(4, 1), "map=5x1 is 4x1");
    CHECK(!ab::ParsePlan("0:smooth", p) && !ab::ParsePlan("smooth", p) && !ab::ParsePlan("", p) && !ab::ParsePlan(nullptr, p), "malformed");
    CHECK(!ab::ParsePlan("3:", p), "no variants");

    if (argc > 1)
    {
        const std::string r = ReadText(argv[1]);
        CHECK(r.find("if (const gpu::RenderScale* forced = ForcedShadowScale())\n                return *forced;\n            return setting == "
                     "settings::kShadowResolutionAuto ? gpu::AutoShadowScale(") != std::string::npos,
            "a forced scale first, then Shadow Resolution's");
        CHECK(r.find("return gpu::shadow_ab::EnvScale();") != std::string::npos, "NFSMW_SHADOW_SCALE when no variant has a scale");
        CHECK(r.find("rt->scale = s_shadowScale;") != std::string::npos, "the map's target takes the scale, an axis each");
        // A scaled map is sampled straight from its depth target, and every draw
        // receiving the shadow looks it up: its pages' sequence is read once per
        // write epoch, the epoch before the sequence (GitHub #90, the Deck's city).
        {
            const size_t epoch = r.find("const uint64_t epoch = writewatch::WriteEpoch();\n                    if (resolved->sampledEpoch != epoch || resolved->sampledSize != size)");
            const size_t walk = r.find("resolved->sampledSequence = writewatch::WriteSequence(base, size);");
            CHECK(epoch != std::string::npos && walk != std::string::npos && epoch < walk, "the sampled map's sequence is kept per write epoch");
            CHECK(r.find("sampleSequence = writewatch::WriteSequence(base, size);") == std::string::npos, "no walk of the map's pages per draw");
            CHECK(r.find("sampleSequence == it->second.writeSequence") != std::string::npos, "still compared with the resolve's sequence");
        }
        CHECK(r.find("ShadowAbOnSwap();\n        if (s_vk)\n            ApplyLiveSettings();") != std::string::npos,
            "the phase's variant chosen before the live settings apply it");
        CHECK(r.find("if (ab >= 0)\n            {\n                s_shadowEdgesSmooth = ab > 0;") != std::string::npos, "a variant's Shadow Edges");
    }
    if (argc > 2)
    {
        const std::string u = ReadText(argv[2]);
        CHECK(u.find("if (const int ab = gpu::shadow_ab::g_fit.load(std::memory_order_relaxed); ab >= 0)\n        fit = ShadowFit(ab);") != std::string::npos,
            "the fit hook takes the variant's fit");
        CHECK(u.find("ShadowFit mode = ShadowFit::View;") != std::string::npos, "the default fit is the view's, as before");
    }
    if (s_failures)
    {
        printf("%d check(s) FAILED\n", s_failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
