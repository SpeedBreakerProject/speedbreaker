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
    // the display's current headroom, capped at one stop more than SDR (2x)
    // up to 4x on a display with more headroom. 1: no EDR (a standard-range
    // display), and Expand then leaves the picture alone.
    inline float PeakForHeadroom(float headroom)
    {
        return std::clamp(headroom, 1.0f, 4.0f);
    }

    // v: an SDR value in linear light (0 is black, 1 is SDR white). SDR white
    // becomes `peak`; the curve is steep (v^6), so shadows and mid-tones stay
    // where the SDR picture put them and only highlights reach the headroom.
    inline float Expand(float v, float peak)
    {
        v = std::clamp(v, 0.0f, 1.0f);
        return v + (std::max(peak, 1.0f) - 1.0f) * std::pow(v, 6.0f);
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

    // HDR10's absolute scale: SDR white, and the peak the metadata declares.
    constexpr float kSdrWhiteNits = 100.0f;
    constexpr float kHdr10PeakNits = 1000.0f;

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
