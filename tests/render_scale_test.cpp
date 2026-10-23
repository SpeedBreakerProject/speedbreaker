// Tests for gpu/render_scale.h: the internal resolution as a ratio, 1.5x
// (3/2) beside the whole scales. From the repo root:
//   clang++ -std=c++20 -O2 -Iruntime tests/render_scale_test.cpp -o build/render_scale_test
//   build/render_scale_test runtime/gpu/renderer.cpp
// SB_SCALE_FUNCTIONS is the code the shaders compile (GLSL_SCALE_CODE), run
// here as C++. Checked:
//   - whole scales give exactly the arithmetic from before 1.5x (edges g x s,
//     a texel's samples at word x s + j x sx + i), so 2x and 3x don't move;
//   - at 3/2: edges, ownership (every host pixel one guest owner), the box
//     filter's weights, the depth sample, the inverse of an edge;
//   - the scaled memory's layout: every host sample of a page one word of
//     its slot, and a surface resolved in strips and side-by-side halves
//     (as the game resolves its scene) read back by the texture and
//     front-buffer untiles sample for sample, also from a texture that
//     starts at a page inside it, through both resolve paths (per thread
//     and coalesced, modelled as the shader does them);
//   - NFSMW_RENDER_SCALE's parse and the settings' half steps;
//   - with renderer.cpp's path (ctest passes it): the shaders that read or
//     write the scaled memory splice the shared code in.
// Planted bugs this catches (each tried): sbBlockIndex keeping bit 2,
// sbHostEdge rounding to nearest, sbOwner off by one, sbOverlap's upper
// bound one pixel short, a coalesced group's block index taken from the
// pixel (not the word), the untile's block corner not multiplied by den.
#include <gpu/render_scale.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using gpu::AxisScale;
using gpu::RenderScale;

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

static gpu::ScaleGlsl G;  // the shaders' functions

static const AxisScale kHalf{ 3, 2 };
static const AxisScale kAxes[] = { { 1, 1 }, { 3, 2 }, { 2, 1 }, { 3, 1 }, { 4, 1 } };

// renderer.cpp's Tiled2D (Xenos 2D tiling), as its shaders' tiled2D.
static uint32_t Tiled2D(uint32_t x, uint32_t y, uint32_t pitch, uint32_t bppLog2)
{
    uint32_t outer = (((y >> 5) * (pitch >> 5)) + (x >> 5)) << 6;
    uint32_t inner = (((y >> 1) & 7) << 3) | (x & 7);
    uint32_t oi = (outer | inner) << bppLog2;
    uint32_t bank = (y >> 4) & 1;
    uint32_t pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1);
    return ((y & 1) << 4) | (pipe << 6) | (bank << 11) | (oi & 0xF) | (((oi >> 4) & 1) << 5) | (((oi >> 5) & 7) << 8) | ((oi >> 8) << 12);
}

static void TestWholeScalesUnchanged()
{
    printf("whole scales: the arithmetic from before\n");
    for (uint32_t s : { 1u, 2u, 3u, 4u })
    {
        for (uint32_t g = 0; g < 4096; g++)
        {
            CHECK(G.sbHostEdge(g, s, 1) == g * s, "%ux: edge %u -> %u", s, g, G.sbHostEdge(g, s, 1));
            CHECK(G.sbHostSize(g, s, 1) == g * s, "%ux: size %u", s, g);
            CHECK(G.sbCenterHost(g, s, 1) == g * s, "%ux: depth sample of %u is %u, not its first", s, g, G.sbCenterHost(g, s, 1));
        }
        for (uint32_t h = 0; h < 4096; h++)
            CHECK(G.sbOwner(h, s, 1) == h / s, "%ux: owner of %u", s, h);
    }
    for (uint32_t sx : { 1u, 2u, 3u, 4u })
        for (uint32_t sy : { 1u, 2u, 3u, 4u })
        {
            RenderScale r = RenderScale::Whole(sx, sy);
            CHECK(r.SlotWords() == 1024 * sx * sy, "%ux%u: slot %u words", sx, sy, r.SlotWords());
            for (uint32_t w = 0; w < 1024; w++)
                for (uint32_t j = 0; j < sy; j++)
                    for (uint32_t i = 0; i < sx; i++)
                        CHECK(G.sbSampleWord(w, i, j, sx, sy, 1, 1) == w * sx * sy + j * sx + i, "%ux%u: word %u sample %u,%u", sx, sy, w, i, j);
        }
}

static void TestHalfAxis()
{
    printf("3/2: edges, ownership, weights, depth sample\n");
    const uint32_t n = 3, d = 2;
    // Even guest edges land exactly; odd ones on the floor of x.5.
    for (uint32_t g = 0; g < 4096; g++)
    {
        uint32_t e = G.sbHostEdge(g, n, d);
        CHECK(g % 2 == 1 || e * 2 == g * 3, "edge %u -> %u", g, e);
        CHECK(g % 2 == 0 || e * 2 + 1 == g * 3, "odd edge %u -> %u", g, e);
        CHECK(G.sbGuestEdge(e, n, d) == g, "edge %u -> %u -> %u", g, e, G.sbGuestEdge(e, n, d));
        CHECK(G.sbHostSize(g, n, d) == (g * 3 + 1) / 2, "size %u", g);
    }
    CHECK(G.sbHostEdge(1280, n, d) == 1920 && G.sbHostEdge(720, n, d) == 1080, "1280x720 -> %ux%u", G.sbHostEdge(1280, n, d), G.sbHostEdge(720, n, d));
    // Every host pixel has one owner, and the owners' spans tile the line.
    for (uint32_t h = 0; h < 6144; h++)
    {
        uint32_t g = G.sbOwner(h, n, d);
        CHECK(G.sbHostEdge(g, n, d) <= h && h < G.sbHostEdge(g + 1, n, d), "host %u: owner %u spans %u..%u", h, g, G.sbHostEdge(g, n, d),
            G.sbHostEdge(g + 1, n, d));
    }
    // Spans: even pixels one host pixel, odd ones two.
    for (uint32_t g = 0; g < 64; g++)
    {
        uint32_t span = G.sbHostEdge(g + 1, n, d) - G.sbHostEdge(g, n, d);
        CHECK(span == (g % 2 ? 2u : 1u), "guest %u spans %u", g, span);
    }
    // Box weights: num a guest pixel, den a host pixel; 2:1 and 1:2.
    for (uint32_t g = 0; g < 512; g++)
    {
        uint32_t sum = 0;
        for (uint32_t h = 0; h < 1024; h++)
            sum += G.sbOverlap(g, h, n, d);
        CHECK(sum == n, "guest %u weighs %u", g, sum);
        uint32_t lo = G.sbHostEdge(g, n, d), hi = G.sbHostSize(g + 1, n, d);
        for (uint32_t h = 0; h < 1024; h++)
            CHECK((G.sbOverlap(g, h, n, d) != 0) == (h >= lo && h < hi), "guest %u host %u: weight %u outside %u..%u", g, h,
                G.sbOverlap(g, h, n, d), lo, hi);
    }
    for (uint32_t h = 0; h < 768; h++)
    {
        uint32_t sum = 0;
        for (uint32_t g = 0; g < 512; g++)
            sum += G.sbOverlap(g, h, n, d);
        CHECK(sum == d, "host %u spread %u", h, sum);
    }
    CHECK(G.sbOverlap(0, 0, n, d) == 2 && G.sbOverlap(0, 1, n, d) == 1 && G.sbOverlap(1, 1, n, d) == 1 && G.sbOverlap(1, 2, n, d) == 2,
        "weights %u %u | %u %u", G.sbOverlap(0, 0, n, d), G.sbOverlap(0, 1, n, d), G.sbOverlap(1, 1, n, d), G.sbOverlap(1, 2, n, d));
    // The depth sample: the host pixel holding the guest pixel's centre.
    for (uint32_t g = 0; g < 4096; g++)
    {
        uint32_t h = G.sbCenterHost(g, n, d);
        double centre = (g + 0.5) * 1.5;
        CHECK(double(h) <= centre && centre < double(h + 1), "depth sample of %u is %u (centre %.2f)", g, h, centre);
    }
    // Rectangles sharing a guest edge share the host edge (no gap, no overlap):
    // the split anywhere of [0, 1280) gives host spans that add up.
    for (uint32_t cut = 0; cut <= 1280; cut++)
        CHECK(G.sbHostEdge(cut, n, d) - G.sbHostEdge(0, n, d) + G.sbHostEdge(1280, n, d) - G.sbHostEdge(cut, n, d) == 1920, "cut %u", cut);
}

// The box filter on a picture: a step at an even guest column stays exact,
// a flat picture stays flat, and the mean over whole blocks is the host mean.
static void TestBoxFilter()
{
    printf("3/2: the 1x average\n");
    const uint32_t n = 3, d = 2, W = 64, H = 4;
    const uint32_t HW = G.sbHostEdge(W, n, d), HH = G.sbHostEdge(H, n, d);
    auto average = [&](const std::vector<double>& host, uint32_t gx, uint32_t gy) {
        double v = 0;
        for (uint32_t j = G.sbHostEdge(gy, n, d); j < G.sbHostSize(gy + 1, n, d); j++)
            for (uint32_t i = G.sbHostEdge(gx, n, d); i < G.sbHostSize(gx + 1, n, d); i++)
                v += double(G.sbOverlap(gx, i, n, d) * G.sbOverlap(gy, j, n, d)) * host[j * HW + i];
        return v / double(n * n);
    };
    std::vector<double> host(HW * HH);
    for (uint32_t edge : { 10u, 32u, 50u })  // even guest columns
    {
        for (uint32_t j = 0; j < HH; j++)
            for (uint32_t i = 0; i < HW; i++)
                host[j * HW + i] = i < G.sbHostEdge(edge, n, d) ? 0.25 : 0.75;
        for (uint32_t g = 0; g < W; g++)
            CHECK(average(host, g, 1) == (g < edge ? 0.25 : 0.75), "step at %u: guest %u averages %.4f", edge, g, average(host, g, 1));
    }
    std::mt19937 rng(7);
    for (double& v : host)
        v = double(rng() % 1024) / 1023.0;
    double hostSum = 0, guestSum = 0;
    for (double v : host)
        hostSum += v;
    for (uint32_t gy = 0; gy < H; gy++)
        for (uint32_t gx = 0; gx < W; gx++)
            guestSum += average(host, gx, gy);
    CHECK(std::fabs(hostSum / (HW * HH) - guestSum / (W * H)) < 1e-12, "mean %.9f host, %.9f guest", hostSum / (HW * HH), guestSum / (W * H));
}

// Every (word, owned host sample) of a page is one word of its slot, and
// every slot word is one of them; the block word inverts the block index.
static void TestSlotLayout()
{
    printf("scaled memory: every sample one slot word\n");
    for (const AxisScale& ax : kAxes)
        for (const AxisScale& ay : kAxes)
        {
            RenderScale r{ ax, ay };
            const uint32_t words = r.SlotWords();
            CHECK(words * r.PixelsDen() == 1024 * r.PixelsNum(), "%s: %u words", gpu::ScaleName(r).c_str(), words);
            std::vector<uint32_t> hits(words, 0);
            for (uint32_t w = 0; w < 1024; w++)
            {
                uint32_t subX = G.sbSubX(w, ax.den), subY = G.sbSubY(w, ay.den);
                uint32_t b = G.sbBlockIndex(w, ax.den, ay.den);
                CHECK(G.sbBlockWord(b, ax.den, ay.den) == (w & ~(ax.den == 2 ? 1u : 0u) & ~(ay.den == 2 ? 4u : 0u)), "%s: word %u block %u",
                    gpu::ScaleName(r).c_str(), w, b);
                for (uint32_t j = G.sbHostEdge(subY, ay.num, ay.den); j < G.sbHostEdge(subY + 1, ay.num, ay.den); j++)
                    for (uint32_t i = G.sbHostEdge(subX, ax.num, ax.den); i < G.sbHostEdge(subX + 1, ax.num, ax.den); i++)
                    {
                        uint32_t k = G.sbSampleWord(w, i, j, ax.num, ay.num, ax.den, ay.den);
                        if (k < words)
                            hits[k]++;
                        else
                            CHECK(false, "%s: word %u sample %u,%u at %u of %u", gpu::ScaleName(r).c_str(), w, i, j, k, words);
                    }
            }
            uint32_t bad = 0;
            for (uint32_t k = 0; k < words; k++)
                bad += hits[k] != 1;
            CHECK(bad == 0, "%s: %u slot words not written exactly once", gpu::ScaleName(r).c_str(), bad);
        }
    // The block's texels are the words its sub bits name: at 3/2 a 2x2 block
    // at even x, y of a tiled surface is one block, whatever the pitch.
    for (uint32_t pitch : { 640u, 1280u, 1600u, 288u })
        for (uint32_t y = 0; y < 64; y += 2)
            for (uint32_t x = 0; x < pitch && x < 256; x += 2)
            {
                uint32_t a = Tiled2D(x, y, pitch, 2);
                uint32_t w = (a & 4095) >> 2, page = a >> 12;
                uint32_t b = G.sbBlockIndex(w, 2, 2);
                for (uint32_t dy = 0; dy < 2; dy++)
                    for (uint32_t dx = 0; dx < 2; dx++)
                    {
                        uint32_t t = Tiled2D(x + dx, y + dy, pitch, 2), tw = (t & 4095) >> 2;
                        CHECK((t >> 12) == page && G.sbBlockIndex(tw, 2, 2) == b && G.sbSubX(tw, 2) == dx && G.sbSubY(tw, 2) == dy,
                            "pitch %u texel %u,%u: page %u block %u sub %u,%u (block %u at %u,%u)", pitch, x + dx, y + dy, t >> 12,
                            G.sbBlockIndex(tw, 2, 2), G.sbSubX(tw, 2), G.sbSubY(tw, 2), b, x, y);
                    }
            }
}

// The scaled memory, modelled: a page table and a pool of slots.
struct Pool
{
    RenderScale scale;
    std::vector<uint32_t> words;
    std::vector<uint32_t> table;  // page -> slot + 1
    uint32_t slots = 0;
    explicit Pool(RenderScale s, uint32_t pages) : scale(s), words(size_t(s.SlotWords()) * pages, 0xDEADBEEF), table(1 << 17, 0)
    {
    }
    uint32_t Slot(uint32_t page)
    {
        if (!table[page])
            table[page] = ++slots;
        return table[page];
    }
};

// A resolve of guest rectangle [x0, x1) x [y0, y1) (of a host picture
// `src`, srcW wide, at the pool's scale) into the tiled surface at `base`,
// `pitch`: the shader's scaled writes, per thread (coalesced false) or a
// 32x2 group at a time through shared memory (coalesced true).
static void Resolve(Pool& pool, const std::vector<uint32_t>& src, uint32_t srcW, uint32_t base, uint32_t pitch, uint32_t x0, uint32_t y0,
    uint32_t x1, uint32_t y1, bool coalesced)
{
    const RenderScale& r = pool.scale;
    const uint32_t nx = r.x.num, ny = r.y.num, dx = r.x.den, dy = r.y.den, s = nx * ny;
    const uint32_t slotWords = r.SlotWords();
    const int32_t originX = int32_t(r.x.Edge(x0)), originY = int32_t(r.y.Edge(y0));
    auto thread = [&](uint32_t gx, uint32_t gy, uint32_t l, std::vector<uint32_t>* samples, std::vector<uint32_t>* offsets) {
        bool inside = gx < x1 - x0 && gy < y1 - y0;
        uint32_t x = x0 + gx, y = y0 + gy;
        uint32_t a = base + Tiled2D(x, y, pitch, 2);
        uint32_t slot = inside ? pool.Slot(a >> 12) : 0;
        uint32_t k = (a & 4095) >> 2;
        uint32_t o = (slot - 1) * slotWords + G.sbBlockIndex(k, dx, dy) * s;
        uint32_t subX = G.sbSubX(k, dx), subY = G.sbSubY(k, dy);
        uint32_t i0 = G.sbHostEdge(subX, nx, dx), i1 = G.sbHostEdge(subX + 1, nx, dx);
        uint32_t j0 = G.sbHostEdge(subY, ny, dy), j1 = G.sbHostEdge(subY + 1, ny, dy);
        uint32_t lb = G.sbBlockIndex(l, dx, dy);
        if (slot != 0)
        {
            int32_t cx = originX + int32_t(G.sbHostEdge(gx, nx, dx)) - int32_t(i0), cy = originY + int32_t(G.sbHostEdge(gy, ny, dy)) - int32_t(j0);
            for (uint32_t j = j0; j < j1; j++)
                for (uint32_t i = i0; i < i1; i++)
                {
                    uint32_t v = src[size_t(cy + int32_t(j)) * srcW + uint32_t(cx + int32_t(i))];
                    if (samples)
                        (*samples)[lb * s + j * nx + i] = v;
                    else
                        pool.words[o + j * nx + i] = v;
                }
        }
        if (offsets && subX == 0 && subY == 0)
            (*offsets)[lb] = slot != 0 ? o : ~0u;
    };
    if (!coalesced)
    {
        for (uint32_t gy = 0; gy < y1 - y0; gy++)
            for (uint32_t gx = 0; gx < x1 - x0; gx++)
                thread(gx, gy, 0, nullptr, nullptr);
        return;
    }
    for (uint32_t wy = 0; wy < (y1 - y0 + 1) / 2; wy++)
        for (uint32_t wx = 0; wx < (x1 - x0 + 31) / 32; wx++)
        {
            std::vector<uint32_t> samples(64 * 16, 0), offsets(64, ~0u);
            for (uint32_t l = 0; l < 64; l++)
                thread(wx * 32 + ((l & 3) | ((l >> 3) << 2)), wy * 2 + ((l >> 2) & 1), l, &samples, &offsets);
            for (uint32_t q = 0; q < 64 / (dx * dy) * s; q++)
                if (uint32_t po = offsets[q / s]; po != ~0u)
                    pool.words[po + q % s] = samples[q];
        }
}

// The untile (a scaled texture, the front buffer): host texel (hx, hy) of a
// texture at `base`, `pitch`.
static uint32_t Read(const Pool& pool, uint32_t base, uint32_t pitch, uint32_t hx, uint32_t hy)
{
    const RenderScale& r = pool.scale;
    uint32_t bx = hx / r.x.num, by = hy / r.y.num;
    uint32_t a = base + Tiled2D(bx * r.x.den, by * r.y.den, pitch, 2);
    uint32_t slot = pool.table[a >> 12];
    if (!slot)
        return 0;
    return pool.words[size_t(slot - 1) * r.SlotWords() + G.sbSampleWord((a & 4095) >> 2, hx - bx * r.x.num, hy - by * r.y.num, r.x.num,
        r.y.num, r.x.den, r.y.den)];
}

static void TestRoundTrip()
{
    printf("scaled memory: resolved in strips and halves, read back sample for sample\n");
    const uint32_t W = 1280, H = 720, pitch = 1280;
    for (const RenderScale& r : { RenderScale{ kHalf, kHalf }, RenderScale{ kHalf, { 1, 1 } }, RenderScale{ { 2, 1 }, kHalf },
             RenderScale::Whole(2, 2), RenderScale::Whole(3, 2) })
        for (bool coalesced : { false, true })
        {
            const uint32_t HW = r.x.Size(W), HH = r.y.Size(H);
            std::vector<uint32_t> src(size_t(HW) * HH);
            for (uint32_t j = 0; j < HH; j++)
                for (uint32_t i = 0; i < HW; i++)
                    src[size_t(j) * HW + i] = (j << 16) | i;  // each host pixel its own coordinates
            Pool pool(r, 1024);
            const uint32_t base = 0x00400000;
            // The scene's three tile strips (rows 0, 256, 512), the right one
            // in side-by-side 640-wide halves.
            Resolve(pool, src, HW, base, pitch, 0, 0, W, 256, coalesced);
            Resolve(pool, src, HW, base, pitch, 0, 256, 640, 512, coalesced);
            Resolve(pool, src, HW, base, pitch, 640, 256, W, 512, coalesced);
            Resolve(pool, src, HW, base, pitch, 0, 512, W, H, coalesced);
            uint32_t wrong = 0, first = ~0u;
            for (uint32_t j = 0; j < HH; j++)
                for (uint32_t i = 0; i < HW; i++)
                    if (Read(pool, base, pitch, i, j) != src[size_t(j) * HW + i] && wrong++ == 0)
                        first = (j << 16) | i;
            CHECK(wrong == 0, "%s %s: %u of %ux%u samples wrong, the first at %u,%u (read %08X)", gpu::ScaleName(r).c_str(),
                coalesced ? "coalesced" : "per thread", wrong, HW, HH, first & 0xFFFF, first >> 16,
                first == ~0u ? 0 : Read(pool, base, pitch, first & 0xFFFF, first >> 16));
            // A texture of the right half, starting 20 tiles in (the world's
            // entry resolve): its host texel (i, j) is the picture's (i + the
            // half's host edge, j).
            const uint32_t halfBase = base + 20 * 4096, x0 = r.x.Edge(640u);
            wrong = 0;
            for (uint32_t j = 0; j < HH; j++)
                for (uint32_t i = 0; i < r.x.Size(640); i++)
                    wrong += Read(pool, halfBase, pitch, i, j) != src[size_t(j) * HW + x0 + i];
            CHECK(wrong == 0, "%s %s: the right half's texture has %u wrong samples", gpu::ScaleName(r).c_str(),
                coalesced ? "coalesced" : "per thread", wrong);
        }
}

static void TestParse()
{
    printf("NFSMW_RENDER_SCALE and the setting's half steps\n");
    struct Case { const char* text; bool ok; RenderScale want; };
    const Case cases[] = {
        { "1", true, RenderScale::Whole(1, 1) },
        { "1.5", true, { kHalf, kHalf } },
        { "1.5x1", true, { kHalf, { 1, 1 } } },
        { "3x1.5", true, { { 3, 1 }, kHalf } },
        { "2", true, RenderScale::Whole(2, 2) },
        { "3x2", true, RenderScale::Whole(3, 2) },
        { "4", true, RenderScale::Whole(4, 4) },
        { "9", true, RenderScale::Whole(4, 4) },  // clamped, as before
        { "0", true, RenderScale::Whole(1, 1) },  // as before
        { "2.5", false, {} },
        { "1.25", false, {} },
        { "abc", false, {} },
        { "2x", false, {} },
        { "1.5y2", false, {} },
        { "", false, {} },
    };
    for (const Case& c : cases)
    {
        RenderScale got = RenderScale::Whole(7, 7);
        bool ok = gpu::ParseRenderScale(c.text, got);
        CHECK(ok == c.ok, "\"%s\": %s", c.text, ok ? "accepted" : "refused");
        if (ok && c.ok)
            CHECK(got == c.want, "\"%s\": %s, want %s", c.text, gpu::ScaleName(got).c_str(), gpu::ScaleName(c.want).c_str());
    }
    CHECK(gpu::ScaleName({ kHalf, kHalf }) == "1.5x1.5", "%s", gpu::ScaleName({ kHalf, kHalf }).c_str());
    CHECK(gpu::ScaleName(RenderScale::Whole(3, 2)) == "3x2", "%s", gpu::ScaleName(RenderScale::Whole(3, 2)).c_str());
    CHECK((gpu::AxisFromHalves(2) == AxisScale{ 1, 1 }) && (gpu::AxisFromHalves(3) == kHalf) && (gpu::AxisFromHalves(4) == AxisScale{ 2, 1 }) &&
        (gpu::AxisFromHalves(6) == AxisScale{ 3, 1 }) && (gpu::AxisFromHalves(8) == AxisScale{ 4, 1 }), "half steps through 4x");
    RenderScale half{ kHalf, kHalf };
    CHECK(half.x.Size(1280) == 1920 && half.y.Size(720) == 1080 && half.SlotWords() == 2304 && half.PixelsNum() == 9 && half.PixelsDen() == 4,
        "1.5x: %ux%u, %u words a page", half.x.Size(1280), half.y.Size(720), half.SlotWords());
    CHECK(half.Exact(1280, 720) && !half.Exact(1279, 720) && !half.Exact(640, 359) && RenderScale::Whole(2, 2).Exact(1279, 359), "exact");
}

// The shaders that touch the scaled memory take the shared code.
static void TestShaders(const char* rendererPath)
{
    printf("renderer.cpp's shaders splice GLSL_SCALE_CODE\n");
    std::ifstream f(rendererPath);
    CHECK(f.good(), "can't read %s", rendererPath);
    if (!f.good())
        return;
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();
    for (const char* shader : { "kResolveGlsl", "kScaledFillGlsl", "kScaledUntileGlsl", "kFrontGlsl" })
    {
        size_t at = text.find(std::string("const char* ") + shader + " = R\"(");
        CHECK(at != std::string::npos, "%s not found", shader);
        if (at == std::string::npos)
            continue;
        size_t end = text.find("\n)\";", at);
        std::string body = text.substr(at, end - at);
        CHECK(body.find(")\" GLSL_SCALE_CODE R\"(") != std::string::npos, "%s doesn't splice GLSL_SCALE_CODE", shader);
        CHECK(body.find("sbSampleWord") != std::string::npos || body.find("sbBlockIndex") != std::string::npos,
            "%s doesn't use the block layout", shader);
    }
    const size_t untileAt = text.find("const char* kScaledUntileGlsl = R\"(");
    const size_t untileEnd = text.find("\n)\";", untileAt);
    const std::string scaledUntile = untileAt == std::string::npos || untileEnd == std::string::npos
        ? std::string{} : text.substr(untileAt, untileEnd - untileAt);
    CHECK(scaledUntile.find("if (p.convert == 4u)") != std::string::npos &&
        scaledUntile.find("floatBitsToUint(float(v >> 8) / 16777215.0)") != std::string::npos,
        "scaled textures convert resolved D24 words to the same float representation as ordinary untile");
    CHECK(text.find("bool shadowMap = fi.convert == 4 && width == 1600 && height == 1600") != std::string::npos &&
        text.find("ScaledMemory& scaledMemory = shadowPool ? s_shadowScaled : s_scaled") != std::string::npos &&
        text.find("ScaledFormat(fi, shadowPool)") != std::string::npos,
        "the 1600x1600 depth texture uses its independent scaled-memory pool");
    CHECK(text.find("(s_drawScaled || shadowPool) && scaledMemory.enabled") != std::string::npos &&
        text.find("(drawScaled || e.shadowPool)") != std::string::npos,
        "a scaled shadow texture is sampled even when the scene target is 1x");
    CHECK(text.find("updatePoolSequences(s_scaled, scaledWrite && !shadowPool, sceneSiblings)") != std::string::npos &&
        text.find("updatePoolSequences(s_shadowScaled, scaledWrite && shadowPool, shadowSiblings)") != std::string::npos,
        "resolves invalidate overlapping pages in the other scale pool");
    CHECK(text.find("(scaledMemory.direct || textureScale.HostPixels(width, height) * 4 <= scaledMemory.scratchSize)") != std::string::npos,
        "direct scaled stores don't require scratch-buffer capacity");
    const std::string glsl = GLSL_SCALE_CODE;
    for (const char* fn : { "uint sbHostEdge(", "uint sbOverlap(", "uint sbCenterHost(", "uint sbBlockIndex(", "uint sbSampleWord(" })
        CHECK(glsl.find(fn) != std::string::npos, "GLSL_SCALE_CODE lacks %s", fn);
    size_t comments = 0;
    for (size_t at = glsl.find("//"); at != std::string::npos; at = glsl.find("//", at + 2))
        comments++;
    CHECK(comments == 1 && glsl.find("/*") == std::string::npos, "%zu comments in the GLSL text (its header line only)", comments);
}

int main(int argc, char** argv)
{
    TestWholeScalesUnchanged();
    TestHalfAxis();
    TestBoxFilter();
    TestSlotLayout();
    TestRoundTrip();
    TestParse();
    if (argc > 1)
        TestShaders(argv[1]);
    if (g_failures)
    {
        printf("%d FAILED\n", g_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
