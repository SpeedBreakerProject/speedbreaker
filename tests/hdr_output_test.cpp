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
    CHECK(kSdrWhiteNits == 100.0f && kHdr10MaxFallNits == 100.0f && kHdrStrengthDefault == 2.0f && kHdrStrengthMax == 4.0f,
        "%.1f %.1f %.1f %.1f", kSdrWhiteNits, kHdr10MaxFallNits, kHdrStrengthDefault, kHdrStrengthMax);
    CHECK(Near(FramePeak(4.0f, 0.0f), 4.0f), "%.6f", FramePeak(4.0f, 0.0f));
    CHECK(Near(FramePeak(4.0f, 0.25f), 4.0f), "%.6f", FramePeak(4.0f, 0.25f));
    CHECK(Near(FramePeak(4.0f, 0.5f), 2.0f), "%.6f", FramePeak(4.0f, 0.5f));
    CHECK(Near(FramePeak(2.0f, 0.5f), 2.0f), "%.6f", FramePeak(2.0f, 0.5f));
    CHECK(Near(FramePeak(4.0f, 1.0f), 1.0f), "%.6f", FramePeak(4.0f, 1.0f));
    CHECK(Near(FramePeak(1.0f, 0.5f), 1.0f), "%.6f", FramePeak(1.0f, 0.5f));
    // A near-white frame stays near SDR rather than flashing.
    CHECK(FramePeak(4.0f, 0.9f) < 1.12f, "%.6f", FramePeak(4.0f, 0.9f));
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

    if (s_failures)
        fprintf(stderr, "%d failure(s)\n", s_failures);
    return s_failures ? 1 : 0;
}
