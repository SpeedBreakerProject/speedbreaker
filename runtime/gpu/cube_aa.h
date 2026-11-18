// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The samples the game asks for on the car's environment cube
// (gpu/renderer.cpp; header only, tests/cube_aa_test.cpp).
//
// The game renders the six 256x256 faces of the car's environment cube on a
// surface of its own (pitch 280, colour at EDRAM base 0, depth at 1824) with
// 4x MSAA, and resolves each face with the four-sample average
// (RB_COPY_CONTROL 0x00100360) into its slice of the cube texture. The car
// body samples that cube, and so does the rear-view mirror: one HUD quad
// that shows the cube 352x92 pixels large at 1280x720. The renderer drew
// every target with one sample per pixel and scaled only the scene's
// surfaces (pitch 640-1280) with Internal Resolution, so the faces had no
// antialiasing at all: the mirror's stair-stepped edges (issue #89).
//
// NFSMW_CUBE_AA (default ss, operator 2026-11-13; off: as before v0.2.6)
// gives that surface its four samples a pixel back, either way without
// changing what its readers see (a 256x256 face in guest memory and in the
// cached cube texture):
//   ss    a factor of its own, 2x2 host pixels a guest pixel whatever
//         Internal Resolution is (as the sun shadow's own factor): the
//         existing scaled resolve averages them. Shades four times the
//         pixels; an ordered grid.
//   msaa  real 4x multisampling on the host: a 4-sample image beside the
//         target's own, which each pass resolves into (the average). Shades
//         once a pixel, as the console; the driver's sample pattern.
// Both also size the target as the console's surface (below) instead of the
// 1280-wide target untiled rendering gives MSAA surfaces: its draws never
// leave the face, and four samples a pixel then cost no more memory than
// the wide target did.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace gpu::cube_aa
{
    enum class Mode : uint32_t { Off, Supersample, Msaa };

    // The cube's surface pitch and face size (guest pixels).
    inline constexpr uint32_t kPitch = 280;
    inline constexpr uint32_t kFace = 256;
    // Samples a guest pixel either way (the game's 4x).
    inline constexpr uint32_t kSamples = 4;
    // Supersample: host pixels a guest pixel on each axis.
    inline constexpr uint32_t kFactor = 2;

    // NFSMW_CUBE_AA's value: false when it is none of the names (`out` is
    // then left as it was). Empty: Off. (Unset, the renderer keeps its
    // default, ss.)
    inline bool ParseMode(const char* text, Mode& out)
    {
        if (!text || !*text || !strcmp(text, "0") || !strcmp(text, "off"))
        {
            out = Mode::Off;
            return true;
        }
        if (!strcmp(text, "ss"))
        {
            out = Mode::Supersample;
            return true;
        }
        if (!strcmp(text, "msaa"))
        {
            out = Mode::Msaa;
            return true;
        }
        return false;
    }

    constexpr const char* ModeName(Mode mode)
    {
        return mode == Mode::Supersample ? "ss" : mode == Mode::Msaa ? "msaa" : "off";
    }

    // Whether a target is the cube's: its pitch, made for a surface under
    // guest MSAA with the window offset on (the renderer's `wide` targets;
    // one made at the same pitch without MSAA is another target and stays as
    // it is).
    constexpr bool Applies(Mode mode, uint32_t pitch, bool msaaSurface)
    {
        return mode != Mode::Off && msaaSurface && pitch == kPitch;
    }

    // Guest pixel rows of a surface at EDRAM tile `base`: what fits in the
    // 2048 tiles (80x16 samples each) after it, where a row of `pitch`
    // pixels is `pitch` samples wide without MSAA and at 2x, twice that at
    // 4x, and a pixel is two sample rows at 2x and 4x (`msaaLog2`: 0, 1, 2).
    // The cube's depth at base 1824: 224 tiles / 7 a row = 32 tile rows =
    // 512 sample rows = 256 pixel rows, exactly a face.
    constexpr uint32_t SurfaceRows(uint32_t base, uint32_t pitch, uint32_t msaaLog2, bool wide64 = false)
    {
        const uint32_t samplesX = pitch * (msaaLog2 >= 2 ? 2u : 1u);
        const uint32_t tilesPerRow = std::max(1u, (samplesX + 79) / 80) * (wide64 ? 2u : 1u);
        const uint32_t tileRows = (2048u - std::min(base, 2047u)) / tilesPerRow;
        return std::clamp(tileRows * 16 / (msaaLog2 >= 1 ? 2u : 1u), 16u, 2048u);
    }

    // The cube target's guest size with the mode on: its pitch wide (not the
    // untiled 1280) and no taller than the console's surface (`rows`: the
    // renderer's own count, which takes the pitch for samples).
    struct Size
    {
        uint32_t width, height;
    };
    constexpr Size TargetSize(uint32_t base, uint32_t pitch, uint32_t msaaLog2, bool wide64, uint32_t rows)
    {
        return { pitch, std::min(rows, SurfaceRows(base, pitch, msaaLog2, wide64)) };
    }

    // Host samples a pixel of the target's attachment image.
    constexpr uint32_t HostSamples(Mode mode)
    {
        return mode == Mode::Msaa ? kSamples : 1u;
    }

    // An occlusion query counts host samples; the game expects its own
    // (guest pixels x its MSAA). What one counted sample stands for, as
    // num/den: the guest's samples a pixel over the host's (host pixels a
    // guest pixel x host samples a pixel). With one host sample the pair is
    // the renderer's as before (unreduced); with more it is reduced, so 4x
    // over 4x at 1x is 1/1.
    struct Weight
    {
        uint32_t num, den;
    };
    constexpr Weight OcclusionWeight(uint32_t msaaLog2, uint32_t hostSamples, uint32_t pixelsNum, uint32_t pixelsDen)
    {
        Weight w{ (1u << msaaLog2) * pixelsDen, pixelsNum * hostSamples };
        if (hostSamples > 1)
        {
            uint32_t a = w.num, b = w.den;
            while (b)
            {
                const uint32_t t = a % b;
                a = b;
                b = t;
            }
            w.num /= a;
            w.den /= a;
        }
        return w;
    }

    // Host pixels shaded for `faces` whole faces (a frame draws six), and
    // the samples their colour and depth hold.
    constexpr uint64_t ShadedPixels(Mode mode, uint32_t faces = 6)
    {
        const uint64_t side = uint64_t(kFace) * (mode == Mode::Supersample ? kFactor : 1u);
        return side * side * faces;
    }
    constexpr uint64_t StoredSamples(Mode mode, uint32_t faces = 6)
    {
        return ShadedPixels(mode, faces) * HostSamples(mode);
    }
}
