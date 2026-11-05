// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Settings > Graphics > Sharper UI (NFSMW_SHARP_UI=0/1, live; public issue
// #85): the menus and the HUD with a sharper magnification filter.
//
// The front end's art (glyph atlases, panels, icons: mostly k_8_8_8_8) was
// made for 720p or less. Above 1x Internal Resolution its quads land at host
// resolution, and each texel is spread over 2-3 pixels by bilinear
// filtering: the softness is the filter's, not the resolution's. Here a
// front-end draw (the front-end matrix in c0..c3 and ePolys at vertex fetch
// constant 95, the test gpu/renderer.cpp's edge-bar and cover rules use; at
// every screen shape, 16:9 included) into a scaled target takes a variant of
// its pixel shader in which each 2D fetch goes through sharpTex2D (below):
// where the texture is magnified and linearly filtered, it is reconstructed
// by Catmull-Rom over the 4x4 texels around the sample, clamped to the 2x2
// texels bilinear filtering would blend (it can't ring, overshoot, or bring
// in a colour that wasn't there), on premultiplied texels so a glyph's
// transparent surround (whatever colour its texels hold) can't fringe it.
// Everything else (minified, point filtered, a mirrored or bordered
// texture, a mip floor above level 0) is the hardware's own fetch, the same
// instruction the plain shader runs. Layout, blending and the picture
// outside the front end's draws are untouched; Off, and every draw at 1x,
// is the plain pipeline (bit-identical to before).
//
// Vulkan-free: tests/sharp_ui_test.cpp checks the rules and a CPU model of
// the filter.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace gpu::sharp_ui
{
    // How a front-end draw's 2D fetches are filtered.
    enum class Mode : uint8_t
    {
        Off = 0,            // the plain pixel shader
        Premultiplied = 1,  // sharpTex2D on premultiplied texels (colour weighted by its alpha: SRC_ALPHA)
        Straight = 2,       // sharpTex2D on the texels as stored (blending off, or a source factor of one): its faster form
        Luma = 3,           // Straight on fetch slot 0 only: the movie quad's Y plane (its U/V planes stay the hardware's)
    };

    // RB_BLENDCONTROL0 (0x1FFF1FFF bits) as the pipeline key holds it (~0u:
    // blending off). Colour source factor SRC_ALPHA (6): the texel's colour
    // reaches the target weighted by its alpha, so premultiplied filtering is
    // the exact one. ONE (1) or blending off: the colour counts whatever the
    // alpha, so filter the texels as they are. Any other source factor
    // (destination colour, constants...): left to the hardware.
    inline Mode ModeForBlend(uint32_t blend)
    {
        if (blend == ~0u)
            return Mode::Straight;
        const uint32_t src = blend & 0x1F;
        if (src == 6)
            return Mode::Premultiplied;
        if (src == 1)
            return Mode::Straight;
        return Mode::Off;
    }

    // The pixel-shader variant's key bits (gpu/renderer.cpp PixelVariant).
    inline uint32_t VariantBits(Mode mode)
    {
        return mode == Mode::Off ? 0u : mode == Mode::Premultiplied ? (1u << 21) : mode == Mode::Straight ? (1u << 21) | (1u << 22)
            : (1u << 21) | (1u << 22) | (1u << 23);
    }

    // The rule sharpTex2D applies before it reconstructs anything (its CPU
    // twin, for the tests). `rho`: the fetch's footprint, texels per pixel
    // along its longer screen axis (below 1: magnified); `lod`: the
    // instruction's LOD (explicit) or bias (computed); the fetch constant's
    // clamp modes, mag filter and mip floor; `levels`: the texture's mips.
    inline bool Reconstructs(float rho, bool computedLod, float lod, uint32_t clampX, uint32_t clampY, uint32_t magFilter,
        uint32_t mipMinLevel, uint32_t levels)
    {
        auto addressable = [](uint32_t c) { return c == 0 || (c >= 2 && c <= 5); };  // repeat, or any clamp to the edge
        const float lambda = computedLod ? std::log2(std::max(rho, 1e-8f)) + lod : lod;
        const bool baseLevel = levels == 1 || mipMinLevel == 0;
        return rho < 1.0f && lambda <= 0.0f && magFilter != 0 && addressable(clampX) && addressable(clampY) && baseLevel;
    }

    // Catmull-Rom's four weights for a sample `f` (0..1) past the second tap.
    inline std::array<float, 4> CatmullRom(float f)
    {
        return { f * (-0.5f + f * (1.0f - 0.5f * f)), 1.0f + f * f * (-2.5f + 1.5f * f), f * (0.5f + f * (2.0f - 1.5f * f)),
            f * f * (-0.5f + 0.5f * f) };
    }

    struct Rgba
    {
        float r, g, b, a;
    };

    // sharpTex2D's reconstruction on the CPU: `texel(x, y)` for any integer
    // texel (the caller wraps or clamps it), at texture position (px, py)
    // in texels (uv * size). {-1,...}: the shader keeps the hardware's
    // sample there (a flat 2x2, or a clear one premultiplied).
    template<typename Texel>
    Rgba Bilinear(const Texel& texel, float px, float py)
    {
        const float cx = std::floor(px - 0.5f), cy = std::floor(py - 0.5f), fx = px - 0.5f - cx, fy = py - 0.5f - cy;
        const Rgba a = texel(int(cx), int(cy)), b = texel(int(cx) + 1, int(cy)), c = texel(int(cx), int(cy) + 1), d = texel(int(cx) + 1, int(cy) + 1);
        auto mix = [](float p, float q, float t) { return p + (q - p) * t; };
        return { mix(mix(a.r, b.r, fx), mix(c.r, d.r, fx), fy), mix(mix(a.g, b.g, fx), mix(c.g, d.g, fx), fy),
            mix(mix(a.b, b.b, fx), mix(c.b, d.b, fx), fy), mix(mix(a.a, b.a, fx), mix(c.a, d.a, fx), fy) };
    }

    template<typename Texel>
    Rgba Reconstruct(const Texel& texel, float px, float py, bool premultiplied)
    {
        const float cx = std::floor(px - 0.5f), cy = std::floor(py - 0.5f);
        const float fx = px - 0.5f - cx, fy = py - 0.5f - cy;
        const auto wx = CatmullRom(fx), wy = CatmullRom(fy);
        Rgba t[4];
        for (int k = 0; k < 4; k++)
            t[k] = texel(int(cx) + (k & 1), int(cy) + (k >> 1));
        auto same = [](const Rgba& a, const Rgba& b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; };
        const Rgba keep{ -1.0f, -1.0f, -1.0f, -1.0f };
        if (same(t[0], t[1]) && same(t[0], t[2]) && same(t[0], t[3]))
            return keep;
        if (premultiplied && std::max(std::max(t[0].a, t[1].a), std::max(t[2].a, t[3].a)) == 0.0f)
            return keep;
        auto pm = [&](Rgba v) { return premultiplied ? Rgba{ v.r * v.a, v.g * v.a, v.b * v.a, v.a } : v; };
        Rgba p[4];
        for (int k = 0; k < 4; k++)
            p[k] = pm(t[k]);
        const float w12x = wx[1] + wx[2], w12y = wy[1] + wy[2], gx = wx[2] / w12x, gy = wy[2] / w12y;
        const float ccx = cx + 0.5f, ccy = cy + 0.5f;
        const Rgba n = pm(Bilinear(texel, ccx + gx, ccy - 1.0f)), w = pm(Bilinear(texel, ccx - 1.0f, ccy + gy)),
                   e = pm(Bilinear(texel, ccx + 2.0f, ccy + gy)), s = pm(Bilinear(texel, ccx + gx, ccy + 2.0f));
        const float wN = w12x * wy[0], wW = wx[0] * w12y, wM = w12x * w12y, wE = wx[3] * w12y, wS = w12x * wy[3];
        const float sum = wN + wW + wM + wE + wS;
        float out[4], lo[4], hi[4];
        auto ch = [](const Rgba& v, int k) { return k == 0 ? v.r : k == 1 ? v.g : k == 2 ? v.b : v.a; };
        for (int k = 0; k < 4; k++)
        {
            const float m0 = ch(p[0], k) + (ch(p[1], k) - ch(p[0], k)) * gx, m1 = ch(p[2], k) + (ch(p[3], k) - ch(p[2], k)) * gx;
            const float middle = m0 + (m1 - m0) * gy;
            out[k] = (ch(n, k) * wN + ch(w, k) * wW + middle * wM + ch(e, k) * wE + ch(s, k) * wS) / sum;
            lo[k] = std::min(std::min(ch(p[0], k), ch(p[1], k)), std::min(ch(p[2], k), ch(p[3], k)));
            hi[k] = std::max(std::max(ch(p[0], k), ch(p[1], k)), std::max(ch(p[2], k), ch(p[3], k)));
            out[k] = std::clamp(out[k], lo[k], hi[k]);
        }
        Rgba r{ out[0], out[1], out[2], out[3] };
        if (premultiplied)
        {
            if (r.a < 1.0f / 1024.0f)
                return keep;
            float slo[3] = { 1e30f, 1e30f, 1e30f }, shi[3] = { -1e30f, -1e30f, -1e30f };
            for (int k = 0; k < 4; k++)
                if (t[k].a > 0.0f)
                    for (int j = 0; j < 3; j++)
                    {
                        slo[j] = std::min(slo[j], ch(t[k], j));
                        shi[j] = std::max(shi[j], ch(t[k], j));
                    }
            r.r = std::clamp(r.r / r.a, slo[0], shi[0]);
            r.g = std::clamp(r.g / r.a, slo[1], shi[1]);
            r.b = std::clamp(r.b / r.a, slo[2], shi[2]);
        }
        return r;
    }

    // The GLSL the variant's translation adds (gpu/shader_translator.cpp,
    // TranslateOptions::sharpFetch2D). The hardware's sample is always taken
    // first, in the same control flow as the plain shader's (its implicit
    // derivatives), and returned unless the fetch magnifies; texelFetch goes
    // through the same image view (its swizzle).
    inline constexpr const char* kSharpTex2DGlsl = R"(
vec4 sharpTex2D(sampler2D t2, vec2 uv, uint slot, bool computedLod, float lod)
{
    vec4 hw = computedLod ? texture(t2, uv, lod) : textureLod(t2, uv, lod);
#if defined(SHARP_EXP) && SHARP_EXP == 1
    return hw;  // NFSMW_SHARP_UI_EXP=1 (a diagnostic): the variant's pipeline, none of its work
#endif
#ifdef SHARP_SLOT0_ONLY
    if (slot != 0u)
        return hw;
#endif
    ivec2 isize = textureSize(t2, 0);
    vec2 pos = uv * vec2(isize);
    vec2 dx = dFdx(pos), dy = dFdy(pos);
    float rho = sqrt(max(dot(dx, dx), dot(dy, dy)));
    uint d0 = fetchDword(slot, 0u), d3 = fetchDword(slot, 3u), d4 = fetchDword(slot, 4u);
    uint cx = (d0 >> 10) & 7u, cy = (d0 >> 13) & 7u;
    bool addressable = (cx == 0u || (cx >= 2u && cx <= 5u)) && (cy == 0u || (cy >= 2u && cy <= 5u));
    float lambda = computedLod ? log2(max(rho, 1e-8)) + lod : lod;
    bool baseLevel = textureQueryLevels(t2) == 1 || ((d4 >> 2) & 15u) == 0u;
    if (!(rho < 1.0 && lambda <= 0.0 && ((d3 >> 19) & 3u) != 0u && addressable && baseLevel))
        return hw;
#if defined(SHARP_EXP) && SHARP_EXP == 2
    return hw;  // NFSMW_SHARP_UI_EXP=2: the checks, no texel fetched
#endif
    vec2 c = floor(pos - 0.5);
    vec2 f = pos - 0.5 - c;
    vec2 w[4];
    w[0] = f * (-0.5 + f * (1.0 - 0.5 * f));
    w[1] = 1.0 + f * f * (-2.5 + 1.5 * f);
    w[2] = f * (0.5 + f * (2.0 - 1.5 * f));
    w[3] = f * f * (-0.5 + 0.5 * f);
    // The 2x2 texels bilinear filtering blends first: where they are all
    // the same (a fill, a panel's flat inside) or all clear (a glyph's
    // surround), the clamp below would return exactly what the hardware
    // did: keep its sample and fetch nothing more.
    ivec2 base = ivec2(c);
    vec4 t[4];
    for (int k = 0; k < 4; k++)
    {
        ivec2 q = base + ivec2(k & 1, k >> 1);
        q.x = cx == 0u ? ((q.x % isize.x) + isize.x) % isize.x : clamp(q.x, 0, isize.x - 1);
        q.y = cy == 0u ? ((q.y % isize.y) + isize.y) % isize.y : clamp(q.y, 0, isize.y - 1);
        t[k] = texelFetch(t2, q, 0);
    }
    if (t[0] == t[1] && t[0] == t[2] && t[0] == t[3])
        return hw;
    // Catmull-Rom from the 2x2 and four bilinear taps (the middle two
    // weights of each axis folded into one), the four corner taps (at most
    // 1.6% of the weight) left out and the rest renormalised, as the
    // presenter's fast scale pass does: 8 fetches instead of 16 (the 16
    // fetches were Sharper UI's whole cost: 2 ms at 4K on an M1 Pro).
    // Premultiplied: the 2x2 exactly; each edge tap's bilinear sample
    // premultiplied after filtering (exact where its two texels' alpha
    // agree; it weighs at most ~7%).
#ifndef SHARP_STRAIGHT
    if (max(max(t[0].a, t[1].a), max(t[2].a, t[3].a)) == 0.0)
        return hw;
    vec4 p[4];
    for (int k = 0; k < 4; k++)
        p[k] = vec4(t[k].rgb * t[k].a, t[k].a);
#define SHARP_PM(v) vec4((v).rgb * (v).a, (v).a)
#else
    vec4 p[4] = t;
#define SHARP_PM(v) (v)
#endif
    vec2 w12 = w[1] + w[2];
    vec2 g = w[2] / w12;
    vec2 cc = c + 0.5, size = vec2(isize);
    vec2 t0 = (cc - 1.0) / size, t12 = (cc + g) / size, t3 = (cc + 2.0) / size;
    vec4 middle = mix(mix(p[0], p[1], g.x), mix(p[2], p[3], g.x), g.y);
    float wN = w12.x * w[0].y, wW = w[0].x * w12.y, wM = w12.x * w12.y, wE = w[3].x * w12.y, wS = w12.x * w[3].y;
    vec4 cr = (SHARP_PM(textureLod(t2, vec2(t12.x, t0.y), 0.0)) * wN + SHARP_PM(textureLod(t2, vec2(t0.x, t12.y), 0.0)) * wW + middle * wM +
        SHARP_PM(textureLod(t2, vec2(t3.x, t12.y), 0.0)) * wE + SHARP_PM(textureLod(t2, vec2(t12.x, t3.y), 0.0)) * wS) / (wN + wW + wM + wE + wS);
#undef SHARP_PM
    vec4 r = clamp(cr, min(min(p[0], p[1]), min(p[2], p[3])), max(max(p[0], p[1]), max(p[2], p[3])));
#ifndef SHARP_STRAIGHT
    if (r.a < 1.0 / 1024.0)
        return hw;
    vec3 slo = vec3(1e30), shi = vec3(-1e30);
    for (int k = 0; k < 4; k++)
        if (t[k].a > 0.0)
        {
            slo = min(slo, t[k].rgb);
            shi = max(shi, t[k].rgb);
        }
    r.rgb = clamp(r.rgb / r.a, slo, shi);
#endif
    return r;
}
)";

    // UI-C (video/presenter.cpp's UI pass): a front-end variant also records
    // how much of each pixel it wrote, as an 8-bit weight, into an R32UI
    // image (imageAtomicMax: the most any front-end layer covered it), so the
    // presenter can reconstruct those pixels with EASU from the frame before
    // anti-aliasing. SHARP_UI_MASK 1: the output's alpha (SRC_ALPHA blends);
    // 2: its brightest channel (additive ONE); 3: fully (blending off). Only
    // the variant's GLSL has it: the plain translation is unchanged.
    inline constexpr const char* kUiMaskDeclGlsl = R"(
#ifdef SHARP_UI_MASK
layout(set = 1, binding = 127, r32ui) uniform coherent uimage2D sb_uiMask;
#endif
)";
    inline constexpr const char* kUiMaskStoreGlsl = R"(#ifdef SHARP_UI_MASK
    {
#if SHARP_UI_MASK == 1
        float sbWeight = o_color[0].a;
#elif SHARP_UI_MASK == 2
        float sbWeight = max(max(o_color[0].r, o_color[0].g), o_color[0].b);
#else
        float sbWeight = 1.0;
#endif
        ivec2 sbAt = ivec2(gl_FragCoord.xy);
        uint sbValue = uint(clamp(sbWeight, 0.0, 1.0) * 255.0 + 0.5);
        if (sbValue != 0u && all(lessThan(sbAt, imageSize(sb_uiMask))))
            imageAtomicMax(sb_uiMask, sbAt, sbValue);
    }
#endif
)";

    // The mask's weight kind for a front-end draw (0: no mask): its blend
    // as ModeForBlend reads it, and none for a draw whose 2D textures are
    // all 32x32 or smaller (the game's white fill under every dim and fade:
    // those cover the 3D scene and carry no UI detail).
    inline uint32_t UiMaskKind(uint32_t blend, bool smallTexturesOnly)
    {
        if (smallTexturesOnly)
            return 0;
        if (blend == ~0u)
            return 3;
        const uint32_t src = blend & 0x1F;
        return src == 6 ? 1 : src == 1 ? 2 : 0;
    }
}

