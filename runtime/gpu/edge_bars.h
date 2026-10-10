// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Edge bars: front-end black masks drawn for a 16:9 frame, and what fills
// the screen past the 16:9 band around them. Vulkan-free:
// tests/edge_bars_test.cpp checks it alone; gpu/renderer.cpp hands
// PlanFill a front-end draw (its registers, what the other front-end rules
// did, a reader for its vertices) and draws what the plan says.
//
// The career intro's "ROCKPORT POLICE VID CAM" overlay ends with a mask of
// six front-end quads: four textured CRT-bezel quadrants, each reaching one
// edge of the 16:9 frame (about 3% past it, black there), and two solid
// black side bars (the game's 32x32 white texture times vertex colour
// FD000000) spanning both edges (~3% past) and one side (41% past). On a
// 16:9 screen they cover everything outside the CRT window. Under Vert+ the
// front-end matrix is narrowed in y, so they end ~3% past the 16:9 band and
// the world shows in the rows above and below (28/31 rows on a 1280x800
// Deck, 87/90 at 1024x768); under Hor+ the bars end at 1.406 s, short of
// the screen's sides past a 2.50:1 screen (2880x810: 424 px a side).
//
// The intro also cuts between two shots (the van crash, ~95 s into the
// standard script) through a few frames of black: one black quad, centred,
// 0.970 of the 16:9 frame's width and 0.939 of its height (on a 16:9 screen
// a thin border of the world shows around it, as on the console; a TV's
// overscan hid it). Under Vert+ the world showed above and below it (63
// rows on the Deck), under Hor+ beside it (720 px a side at 2880x810). The
// intro's start (~57 s) has another, 0.920 of the frame's height.
//
// The fix leaves those draws as they are and, right after each side bar or
// black frame, draws it once or twice more, stretched over the whole frame
// and scissored to (its half of) what lies outside the 16:9 band: the rows
// above and below it (Vert+), or the columns beyond its side (Hor+; for a
// bar only when it ends short of the screen edge). Nothing inside the band
// changes, and the strips get the quad's own colour, alpha and blend.
//
// Units: the frame's clip space after the front-end matrix (w = 1), and
// "band units" = clip / view scale (x / scaleX, y / scaleY): +-1 are the
// 16:9 frame's edges, what a 16:9 screen shows.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace gpu::bars
{
    enum class Side : uint8_t { None, Left, Right };

    // A quad's box in the frame's clip space.
    struct Box
    {
        float x0, x1, y0, y1;
    };

    // One ePoly vertex: clip x and y through the front-end matrix (c0..c7),
    // and its colour word as read (ARGB, after the endian swap).
    struct Vertex
    {
        float x, y;
        uint32_t argb;
    };

    // The thresholds, in band units. Measured (six screen shapes): y edges
    // -1.027/-1.024 and 1.035/1.039, inner x 0.651/0.655, outer 1.406/1.410,
    // colour FD000000. The nearest draws they must not take: the fades and
    // dims (x +-1.29, y +-1.25..1.33), the pause dim's side gradients (x
    // 0.53..1.08 and 0.57..1.12, y +-1.91, alpha C8 to 00), the graffiti
    // wipes and splats (y to 1.23..3.3, one edge only).
    struct Rule
    {
        float pastMin = 1.01f, pastMax = 1.10f;    // each y edge, past the band's
        float innerMin = 0.50f, innerMax = 0.80f;  // |x| of the edge toward the centre
        float outerMin = 1.20f, outerMax = 2.00f;  // |x| of the outer edge
        uint32_t minAlpha = 0xF0;                  // vertex alpha, all four the same
        float cornerTolerance = 1e-3f;             // clip units: a vertex on its box's corner
    };

    // How far past the frame the stretched copy reaches, in clip units (the
    // draw's scissor trims it back to the frame). Not 1: with D3D's
    // half-pixel centres at 2x or 3x, clip +-1 lands a whole host pixel in
    // (2x: row and column 0 have their centres at 0.5, the edge at 1.0), and
    // that row would show the world.
    constexpr float kFillEdge = 1.02f;

    // Black, at least minAlpha.
    inline bool Black(uint32_t argb, uint32_t minAlpha)
    {
        return (argb & 0xFFFFFFu) == 0 && (argb >> 24) >= minAlpha;
    }

    // A side bar's colour: black, at least minAlpha. The renderer reads one
    // vertex's colour first and asks this before it reads the other three
    // (a black frame's is the same: black, at least F0).
    inline bool BarColour(uint32_t argb, const Rule& rule = {})
    {
        return Black(argb, rule.minAlpha);
    }

    // The box of a quad that is one axis-aligned rectangle: every vertex on
    // a corner (within `tolerance`), all four corners used, each edge along
    // x or y, so the two triangles (0,1,2) and (0,2,3) cover the box. False
    // for anything else (a diagonal, a repeated vertex, a bow-tie).
    inline bool RectangleBox(const Vertex v[4], float tolerance, Box& b)
    {
        b = { v[0].x, v[0].x, v[0].y, v[0].y };
        for (int i = 1; i < 4; i++)
        {
            b.x0 = std::min(b.x0, v[i].x);
            b.x1 = std::max(b.x1, v[i].x);
            b.y0 = std::min(b.y0, v[i].y);
            b.y1 = std::max(b.y1, v[i].y);
        }
        // (Not "near": <windows.h> defines near and far as empty macros.)
        const float t = tolerance;
        auto within = [t](float a, float c) { return std::fabs(a - c) <= t; };
        uint32_t corners = 0;
        for (int i = 0; i < 4; i++)
        {
            const bool lx = within(v[i].x, b.x0), hx = within(v[i].x, b.x1), ly = within(v[i].y, b.y0), hy = within(v[i].y, b.y1);
            if ((lx == hx) || (ly == hy))  // off the corners (or a box too thin to tell)
                return false;
            corners |= 1u << ((hx ? 1 : 0) | (hy ? 2 : 0));
            const Vertex& n = v[(i + 1) & 3];
            if (within(n.x, v[i].x) == within(n.y, v[i].y))  // a diagonal, or a repeated vertex
                return false;
        }
        return corners == 0xF;
    }

    // A side bar: one axis-aligned rectangle (its vertices in order around
    // it), one colour on all four vertices, black, at least minAlpha; in y
    // a little past both band edges; in x from inside the band to well past
    // one side. Its box goes to `box`.
    inline Side MatchSideBar(const Vertex v[4], float scaleX, float scaleY, const Rule& rule = {}, Box* box = nullptr)
    {
        if (!(scaleX > 0.0f && scaleY > 0.0f))
            return Side::None;
        for (int i = 1; i < 4; i++)
            if (v[i].argb != v[0].argb)
                return Side::None;
        if (!BarColour(v[0].argb, rule))
            return Side::None;
        Box b;
        if (!RectangleBox(v, rule.cornerTolerance, b))
            return Side::None;
        const float lo = b.x0 / scaleX, hi = b.x1 / scaleX, loY = b.y0 / scaleY, hiY = b.y1 / scaleY;
        auto in = [](float a, float lo_, float hi_) { return a >= lo_ && a <= hi_; };
        if (!in(-loY, rule.pastMin, rule.pastMax) || !in(hiY, rule.pastMin, rule.pastMax))
            return Side::None;
        Side side = Side::None;
        if (in(lo, rule.innerMin, rule.innerMax) && in(hi, rule.outerMin, rule.outerMax))
            side = Side::Right;
        else if (in(-hi, rule.innerMin, rule.innerMax) && in(-lo, rule.outerMin, rule.outerMax))
            side = Side::Left;
        if (side != Side::None && box)
            *box = b;
        return side;
    }

    // A black frame's thresholds, in band units. Logged on the Deck and at
    // 32:9: x +-0.970, y +-0.939, centred, FF000000 (the cut at ~95 s), and
    // x +-0.970, y +-0.920, FE000000 (the intro's start, ~57 s). The nearest
    // draws it must not take: the fades and dims (+-1.29: past the band,
    // the widen, stretch and cover rules' own), the title key art (x
    // +-1.104, y +-0.981, grey), the overlay at ~57 s (x +-1.104, B8 alpha),
    // the vid cam's noise and dark overlays (x +-0.70; 3C alpha), its dialog
    // boxes (x +-0.58), a full-band black layer (+-1.0, the cover rule's).
    struct BlackoutRule
    {
        float centreMax = 0.01f;                     // |the box's centre|, each axis
        float extentMin = 0.90f, extentMax = 0.99f;  // half its width and half its height
        uint32_t minAlpha = 0xF0;                    // vertex alpha, all four the same
        float cornerTolerance = 1e-3f;               // clip units, as Rule's
    };

    // A black frame: one axis-aligned rectangle, one colour on all four
    // vertices, black, at least minAlpha, centred in the band and covering
    // most of it in x and y while stopping short of its edges. Its box goes
    // to `box`.
    inline bool MatchBlackout(const Vertex v[4], float scaleX, float scaleY, const BlackoutRule& rule = {}, Box* box = nullptr)
    {
        if (!(scaleX > 0.0f && scaleY > 0.0f))
            return false;
        for (int i = 1; i < 4; i++)
            if (v[i].argb != v[0].argb)
                return false;
        if (!Black(v[0].argb, rule.minAlpha))
            return false;
        Box b;
        if (!RectangleBox(v, rule.cornerTolerance, b))
            return false;
        const float midX = (b.x0 + b.x1) * 0.5f / scaleX, midY = (b.y0 + b.y1) * 0.5f / scaleY;
        const float halfX = (b.x1 - b.x0) * 0.5f / scaleX, halfY = (b.y1 - b.y0) * 0.5f / scaleY;
        if (!(std::fabs(midX) <= rule.centreMax && std::fabs(midY) <= rule.centreMax))
            return false;
        if (!(halfX >= rule.extentMin && halfX <= rule.extentMax && halfY >= rule.extentMin && halfY <= rule.extentMax))
            return false;
        if (box)
            *box = b;
        return true;
    }

    // Host pixels, [x0, x1) x [y0, y1).
    struct Rect
    {
        int32_t x0, y0, x1, y1;
    };

    inline bool Empty(const Rect& r)
    {
        return r.x1 <= r.x0 || r.y1 <= r.y0;
    }

    inline Rect Intersect(const Rect& a, const Rect& b)
    {
        return { std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1) };
    }

    // The first host pixel whose centre lies past NDC `ndc` over `size`
    // pixels, the edge snapped to 1/256 pixel as the rasteriser snaps
    // vertices. The band clip's rows (BandRows) and the fills' edges both
    // come from it, so they meet exactly.
    inline int32_t Edge(float ndc, float size)
    {
        const float a = std::round((ndc + 1.0f) * 0.5f * size * 256.0f) / 256.0f;
        return int32_t(std::ceil(a - 0.5f));
    }

    // Host rows [top, bottom).
    struct Rows
    {
        int32_t top, bottom;
    };

    // The host rows whose centres lie between clip y `lo` and `hi` through a
    // draw's posScale y (NDC = y * ps1 + ps3, either sign of ps1): gpu/
    // renderer.cpp clips the rest of the front end to the band's rows with
    // this, and the fills cover the rows it leaves.
    inline Rows BandRows(float lo, float hi, float ps1, float ps3, float hostH)
    {
        const int32_t a = Edge(lo * ps1 + ps3, hostH), b = Edge(hi * ps1 + ps3, hostH);
        return { std::min(a, b), std::max(a, b) };
    }

    // The host pixels of clip rectangle [cx0, cx1] x [cy0, cy1] through a
    // draw's posScale (NDC = clip * ps.xy + ps.zw, w = 1), either sign of
    // ps0/ps1 (the front end's viewport y scale is negative).
    inline Rect ClipToHost(float cx0, float cx1, float cy0, float cy1, const float ps[4], float hostW, float hostH)
    {
        const int32_t a = Edge(cx0 * ps[0] + ps[2], hostW), b = Edge(cx1 * ps[0] + ps[2], hostW);
        const int32_t c = Edge(cy0 * ps[1] + ps[3], hostH), d = Edge(cy1 * ps[1] + ps[3], hostH);
        return { std::min(a, b), std::min(c, d), std::max(a, b), std::max(c, d) };
    }

    // What a matched bar fills, inside the draw's scissor (at most two
    // rectangles): under Vert+ (scaleY < 1) its half of the frame (centre to
    // its side's edge) in the rows above and below the band (|y| > scaleY);
    // under Hor+ (scaleX < 1), only if the bar ends short of its side of the
    // screen (clip 1, the screen edge itself), the columns past the band on
    // its side (|x| > scaleX). The far edges reach kFillEdge, which the
    // scissor trims back to the frame. The band is centred: the caller
    // checks the matrix has no translation.
    inline uint32_t FillRects(Side side, const Box& box, const float ps[4], float hostW, float hostH, const Rect& scissor,
        float scaleX, float scaleY, Rect out[2])
    {
        uint32_t n = 0;
        auto add = [&](const Rect& r) {
            const Rect c = Intersect(r, scissor);
            if (!Empty(c))
                out[n++] = c;
        };
        if (side == Side::None)
            return 0;
        const bool right = side == Side::Right;
        if (scaleY < 1.0f)
        {
            const float e = kFillEdge, xa = right ? 0.0f : -e, xb = right ? e : 0.0f;
            add(ClipToHost(xa, xb, scaleY, e, ps, hostW, hostH));
            add(ClipToHost(xa, xb, -e, -scaleY, ps, hostW, hostH));
        }
        else if (scaleX < 1.0f && (right ? box.x1 < 1.0f : box.x0 > -1.0f))
        {
            const float e = kFillEdge;
            add(right ? ClipToHost(scaleX, e, -e, e, ps, hostW, hostH) : ClipToHost(-e, -scaleX, -e, e, ps, hostW, hostH));
        }
        return n;
    }

    // What a black frame fills, inside the draw's scissor: everything
    // outside the band, the rows above and below it across the whole width
    // (Vert+) or the columns beside it down the whole height (Hor+; at most
    // one of the view scales is below 1). The far edges reach kFillEdge.
    inline uint32_t BlackoutRects(const float ps[4], float hostW, float hostH, const Rect& scissor, float scaleX, float scaleY,
        Rect out[2])
    {
        uint32_t n = 0;
        auto add = [&](const Rect& r) {
            const Rect c = Intersect(r, scissor);
            if (!Empty(c))
                out[n++] = c;
        };
        const float e = kFillEdge;
        if (scaleY < 1.0f)
        {
            add(ClipToHost(-e, e, scaleY, e, ps, hostW, hostH));
            add(ClipToHost(-e, e, -e, -scaleY, ps, hostW, hostH));
        }
        else if (scaleX < 1.0f)
        {
            add(ClipToHost(scaleX, e, -e, e, ps, hostW, hostH));
            add(ClipToHost(-e, -scaleX, -e, e, ps, hostW, hostH));
        }
        return n;
    }

    // The posScale that stretches the quad's box over [-edge, edge] in clip
    // x and y (x' = ax x + bx, likewise y), folded in: NDC = x' ps0 + ps2 =
    // x (ax ps0) + (ps2 + bx ps0). Orientation is kept (ax, ay > 0) whatever
    // the signs of c5 and the viewport; a tile's window offset (in ps2/ps3,
    // NFSMW_TILING=1) passes through, so the copy lands where this draw's
    // own viewport puts clip +-edge.
    inline void CoverFrame(const Box& b, const float ps[4], float out[4], float edge = kFillEdge)
    {
        const float ax = 2.0f * edge / (b.x1 - b.x0), bx = -edge - ax * b.x0;
        const float ay = 2.0f * edge / (b.y1 - b.y0), by = -edge - ay * b.y0;
        out[0] = ax * ps[0];
        out[1] = ay * ps[1];
        out[2] = ps[2] + bx * ps[0];
        out[3] = ps[3] + by * ps[1];
    }

    // A front-end draw as gpu/renderer.cpp sees it once the widen, stretch,
    // cover and band-clip rules are done: what PlanFill decides from.
    struct Draw
    {
        uint32_t prim = 0;            // VGT_DRAW_INITIATOR prim_type: 13 a quad list
        uint32_t count = 0;           // vertices
        const float* c = nullptr;     // shader constants c0..c7: the front-end matrix's x and y rows
        float frontEndY = 0.0f;       // game::FrontEndScaleY(), c5's magnitude; 0 from a ppc/ without the Vert+ hooks
        float scaleX = 1.0f, scaleY = 1.0f;  // the view scale
        bool frontEndShader = false;  // the vertex shader reads c0..c3 and the ePolys (fetch constant 95)
        bool leftAlone = false;       // not widened, stretched, covered or clipped to the band
        bool occlusion = false;       // inside an occlusion query's bracket
        uint32_t vte = 0;             // PA_CL_VTE_CNTL
        uint32_t blend0 = 0;          // RB_BLENDCONTROL0 & 0x1FFF1FFF
        bool t0Sampled = false;       // the pixel shader samples t0
        uint32_t t0Width = 0, t0Height = 0;  // t0's size in texels
        float posScale[4] = {};       // the draw's own: NDC = clip * ps.xy + ps.zw
        float hostW = 0.0f, hostH = 0.0f;    // the host viewport: the target's size times the render scale
        Rect scissor{};               // the draw's scissor (host pixels)
    };

    enum class Fill : uint8_t { None, LeftBar, RightBar, Blackout };

    // What to draw after the draw: `rectCount` more copies of it (none for a
    // bar that reaches the screen edge), with `posScale` and each rectangle
    // as the scissor.
    struct Plan
    {
        Fill fill = Fill::None;
        Box box{};          // the quad's, clip space
        uint32_t argb = 0;  // its colour
        uint32_t rectCount = 0;
        Rect rects[2]{};
        float posScale[4]{};
    };

    // The register checks, before any vertex is read: one quad of the
    // front-end shader, the other rules left it alone, outside an occlusion
    // query, no translation, c5 the front end's y scale (unless the ppc/
    // predates the Vert+ hooks, which report none: then the view is never
    // narrowed in y and only Hor+ can need a fill), drawn through its
    // viewport transform (x and y scaled and offset: posScale is that
    // transform), with alpha blending or none (the copy must look like the
    // draw), over a small t0 (the game's white texture is 32x32).
    inline bool Candidate(const Draw& d)
    {
        const float* c = d.c;
        const bool yScale = d.frontEndY > 0.0f ? std::fabs(std::fabs(c[5]) - d.frontEndY) <= d.frontEndY * 1e-5f
                                               : d.frontEndY == 0.0f && d.scaleY >= 1.0f;
        return !d.occlusion && d.prim == 13 && d.count == 4 && d.frontEndShader && d.leftAlone && c[3] == 0.0f && c[4] == 0.0f &&
            c[6] == 0.0f && c[7] == 0.0f && yScale && (d.vte & 0xF) == 0xF && (d.blend0 == 0x07060706u || d.blend0 == 0x00010001u) &&
            d.t0Sampled && d.t0Width <= 32 && d.t0Height <= 32;
    }

    // The plan for a draw. read(k, j, word): word j of the draw's vertex k
    // (k 0..3 in draw order, through its indices; an ePoly's words 0..3 are
    // x, y, z and the colour), false past the vertex buffer. Register checks
    // first, then vertex 0's colour (one read: a front-end frame has
    // hundreds of quads and few are black), then the rest.
    template <class ReadWord>
    Plan PlanFill(const Draw& d, ReadWord&& read)
    {
        Plan p;
        if (!Candidate(d))
            return p;
        uint32_t colour = 0;
        if (!read(0u, 3u, colour) || !BarColour(colour))
            return p;
        const float* c = d.c;
        Vertex v[4];
        for (uint32_t k = 0; k < 4; k++)
        {
            uint32_t w[4];
            for (uint32_t j = 0; j < 4; j++)
                if (!read(k, j, w[j]))
                    return p;
            float q[3];
            std::memcpy(q, w, sizeof(q));
            v[k] = { q[0] * c[0] + q[1] * c[1] + q[2] * c[2] + c[3], q[0] * c[4] + q[1] * c[5] + q[2] * c[6] + c[7], w[3] };
        }
        Box box{};
        if (const Side side = MatchSideBar(v, d.scaleX, d.scaleY, {}, &box); side != Side::None)
        {
            p.fill = side == Side::Right ? Fill::RightBar : Fill::LeftBar;
            p.rectCount = FillRects(side, box, d.posScale, d.hostW, d.hostH, d.scissor, d.scaleX, d.scaleY, p.rects);
        }
        else if (MatchBlackout(v, d.scaleX, d.scaleY, {}, &box))
        {
            p.fill = Fill::Blackout;
            p.rectCount = BlackoutRects(d.posScale, d.hostW, d.hostH, d.scissor, d.scaleX, d.scaleY, p.rects);
        }
        else
            return p;
        p.box = box;
        p.argb = v[0].argb;
        CoverFrame(box, d.posScale, p.posScale);
        return p;
    }
}
