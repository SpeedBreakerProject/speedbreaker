// Tests for gpu::bars (runtime/gpu/edge_bars.h): which front-end quads are
// the police vid-cam's side bars or the intro's black frame, what they fill
// past the 16:9 band, the stretched copy's posScale, the band clip's rows,
// and the whole plan the renderer asks for (PlanFill) on the draws as
// dumped. From the repo root:
//   clang++ -std=c++20 -O2 -UNDEBUG -Iruntime tests/edge_bars_test.cpp -o build/edge_bars_test
//   build/edge_bars_test
// The match cases run again under deliberately wrong thresholds (Planted):
// each must fail at least one case, so a threshold nobody tests can't drift.
// The fill, cover, colour, band-row and plan code was checked the same way,
// by planting wrong code in a copy of the header (each mutation fails a
// check).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>

// On Windows gpu/renderer.cpp includes the header after <windows.h>, whose
// macros (WIN32_LEAN_AND_MEAN and NOMINMAX as platform/win32/posix.h sets
// them) turn some words into nothing or a number: "auto near = ..." was a
// Windows-only compile break. The header compiles here under the same ones.
#define near
#define far
#define pascal
#define cdecl
#define IN
#define OUT
#define OPTIONAL
#define CONST const
#define TRUE 1
#define FALSE 0
#define DELETE (0x00010000L)
#define ERROR 0
#define OPAQUE 2
#define TRANSPARENT 1
#define ABSOLUTE 1
#define RELATIVE 2
#define DIFFERENCE 11
#include <gpu/edge_bars.h>
#undef near
#undef far
#undef pascal
#undef cdecl
#undef IN
#undef OUT
#undef OPTIONAL
#undef CONST
#undef TRUE
#undef FALSE
#undef DELETE
#undef ERROR
#undef OPAQUE
#undef TRANSPARENT
#undef ABSOLUTE
#undef RELATIVE
#undef DIFFERENCE

using namespace gpu::bars;

static int g_failures = 0;
static bool g_quiet = false;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; if (!g_quiet) { printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } } while (0)

// The screen shapes (view scale x, y), as picture_fit gives them.
struct Shape
{
    const char* name;
    float sx, sy;
};
static const Shape kShapes[] = {
    { "1280x800 (Deck)", 1.0f, 0.9f },
    { "1200x800 (3:2)", 1.0f, 0.84375f },
    { "1024x768 (4:3)", 1.0f, 0.75f },
    { "2732x2048 (iPad Pro)", 1.0f, 0.750366f },
    { "1280x720 (16:9)", 1.0f, 1.0f },
    { "2560x1080 (21:9)", 0.75f, 1.0f },
    { "3440x1440", 0.744186f, 1.0f },
    { "2880x810 (32:9)", 0.5f, 1.0f },
};

// A rectangle from band units at a shape, vertices in order around it
// (the game's: (x0,y0) (x1,y0) (x1,y1) (x0,y1)).
struct Quad
{
    Vertex v[4];
};
static Quad Rectangle(float bx0, float bx1, float by0, float by1, float sx, float sy, uint32_t argb)
{
    const float x0 = bx0 * sx, x1 = bx1 * sx, y0 = by0 * sy, y1 = by1 * sy;
    return { { { x0, y0, argb }, { x1, y0, argb }, { x1, y1, argb }, { x0, y1, argb } } };
}

static const char* Name(Side s)
{
    return s == Side::Right ? "right" : s == Side::Left ? "left" : "none";
}

static void Match(const char* what, const Quad& q, float sx, float sy, Side want, const Rule& rule)
{
    Box box{};
    const Side got = MatchSideBar(q.v, sx, sy, rule, &box);
    CHECK(got == want, "%s at x %.4f y %.4f: %s, want %s", what, sx, sy, Name(got), Name(want));
}

// Every match case. Returns the failures it added (for Planted).
static int MatchCases(const Rule& rule)
{
    const int before = g_failures;
    constexpr uint32_t kBar = 0xFD000000;
    for (const Shape& s : kShapes)
    {
        // The two side bars, as measured at every shape (band units).
        Match("right bar", Rectangle(0.651f, 1.406f, -1.024f, 1.039f, s.sx, s.sy, kBar), s.sx, s.sy, Side::Right, rule);
        Match("left bar", Rectangle(-1.410f, -0.655f, -1.027f, 1.035f, s.sx, s.sy, kBar), s.sx, s.sy, Side::Left, rule);
        // The bezel quadrants: textured grey, one band edge.
        Match("bezel B", Rectangle(-0.001f, 0.654f, -1.027f, 0.003f, s.sx, s.sy, 0xFB7D7D7D), s.sx, s.sy, Side::None, rule);
        Match("bezel T", Rectangle(-0.657f, -0.001f, 0.003f, 1.033f, s.sx, s.sy, 0xFB7D7D7D), s.sx, s.sy, Side::None, rule);
        // ... and the same shape in the bar's black: still one edge only.
        Match("black bezel shape", Rectangle(-0.001f, 0.654f, 0.003f, 1.033f, s.sx, s.sy, kBar), s.sx, s.sy, Side::None, rule);
        // The rest of the vid cam: noise, dark overlay, rolling bar.
        Match("noise overlay", Rectangle(-0.692f, 0.709f, -1.058f, 1.050f, s.sx, s.sy, 0x3C727272), s.sx, s.sy, Side::None, rule);
        Match("noise overlay in black", Rectangle(-0.692f, 0.709f, -1.058f, 1.050f, s.sx, s.sy, kBar), s.sx, s.sy, Side::None, rule);
        Match("dark overlay", Rectangle(-0.712f, 0.668f, -0.893f, 0.908f, s.sx, s.sy, 0x3C000000), s.sx, s.sy, Side::None, rule);
        Match("rolling bar", Rectangle(-0.68f, 0.70f, -1.04f, -0.90f, s.sx, s.sy, 0x147E7E7E), s.sx, s.sy, Side::None, rule);
        // Fades and dims (the existing full-width rule's), the pause dim.
        Match("fade", Rectangle(-1.29f, 1.29f, -1.30f, 1.30f, s.sx, s.sy, 0xFF000000), s.sx, s.sy, Side::None, rule);
        Match("pause dim centre", Rectangle(-0.53f, 0.57f, -1.91f, 1.91f, s.sx, s.sy, 0xC8000000), s.sx, s.sy, Side::None, rule);
        Quad grad = Rectangle(0.569f, 1.117f, -1.905f, 1.913f, s.sx, s.sy, 0xC8000000);
        grad.v[1].argb = grad.v[2].argb = 0x00000000;  // fades to nothing at the outer edge
        Match("pause dim side gradient", grad, s.sx, s.sy, Side::None, rule);
        Match("pause gradient shape in black", Rectangle(0.569f, 1.117f, -1.905f, 1.913f, s.sx, s.sy, kBar), s.sx, s.sy, Side::None, rule);
        // Graffiti wipes, brushes and splats; the minimap; small pieces.
        Match("wipe 1486D000", Rectangle(-0.34f, 1.09f, -3.30f, -0.75f, s.sx, s.sy, 0xFF000000), s.sx, s.sy, Side::None, rule);
        Match("wipe 1486D000 right", Rectangle(0.70f, 1.60f, -2.55f, 2.50f, s.sx, s.sy, 0xFF000000), s.sx, s.sy, Side::None, rule);
        Match("brush 15299000", Rectangle(-0.90f, 0.90f, -1.07f, -0.58f, s.sx, s.sy, 0xFF000000), s.sx, s.sy, Side::None, rule);
        Match("splat 15B4D000", Rectangle(-0.69f, -0.22f, -1.234f, -0.39f, s.sx, s.sy, 0xFE000000), s.sx, s.sy, Side::None, rule);
        Match("minimap piece", Rectangle(-0.73f, -0.46f, -1.02f, -0.53f, s.sx, s.sy, 0xFE7F7F7F), s.sx, s.sy, Side::None, rule);
        Match("small T piece", Rectangle(0.62f, 0.69f, 0.95f, 1.07f, s.sx, s.sy, kBar), s.sx, s.sy, Side::None, rule);
        Match("loading band", Rectangle(-1.29f, 1.29f, -0.30f, 0.30f, s.sx, s.sy, 0xFF000000), s.sx, s.sy, Side::None, rule);
        // The rest of the front-end quads catalogued while investigating.
        Match("corner bracket", Rectangle(0.568f, 0.637f, 0.723f, 0.846f, s.sx, s.sy, 0x777E7E7E), s.sx, s.sy, Side::None, rule);
        Match("corner bracket slid out", Rectangle(1.04f, 1.11f, 0.723f, 0.846f, s.sx, s.sy, 0x777E7E7E), s.sx, s.sy, Side::None, rule);
        Match("REC dot", Rectangle(-0.590f, -0.521f, 0.639f, 0.762f, s.sx, s.sy, 0xFB7E0505), s.sx, s.sy, Side::None, rule);
        Match("garage wipe 148CD000", Rectangle(-1.67f, 1.29f, -0.40f, 2.50f, s.sx, s.sy, 0xFF000000), s.sx, s.sy, Side::None, rule);
        Match("grey wipe 14CB0000", Rectangle(-1.20f, 1.97f, -1.196f, 0.18f, s.sx, s.sy, 0xFF7F7F7F), s.sx, s.sy, Side::None, rule);
        Match("grey splat 1470B000", Rectangle(0.36f, 0.70f, 0.89f, 1.64f, s.sx, s.sy, 0xFE7F7F7F), s.sx, s.sy, Side::None, rule);
        Match("loading glow 19A5A000", Rectangle(-0.29f, 0.05f, -1.03f, -0.44f, s.sx, s.sy, 0xFD2E2664), s.sx, s.sy, Side::None, rule);
        Match("loading glow 1AD58000", Rectangle(-1.29f, 1.29f, -1.05f, 1.05f, s.sx, s.sy, 0x3C000000), s.sx, s.sy, Side::None, rule);
        Match("minimap piece L", Rectangle(-1.016f, -0.73f, -0.54f, -0.04f, s.sx, s.sy, 0xFE7F7F7F), s.sx, s.sy, Side::None, rule);
        Match("menu glyph L", Rectangle(-1.05f, -0.95f, -0.70f, -0.50f, s.sx, s.sy, 0xFE7F7F7F), s.sx, s.sy, Side::None, rule);
        Match("parked additive 19B04000", Rectangle(0.80f, 1.10f, -1.40f, -1.10f, s.sx, s.sy, 0xFF000000), s.sx, s.sy, Side::None, rule);
        Match("title key art", Rectangle(-1.105f, 1.105f, -1.105f, 1.105f, s.sx, s.sy, 0xFF000000), s.sx, s.sy, Side::None, rule);
        // The FD000000 dialog boxes over the vid cam (cam_329; same colour
        // as the bars, inside the band).
        Match("cam dialog box 1", Rectangle(-0.578f, 0.586f, -0.075f, 0.462f, s.sx, s.sy, kBar), s.sx, s.sy, Side::None, rule);
        Match("cam dialog box 2", Rectangle(-0.578f, 0.586f, -0.167f, 0.569f, s.sx, s.sy, kBar), s.sx, s.sy, Side::None, rule);
        Match("cam dialog box 3", Rectangle(0.172f, 0.494f, -0.348f, -0.245f, s.sx, s.sy, kBar), s.sx, s.sy, Side::None, rule);
        // A black panel at one side inside the band (a menu's side column):
        // stops short of the band's top and bottom, so not a mask bar.
        Match("black side panel in the band", Rectangle(0.651f, 1.406f, -0.95f, 0.95f, s.sx, s.sy, kBar), s.sx, s.sy, Side::None, rule);
    }
    // Each threshold from both sides (the right bar at 4:3, one value
    // moved; the left bar mirrored), so a moved threshold fails a case.
    const float sx = 1.0f, sy = 0.75f;
    auto bar = [&](float in, float out, float bottom, float top, uint32_t argb = kBar) {
        return Rectangle(in, out, -bottom, top, sx, sy, argb);
    };
    auto mirrored = [&](float in, float out, float bottom, float top) { return Rectangle(-out, -in, -bottom, top, sx, sy, kBar); };
    Match("top 1.005", bar(0.651f, 1.406f, 1.024f, 1.005f), sx, sy, Side::None, rule);
    Match("top 1.015", bar(0.651f, 1.406f, 1.024f, 1.015f), sx, sy, Side::Right, rule);
    Match("top 1.095", bar(0.651f, 1.406f, 1.024f, 1.095f), sx, sy, Side::Right, rule);
    Match("top 1.105", bar(0.651f, 1.406f, 1.024f, 1.105f), sx, sy, Side::None, rule);
    Match("bottom 1.005", bar(0.651f, 1.406f, 1.005f, 1.039f), sx, sy, Side::None, rule);
    Match("bottom 1.105", bar(0.651f, 1.406f, 1.105f, 1.039f), sx, sy, Side::None, rule);
    Match("inner 0.495", bar(0.495f, 1.406f, 1.024f, 1.039f), sx, sy, Side::None, rule);
    Match("inner 0.505", bar(0.505f, 1.406f, 1.024f, 1.039f), sx, sy, Side::Right, rule);
    Match("inner 0.795", bar(0.795f, 1.406f, 1.024f, 1.039f), sx, sy, Side::Right, rule);
    Match("inner 0.805", bar(0.805f, 1.406f, 1.024f, 1.039f), sx, sy, Side::None, rule);
    Match("outer 1.195", bar(0.651f, 1.195f, 1.024f, 1.039f), sx, sy, Side::None, rule);
    Match("outer 1.205", bar(0.651f, 1.205f, 1.024f, 1.039f), sx, sy, Side::Right, rule);
    Match("outer 1.995", bar(0.651f, 1.995f, 1.024f, 1.039f), sx, sy, Side::Right, rule);
    Match("outer 2.005", bar(0.651f, 2.005f, 1.024f, 1.039f), sx, sy, Side::None, rule);
    Match("left inner 0.495", mirrored(0.495f, 1.410f, 1.027f, 1.035f), sx, sy, Side::None, rule);
    Match("left inner 0.805", mirrored(0.805f, 1.410f, 1.027f, 1.035f), sx, sy, Side::None, rule);
    Match("left outer 1.195", mirrored(0.655f, 1.195f, 1.027f, 1.035f), sx, sy, Side::None, rule);
    Match("left outer 2.005", mirrored(0.655f, 2.005f, 1.027f, 1.035f), sx, sy, Side::None, rule);
    Match("left top 1.105", mirrored(0.655f, 1.410f, 1.027f, 1.105f), sx, sy, Side::None, rule);
    Match("alpha EF", bar(0.651f, 1.406f, 1.024f, 1.039f, 0xEF000000), sx, sy, Side::None, rule);
    Match("alpha F0", bar(0.651f, 1.406f, 1.024f, 1.039f, 0xF0000000), sx, sy, Side::Right, rule);
    Match("alpha FF", bar(0.651f, 1.406f, 1.024f, 1.039f, 0xFF000000), sx, sy, Side::Right, rule);
    Match("blue 01", bar(0.651f, 1.406f, 1.024f, 1.039f, 0xFD000001), sx, sy, Side::None, rule);
    Match("red 01", bar(0.651f, 1.406f, 1.024f, 1.039f, 0xFD010000), sx, sy, Side::None, rule);
    Quad mixed = bar(0.651f, 1.406f, 1.024f, 1.039f);
    mixed.v[2].argb = 0xFE000000;
    Match("one vertex's colour differs", mixed, sx, sy, Side::None, rule);
    Quad off = bar(0.651f, 1.406f, 1.024f, 1.039f);
    off.v[2].x += 0.01f;
    Match("a vertex 0.01 off its corner", off, sx, sy, Side::None, rule);
    Quad nearly = bar(0.651f, 1.406f, 1.024f, 1.039f);
    nearly.v[2].x += 0.0005f;
    Match("a vertex 0.0005 off its corner", nearly, sx, sy, Side::Right, rule);
    Quad bowtie = bar(0.651f, 1.406f, 1.024f, 1.039f);
    std::swap(bowtie.v[1], bowtie.v[2]);
    Match("bow-tie order", bowtie, sx, sy, Side::None, rule);
    Quad repeated = bar(0.651f, 1.406f, 1.024f, 1.039f);
    repeated.v[3] = repeated.v[2];
    Match("a repeated vertex", repeated, sx, sy, Side::None, rule);
    return g_failures - before;
}

// The same cases under a wrong rule must fail at least one.
static void Planted(const char* what, const Rule& wrong)
{
    const int saved = g_failures;
    g_quiet = true;
    const int failed = MatchCases(wrong);
    g_quiet = false;
    g_failures = saved;
    printf("planted %-26s %d case(s) fail\n", what, failed);
    CHECK(failed > 0, "planted %s: every case still passes", what);
}

// The front end's posScale (renderer.cpp: 2 sx / w, 2 sy / h, 2 ox / w - 1,
// 2 oy / h - 1): viewport s(640,-360) o(640,360) in the 1280x2048 target.
static void FrontEndPs(float ps[4], float ox = 640.0f, float oy = 360.0f, float sy = -360.0f)
{
    ps[0] = 2.0f * 640.0f / 1280.0f;
    ps[1] = 2.0f * sy / 2048.0f;
    ps[2] = 2.0f * ox / 1280.0f - 1.0f;
    ps[3] = 2.0f * oy / 2048.0f - 1.0f;
}

static bool Same(const Rect& a, const Rect& b)
{
    return a.x0 == b.x0 && a.y0 == b.y0 && a.x1 == b.x1 && a.y1 == b.y1;
}

static void FillBar(const char* what, Side side, float bx0, float bx1, float sx, float sy, const float ps[4], float hostW, float hostH,
    const Rect& scissor, uint32_t wantN, const Rect* want)
{
    const Box box{ bx0 * sx, bx1 * sx, -1.024f * sy, 1.039f * sy };
    Rect got[2]{};
    const uint32_t n = FillRects(side, box, ps, hostW, hostH, scissor, sx, sy, got);
    printf("%s: %u rect(s)", what, n);
    for (uint32_t i = 0; i < n; i++)
        printf(" [%d,%d)x[%d,%d)", got[i].x0, got[i].x1, got[i].y0, got[i].y1);
    printf("\n");
    CHECK(n == wantN, "%s: %u rects, want %u", what, n, wantN);
    for (uint32_t i = 0; i < n && i < wantN; i++)
        CHECK(Same(got[i], want[i]), "%s: rect %u [%d,%d)x[%d,%d), want [%d,%d)x[%d,%d)", what, i, got[i].x0, got[i].x1, got[i].y0,
            got[i].y1, want[i].x0, want[i].x1, want[i].y0, want[i].y1);
}

// An independent oracle (pixel centres, no Edge()): a pixel of the scissor
// is filled exactly when its centre is outside the band on the bar's side:
// under Vert+ |y| > sy and on the bar's half (x > 0 right, x < 0 left);
// under Hor+, when the bar ends short of the screen edge, x > sx (right) or
// x < -sx (left). A pixel whose centre lies within 1/256 pixel of one of
// those edges is a tie (Edge() takes it, by the rasteriser's rule) and is
// skipped: the exact rectangles and the complement check cover ties.
static void Oracle(const char* what, Side side, float sx, float sy, const float ps[4], float hostW, float hostH, const Rect& scissor)
{
    const bool right = side == Side::Right;
    const Box box = right ? Box{ 0.651f * sx, 1.406f * sx, -1.024f * sy, 1.039f * sy } : Box{ -1.410f * sx, -0.655f * sx, -1.027f * sy, 1.035f * sy };
    Rect r[2]{};
    const uint32_t n = FillRects(side, box, ps, hostW, hostH, scissor, sx, sy, r);
    const bool horPlus = sy >= 1.0f && sx < 1.0f && (right ? box.x1 < 1.0f : box.x0 > -1.0f);
    // Host pixels per clip unit, for the tie distance.
    const float pxX = std::fabs(ps[0]) * hostW * 0.5f, pxY = std::fabs(ps[1]) * hostH * 0.5f;
    auto tie = [](float d, float px) { return std::fabs(d) * px < 1.0f / 256.0f; };
    int wrong = 0, ties = 0;
    for (int32_t y = scissor.y0; y < scissor.y1; y++)
    {
        const float cy = ((float(y) + 0.5f) / hostH * 2.0f - 1.0f - ps[3]) / ps[1];
        for (int32_t x = scissor.x0; x < scissor.x1; x++)
        {
            const float cx = ((float(x) + 0.5f) / hostW * 2.0f - 1.0f - ps[2]) / ps[0];
            bool want = false, onEdge = false;
            if (sy < 1.0f)
            {
                want = std::fabs(cy) > sy && (right ? cx > 0.0f : cx < 0.0f);
                onEdge = tie(std::fabs(cy) - sy, pxY) || tie(cx, pxX);
            }
            else if (horPlus)
            {
                want = right ? cx > sx : cx < -sx;
                onEdge = tie(std::fabs(cx) - sx, pxX);
            }
            bool got = false;
            for (uint32_t i = 0; i < n; i++)
                got = got || (x >= r[i].x0 && x < r[i].x1 && y >= r[i].y0 && y < r[i].y1);
            if (onEdge)
                ties++;
            else
                wrong += got != want;
        }
    }
    printf("oracle %s: %d pixel(s) wrong (%d on an edge, skipped)\n", what, wrong, ties);
    CHECK(wrong == 0, "%s: %d pixels differ from the pixel-centre oracle", what, wrong);
}

// The band clip's rows (gpu::bars::BandRows, which renderer.cpp clips the
// rest of the front end with) are the rows whose centres lie inside the
// band: checked against pixel centres, ties (within 1/256 pixel) skipped.
static void BandOracle(const char* what, float sy, const float ps[4], float hostH, int32_t rows)
{
    const Rows band = BandRows(-sy, sy, ps[1], ps[3], hostH);
    const float pxY = std::fabs(ps[1]) * hostH * 0.5f;
    int wrong = 0, ties = 0;
    for (int32_t y = 0; y < rows; y++)
    {
        const float cy = ((float(y) + 0.5f) / hostH * 2.0f - 1.0f - ps[3]) / ps[1];
        if (std::fabs(std::fabs(cy) - sy) * pxY < 1.0f / 256.0f)
        {
            ties++;
            continue;
        }
        wrong += (std::fabs(cy) < sy) != (y >= band.top && y < band.bottom);
    }
    printf("band rows %s: [%d,%d), %d row(s) wrong (%d on an edge, skipped)\n", what, band.top, band.bottom, wrong, ties);
    CHECK(wrong == 0, "%s: %d band rows differ from the pixel-centre oracle", what, wrong);
}

// Under Vert+ the fills and the band clip's rows split the scissor's rows
// with no gap and no overlap: a bar's half and a black frame's whole width.
static void Complement(const char* what, float sy, const float ps[4], float hostW, float hostH, const Rect& scissor)
{
    const Rows band = BandRows(-sy, sy, ps[1], ps[3], hostH);
    const int32_t top = band.top, bottom = band.bottom;
    const Box box{ 0.651f, 1.406f, -1.024f * sy, 1.039f * sy };
    Rect r[2]{};
    const uint32_t n = FillRects(Side::Right, box, ps, hostW, hostH, scissor, 1.0f, sy, r);
    int wrong = 0;
    for (int32_t y = scissor.y0; y < scissor.y1; y++)
    {
        const bool band = y >= top && y < bottom;
        bool fill = false;
        for (uint32_t i = 0; i < n; i++)
            fill = fill || (y >= r[i].y0 && y < r[i].y1);
        wrong += band == fill;
    }
    // The black frame's fill, every column: each pixel of the scissor is in
    // exactly one of the band's rows and the fill.
    Rect k[2]{};
    const uint32_t kn = BlackoutRects(ps, hostW, hostH, scissor, 1.0f, sy, k);
    int wrongPx = 0;
    for (int32_t y = scissor.y0; y < scissor.y1; y++)
        for (int32_t x = scissor.x0; x < scissor.x1; x++)
        {
            int in = (y >= top && y < bottom) ? 1 : 0;
            for (uint32_t i = 0; i < kn; i++)
                in += (x >= k[i].x0 && x < k[i].x1 && y >= k[i].y0 && y < k[i].y1) ? 1 : 0;
            wrongPx += in != 1;
        }
    printf("complement %s: band rows [%d,%d), %d row(s) wrong; black frame %d pixel(s) wrong\n", what, top, bottom, wrong, wrongPx);
    CHECK(wrong == 0, "%s: %d rows both or neither in the band clip and the fill", what, wrong);
    CHECK(wrongPx == 0, "%s: %d pixels both or neither in the band clip and the black frame's fill", what, wrongPx);
}

static void Cover(const char* what, float bx0, float bx1, float sx, float sy, const float ps[4])
{
    const Box b{ bx0 * sx, bx1 * sx, -1.024f * sy, 1.039f * sy };
    float q[4];
    CoverFrame(b, ps, q);
    // The box's corners land where this draw's viewport puts clip +-1.02.
    const float e = 1.02f;
    const float gx0 = b.x0 * q[0] + q[2], gx1 = b.x1 * q[0] + q[2], gy0 = b.y0 * q[1] + q[3], gy1 = b.y1 * q[1] + q[3];
    const float wx0 = -e * ps[0] + ps[2], wx1 = e * ps[0] + ps[2], wy0 = -e * ps[1] + ps[3], wy1 = e * ps[1] + ps[3];
    printf("cover %s: posScale %.5f %.5f %.5f %.5f\n", what, q[0], q[1], q[2], q[3]);
    CHECK(std::fabs(gx0 - wx0) < 1e-5f && std::fabs(gx1 - wx1) < 1e-5f, "%s: x %.6f..%.6f, want %.6f..%.6f", what, gx0, gx1, wx0, wx1);
    CHECK(std::fabs(gy0 - wy0) < 1e-5f && std::fabs(gy1 - wy1) < 1e-5f, "%s: y %.6f..%.6f, want %.6f..%.6f", what, gy0, gy1, wy0, wy1);
    CHECK(q[0] * ps[0] > 0.0f && q[1] * ps[1] > 0.0f, "%s: flipped (%.5f vs %.5f, %.5f vs %.5f)", what, q[0], ps[0], q[1], ps[1]);
}

// ---- The intro's black frame (the cut between two shots, ~95 s) ----

// Its box in band units, as logged (NFSMW_LOG_FRONTEND "inset candidate",
// the Deck and 32:9): x +-0.970, y +-0.939, FF000000; and the one at the
// intro's start (~57 s): y +-0.920, FE000000.
constexpr float kFrameX = 0.970f, kFrameY = 0.939f, kFrameY57 = 0.920f;
constexpr uint32_t kFrameColour = 0xFF000000;

static void MatchFrame(const char* what, const Quad& q, float sx, float sy, bool want, const BlackoutRule& rule)
{
    Box box{};
    const bool got = MatchBlackout(q.v, sx, sy, rule, &box);
    CHECK(got == want, "%s at x %.4f y %.4f: %s, want %s", what, sx, sy, got ? "black frame" : "none", want ? "black frame" : "none");
}

// Every black-frame case. Returns the failures it added (for Planted).
static int BlackoutCases(const BlackoutRule& rule)
{
    const int before = g_failures;
    constexpr uint32_t kBar = 0xFD000000;
    for (const Shape& s : kShapes)
    {
        auto at = [&](float x0, float x1, float y0, float y1, uint32_t argb) { return Rectangle(x0, x1, y0, y1, s.sx, s.sy, argb); };
        MatchFrame("black frame", at(-kFrameX, kFrameX, -kFrameY, kFrameY, kFrameColour), s.sx, s.sy, true, rule);
        MatchFrame("black frame FD", at(-kFrameX, kFrameX, -kFrameY, kFrameY, kBar), s.sx, s.sy, true, rule);
        MatchFrame("black frame at ~57 s", at(-kFrameX, kFrameX, -kFrameY57, kFrameY57, 0xFE000000), s.sx, s.sy, true, rule);
        // The overlay drawn with it there (B8 alpha, past the band in x).
        MatchFrame("~57 s overlay", at(-1.104f, 1.104f, -0.978f, 0.985f, 0xB8000000), s.sx, s.sy, false, rule);
        MatchFrame("~57 s overlay shape, opaque", at(-1.104f, 1.104f, -0.978f, 0.985f, 0xFF000000), s.sx, s.sy, false, rule);
        // The title's key art (1024x512, grey vertex colour), short of the
        // band in y only: an inset candidate in the log, never a black frame.
        MatchFrame("title key art", at(-1.104f, 1.104f, -0.981f, 0.981f, 0xFF7F7F7F), s.sx, s.sy, false, rule);
        // Everything else black or nearly: the vid-cam bars, the cover
        // rule's full-band layer, the fades, the key art, the overlays.
        MatchFrame("right bar", at(0.651f, 1.406f, -1.024f, 1.039f, kBar), s.sx, s.sy, false, rule);
        MatchFrame("left bar", at(-1.410f, -0.655f, -1.027f, 1.035f, kBar), s.sx, s.sy, false, rule);
        MatchFrame("full band", at(-1.0f, 1.0f, -1.0f, 1.0f, 0xFF000000), s.sx, s.sy, false, rule);
        MatchFrame("fade", at(-1.29f, 1.29f, -1.30f, 1.30f, 0xFF000000), s.sx, s.sy, false, rule);
        MatchFrame("title key art", at(-1.105f, 1.105f, -1.105f, 1.105f, 0xFF000000), s.sx, s.sy, false, rule);
        MatchFrame("loading band", at(-1.29f, 1.29f, -0.30f, 0.30f, 0xFF000000), s.sx, s.sy, false, rule);
        MatchFrame("noise overlay in black", at(-0.692f, 0.709f, -1.058f, 1.050f, kBar), s.sx, s.sy, false, rule);
        MatchFrame("dark overlay", at(-0.712f, 0.668f, -0.893f, 0.908f, 0x3C000000), s.sx, s.sy, false, rule);
        MatchFrame("cam dialog box 2", at(-0.578f, 0.586f, -0.167f, 0.569f, kBar), s.sx, s.sy, false, rule);
        MatchFrame("pause dim centre", at(-0.53f, 0.57f, -1.91f, 1.91f, 0xC8000000), s.sx, s.sy, false, rule);
        MatchFrame("black frame in grey", at(-kFrameX, kFrameX, -kFrameY, kFrameY, 0xFF7F7F7F), s.sx, s.sy, false, rule);
        MatchFrame("black frame at 3C", at(-kFrameX, kFrameX, -kFrameY, kFrameY, 0x3C000000), s.sx, s.sy, false, rule);
        MatchFrame("frame width, full height", at(-kFrameX, kFrameX, -1.0f, 1.0f, 0xFF000000), s.sx, s.sy, false, rule);
        MatchFrame("full width, frame height", at(-1.0f, 1.0f, -kFrameY, kFrameY, 0xFF000000), s.sx, s.sy, false, rule);
    }
    // Each threshold from both sides, on the Deck and at 32:9.
    const Shape shapes[] = { { "Deck", 1.0f, 0.9f }, { "32:9", 0.5f, 1.0f } };
    for (const Shape& s : shapes)
    {
        auto frame = [&](float dx, float dy, float hx, float hy, uint32_t argb = kFrameColour) {
            return Rectangle(dx - hx, dx + hx, dy - hy, dy + hy, s.sx, s.sy, argb);
        };
        MatchFrame("centre x +0.009", frame(0.009f, 0.0f, kFrameX, kFrameY), s.sx, s.sy, true, rule);
        MatchFrame("centre x +0.011", frame(0.011f, 0.0f, kFrameX, kFrameY), s.sx, s.sy, false, rule);
        MatchFrame("centre x -0.011", frame(-0.011f, 0.0f, kFrameX, kFrameY), s.sx, s.sy, false, rule);
        MatchFrame("centre y -0.009", frame(0.0f, -0.009f, kFrameX, kFrameY), s.sx, s.sy, true, rule);
        MatchFrame("centre y -0.011", frame(0.0f, -0.011f, kFrameX, kFrameY), s.sx, s.sy, false, rule);
        MatchFrame("centre y +0.011", frame(0.0f, 0.011f, kFrameX, kFrameY), s.sx, s.sy, false, rule);
        MatchFrame("half width 0.895", frame(0.0f, 0.0f, 0.895f, kFrameY), s.sx, s.sy, false, rule);
        MatchFrame("half width 0.905", frame(0.0f, 0.0f, 0.905f, kFrameY), s.sx, s.sy, true, rule);
        MatchFrame("half width 0.985", frame(0.0f, 0.0f, 0.985f, kFrameY), s.sx, s.sy, true, rule);
        MatchFrame("half width 0.995", frame(0.0f, 0.0f, 0.995f, kFrameY), s.sx, s.sy, false, rule);
        MatchFrame("half height 0.895", frame(0.0f, 0.0f, kFrameX, 0.895f), s.sx, s.sy, false, rule);
        MatchFrame("half height 0.905", frame(0.0f, 0.0f, kFrameX, 0.905f), s.sx, s.sy, true, rule);
        MatchFrame("half height 0.985", frame(0.0f, 0.0f, kFrameX, 0.985f), s.sx, s.sy, true, rule);
        MatchFrame("half height 0.995", frame(0.0f, 0.0f, kFrameX, 0.995f), s.sx, s.sy, false, rule);
        MatchFrame("alpha EF", frame(0.0f, 0.0f, kFrameX, kFrameY, 0xEF000000), s.sx, s.sy, false, rule);
        MatchFrame("alpha F0", frame(0.0f, 0.0f, kFrameX, kFrameY, 0xF0000000), s.sx, s.sy, true, rule);
        MatchFrame("blue 01", frame(0.0f, 0.0f, kFrameX, kFrameY, 0xFF000001), s.sx, s.sy, false, rule);
        Quad mixed = frame(0.0f, 0.0f, kFrameX, kFrameY);
        mixed.v[3].argb = 0xFE000000;
        MatchFrame("one vertex's colour differs", mixed, s.sx, s.sy, false, rule);
        Quad off = frame(0.0f, 0.0f, kFrameX, kFrameY);
        off.v[2].y += 0.01f;
        MatchFrame("a vertex 0.01 off its corner", off, s.sx, s.sy, false, rule);
        Quad nearly = frame(0.0f, 0.0f, kFrameX, kFrameY);
        nearly.v[2].y += 0.0005f;
        MatchFrame("a vertex 0.0005 off its corner", nearly, s.sx, s.sy, true, rule);
        Quad bowtie = frame(0.0f, 0.0f, kFrameX, kFrameY);
        std::swap(bowtie.v[1], bowtie.v[2]);
        MatchFrame("bow-tie order", bowtie, s.sx, s.sy, false, rule);
    }
    return g_failures - before;
}

static void PlantedBlackout(const char* what, const BlackoutRule& wrong)
{
    const int saved = g_failures;
    g_quiet = true;
    const int failed = BlackoutCases(wrong);
    g_quiet = false;
    g_failures = saved;
    printf("planted %-26s %d case(s) fail\n", what, failed);
    CHECK(failed > 0, "planted %s: every case still passes", what);
}

static void FillFrame(const char* what, float sx, float sy, const float ps[4], float hostW, float hostH, const Rect& scissor,
    uint32_t wantN, const Rect* want)
{
    Rect got[2]{};
    const uint32_t n = BlackoutRects(ps, hostW, hostH, scissor, sx, sy, got);
    printf("%s: %u rect(s)", what, n);
    for (uint32_t i = 0; i < n; i++)
        printf(" [%d,%d)x[%d,%d)", got[i].x0, got[i].x1, got[i].y0, got[i].y1);
    printf("\n");
    CHECK(n == wantN, "%s: %u rects, want %u", what, n, wantN);
    for (uint32_t i = 0; i < n && i < wantN; i++)
        CHECK(Same(got[i], want[i]), "%s: rect %u [%d,%d)x[%d,%d), want [%d,%d)x[%d,%d)", what, i, got[i].x0, got[i].x1, got[i].y0,
            got[i].y1, want[i].x0, want[i].x1, want[i].y0, want[i].y1);
}

// The black frame's fill against pixel centres: a pixel of the scissor is
// filled exactly when its centre is outside the band (|y| > sy under Vert+,
// |x| > sx under Hor+); ties within 1/256 pixel skipped, as above.
static void FrameOracle(const char* what, float sx, float sy, const float ps[4], float hostW, float hostH, const Rect& scissor)
{
    Rect r[2]{};
    const uint32_t n = BlackoutRects(ps, hostW, hostH, scissor, sx, sy, r);
    const float pxX = std::fabs(ps[0]) * hostW * 0.5f, pxY = std::fabs(ps[1]) * hostH * 0.5f;
    auto tie = [](float d, float px) { return std::fabs(d) * px < 1.0f / 256.0f; };
    int wrong = 0, ties = 0;
    for (int32_t y = scissor.y0; y < scissor.y1; y++)
    {
        const float cy = ((float(y) + 0.5f) / hostH * 2.0f - 1.0f - ps[3]) / ps[1];
        for (int32_t x = scissor.x0; x < scissor.x1; x++)
        {
            const float cx = ((float(x) + 0.5f) / hostW * 2.0f - 1.0f - ps[2]) / ps[0];
            const bool want = sy < 1.0f ? std::fabs(cy) > sy : sx < 1.0f ? std::fabs(cx) > sx : false;
            const bool onEdge = sy < 1.0f ? tie(std::fabs(cy) - sy, pxY) : sx < 1.0f && tie(std::fabs(cx) - sx, pxX);
            bool got = false;
            for (uint32_t i = 0; i < n; i++)
                got = got || (x >= r[i].x0 && x < r[i].x1 && y >= r[i].y0 && y < r[i].y1);
            if (onEdge)
                ties++;
            else
                wrong += got != want;
        }
    }
    printf("oracle black frame %s: %d pixel(s) wrong (%d on an edge, skipped)\n", what, wrong, ties);
    CHECK(wrong == 0, "black frame %s: %d pixels differ from the pixel-centre oracle", what, wrong);
}

// ---- PlanFill on the draws as the renderer sees them ----

// The front end's matrix at a shape: c0 0.69 (0.92 safe area x 0.75, a 4:3
// canvas centred in 16:9) x scaleX, c5 0.92 x scaleY (positive, as logged:
// "c5 0.6900 (front end y 0.6900)" at 4:3), the rest 0.
struct Matrix
{
    float c[8];
};
static Matrix FrontEndMatrix(float sx, float sy)
{
    return { { 0.69f * sx, 0.0f, 0.0f, 0.0f, 0.0f, 0.92f * sy, 0.0f, 0.0f } };
}

// A quad's ePolys (words 0..3: x, y, z, colour), the positions taken back
// through the matrix: what the game wrote for that box.
struct EPolys
{
    uint32_t w[4][4];
};
static EPolys Polys(const Quad& q, const Matrix& m)
{
    EPolys e{};
    for (int k = 0; k < 4; k++)
    {
        const float p[3] = { q.v[k].x / m.c[0], q.v[k].y / m.c[5], 0.0f };
        std::memcpy(e.w[k], p, sizeof(p));
        e.w[k][3] = q.v[k].argb;
    }
    return e;
}

// The draw the S2 dump shows for a vid-cam bar (and the cut's black frame):
// 1280x2048 vp s(640,-360) o(640,360) sc(0,0)-(1280,720) vte43F, blend
// 07060706, t0 1AD6E000 32x32, prim 13 n4, untouched by the other rules.
static Draw S2Draw(const Matrix& m, float sx, float sy, const float ps[4], float hostW = 1280.0f, float hostH = 2048.0f,
    Rect scissor = Rect{ 0, 0, 1280, 720 })
{
    return Draw{ .prim = 13,
        .count = 4,
        .c = m.c,
        .frontEndY = std::fabs(m.c[5]),
        .scaleX = sx,
        .scaleY = sy,
        .frontEndShader = true,
        .leftAlone = true,
        .occlusion = false,
        .vte = 0x43F,
        .blend0 = 0x07060706,
        .t0Sampled = true,
        .t0Width = 32,
        .t0Height = 32,
        .posScale = { ps[0], ps[1], ps[2], ps[3] },
        .hostW = hostW,
        .hostH = hostH,
        .scissor = scissor };
}

static const char* Name(Fill f)
{
    return f == Fill::RightBar ? "right bar" : f == Fill::LeftBar ? "left bar" : f == Fill::Blackout ? "black frame" : "none";
}

// Runs PlanFill over the ePolys; the reader fails for vertices from
// `limit` on (past the vertex buffer) but still hands the word over, so a
// plan that ignored the failure would act on it. `reads` counts calls.
static Plan RunPlan(const Draw& d, const EPolys& e, uint32_t limit = 4, int* reads = nullptr)
{
    int n = 0;
    const Plan p = PlanFill(d, [&](uint32_t k, uint32_t j, uint32_t& word) {
        n++;
        word = e.w[k & 3][j & 3];
        return k < limit;
    });
    if (reads)
        *reads = n;
    return p;
}

static void PlanCase(const char* what, const Draw& d, const EPolys& e, Fill want, uint32_t wantN = 0, const Rect* wantRects = nullptr,
    uint32_t limit = 4)
{
    const Plan p = RunPlan(d, e, limit);
    printf("plan %s: %s, %u rect(s)", what, Name(p.fill), p.rectCount);
    for (uint32_t i = 0; i < p.rectCount; i++)
        printf(" [%d,%d)x[%d,%d)", p.rects[i].x0, p.rects[i].x1, p.rects[i].y0, p.rects[i].y1);
    printf("\n");
    CHECK(p.fill == want, "%s: %s, want %s", what, Name(p.fill), Name(want));
    if (p.fill != want || want == Fill::None)
        return;
    CHECK(p.rectCount == wantN, "%s: %u rects, want %u", what, p.rectCount, wantN);
    for (uint32_t i = 0; i < p.rectCount && i < wantN; i++)
        CHECK(Same(p.rects[i], wantRects[i]), "%s: rect %u [%d,%d)x[%d,%d), want [%d,%d)x[%d,%d)", what, i, p.rects[i].x0, p.rects[i].x1,
            p.rects[i].y0, p.rects[i].y1, wantRects[i].x0, wantRects[i].x1, wantRects[i].y0, wantRects[i].y1);
    // The copy's posScale stretches the quad's own box (as the renderer
    // reads it) over the frame through the draw's posScale.
    float q[4];
    CoverFrame(p.box, d.posScale, q);
    CHECK(std::memcmp(q, p.posScale, sizeof(q)) == 0, "%s: posScale %.5f %.5f %.5f %.5f, want %.5f %.5f %.5f %.5f", what, p.posScale[0],
        p.posScale[1], p.posScale[2], p.posScale[3], q[0], q[1], q[2], q[3]);
    CHECK(p.argb == e.w[0][3], "%s: colour %08X, want %08X", what, p.argb, e.w[0][3]);
}

int main()
{
    printf("match:\n");
    MatchCases(Rule{});

    printf("\nguards:\n");
    {
        // The colour test the renderer asks of vertex 0 before it reads the
        // other three (BarColour): black, alpha at least minAlpha.
        CHECK(BarColour(0xFD000000), "FD000000 is a bar's colour");
        CHECK(BarColour(0xF0000000), "F0000000 is a bar's colour (minAlpha)");
        CHECK(!BarColour(0xEF000000), "EF000000 is below minAlpha");
        CHECK(!BarColour(0xFD000001) && !BarColour(0xFD000100) && !BarColour(0xFD010000), "a colour other than black");
        CHECK(!BarColour(0x00000000), "transparent black");
        // No view scale (or a negative one): nothing matches, and the box
        // is left as it was, as on any miss.
        const Quad right = Rectangle(0.651f, 1.406f, -1.024f, 1.039f, 1.0f, 0.9f, 0xFD000000);
        const Box sentinel{ 9.0f, 9.0f, 9.0f, 9.0f };
        const float scales[][2] = { { 0.0f, 0.9f }, { 1.0f, 0.0f }, { -1.0f, 0.9f }, { 1.0f, -0.9f }, { -1.0f, -0.9f } };
        for (const auto& sc : scales)
        {
            Box b = sentinel;
            const Side got = MatchSideBar(right.v, sc[0], sc[1], Rule{}, &b);
            CHECK(got == Side::None && b.x0 == 9.0f && b.y1 == 9.0f, "view scale %.1f %.1f: %s, box %s", sc[0], sc[1], Name(got),
                b.x0 == 9.0f ? "kept" : "written");
        }
        Box b = sentinel;
        const Quad bezel = Rectangle(-0.001f, 0.654f, -1.027f, 0.003f, 1.0f, 0.9f, 0xFB7D7D7D);
        CHECK(MatchSideBar(bezel.v, 1.0f, 0.9f, Rule{}, &b) == Side::None && b.x0 == 9.0f, "a miss leaves the box as it was");
        // ... also one that fails only the last test, the sides (a black
        // column past both band edges, but neither inner nor outer edge fits).
        const Quad column = Rectangle(0.20f, 0.60f, -1.024f, 1.039f, 1.0f, 0.9f, 0xFD000000);
        CHECK(MatchSideBar(column.v, 1.0f, 0.9f, Rule{}, &b) == Side::None && b.x0 == 9.0f, "a miss on the sides leaves the box as it was");
        CHECK(MatchSideBar(right.v, 1.0f, 0.9f, Rule{}, &b) == Side::Right && b.x0 == right.v[0].x && b.x1 == right.v[1].x &&
                b.y0 == right.v[0].y && b.y1 == right.v[2].y,
            "a match writes the bar's box");
        CHECK(MatchSideBar(right.v, 1.0f, 0.9f) == Side::Right, "the box is optional");
        printf("colour, view scale and box guards checked\n");
    }

    printf("\nplanted (each must fail a case):\n");
    auto plant = [](const char* what, auto set) { Rule r; set(r); Planted(what, r); };
    plant("pastMin 1.00", [](Rule& r) { r.pastMin = 1.00f; });
    plant("pastMin 1.02", [](Rule& r) { r.pastMin = 1.02f; });
    plant("pastMax 1.09", [](Rule& r) { r.pastMax = 1.09f; });
    plant("pastMax 1.11", [](Rule& r) { r.pastMax = 1.11f; });
    plant("innerMin 0.49", [](Rule& r) { r.innerMin = 0.49f; });
    plant("innerMin 0.51", [](Rule& r) { r.innerMin = 0.51f; });
    plant("innerMax 0.79", [](Rule& r) { r.innerMax = 0.79f; });
    plant("innerMax 0.81", [](Rule& r) { r.innerMax = 0.81f; });
    plant("outerMin 1.19", [](Rule& r) { r.outerMin = 1.19f; });
    plant("outerMin 1.21", [](Rule& r) { r.outerMin = 1.21f; });
    plant("outerMax 1.99", [](Rule& r) { r.outerMax = 1.99f; });
    plant("outerMax 2.01", [](Rule& r) { r.outerMax = 2.01f; });
    plant("minAlpha EF", [](Rule& r) { r.minAlpha = 0xEF; });
    plant("minAlpha F1", [](Rule& r) { r.minAlpha = 0xF1; });
    plant("cornerTolerance 0.02", [](Rule& r) { r.cornerTolerance = 0.02f; });
    plant("cornerTolerance 0.0001", [](Rule& r) { r.cornerTolerance = 0.0001f; });

    printf("\nfill:\n");
    float ps[4], half[4], tiled[4], flipped[4], flippedHalf[4];
    FrontEndPs(ps);
    FrontEndPs(half, 640.5f, 360.5f);  // D3D's half-pixel centre
    FrontEndPs(tiled, 640.0f, 360.0f - 256.0f);  // NFSMW_TILING=1, window y offset -256
    FrontEndPs(flipped, 640.0f, 360.0f, 360.0f);  // the viewport's y scale +360 (ps1 > 0)
    FrontEndPs(flippedHalf, 640.5f, 360.5f, 360.0f);
    const Rect frame{ 0, 0, 1280, 720 }, frame2{ 0, 0, 2560, 1440 }, frame3{ 0, 0, 3840, 2160 };
    {
        const Rect want[2] = { { 640, 0, 1280, 36 }, { 640, 684, 1280, 720 } };
        FillBar("Deck right, 1x", Side::Right, 0.651f, 1.406f, 1.0f, 0.9f, ps, 1280, 2048, frame, 2, want);
        FillBar("Deck right, 1x, half-pixel", Side::Right, 0.651f, 1.406f, 1.0f, 0.9f, half, 1280, 2048, frame, 2, want);
        const Rect left[2] = { { 0, 0, 640, 36 }, { 0, 684, 640, 720 } };
        FillBar("Deck left, 1x", Side::Left, -1.410f, -0.655f, 1.0f, 0.9f, ps, 1280, 2048, frame, 2, left);
        const Rect want2[2] = { { 1280, 0, 2560, 72 }, { 1280, 1368, 2560, 1440 } };
        FillBar("Deck right, 2x", Side::Right, 0.651f, 1.406f, 1.0f, 0.9f, ps, 2560, 4096, frame2, 2, want2);
        const Rect r43[2] = { { 640, 0, 1280, 90 }, { 640, 630, 1280, 720 } };
        FillBar("4:3 right", Side::Right, 0.651f, 1.406f, 1.0f, 0.75f, ps, 1280, 2048, frame, 2, r43);
        FillBar("iPad Pro right", Side::Right, 0.651f, 1.406f, 1.0f, 0.750366f, ps, 1280, 2048, frame, 2, r43);
        const Rect r32[2] = { { 640, 0, 1280, 56 }, { 640, 664, 1280, 720 } };
        FillBar("3:2 right", Side::Right, 0.651f, 1.406f, 1.0f, 0.84375f, ps, 1280, 2048, frame, 2, r32);
        // A tile (rows 256.. of the frame): only the bottom strip is in it.
        const Rect tile{ 0, 0, 1280, 464 }, t[1] = { { 640, 428, 1280, 464 } };
        FillBar("Deck right, tile at y -256", Side::Right, 0.651f, 1.406f, 1.0f, 0.9f, tiled, 1280, 2048, tile, 1, t);
        // The game's own scissor narrower than the frame: kept.
        const Rect game{ 100, 10, 1180, 700 }, g[2] = { { 640, 10, 1180, 36 }, { 640, 684, 1180, 700 } };
        FillBar("Deck right, game scissor", Side::Right, 0.651f, 1.406f, 1.0f, 0.9f, ps, 1280, 2048, game, 2, g);
        // Hor+: only past a 2.50:1 screen, the columns beyond the band.
        const Rect w329[1] = { { 1920, 0, 2560, 1440 } }, w329l[1] = { { 0, 0, 640, 1440 } };
        FillBar("32:9 right, 2x", Side::Right, 0.651f, 1.406f, 0.5f, 1.0f, ps, 2560, 4096, frame2, 1, w329);
        FillBar("32:9 left, 2x", Side::Left, -1.410f, -0.655f, 0.5f, 1.0f, ps, 2560, 4096, frame2, 1, w329l);
        FillBar("21:9 right", Side::Right, 0.651f, 1.406f, 0.75f, 1.0f, ps, 2560, 4096, frame2, 0, nullptr);
        FillBar("21:9 left", Side::Left, -1.410f, -0.655f, 0.75f, 1.0f, ps, 2560, 4096, frame2, 0, nullptr);
        FillBar("3440x1440 right", Side::Right, 0.651f, 1.406f, 0.744186f, 1.0f, ps, 2560, 4096, frame2, 0, nullptr);
        const Rect w250[1] = { { 1095, 0, 1280, 720 } };
        FillBar("2.51:1 right (bar ends at 0.9997)", Side::Right, 0.651f, 1.406f, 0.711f, 1.0f, ps, 1280, 2048, frame, 1, w250);
        FillBar("16:9", Side::Right, 0.651f, 1.406f, 1.0f, 1.0f, ps, 1280, 2048, frame, 0, nullptr);
        // Just short of 2.50:1 the bar ends past clip 1 (1.0100): the screen
        // edge test is clip 1, not the fill's reach (kFillEdge).
        FillBar("2.49:1 right (bar ends at 1.0100)", Side::Right, 0.651f, 1.406f, 0.71835f, 1.0f, ps, 1280, 2048, frame, 0, nullptr);
        FillBar("2.49:1 left (bar ends at -1.0130)", Side::Left, -1.410f, -0.655f, 0.71845f, 1.0f, ps, 1280, 2048, frame, 0, nullptr);
        // Half-pixel centres at 2x and 3x (viewport o(640.5,360.5)): the
        // first row is filled (with fills ending at clip +-1 it was not).
        const Rect h2[2] = { { 1281, 0, 2560, 73 }, { 1281, 1369, 2560, 1440 } };
        FillBar("Deck right, 2x, half-pixel", Side::Right, 0.651f, 1.406f, 1.0f, 0.9f, half, 2560, 4096, frame2, 2, h2);
        const Rect h2l[2] = { { 0, 0, 1281, 73 }, { 0, 1369, 1281, 1440 } };
        FillBar("Deck left, 2x, half-pixel", Side::Left, -1.410f, -0.655f, 1.0f, 0.9f, half, 2560, 4096, frame2, 2, h2l);
        const Rect h3[2] = { { 1921, 0, 3840, 109 }, { 1921, 2053, 3840, 2160 } };
        FillBar("Deck right, 3x, half-pixel", Side::Right, 0.651f, 1.406f, 1.0f, 0.9f, half, 3840, 6144, frame3, 2, h3);
        // 32:9 at 2x, half-pixel: the last column and the first row.
        const Rect w329h[1] = { { 1921, 0, 2560, 1440 } }, w329hl[1] = { { 0, 0, 641, 1440 } };
        FillBar("32:9 right, 2x, half-pixel", Side::Right, 0.651f, 1.406f, 0.5f, 1.0f, half, 2560, 4096, frame2, 1, w329h);
        FillBar("32:9 left, 2x, half-pixel", Side::Left, -1.410f, -0.655f, 0.5f, 1.0f, half, 2560, 4096, frame2, 1, w329hl);
        // The viewport's y scale flipped: the clip +y strip is the bottom rows.
        const Rect fl[2] = { { 640, 684, 1280, 720 }, { 640, 0, 1280, 36 } };
        FillBar("Deck right, 1x, y flipped", Side::Right, 0.651f, 1.406f, 1.0f, 0.9f, flipped, 1280, 2048, frame, 2, fl);
        FillBar("no side", Side::None, 0.651f, 1.406f, 1.0f, 0.9f, ps, 1280, 2048, frame, 0, nullptr);
    }
    Oracle("Deck right 1x", Side::Right, 1.0f, 0.9f, ps, 1280, 2048, frame);
    Oracle("Deck left 1x", Side::Left, 1.0f, 0.9f, ps, 1280, 2048, frame);
    Oracle("4:3 right 2x", Side::Right, 1.0f, 0.75f, ps, 2560, 4096, frame2);
    Oracle("3:2 left 1x", Side::Left, 1.0f, 0.84375f, ps, 1280, 2048, frame);
    Oracle("Deck right, tile", Side::Right, 1.0f, 0.9f, tiled, 1280, 2048, Rect{ 0, 0, 1280, 464 });
    // D3D's half-pixel centres at 2x and 3x: clip +-1 lands a host pixel in,
    // so the first row and column are filled only because the fill reaches
    // past the frame (kFillEdge) and the scissor trims it.
    Oracle("Deck right 2x, half-pixel", Side::Right, 1.0f, 0.9f, half, 2560, 4096, frame2);
    Oracle("Deck left 2x, half-pixel", Side::Left, 1.0f, 0.9f, half, 2560, 4096, frame2);
    Oracle("Deck right 3x, half-pixel", Side::Right, 1.0f, 0.9f, half, 3840, 6144, frame3);
    Oracle("Deck left 3x, half-pixel", Side::Left, 1.0f, 0.9f, half, 3840, 6144, frame3);
    Oracle("32:9 right 2x, half-pixel", Side::Right, 0.5f, 1.0f, half, 2560, 4096, frame2);
    Oracle("32:9 left 2x, half-pixel", Side::Left, 0.5f, 1.0f, half, 2560, 4096, frame2);
    Oracle("32:9 right 1x", Side::Right, 0.5f, 1.0f, ps, 1280, 2048, frame);
    // The viewport's y scale flipped (vp s y +360: ps1 > 0): the strips swap
    // ends, the rows stay the same.
    Oracle("Deck right 1x, y flipped", Side::Right, 1.0f, 0.9f, flipped, 1280, 2048, frame);
    Oracle("Deck left 2x, y flipped, half-pixel", Side::Left, 1.0f, 0.9f, flippedHalf, 2560, 4096, frame2);
    Complement("Deck 1x", 0.9f, ps, 1280, 2048, frame);
    Complement("Deck 1x, half-pixel", 0.9f, half, 1280, 2048, frame);
    Complement("3:2 1x, half-pixel", 0.84375f, half, 1280, 2048, frame);
    Complement("iPad Pro 1x", 0.750366f, ps, 1280, 2048, frame);
    Complement("4:3 3x", 0.75f, ps, 3840, 6144, Rect{ 0, 0, 3840, 2160 });
    Complement("Deck, tile", 0.9f, tiled, 1280, 2048, Rect{ 0, 0, 1280, 464 });
    Complement("Deck 2x, half-pixel", 0.9f, half, 2560, 4096, frame2);
    Complement("Deck 3x, half-pixel", 0.9f, half, 3840, 6144, frame3);
    Complement("Deck 1x, y flipped", 0.9f, flipped, 1280, 2048, frame);
    Complement("4:3 2x, y flipped, half-pixel", 0.75f, flippedHalf, 2560, 4096, frame2);

    printf("\ncover:\n");
    Cover("Deck right", 0.651f, 1.406f, 1.0f, 0.9f, ps);
    Cover("Deck left", -1.410f, -0.655f, 1.0f, 0.9f, ps);
    Cover("Deck right, tile", 0.651f, 1.406f, 1.0f, 0.9f, tiled);
    Cover("4:3 left, half-pixel", -1.410f, -0.655f, 1.0f, 0.75f, half);
    Cover("32:9 right", 0.651f, 1.406f, 0.5f, 1.0f, ps);
    Cover("32:9 left", -1.410f, -0.655f, 0.5f, 1.0f, ps);
    Cover("Deck right, y flipped", 0.651f, 1.406f, 1.0f, 0.9f, flipped);
    Cover("Deck left, 2x, half-pixel", -1.410f, -0.655f, 1.0f, 0.9f, half);
    {
        // The copy covers the whole frame (its host rectangle contains it).
        const Box b{ 0.651f, 1.406f, -1.024f * 0.9f, 1.039f * 0.9f };
        float q[4];
        CoverFrame(b, ps, q);
        const Rect r = ClipToHost(b.x0, b.x1, b.y0, b.y1, q, 1280, 2048);
        CHECK(r.x0 <= 0 && r.y0 <= 0 && r.x1 >= 1280 && r.y1 >= 720, "Deck cover: [%d,%d)x[%d,%d) misses the frame", r.x0, r.x1, r.y0, r.y1);
    }

    printf("\nband rows:\n");
    BandOracle("Deck 1x", 0.9f, ps, 2048, 720);
    BandOracle("Deck 1x, half-pixel", 0.9f, half, 2048, 720);
    BandOracle("3:2 1x", 0.84375f, ps, 2048, 720);
    BandOracle("4:3 2x", 0.75f, ps, 4096, 1440);
    BandOracle("iPad Pro 3x, half-pixel", 0.750366f, half, 6144, 2160);
    BandOracle("Deck 1x, y flipped", 0.9f, flipped, 2048, 720);
    BandOracle("Deck, tile", 0.9f, tiled, 2048, 464);
    {
        const Rows deck = BandRows(-0.9f, 0.9f, ps[1], ps[3], 2048), fl = BandRows(-0.9f, 0.9f, flipped[1], flipped[3], 2048);
        CHECK(deck.top == 36 && deck.bottom == 684, "Deck band rows [%d,%d), want [36,684)", deck.top, deck.bottom);
        CHECK(fl.top == 36 && fl.bottom == 684, "Deck band rows, y flipped [%d,%d), want [36,684)", fl.top, fl.bottom);
        const Rows r43 = BandRows(-0.75f, 0.75f, ps[1], ps[3], 2048);
        CHECK(r43.top == 90 && r43.bottom == 630, "4:3 band rows [%d,%d), want [90,630)", r43.top, r43.bottom);
    }

    printf("\nblack frame:\n");
    BlackoutCases(BlackoutRule{});
    {
        // Guards: no view scale, and the box written on a match only.
        const Quad f = Rectangle(-kFrameX, kFrameX, -kFrameY, kFrameY, 1.0f, 0.9f, kFrameColour);
        Box b{ 9.0f, 9.0f, 9.0f, 9.0f };
        CHECK(!MatchBlackout(f.v, 0.0f, 0.9f, BlackoutRule{}, &b) && !MatchBlackout(f.v, 1.0f, -0.9f, BlackoutRule{}, &b) && b.x0 == 9.0f,
            "a black frame with no view scale");
        const Quad bar = Rectangle(0.651f, 1.406f, -1.024f, 1.039f, 1.0f, 0.9f, 0xFD000000);
        CHECK(!MatchBlackout(bar.v, 1.0f, 0.9f, BlackoutRule{}, &b) && b.x0 == 9.0f, "a miss leaves the box as it was");
        CHECK(MatchBlackout(f.v, 1.0f, 0.9f, BlackoutRule{}, &b) && b.x0 == f.v[0].x && b.x1 == f.v[1].x && b.y0 == f.v[0].y && b.y1 == f.v[2].y,
            "a match writes the frame's box");
        // A bar is never a black frame, nor a black frame a bar.
        CHECK(MatchSideBar(f.v, 1.0f, 0.9f) == Side::None, "a black frame is not a side bar");
    }
    printf("\nplanted, black frame (each must fail a case):\n");
    auto plantFrame = [](const char* what, auto set) { BlackoutRule r; set(r); PlantedBlackout(what, r); };
    plantFrame("centreMax 0", [](BlackoutRule& r) { r.centreMax = 0.0f; });
    plantFrame("centreMax 0.02", [](BlackoutRule& r) { r.centreMax = 0.02f; });
    plantFrame("extentMin 0.89", [](BlackoutRule& r) { r.extentMin = 0.89f; });
    plantFrame("extentMin 0.91", [](BlackoutRule& r) { r.extentMin = 0.91f; });
    plantFrame("extentMax 0.98", [](BlackoutRule& r) { r.extentMax = 0.98f; });
    plantFrame("extentMax 1.00", [](BlackoutRule& r) { r.extentMax = 1.00f; });
    plantFrame("minAlpha EF", [](BlackoutRule& r) { r.minAlpha = 0xEF; });
    plantFrame("minAlpha F1", [](BlackoutRule& r) { r.minAlpha = 0xF1; });
    plantFrame("cornerTolerance 0.02", [](BlackoutRule& r) { r.cornerTolerance = 0.02f; });
    plantFrame("cornerTolerance 0.0001", [](BlackoutRule& r) { r.cornerTolerance = 0.0001f; });

    printf("\nblack frame fill:\n");
    {
        const Rect deck[2] = { { 0, 0, 1280, 36 }, { 0, 684, 1280, 720 } };
        FillFrame("Deck, 1x", 1.0f, 0.9f, ps, 1280, 2048, frame, 2, deck);
        FillFrame("Deck, 1x, half-pixel", 1.0f, 0.9f, half, 1280, 2048, frame, 2, deck);
        const Rect deck2[2] = { { 0, 0, 2560, 72 }, { 0, 1368, 2560, 1440 } };
        FillFrame("Deck, 2x", 1.0f, 0.9f, ps, 2560, 4096, frame2, 2, deck2);
        const Rect deck2h[2] = { { 0, 0, 2560, 73 }, { 0, 1369, 2560, 1440 } };
        FillFrame("Deck, 2x, half-pixel", 1.0f, 0.9f, half, 2560, 4096, frame2, 2, deck2h);
        const Rect r43[2] = { { 0, 0, 1280, 90 }, { 0, 630, 1280, 720 } };
        FillFrame("4:3", 1.0f, 0.75f, ps, 1280, 2048, frame, 2, r43);
        FillFrame("iPad Pro", 1.0f, 0.750366f, ps, 1280, 2048, frame, 2, r43);
        const Rect r32[2] = { { 0, 0, 1280, 56 }, { 0, 664, 1280, 720 } };
        FillFrame("3:2", 1.0f, 0.84375f, ps, 1280, 2048, frame, 2, r32);
        const Rect t[1] = { { 0, 428, 1280, 464 } };
        FillFrame("Deck, tile at y -256", 1.0f, 0.9f, tiled, 1280, 2048, Rect{ 0, 0, 1280, 464 }, 1, t);
        const Rect g[2] = { { 100, 10, 1180, 36 }, { 100, 684, 1180, 700 } };
        FillFrame("Deck, game scissor", 1.0f, 0.9f, ps, 1280, 2048, Rect{ 100, 10, 1180, 700 }, 2, g);
        const Rect fl[2] = { { 0, 684, 1280, 720 }, { 0, 0, 1280, 36 } };
        FillFrame("Deck, y flipped", 1.0f, 0.9f, flipped, 1280, 2048, frame, 2, fl);
        const Rect w329[2] = { { 1920, 0, 2560, 1440 }, { 0, 0, 640, 1440 } };
        FillFrame("32:9, 2x", 0.5f, 1.0f, ps, 2560, 4096, frame2, 2, w329);
        const Rect w329h[2] = { { 1921, 0, 2560, 1440 }, { 0, 0, 641, 1440 } };
        FillFrame("32:9, 2x, half-pixel", 0.5f, 1.0f, half, 2560, 4096, frame2, 2, w329h);
        const Rect uw[2] = { { 1120, 0, 1280, 720 }, { 0, 0, 160, 720 } };
        FillFrame("21:9", 0.75f, 1.0f, ps, 1280, 2048, frame, 2, uw);
        const Rect u3440[2] = { { 1116, 0, 1280, 720 }, { 0, 0, 164, 720 } };
        FillFrame("3440x1440", 0.744186f, 1.0f, ps, 1280, 2048, frame, 2, u3440);
        FillFrame("16:9", 1.0f, 1.0f, ps, 1280, 2048, frame, 0, nullptr);
    }
    FrameOracle("Deck 1x", 1.0f, 0.9f, ps, 1280, 2048, frame);
    FrameOracle("Deck 3x, half-pixel", 1.0f, 0.9f, half, 3840, 6144, frame3);
    FrameOracle("4:3 2x, y flipped, half-pixel", 1.0f, 0.75f, flippedHalf, 2560, 4096, frame2);
    FrameOracle("Deck, tile", 1.0f, 0.9f, tiled, 1280, 2048, Rect{ 0, 0, 1280, 464 });
    FrameOracle("32:9 2x, half-pixel", 0.5f, 1.0f, half, 2560, 4096, frame2);
    FrameOracle("21:9 1x", 0.75f, 1.0f, ps, 1280, 2048, frame);
    FrameOracle("3440x1440 2x", 0.744186f, 1.0f, ps, 2560, 4096, frame2);
    FrameOracle("16:9", 1.0f, 1.0f, ps, 1280, 2048, frame);
    Cover("black frame, Deck", -kFrameX, kFrameX, 1.0f, 0.9f, ps);
    Cover("black frame, 32:9, half-pixel", -kFrameX, kFrameX, 0.5f, 1.0f, half);

    printf("\nplan (the draws as dumped):\n");
    {
        const Matrix deck = FrontEndMatrix(1.0f, 0.9f), w329 = FrontEndMatrix(0.5f, 1.0f);
        const EPolys rightDeck = Polys(Rectangle(0.651f, 1.406f, -1.024f, 1.039f, 1.0f, 0.9f, 0xFD000000), deck);
        const EPolys leftDeck = Polys(Rectangle(-1.410f, -0.655f, -1.027f, 1.035f, 1.0f, 0.9f, 0xFD000000), deck);
        const EPolys frameDeck = Polys(Rectangle(-kFrameX, kFrameX, -kFrameY, kFrameY, 1.0f, 0.9f, kFrameColour), deck);
        const Draw d = S2Draw(deck, 1.0f, 0.9f, ps);
        // The S2 run's "edge bars" line: right bar, first [640,1280)x[0,36).
        const Rect right[2] = { { 640, 0, 1280, 36 }, { 640, 684, 1280, 720 } }, left[2] = { { 0, 0, 640, 36 }, { 0, 684, 640, 720 } };
        PlanCase("Deck right bar", d, rightDeck, Fill::RightBar, 2, right);
        PlanCase("Deck left bar", d, leftDeck, Fill::LeftBar, 2, left);
        const Rect black[2] = { { 0, 0, 1280, 36 }, { 0, 684, 1280, 720 } };
        PlanCase("Deck black frame", d, frameDeck, Fill::Blackout, 2, black);
        {
            const Plan p = RunPlan(d, rightDeck);
            CHECK(std::fabs(p.box.x0 - 0.651f) < 1e-5f && std::fabs(p.box.x1 - 1.406f) < 1e-5f && std::fabs(p.box.y0 + 1.024f * 0.9f) < 1e-5f &&
                    std::fabs(p.box.y1 - 1.039f * 0.9f) < 1e-5f,
                "Deck right bar: box x %.5f..%.5f y %.5f..%.5f", p.box.x0, p.box.x1, p.box.y0, p.box.y1);
        }
        // At 2x (the viewport and scissor in host pixels at twice the size).
        const Rect right2[2] = { { 1280, 0, 2560, 72 }, { 1280, 1368, 2560, 1440 } };
        PlanCase("Deck right bar, 2x", S2Draw(deck, 1.0f, 0.9f, ps, 2560, 4096, frame2), rightDeck, Fill::RightBar, 2, right2);
        // 32:9 (2880x810, 2x): the bars past 2.50:1 and the black frame.
        const EPolys right329 = Polys(Rectangle(0.651f, 1.406f, -1.024f, 1.039f, 0.5f, 1.0f, 0xFD000000), w329);
        const EPolys frame329 = Polys(Rectangle(-kFrameX, kFrameX, -kFrameY, kFrameY, 0.5f, 1.0f, kFrameColour), w329);
        const Draw d329 = S2Draw(w329, 0.5f, 1.0f, ps, 2560, 4096, frame2);
        const Rect r329[1] = { { 1920, 0, 2560, 1440 } }, f329[2] = { { 1920, 0, 2560, 1440 }, { 0, 0, 640, 1440 } };
        PlanCase("32:9 right bar", d329, right329, Fill::RightBar, 1, r329);
        PlanCase("32:9 black frame", d329, frame329, Fill::Blackout, 2, f329);
        // 21:9: the bars reach the screen edge (nothing to fill), the black
        // frame does not.
        const Matrix uw = FrontEndMatrix(0.75f, 1.0f);
        const Draw duw = S2Draw(uw, 0.75f, 1.0f, ps);
        PlanCase("21:9 right bar", duw, Polys(Rectangle(0.651f, 1.406f, -1.024f, 1.039f, 0.75f, 1.0f, 0xFD000000), uw), Fill::RightBar, 0, nullptr);
        const Rect fuw[2] = { { 1120, 0, 1280, 720 }, { 0, 0, 160, 720 } };
        PlanCase("21:9 black frame", duw, Polys(Rectangle(-kFrameX, kFrameX, -kFrameY, kFrameY, 0.75f, 1.0f, kFrameColour), uw), Fill::Blackout, 2,
            fuw);
        // 16:9 never gets here (the renderer's block needs a view scale
        // below 1), and a plan there fills nothing.
        const Matrix m169 = FrontEndMatrix(1.0f, 1.0f);
        PlanCase("16:9 black frame", S2Draw(m169, 1.0f, 1.0f, ps),
            Polys(Rectangle(-kFrameX, kFrameX, -kFrameY, kFrameY, 1.0f, 1.0f, kFrameColour), m169), Fill::Blackout, 0, nullptr);
        // A ppc/ from before the Vert+ hooks reports no front-end y scale
        // (0): Hor+ still fills (c5 the unnarrowed 0.92), a narrowed view
        // can't happen there and is refused.
        Draw old = d329;
        old.frontEndY = 0.0f;
        PlanCase("32:9 right bar, old ppc/", old, right329, Fill::RightBar, 1, r329);
        Draw oldDeck = d;
        oldDeck.frontEndY = 0.0f;
        PlanCase("Deck right bar, old ppc/ (no y scale)", oldDeck, rightDeck, Fill::None);
        // Accepted variants: blending off; c5 negative (its sign was never
        // checked); a 1x1 t0.
        Draw v = d;
        v.blend0 = 0x00010001;
        PlanCase("blend off", v, rightDeck, Fill::RightBar, 2, right);
        Matrix neg = deck;
        neg.c[5] = -neg.c[5];
        PlanCase("c5 negative", S2Draw(neg, 1.0f, 0.9f, ps), Polys(Rectangle(0.651f, 1.406f, -1.039f, 1.024f, 1.0f, 0.9f, 0xFD000000), neg),
            Fill::RightBar, 2, right);
        v = d;
        v.t0Width = v.t0Height = 1;
        PlanCase("t0 1x1", v, rightDeck, Fill::RightBar, 2, right);
        // Each register check refuses on its own (the right bar otherwise).
        auto refuse = [&](const char* what, auto change) {
            Draw x = d;
            Matrix m = deck;
            change(x, m);
            x.c = m.c;
            PlanCase(what, x, rightDeck, Fill::None);
        };
        refuse("a triangle list", [](Draw& x, Matrix&) { x.prim = 4; });
        refuse("two quads", [](Draw& x, Matrix&) { x.count = 8; });
        refuse("not the front-end shader", [](Draw& x, Matrix&) { x.frontEndShader = false; });
        refuse("taken by another rule", [](Draw& x, Matrix&) { x.leftAlone = false; });
        refuse("inside an occlusion query", [](Draw& x, Matrix&) { x.occlusion = true; });
        refuse("x translation c3", [](Draw&, Matrix& m) { m.c[3] = 0.01f; });
        refuse("c4", [](Draw&, Matrix& m) { m.c[4] = 0.01f; });
        refuse("c6", [](Draw&, Matrix& m) { m.c[6] = 0.01f; });
        refuse("y translation c7", [](Draw&, Matrix& m) { m.c[7] = 0.01f; });
        refuse("c5 not the front end's y scale", [](Draw& x, Matrix&) { x.frontEndY = 0.83f; });
        refuse("VTE x scale off", [](Draw& x, Matrix&) { x.vte = 0x43E; });
        refuse("VTE x offset off", [](Draw& x, Matrix&) { x.vte = 0x43D; });
        refuse("VTE y scale off", [](Draw& x, Matrix&) { x.vte = 0x43B; });
        refuse("VTE y offset off", [](Draw& x, Matrix&) { x.vte = 0x437; });
        refuse("additive blend", [](Draw& x, Matrix&) { x.blend0 = 0x01060106; });
        refuse("another blend", [](Draw& x, Matrix&) { x.blend0 = 0x07060705; });
        refuse("t0 not sampled", [](Draw& x, Matrix&) { x.t0Sampled = false; });
        refuse("t0 64 wide", [](Draw& x, Matrix&) { x.t0Width = 64; });
        refuse("t0 64 high", [](Draw& x, Matrix&) { x.t0Height = 64; });
        // A vertex past the vertex buffer: nothing (the reader's words are
        // still the bar's, so only the failure refuses).
        PlanCase("vertex 0 past the buffer", d, rightDeck, Fill::None, 0, nullptr, 0);
        PlanCase("vertex 3 past the buffer", d, rightDeck, Fill::None, 0, nullptr, 3);
        // Reads: one for a quad that isn't black (vertex 0's colour), none
        // when a register check refuses, 17 for a black one.
        int reads = -1;
        EPolys grey = rightDeck;
        for (auto& w : grey.w)
            w[3] = 0xFD7E7E7E;
        RunPlan(d, grey, 4, &reads);
        CHECK(reads == 1, "a grey quad: %d reads, want 1", reads);
        Draw tri = d;
        tri.prim = 4;
        RunPlan(tri, rightDeck, 4, &reads);
        CHECK(reads == 0, "a refused draw: %d reads, want 0", reads);
        RunPlan(d, rightDeck, 4, &reads);
        CHECK(reads == 17, "a bar: %d reads, want 17", reads);
    }

    printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "all passed", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
