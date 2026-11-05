// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See fsr1.h.
#include "fsr1.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

// AMD's headers (thirdparty/fsr1, MIT), as text for the shaders: k_ffx_a and
// k_ffx_fsr1 (runtime/CMakeLists.txt).
#include <fsr1_headers.inc>

// The same headers' CPU code, for the constants (it defines short type
// names and many helpers this file doesn't use).
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#define A_CPU 1
#include <thirdparty/fsr1/ffx_a.h>
#include <thirdparty/fsr1/ffx_fsr1.h>
#pragma GCC diagnostic pop

namespace video::fsr1
{
    namespace
    {
        std::string Text(const unsigned char* bytes)
        {
            return std::string(reinterpret_cast<const char*>(bytes));
        }

        // Both shaders: AMD's 32-bit GLSL path (no extensions needed).
        constexpr const char* kPrelude = R"(#version 460
#define A_GPU 1
#define A_GLSL 1
)";

        // EASU, one 8x8 group per 64 pixels of the picture. Its 12 taps come
        // from 4 gathers (FsrEasuRF..BF at the corners between texels);
        // under Hor+ and Vert+ (bandOn) each of a gather's 2x2 texels is
        // fetched instead and kept in this pixel's span of the band's three
        // along each axis (band_filter.h's SpanOf and ClampTexel), so a strip
        // pixel reads only the strip and a band pixel only the band.
        // band: the band's columns x..y and rows z..w in texels (video::BandOf).
        constexpr const char* kEasuHead = R"(
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1, rgba8) uniform writeonly image2D dst;
layout(push_constant) uniform P { uvec4 con0, con1, con2, con3; ivec4 band; ivec2 outSize; ivec2 srcSize; uint bandOn; } p;

// This pixel's span: its first and last texels along each axis.
ivec2 spanLo = ivec2(0), spanHi = ivec2(0);

// band_filter.h's SpanOf: [0, lo), [lo, hi) or [hi, size) holding t, as its first and end texels.
ivec2 spanOf(float t, int lo, int hi, int size)
{
    return t < float(lo) ? ivec2(0, lo) : t < float(hi) ? ivec2(lo, hi) : ivec2(hi, size);
}

vec3 texelIn(ivec2 t)
{
    return texelFetch(src, clamp(t, spanLo, spanHi), 0).rgb;
}

// What a gather at q reads, each texel kept in the span, in a gather's
// order: (x0, y1), (x1, y1), (x1, y0), (x0, y0).
vec4 quadIn(vec2 q, int c)
{
    ivec2 t = ivec2(floor(q * vec2(p.srcSize) - 0.5));
    return vec4(texelIn(t + ivec2(0, 1))[c], texelIn(t + ivec2(1, 1))[c], texelIn(t + ivec2(1, 0))[c], texelIn(t)[c]);
}

AF4 FsrEasuRF(AF2 q) { return p.bandOn == 0u ? textureGather(src, q, 0) : quadIn(q, 0); }
AF4 FsrEasuGF(AF2 q) { return p.bandOn == 0u ? textureGather(src, q, 1) : quadIn(q, 1); }
AF4 FsrEasuBF(AF2 q) { return p.bandOn == 0u ? textureGather(src, q, 2) : quadIn(q, 2); }
#define FSR_EASU_F 1
)";

        constexpr const char* kEasuMain = R"(
void main()
{
    uvec2 ip = gl_GlobalInvocationID.xy;
    if (ip.x >= uint(p.outSize.x) || ip.y >= uint(p.outSize.y))
        return;
    if (p.bandOn != 0u)
    {
        // Where this pixel's centre falls in the frame image (texels).
        vec2 pos = (vec2(ip) + 0.5) * vec2(p.srcSize) / vec2(p.outSize);
        ivec2 sx = spanOf(pos.x, p.band.x, p.band.y, p.srcSize.x), sy = spanOf(pos.y, p.band.z, p.band.w, p.srcSize.y);
        spanLo = ivec2(sx.x, sy.x);
        spanHi = ivec2(sx.y, sy.y) - 1;
    }
    AF3 c;
    FsrEasuF(c, ip, p.con0, p.con1, p.con2, p.con3);
    imageStore(dst, ivec2(ip), vec4(c, 1.0));
}
)";

        // RCAS, drawn over the picture's place in the swapchain image (the
        // viewport at origin, outSize): src is EASU's image. Under Hor+ and
        // Vert+ a neighbour on the other side of the band's edge (its centre
        // maps into another span than this pixel's, by EASU's rule) reads
        // this pixel instead: the span's edge, as ClampTexel keeps a tap.
        // con.x 0 (Sharpening 0): EASU's picture as it is (RCAS's own
        // arithmetic would still scale it by its approximate reciprocal).
        constexpr const char* kRcasHead = R"(
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform P { uvec4 con; ivec4 band; ivec2 origin; ivec2 outSize; ivec2 srcSize; uint bandOn; } p;

ivec2 centre = ivec2(0), centreSpan = ivec2(0);

// Which of the band's three spans along an axis holds output pixel i's centre: 0, 1 or 2.
int spanIndex(int i, int lo, int hi, int outSize, int srcSize)
{
    float t = (float(i) + 0.5) * float(srcSize) / float(outSize);
    return t < float(lo) ? 0 : t < float(hi) ? 1 : 2;
}

AF4 FsrRcasLoadF(ASU2 q)
{
    ivec2 t = clamp(q, ivec2(0), p.outSize - 1);
    if (p.bandOn != 0u)
    {
        if (spanIndex(t.x, p.band.x, p.band.y, p.outSize.x, p.srcSize.x) != centreSpan.x)
            t.x = centre.x;
        if (spanIndex(t.y, p.band.z, p.band.w, p.outSize.y, p.srcSize.y) != centreSpan.y)
            t.y = centre.y;
    }
    return texelFetch(src, t, 0);
}
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
#define FSR_RCAS_F 1
)";

        constexpr const char* kRcasMain = R"(
void main()
{
    centre = clamp(ivec2(gl_FragCoord.xy) - p.origin, ivec2(0), p.outSize - 1);
    if (p.bandOn != 0u)
        centreSpan = ivec2(spanIndex(centre.x, p.band.x, p.band.y, p.outSize.x, p.srcSize.x),
            spanIndex(centre.y, p.band.z, p.band.w, p.outSize.y, p.srcSize.y));
    if (p.con.x == 0u)
    {
        color = vec4(texelFetch(src, centre, 0).rgb, 1.0);
        return;
    }
    AF1 r, g, b;
    FsrRcasF(r, g, b, uvec2(centre), p.con);
    color = vec4(r, g, b, 1.0);
}
)";
    }

    std::array<uint32_t, 16> EasuConstants(uint32_t srcW, uint32_t srcH, uint32_t dstW, uint32_t dstH)
    {
        std::array<uint32_t, 16> con{};
        FsrEasuCon(&con[0], &con[4], &con[8], &con[12], AF1(srcW), AF1(srcH), AF1(srcW), AF1(srcH), AF1(dstW), AF1(dstH));
        return con;
    }

    std::array<uint32_t, 4> RcasConstants(float stops)
    {
        std::array<uint32_t, 4> con{};
        if (!std::isfinite(stops))
            return con;  // off: con.x 0 (the shader then copies EASU's picture)
        FsrRcasCon(con.data(), AF1(stops));
        return con;
    }

    // UI-C's pass (video/presenter.cpp): a fragment shader over the
    // picture's place, drawn after the scale pass with blending: where the
    // UI mask (the renderer's front-end coverage, R8, read bilinear at this
    // pixel's place in the frame) is 0 it discards (that pixel stays the
    // scale pass's, exactly); elsewhere EASU from the frame before
    // anti-aliasing (src), with the mask as its alpha. NFSMW_UI_MASK_SHOW
    // (UI_MASK_SHOW): red, at half the mask.
    std::string UiEasuGlsl()
    {
        static const char* kHead = R"(
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 2) uniform sampler2D uiMask;
// band: under Hor+ and Vert+ (bandOn), the 16:9 band's columns x..y and rows
// z..w in texels (video::BandOf): each tap is kept in this pixel's span of
// the band's three along each axis, as the EASU pass's (kEasuHead), so the
// band's edge stays a filtering boundary and shows no seam.
layout(push_constant) uniform P { uvec4 con0, con1, con2, con3; ivec4 band; ivec2 origin; ivec2 outSize; ivec2 srcSize; uint show; uint bandOn; } p;
ivec2 spanLo = ivec2(0), spanHi = ivec2(0);
ivec2 spanOf(float t, int lo, int hi, int size)
{
    return t < float(lo) ? ivec2(0, lo) : t < float(hi) ? ivec2(lo, hi) : ivec2(hi, size);
}
vec3 texelIn(ivec2 t)
{
    return texelFetch(src, clamp(t, spanLo, spanHi), 0).rgb;
}
vec4 quadIn(vec2 q, int c)
{
    ivec2 t = ivec2(floor(q * vec2(p.srcSize) - 0.5));
    return vec4(texelIn(t + ivec2(0, 1))[c], texelIn(t + ivec2(1, 1))[c], texelIn(t + ivec2(1, 0))[c], texelIn(t)[c]);
}
AF4 FsrEasuRF(AF2 q) { return p.bandOn == 0u ? textureGather(src, q, 0) : quadIn(q, 0); }
AF4 FsrEasuGF(AF2 q) { return p.bandOn == 0u ? textureGather(src, q, 1) : quadIn(q, 1); }
AF4 FsrEasuBF(AF2 q) { return p.bandOn == 0u ? textureGather(src, q, 2) : quadIn(q, 2); }
#define FSR_EASU_F 1
)";
        static const char* kMain = R"(
void main()
{
    ivec2 ip = ivec2(gl_FragCoord.xy) - p.origin;
    vec2 at = (vec2(ip) + 0.5) / vec2(p.outSize);
    float w = textureLod(uiMask, at, 0.0).r;
    if (w <= 0.0)
        discard;
    if (p.show != 0u)
    {
        color = vec4(1.0, 0.0, 0.0, 0.5 * w);
        return;
    }
    if (p.bandOn != 0u)
    {
        vec2 pos = at * vec2(p.srcSize);
        ivec2 sx = spanOf(pos.x, p.band.x, p.band.y, p.srcSize.x), sy = spanOf(pos.y, p.band.z, p.band.w, p.srcSize.y);
        spanLo = ivec2(sx.x, sy.x);
        spanHi = ivec2(sx.y, sy.y) - 1;
    }
    AF3 c;
    FsrEasuF(c, uvec2(ip), p.con0, p.con1, p.con2, p.con3);
    color = vec4(c, w);
}
)";
        return kPrelude + Text(k_ffx_a) + kHead + Text(k_ffx_fsr1) + kMain;
    }

    std::string EasuGlsl()
    {
        return kPrelude + Text(k_ffx_a) + kEasuHead + Text(k_ffx_fsr1) + kEasuMain;
    }

    std::string RcasGlsl()
    {
        return kPrelude + Text(k_ffx_a) + kRcasHead + Text(k_ffx_fsr1) + kRcasMain;
    }
}
