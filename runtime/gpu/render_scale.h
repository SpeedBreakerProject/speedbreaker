// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The internal resolution as a ratio: on each axis `num` host pixels for
// every `den` guest pixels. 1x is 1/1, 2x 2/1, 3x 3/1, and 1.5x is 3/2:
// 1280x720 becomes 1920x1080. Vulkan-free; gpu/renderer.cpp uses it for
// every guest <-> host pixel mapping, and tests/render_scale_test.cpp checks
// it alone.
//
// Whole scales map guest pixel g to host pixels [g s, g s + s). At 3/2 a
// guest pixel is a pixel and a half, so the mapping works on blocks: every
// two guest pixels (an even one and the odd one after it) are exactly three
// host pixels. On each axis:
//   - Edges: guest edge g is host edge floor(g num / den) (HostEdge). A
//     guest rectangle's host one is its edges', so rectangles that share a
//     guest edge share the host edge, with no gap and no overlap: draw
//     scissors, clears, pass areas and what a resolve reads all use it. It is
//     also the rasteriser's own rule for a pixel-aligned quad (a host pixel
//     whose centre lies exactly on an odd guest edge belongs to the pixel
//     after it, the top-left rule), so a scissor and the quad it clips agree.
//     Even guest coordinates land exactly (g 3/2 is whole); the game's
//     resolves are 8-pixel aligned and its tile offsets even, so every
//     resolve, clear of a resolve and tile band maps exactly.
//   - Ownership: guest pixel g owns host pixels [HostEdge(g), HostEdge(g+1)):
//     at 3/2 an even pixel owns one, the odd one after it two. Each host
//     pixel has exactly one owner (sbOwner).
//   - The 1x average a resolve writes into guest memory weighs every host
//     pixel by how much of it lies inside the guest pixel (sbOverlap: a box
//     filter, 2/3 and 1/3 at 3/2), so the mean over any whole number of
//     blocks is the host mean, and an edge between two colours lands where
//     it was. Whole scales give every host pixel weight 1, as before.
//   - A depth resolve keeps one sample per guest pixel: the first at a whole
//     scale (as before), at 3/2 the host pixel holding the guest pixel's
//     centre (sbCenterHost).
//
// The scaled resolve memory (renderer.cpp's ScaledMemory) keeps each host
// sample of a 32-bit tiled surface, found from the guest address of the texel
// that owns it, so a resolve (the writer) and a texture or the front buffer
// (the readers) meet whatever the pitch and base of each. In Xenos's 32-bit
// tiling a texel's word in its 4 KB page has x & 1 in bit 0 and y & 1 in bit 2
// (the other bits depend only on x >> 1 and y >> 1), so a 2x2 block of texels
// at even (x, y) is the words w, w + 1, w + 4, w + 5 of one page, whatever the
// pitch. A page's slot holds its blocks in order of their word with the
// sub-texel bits taken out (sbBlockIndex), each block's num.x x num.y host
// samples row by row. At 3/2: 256 blocks of 9 samples, 9 KB a 4 KB page; at
// a whole scale a block is one texel and this is the layout from before
// (word x s + j x sx + i).
//
// So 1.5x only covers surfaces whose texels come in whole blocks: a scaled
// texture or front buffer needs an even width and height at 3/2 (Exact),
// else it is used at 1x; a resolve needs an even origin, which the 8-pixel
// alignment gives (else it isn't kept at host resolution, logged once).
//
// Native rows and columns (GitHub #34): on a screen of another shape the
// axis a Vert+ or Hor+ picture extends gets the host pixels the screen has
// there, so a Steam Deck's 1280x800 renders 1280x800 at 1x, not 720 rows
// stretched: 800 rows for 720 is 10/9, and a den above 2 (9; 3 for 5/3 at
// 1.5x; 720 for a MacBook's 803 rows) is a texel axis. Its edges are where
// the rasteriser puts them for any ratio: guest edge g is host edge
// ceil(g num / den - 1/2), the first host pixel whose centre lies at or past
// it (sbHostEdge; the same floor(g num / den) for a den of 1 or 2), and guest
// pixel g owns the host pixels whose centres lie inside it, one or two at
// 10/9. A texel axis has no blocks: each texel keeps ceil(num / den) samples
// along it (sbStride; 2 at 10/9, 8 KB a 4 KB page), the ones it owns first,
// the rest repeating its last (a reader that disagrees about where a texel's
// samples end still reads that texel, never another's or garbage). Which
// texel owns which host pixels depends on the texel's coordinate, not on
// bits of its word, so a resolve counts it in the source target (the
// screen's coordinates in untiled rendering: tile strips at rows 256 and 512
// are 284 and 569 host rows down at 10/9), and so does a texture or the
// front buffer reading the surface from its own first texel: the scene's
// strips, its side-by-side halves and the right-half texture read back
// sample for sample (render_scale_test). Any surface size or resolve origin
// works (Exact is always true there).
//
// The block arithmetic is written once, in tokens that are both GLSL and
// C++ (SB_SCALE_FUNCTIONS, as gpu/depth24_glsl.h does for its pack): the shaders
// get it as text (GLSL_SCALE_CODE, spliced into renderer.cpp's resolve,
// fill, untile and front-buffer shaders) and the test runs the same code.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

// uint arithmetic only (wrapping 32-bit in both languages); `1u` literals,
// min/max of two uints, if, ?:. Every function's name starts "sb".
#define SB_SCALE_FUNCTIONS                                                                                       \
    /* Host edge of guest edge g: floor(g num / den); for a den above 2 (a texel axis) the rasteriser's, */       \
    /* ceil(g num / den - 1/2), the same for a den of 1 or 2. */                                                 \
    uint sbHostEdge(uint g, uint num, uint den)                                                                  \
    {                                                                                                            \
        return den > 2u ? (2u * g * num + den - 1u) / (2u * den) : g * num / den;                                \
    }                                                                                                            \
    /* The host pixels touching guest pixels [0, g): ceil(g num / den) (a box filter's reach). */                \
    uint sbHostSize(uint g, uint num, uint den) { return (g * num + den - 1u) / den; }                            \
    /* The guest edge whose host edge is h (h from sbHostEdge). */                                               \
    uint sbGuestEdge(uint h, uint num, uint den)                                                                 \
    {                                                                                                            \
        return den > 2u ? (2u * h * den + 2u * num - den) / (2u * num) : (h * den + num - 1u) / num;             \
    }                                                                                                            \
    /* The guest pixel owning host pixel h: the g with HostEdge(g) <= h < HostEdge(g + 1). */                     \
    uint sbOwner(uint h, uint num, uint den)                                                                     \
    {                                                                                                            \
        return den > 2u ? ((2u * h + 1u) * den) / (2u * num) : ((h + 1u) * den + num - 1u) / num - 1u;            \
    }                                                                                                            \
    /* How much of host pixel h lies in guest pixel g, in 1/den of a pixel (num for the whole guest pixel). */   \
    uint sbOverlap(uint g, uint h, uint num, uint den)                                                           \
    {                                                                                                            \
        uint lo = max(g * num, h * den);                                                                         \
        uint hi = min((g + 1u) * num, (h + 1u) * den);                                                           \
        return hi > lo ? hi - lo : 0u;                                                                           \
    }                                                                                                            \
    /* The one host pixel a depth resolve reads for guest pixel g. */                                            \
    uint sbCenterHost(uint g, uint num, uint den) { return den == 1u ? g * num : ((2u * g + 1u) * num) / (2u * den); } \
    /* A block's samples along an axis: num, or on a texel axis ceil(num / den), a texel's most. */              \
    uint sbStride(uint num, uint den) { return den > 2u ? (num + den - 1u) / den : num; }                         \
    /* A block's texels along an axis: den, or 1 on a texel axis. */                                             \
    uint sbLayoutDen(uint den) { return den > 2u ? 1u : den; }                                                   \
    /* A word of a 4 KB page of 32-bit tiled texels (0..1023): its block (the sub-texel bits out). */             \
    uint sbBlockIndex(uint w, uint denX, uint denY)                                                              \
    {                                                                                                            \
        if (denY == 2u)                                                                                          \
            w = (w & 3u) | ((w >> 3u) << 2u);                                                                    \
        if (denX == 2u)                                                                                          \
            w = w >> 1u;                                                                                         \
        return w;                                                                                                \
    }                                                                                                            \
    /* A block's first word (its texel at even x and y). */                                                      \
    uint sbBlockWord(uint b, uint denX, uint denY)                                                               \
    {                                                                                                            \
        if (denX == 2u)                                                                                          \
            b = b << 1u;                                                                                         \
        if (denY == 2u)                                                                                          \
            b = (b & 3u) | ((b >> 2u) << 3u);                                                                    \
        return b;                                                                                                \
    }                                                                                                            \
    /* A word's texel within its block: x & 1 (bit 0) and y & 1 (bit 2), at 3/2 only. */                         \
    uint sbSubX(uint w, uint denX) { return denX == 2u ? (w & 1u) : 0u; }                                        \
    uint sbSubY(uint w, uint denY) { return denY == 2u ? ((w >> 2u) & 1u) : 0u; }                                \
    /* A 4 KB page's samples: its blocks' x theirs. */                                                           \
    uint sbSlotWords(uint numX, uint numY, uint denX, uint denY)                                                 \
    {                                                                                                            \
        return 1024u / (sbLayoutDen(denX) * sbLayoutDen(denY)) * (sbStride(numX, denX) * sbStride(numY, denY));  \
    }                                                                                                            \
    /* Host sample (i, j) of word w's block, i and j from the block's corner (below its strides): its word in the slot. */ \
    uint sbSampleWord(uint w, uint i, uint j, uint numX, uint numY, uint denX, uint denY)                        \
    {                                                                                                            \
        return sbBlockIndex(w, denX, denY) * (sbStride(numX, denX) * sbStride(numY, denY)) + j * sbStride(numX, denX) + i; \
    }                                                                                                            \
    /* A reader's host pixel h: the first texel of the block holding it (h / num x den: the block's corner), */  \
    /* or on a texel axis its owner. */                                                                          \
    uint sbBlockTexel(uint h, uint num, uint den) { return den > 2u ? sbOwner(h, num, den) : h / num * den; }     \
    /* ... and its sample there, from the block's corner. */                                                     \
    uint sbBlockSample(uint h, uint num, uint den)                                                               \
    {                                                                                                            \
        return den > 2u ? h - sbHostEdge(sbOwner(h, num, den), num, den) : h - h / num * num;                    \
    }                                                                                                            \
    /* A writer's samples [lo, hi) of the texel at sub in its block: the host pixels it owns, from the */        \
    /* block's corner; on a texel axis all its stride (past the ones it owns, its last again). */                \
    uint sbSampleLo(uint sub, uint num, uint den) { return den > 2u ? 0u : sbHostEdge(sub, num, den); }          \
    uint sbSampleHi(uint sub, uint num, uint den) { return den > 2u ? sbStride(num, den) : sbHostEdge(sub + 1u, num, den); } \
    /* Where sample i (from sbSampleLo) of guest pixel g (sub in its block) is in a target counted from guest */ \
    /* pixel 0: on a texel axis its i-th host pixel, or its last one past them. */                               \
    uint sbSampleHost(uint g, uint sub, uint i, uint num, uint den)                                              \
    {                                                                                                            \
        if (den > 2u)                                                                                            \
            return sbHostEdge(g, num, den) + min(i, sbHostEdge(g + 1u, num, den) - sbHostEdge(g, num, den) - 1u); \
        return sbHostEdge(g - sub, num, den) + i;                                                                \
    }                                                                                                            \
    /* A writer's sample i (from sbSampleLo) of a reader's texel t (sub in its block, t counted from the */      \
    /* reader's own first texel): the reader's host pixel the untile fills with it, or ~0u for a texel */        \
    /* axis's repeated padding past the host pixels t owns (a resolve storing straight into a texture). */       \
    uint sbReaderHost(uint t, uint sub, uint i, uint num, uint den)                                              \
    {                                                                                                            \
        if (den > 2u)                                                                                            \
            return i < sbHostEdge(t + 1u, num, den) - sbHostEdge(t, num, den) ? sbHostEdge(t, num, den) + i : ~0u; \
        return sbHostEdge(t - sub, num, den) + i;                                                                \
    }

#define SB_SCALE_STRINGIFY_(...) #__VA_ARGS__
#define SB_SCALE_STRINGIFY(...) SB_SCALE_STRINGIFY_(__VA_ARGS__)
// The functions as the shaders have them (comments are gone by now).
#define GLSL_SCALE_CODE "\n// Guest <-> host pixels at the internal resolution (gpu/render_scale.h).\n" \
    SB_SCALE_STRINGIFY(SB_SCALE_FUNCTIONS) "\n"

namespace gpu
{
    // SB_SCALE_FUNCTIONS as C++, the shaders' own tokens: member functions
    // (defined in the class, so inline), called on a temporary, ScaleGlsl{}.
    struct ScaleGlsl
    {
        using uint = uint32_t;
        static uint min(uint a, uint b) { return a < b ? a : b; }
        static uint max(uint a, uint b) { return a > b ? a : b; }
        SB_SCALE_FUNCTIONS
    };

    // One axis: num host pixels for every den guest pixels, reduced (den 1
    // or 2: blocks; above 2: a texel axis, num / den above 1).
    struct AxisScale
    {
        uint32_t num = 1, den = 1;

        bool operator==(const AxisScale&) const = default;
        bool IsOne() const { return num == den; }
        // No blocks: each texel's samples apart (a den above 2).
        bool Texel() const { return den > 2; }
        float Value() const { return float(num) / float(den); }
        uint32_t Edge(uint32_t g) const { return ScaleGlsl{}.sbHostEdge(g, num, den); }
        // The host pixels of g guest ones (a target's or image's size): on a
        // texel axis those the g own, so every host pixel has an owner.
        uint32_t Size(uint32_t g) const { return Texel() ? Edge(g) : ScaleGlsl{}.sbHostSize(g, num, den); }
        uint32_t GuestEdge(uint32_t h) const { return ScaleGlsl{}.sbGuestEdge(h, num, den); }
        // A block's samples along the axis (a texel's most on a texel axis).
        uint32_t Stride() const { return ScaleGlsl{}.sbStride(num, den); }
        // Guest length g covers whole host pixels (every block whole; any
        // length on a texel axis).
        bool Exact(uint32_t g) const { return Texel() || g % den == 0; }
        // A guest coordinate clamped to 0 first (rectangles' signed edges).
        int32_t EdgeAt(int32_t g) const { return g <= 0 ? 0 : int32_t(Edge(uint32_t(g))); }
    };

    struct RenderScale
    {
        AxisScale x, y;

        bool operator==(const RenderScale&) const = default;
        bool IsOne() const { return x.IsOne() && y.IsOne(); }
        // Host samples a block of den.x x den.y guest texels has (one texel
        // on a texel axis, its most).
        uint32_t SamplesPerBlock() const { return x.Stride() * y.Stride(); }
        // A 4 KB guest page's host samples (words) in the scaled memory.
        uint32_t SlotWords() const { return ScaleGlsl{}.sbSlotWords(x.num, y.num, x.den, y.den); }
        // A width x height surface maps to whole host pixels and blocks.
        bool Exact(uint32_t width, uint32_t height) const { return x.Exact(width) && y.Exact(height); }
        // Host pixels per guest pixel as a ratio (the occlusion weight):
        // pixelsNum / pixelsDen (9/4 at 1.5x).
        uint32_t PixelsNum() const { return x.num * y.num; }
        uint32_t PixelsDen() const { return x.den * y.den; }
        // Host pixels a frame of guest w x h has (sizes the scratch buffer).
        uint64_t HostPixels(uint32_t w, uint32_t h) const { return uint64_t(x.Size(w)) * y.Size(h); }

        static RenderScale Whole(uint32_t sx, uint32_t sy) { return { { sx, 1 }, { sy, 1 } }; }
    };

    inline RenderScale ShadowScale(const RenderScale& internal)
    {
        auto cap = [](AxisScale axis) { return axis.num > 2 * axis.den ? AxisScale{ 2, 1 } : axis; };
        return { cap(internal.x), cap(internal.y) };
    }

    // The scaled resolve memory's default size in MB (renderer.cpp's
    // EnsureScaledMemory): 512 on a discrete GPU; on an integrated one, whose
    // VRAM is the system's (a Steam Deck's 1 GB carve-out), room for 16,384
    // guest pages at `s` (64 MB of guest memory; a career start and its
    // first race resolve ~4,200 pages at every scale), at most 512: 128 MB at
    // a Steam Deck's native rows (1x10/9), 256 at 2x.
    inline uint64_t ScaledPoolMegabytes(const RenderScale& s, bool discrete)
    {
        constexpr uint64_t kIntegratedSlots = 16384;
        if (discrete)
            return 512;
        return std::min<uint64_t>(512, (kIntegratedSlots * 4ull * s.SlotWords() + (1u << 20) - 1) >> 20);
    }

    // An axis from a number of half steps (2: 1x, 3: 1.5x, 4: 2x, 6: 3x),
    // reduced (4/2 is 2/1).
    inline AxisScale AxisFromHalves(uint32_t halves)
    {
        halves = std::max(halves, 2u);
        return halves % 2 == 0 ? AxisScale{ halves / 2, 1 } : AxisScale{ halves, 2 };
    }

    // num / den with no common factor.
    inline AxisScale Reduced(uint32_t num, uint32_t den)
    {
        uint32_t a = num, b = den;
        while (b)
        {
            const uint32_t t = a % b;
            a = b;
            b = t;
        }
        return a > 1 ? AxisScale{ num / a, den / a } : AxisScale{ num, den };
    }

    // The axis that gives `guest` pixels `host` host pixels exactly (720
    // rows for 800: 10/9), reduced; 1x for a host below the guest.
    inline AxisScale AxisFor(uint32_t host, uint32_t guest)
    {
        if (guest == 0 || host <= guest)
            return {};
        return Reduced(host, guest);
    }

    // "1", "1.5", "2", "1.111" (10/9) (the log's and settings' spelling).
    inline std::string AxisName(const AxisScale& a)
    {
        if (a.den == 1)
            return std::to_string(a.num);
        char text[16];
        snprintf(text, sizeof(text), a.Texel() ? "%.4g" : "%g", double(a.num) / double(a.den));
        return text;
    }

    // "1x1", "1.5x1.5", "3x2", "1x1.111": as the log has always named the scale.
    inline std::string ScaleName(const RenderScale& s)
    {
        return AxisName(s.x) + "x" + AxisName(s.y);
    }

    // The scales the renderer takes on an axis: 1, 1.5, 2, 3, 4 (the
    // whole ones of NFSMW_RENDER_SCALE before 1.5x, and 1.5x). Other
    // fractions are refused: false.
    inline bool AxisFromNumber(double v, AxisScale& out)
    {
        if (!(v >= 0.0))  // (NaN too)
            return false;
        v = std::max(v, 1.0);  // (0 was 1x, as the old whole-number parse clamped it)
        const double halves = std::round(v * 2.0);
        if (std::fabs(v * 2.0 - halves) > 1e-6)
            return false;
        const uint32_t h = uint32_t(std::clamp(halves, 2.0, 8.0));
        if (h % 2 == 1 && h != 3)
            return false;  // 2.5x, 3.5x: not offered
        out = AxisFromHalves(h);
        return true;
    }

    // One axis of NFSMW_RENDER_SCALE from `text`, `end` past it: a number
    // (AxisFromNumber's), or a ratio <num>/<den> from 1 to 4 ("10/9": a
    // Steam Deck's native rows at 1x), reduced.
    inline bool ParseAxis(const char* text, const char*& end, AxisScale& out)
    {
        char* stop = nullptr;
        const double v = std::strtod(text, &stop);
        if (stop == text)
            return false;
        if (*stop != '/')
        {
            end = stop;
            return AxisFromNumber(v, out);
        }
        const char* second = stop + 1;
        const unsigned long den = std::strtoul(second, &stop, 10);
        const double num = v;
        if (stop == second || den == 0 || den > 4096 || num != std::floor(num) || num < double(den) || num > 4.0 * double(den))
            return false;
        const AxisScale r = Reduced(uint32_t(num), uint32_t(den));
        if (r.den == 2 && r.num != 3)
            return false;  // 2.5x, 3.5x: not offered (as AxisFromNumber)
        end = stop;
        out = r;
        return true;
    }

    // NFSMW_RENDER_SCALE: "<s>" or "<sx>x<sy>", each 1, 1.5, 2, 3 or 4
    // ("2", "1.5", "3x2", "1.5x1") or a ratio ("1x10/9"); whole numbers above
    // 4 are clamped to 4, as before. False for anything else (the caller
    // logs it).
    inline bool ParseRenderScale(const char* text, RenderScale& out)
    {
        if (!text || !*text)
            return false;
        const char* end = nullptr;
        RenderScale s;
        if (!ParseAxis(text, end, s.x))
            return false;
        s.y = s.x;
        if (*end == 'x' || *end == 'X')
        {
            const char* second = end + 1;
            if (!ParseAxis(second, end, s.y))
                return false;
        }
        if (*end != '\0')
            return false;
        out = s;
        return true;
    }
}
