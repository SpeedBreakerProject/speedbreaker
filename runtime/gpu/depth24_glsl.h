// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// 24-bit depth as the resolve shader packs it: formats 22 and 23 (k_24_8,
// k_24_8_FLOAT), stored as unsigned normalized depth in a word's top 24
// bits, the stencil byte below them 0. The expression is written once
// (SB_DEPTH24_PACK_EXPR). The shader gets it as text (GLSL_DEPTH24_PACK,
// spliced into renderer.cpp's kResolveGlsl the way shared_memory_glsl.h's
// GLSL_SHARED_MEMORY_WRITE is), and C++ evaluates the same tokens
// (PackDepth24, through Depth24Glsl's GLSL meanings for them). So the text
// the GPU compiles and the arithmetic tests/depth24_pack_test.cpp checks
// over every binary32 input that can reach the top of the range cannot
// drift apart.
//
// The clamp to 0xFFFFFF is the point. For depth 1.0, v * 16777215.0 + 0.5
// is 16777215.5, which binary32 rounds (ties to even) to 16777216: one past
// 24 bits, so `<< 8` wrapped the far plane to 0, the nearest depth. The game
// clears its 1600x1600 sun shadow map to 1.0, so every texel without a
// caster read "nearest" and every receiver under it was in shadow: dark
// roads, cars without their sun, on every device and setting from the
// first Vulkan renderer (2f796ec) to v0.1.0. 1.0 is the only binary32
// input that overflows, whether the multiply and the add round separately
// or a driver fuses them into one fma (SPIR-V allows that contraction):
// both give 16777216.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// packDepth24's body, in tokens that are both GLSL and (with Depth24Glsl's
// definitions of min, uint, clamp, * and +) C++. Its literals must be exact
// in binary32: GLSL reads them as float, Depth24Glsl rounds C++'s double.
#define SB_DEPTH24_PACK_EXPR(v) min(uint(clamp(v, 0.0, 1.0) * 16777215.0 + 0.5), 0xFFFFFFu)
#define SB_DEPTH24_STRINGIFY_(...) #__VA_ARGS__
#define SB_DEPTH24_STRINGIFY(...) SB_DEPTH24_STRINGIFY_(__VA_ARGS__)
// The definition as the shader has it, on a line of its own:
//   uint packDepth24(float v) { return min(uint(clamp(v, 0.0, 1.0) * 16777215.0 + 0.5), 0xFFFFFFu); }
#define GLSL_DEPTH24_PACK_DEFINITION "uint packDepth24(float v) { return " SB_DEPTH24_STRINGIFY(SB_DEPTH24_PACK_EXPR(v)) "; }"
#define GLSL_DEPTH24_PACK "\n" \
    "// 24-bit unsigned normalized depth, clamped: 1.0 rounds to 2^24 otherwise\n" \
    "// (gpu/depth24_glsl.h).\n" \
    GLSL_DEPTH24_PACK_DEFINITION "\n"

namespace gpu
{
    // How the shader's `v * 16777215.0 + 0.5` may be evaluated: rounded
    // after the multiply and again after the add (SPIR-V OpFMul, OpFAdd),
    // or rounded once (a driver may contract the two into an fma; SPIR-V
    // allows it unless NoContraction is set, and glslang doesn't set it).
    enum class Depth24Rounding { Separate, Fused };

    // SB_DEPTH24_PACK_EXPR's tokens as GLSL reads them, in binary32, for the
    // C++ mirror. Only what the expression uses is here: an expression that
    // needs more stops compiling until its GLSL meaning is added.
    template <Depth24Rounding R>
    struct Depth24Glsl
    {
        struct Float { float f; };
        struct Product { float a, b; };  // a * b: rounded on its own (Separate) or only with the add (Fused)
        // GLSL's uint(float) truncates. Outside uint's range (reachable only
        // without the clamp) it is undefined; this saturates, as GPUs do.
        struct uint
        {
            uint32_t u;
            explicit uint(Float x) : u(!(x.f > 0.0f) ? 0u : x.f >= 4294967296.0f ? 0xFFFFFFFFu : uint32_t(x.f)) {}
            operator uint32_t() const { return u; }
        };
        // GLSL's clamp is min(max(x, lo), hi), with a NaN undefined:
        // fmax/fmin return the number, as GPUs' max/min do.
        static Float clamp(Float x, double lo, double hi) { return { std::fmin(std::fmax(x.f, float(lo)), float(hi)) }; }
        static uint32_t min(uint32_t a, uint32_t b) { return std::min(a, b); }
        friend Product operator*(Float x, double y) { return { x.f, float(y) }; }
        friend Float operator+(Product p, double y)
        {
            if constexpr (R == Depth24Rounding::Fused)
                return { std::fmaf(p.a, p.b, float(y)) };
            else
            {
                volatile float product = p.a * p.b;  // (volatile: never contracted into an fma)
                return { product + float(y) };
            }
        }
        static uint32_t Pack(float value)
        {
            const Float v{ value };
            return SB_DEPTH24_PACK_EXPR(v);
        }
    };

    // GLSL_DEPTH24_PACK's packDepth24 in C++, from the shader's own tokens:
    // the 24-bit depth for `v` (the resolve stores it shifted left by 8).
    // Negatives, -0 and NaN give 0, 1.0 and above 0xFFFFFF.
    inline uint32_t PackDepth24(float v, Depth24Rounding rounding = Depth24Rounding::Separate)
    {
        return rounding == Depth24Rounding::Fused ? Depth24Glsl<Depth24Rounding::Fused>::Pack(v)
                                                  : Depth24Glsl<Depth24Rounding::Separate>::Pack(v);
    }

    // The depth a 24-bit value stands for, as the untile (convert 4) and the
    // resolve's store into a cached texture compute it from a word's top 24
    // bits: float(d) / 16777215.0. (Correctly rounded here; GLSL allows a
    // division 2.5 ULP of error.) 0xFFFFFF is exactly 1.0.
    inline float UnpackDepth24(uint32_t d24)
    {
        return float(d24 & 0xFFFFFFu) / 16777215.0f;
    }
}
