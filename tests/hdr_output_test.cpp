// SpeedBreaker tests. GPL-3.0-or-later (see COPYING).
//
// HDR Output's highlight curve and transfer functions (video/hdr_output.h,
// header only): SDR white maps to the display's headroom, mid-tones stay
// close to the SDR picture, and the sRGB transfer round-trips.
//
// Build by hand: c++ -std=c++20 -O2 -UNDEBUG -I runtime tests/hdr_output_test.cpp
#include <video/hdr_output.h>

#include <cmath>
#include <cstdio>
#include <initializer_list>

namespace
{
    int s_failures = 0;

#define CHECK(cond, ...)                                              \
    do                                                                \
    {                                                                 \
        if (!(cond))                                                  \
        {                                                             \
            s_failures++;                                             \
            fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
            fprintf(stderr, __VA_ARGS__);                             \
            fprintf(stderr, "\n");                                    \
        }                                                             \
    } while (0)

    bool Near(float a, float b, float tolerance = 1e-6f)
    {
        return std::abs(a - b) <= tolerance;
    }
}

int main()
{
    using namespace video::hdr;

    // The headroom is the display's, within a safe range.
    CHECK(PeakForHeadroom(0.5f) == 1.0f, "%.6f", PeakForHeadroom(0.5f));
    CHECK(PeakForHeadroom(1.0f) == 1.0f, "%.6f", PeakForHeadroom(1.0f));
    CHECK(PeakForHeadroom(2.0f) == 2.0f, "%.6f", PeakForHeadroom(2.0f));
    CHECK(PeakForHeadroom(16.0f) == 4.0f, "%.6f", PeakForHeadroom(16.0f));

    // No headroom: the SDR picture exactly.
    for (float v : { 0.0f, 0.18f, 0.5f, 1.0f })
        CHECK(Expand(v, 1.0f) == v, "%.6f -> %.6f", v, Expand(v, 1.0f));

    // SDR white reaches the peak; black stays black; the known points.
    CHECK(Expand(0.0f, 2.0f) == 0.0f, "%.6f", Expand(0.0f, 2.0f));
    CHECK(Expand(1.0f, 2.0f) == 2.0f, "%.6f", Expand(1.0f, 2.0f));
    CHECK(Near(Expand(0.5f, 2.0f), 0.50390625f, 1e-6f), "%.9f", Expand(0.5f, 2.0f));
    CHECK(Near(Expand(0.9f, 4.0f), 2.19140163f, 1e-5f), "%.9f", Expand(0.9f, 4.0f));

    // Expanding by luma keeps the channels' ratios (hue); gray stays gray.
    {
        float r = 1.0f, g = 0.0f, b = 0.0f;
        ExpandRgb(r, g, b, 4.0f);
        CHECK(g == 0.0f && b == 0.0f && r > 1.0f && r < 4.0f, "%.6f %.6f %.6f", r, g, b);
        float gr = 0.5f, gg = 0.5f, gb = 0.5f;
        ExpandRgb(gr, gg, gb, 4.0f);
        CHECK(Near(gr, gg, 1e-6f) && Near(gr, gb, 1e-6f) && Near(gr, Expand(0.5f, 4.0f), 1e-6f),
            "%.6f %.6f %.6f", gr, gg, gb);
    }

    // Rec.709 <-> Rec.2020, BT.2087.
    {
        float r = 1.0f, g = 0.0f, b = 0.0f;
        Rec709ToRec2020(r, g, b);
        CHECK(Near(r, 0.6274040f, 1e-5f) && Near(g, 0.0690970f, 1e-5f) && Near(b, 0.0163916f, 1e-5f),
            "%.7f %.7f %.7f", r, g, b);
        Rec2020ToRec709(r, g, b);
        CHECK(Near(r, 1.0f, 1e-4f) && Near(g, 0.0f, 1e-4f) && Near(b, 0.0f, 1e-4f), "%.7f %.7f %.7f", r, g, b);
    }

    // Out-of-range inputs are clamped, not wrapped.
    CHECK(Expand(-1.0f, 2.0f) == 0.0f, "%.6f", Expand(-1.0f, 2.0f));
    CHECK(Expand(2.0f, 2.0f) == 2.0f, "%.6f", Expand(2.0f, 2.0f));

    // Monotonic: a brighter SDR value is never a darker HDR value.
    for (float peak : { 2.0f, 4.0f })
    {
        float previous = Expand(0.0f, peak);
        for (int i = 1; i <= 1000; i++)
        {
            const float v = float(i) / 1000.0f;
            const float out = Expand(v, peak);
            CHECK(out >= previous, "peak %.1f: %.3f -> %.6f after %.6f", peak, v, out, previous);
            previous = out;
        }
    }

    // IEC 61966-2-1.
    CHECK(Near(SrgbToLinear(0.0f), 0.0f), "%.9f", SrgbToLinear(0.0f));
    CHECK(Near(SrgbToLinear(0.5f), 0.214041f, 1e-6f), "%.9f", SrgbToLinear(0.5f));
    CHECK(Near(SrgbToLinear(1.0f), 1.0f), "%.9f", SrgbToLinear(1.0f));
    CHECK(Near(LinearToSrgb(0.0f), 0.0f), "%.9f", LinearToSrgb(0.0f));
    CHECK(Near(LinearToSrgb(1.0f), 1.0f), "%.9f", LinearToSrgb(1.0f));
    for (int i = 0; i <= 1000; i++)
    {
        const float v = float(i) / 1000.0f;
        CHECK(Near(LinearToSrgb(SrgbToLinear(v)), v, 1e-6f), "%.6f -> %.9f", v, LinearToSrgb(SrgbToLinear(v)));
    }

    // PQ: HDR10's absolute scale, and the frame-average limiter.
    CHECK(kSdrWhiteNits == 100.0f && kHdrStrengthDefault == 2.0f && kHdrStrengthMax == 4.0f,
        "%.1f %.1f %.1f", kSdrWhiteNits, kHdrStrengthDefault, kHdrStrengthMax);

    // HDR Paper White: Auto follows the system; a set value is that value.
    CHECK(PaperWhiteNits(0, true, false) == 100.0f, "%.1f", PaperWhiteNits(0, true, false));
    CHECK(PaperWhiteNits(0, false, true) == 400.0f, "%.1f", PaperWhiteNits(0, false, true));
    CHECK(PaperWhiteNits(0, false, false) == 200.0f, "%.1f", PaperWhiteNits(0, false, false));
    CHECK(PaperWhiteNits(300, false, true) == 300.0f, "%.1f", PaperWhiteNits(300, false, true));
    CHECK(PaperWhiteNits(100, false, false) == 100.0f, "%.1f", PaperWhiteNits(100, false, false));
    CHECK(PaperWhiteNits(999, true, false) == 100.0f, "%.1f", PaperWhiteNits(999, true, false));
    CHECK(Near(FramePeak(4.0f, 0.0f), 4.0f), "%.6f", FramePeak(4.0f, 0.0f));
    CHECK(Near(FramePeak(4.0f, 0.25f), 4.0f), "%.6f", FramePeak(4.0f, 0.25f));
    CHECK(Near(FramePeak(4.0f, 0.5f), 2.0f), "%.6f", FramePeak(4.0f, 0.5f));
    CHECK(Near(FramePeak(2.0f, 0.5f), 2.0f), "%.6f", FramePeak(2.0f, 0.5f));
    CHECK(Near(FramePeak(4.0f, 1.0f), 1.0f), "%.6f", FramePeak(4.0f, 1.0f));
    CHECK(Near(FramePeak(1.0f, 0.5f), 1.0f), "%.6f", FramePeak(1.0f, 0.5f));
    // A near-white frame stays near SDR rather than flashing.
    CHECK(FramePeak(4.0f, 0.9f) < 1.12f, "%.6f", FramePeak(4.0f, 0.9f));

    // The limiter's smoothing: up at once, down by kAverageRelease a frame.
    CHECK(Near(SmoothAverage(0.2f, 0.97f), 0.97f), "%.6f", SmoothAverage(0.2f, 0.97f));
    CHECK(Near(SmoothAverage(0.97f, 0.25f), 0.97f - 0.72f * kAverageRelease), "%.6f", SmoothAverage(0.97f, 0.25f));
    CHECK(Near(SmoothAverage(0.0f, 0.0f), 0.0f), "%.6f", SmoothAverage(0.0f, 0.0f));

    // The local limit: a dark neighbourhood (a light, a glint) keeps the
    // whole expansion; a near-white one (smoke, the sun's halo) adds at most
    // the budget; off (0) and no headroom change nothing.
    CHECK(Near(LocalScale(4.0f, 0.01f, kLocalBudget), 1.0f), "%.6f", LocalScale(4.0f, 0.01f, kLocalBudget));
    CHECK(Near(LocalScale(4.0f, 0.5f, kLocalBudget), kLocalBudget / 1.5f), "%.6f", LocalScale(4.0f, 0.5f, kLocalBudget));
    CHECK(Near(3.0f * 0.5f * LocalScale(4.0f, 0.5f, kLocalBudget), kLocalBudget), "%.6f", LocalScale(4.0f, 0.5f, kLocalBudget));
    CHECK(Near(LocalScale(4.0f, 0.5f, 0.0f), 1.0f), "%.6f", LocalScale(4.0f, 0.5f, 0.0f));
    CHECK(Near(LocalScale(1.0f, 1.0f, kLocalBudget), 1.0f), "%.6f", LocalScale(1.0f, 1.0f, kLocalBudget));
    CHECK(LocalScale(2.0f, 0.5f, kLocalBudget) > LocalScale(4.0f, 0.5f, kLocalBudget), "%.6f",
        LocalScale(2.0f, 0.5f, kLocalBudget));
    CHECK(NitsToPq(0.0f) <= 1e-6f, "%.9f", NitsToPq(0.0f));
    CHECK(Near(NitsToPq(100.0f), 0.508078f, 1e-5f), "%.9f", NitsToPq(100.0f));
    CHECK(Near(NitsToPq(203.0f), 0.580689f, 1e-5f), "%.9f", NitsToPq(203.0f));
    CHECK(Near(NitsToPq(400.0f), 0.652579f, 1e-5f), "%.9f", NitsToPq(400.0f));
    CHECK(Near(NitsToPq(1000.0f), 0.751827f, 1e-5f), "%.9f", NitsToPq(1000.0f));
    CHECK(Near(NitsToPq(10000.0f), 1.0f, 1e-6f), "%.9f", NitsToPq(10000.0f));
    for (float nits : { 1.0f, 100.0f, 203.0f, 400.0f, 1000.0f, 10000.0f })
        CHECK(Near(PqToNits(NitsToPq(nits)), nits, 0.05f), "%.3f -> %.9f -> %.3f", nits, NitsToPq(nits), PqToNits(NitsToPq(nits)));
    float previousPq = NitsToPq(0.0f);
    for (int i = 1; i <= 1000; i++)
    {
        const float nits = 10000.0f * float(i) / 1000.0f;
        const float pq = NitsToPq(nits);
        CHECK(pq >= previousPq, "%.1f nits: %.9f after %.9f", nits, pq, previousPq);
        previousPq = pq;
    }

    // The settings overlay over the HDR scene (BlendUi) is the SDR picture's
    // blend wherever the scene is within SDR white: ImGui's own SRC_ALPHA
    // blend of 8-bit sRGB values, decoded. A 75% black panel over mid-grey
    // keeps about 7% of its light (linear light kept a quarter: the bug).
    for (int s = 0; s <= 255; s += 15)
        for (int a : { 0, 64, 128, 191, 255 })
            for (int u : { 0, 30, 128 })
            {
                const float alpha = float(a) / 255.0f, ui = std::min(float(u) / 255.0f, alpha);  // premultiplied
                const float scene = SrgbToLinear(float(s) / 255.0f);
                const float sdr = SrgbToLinear(float(s) / 255.0f * (1.0f - alpha) + ui);
                CHECK(Near(BlendUi(scene, ui, alpha), sdr, 1e-5f), "%d under %d/%d: %.6f vs SDR %.6f", s, u, a,
                    BlendUi(scene, ui, alpha), sdr);
            }
    CHECK(Near(BlendUi(SrgbToLinear(0.5f), 0.0f, 0.75f), SrgbToLinear(0.125f), 1e-6f), "%.6f", BlendUi(SrgbToLinear(0.5f), 0.0f, 0.75f));
    CHECK(BlendUi(SrgbToLinear(0.5f), 0.0f, 0.75f) < 0.08f * SrgbToLinear(0.5f), "%.6f", BlendUi(SrgbToLinear(0.5f), 0.0f, 0.75f));
    // No UI: the scene, highlights included. The UI's opaque white is SDR
    // white over any scene.
    for (float scene : { 0.0f, 0.2f, 1.0f, 1.5f, 4.0f })
    {
        CHECK(Near(BlendUi(scene, 0.0f, 0.0f), scene, 1e-5f), "%.3f -> %.6f", scene, BlendUi(scene, 0.0f, 0.0f));
        CHECK(Near(BlendUi(scene, 1.0f, 1.0f), 1.0f, 1e-6f), "%.3f -> %.6f", scene, BlendUi(scene, 1.0f, 1.0f));
    }
    // Under the UI the scene is SDR: a highlight is clipped at SDR white and
    // dimmed as SDR's 8-bit blend would (a 4x highlight under a 75% panel
    // used to add 0.75 of SDR white: the R8's hood through the settings
    // window), from kUiSdrCoverage up, for every overlay fill and every
    // 8-bit alpha level from 16/255.
    for (float scene : { 1.0f, 1.2f, 2.0f, 4.0f, 16.0f })
        for (int a = 16; a <= 255; a++)
            for (int u : { 0, 14, 128 })
            {
                const float alpha = float(a) / 255.0f, ui = std::min(float(u) / 255.0f, alpha);
                const float sdr = SrgbToLinear(1.0f * (1.0f - alpha) + ui);
                CHECK(Near(BlendUi(scene, ui, alpha), sdr, 1e-5f), "%.1f under %d/%d: %.6f vs SDR %.6f", scene, u, a,
                    BlendUi(scene, ui, alpha), sdr);
            }
    for (float alpha : { 0.55f, 0.62f, 0.75f, 0.95f, 0.98f })
        CHECK(Near(BlendUi(4.0f, 0.0f, alpha), BlendUi(1.0f, 0.0f, alpha), 1e-6f), "%.2f: %.6f vs %.6f", alpha,
            BlendUi(4.0f, 0.0f, alpha), BlendUi(1.0f, 0.0f, alpha));
    CHECK(Near(BlendUi(4.0f, 0.0f, 0.75f), SrgbToLinear(0.25f), 1e-6f), "%.6f", BlendUi(4.0f, 0.0f, 0.75f));
    // Below kUiSdrCoverage (anti-aliased fringes) the highlight's share
    // ramps: 1 at no coverage, 0 at kUiSdrCoverage, monotonic and without a
    // step (8-bit alpha levels, and finer).
    CHECK(UiHighlightShare(0.0f) == 1.0f && UiHighlightShare(kUiSdrCoverage) == 0.0f && UiHighlightShare(1.0f) == 0.0f,
        "%.6f %.6f %.6f", UiHighlightShare(0.0f), UiHighlightShare(kUiSdrCoverage), UiHighlightShare(1.0f));
    CHECK(Near(UiHighlightShare(kUiSdrCoverage * 0.5f), 0.5f, 1e-6f), "%.6f", UiHighlightShare(kUiSdrCoverage * 0.5f));
    CHECK(UiHighlightShare(1.0f / 255.0f) > 0.95f, "one alpha level keeps %.6f", UiHighlightShare(1.0f / 255.0f));
    for (int i = 0; i < 1000; i++)
    {
        const float a = float(i) / 4000.0f, b = float(i + 1) / 4000.0f;
        CHECK(UiHighlightShare(b) <= UiHighlightShare(a), "%.5f: %.6f after %.6f", b, UiHighlightShare(b), UiHighlightShare(a));
        CHECK(UiHighlightShare(a) - UiHighlightShare(b) < 0.0065f, "%.5f: step %.6f", b, UiHighlightShare(a) - UiHighlightShare(b));
        CHECK(BlendUi(4.0f, 0.0f, b) <= BlendUi(4.0f, 0.0f, a), "4x under %.5f: %.6f after %.6f", b, BlendUi(4.0f, 0.0f, b),
            BlendUi(4.0f, 0.0f, a));
    }
    // Continuous across SDR white, and monotonic in the scene.
    CHECK(Near(BlendUi(1.0001f, 0.2f, 0.5f), BlendUi(1.0f, 0.2f, 0.5f), 1e-4f), "%.6f %.6f", BlendUi(1.0001f, 0.2f, 0.5f),
        BlendUi(1.0f, 0.2f, 0.5f));
    CHECK(Near(BlendUi(1.0001f, 0.0f, 0.02f), BlendUi(1.0f, 0.0f, 0.02f), 1e-4f), "%.6f %.6f", BlendUi(1.0001f, 0.0f, 0.02f),
        BlendUi(1.0f, 0.0f, 0.02f));
    for (float alpha : { 0.0f, 0.01f, 0.03f, 0.6f })
        for (int i = 0; i <= 400; i++)
        {
            const float a = BlendUi(float(i) / 100.0f, 0.1f * alpha, alpha), b = BlendUi(float(i + 1) / 100.0f, 0.1f * alpha, alpha);
            CHECK(b >= a, "%.2f, %d: %.6f after %.6f", alpha, i, b, a);
        }

    if (s_failures)
        fprintf(stderr, "%d failure(s)\n", s_failures);
    return s_failures ? 1 : 0;
}
