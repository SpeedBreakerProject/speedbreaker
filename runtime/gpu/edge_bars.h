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
// intro's start (~57 s) has another, 0.920 of the frame's height. The
// Blacklist's rival bio (SafeHouseRivalBio.fng, "View Rival Movie / View
// Rival Car") sets its text and picture on one more, 0.976 x 0.985 of the
// frame (FF000000, fading in from 11000000), for as long as the screen is
// up: a thin rectangular border of the scene behind it all round it, on
// every screen shape (GitHub #19).
//
// The fix leaves those draws as they are and, right after each side bar or
// black frame, draws it once or more again, stretched over the whole frame
// and scissored to what it must fill: for a bar, (its half of) what lies
// outside the 16:9 band, the rows above and below it (Vert+) or the columns
// beyond its side (Hor+; only when it ends short of the screen edge), and
// nothing inside the band changes; for a black frame, everything outside
// the quad itself, the band's thin border too, on every screen shape, 16:9
// included (the period TV's look, as the results screen's overscan rows).
// The copies get the quad's own colour, alpha and blend.
//
// Two pictures the front end draws just short of the 16:9 frame's edges
// get the screen's edges instead, on every screen shape: around the results
// screen's backdrop the rows are cleared to black (PlanBackdrop), and the
// Showcase's vignette is stretched to the screen's edges (PlanVignette); so
// is a flat full-screen layer that stops short of them (the fade to black
// before a load: PlanLayer).
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
    // 32:9: x +-0.970, y +-0.939, centred, FF000000 (the cut at ~95 s), and x
    // +-0.970, y +-0.920, FE000000 (the intro's start, ~57 s); x +-0.976, y
    // +-0.985, FB000000 then FF000000 (the Blacklist bio's panel, its fade in
    // from 11000000 short of minAlpha: the border shows for those ~15 frames,
    // as before). The Rap Sheet's panel (RapSheetMain.fng: x -0.983..0.957,
    // y +-0.985, FF000000, GitHub #87) sits 0.013 left of centre: centreMax
    // 0.02 (it was 0.01, and its border showed the scene all round it). The nearest draws it must not take: the fades and dims
    // (+-1.29: past the band, the widen, stretch and cover rules' own), the
    // title key art (x +-1.104, y +-0.981, grey), the overlay at ~57 s (x
    // +-1.104, B8 alpha), the vid cam's noise and dark overlays (x +-0.70; 3C
    // alpha), its dialog boxes (x +-0.58), a full-band black layer (+-1.0, the
    // cover rule's).
    struct BlackoutRule
    {
        float centreMax = 0.02f;                     // |the box's centre|, each axis (the Rap Sheet's: -0.013 in x)
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
    // outside its box, on every screen shape (16:9 too): the rows above and
    // below it across the whole width, then the columns beside it within its
    // rows; up to 4 rectangles, which never overlap and meet the box on the
    // same pixel edges (Edge) as its own rasterised pixels. The far edges
    // reach kFillEdge. (v0.1.1 to v0.1.5 filled only past the band: the
    // thin border inside it showed what was behind, as a 360's own output
    // does and a period TV's overscan hid: GitHub #19's Blacklist bio.)
    inline uint32_t BlackoutRects(const Box& b, const float ps[4], float hostW, float hostH, const Rect& scissor, Rect out[4])
    {
        uint32_t n = 0;
        auto add = [&](float cx0, float cx1, float cy0, float cy1) {
            if (cx1 <= cx0 || cy1 <= cy0)
                return;
            const Rect c = Intersect(ClipToHost(cx0, cx1, cy0, cy1, ps, hostW, hostH), scissor);
            if (!Empty(c) && n < 4)
                out[n++] = c;
        };
        const float e = kFillEdge;
        add(-e, e, b.y1, e);         // past its top in clip y (either way up on screen)
        add(-e, e, -e, b.y0);        // past its bottom
        add(-e, b.x0, b.y0, b.y1);   // left of it, within its rows
        add(b.x1, e, b.y0, b.y1);    // right of it
        return n;
    }

    // A full-band backdrop: the results screen's picture after a race (the
    // "WINNER" polaroid over asphalt and leaves; the title's and Controls'
    // key art have the same shape) is one opaque textured quad of the front
    // end, centred, x +-1.104 and y +-0.981 of the 16:9 band. On a 16:9
    // screen it hides the world but for a thin border at the top and bottom
    // (y 0.981 to 1: ~7 of 720 rows each), where the race behind it shows;
    // a real 360's own output has that border too (HDMI captures of the
    // results screen), and a TV's overscan hid it. Past the band (Hor+
    // beside it, Vert+ above and below it) the world rendered behind it
    // showed as well: the race's road and cars at the sides of the results
    // on an iPhone (2868x1320). The art's own pixels are all kept, past the
    // band too; everything else is filled black on every screen shape, 16:9
    // too: past the band (what no 16:9 screen shows, as a 16:9 picture's
    // letterbox) and, inside it, the rows between the picture's top or
    // bottom edge and the band's (what a period TV's overscan hid). The
    // title's and Controls' key art have black around them already, so their
    // fill changes nothing on screen. Thresholds in band units. The nearest
    // draws it must not take: the fades and dims
    // (+-1.29: flat, and the widen, stretch and cover rules' own), the pause
    // dim (C8 alpha) and its gradients, the vid cam's noise and dark overlays
    // (3C alpha, x +-0.70), the overlay at the intro's start (x +-1.104, B8
    // alpha), the black frames (the 32x32 white texture), a menu backdrop the
    // cover rule zooms under Vert+ (not left alone), the HUD's pieces (none
    // covers the band).
    struct BackdropRule
    {
        float centreMax = 0.01f;    // |the box's centre|, each axis
        float extentMinX = 0.99f;   // half its width: the whole band's (to 1%)
        float extentMinY = 0.95f;   // half its height: nearly all of it
        float extentMax = 1.20f;    // each half extent: short of the fades' +-1.29
        uint32_t minAlpha = 0xF0;   // vertex alpha, all four
        uint32_t minTexels = 128;   // t0 at least this wide and tall (not the 32x32 white texture)
        float cornerTolerance = 1e-3f;
    };

    // A backdrop: one axis-aligned rectangle, every vertex's alpha at least
    // minAlpha, centred in the band and covering all of its width and nearly
    // all of its height, not reaching the fades' extent. Its box goes to
    // `box`. (The texture's size is a register check: BackdropCandidate.)
    inline bool MatchBackdrop(const Vertex v[4], float scaleX, float scaleY, const BackdropRule& rule = {}, Box* box = nullptr)
    {
        if (!(scaleX > 0.0f && scaleY > 0.0f))
            return false;
        for (int i = 0; i < 4; i++)
            if ((v[i].argb >> 24) < rule.minAlpha)
                return false;
        Box b;
        if (!RectangleBox(v, rule.cornerTolerance, b))
            return false;
        const float midX = (b.x0 + b.x1) * 0.5f / scaleX, midY = (b.y0 + b.y1) * 0.5f / scaleY;
        const float halfX = (b.x1 - b.x0) * 0.5f / scaleX, halfY = (b.y1 - b.y0) * 0.5f / scaleY;
        if (!(std::fabs(midX) <= rule.centreMax && std::fabs(midY) <= rule.centreMax))
            return false;
        if (!(halfX >= rule.extentMinX && halfX < rule.extentMax && halfY >= rule.extentMinY && halfY < rule.extentMax))
            return false;
        if (box)
            *box = b;
        return true;
    }

    // What a backdrop fills past the band, inside the draw's scissor: what
    // lies past it (the columns beside it under Hor+, the rows above and
    // below it under Vert+; at most one of the view scales is below 1) and
    // outside the backdrop's own box, up to kFillEdge: per side, the part
    // beyond the box and, where the box reaches into that side, the parts
    // above and below it (Hor+; left and right of it under Vert+). At most
    // 6; none at 16:9.
    inline uint32_t PastBandRects(const Box& b, const float ps[4], float hostW, float hostH, const Rect& scissor, float scaleX, float scaleY,
        Rect out[6])
    {
        uint32_t n = 0;
        auto add = [&](float cx0, float cx1, float cy0, float cy1) {
            if (cx1 <= cx0 || cy1 <= cy0)
                return;
            const Rect c = Intersect(ClipToHost(cx0, cx1, cy0, cy1, ps, hostW, hostH), scissor);
            if (!Empty(c) && n < 6)
                out[n++] = c;
        };
        const float e = kFillEdge;
        if (scaleX < 1.0f)
        {
            // Right of the band: [scaleX, e] x [-e, e]; left: [-e, -scaleX].
            const float in = std::min(std::max(b.x1, scaleX), e);  // where the box ends in the right strip
            add(in, e, -e, e);
            add(scaleX, in, b.y1, e);
            add(scaleX, in, -e, b.y0);
            const float inL = std::max(std::min(b.x0, -scaleX), -e);
            add(-e, inL, -e, e);
            add(inL, -scaleX, b.y1, e);
            add(inL, -scaleX, -e, b.y0);
        }
        else if (scaleY < 1.0f)
        {
            const float in = std::min(std::max(b.y1, scaleY), e);
            add(-e, e, in, e);
            add(b.x1, e, scaleY, in);
            add(-e, b.x0, scaleY, in);
            const float inB = std::max(std::min(b.y0, -scaleY), -e);
            add(-e, e, -e, inB);
            add(b.x1, e, inB, -scaleY);
            add(-e, b.x0, inB, -scaleY);
        }
        return n;
    }

    // The overscan rows, inside the draw's scissor: inside the band, the
    // rows between the backdrop's top edge and the band's and between its
    // bottom edge and the band's (y +-0.981 to +-1 of the band for the
    // results picture), across the band's width. Where the band's edge is
    // the screen's (y at 16:9 and under Hor+, x at 16:9 and under Vert+)
    // they reach kFillEdge, as the fills past the band do (at 2x with
    // half-pixel centres the outermost row's centre lies past clip 1); where
    // it isn't, they stop at it and PastBandRects' rectangles go on from
    // there, meeting them on the same pixel edge (gpu::bars::Edge). None
    // where the box reaches the band's edge. At most 2.
    inline uint32_t OverscanRows(const Box& b, const float ps[4], float hostW, float hostH, const Rect& scissor, float scaleX, float scaleY,
        Rect out[2])
    {
        uint32_t n = 0;
        auto add = [&](float cx0, float cx1, float cy0, float cy1) {
            if (cx1 <= cx0 || cy1 <= cy0)
                return;
            const Rect c = Intersect(ClipToHost(cx0, cx1, cy0, cy1, ps, hostW, hostH), scissor);
            if (!Empty(c) && n < 2)
                out[n++] = c;
        };
        const float e = kFillEdge;
        const float bandX = scaleX < 1.0f ? scaleX : e, bandY = scaleY < 1.0f ? scaleY : e;
        add(-bandX, bandX, b.y1, bandY);
        add(-bandX, bandX, -bandY, b.y0);
        return n;
    }

    // What a backdrop fills, inside the draw's scissor: everything outside
    // its box but the columns inside the band beside it (none for a picture
    // as wide as the band): PastBandRects' and then OverscanRows'
    // rectangles, which never overlap. At most 8.
    inline uint32_t BackdropRects(const Box& b, const float ps[4], float hostW, float hostH, const Rect& scissor, float scaleX, float scaleY,
        Rect out[8])
    {
        const uint32_t n = PastBandRects(b, ps, hostW, hostH, scissor, scaleX, scaleY, out);
        return n + OverscanRows(b, ps, hostW, hostH, scissor, scaleX, scaleY, out + n);
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
        uint32_t t0Format = 0;        // t0's fetch constant format (6: k_8_8_8_8, 32-bit texels)
        float posScale[4] = {};       // the draw's own: NDC = clip * ps.xy + ps.zw
        float hostW = 0.0f, hostH = 0.0f;    // the host viewport: the target's size times the render scale
        Rect scissor{};               // the draw's scissor (host pixels)
    };

    enum class Fill : uint8_t { None, LeftBar, RightBar, Blackout, Backdrop, Vignette, Layer, EdgePiece, Raise };

    // What to draw after the draw: for a bar or a black frame, `rectCount`
    // more copies of it (none for a bar that reaches the screen edge), with
    // `posScale` and each rectangle as the scissor; for a backdrop, each
    // rectangle cleared to opaque black (its picture must not be copied);
    // for a vignette or a layer, nothing after it: the draw itself is drawn
    // with `posScale` instead of its own (no rectangles); for an edge piece,
    // the draw itself within `own` (its scissor narrowed to stop at the
    // band's edge), then one copy as for a bar.
    struct Plan
    {
        Fill fill = Fill::None;
        Box box{};          // the quad's, clip space
        uint32_t argb = 0;  // its colour
        uint32_t rectCount = 0;
        Rect rects[8]{};    // a bar's 2, a black frame's 4, a backdrop's 8, an edge piece's 1
        float posScale[4]{};
        bool clipOwn = false;  // an edge piece: the draw's own scissor becomes `own`
        Rect own{};
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

    // The register checks for a backdrop, before any vertex is read:
    // Candidate's, but over a large t0 (a picture, not the white texture),
    // at every screen shape (16:9 too: its overscan rows).
    inline bool BackdropCandidate(const Draw& d, const BackdropRule& rule = {})
    {
        const float* c = d.c;
        const bool yScale = d.frontEndY > 0.0f ? std::fabs(std::fabs(c[5]) - d.frontEndY) <= d.frontEndY * 1e-5f
                                               : d.frontEndY == 0.0f && d.scaleY >= 1.0f;
        return !d.occlusion && d.prim == 13 && d.count == 4 && d.frontEndShader && d.leftAlone &&
            c[3] == 0.0f && c[4] == 0.0f && c[6] == 0.0f && c[7] == 0.0f && yScale && (d.vte & 0xF) == 0xF &&
            (d.blend0 == 0x07060706u || d.blend0 == 0x00010001u) && d.t0Sampled && d.t0Width >= rule.minTexels && d.t0Height >= rule.minTexels;
    }

    // The plan for a backdrop (PlanFill's for bars and black frames):
    // vertex 0's alpha first, then the rest.
    template <class ReadWord>
    Plan PlanBackdrop(const Draw& d, ReadWord&& read, const BackdropRule& rule = {})
    {
        Plan p;
        if (!BackdropCandidate(d, rule))
            return p;
        uint32_t colour = 0;
        if (!read(0u, 3u, colour) || (colour >> 24) < rule.minAlpha)
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
        if (!MatchBackdrop(v, d.scaleX, d.scaleY, rule, &box))
            return p;
        p.fill = Fill::Backdrop;
        p.box = box;
        p.argb = v[0].argb;
        p.rectCount = BackdropRects(box, d.posScale, d.hostW, d.hostH, d.scissor, d.scaleX, d.scaleY, p.rects);
        return p;
    }

    // The Showcase's vignette (Showcase.fng: the "Rotate / Zoom Out / Zoom In"
    // car viewer of the car lot, the customize shop and the Blacklist's rival
    // cars; GitHub #19): one front-end quad over the 3D garage, centred, x
    // +-0.970 and y +-0.958 of the 16:9 band (ten of the front end's 640x480
    // units in from each edge), its 256x256 k_8_8_8_8 texture black, clear in
    // the middle (every texel there 00000000) and darkening steeply toward its
    // edges (alpha 0.1-0.8 at the edge itself, 1.0 toward the corners), alpha-
    // blended (vertex colour 7F7F7F, its alpha fading in from 04 to FE). On a
    // 16:9 screen the garage shows undimmed in a thin border all round it; a
    // real 360's own output has that border too (an HDMI capture of the car
    // lot's Showcase: the same step at x +-0.970), and a period TV's overscan
    // (2.5-5% a side) hid it. Under Vert+ the garage rendered above and below
    // the band showed undimmed as well (7% of a MacBook's height at the top and
    // at the bottom, 55 rows of a Deck's 800): a bright rectangular frame round
    // the dimmed picture. The quad is stretched to the screen's edges (its box
    // to +-kFillEdge: CoverFrame), on every screen shape, 16:9 too, as a period
    // TV showed it (the results screen's overscan rows are filled the same way:
    // OverscanRows). Its middle is clear, so only its dark edges move out:
    // 3.1% in x and 4.4% in y at 16:9, y by 1/sv more under Vert+, x by 1/sx
    // more under Hor+. Thresholds in band units. The nearest draws it must not
    // take: the intro's black frames (x +-0.970, y +-0.939/0.920: opaque black
    // over the 32x32 white texture, and the black frame rule's), the title's
    // key art and the results picture (x +-1.104), the vid cam's noise and dark
    // overlays (x +-0.70), a full-band picture (+-1.0), an inset picture
    // (opaque in the middle: the texel check).
    struct VignetteRule
    {
        float centreMax = 0.01f;                     // |the box's centre|, each axis
        float extentMin = 0.90f, extentMax = 0.99f;  // half its width and half its height
        uint32_t minTexels = 128;                    // t0 at least this wide and tall
        float cornerTolerance = 1e-3f;               // clip units, as Rule's
    };

    // A vignette's shape: one axis-aligned rectangle, one colour on all four
    // vertices (any alpha: it fades in), centred in the band and covering
    // most of it in x and y while stopping short of its edges. Its box goes
    // to `box`. (Its texture is the plan's check: PlanVignette.)
    inline bool MatchVignette(const Vertex v[4], float scaleX, float scaleY, const VignetteRule& rule = {}, Box* box = nullptr)
    {
        if (!(scaleX > 0.0f && scaleY > 0.0f))
            return false;
        for (int i = 1; i < 4; i++)
            if (v[i].argb != v[0].argb)
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

    // The byte offset of texel (x, y) in a 32-bit 2D texture: Xenos tiling
    // (gpu/renderer.cpp's GLSL tiled2D, pitch in texels, a multiple of 32)
    // or linear rows of `pitch` texels.
    inline uint32_t TexelOffset32(uint32_t x, uint32_t y, uint32_t pitch, bool tiled)
    {
        if (!tiled)
            return (y * pitch + x) * 4u;
        const uint32_t outer = (((y >> 5) * (pitch >> 5)) + (x >> 5)) << 6;
        const uint32_t inner = (((y >> 1) & 7u) << 3) | (x & 7u);
        const uint32_t oi = (outer | inner) << 2;
        const uint32_t bank = (y >> 4) & 1u;
        const uint32_t pipe = ((x >> 3) & 3u) ^ (((y >> 3) & 1u) << 1);
        return ((y & 1u) << 4) | (pipe << 6) | (bank << 11) | (oi & 0xFu) | (((oi >> 4) & 1u) << 5) | (((oi >> 5) & 7u) << 8) |
            ((oi >> 8) << 12);
    }

    // The texels a vignette's middle must hold clear (00000000, whatever the
    // endian): the centre and halfway from it to each edge. An inset
    // picture, opaque there, is no vignette.
    constexpr uint32_t kClearSamples = 5;
    inline void ClearSample(uint32_t i, uint32_t width, uint32_t height, uint32_t& x, uint32_t& y)
    {
        const uint32_t xs[kClearSamples] = { width / 2, width / 4, width * 3 / 4, width / 2, width / 2 };
        const uint32_t ys[kClearSamples] = { height / 2, height / 2, height / 2, height / 4, height * 3 / 4 };
        x = xs[i % kClearSamples];
        y = ys[i % kClearSamples];
    }

    // The register checks for a vignette, before any vertex is read:
    // Candidate's, but alpha-blended (07060706: it is translucent), over a
    // large 32-bit t0 (format 6, k_8_8_8_8), at every screen shape.
    inline bool VignetteCandidate(const Draw& d, const VignetteRule& rule = {})
    {
        const float* c = d.c;
        const bool yScale = d.frontEndY > 0.0f ? std::fabs(std::fabs(c[5]) - d.frontEndY) <= d.frontEndY * 1e-5f
                                               : d.frontEndY == 0.0f && d.scaleY >= 1.0f;
        return !d.occlusion && d.prim == 13 && d.count == 4 && d.frontEndShader && d.leftAlone && c[3] == 0.0f && c[4] == 0.0f &&
            c[6] == 0.0f && c[7] == 0.0f && yScale && (d.vte & 0xF) == 0xF && d.blend0 == 0x07060706u && d.t0Sampled && d.t0Format == 6 &&
            d.t0Width >= rule.minTexels && d.t0Height >= rule.minTexels;
    }

    // The plan for a vignette: the draw's posScale stretched so its box
    // reaches past every screen edge (CoverFrame), no rectangles. read: as
    // PlanFill's; texel(x, y, word): t0's 32-bit texel (x, y), false if it
    // can't be read. Vertices first (as PlanBackdrop: one quad over a large
    // t0 gets this far), then the texels of its middle.
    template <class ReadWord, class ReadTexel>
    Plan PlanVignette(const Draw& d, ReadWord&& read, ReadTexel&& texel, const VignetteRule& rule = {})
    {
        Plan p;
        if (!VignetteCandidate(d, rule))
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
        if (!MatchVignette(v, d.scaleX, d.scaleY, rule, &box))
            return p;
        for (uint32_t i = 0; i < kClearSamples; i++)
        {
            uint32_t x, y, word = 1;
            ClearSample(i, d.t0Width, d.t0Height, x, y);
            if (!texel(x, y, word) || word != 0)
                return p;
        }
        p.fill = Fill::Vignette;
        p.box = box;
        p.argb = v[0].argb;
        CoverFrame(box, d.posScale, p.posScale);
        return p;
    }

    // A full-screen layer short of the screen's edges: the fade to black
    // before a load (FB000000 at its darkest, x +-1.121 and y +-0.972 of the
    // 16:9 band: the career's EndingNis03 into the storyfmv_pin11 movie, the
    // last prologue race's, and the loads round the career's races) and the
    // dim over the loading screen (B8000000, x +-1.104, y -0.978..0.985):
    // one front-end quad of one colour (any alpha: it fades), over the game's
    // 32x32 white texture, centred in the band and covering 95% of it or more
    // each way, but short of the screen's edge in x or y. On a 16:9 screen a
    // thin strip of the scene shows at its top and bottom (10 rows of 720;
    // a 360's own output has it, a period TV's overscan hid it); under Vert+
    // the scene rendered above and below the band shows too (6.4% of a
    // MacBook's height at each end, a black box over the middle of the
    // picture for the fade's ~1.5 s), under Hor+ beside it (x +-1.121 of the
    // band ends 17% short of a 21:9 screen's sides). None of the other rules
    // takes it: the widen rule wants +-1.2 of the band (the fades and dims at
    // +-1.29), the black frame rule 0.90..0.99 (and alpha F0 or more), the
    // cover rule both band edges. The quad is stretched to the screen's
    // edges (its box to +-kFillEdge: CoverFrame), on every screen shape, 16:9
    // too (the period TV's look, as the results screen's overscan rows and
    // the Showcase's vignette). It is flat, so nothing is distorted. The
    // nearest draws it must not take: the intro's black frames (x +-0.970:
    // the black frame rule's), the Blacklist bio's panel (0.976 x 0.985, the
    // black frame rule's once opaque; during its fade-in this takes it, so
    // its border no longer shows then either), the title's key art and the
    // results picture (x +-1.104 but textured), the vid cam's overlays (x
    // +-0.70) and dialog boxes (x +-0.58), the pause dim's side gradients
    // (alpha varying across them), and any layer already past the screen's
    // edges (nothing short to stretch).
    struct LayerRule
    {
        float centreMax = 0.01f;       // |the box's centre|, each axis, band units
        float extentMin = 0.95f;       // half its width and half its height, band units
        float shortMin = 1e-3f;        // band units short of the screen's edge (1/scale) that count
        float cornerTolerance = 1e-3f;  // clip units, as Rule's
    };

    inline bool MatchLayer(const Vertex v[4], float scaleX, float scaleY, const LayerRule& rule = {}, Box* box = nullptr)
    {
        if (!(scaleX > 0.0f && scaleY > 0.0f))
            return false;
        for (int i = 1; i < 4; i++)
            if (v[i].argb != v[0].argb)
                return false;
        Box b;
        if (!RectangleBox(v, rule.cornerTolerance, b))
            return false;
        const float midX = (b.x0 + b.x1) * 0.5f / scaleX, midY = (b.y0 + b.y1) * 0.5f / scaleY;
        const float halfX = (b.x1 - b.x0) * 0.5f / scaleX, halfY = (b.y1 - b.y0) * 0.5f / scaleY;
        // Its nearer reach in each axis against the screen's edge (clip 1 =
        // 1/scale band units).
        const float reachX = std::min(-b.x0, b.x1), reachY = std::min(-b.y0, b.y1);
        // Centred in each axis it is short of the screen's edges in; an axis
        // already past both of them can be off-centre (the dialogs' dim, x
        // -1.307..1.280 of the band, y -0.999..0.995: GitHub #87), nothing
        // there moves into view.
        const bool pastX = reachX >= 1.0f, pastY = reachY >= 1.0f;
        if (!((pastX || std::fabs(midX) <= rule.centreMax) && (pastY || std::fabs(midY) <= rule.centreMax)))
            return false;
        if (!(halfX >= rule.extentMin && halfY >= rule.extentMin))
            return false;
        if (!(reachX < 1.0f - rule.shortMin * scaleX || reachY < 1.0f - rule.shortMin * scaleY))
            return false;
        if (box)
            *box = b;
        return true;
    }

    // The plan for a layer: the draw's posScale stretched so its box reaches
    // past every screen edge (CoverFrame), no rectangles. Candidate's
    // register checks (one quad, the front-end shader, left alone by the
    // other rules, alpha-blended or opaque, over the 32x32 white texture).
    template <class ReadWord>
    Plan PlanLayer(const Draw& d, ReadWord&& read, const LayerRule& rule = {})
    {
        Plan p;
        if (!Candidate(d))
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
        if (!MatchLayer(v, d.scaleX, d.scaleY, rule, &box))
            return p;
        p.fill = Fill::Layer;
        p.box = box;
        p.argb = v[0].argb;
        CoverFrame(box, d.posScale, p.posScale);
        return p;
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
            p.rectCount = BlackoutRects(box, d.posScale, d.hostW, d.hostH, d.scissor, p.rects);
        }
        else
            return p;
        p.box = box;
        p.argb = v[0].argb;
        CoverFrame(box, d.posScale, p.posScale);
        return p;
    }

    // An edge piece (GitHub #87): menu art authored a little past the 16:9
    // frame's top or bottom edge so that it bleeds off a 16:9 screen, ending
    // short of a taller screen's. The Options screens' black graffiti header
    // (Options.fng, t0 15299000 512x128) is one front-end quad at band y
    // 0.508..1.027 once it has slid in: on a 16:9 screen (and a 360's) its own
    // top edge lies off the screen, but under Vert+ the screen goes on to
    // 1/sv (1.117 of the band on a 16:10 MacBook) and the edge showed as a
    // straight cut with the 3D scene above it. Such a quad (an axis-aligned
    // rectangle of the front-end shader the other rules left alone, crossing
    // one band edge by
    // pastMin..pastMax, its other edge inside the band, short of the screen's
    // edge) is drawn as two parts that meet on one pixel edge (Edge): the draw
    // itself, scissored to stop at a pivot row, and one copy of it over the
    // rows from there to the screen's edge, stretched in y about the pivot so
    // that the quad's own edge reaches kFillEdge. The pivot lies sliceFrac of
    // the quad's height inside its edge (never past the band edge), so the
    // stretch is gentle: 1.86x for the header (stretching only its 0.027 past
    // the band, 4.4x, drew its ragged top as vertical streaks). Under Vert+
    // only; at 16:9 nothing changes. Thresholds in band units. The nearest
    // draws it must not take: the vid-cam bars (the bar rule's: past both
    // edges), the fades and dims (past both edges), the parked white box and
    // triangle (wholly outside the band: clipped, never left alone), a text
    // batch (more than one quad), the Options hub's graffiti tags (the
    // raise's, asked first: mid-reveal their quads cross the band edge too).
    struct EdgePieceRule
    {
        float pastMin = 0.01f;   // past the band's edge by at least this
        float pastMax = 0.20f;   // and at most this (graffiti wipes reach 1.23..3.3: past the screen anyway)
        float shortMin = 1e-3f;  // clip units short of the screen's edge that count
        float cornerTolerance = 1e-3f;
    };
    constexpr float kEdgeSliceFrac = 0.25f;  // the pivot: this share of the quad's height inside its edge

    // Which band edge a quad's box crosses: +1 (clip +y), -1, or 0.
    inline int MatchEdgePiece(const Vertex v[4], float scaleX, float scaleY, const EdgePieceRule& rule = {}, Box* box = nullptr)
    {
        if (!(scaleX > 0.0f && scaleY > 0.0f && scaleY < 1.0f))
            return 0;
        Box b;
        if (!RectangleBox(v, rule.cornerTolerance, b))
            return 0;
        const float lo = b.y0 / scaleY, hi = b.y1 / scaleY;
        const bool up = hi >= 1.0f + rule.pastMin && hi <= 1.0f + rule.pastMax && b.y1 < 1.0f - rule.shortMin && lo > -1.0f && lo < 1.0f;
        const bool down = -lo >= 1.0f + rule.pastMin && -lo <= 1.0f + rule.pastMax && b.y0 > -1.0f + rule.shortMin && hi < 1.0f && hi > -1.0f;
        if (up == down)
            return 0;
        if (box)
            *box = b;
        return up ? 1 : -1;
    }

    // The register checks for an edge piece: Candidate's, over any t0 (a
    // picture or the white texture), under Vert+ only.
    inline bool EdgePieceCandidate(const Draw& d)
    {
        const float* c = d.c;
        const bool yScale = d.frontEndY > 0.0f && std::fabs(std::fabs(c[5]) - d.frontEndY) <= d.frontEndY * 1e-5f;
        return d.scaleY < 1.0f && !d.occlusion && d.prim == 13 && d.count == 4 && d.frontEndShader && d.leftAlone && c[3] == 0.0f &&
            c[4] == 0.0f && c[6] == 0.0f && c[7] == 0.0f && yScale && (d.vte & 0xF) == 0xF &&
            (d.blend0 == 0x07060706u || d.blend0 == 0x00010001u);
    }

    // The pivot of an edge piece, clip y: sliceFrac of its height inside its
    // edge on `side`, and never past the band edge (+-scaleY).
    inline float EdgePiecePivot(int side, const Box& b, float scaleY, float sliceFrac = kEdgeSliceFrac)
    {
        const float h = b.y1 - b.y0;
        return side > 0 ? std::min(b.y1 - sliceFrac * h, scaleY) : std::max(b.y0 + sliceFrac * h, -scaleY);
    }

    // The plan for an edge piece: the copy's posScale (y' = q + (y - q) k
    // about the pivot q, k taking the quad's edge to +-kFillEdge, folded in as
    // CoverFrame's), its rectangle (the rows from the pivot to the screen's
    // edge, inside the draw's scissor) and the draw's own scissor (the rest).
    inline bool EdgePieceRects(int side, const Box& b, const float ps[4], float hostW, float hostH, const Rect& scissor, float scaleY,
        Plan& p, float sliceFrac = kEdgeSliceFrac)
    {
        const float e = kFillEdge, q = EdgePiecePivot(side, b, scaleY, sliceFrac), end = side > 0 ? b.y1 : b.y0;
        const float k = (side > 0 ? e - q : -e - q) / (end - q);
        if (!(k > 1.0f) || !std::isfinite(k))
            return false;
        p.posScale[0] = ps[0];
        p.posScale[1] = k * ps[1];
        p.posScale[2] = ps[2];
        p.posScale[3] = ps[3] + ps[1] * q * (1.0f - k);
        const Rect past = Intersect(side > 0 ? ClipToHost(-e, e, q, e, ps, hostW, hostH) : ClipToHost(-e, e, -e, q, ps, hostW, hostH), scissor);
        const Rect own = Intersect(side > 0 ? ClipToHost(-e, e, -e, q, ps, hostW, hostH) : ClipToHost(-e, e, q, e, ps, hostW, hostH), scissor);
        if (Empty(past))
            return false;
        p.rects[0] = past;
        p.rectCount = 1;
        p.clipOwn = true;
        p.own = own;
        return true;
    }

    template <class ReadWord>
    Plan PlanEdgePiece(const Draw& d, ReadWord&& read, const EdgePieceRule& rule = {})
    {
        Plan p;
        if (!EdgePieceCandidate(d))
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
        const int side = MatchEdgePiece(v, d.scaleX, d.scaleY, rule, &box);
        if (!side || !EdgePieceRects(side, box, d.posScale, d.hostW, d.hostH, d.scissor, d.scaleY, p))
            return Plan{};
        p.fill = Fill::EdgePiece;
        p.box = box;
        p.argb = v[0].argb;
        return p;
    }

    // A raise (GitHub #87): the Options hub's two graffiti tags (t0 1470B000,
    // 128x128 k_8_8_8_8, x -0.658..-0.437 and 0.322..0.653) are pictures the
    // game reveals in place, like paint dripping: each frame the quad's
    // bottom edge moves down and its texture coordinates move with it, so the
    // texture's top row (v = 0) stays at band y 0.974 (left) and 0.950
    // (right), and above it the quad draws nothing (v < 0); at rest the quad
    // is the picture (v 0.004..0.996). On a 16:9 screen the tags' tops sit 9
    // and 18 rows below the top of 720 (the 360's own picture); under Vert+
    // the screen goes on to 1/sv of the band and the gap above them was a
    // band of 3D scene (0.143 and 0.167 of the band on a 16:10 MacBook). Such
    // a quad (one axis-aligned rectangle of the front-end shader the other
    // rules left alone, opaque (every vertex alpha >= minAlpha), over a
    // 32-bit picture of at least minTexels a side, its u spanning the
    // picture's width, v rising downward across it with v = 0 (the art's top)
    // at artTopMin..1 of the band and v = 1 inside it, at least minArtHeight
    // tall), once seen mid-reveal (the quad's top edge past the art's top) and
    // from then on while it is drawn, is moved up, nothing else, by exactly
    // what puts that top on the
    // screen's top edge (clip 1): the same amount every frame of the reveal
    // and at rest, so the game's reveal starts at the screen's top and its
    // front moves down as before. Under Vert+ only (16:9: nothing). The
    // bottom edge mirrored. Thresholds in band units. The nearest draws it
    // must not take: the header (its art past the band: the edge piece's),
    // the Rap Sheet's and the bio's panels (the black frame rule's, never
    // left alone), the dialogs' boxes (the white 32x32 texture), the HUD's
    // pieces (too small), a translucent glow (alpha), text (several quads).
    struct RaiseRule
    {
        float artTopMin = 0.88f;     // the art's top (v = 0) at or past this, and inside the band
        float minArtHeight = 0.30f;  // v 0..1 at least this tall
        uint32_t minAlpha = 0xF0;
        uint32_t minTexels = 64;
        float minUSpan = 0.95f;      // u across the quad: (nearly) the picture's whole width
        float revealMin = 0.02f;     // the quad's edge past the art's top by this: a reveal under way
        uint32_t inkAlpha = 0x40;    // a texel at least this opaque is ink (the art's visible top: FirstInkRow)
        float cornerTolerance = 1e-3f;
    };

    // One ePoly corner for the raise: clip x and y, colour, texture u and v.
    struct UvVertex
    {
        float x, y;
        uint32_t argb;
        float u, v;
    };

    // The band y of v = 0 (the art's top) and of v = 1, for a quad whose v
    // changes along y only; false if it doesn't (or isn't a rectangle).
    inline bool ArtSpan(const UvVertex q[4], float scaleY, int& side, float& top, float& bottom, const RaiseRule& rule = {}, float inkV = 0.0f)
    {
        Vertex v[4];
        for (int i = 0; i < 4; i++)
            v[i] = { q[i].x, q[i].y, q[i].argb };
        Box b;
        if (!RectangleBox(v, rule.cornerTolerance, b) || b.y1 - b.y0 <= 0.0f)
            return false;
        // v and u at the box's top (y1) and bottom (y0) rows, the same along each row.
        float vTop = 0, vBottom = 0, uLo = 1e30f, uHi = -1e30f;
        int nTop = 0, nBottom = 0;
        for (int i = 0; i < 4; i++)
        {
            uLo = std::min(uLo, q[i].u);
            uHi = std::max(uHi, q[i].u);
            if (std::fabs(q[i].y - b.y1) <= rule.cornerTolerance)
            {
                if (nTop++ && std::fabs(q[i].v - vTop) > 1e-4f)
                    return false;
                vTop = q[i].v;
            }
            else
            {
                if (nBottom++ && std::fabs(q[i].v - vBottom) > 1e-4f)
                    return false;
                vBottom = q[i].v;
            }
        }
        if (nTop != 2 || nBottom != 2 || uHi - uLo < rule.minUSpan || vTop == vBottom)
            return false;
        // y as a function of v (linear across the quad).
        auto yAt = [&](float vv) { return b.y1 + (vv - vTop) * (b.y0 - b.y1) / (vBottom - vTop); };
        const float yv0 = yAt(inkV) / scaleY, yv1 = yAt(1.0f) / scaleY;
        side = yv0 > yv1 ? 1 : -1;  // the art's top toward clip +y: the screen's top edge (either way up on screen)
        top = yv0;
        bottom = yv1;
        return true;
    }

    // The raise for a quad: its clip y shift (0: none). revealing: the art's
    // top lies inside the quad, below its top edge (v < 0 above it): a reveal
    // under way, which tells these tags from a still picture of the same
    // shape (the title's logo, 1ACA4000, has its top at 0.897 of the band
    // too). gpu/renderer.cpp raises a quad only once it has been seen
    // revealing, and from then on while it is drawn.
    // inkV: the v of the art's visible top (FirstInkV; 0: the picture's top row).
    inline float MatchRaise(const UvVertex q[4], float scaleX, float scaleY, const RaiseRule& rule = {}, bool* revealing = nullptr, float inkV = 0.0f)
    {
        if (!(scaleX > 0.0f && scaleY > 0.0f && scaleY < 1.0f))
            return 0.0f;
        for (int i = 0; i < 4; i++)
            if ((q[i].argb >> 24) < rule.minAlpha)
                return 0.0f;
        int side = 0;
        float top = 0, bottom = 0;
        if (!ArtSpan(q, scaleY, side, top, bottom, rule, inkV))
            return 0.0f;
        const float t = side * top, bt = side * bottom;  // measured outward
        if (!(t >= rule.artTopMin && t < 1.0f && bt > -1.0f && t - bt >= rule.minArtHeight))
            return 0.0f;
        if (revealing)
        {
            float edge = side > 0 ? -1e30f : 1e30f;
            for (int i = 0; i < 4; i++)
                edge = side > 0 ? std::max(edge, q[i].y) : std::min(edge, q[i].y);
            *revealing = side * edge / scaleY > t + rule.revealMin;
        }
        return side * (1.0f - t * scaleY);  // clip units: the art's top to clip +-1
    }

    // The register checks for a raise: Candidate's, over a 32-bit picture.
    inline bool RaiseCandidate(const Draw& d, const RaiseRule& rule = {})
    {
        const float* c = d.c;
        const bool yScale = d.frontEndY > 0.0f && std::fabs(std::fabs(c[5]) - d.frontEndY) <= d.frontEndY * 1e-5f;
        return d.scaleY < 1.0f && !d.occlusion && d.prim == 13 && d.count == 4 && d.frontEndShader && d.leftAlone && c[3] == 0.0f &&
            c[4] == 0.0f && c[6] == 0.0f && c[7] == 0.0f && yScale && (d.vte & 0xF) == 0xF &&
            (d.blend0 == 0x07060706u || d.blend0 == 0x00010001u) && d.t0Sampled && d.t0Format == 6 && d.t0Width >= rule.minTexels &&
            d.t0Height >= rule.minTexels;
    }

    // The plan for a raise: the draw's posScale with its y offset moved by the
    // shift (NDC y = (y + shift) ps1 + ps3), no rectangles. read: as PlanFill's
    // (words 4 and 5 of an ePoly are its first u and v).
    // The first row of a 32-bit picture, from its top (v = 0), with a texel
    // at least inkAlpha opaque (alpha: the word's top byte, ARGB as the
    // renderer reads it); -1 if none or unreadable. The tags' picture has
    // clear rows above its ink: raising v = 0 to the screen's top left a
    // few rows of sky above the tag (seen in a test build's screenshot).
    template <class ReadTexel>
    int FirstInkRow(ReadTexel&& texel, uint32_t width, uint32_t height, const RaiseRule& rule = {})
    {
        for (uint32_t y = 0; y < height; y++)
            for (uint32_t x = 0; x < width; x++)
            {
                uint32_t w = 0;
                if (!texel(x, y, w))
                    return -1;
                if ((w >> 24) >= rule.inkAlpha)
                    return int(y);
            }
        return -1;
    }

    // The v the raise puts on the screen's top edge: the first ink row's
    // centre (with bilinear filtering its top half blends with the clear row
    // above it, so its centre is the first row drawn at full ink).
    inline float FirstInkV(int row, uint32_t height)
    {
        return row > 0 && height ? (float(row) + 0.5f) / float(height) : 0.0f;
    }

    // admit(revealing): whether to raise it (gpu/renderer.cpp: seen revealing).
    // inkV: FirstInkV of its picture.
    template <class ReadWord, class Admit>
    Plan PlanRaise(const Draw& d, ReadWord&& read, Admit&& admit, float inkV, const RaiseRule& rule = {})
    {
        Plan p;
        if (!RaiseCandidate(d, rule))
            return p;
        const float* c = d.c;
        UvVertex q[4];
        for (uint32_t k = 0; k < 4; k++)
        {
            uint32_t w[6];
            for (uint32_t j = 0; j < 6; j++)
                if (!read(k, j, w[j]))
                    return p;
            float f[6];
            std::memcpy(f, w, sizeof(f));
            q[k] = { f[0] * c[0] + f[1] * c[1] + f[2] * c[2] + c[3], f[0] * c[4] + f[1] * c[5] + f[2] * c[6] + c[7], w[3], f[4], f[5] };
        }
        bool revealing = false;
        const float shift = MatchRaise(q, d.scaleX, d.scaleY, rule, &revealing, inkV);
        if (shift == 0.0f || !admit(revealing))
            return p;
        p.fill = Fill::Raise;
        p.argb = q[0].argb;
        p.box = { std::min(q[0].x, q[2].x), std::max(q[0].x, q[2].x), std::min(q[0].y, q[2].y), std::max(q[0].y, q[2].y) };
        std::memcpy(p.posScale, d.posScale, sizeof(p.posScale));
        p.posScale[3] += d.posScale[1] * shift;
        return p;
    }
}
