// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Deferred shadow resolves (renderer.cpp, ShadowLend): the decisions, apart
// from Vulkan, so tests/shadow_lend_test.cpp can check them.
//
// NFSMW's sun shadow map is a 1600x1600 D24S8 EDRAM surface (pitch 1600,
// EDRAM base 48). Each frame the game draws the world's casters into it and
// resolves it to the texture at 03A8A000, draws the cars on top, resolves it
// to 0444E000, and clears it (the resolve's depth clear) for the next frame.
// Every resolve was a compute pass reading the 10 MB depth image and writing
// 10 MB of guest memory plus the 10 MB R32F texture the scene samples. On a
// tile GPU (Mali, Apple) that is ~30 MB of memory traffic and a barrier per
// map, for data nobody but that texture reads.
//
// A deferred resolve records nothing: the texture samples the target's depth
// image in place (it is lent), and the resolve runs later only if something
// needs what it would have written:
//   - a pass about to draw into the target without clearing all of it first
//     (the cars' pass, after the world's resolve): the resolve runs then,
//     exactly as it would have (texture and guest memory), and the target
//     keeps its image;
//   - a pass that clears the whole target (the next frame's world pass): the
//     target takes a fresh image instead, the texture keeps sampling the old;
//   - a texture load, a resolve or a scaled-memory fill touching its guest
//     memory, or a vertex fetch from it: the guest memory part runs then;
//   - a write into the texture's own image (a load, another resolve into it):
//     the texture part runs then, unless that write covers all of it.
// A CPU store into the range after the resolve wins: its guest part is then
// dropped (logged), never written over the store.
//
// What the scene samples differs from the resolve's texels by the 24-bit
// quantization only: the resolve stores packDepth24(d) / 16777215 (depth24_glsl.h),
// the lent image d itself, at most half a 24-bit step apart (3e-8).
// NFSMW_SHADOW_VERIFY=1 checks that on the GPU for every deferred resolve.
//
// Only the second map's resolve goes away each frame: the cars' pass loads
// the world's depth, so the first map's resolve runs before it (as before,
// just later). NFSMW_SHADOW_COPY=1 (an experiment) instead copies the
// target's image for the cars' pass and keeps both resolves deferred; on an
// M1 Pro the copy cost what the resolve did. NFSMW_SHADOW_DEFER=0 turns it
// all off; =alt alternates 120-frame windows for A/B in one run.
#pragma once
#include <array>
#include <cstdint>

namespace gpu::shadow_lend
{
    // What a deferred resolve needs to know about the resolve and its texture.
    struct Candidate
    {
        bool depthSource = false;  // a depth resolve
        uint32_t format = 0;       // the resolve's destination format (22: k_24_8)
        uint32_t bppLog2 = 0;      // destination bytes per pixel, log2
        bool scaledSource = false; // the source target isn't 1x (Internal Resolution)
        bool scaledWrite = false;  // the resolve also writes scaled memory
        bool fusedExact = false;   // the resolve stores into a cached texture of exactly its surface (not a partial fusion)
        uint32_t texConvert = 0;   // that texture's untile conversion (4: D24 -> float)
        bool texCube = false;
        uint32_t texLevels = 1, texLayers = 1;
        uint32_t texWidth = 0, texHeight = 0;
        uint32_t srcWidth = 0, srcHeight = 0;  // the source target's image (host pixels)
        int32_t rectX0 = 0, rectY0 = 0;        // the resolve's rectangle in the destination (guest pixels)
        uint32_t rectWidth = 0, rectHeight = 0;
        int32_t srcOriginX = 0, srcOriginY = 0;  // where it is in the source target (host pixels)
    };

    // Why `c` can't be deferred, or nullptr: the texture's texel (x, y) must
    // be the target's pixel (x, y) for every texel, sampled with the same
    // normalized coordinates (the same size), from 24-bit depth stored as
    // float (convert 4), a single 2D level, and the resolve's every other
    // effect (scaled memory) absent.
    inline const char* Ineligible(const Candidate& c)
    {
        if (!c.depthSource || c.format != 22 || c.bppLog2 != 2)
            return "not a 24-bit depth resolve";
        if (c.scaledSource || c.scaledWrite)
            return "scaled";
        if (!c.fusedExact)
            return "no exact cached texture";
        if (c.texConvert != 4 || c.texCube || c.texLevels != 1 || c.texLayers != 1)
            return "texture layout";
        if (c.texWidth != c.srcWidth || c.texHeight != c.srcHeight)
            return "texture and target sizes differ";
        if (c.rectX0 != 0 || c.rectY0 != 0 || c.srcOriginX != 0 || c.srcOriginY != 0)
            return "rectangle not at the origin";
        if (c.rectWidth < c.texWidth || c.rectHeight < c.texHeight)
            return "rectangle doesn't cover the texture";
        return nullptr;
    }

    // The guest bytes a resolve writes: its destination, pitch, format and
    // rectangle (ResolveExtent's tiles, and the pixels in them it writes).
    struct WriteSet
    {
        uint32_t destBase = 0, destPitch = 0, format = 0;
        int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        bool operator==(const WriteSet&) const = default;
    };

    // A later resolve writing exactly the same bytes supersedes a deferred
    // one's guest part (it would have been overwritten whole); any other
    // overlap needs it written first.
    enum class Overlap { kNone, kSuperseded, kRunFirst };
    inline Overlap ResolveOverlap(const WriteSet& owed, uint32_t owedBase, uint32_t owedBytes, const WriteSet& next,
        uint32_t nextBase, uint32_t nextBytes)
    {
        if (uint64_t(nextBase) >= uint64_t(owedBase) + owedBytes || uint64_t(owedBase) >= uint64_t(nextBase) + nextBytes)
            return Overlap::kNone;
        return owed == next ? Overlap::kSuperseded : Overlap::kRunFirst;
    }

    // A pass about to draw into a target whose image is lent: a fresh image
    // when it clears all of the target first (nothing of the old is read),
    // else the deferred resolves run now, before the image changes.
    enum class TargetWrite { kInPlace, kFreshImage, kRunDeferred };
    inline TargetWrite BeforeTargetWrite(bool lent, bool clearsWhole)
    {
        if (!lent)
            return TargetWrite::kInPlace;
        return clearsWhole ? TargetWrite::kFreshImage : TargetWrite::kRunDeferred;
    }

    // A depth view standing in for an R32F texture: what each component of
    // the guest's swizzle (3 bits each, x y z w: 0-3 red..alpha, 4 zero,
    // 5 one, 6-7 the component itself) samples from an R32F texture is red,
    // zero, zero or one, so the view names only R (the depth), ZERO and ONE:
    // a depth view's green, blue and alpha aren't the same everywhere.
    enum class Component { kR, kZero, kOne };
    inline std::array<Component, 4> DepthSwizzle(uint32_t swizzle)
    {
        auto source = [](uint32_t s, uint32_t self) {
            switch (s & 7)
            {
            case 0: return Component::kR;
            case 1: case 2: case 4: return Component::kZero;
            case 3: case 5: return Component::kOne;
            default: return self == 0 ? Component::kR : self == 3 ? Component::kOne : Component::kZero;
            }
        };
        return { source(swizzle, 0), source(swizzle >> 3, 1), source(swizzle >> 6, 2), source(swizzle >> 9, 3) };
    }
}
