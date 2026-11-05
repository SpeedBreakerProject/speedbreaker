// Tests for the fade to black before a load (gpu/edge_bars.h's PlanLayer,
// and gpu/renderer.cpp using it), a report of a "black rectangle right
// before the last intro race with Razor" (v0.1.7 on a 3024x1898 MacBook, the
// career prologue's EndingNis03 fading out before the storyfmv_pin11 movie):
// one flat front-end quad, FB000000 at its darkest, x +-1.121 and y +-0.972
// of the 16:9 band, short of the screen's top and bottom (and of a wide
// screen's sides), so the frozen scene showed above and below it for the
// fade's ~1.5 s; the dim over the loading screen (B8000000, x +-1.104, y
// -0.978..0.985) is the same shape. Both are now stretched to the screen's
// edges, at every screen shape. From the repo root:
//   clang++ -std=c++20 -O2 -UNDEBUG -Iruntime tests/fade_layers_test.cpp -o build/fade_layers_test
//   build/fade_layers_test runtime/gpu/renderer.cpp
// Planted faults: the match cases run again under deliberately wrong
// thresholds (each must fail a case), the pixel oracle on the layer as drawn
// before (it must fail), and the renderer checks on copies of renderer.cpp
// with the wiring cut (each must fail).
#include <gpu/edge_bars.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

using namespace gpu::bars;

static int g_failures = 0;
static bool g_quiet = false;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; if (!g_quiet) { printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } } while (0)

// The screen shapes (view scale x, y), as menu_borders_test's.
struct Shape
{
    const char* name;
    float sx, sy;
};
static const Shape kShapes[] = {
    { "1280x720 (16:9)", 1.0f, 1.0f },
    { "1280x800 (Deck)", 1.0f, 0.9f },
    { "3024x1898 (MacBook Pro 14, the report's)", 1.0f, 0.896207f },
    { "3024x1964 (its whole screen)", 1.0f, 0.866667f },
    { "1200x800 (3:2)", 1.0f, 0.84375f },
    { "1024x768 (4:3)", 1.0f, 0.75f },
    { "2868x1320 (iPhone 18 Pro Max)", 0.818182f, 1.0f },
    { "2560x1080 (21:9)", 0.75f, 1.0f },
    { "3440x1440", 0.744186f, 1.0f },
    { "2880x810 (32:9)", 0.5f, 1.0f },
};

// As logged (NFSMW_LOG_COVERS, runs r1/x3, 2026-10-20): the fade before the
// pin11 movie and the loading screen's dim.
constexpr float kFadeX = 1.121f, kFadeY = 0.972f;
constexpr uint32_t kFade = 0xFB000000u;

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

static void Match(const char* what, const Quad& q, const Shape& s, bool want, const LayerRule& rule)
{
    const bool got = MatchLayer(q.v, s.sx, s.sy, rule);
    CHECK(got == want, "%s at %s: %s, want %s", what, s.name, got ? "layer" : "none", want ? "layer" : "none");
}

static int LayerCases(const LayerRule& rule)
{
    const int before = g_failures;
    for (const Shape& s : kShapes)
    {
        auto at = [&](float x0, float x1, float y0, float y1, uint32_t argb) { return Rectangle(x0, x1, y0, y1, s.sx, s.sy, argb); };
        // Taken: short of the screen's top and bottom at every shape (and of
        // its sides past 1.121 of the band: 21:9, 32:9, the iPhone).
        Match("the fade before the pin11 movie", at(-kFadeX, kFadeX, -kFadeY, kFadeY, kFade), s, true, rule);
        Match("the fade as it starts (alpha 08)", at(-kFadeX, kFadeX, -kFadeY, kFadeY, 0x08000000u), s, true, rule);
        Match("the loading screen's dim", at(-1.104f, 1.104f, -0.978f, 0.985f, 0xB8000000u), s, true, rule);
        Match("the fade, y flipped", at(-kFadeX, kFadeX, kFadeY, -kFadeY, kFade), s, true, rule);
        Match("a grey layer of the fade's shape", at(-kFadeX, kFadeX, -kFadeY, kFadeY, 0x80404040u), s, true, rule);
        Match("the Blacklist bio's panel fading in (the black frame rule's once opaque)", at(-0.976f, 0.976f, -0.985f, 0.985f, 0x11000000u), s,
            true, rule);
        // Left alone.
        Match("the intro's black frame (x 0.970, y 0.939: the black frame rule's)", at(-0.970f, 0.970f, -0.939f, 0.939f, 0xFF000000u), s, false,
            rule);
        Match("the intro's other black frame (y 0.920)", at(-0.970f, 0.970f, -0.920f, 0.920f, 0xFE000000u), s, false, rule);
        Match("a fade past the screen's edges (+-1.29 / +-1.40)", at(-1.29f / s.sx, 1.29f / s.sx, -1.40f / s.sy, 1.40f / s.sy, 0xFF000000u), s,
            false, rule);
        Match("vid cam dark overlay", at(-0.712f, 0.668f, -0.893f, 0.908f, 0x3C000000u), s, false, rule);
        Match("cam dialog box", at(-0.578f, 0.586f, -0.167f, 0.569f, 0xFD000000u), s, false, rule);
        Match("pause dim centre", at(-0.53f, 0.57f, -1.91f, 1.91f, 0xC8000000u), s, false, rule);
        Match("a layer 0.93 of the band", at(-0.93f, 0.93f, -0.93f, 0.93f, kFade), s, false, rule);
        Match("the fade off-centre by 0.03 in y", at(-kFadeX, kFadeX, -kFadeY + 0.03f, kFadeY + 0.03f, kFade), s, false, rule);
        // GitHub #87: the dialogs' dim (the career menu's "No aliases were
        // found" / "Loading a new alias", NFSMW_LOG_COVERS 2026-11-04), off
        // centre in x (-0.0135) but past both sides of the screen there, a
        // hair short of its top: taken wherever x still reaches past the
        // screen's sides (under Hor+ narrower than that the widen rule takes
        // it first); the same off-centre x short of the sides is not.
        Match("the dialogs' dim (x -1.307..1.280, y -0.999..0.995)", at(-1.307f, 1.280f, -0.999f, 0.995f, 0x4D000000u), s,
            1.280f * s.sx >= 1.0f, rule);
        Match("the dialogs' dim, opaque B3", at(-1.307f, 1.280f, -0.999f, 0.995f, 0xB3000000u), s, 1.280f * s.sx >= 1.0f, rule);
        Match("a layer off-centre by 0.03 in x, short of the sides", at(-0.97f + 0.03f, 0.97f + 0.03f, -kFadeY, kFadeY, kFade), s, false, rule);
        Quad grad = at(-kFadeX, kFadeX, -kFadeY, kFadeY, kFade);
        grad.v[1].argb = grad.v[2].argb = 0x00000000u;  // the pause dim's side gradients: alpha varies
        Match("a gradient of the fade's shape", grad, s, false, rule);
        Quad bent = at(-kFadeX, kFadeX, -kFadeY, kFadeY, kFade);
        bent.v[2].x -= 0.05f;
        Match("a bent quad", bent, s, false, rule);
    }
    // A layer short of the screen's edge by less than the threshold: the
    // whole band's height at 16:9 (+-1.0): nothing to stretch.
    Match("a layer over the whole 16:9 frame at 16:9", Rectangle(-kFadeX, kFadeX, -1.0f, 1.0f, 1.0f, 1.0f, kFade), kShapes[0], false, rule);
    return g_failures - before;
}

static void Planted(const char* what, const LayerRule& wrong)
{
    const int saved = g_failures;
    g_quiet = true;
    const int failed = LayerCases(wrong);
    g_quiet = false;
    g_failures = saved;
    printf("planted %s: %d case(s) failed\n", what, failed);
    CHECK(failed > 0, "planted %s passed every layer case", what);
}

// ---- The plan on the draw as the renderer sees it ----

struct Matrix
{
    float c[8];
};
static Matrix FrontEndMatrix(float sx, float sy)
{
    return { { 0.69f * sx, 0.0f, 0.0f, 0.0f, 0.0f, 0.92f * sy, 0.0f, 0.0f } };
}

// The front end's posScale (renderer.cpp: 2 sx / w, 2 sy / h, 2 ox / w - 1,
// 2 oy / h - 1): viewport s(640,-360) o(640,360) in the 1280x2048 target at
// `scale` (1x or 2x: the host viewport is the target's size times it).
static void FrontEndPs(float ps[4], float ox = 640.0f, float oy = 360.0f)
{
    ps[0] = 2.0f * 640.0f / 1280.0f;
    ps[1] = 2.0f * -360.0f / 2048.0f;
    ps[2] = 2.0f * ox / 1280.0f - 1.0f;
    ps[3] = 2.0f * oy / 2048.0f - 1.0f;
}

// The fade's draw as logged: blend 07060706, t0 32x32 (the white texture).
static Draw FadeDraw(const Matrix& m, float sx, float sy, const float ps[4], float hostW, float hostH)
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
        .t0Format = 20,
        .posScale = { ps[0], ps[1], ps[2], ps[3] },
        .hostW = hostW,
        .hostH = hostH,
        .scissor = { 0, 0, int32_t(hostW), int32_t(hostH) } };
}

static Plan Run(const Draw& d, const Quad& q, const Matrix& m, uint32_t vertexLimit = 4)
{
    uint32_t w[4][4];
    for (int k = 0; k < 4; k++)
    {
        const float p[3] = { q.v[k].x / m.c[0], q.v[k].y / m.c[5], 0.0f };
        std::memcpy(w[k], p, sizeof(p));
        w[k][3] = q.v[k].argb;
    }
    return PlanLayer(d, [&](uint32_t k, uint32_t j, uint32_t& word) {
        word = w[k & 3][j & 3];
        return k < vertexLimit;
    });
}

// Every host pixel of the screen (the target's top `screenH` rows) has its
// centre inside the quad as `ps` draws its box: returns those outside.
static int Outside(const Box& b, const float ps[4], float hostW, float hostH, float screenH)
{
    const float nx0 = b.x0 * ps[0] + ps[2], nx1 = b.x1 * ps[0] + ps[2], ny0 = b.y0 * ps[1] + ps[3], ny1 = b.y1 * ps[1] + ps[3];
    const float lx = std::min(nx0, nx1), hx = std::max(nx0, nx1), ly = std::min(ny0, ny1), hy = std::max(ny0, ny1);
    int outside = 0;
    for (int32_t y = 0; y < int32_t(screenH); y++)
        for (int32_t x = 0; x < int32_t(hostW); x++)
        {
            const float ndcX = (float(x) + 0.5f) / hostW * 2.0f - 1.0f, ndcY = (float(y) + 0.5f) / hostH * 2.0f - 1.0f;
            outside += !(ndcX > lx && ndcX < hx && ndcY > ly && ndcY < hy);
        }
    return outside;
}

static void PlanTests()
{
    int stretched = 0;
    for (const Shape& s : kShapes)
        for (const int scale : { 1, 2 })
            for (const bool half : { false, true })  // D3D's half-pixel centres
            {
                const Matrix m = FrontEndMatrix(s.sx, s.sy);
                float ps[4];
                FrontEndPs(ps, half ? 640.5f : 640.0f, half ? 360.5f : 360.0f);
                const float hostW = 1280.0f * scale, hostH = 2048.0f * scale, screenH = 720.0f * scale;
                const Draw d = FadeDraw(m, s.sx, s.sy, ps, hostW, hostH);
                const Quad fade = Rectangle(-kFadeX, kFadeX, -kFadeY, kFadeY, s.sx, s.sy, kFade);
                const Plan p = Run(d, fade, m);
                CHECK(p.fill == Fill::Layer, "the fade at %s, %dx%s: not taken", s.name, scale, half ? " (half-pixel)" : "");
                if (p.fill != Fill::Layer)
                    continue;
                CHECK(p.rectCount == 0 && p.argb == kFade, "the fade's plan at %s: %u rectangles, colour %08X", s.name, p.rectCount, p.argb);
                // The oracle: as drawn before, the screen's top and bottom
                // rows (and a wide screen's sides) are outside it; stretched,
                // none.
                const int before = Outside(p.box, ps, hostW, hostH, screenH), after = Outside(p.box, p.posScale, hostW, hostH, screenH);
                CHECK(before > 0, "the fade as the game draws it covers %s's whole screen (the oracle sees nothing to fix)", s.name);
                CHECK(after == 0, "the fade stretched at %s, %dx: %d host pixel(s) still outside it", s.name, scale, after);
                stretched++;
            }
    printf("the fade stretched to the screen's edges: %d shape/scale/centre cases\n", stretched);
    // The register checks refuse what isn't a flat front-end layer.
    const Shape& mac = kShapes[2];
    const Matrix m = FrontEndMatrix(mac.sx, mac.sy);
    float ps[4];
    FrontEndPs(ps);
    const Quad fade = Rectangle(-kFadeX, kFadeX, -kFadeY, kFadeY, mac.sx, mac.sy, kFade);
    struct Fault
    {
        const char* what;
        void (*apply)(Draw&);
    };
    const Fault faults[] = {
        { "over a picture (t0 1024x512: the results screen)", [](Draw& d) { d.t0Width = 1024; d.t0Height = 512; } },
        { "widened by another rule", [](Draw& d) { d.leftAlone = false; } },
        { "inside an occlusion query", [](Draw& d) { d.occlusion = true; } },
        { "not the front-end shader", [](Draw& d) { d.frontEndShader = false; } },
        { "two quads", [](Draw& d) { d.count = 8; } },
        { "a triangle list", [](Draw& d) { d.prim = 4; } },
        { "additive blending", [](Draw& d) { d.blend0 = 0x01060106; } },
        { "no viewport transform", [](Draw& d) { d.vte = 0x400; } },
    };
    for (const Fault& f : faults)
    {
        Draw d = FadeDraw(m, mac.sx, mac.sy, ps, 1280.0f, 2048.0f);
        f.apply(d);
        CHECK(Run(d, fade, m).fill == Fill::None, "the fade %s taken", f.what);
    }
    Draw d = FadeDraw(m, mac.sx, mac.sy, ps, 1280.0f, 2048.0f);
    CHECK(Run(d, fade, m, 3).fill == Fill::None, "a short vertex buffer taken");
    // The black frame rule keeps its own (PlanFill first in the renderer):
    // the intro's black frame is PlanFill's, and PlanLayer leaves it.
    const Quad frame = Rectangle(-0.970f, 0.970f, -0.939f, 0.939f, mac.sx, mac.sy, 0xFF000000u);
    CHECK(Run(d, frame, m).fill == Fill::None, "the intro's black frame taken for a layer");
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

static int RendererChecks(const std::string& source, bool quiet)
{
    const int before = g_failures;
    const bool was = g_quiet;
    g_quiet = quiet;
    const std::string all = Squash(source);
    // planEdgeFills: the layer last, only when nothing earlier took the draw.
    const size_t vignette = all.find("plan=gpu::bars::PlanVignette(draw,read,texel);");
    const size_t layer = all.find("if(plan.fill==gpu::bars::Fill::None)plan=gpu::bars::PlanLayer(draw,read);");
    const size_t ret = layer == std::string::npos ? layer : all.find("returnplan;", layer);
    CHECK(vignette != std::string::npos && layer != std::string::npos && vignette < layer && ret != std::string::npos && ret - layer < 200,
        "planEdgeFills asks PlanLayer after PlanVignette, for a draw nothing else took");
    CHECK(Count(all, "barPlan=planEdgeFills(c,") == 2, "planEdgeFills asked on both paths: %zu", Count(all, "barPlan=planEdgeFills(c,"));
    // Applied to the draw itself on both paths (wide or tall screens, 16:9),
    // before its constants are pushed.
    const std::string apply = "if(barPlan.fill==gpu::bars::Fill::Layer)memcpy(pc.posScale,barPlan.posScale,sizeof(pc.posScale));";
    CHECK(Count(all, apply) == 2, "the layer's posScale applied on both paths: %zu", Count(all, apply));
    const size_t lastApply = all.rfind(apply);
    const size_t push = lastApply == std::string::npos ? lastApply : all.find("pushConstants(pc);", lastApply);
    const size_t draw = lastApply == std::string::npos ? lastApply : all.find("vkCmdDraw", lastApply);
    CHECK(push != std::string::npos && draw != std::string::npos && push < draw,
        "the posScale is set before the draw's constants are pushed and it is drawn");
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
    CHECK(RendererChecks(source, false) == 0, "renderer.cpp's layer wiring");
    struct Cut
    {
        const char* what;
        const char* from;
        const char* to;
    };
    const Cut cuts[] = {
        { "PlanLayer never asked", "plan = gpu::bars::PlanLayer(draw, read);", ";" },
        { "PlanLayer's guarded call removed", "            if (plan.fill == gpu::bars::Fill::None)\n                plan = gpu::bars::PlanLayer(draw, read);\n", "" },
        { "the 16:9 path doesn't apply it",
            "                    if (barPlan.fill == gpu::bars::Fill::Layer)\n                        memcpy(pc.posScale, barPlan.posScale, sizeof(pc.posScale));\n", "" },
        { "the wide/tall path doesn't apply it",
            "                        if (barPlan.fill == gpu::bars::Fill::Layer)\n                            memcpy(pc.posScale, barPlan.posScale, sizeof(pc.posScale));\n", "" },
    };
    for (const Cut& c : cuts)
    {
        std::string copy = source;
        const size_t at = copy.find(c.from);
        CHECK(at != std::string::npos, "planted '%s': its text is not in renderer.cpp", c.what);
        if (at == std::string::npos)
            continue;
        copy.replace(at, std::strlen(c.from), c.to);
        const int saved = g_failures;
        const int failed = RendererChecks(copy, true);
        g_failures = saved;
        printf("planted renderer '%s': %d check(s) failed\n", c.what, failed);
        CHECK(failed > 0, "planted renderer '%s' passed every check", c.what);
    }
}

int main(int argc, char** argv)
{
    CHECK(LayerCases({}) == 0, "the layer cases");
    {
        LayerRule r;
        r.extentMin = 0.90f;  // the intro's black frames
        Planted("extentMin 0.90", r);
        r = {};
        r.extentMin = 0.98f;  // the fade itself (y 0.972)
        Planted("extentMin 0.98", r);
        r = {};
        r.centreMax = 0.05f;  // the off-centre layer
        Planted("centreMax 0.05", r);
        r = {};
        r.shortMin = 0.05f;  // the fade's 0.028 short at 16:9
        Planted("shortMin 0.05", r);
        r = {};
        r.shortMin = -0.5f;  // layers already past the edges
        Planted("shortMin -0.5", r);
        r = {};
        r.cornerTolerance = 0.1f;  // the bent quad
        Planted("cornerTolerance 0.1", r);
    }
    PlanTests();
    if (argc > 1)
        RendererTests(argv[1]);
    else
        printf("(no renderer.cpp given: its checks skipped)\n");
    if (g_failures)
    {
        printf("fade_layers_test: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("fade_layers_test: all passed\n");
    return 0;
}
