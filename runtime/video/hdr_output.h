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

    // HDR10's absolute scale: SDR white, and the frame-average level the
    // limiter keeps the picture near. The peak itself comes from the HDR
    // Strength setting (2x/200 nits by default, up to 4x/400).
    constexpr float kSdrWhiteNits = 100.0f;

    // HDR10's SDR white ("paper white"): the nits the game's own white is
    // sent at, its peak HDR Strength times that, and the frame-average level
    // (MaxFALL) the limiter holds to. Auto: 100 on a Mac (kSdrWhiteNits),
    // 400 under gamescope (SteamOS's and the Steam Deck OLED's own brightness
    // for SDR games in HDR, its --hdr-sdr-content-nits default), 200 on other
    // systems (Windows and Linux desktops, whose SDR white sliders sit near
    // it); or the setting's 100, 200, 300 or 400.
    inline float PaperWhiteNits(int32_t setting, bool apple, bool gamescope)
    {
        if (setting >= 100 && setting <= 400)
            return float(setting);
        return apple ? kSdrWhiteNits : gamescope ? 400.0f : 200.0f;
    }
    constexpr float kHdrStrengthDefault = 2.0f;
    constexpr float kHdrStrengthMax = 4.0f;

    // The peak for a frame whose SDR luma averages `average` (0..1): keep
    // peak * average at SDR white, so a full-white frame (a boot flash, a
    // logo) cannot become a full-screen 4x flash. Normal, darker frames keep
    // the requested `base` peak.
    inline float FramePeak(float base, float average)
    {
        base = std::max(base, 1.0f);
        average = std::clamp(average, 0.0f, 1.0f);
        if (average <= 0.0f)
            return base;
        return std::clamp(std::min(base, 1.0f / average), 1.0f, base);
    }

    // The limiter's frame-to-frame state (kHdrAverageCs keeps it on the GPU):
    // a brighter frame takes effect at once, a darker one eases back by
    // kAverageRelease of the gap a frame, so the peak doesn't pump.
    constexpr float kAverageRelease = 0.15f;
    inline float SmoothAverage(float previous, float average)
    {
        return average > previous ? average : previous + (average - previous) * kAverageRelease;
    }

    // The local limit on added light: Expand adds (peak - 1) * v^8, and in a
    // large near-white area (tyre smoke, the sun's halo, a bright sky) the top
    // of that curve stretches the last few 8-bit steps into a flat glowing
    // blob (240 gains 2.1x at 4x, 255 gains 4x). kHdrAverageCs keeps each of
    // its 32x18 cells' mean v^8 averaged over the 3x3 cells around it (about
    // 120 px at 720p); the composite scales a pixel's added light so that
    // neighbourhood adds at most kLocalBudget of SDR white. Small highlights
    // (lights, glints, chrome) sit in dark neighbourhoods and keep the full
    // peak. Measured on Most Wanted's race start in its burnout smoke, 4x:
    // the smoke's 99th percentile 3.35x -> 1.77x, the tail lights inside it
    // 4.0x -> 3.3x. NFSMW_HDR_LOCAL_BUDGET overrides it; 0 turns it off.
    constexpr float kLocalBudget = 0.1f;
    inline float LocalScale(float peak, float neighbourhoodV8, float budget)
    {
        const float added = (std::max(peak, 1.0f) - 1.0f) * std::max(neighbourhoodV8, 0.0f);
        if (budget <= 0.0f || added <= budget)
            return 1.0f;
        return budget / added;
    }

    // The UI coverage at which the scene under it has no light above SDR
    // white left (BlendUi): 1/16, 16 levels of ImGui's 8-bit alpha. Every
    // fill the overlay draws on its own covers far more (the panel 0.95,
    // popups 0.98, the modal dim 0.55, the performance overlay 0.62), so
    // everywhere inside a window the scene is SDR; only the outermost
    // anti-aliased fringe of a window's edge or a glyph, below 1/16, ramps
    // between the two, so the cut has no hard step (smoothstep: no kink at
    // either end, and fully transparent pixels keep their highlights).
    inline constexpr float kUiSdrCoverage = 1.0f / 16.0f;

    // How much of the scene's light above SDR white shows through UI
    // coverage `uiAlpha`: 1 with no UI, 0 from kUiSdrCoverage up,
    // 1 - smoothstep(0, kUiSdrCoverage, uiAlpha) between.
    inline float UiHighlightShare(float uiAlpha)
    {
        const float t = std::clamp(uiAlpha / kUiSdrCoverage, 0.0f, 1.0f);
        return 1.0f - t * t * (3.0f - 2.0f * t);
    }

    // The settings overlay over the HDR scene, one channel: `scene` in linear
    // light (1.0 = SDR white, above it the highlights), `ui` and `uiAlpha` as
    // ImGui leaves them in its 8-bit target (premultiplied, sRGB-encoded).
    // ImGui blends sRGB-encoded values: in SDR it draws straight onto the
    // UNORM swapchain. Blended in linear light instead, a dark translucent
    // panel lets far more of the game through (a 75% black one: a quarter
    // of the light instead of 5-7%), so the game under the panel brightened
    // when HDR came on. So it blends as in SDR: the scene up to SDR white
    // encoded, the UI over it, decoded (the SDR picture's value; the UI's
    // own white stays SDR white).
    // The overlay stays at standard brightness, so under it the scene is
    // SDR: its light above SDR white is removed where the UI covers the
    // pixel (UiHighlightShare), as SDR clips a highlight at white before the
    // panel dims it. Dimming it only by (1 - uiAlpha) left a 4x highlight
    // adding 0.75 of SDR white through a 75% panel: a blown-out patch inside
    // the window. Where there is no UI the highlight keeps its full strength.
    inline float BlendUi(float scene, float ui, float uiAlpha)
    {
        scene = std::max(scene, 0.0f);
        uiAlpha = std::clamp(uiAlpha, 0.0f, 1.0f);
        const float sdr = std::min(scene, 1.0f);
        const float blended = SrgbToLinear(LinearToSrgb(sdr) * (1.0f - uiAlpha) + ui);
        return blended + (scene - sdr) * UiHighlightShare(uiAlpha);
    }

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
