// Tests for GitHub #19, "rectangular borders in specific places in menus"
// (gpu/edge_bars.h, and gpu/renderer.cpp using it), at every screen shape:
//  - the Showcase (the car lot's, customize shop's and Blacklist rival cars'
//    "Rotate / Zoom" viewer) darkens the 3D garage with one front-end quad,
//    centred, x +-0.970 and y +-0.958 of the 16:9 band, its 256x256 texture
//    clear in the middle and dark toward its edges (MatchVignette,
//    TexelOffset32, PlanVignette): it is drawn stretched to the screen's
//    edges;
//  - the Blacklist rival bio (SafeHouseRivalBio.fng) sets its text and
//    picture on a black panel, x +-0.976 and y +-0.985 (a black frame,
//    PlanFill): its copies now fill everything outside it, the band's thin
//    border too, at 16:9 as well (v0.1.5: only past the band).
// Both stopped short of every screen edge: a thin border of what was behind
// at 16:9, as on a real 360's own output (a period TV's overscan hid it);
// under Vert+ the Showcase's garage above and below the band showed
// undimmed too, a bright rectangular frame.
// From the repo root:
//   clang++ -std=c++20 -O2 -UNDEBUG -Iruntime tests/menu_borders_test.cpp -o build/menu_borders_test
//   build/menu_borders_test runtime/gpu/renderer.cpp
// Planted faults: the match cases run again under deliberately wrong
// thresholds (each must fail a case), the texel and register checks are
// broken one at a time (each must refuse the plan), the pixel oracles run on
// wrong fills (each must fail), and the renderer checks run on copies of
// renderer.cpp with the wiring cut (each must fail).
#include <gpu/edge_bars.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <utility>
#include <sstream>
#include <string>
#include <vector>

using namespace gpu::bars;

static int g_failures = 0;
static bool g_quiet = false;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; if (!g_quiet) { printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } } while (0)

// The screen shapes (view scale x, y), as video/picture_fit gives them, and
// the host size each is drawn at (Auto internal resolution: 2x on the big
// ones; posScale covers it).
struct Shape
{
    const char* name;
    float sx, sy;
};
static const Shape kShapes[] = {
    { "1280x720 (16:9)", 1.0f, 1.0f },
    { "1280x800 (Deck)", 1.0f, 0.9f },
    { "3024x1898 (MacBook Pro 14)", 1.0f, 0.896207f },
    { "3600x2338 less the notch (the report's MacBook, 3600x2260)", 1.0f, 0.896f },
    { "1200x800 (3:2)", 1.0f, 0.84375f },
    { "1024x768 (4:3)", 1.0f, 0.75f },
    { "2732x2048 (iPad Pro)", 1.0f, 0.750366f },
    { "2868x1320 (iPhone 18 Pro Max)", 0.818182f, 1.0f },
    { "2560x1080 (21:9)", 0.75f, 1.0f },
    { "3440x1440", 0.744186f, 1.0f },
    { "2880x810 (32:9)", 0.5f, 1.0f },
};

// The vignette as logged (NFSMW_LOG_FRONTEND, "inset candidate": band x
// -0.970..0.970 y -0.958..0.958, t0 14749000 256x256 f6, blend 07060706,
// colour 047F7F7F fading in to FE7F7F7F) at every shape.
constexpr float kVigX = 0.970f, kVigY = 0.958f;
constexpr uint32_t kVigColour = 0xFE7F7F7F;

struct Quad
{
    Vertex v[4];
};
static Quad Rectangle(float bx0, float bx1, float by0, float by1, float sx, float sy, uint32_t argb)
{
    const float x0 = bx0 * sx, x1 = bx1 * sx, y0 = by0 * sy, y1 = by1 * sy;
    return { { { x0, y0, argb }, { x1, y0, argb }, { x1, y1, argb }, { x0, y1, argb } } };
}

// ---- The shape ----

static void Match(const char* what, const Quad& q, float sx, float sy, bool want, const VignetteRule& rule)
{
    Box box{};
    const bool got = MatchVignette(q.v, sx, sy, rule, &box);
    CHECK(got == want, "%s at x %.4f y %.4f: %s, want %s", what, sx, sy, got ? "vignette" : "none", want ? "vignette" : "none");
    if (got && want)
    {
        const float x0 = std::min(q.v[0].x, q.v[2].x), x1 = std::max(q.v[0].x, q.v[2].x);
        const float y0 = std::min(q.v[0].y, q.v[2].y), y1 = std::max(q.v[0].y, q.v[2].y);
        CHECK(box.x0 == x0 && box.x1 == x1 && box.y0 == y0 && box.y1 == y1, "%s: box %.4f..%.4f %.4f..%.4f", what, box.x0, box.x1, box.y0,
            box.y1);
    }
}

// Every shape case. Returns the failures it added (for the planted rules).
static int VignetteCases(const VignetteRule& rule)
{
    const int before = g_failures;
    for (const Shape& s : kShapes)
    {
        auto at = [&](float x0, float x1, float y0, float y1, uint32_t argb) { return Rectangle(x0, x1, y0, y1, s.sx, s.sy, argb); };
        Match("Showcase vignette", at(-kVigX, kVigX, -kVigY, kVigY, kVigColour), s.sx, s.sy, true, rule);
        Match("Showcase vignette fading in (alpha 04)", at(-kVigX, kVigX, -kVigY, kVigY, 0x047F7F7F), s.sx, s.sy, true, rule);
        Match("an overlay 0.92 of the band", at(-0.92f, 0.92f, -0.92f, 0.92f, kVigColour), s.sx, s.sy, true, rule);
        Match("an overlay 0.985 of the band", at(-0.985f, 0.985f, -0.985f, 0.985f, kVigColour), s.sx, s.sy, true, rule);
        // Its vertices in another order around it (y flipped).
        Quad flip = at(-kVigX, kVigX, kVigY, -kVigY, kVigColour);
        Match("Showcase vignette, y flipped", flip, s.sx, s.sy, true, rule);
        // What must keep its own shape (or isn't this overlay).
        Match("title key art / results picture", at(-1.104f, 1.104f, -0.981f, 0.981f, 0xFF7F7F7F), s.sx, s.sy, false, rule);
        Match("a picture over the whole band", at(-1.0f, 1.0f, -1.0f, 1.0f, 0xFF808080), s.sx, s.sy, false, rule);
        Match("an overlay 0.995 of the band", at(-0.995f, 0.995f, -0.995f, 0.995f, kVigColour), s.sx, s.sy, false, rule);
        Match("an overlay 0.88 of the band", at(-0.88f, 0.88f, -0.88f, 0.88f, kVigColour), s.sx, s.sy, false, rule);
        Match("vignette width, half the height", at(-kVigX, kVigX, -0.5f, 0.5f, kVigColour), s.sx, s.sy, false, rule);
        Match("vignette height, the whole band's width", at(-1.0f, 1.0f, -kVigY, kVigY, kVigColour), s.sx, s.sy, false, rule);
        Match("fade", at(-1.29f, 1.29f, -1.30f, 1.30f, 0xFF000000), s.sx, s.sy, false, rule);
        Match("vid cam noise overlay", at(-0.692f, 0.709f, -1.058f, 1.050f, 0x3C727272), s.sx, s.sy, false, rule);
        Match("vid cam dark overlay", at(-0.712f, 0.668f, -0.893f, 0.908f, 0x3C000000), s.sx, s.sy, false, rule);
        Match("pause dim centre", at(-0.53f, 0.57f, -1.91f, 1.91f, 0xC8000000), s.sx, s.sy, false, rule);
        Match("cam dialog box", at(-0.578f, 0.586f, -0.167f, 0.569f, 0xFD000000), s.sx, s.sy, false, rule);
        Match("vignette off-centre by 0.03 in x", at(-kVigX + 0.03f, kVigX + 0.03f, -kVigY, kVigY, kVigColour), s.sx, s.sy, false, rule);
        Match("vignette off-centre by 0.03 in y", at(-kVigX, kVigX, -kVigY + 0.03f, kVigY + 0.03f, kVigColour), s.sx, s.sy, false, rule);
        Match("vignette off-centre by 0.015 in x (the black frame rule allows 0.02, this one 0.01)",
            at(-kVigX + 0.015f, kVigX + 0.015f, -kVigY, kVigY, kVigColour), s.sx, s.sy, false, rule);
        Quad grad = at(-kVigX, kVigX, -kVigY, kVigY, kVigColour);
        grad.v[1].argb = grad.v[2].argb = 0x007F7F7F;  // a gradient, not one colour
        Match("a gradient of the vignette's shape", grad, s.sx, s.sy, false, rule);
        Quad bent = at(-kVigX, kVigX, -kVigY, kVigY, kVigColour);
        bent.v[2].x -= 0.05f;
        Match("a bent quad", bent, s.sx, s.sy, false, rule);
    }
    return g_failures - before;
}

static void Planted(const char* what, const VignetteRule& wrong)
{
    const int saved = g_failures;
    g_quiet = true;
    const int failed = VignetteCases(wrong);
    g_quiet = false;
    g_failures = saved;
    printf("planted %s: %d case(s) failed\n", what, failed);
    CHECK(failed > 0, "planted %s passed every vignette case", what);
}

// ---- Texel addressing ----

static void TexelTests()
{
    // Reference offsets: gpu/renderer.cpp's GLSL tiled2D, as a separate
    // Python port computed them (that port untiled the vignette's texture
    // dumped from guest memory into the clear-middle picture).
    struct Ref
    {
        uint32_t x, y, pitch, offset;
    };
    const Ref refs[] = { { 0, 0, 256, 0x0u }, { 1, 0, 256, 0x4u }, { 0, 1, 256, 0x10u }, { 7, 0, 256, 0x2Cu }, { 8, 0, 256, 0x40u },
        { 0, 8, 256, 0x480u }, { 0, 16, 256, 0x800u }, { 31, 31, 256, 0xF7Cu }, { 32, 0, 256, 0x1000u }, { 0, 32, 256, 0x8000u },
        { 128, 128, 256, 0x24000u }, { 255, 255, 256, 0x3FF7Cu }, { 5, 3, 256, 0x134u }, { 100, 200, 256, 0x334A0u },
        { 13, 77, 128, 0x86F4u } };
    for (const Ref& r : refs)
        CHECK(TexelOffset32(r.x, r.y, r.pitch, true) == r.offset, "tiled (%u,%u) pitch %u: %X, want %X", r.x, r.y, r.pitch,
            TexelOffset32(r.x, r.y, r.pitch, true), r.offset);
    CHECK(TexelOffset32(5, 3, 256, false) == (3u * 256u + 5u) * 4u, "linear (5,3) pitch 256");
    // A 256x256 tiled texture: every texel at its own word, all of them
    // within the 256 KB.
    std::vector<uint8_t> seen(256 * 256, 0);
    int bad = 0;
    for (uint32_t y = 0; y < 256; y++)
        for (uint32_t x = 0; x < 256; x++)
        {
            const uint32_t o = TexelOffset32(x, y, 256, true);
            if (o % 4 || o / 4 >= seen.size() || seen[o / 4]++)
                bad++;
        }
    CHECK(bad == 0, "tiled 256x256: %d texel(s) outside, unaligned or on another's word", bad);
    printf("texel addressing: %zu reference offsets, 65536 texels each on its own word\n", std::size(refs));
}

// ---- The plan on the draw as the renderer sees it ----

// The front end's matrix at a shape (edge_bars_test's): c0 0.69 x scaleX,
// c5 0.92 x scaleY, the rest 0.
struct Matrix
{
    float c[8];
};
static Matrix FrontEndMatrix(float sx, float sy)
{
    return { { 0.69f * sx, 0.0f, 0.0f, 0.0f, 0.0f, 0.92f * sy, 0.0f, 0.0f } };
}

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

// The front end's posScale (renderer.cpp: 2 sx / w, 2 sy / h, 2 ox / w - 1,
// 2 oy / h - 1): viewport s(640,-360) o(640,360) in the 1280x2048 target, as
// edge_bars_test's; `ox`, `oy` (D3D's half-pixel centres: 640.5, 360.5) and
// `vy` (flipped: +360) move it. The screen is the target's top 720 rows.
static void FrontEndPs(float ps[4], float ox = 640.0f, float oy = 360.0f, float vy = -360.0f)
{
    ps[0] = 2.0f * 640.0f / 1280.0f;
    ps[1] = 2.0f * vy / 2048.0f;
    ps[2] = 2.0f * ox / 1280.0f - 1.0f;
    ps[3] = 2.0f * oy / 2048.0f - 1.0f;
}

// The vignette's draw as dumped: blend 07060706, t0 256x256 format 6.
static Draw ShowcaseDraw(const Matrix& m, float sx, float sy, const float ps[4], float hostW, float hostH)
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
        .t0Width = 256,
        .t0Height = 256,
        .t0Format = 6,
        .posScale = { ps[0], ps[1], ps[2], ps[3] },
        .hostW = hostW,
        .hostH = hostH,
        .scissor = { 0, 0, int32_t(hostW), int32_t(hostH) } };
}

// A 256x256 tiled k_8_8_8_8 texture in guest byte order: the vignette's
// (black, alpha 0 inside an ellipse, rising to the edges; its dumped texture
// is all 00000000 in the middle) or a picture's (opaque everywhere).
struct Texture
{
    uint32_t w = 256, h = 256;
    std::vector<uint32_t> words = std::vector<uint32_t>(256 * 256, 0);
    void Set(uint32_t x, uint32_t y, uint32_t v) { words[TexelOffset32(x, y, w, true) / 4] = v; }
    uint32_t Get(uint32_t x, uint32_t y) const { return words[TexelOffset32(x, y, w, true) / 4]; }
};
static Texture VignetteTexture()
{
    Texture t;
    for (uint32_t y = 0; y < t.h; y++)
        for (uint32_t x = 0; x < t.w; x++)
        {
            const float u = (float(x) + 0.5f) / t.w * 2.0f - 1.0f, v = (float(y) + 0.5f) / t.h * 2.0f - 1.0f;
            const float r = std::sqrt(u * u * 1.1f + v * v);
            const uint32_t a = r < 0.8f ? 0u : uint32_t(std::min(255.0f, (r - 0.8f) * 600.0f));
            t.Set(x, y, a);  // A in byte 0 as stored (big-endian ARGB word), RGB 0
        }
    return t;
}
static Texture PictureTexture()
{
    Texture t;
    for (uint32_t y = 0; y < t.h; y++)
        for (uint32_t x = 0; x < t.w; x++)
            t.Set(x, y, 0xFF000000u | (x << 16) | (y << 8) | 0x40u);
    return t;
}

static Plan RunPlan(const Draw& d, const EPolys& e, const Texture& t, uint32_t vertexLimit = 4, bool texelsReadable = true)
{
    return PlanVignette(
        d,
        [&](uint32_t k, uint32_t j, uint32_t& word) {
            word = e.w[k & 3][j & 3];
            return k < vertexLimit;
        },
        [&](uint32_t x, uint32_t y, uint32_t& word) {
            if (!texelsReadable || x >= t.w || y >= t.h)
                return false;
            word = t.Get(x, y);
            return true;
        });
}

static const char* Name(Fill f)
{
    return f == Fill::Vignette ? "vignette" : f == Fill::None ? "none" : "another fill";
}

// The stretched quad against pixel centres: every host pixel of the screen
// (the target's top `screenH` rows) has its centre inside the quad as the
// new posScale draws it, and the box lands at +-kFillEdge of the viewport
// (clip space through the new posScale = the old one's at +-kFillEdge).
static void CoverOracle(const char* what, const Plan& p, const float ps[4], float hostW, float hostH, float screenH)
{
    const float* q = p.posScale;
    // The box's corners through the new posScale, and clip +-kFillEdge
    // through the draw's own: the same NDC.
    const float nx0 = p.box.x0 * q[0] + q[2], nx1 = p.box.x1 * q[0] + q[2];
    const float ny0 = p.box.y0 * q[1] + q[3], ny1 = p.box.y1 * q[1] + q[3];
    const float ex0 = -kFillEdge * ps[0] + ps[2], ex1 = kFillEdge * ps[0] + ps[2];
    const float ey0 = -kFillEdge * ps[1] + ps[3], ey1 = kFillEdge * ps[1] + ps[3];
    CHECK(std::fabs(nx0 - ex0) < 1e-4f && std::fabs(nx1 - ex1) < 1e-4f && std::fabs(ny0 - ey0) < 1e-4f && std::fabs(ny1 - ey1) < 1e-4f,
        "%s: the box lands at NDC x %.5f..%.5f y %.5f..%.5f, want x %.5f..%.5f y %.5f..%.5f", what, nx0, nx1, ny0, ny1, ex0, ex1, ey0, ey1);
    const float lx = std::min(nx0, nx1), hx = std::max(nx0, nx1), ly = std::min(ny0, ny1), hy = std::max(ny0, ny1);
    int outside = 0;
    for (int32_t y = 0; y < int32_t(screenH); y++)
        for (int32_t x = 0; x < int32_t(hostW); x++)
        {
            const float ndcX = (float(x) + 0.5f) / hostW * 2.0f - 1.0f, ndcY = (float(y) + 0.5f) / hostH * 2.0f - 1.0f;
            outside += !(ndcX > lx && ndcX < hx && ndcY > ly && ndcY < hy);
        }
    CHECK(outside == 0, "%s: %d host pixel(s) left outside the stretched vignette", what, outside);
}

static void PlanTests()
{
    const Texture vignette = VignetteTexture(), picture = PictureTexture();
    struct Host
    {
        const char* name;
        float scale, ox, oy;
    };
    // 1x, and 2x with D3D's half-pixel centres (as edge_bars_test's: the
    // viewport offset 640.5, 360.5).
    const Host hosts[] = { { "1x", 1.0f, 640.0f, 360.0f }, { "2x half-pixel", 2.0f, 640.5f, 360.5f } };
    int planned = 0;
    for (const Shape& s : kShapes)
        for (const Host& h : hosts)
            for (int flipped = 0; flipped < 2; flipped++)
            {
                float ps[4];
                // Flipped: the viewport's y scale positive, its offset the
                // band's far edge (the screen rows stay 0..720).
                FrontEndPs(ps, h.ox, flipped ? 360.0f - (h.oy - 360.0f) : h.oy, flipped ? 360.0f : -360.0f);
                const float hostW = 1280.0f * h.scale, hostH = 2048.0f * h.scale, screenH = 720.0f * h.scale;
                const Matrix m = FrontEndMatrix(s.sx, s.sy);
                const Quad q = Rectangle(-kVigX, kVigX, -kVigY, kVigY, s.sx, s.sy, kVigColour);
                const EPolys e = Polys(q, m);
                const Draw d = ShowcaseDraw(m, s.sx, s.sy, ps, hostW, hostH);
                const Plan p = RunPlan(d, e, vignette);
                char what[160];
                snprintf(what, sizeof(what), "%s, %s%s", s.name, h.name, flipped ? ", y flipped" : "");
                CHECK(p.fill == Fill::Vignette, "%s: %s, want vignette", what, Name(p.fill));
                if (p.fill != Fill::Vignette)
                    continue;
                CHECK(p.rectCount == 0, "%s: %u rects, want none (the draw itself is stretched)", what, p.rectCount);
                CHECK(p.argb == kVigColour, "%s: colour %08X", what, p.argb);
                float want[4];
                CoverFrame(p.box, d.posScale, want);
                CHECK(std::memcmp(want, p.posScale, sizeof(want)) == 0, "%s: posScale %.5f %.5f %.5f %.5f, want CoverFrame's", what, p.posScale[0],
                    p.posScale[1], p.posScale[2], p.posScale[3]);
                CoverOracle(what, p, ps, hostW, hostH, screenH);
                planned++;
            }
    printf("plan: the Showcase's draw at %zu shapes x 1x/2x x y-flipped: %d stretched, every host pixel covered\n", std::size(kShapes), planned);
    // Planted: the oracle catches a vignette left as the game draws it, and
    // one stretched only to clip +-1 at 2x (D3D's half-pixel centres: the
    // outermost column's centre lies past it).
    {
        const Shape deck = kShapes[1];
        float ps[4];
        FrontEndPs(ps, 640.5f, 360.5f);
        const Matrix m = FrontEndMatrix(deck.sx, deck.sy);
        const Draw d = ShowcaseDraw(m, deck.sx, deck.sy, ps, 2560.0f, 4096.0f);
        const Plan good = RunPlan(d, Polys(Rectangle(-kVigX, kVigX, -kVigY, kVigY, deck.sx, deck.sy, kVigColour), m), vignette);
        Plan unstretched = good, toOne = good;
        std::memcpy(unstretched.posScale, d.posScale, sizeof(unstretched.posScale));
        CoverFrame(good.box, d.posScale, toOne.posScale, 1.0f);
        for (const auto& [what, plan] : { std::pair{ "left as the game draws it", unstretched }, std::pair{ "stretched to clip +-1 only", toOne } })
        {
            const int saved = g_failures;
            g_quiet = true;
            CoverOracle(what, plan, ps, 2560.0f, 4096.0f, 1440.0f);
            g_quiet = false;
            const int failed = g_failures - saved;
            g_failures = saved;
            printf("planted oracle '%s' (Deck, 2x): %d check(s) failed\n", what, failed);
            CHECK(failed > 0, "planted oracle '%s' passed", what);
        }
    }

    // A ppc/ without the Vert+ hooks (front-end y scale 0) at 16:9.
    {
        float ps[4];
        FrontEndPs(ps);
        const Matrix m = FrontEndMatrix(1.0f, 1.0f);
        Draw d = ShowcaseDraw(m, 1.0f, 1.0f, ps, 1280.0f, 2048.0f);
        d.frontEndY = 0.0f;
        const Plan p = RunPlan(d, Polys(Rectangle(-kVigX, kVigX, -kVigY, kVigY, 1.0f, 1.0f, kVigColour), m), vignette);
        CHECK(p.fill == Fill::Vignette, "16:9 with an old ppc/: %s", Name(p.fill));
    }

    // What must be left as it is: each breaks one condition of the
    // vignette's draw (planted register and texel faults).
    float ps[4];
    FrontEndPs(ps);
    const Shape deck = kShapes[1];
    const Matrix m = FrontEndMatrix(deck.sx, deck.sy);
    const EPolys e = Polys(Rectangle(-kVigX, kVigX, -kVigY, kVigY, deck.sx, deck.sy, kVigColour), m);
    const Draw base = ShowcaseDraw(m, deck.sx, deck.sy, ps, 1280.0f, 2048.0f);
    struct Case
    {
        const char* what;
        void (*change)(Draw&);
    };
    const Case refuse[] = {
        { "opaque, no blending (00010001)", [](Draw& d) { d.blend0 = 0x00010001; } },
        { "additive blending", [](Draw& d) { d.blend0 = 0x07010701; } },
        { "the 32x32 white texture", [](Draw& d) { d.t0Width = d.t0Height = 32; } },
        { "a 64x256 texture", [](Draw& d) { d.t0Width = 64; } },
        { "a DXT1 texture (format 18)", [](Draw& d) { d.t0Format = 18; } },
        { "no texture sampled", [](Draw& d) { d.t0Sampled = false; } },
        { "inside an occlusion query", [](Draw& d) { d.occlusion = true; } },
        { "widened, covered or clipped by the other rules", [](Draw& d) { d.leftAlone = false; } },
        { "not the front-end shader", [](Draw& d) { d.frontEndShader = false; } },
        { "two quads", [](Draw& d) { d.count = 8; } },
        { "a triangle list", [](Draw& d) { d.prim = 4; } },
        { "no viewport transform", [](Draw& d) { d.vte = 0x300; } },
        { "c5 not the front end's y scale", [](Draw& d) { d.frontEndY = 0.5f; } },
    };
    for (const Case& c : refuse)
    {
        Draw d = base;
        c.change(d);
        const Plan p = RunPlan(d, e, vignette);
        CHECK(p.fill == Fill::None, "%s: %s, want none", c.what, Name(p.fill));
    }
    // A translation in the matrix (c3 / c7).
    {
        Matrix t = m;
        t.c[3] = 0.1f;
        const Plan p = RunPlan(ShowcaseDraw(t, deck.sx, deck.sy, ps, 1280.0f, 2048.0f), e, vignette);
        CHECK(p.fill == Fill::None, "x translation: %s, want none", Name(p.fill));
    }
    // The texture: an opaque picture of the same shape, a vignette with any
    // one of its middle samples dark, an unreadable texture, a vertex past
    // the buffer.
    CHECK(RunPlan(base, e, picture).fill == Fill::None, "an opaque picture of the vignette's shape was stretched");
    for (uint32_t i = 0; i < kClearSamples; i++)
    {
        Texture t = VignetteTexture();
        uint32_t x, y;
        ClearSample(i, t.w, t.h, x, y);
        CHECK(t.Get(x, y) == 0, "the test vignette is clear at sample %u", i);
        t.Set(x, y, 0x01000000u);
        CHECK(RunPlan(base, e, t).fill == Fill::None, "middle sample %u (%u,%u) not clear: still stretched", i, x, y);
    }
    CHECK(RunPlan(base, e, vignette, 4, false).fill == Fill::None, "unreadable texels: stretched anyway");
    CHECK(RunPlan(base, e, vignette, 2).fill == Fill::None, "a vertex past the buffer: stretched anyway");
    // And it is the samples that decide: a vignette whose middle is clear
    // only between the samples is refused, one clear only at them is taken.
    {
        Texture t = PictureTexture();
        for (uint32_t i = 0; i < kClearSamples; i++)
        {
            uint32_t x, y;
            ClearSample(i, t.w, t.h, x, y);
            t.Set(x, y, 0);
        }
        CHECK(RunPlan(base, e, t).fill == Fill::Vignette, "the five samples clear: not taken");
    }
    printf("plan: %zu register faults, an opaque picture, %u dark samples, unreadable texels and a short vertex buffer refused\n",
        std::size(refuse) + 1, kClearSamples);
}

// ---- The Blacklist bio's panel (a black frame, PlanFill) ----

// As logged on the Deck (NFSMW_LOG_FRONTEND, the bio pushed from the main
// menu by a test-only hook): "black frame x -0.976..0.976 y -0.887..0.887
// colour FB000000 blend 07060706 t0 1AD6E000 32x32", fading in from
// 11000000 over ~15 frames; the picture inset on it (x +-0.970, y +-0.920,
// blend 01060106) and the text are drawn after it.
constexpr float kBioX = 0.976f, kBioY = 0.985f;

static Draw PanelDraw(const Matrix& m, float sx, float sy, const float ps[4], float hostW, float hostH)
{
    Draw d = ShowcaseDraw(m, sx, sy, ps, hostW, hostH);
    d.t0Width = d.t0Height = 32;  // the game's white texture
    d.t0Format = 20;
    return d;
}

static Plan RunPanel(const Draw& d, const EPolys& e)
{
    return PlanFill(d, [&](uint32_t k, uint32_t j, uint32_t& word) {
        word = e.w[k & 3][j & 3];
        return k < 4;
    });
}

// The panel's fill against pixel centres, over the screen (the target's top
// `screenH` rows): each pixel is in exactly one of the panel (its centre
// inside the box) and the fill rectangles; ties within 1/256 pixel skipped.
// Returns the wrong pixels.
static int PanelOracle(const Box& b, const Rect* r, uint32_t n, const float ps[4], float hostW, float hostH, float screenH)
{
    const float pxX = std::fabs(ps[0]) * hostW * 0.5f, pxY = std::fabs(ps[1]) * hostH * 0.5f;
    auto tie = [](float d, float px) { return std::fabs(d) * px < 1.0f / 256.0f; };
    int wrong = 0;
    for (int32_t y = 0; y < int32_t(screenH); y++)
    {
        const float cy = ((float(y) + 0.5f) / hostH * 2.0f - 1.0f - ps[3]) / ps[1];
        for (int32_t x = 0; x < int32_t(hostW); x++)
        {
            const float cx = ((float(x) + 0.5f) / hostW * 2.0f - 1.0f - ps[2]) / ps[0];
            if (tie(cx - b.x0, pxX) || tie(cx - b.x1, pxX) || tie(cy - b.y0, pxY) || tie(cy - b.y1, pxY))
                continue;
            int in = cx > b.x0 && cx < b.x1 && cy > b.y0 && cy < b.y1;
            for (uint32_t i = 0; i < n; i++)
                in += x >= r[i].x0 && x < r[i].x1 && y >= r[i].y0 && y < r[i].y1;
            wrong += in != 1;
        }
    }
    return wrong;
}

// v0.1.1-v0.1.5's fill (a planted fault): only past the band.
static uint32_t PastBandOnly(const float ps[4], float hostW, float hostH, const Rect& scissor, float scaleX, float scaleY, Rect out[4])
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

static void BioTests()
{
    struct Host
    {
        const char* name;
        float scale, ox, oy;
    };
    const Host hosts[] = { { "1x", 1.0f, 640.0f, 360.0f }, { "2x half-pixel", 2.0f, 640.5f, 360.5f } };
    int planned = 0, oldFails = 0, cases = 0;
    for (const Shape& s : kShapes)
        for (const Host& h : hosts)
            for (int flipped = 0; flipped < 2; flipped++)
            {
                float ps[4];
                FrontEndPs(ps, h.ox, flipped ? 360.0f - (h.oy - 360.0f) : h.oy, flipped ? 360.0f : -360.0f);
                const float hostW = 1280.0f * h.scale, hostH = 2048.0f * h.scale, screenH = 720.0f * h.scale;
                const Matrix m = FrontEndMatrix(s.sx, s.sy);
                const EPolys e = Polys(Rectangle(-kBioX, kBioX, -kBioY, kBioY, s.sx, s.sy, 0xFF000000), m);
                const Draw d = PanelDraw(m, s.sx, s.sy, ps, hostW, hostH);
                const Plan p = RunPanel(d, e);
                char what[160];
                snprintf(what, sizeof(what), "bio panel, %s, %s%s", s.name, h.name, flipped ? ", y flipped" : "");
                CHECK(p.fill == Fill::Blackout, "%s: %s, want a black frame", what, Name(p.fill));
                if (p.fill != Fill::Blackout)
                    continue;
                float want[4];
                CoverFrame(p.box, d.posScale, want);
                CHECK(std::memcmp(want, p.posScale, sizeof(want)) == 0, "%s: the copies' posScale is not CoverFrame's", what);
                const int wrong = PanelOracle(p.box, p.rects, p.rectCount, ps, hostW, hostH, screenH);
                CHECK(wrong == 0, "%s: %d pixel(s) in both or neither of the panel and its fill", what, wrong);
                Rect old[4]{};
                const uint32_t n = PastBandOnly(ps, hostW, hostH, d.scissor, s.sx, s.sy, old);
                oldFails += PanelOracle(p.box, old, n, ps, hostW, hostH, screenH) > 0;
                cases++;
                planned++;
            }
    printf("bio panel: %d plans at %zu shapes x 1x/2x x y-flipped, the panel and its fill split every pixel once\n", planned, std::size(kShapes));
    printf("planted bio panel fill 'past the band only (v0.1.5)': %d of %d cases fail\n", oldFails, cases);
    CHECK(oldFails == cases && cases > 0, "planted 'past the band only' passed %d of %d cases", cases - oldFails, cases);
    // Fading in (11000000): below the black frame's alpha, left alone (as
    // before); over a picture (a large t0), never a black frame, and not a
    // vignette (opaque in the middle).
    float ps[4];
    FrontEndPs(ps);
    const Matrix m = FrontEndMatrix(1.0f, 0.9f);
    const Draw d = PanelDraw(m, 1.0f, 0.9f, ps, 1280.0f, 2048.0f);
    CHECK(RunPanel(d, Polys(Rectangle(-kBioX, kBioX, -kBioY, kBioY, 1.0f, 0.9f, 0x11000000), m)).fill == Fill::None, "the panel fading in was filled");
    const EPolys picture = Polys(Rectangle(-kBioX, kBioX, -kBioY, kBioY, 1.0f, 0.9f, 0xFF000000), m);
    Draw big = d;
    big.t0Width = big.t0Height = 256;
    big.t0Format = 6;
    CHECK(RunPanel(big, picture).fill == Fill::None, "a black quad over a large picture taken for a black frame");
    CHECK(RunPlan(big, picture, PictureTexture()).fill == Fill::None, "a picture of the panel's shape taken for a vignette");
}

// ---- gpu/renderer.cpp uses it ----

static std::string Squash(const std::string& source)
{
    std::string code, r;
    std::istringstream in(source);
    for (std::string line; std::getline(in, line);)
    {
        size_t c = line.find("//");
        code += (c == std::string::npos ? line : line.substr(0, c)) + "\n";
    }
    for (char c : code)
        if (c != ' ' && c != '\n' && c != '\t')
            r += c;
    return r;
}

static size_t Count(const std::string& s, const std::string& what)
{
    size_t n = 0;
    for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + 1))
        n++;
    return n;
}

// The renderer's wiring; returns the failures (quiet: for the planted copies).
static int RendererChecks(const std::string& source, bool quiet)
{
    const int before = g_failures;
    const bool was = g_quiet;
    g_quiet = quiet;
    const std::string all = Squash(source);
    // planEdgeFills: bars and black frames, then the backdrop, then the
    // vignette, each only when nothing earlier took the draw.
    const size_t fill = all.find("gpu::bars::Planplan=gpu::bars::PlanFill(draw,read);");
    const size_t backdrop = all.find("if(plan.fill==gpu::bars::Fill::None)plan=gpu::bars::PlanBackdrop(draw,read);");
    const size_t vignette = all.find("plan=gpu::bars::PlanVignette(draw,read,texel);");
    CHECK(fill != std::string::npos && backdrop != std::string::npos && vignette != std::string::npos && fill < backdrop &&
            backdrop < vignette,
        "planEdgeFills asks PlanFill, then PlanBackdrop, then PlanVignette");
    // Both paths (wide or tall screens, and 16:9) ask it: the black frame's
    // fill and the vignette at every shape.
    CHECK(Count(all, "barPlan=planEdgeFills(c,") == 2, "planEdgeFills asked on both paths: %zu", Count(all, "barPlan=planEdgeFills(c,"));
    CHECK(vignette != std::string::npos && all.rfind("if(plan.fill==gpu::bars::Fill::None){", vignette) != std::string::npos &&
            vignette - all.rfind("if(plan.fill==gpu::bars::Fill::None){", vignette) < 600,
        "PlanVignette only for a draw nothing else took");
    // Its texture as the GPU reads it.
    CHECK(all.find(".t0Format=uint32_t(t0.format),") != std::string::npos, "the draw's t0 format is handed over");
    CHECK(all.find("word=GpuSwap(LoadPhysical(t0Base+gpu::bars::TexelOffset32(x,y,t0Pitch,t0.tiled!=0)),uint32_t(t0.endianness));") !=
            std::string::npos,
        "texels read through TexelOffset32 at t0's base, pitch, tiling and endian");
    // The plan is applied to the draw itself on both paths (wide or tall
    // screens, and 16:9), before its constants are pushed.
    const std::string apply = "if(barPlan.fill==gpu::bars::Fill::Vignette)memcpy(pc.posScale,barPlan.posScale,sizeof(pc.posScale));";
    CHECK(Count(all, apply) == 2, "the vignette's posScale applied on both paths: %zu", Count(all, apply));
    const size_t lastApply = all.rfind(apply);
    const size_t push = lastApply == std::string::npos ? lastApply : all.find("pushConstants(pc);", lastApply);
    const size_t draw = lastApply == std::string::npos ? lastApply : all.find("vkCmdDraw", lastApply);
    CHECK(push != std::string::npos && draw != std::string::npos && push < draw,
        "the posScale is set before the draw's constants are pushed and it is drawn");
    // Both behind the A/B switch, as every edge fill.
    CHECK(all.find("staticconstboolbarFill=[]{constchar*v=std::getenv(\"NFSMW_BAR_FILL\");return!(v&&v[0]=='0');}();") != std::string::npos,
        "NFSMW_BAR_FILL=0 still turns the edge fills off");
    g_quiet = was;
    return g_failures - before;
}

static void RendererTests(const char* path)
{
    std::ifstream f(path);
    CHECK(f.good(), "cannot read %s", path);
    if (!f.good())
        return;
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string source = ss.str();
    CHECK(RendererChecks(source, false) == 0, "renderer.cpp's vignette wiring");
    // Planted: copies with the wiring cut, each must fail.
    struct Cut
    {
        const char* what;
        const char* from;
        const char* to;
        bool all;
    };
    const Cut cuts[] = {
        { "PlanVignette never asked", "plan = gpu::bars::PlanVignette(draw, read, texel);", "(void)texel;", false },
        { "PlanFill only on wide or tall screens (v0.1.5)", "gpu::bars::Plan plan = gpu::bars::PlanFill(draw, read);",
            "gpu::bars::Plan plan = aspectX < 1.0f || aspectY < 1.0f ? gpu::bars::PlanFill(draw, read) : gpu::bars::Plan{};", false },
        { "the 16:9 path never plans", "barPlan = planEdgeFills(c, game::FrontEndScaleY(), s, sv, feShader, true);", ";", false },
        { "the 16:9 path doesn't apply it", "                    if (barPlan.fill == gpu::bars::Fill::Vignette)\n                        memcpy(pc.posScale, barPlan.posScale, sizeof(pc.posScale));\n                }\n            }\n        }", "                }\n            }\n        }", false },
        { "no posScale applied at all", "memcpy(pc.posScale, barPlan.posScale, sizeof(pc.posScale));", ";", true },
        { "texels read linear", "t0.tiled != 0", "false", false },
        { "texels read without the endian", "uint32_t(t0.endianness)", "0u", false },
    };
    for (const Cut& c : cuts)
    {
        std::string copy = source;
        size_t at = copy.find(c.from), n = 0;
        while (at != std::string::npos)
        {
            copy.replace(at, std::strlen(c.from), c.to);
            n++;
            if (!c.all)
                break;
            at = copy.find(c.from, at + std::strlen(c.to));
        }
        CHECK(n > 0, "planted '%s': its text is not in renderer.cpp", c.what);
        const int saved = g_failures;
        const int failed = RendererChecks(copy, true);
        g_failures = saved;
        printf("planted renderer '%s': %d check(s) failed\n", c.what, failed);
        CHECK(failed > 0, "planted renderer '%s' passed every check", c.what);
    }
}

int main(int argc, char** argv)
{
    printf("menu borders (GitHub #19): the Showcase's vignette\n");
    VignetteCases(VignetteRule{});
    printf("shape cases at %zu screen shapes checked\n", std::size(kShapes));
    printf("\nplanted, shape (each must fail a case):\n");
    auto plant = [](const char* what, auto set) { VignetteRule r; set(r); Planted(what, r); };
    plant("centreMax 0.05", [](VignetteRule& r) { r.centreMax = 0.05f; });
    plant("centreMax 0.02", [](VignetteRule& r) { r.centreMax = 0.02f; });
    plant("extentMin 0.96", [](VignetteRule& r) { r.extentMin = 0.96f; });
    plant("extentMin 0.85", [](VignetteRule& r) { r.extentMin = 0.85f; });
    plant("extentMax 0.96", [](VignetteRule& r) { r.extentMax = 0.96f; });
    plant("extentMax 1.0", [](VignetteRule& r) { r.extentMax = 1.0f; });
    plant("cornerTolerance 0.1", [](VignetteRule& r) { r.cornerTolerance = 0.1f; });
    printf("\n");
    TexelTests();
    printf("\n");
    PlanTests();
    printf("\n");
    BioTests();
    printf("\n");
    if (argc > 1)
        RendererTests(argv[1]);
    else
        printf("(no renderer.cpp given: its checks skipped)\n");
    if (g_failures)
    {
        printf("menu_borders_test: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("menu_borders_test: all passed\n");
    return 0;
}
