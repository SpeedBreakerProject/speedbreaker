// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// HDR output (Settings > Display > HDR Output, macOS): SDR sRGB values
// decoded to linear light, expanded into a display's EDR headroom. Header
// only, no runtime dependencies (tests/hdr_output_test.cpp).
#pragma once
#include <algorithm>
#include <cmath>

namespace video::hdr
{
    // The brightest EDR value the picture may use, in multiples of SDR white:
    // the display's current headroom, from 1 (no EDR on a standard-range
    // display, where Expand leaves the picture alone) up to 4x.
    inline float PeakForHeadroom(float headroom)
    {
        return std::clamp(headroom, 1.0f, 4.0f);
    }

    // v: an SDR value in linear light (0 is black, 1 is SDR white). SDR white
    // becomes `peak`; the curve is steep (v^8), so shadows and mid-tones stay
    // where the SDR picture put them and only bright highlights reach the
    // headroom.
    inline float Expand(float v, float peak)
    {
        v = std::clamp(v, 0.0f, 1.0f);
        return v + (std::max(peak, 1.0f) - 1.0f) * std::pow(v, 8.0f);
    }

    // IEC 61966-2-1, the values k/255 of the game's 8-bit picture.
    inline float SrgbToLinear(float v)
    {
        v = std::clamp(v, 0.0f, 1.0f);
        return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
    }
    inline float LinearToSrgb(float v)
    {
        v = std::clamp(v, 0.0f, 1.0f);
        return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
    }

    // HDR10's absolute scale: SDR white, the peak the metadata declares, and
    // a conservative frame-average light level. 400 nits keeps the game's own
    // front-end/HUD white from jumping to a display's full EDR range; the
    // highlight curve still opens above SDR.
    constexpr float kSdrWhiteNits = 100.0f;
    constexpr float kHdr10PeakNits = 400.0f;
    constexpr float kHdr10MaxFallNits = 100.0f;

    // Rec.709 luma, for expanding light rather than each channel (which would
    // shift the hue of bright colours).
    inline float Luma709(float r, float g, float b)
    {
        return 0.2126f * r + 0.7152f * g + 0.0722f * b;
    }

    // Expands an RGB triplet by its Rec.709 luma: the ratio between the
    // channels stays, so saturation and hue do not move.
    inline void ExpandRgb(float& r, float& g, float& b, float peak)
    {
        r = std::clamp(r, 0.0f, 1.0f);
        g = std::clamp(g, 0.0f, 1.0f);
        b = std::clamp(b, 0.0f, 1.0f);
        const float luma = Luma709(r, g, b);
        if (luma <= 0.0f)
            return;
        const float expanded = Expand(luma, peak);
        const float scale = expanded / luma;
        r *= scale;
        g *= scale;
        b *= scale;
    }

    // Linear-light RGB between Rec.709 and Rec.2020 (BT.2087). HDR10's
    // metadata and PQ signal are BT.2020; the game's SDR colours are Rec.709.
    inline void Rec709ToRec2020(float& r, float& g, float& b)
    {
        const float r0 = r, g0 = g, b0 = b;
        r = 0.6274040f * r0 + 0.3292820f * g0 + 0.0433136f * b0;
        g = 0.0690970f * r0 + 0.9195400f * g0 + 0.0113612f * b0;
        b = 0.0163916f * r0 + 0.0880132f * g0 + 0.8955950f * b0;
    }

    inline void Rec2020ToRec709(float& r, float& g, float& b)
    {
        const float r0 = r, g0 = g, b0 = b;
        r = 1.6604910f * r0 - 0.5876411f * g0 - 0.0728499f * b0;
        g = -0.1245505f * r0 + 1.1328999f * g0 - 0.0083494f * b0;
        b = -0.0181508f * r0 - 0.1005789f * g0 + 1.1187297f * b0;
    }

    // SMPTE ST 2084 (PQ), the transfer function HDR10 uses. Nits are cd/m^2,
    // the PQ code is 0..1.
    inline float NitsToPq(float nits)
    {
        constexpr float m1 = 2610.0f / 16384.0f;
        constexpr float m2 = 2523.0f / 4096.0f * 128.0f;
        constexpr float c1 = 3424.0f / 4096.0f;
        constexpr float c2 = 2413.0f / 128.0f;
        constexpr float c3 = 2392.0f / 128.0f;
        const float y = std::pow(std::clamp(nits, 0.0f, 10000.0f) / 10000.0f, m1);
        return std::pow((c1 + c2 * y) / (1.0f + c3 * y), m2);
    }

    inline float PqToNits(float pq)
    {
        constexpr float m1 = 2610.0f / 16384.0f;
        constexpr float m2 = 2523.0f / 4096.0f * 128.0f;
        constexpr float c1 = 3424.0f / 4096.0f;
        constexpr float c2 = 2413.0f / 128.0f;
        constexpr float c3 = 2392.0f / 128.0f;
        const float p = std::pow(std::clamp(pq, 0.0f, 1.0f), 1.0f / m2);
        if (p <= c1)
            return 0.0f;
        return 10000.0f * std::pow((p - c1) / (c2 - c3 * p), 1.0f / m1);
    }
}
