// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// NFSMW_BLOOM (GitHub #106; unset: auto; gpu/renderer.cpp; header only,
// tests/bloom_test.cpp): a finer bloom with the same glow.
//
// Most Wanted draws its bloom and luminance chain into small surfaces after
// the scene (a race frame, 1x and every Internal Resolution alike):
//   320x180  ps_387140822b5b9e76  16 taps of the resolved scene: colour (C0,
//                                 pitch 320 at EDRAM 0) and a second target
//                                 (C1, pitch 320 at 48)
//   64x64    ps_b50c748082de9e73  luminance (pitch 80), read back for exposure
//   320x180  ps_9c442da6825c478a  bright pass (one tap)
//   320x180  ps_409b8ca6d426bef3  13-tap blur
//   160x90   ps_409b8ca6d426bef3  13-tap blur, into pitch 160
//   160x90   ps_7f3b3ab6c71dc07e  16-tap blur, twice
// then the full-screen combine (ps_d52a5eb6c17f9598, five textures) into the
// scene's 1280x720 surface. Every tap's offset is a pixel shader constant
// added to the interpolated texture coordinate: in the 360's texture units
// (UV), not host texels. Internal Resolution scales only surfaces of pitch
// 640-1280 (the scene, the composite), so the chain stays at the 360's size
// and the combine magnifies it 4x (1x), 8x (2x) or 12x (3x) on each axis.
//
// Modes (off: the chain as every release before v0.2.7 drew it; unset: auto):
//   scale      the chain's colour surfaces of pitch 160 and 320 get the
//              scene's scale (its 64x64 luminance stays 1x, and so does what
//              the CPU reads back). The tap offsets stay the game's, in UV, so
//              each blur keeps its radius in the 360's units and the taps
//              simply land on more pixels. The chain's first pass (the 16-tap
//              downsample of the scene) still reads the scene's 1x copy, as on
//              the 360: its 16 bilinear taps were laid out for 1280 texels.
//   scalefull  the same, but that first pass reads the scene at host
//              resolution (as every other draw into a scaled target would):
//              its 16 taps then cover the 360's area with fewer of the host's
//              texels.
//   filter     the combine reads the chain's small textures through a cubic
//              B-spline (four bilinear taps) instead of one bilinear tap: no
//              texel grid at 4-12x magnification; adds 0.41 of a bloom texel
//              of blur (sigma), for the 1/6 texel^2 between the two kernels.
//   both       scale and filter.
//   fill       the chain's first pass reads the whole of the area it covers.
//              Its 16 taps sit 4 scene texels apart and each is one bilinear
//              read on a texel corner: 2x2 texels of every 4x4, so a quarter
//              of the 16x16 texels a pixel covers, and a thin pole or wire
//              counts double or not at all by where it stands (in motion: the
//              darkening around it comes and goes). With fill each tap is
//              four reads, one scene texel out on each diagonal: the 16 taps
//              then tile the 16x16 texels, the same area and weight as on the
//              360, without the holes. The chain stays 1x.
//   fillfilter, fillscale, all
//              fill with filter, with scale, with both.
//   alt        a test aid (as NFSMW_CUBE_AA=alt): off, scale, filter, both,
//              fill, fillfilter, fillscale, all in turn, a [perf] window (120
//              frames; NFSMW_BLOOM_ALT_FRAMES=<n> for another) each, so one
//              run shows the same scene under all of them.
//   auto       fillfilter where Internal Resolution is 2x or more, fill
//              below, and fill at any scale on a Steam Deck (AutoMode: the
//              rule, its threshold and what it reads). The default
//              (kAutoByDefault).
// NFSMW_BLOOM_PS=<hash>[,<hash>...] (16 hex digits each, "ps_" optional):
// the combine shaders the filter applies to, in place of the default two
// (kCombines): ps_d52a5eb6c17f9598, which draws a race frame, the menus'
// garage and the picture under the pause menu, and ps_74c891970bd63fca,
// which draws the camera shots (before a race, the career's opening): the
// same combine with depth of field (a half-size picture of the scene in slot
// 4, blended in by depth; not the chain's, and neither the filter's nor the
// fill's) and the bloom behind a flag. Both read the chain alike: slot 1's
// alpha, the 320x180 luminance, for the tint, and slot 3's colour, the
// 160x90 bloom, added with a constant gain, at the picture's own texture
// coordinate.
#pragma once

#include <gpu/render_scale.h>
#include <video/steam_device.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace gpu::bloom
{
    enum Mode : uint32_t
    {
        kOff = 0,
        kScale = 1,      // the chain's surfaces at the scene's scale
        kFilter = 2,     // the combine's B-spline reads
        kSceneHost = 4,  // with kScale: the first pass reads the scene at host resolution
        kFill = 8,       // the first pass's taps cover their whole 4x4 texels
    };

    inline constexpr uint32_t kAltWindow = 120;  // frames a mode runs under alt (one [perf] window), unless told
    inline constexpr uint32_t kAltCount = 8;
    inline constexpr uint32_t kAltOrder[kAltCount] = { kOff, kScale, kFilter, kScale | kFilter, kFill, kFill | kFilter, kFill | kScale,
        kFill | kScale | kFilter };

    // The combines the filter covers unless NFSMW_BLOOM_PS names others
    // (NTSC-U and PAL alike: shaders are disc data): a race frame's (the
    // menus and the pause menu are drawn with it too), and the camera shots'.
    inline constexpr uint64_t kCombinePs = 0xd52a5eb6c17f9598ull;
    inline constexpr uint64_t kShotCombinePs = 0x74c891970bd63fcaull;
    inline constexpr uint32_t kCombineCount = 2;
    inline constexpr uint64_t kCombines[kCombineCount] = { kCombinePs, kShotCombinePs };
    // The chain's first pass (the scene's 16-tap downsample into 320x180): kFill's shader.
    inline constexpr uint64_t kDownsamplePs = 0x387140822b5b9e76ull;
    // The chain's colour surface pitches that kScale scales (the 64x64
    // luminance, pitch 80, stays 1x: the CPU reads it back for exposure).
    inline constexpr uint32_t kPitches[2] = { 160, 320 };
    // The filter's slots: 2D textures as wide as the chain's surfaces (guest
    // texels: 320x180 and 160x90; not the combine's 256x256 vignette), read
    // with a linear mag filter, by a combine drawing into a target at least
    // kMinTargetWidth wide.
    inline constexpr uint32_t kMinTargetWidth = 1280;
    inline constexpr uint32_t kSlotBits = 5;  // the filter's slots: 0-4 (the combine reads five)

    inline const char* ModeName(uint32_t m)
    {
        switch (m)
        {
        case kOff: return "off";
        case kScale: return "scale";
        case kScale | kSceneHost: return "scalefull";
        case kFilter: return "filter";
        case kScale | kFilter: return "both";
        case kScale | kSceneHost | kFilter: return "bothfull";
        case kFill: return "fill";
        case kFill | kFilter: return "fillfilter";
        case kFill | kScale: return "fillscale";
        case kFill | kScale | kFilter: return "all";
        }
        return "?";
    }

    // NFSMW_BLOOM's value: false when it is none of the names (`mode`, `alt`
    // and `autoMode` are then left as they were). Empty or "0": off. "auto":
    // `mode` is off until the caller gives it AutoMode of the scale it has.
    inline bool ParseMode(const char* text, uint32_t& mode, bool& alt, bool& autoMode)
    {
        if (!text || !*text || !strcmp(text, "0") || !strcmp(text, "off"))
        {
            mode = kOff;
            alt = false;
            autoMode = false;
            return true;
        }
        if (!strcmp(text, "alt") || !strcmp(text, "auto"))
        {
            mode = kOff;
            alt = text[1] == 'l';
            autoMode = !alt;
            return true;
        }
        for (uint32_t m : { uint32_t(kScale), uint32_t(kScale | kSceneHost), uint32_t(kFilter), uint32_t(kScale | kFilter),
                 uint32_t(kScale | kSceneHost | kFilter), uint32_t(kFill), uint32_t(kFill | kFilter), uint32_t(kFill | kScale),
                 uint32_t(kFill | kScale | kFilter) })
            if (!strcmp(text, ModeName(m)))
            {
                mode = m;
                alt = false;
                autoMode = false;
                return true;
            }
        return false;
    }

    // NFSMW_BLOOM=auto: the mode for an Internal Resolution. The rule reads
    // the scale the scene's surfaces have (the renderer's resolved one: the
    // setting or Auto's pick within the device's budget, with a Vert+ or Hor+
    // picture's own rows or columns, split screen's cap, or
    // NFSMW_RENDER_SCALE), and again whenever that changes while the game
    // runs. Neither mode scales the chain, so a change needs no target made
    // again: each draw picks its reads.
    //   - kAutoFilterFrom, the one threshold: the filter from 2x on. The
    //     combine magnifies the chain 4 x the scale on each axis: 8x and more
    //     shows its texel grid as steps, and that is where the filter was
    //     looked at (2x; 3x by its 12x).
    //   - Both axes count: the smaller one is the setting's number (a Vert+
    //     or Hor+ picture has more on the axis it extends, never less: the
    //     Deck's 1x10/9 is 1x, 2x20/9 is 2x, Auto's 3x2 is 2x), and a scale
    //     with one axis below the threshold (Auto's 2x1 for a wide, low
    //     picture) keeps that axis's coarser steps whatever the filter does.
    //   - 1.5x (3/2: 6x) is below it: fill.
    //   - kAutoBelow: fill alone, which costs next to nothing; fillfilter
    //     there too once the filter is known to be affordable at 1x on the
    //     devices that run 1x.
    //   - Not on a Steam Deck (video/steam_device.h): kAutoBelow there at any
    //     scale. On its own screen (1x10/9) that is the rule's pick anyway;
    //     at 1920x1080 it renders 2x2, where it is GPU-bound below 60 with the
    //     switch off (52.7 and 57.8 fps in the two standing scenes), fill
    //     cost nothing measurable and the filter's 0.7-0.85 ms another
    //     1.9-2.6 fps.
    inline constexpr uint32_t kAutoFilterFrom = 2;
    inline constexpr uint32_t kAutoBelow = kFill;
    inline constexpr uint32_t kAutoFrom = kFill | kFilter;
    // NFSMW_BLOOM unset: auto (from v0.2.7). NFSMW_BLOOM=off draws the chain
    // as every release before did.
    inline constexpr bool kAutoByDefault = true;

    constexpr uint32_t AutoMode(const RenderScale& scale, video::SteamDevice device = video::SteamDevice::Other)
    {
        if (device == video::SteamDevice::Deck)
            return kAutoBelow;
        const bool from = scale.x.num >= kAutoFilterFrom * scale.x.den && scale.y.num >= kAutoFilterFrom * scale.y.den;
        return from ? kAutoFrom : kAutoBelow;
    }

    // Under alt: the mode for the frames after `frame` (switched in OnSwap,
    // every `window` frames: kAltWindow is in step with the [perf] line).
    constexpr uint32_t AltMode(uint64_t frame, uint32_t window = kAltWindow)
    {
        return kAltOrder[(frame / (window ? window : 1)) % kAltCount];
    }

    constexpr bool ScaledPitch(uint32_t mode, uint32_t pitch, bool depth)
    {
        if (!(mode & kScale) || depth)
            return false;
        for (uint32_t p : kPitches)
            if (p == pitch)
                return true;
        return false;
    }

    // A texture the chain's first pass reads (the scene: wider than any of the
    // chain's own surfaces), drawn into one of the chain's scaled surfaces:
    // under kScale (not kSceneHost) it is read at 1x, as on the 360.
    constexpr bool SceneReadAt1x(uint32_t mode, bool drawIntoChain, uint32_t textureWidth)
    {
        return (mode & kScale) && !(mode & kSceneHost) && drawIntoChain && textureWidth > kPitches[1];
    }

    // NFSMW_BLOOM_PS's list: up to 8 hashes; false when an entry isn't one.
    inline bool ParseShaderList(const char* text, uint64_t* out, uint32_t& count)
    {
        count = 0;
        if (!text)
            return true;
        const char* p = text;
        while (*p)
        {
            if (!strncmp(p, "ps_", 3))
                p += 3;
            uint64_t v = 0;
            int digits = 0;
            for (; *p && *p != ','; p++, digits++)
            {
                const char c = *p;
                const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
                if (d < 0 || digits >= 16)
                    return false;
                v = v << 4 | uint64_t(d);
            }
            if (digits == 0 || count == 8)
                return false;
            out[count++] = v;
            if (*p == ',')
                p++;
        }
        return true;
    }

    // Whether a fetch slot of the combine takes the filter: a 2D texture of
    // the chain (as wide as one of its surfaces), a linear mag filter (the
    // B-spline is built from bilinear taps), the draw's target the scene's.
    constexpr bool FilterSlot(uint32_t slot, uint32_t textureWidth, bool linearMag, uint32_t targetWidth)
    {
        return slot < kSlotBits && (textureWidth == kPitches[0] || textureWidth == kPitches[1]) && linearMag &&
               targetWidth >= kMinTargetWidth;
    }

    // Whether a fetch slot of the chain's first pass takes kFill: the scene
    // (a 2D texture wider than any of the chain's own surfaces), read with a
    // linear filter, the draw's target one of the chain's 320-wide surfaces.
    constexpr bool FillSlot(uint32_t slot, uint32_t textureWidth, bool linearMag, uint32_t targetPitch)
    {
        return slot < kSlotBits && textureWidth > kPitches[1] && linearMag && targetPitch == kPitches[1];
    }

    // kFill's read: four bilinear reads one texel out on each diagonal, their
    // mean. On a texel corner (where the first pass's taps land) that is the
    // 4x4 texels around it, each a sixteenth. Same arguments as sbBloomTex.
    inline const char* FillGlsl()
    {
        return "vec4 sbBloomBox(sampler2D s, vec2 uv, float bias)\n"
               "{\n"
               "    vec2 d = 1.0 / vec2(textureSize(s, 0));\n"
               "    return 0.25 * (texture(s, uv - d, bias) + texture(s, uv + vec2(d.x, -d.y), bias) +\n"
               "        texture(s, uv + vec2(-d.x, d.y), bias) + texture(s, uv + d, bias));\n"
               "}\n";
    }

    // The cubic B-spline read (GPU Gems 2, ch. 20): the 4x4 texels around the
    // point with the B-spline's weights, as four bilinear taps. Same arguments
    // as the translator's texture(t2d_N, uv, bias).
    inline const char* FilterGlsl()
    {
        return "vec4 sbBloomTex(sampler2D s, vec2 uv, float bias)\n"
               "{\n"
               "    vec2 size = vec2(textureSize(s, 0));\n"
               "    vec2 p = uv * size - 0.5;\n"
               "    vec2 i = floor(p);\n"
               "    vec2 f = p - i;\n"
               "    vec2 f2 = f * f, f3 = f2 * f;\n"
               "    vec2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;\n"
               "    vec2 w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;\n"
               "    vec2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0;\n"
               "    vec2 w3 = f3 / 6.0;\n"
               "    vec2 g0 = w0 + w1, g1 = w2 + w3;\n"
               "    vec2 h0 = (i - 0.5 + w1 / g0) / size;\n"
               "    vec2 h1 = (i + 1.5 + w3 / g1) / size;\n"
               "    vec4 t00 = texture(s, h0, bias), t10 = texture(s, vec2(h1.x, h0.y), bias);\n"
               "    vec4 t01 = texture(s, vec2(h0.x, h1.y), bias), t11 = texture(s, h1, bias);\n"
               "    return g0.y * (g0.x * t00 + g1.x * t10) + g1.y * (g0.x * t01 + g1.x * t11);\n"
               "}\n";
    }

    // The translation's reads of the slots in `mask` through `function`
    // (`texture(t2d_N, ` -> `sbBloomTex(t2d_N, `); returns how many it changed.
    inline uint32_t RewriteFetches(std::string& glsl, uint32_t mask, const char* function = "sbBloomTex")
    {
        uint32_t changed = 0;
        for (uint32_t slot = 0; slot < kSlotBits; slot++)
        {
            if (!((mask >> slot) & 1))
                continue;
            char from[32], to[48];
            snprintf(from, sizeof(from), "texture(t2d_%u, ", slot);
            snprintf(to, sizeof(to), "%s(t2d_%u, ", function, slot);
            const size_t fromLen = strlen(from), toLen = strlen(to);
            for (size_t at = glsl.find(from); at != std::string::npos; at = glsl.find(from, at + toLen))
            {
                glsl.replace(at, fromLen, to);
                changed++;
            }
        }
        return changed;
    }

    // Bytes of a chain surface's host image (32 bits a pixel: the chain is
    // k_8_8_8_8): `rows` guest rows (the renderer gives each surface what
    // fits in EDRAM from its base: 2048 for all of the chain's), `scaleNum /
    // scaleDen` host pixels a guest pixel on each axis, rounded up.
    constexpr uint64_t SurfaceBytes(uint32_t pitch, uint32_t rows, uint32_t scaleNum, uint32_t scaleDen)
    {
        const uint64_t w = (uint64_t(pitch) * scaleNum + scaleDen - 1) / scaleDen;
        const uint64_t h = (uint64_t(rows) * scaleNum + scaleDen - 1) / scaleDen;
        return w * h * 4;
    }
    // Pixels the chain's passes shade a frame at a scale (the race frame's
    // passes: 3 at 320x180 with the first one's two targets, 3 at 160x90;
    // the 64x64 stays 1x and isn't counted).
    constexpr uint64_t ChainPixels(uint32_t scaleNum, uint32_t scaleDen)
    {
        auto px = [&](uint64_t w, uint64_t h) { return ((w * scaleNum + scaleDen - 1) / scaleDen) * ((h * scaleNum + scaleDen - 1) / scaleDen); };
        return 3 * px(320, 180) + 3 * px(160, 90);
    }
}
