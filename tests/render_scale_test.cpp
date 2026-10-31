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
//   - texel axes (a den above 2: a Steam Deck's native rows 10/9 at 1x,
//     GitHub #34, and the other screens' rows and columns): edges where
//     the rasteriser puts them, every host pixel one owner, the inverse,
//     the depth sample, the box filter's weights, the slot layout (each
//     texel its stride of samples), and the same round trips at 1x, 1.5x
//     and 2x on 16:10, a MacBook's 803 rows, 4:3 and ultrawide columns,
//     mixed with a 3/2 or whole other axis; a reader from another first
//     texel still reads its own texel's samples;
//   - NFSMW_RENDER_SCALE's parse and the settings' half steps;
//   - with renderer.cpp's path (ctest passes it): the shaders that read or
//     write the scaled memory splice the shared code in.
// Planted bugs this catches (each tried): sbBlockIndex keeping bit 2,
// sbHostEdge rounding to nearest, sbOwner off by one, sbOverlap's upper
// bound one pixel short, a coalesced group's block index taken from the
// pixel (not the word), the untile's block corner not multiplied by den;
// for texel axes: sbHostEdge flooring, sbStride rounding down, a writer
// counting from the rectangle (not srcGuest), padding samples left unwritten.
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

// A cached scaled texture a resolve stores into as well (the shader's
// SCALED_FUSE): the destination's texel 0,0 is its (texX, texY); guest size
// w x h; its host image, row by row (host width hw).
struct Hosted
{
    uint32_t texX = 0, texY = 0, w = 0, h = 0, hw = 0;
    std::vector<uint32_t>* image = nullptr;
    // Stores per host texel this resolve (each host texel is one sample's:
    // a second store is a race between threads on the GPU), and stores
    // outside the image.
    std::vector<uint8_t>* stores = nullptr;
    uint32_t* outside = nullptr;
};

// The shader's store into the hosted texture of sample (i, j) of the
// destination's texel (x, y) (sub in its block).
static void HostStore(const Hosted* t, const RenderScale& r, uint32_t x, uint32_t y, uint32_t subX, uint32_t subY, uint32_t i, uint32_t j,
    uint32_t v)
{
    if (!t)
        return;
    uint32_t tx = x + t->texX, ty = y + t->texY;
    if (tx >= t->w || ty >= t->h)
        return;
    uint32_t hx = G.sbReaderHost(tx, subX, i, r.x.num, r.x.den), hy = G.sbReaderHost(ty, subY, j, r.y.num, r.y.den);
    if (hx == ~0u || hy == ~0u)
        return;
    if (hx >= t->hw || size_t(hy) * t->hw + hx >= t->image->size())
    {
        ++*t->outside;
        return;
    }
    (*t->image)[size_t(hy) * t->hw + hx] = v;
    (*t->stores)[size_t(hy) * t->hw + hx]++;
}

// A resolve of guest rectangle [x0, x1) x [y0, y1) (of a host picture
// `src`, srcW wide, at the pool's scale) into the tiled surface at `base`,
// `pitch`: the shader's scaled writes, per thread (coalesced false) or a
// 32x2 group at a time through shared memory (coalesced true).
//   With a texel axis, the shader's SB_TEXEL code: strides, and each
//   pixel's host pixels from its place in the source (srcGuest = x0, y0:
//   the picture is the screen's, as untiled rendering's targets are).
//   hosted: also into that scaled texture. srcDY: the source's rows are
//   the destination's + srcDY (a destination base whole tile rows into
//   the surface, its rectangle from row 0; texel axes only).
static void Resolve(Pool& pool, const std::vector<uint32_t>& src, uint32_t srcW, uint32_t base, uint32_t pitch, uint32_t x0, uint32_t y0,
    uint32_t x1, uint32_t y1, bool coalesced, const Hosted* hosted = nullptr, uint32_t srcDY = 0)
{
    const RenderScale& r = pool.scale;
    if (r.x.Texel() || r.y.Texel())
    {
        const uint32_t nx = r.x.num, ny = r.y.num, dx = r.x.den, dy = r.y.den;
        const uint32_t sX = G.sbStride(nx, dx), s = sX * G.sbStride(ny, dy);
        const uint32_t slotWords = r.SlotWords();
        const uint32_t lx = G.sbLayoutDen(dx), ly = G.sbLayoutDen(dy);  // the pipeline's kDen
        const size_t srcH = src.size() / srcW;
        auto thread = [&](uint32_t gx, uint32_t gy, uint32_t l, std::vector<uint32_t>* samples, std::vector<uint32_t>* offsets) {
            bool inside = gx < x1 - x0 && gy < y1 - y0;
            uint32_t a = base + Tiled2D(x0 + gx, y0 + gy, pitch, 2);
            uint32_t slot = inside ? pool.Slot(a >> 12) : 0;
            uint32_t k = (a & 4095) >> 2;
            uint32_t o = (slot - 1) * slotWords + G.sbBlockIndex(k, dx, dy) * s;
            uint32_t subX = G.sbSubX(k, dx), subY = G.sbSubY(k, dy);
            uint32_t i0 = G.sbSampleLo(subX, nx, dx), i1 = G.sbSampleHi(subX, nx, dx);
            uint32_t j0 = G.sbSampleLo(subY, ny, dy), j1 = G.sbSampleHi(subY, ny, dy);
            uint32_t lb = G.sbBlockIndex(l, lx, ly);
            uint32_t gX = x0 + gx, gY = y0 + gy + srcDY;  // srcGuest + the pixel
            if (slot != 0)
                for (uint32_t j = j0; j < j1; j++)
                    for (uint32_t i = i0; i < i1; i++)
                    {
                        uint32_t cx = std::min<uint32_t>(G.sbSampleHost(gX, subX, i, nx, dx), srcW - 1);
                        uint32_t cy = std::min<uint32_t>(G.sbSampleHost(gY, subY, j, ny, dy), uint32_t(srcH - 1));
                        uint32_t v = src[size_t(cy) * srcW + cx];
                        HostStore(hosted, r, x0 + gx, y0 + gy, subX, subY, i, j, v);
                        if (samples)
                            (*samples)[lb * s + j * sX + i] = v;
                        else
                            pool.words[o + j * sX + i] = v;
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
        CHECK(64 / (lx * ly) * s <= 64 * 16, "%s: a coalesced group's %u samples", gpu::ScaleName(r).c_str(), 64 / (lx * ly) * s);
        for (uint32_t wy = 0; wy < (y1 - y0 + 1) / 2; wy++)
            for (uint32_t wx = 0; wx < (x1 - x0 + 31) / 32; wx++)
            {
                std::vector<uint32_t> samples(64 * 16, 0x5EED5EED), offsets(64, ~0u);
                for (uint32_t l = 0; l < 64; l++)
                    thread(wx * 32 + ((l & 3) | ((l >> 3) << 2)), wy * 2 + ((l >> 2) & 1), l, &samples, &offsets);
                for (uint32_t q = 0; q < 64 / (lx * ly) * s; q++)
                    if (uint32_t po = offsets[q / s]; po != ~0u)
                        pool.words[po + q % s] = samples[q];
            }
        return;
    }
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
                    HostStore(hosted, r, x, y, subX, subY, i, j, v);
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
    uint32_t tx = G.sbBlockTexel(hx, r.x.num, r.x.den), ty = G.sbBlockTexel(hy, r.y.num, r.y.den);
    uint32_t a = base + Tiled2D(tx, ty, pitch, 2);
    uint32_t slot = pool.table[a >> 12];
    if (!slot)
        return 0;
    return pool.words[size_t(slot - 1) * r.SlotWords() + G.sbSampleWord((a & 4095) >> 2, G.sbBlockSample(hx, r.x.num, r.x.den),
        G.sbBlockSample(hy, r.y.num, r.y.den), r.x.num, r.y.num, r.x.den, r.y.den)];
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


// Texel axes (a den above 2): the scales native rows and columns give
// (video::NativeShape): 16:10 at 1x, 1.5x, 2x, 3x (10/9, 5/3, 20/9, 10/3),
// a MacBook's 803 and 1607 rows (803/720, 1607/720), 4:3 (4/3, 8/3), an
// ultrawide's 1720 and 3440 columns (43/32, 43/16), a 19.5:9 phone's 1561.
static const AxisScale kTexelAxes[] = { { 10, 9 }, { 5, 3 }, { 20, 9 }, { 10, 3 }, { 803, 720 }, { 1607, 720 }, { 4, 3 }, { 8, 3 },
    { 43, 32 }, { 43, 16 }, { 1561, 1280 } };

static void TestTexelAxis()
{
    printf("texel axes: edges where the rasteriser puts them, owners, inverse, depth sample, weights\n");
    for (const AxisScale& a : kTexelAxes)
    {
        const uint32_t n = a.num, d = a.den;
        CHECK(a.Texel(), "%u/%u isn't a texel axis", n, d);
        // Guest edge g: the first host pixel whose centre lies at or past
        // g n / d (a pixel-aligned quad's first row: the top-left rule).
        for (uint32_t g = 0; g < 4096; g++)
        {
            uint32_t e = G.sbHostEdge(g, n, d);
            CHECK(uint64_t(2 * e + 1) * d >= uint64_t(2) * g * n && (e == 0 || uint64_t(2 * e - 1) * d < uint64_t(2) * g * n),
                "%u/%u: edge %u -> %u", n, d, g, e);
            CHECK(G.sbGuestEdge(e, n, d) == g, "%u/%u: edge %u -> %u -> %u", n, d, g, e, G.sbGuestEdge(e, n, d));
            CHECK(a.Size(g) == e, "%u/%u: size of %u is %u, not its edge %u", n, d, g, a.Size(g), e);
            // A guest pixel owns one or more host pixels, at most its stride.
            uint32_t span = G.sbHostEdge(g + 1, n, d) - e;
            CHECK(span >= 1 && span <= a.Stride(), "%u/%u: guest %u spans %u (stride %u)", n, d, g, span, a.Stride());
            // The depth sample is one of its own.
            uint32_t c = G.sbCenterHost(g, n, d);
            CHECK(c >= e && c < G.sbHostEdge(g + 1, n, d), "%u/%u: depth sample of %u is %u, outside %u..%u", n, d, g, c, e,
                G.sbHostEdge(g + 1, n, d));
        }
        for (uint32_t h = 0; h < 4096; h++)
        {
            uint32_t g = G.sbOwner(h, n, d);
            CHECK(G.sbHostEdge(g, n, d) <= h && h < G.sbHostEdge(g + 1, n, d), "%u/%u: host %u: owner %u spans %u..%u", n, d, h, g,
                G.sbHostEdge(g, n, d), G.sbHostEdge(g + 1, n, d));
            CHECK(G.sbBlockTexel(h, n, d) == g && G.sbBlockSample(h, n, d) == h - G.sbHostEdge(g, n, d), "%u/%u: host %u: texel %u sample %u",
                n, d, h, G.sbBlockTexel(h, n, d), G.sbBlockSample(h, n, d));
        }
        // Weights: num a guest pixel, den a host pixel.
        for (uint32_t g = 0; g < 300; g++)
        {
            uint32_t sum = 0;
            for (uint32_t h = g * n / d; h < G.sbHostSize(g + 1, n, d); h++)
                sum += G.sbOverlap(g, h, n, d);
            CHECK(sum == n, "%u/%u: guest %u weighs %u", n, d, g, sum);
        }
        // A writer's sample i of guest g: its own host pixels, then its last.
        for (uint32_t g = 0; g < 300; g++)
            for (uint32_t i = G.sbSampleLo(0, n, d); i < G.sbSampleHi(0, n, d); i++)
            {
                uint32_t h = G.sbSampleHost(g, 0, i, n, d);
                CHECK(G.sbOwner(h, n, d) == g, "%u/%u: guest %u sample %u is host %u, owned by %u", n, d, g, i, h, G.sbOwner(h, n, d));
            }
    }
    // The ones the screens give.
    const AxisScale deck{ 10, 9 };
    CHECK(deck.Size(720) == 800 && deck.Edge(256) == 284 && deck.Edge(512) == 569 && deck.Stride() == 2,
        "10/9: 720 rows -> %u, strips at %u and %u, stride %u", deck.Size(720), deck.Edge(256), deck.Edge(512), deck.Stride());
    CHECK((AxisScale{ 5, 3 }.Size(720) == 1200) && (AxisScale{ 20, 9 }.Size(720) == 1600) && (AxisScale{ 803, 720 }.Size(720) == 803) &&
        (AxisScale{ 43, 32 }.Size(1280) == 1720) && (AxisScale{ 43, 16 }.Size(1280) == 3440), "native sizes");
    // The scaled memory: each texel its stride, the slot all of them.
    for (const AxisScale& t : kTexelAxes)
        for (const AxisScale& other : { AxisScale{ 1, 1 }, AxisScale{ 3, 2 }, AxisScale{ 2, 1 }, AxisScale{ 3, 1 } })
            for (bool texelY : { true, false })
            {
                RenderScale r = texelY ? RenderScale{ other, t } : RenderScale{ t, other };
                const uint32_t words = r.SlotWords();
                const uint32_t lx = G.sbLayoutDen(r.x.den), ly = G.sbLayoutDen(r.y.den);
                CHECK(words == 1024 / (lx * ly) * r.x.Stride() * r.y.Stride(), "%s: %u words", gpu::ScaleName(r).c_str(), words);
                std::vector<uint32_t> hits(words, 0);
                for (uint32_t w = 0; w < 1024; w++)
                {
                    uint32_t subX = G.sbSubX(w, r.x.den), subY = G.sbSubY(w, r.y.den);
                    for (uint32_t j = G.sbSampleLo(subY, r.y.num, r.y.den); j < G.sbSampleHi(subY, r.y.num, r.y.den); j++)
                        for (uint32_t i = G.sbSampleLo(subX, r.x.num, r.x.den); i < G.sbSampleHi(subX, r.x.num, r.x.den); i++)
                        {
                            uint32_t k = G.sbSampleWord(w, i, j, r.x.num, r.y.num, r.x.den, r.y.den);
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
    // 1x10/9: 2 words a texel, 8 KB a page.
    CHECK((RenderScale{ { 1, 1 }, deck }.SlotWords() == 2048), "1x10/9: %u words a page", (RenderScale{ { 1, 1 }, deck }.SlotWords()));
}

// The round trip of TestRoundTrip at the native scales: the scene's strips
// and halves resolved, the whole texture read back sample for sample; the
// right-half texture (its own first texel 640 columns in) sample for sample
// where 640 columns are whole host ones, else each host texel still one of
// its own texel's samples.
static void TestTexelRoundTrip()
{
    printf("texel axes: resolved in strips and halves, read back sample for sample\n");
    const uint32_t W = 1280, H = 720, pitch = 1280;
    const RenderScale scales[] = {
        { { 1, 1 }, { 10, 9 } },     // Steam Deck 1x
        { { 3, 2 }, { 5, 3 } },      // 1.5x on 16:10: 1920x1200
        { { 2, 1 }, { 20, 9 } },     // 2x: 2560x1600
        { { 3, 1 }, { 10, 3 } },     // 3x: 3840x2400
        { { 1, 1 }, { 803, 720 } },  // a MacBook's 3024x1898 at 1x
        { { 2, 1 }, { 1607, 720 } }, // and 2x
        { { 1, 1 }, { 4, 3 } },      // 4:3
        { { 43, 32 }, { 1, 1 } },    // 3440x1440 at 1x: 1720x720
        { { 43, 16 }, { 2, 1 } },    // and 2x: 3440x1440
        { { 1561, 1280 }, { 1, 1 } },// a 19.5:9 phone at 1x
        { { 5, 2 }, { 1, 1 } },      // a 2.5x block axis (NativeShape can give one)
    };
    for (const RenderScale& r : scales)
        for (bool coalesced : { false, true })
        {
            const uint32_t HW = r.x.Size(W), HH = r.y.Size(H);
            std::vector<uint32_t> src(size_t(HW) * HH);
            for (uint32_t j = 0; j < HH; j++)
                for (uint32_t i = 0; i < HW; i++)
                    src[size_t(j) * HW + i] = (j << 16) | i;
            Pool pool(r, 1024);
            const uint32_t base = 0x00400000;
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
            // Every sample of every resolved texel written, the padding past
            // the ones a texel owns too (no pool fill or shared garbage left).
            uint32_t unwritten = 0;
            for (uint32_t y = 0; y < H; y++)
                for (uint32_t x = 0; x < W; x++)
                {
                    uint32_t a = base + Tiled2D(x, y, pitch, 2), w = (a & 4095) >> 2;
                    uint32_t subX = G.sbSubX(w, r.x.den), subY = G.sbSubY(w, r.y.den);
                    for (uint32_t j = G.sbSampleLo(subY, r.y.num, r.y.den); j < G.sbSampleHi(subY, r.y.num, r.y.den); j++)
                        for (uint32_t i = G.sbSampleLo(subX, r.x.num, r.x.den); i < G.sbSampleHi(subX, r.x.num, r.x.den); i++)
                        {
                            uint32_t v = pool.words[size_t(pool.table[a >> 12] - 1) * r.SlotWords() +
                                G.sbSampleWord(w, i, j, r.x.num, r.y.num, r.x.den, r.y.den)];
                            unwritten += v == 0xDEADBEEF || v == 0x5EED5EED;
                        }
                }
            CHECK(unwritten == 0, "%s %s: %u samples never written", gpu::ScaleName(r).c_str(), coalesced ? "coalesced" : "per thread", unwritten);
            const uint32_t halfBase = base + 20 * 4096, x0 = r.x.Edge(640u);
            const bool whole = uint64_t(640) * r.x.num % r.x.den == 0;
            wrong = 0;
            for (uint32_t j = 0; j < HH; j++)
                for (uint32_t i = 0; i < r.x.Size(640); i++)
                {
                    uint32_t v = Read(pool, halfBase, pitch, i, j);
                    if (whole)
                        wrong += v != src[size_t(j) * HW + x0 + i];
                    else
                        wrong += (v >> 16) != j || r.x.Texel() && G.sbOwner(v & 0xFFFF, r.x.num, r.x.den) != 640 + G.sbOwner(i, r.x.num, r.x.den);
                }
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
        { "1x10/9", true, { { 1, 1 }, { 10, 9 } } },   // native rows (GitHub #34)
        { "10/9", true, { { 10, 9 }, { 10, 9 } } },
        { "2x40/18", true, { { 2, 1 }, { 20, 9 } } },  // reduced
        { "1.5x5/3", true, { kHalf, { 5, 3 } } },
        { "3/2", true, { kHalf, kHalf } },
        { "4/2", true, RenderScale::Whole(2, 2) },
        { "5/2", false, {} },                           // 2.5x, as "2.5"
        { "9/10", false, {} },                          // below 1
        { "17/4", false, {} },                          // above 4
        { "10/0", false, {} },
        { "10/", false, {} },
        { "1x10/9x2", false, {} },
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
        (gpu::AxisFromHalves(6) == AxisScale{ 3, 1 }), "half steps");
    CHECK(gpu::ShadowScale(2) == RenderScale::Whole(1, 1), "shadow setting 1x");
    CHECK((gpu::ShadowScale(3) == RenderScale{ kHalf, kHalf }), "shadow setting 1.5x");
    CHECK(gpu::ShadowScale(4) == RenderScale::Whole(2, 2), "shadow setting 2x");
    CHECK(gpu::ShadowScale(6) == RenderScale::Whole(2, 2), "shadow setting caps at 2x");
    CHECK(gpu::ShadowPoolMegabytes(RenderScale{ kHalf, kHalf }) == 256, "1.5x shadow pool");
    CHECK(gpu::ShadowPoolMegabytes(RenderScale::Whole(2, 2)) == 384, "2x shadow pool");
    RenderScale half{ kHalf, kHalf };
    CHECK(half.x.Size(1280) == 1920 && half.y.Size(720) == 1080 && half.SlotWords() == 2304 && half.PixelsNum() == 9 && half.PixelsDen() == 4,
        "1.5x: %ux%u, %u words a page", half.x.Size(1280), half.y.Size(720), half.SlotWords());
    CHECK(half.Exact(1280, 720) && !half.Exact(1279, 720) && !half.Exact(640, 359) && RenderScale::Whole(2, 2).Exact(1279, 359), "exact");
}

// The shaders that touch the scaled memory take the shared code.
// A scaled resolve storing straight into its cached scaled texture (the
// shader's SCALED_FUSE, sbReaderHost): the texture's host image afterwards is
// exactly what its untile would read from the pool, every host texel, at
// every scale above (block and texel axes), through both resolve paths, with
// the scene resolved in strips and halves, and (texel axes) with strips whose
// destination base is whole tile rows into the texture (texY).
static void TestHostedTexture()
{
    printf("scaled resolves stored straight into their scaled texture: the untile's texels\n");
    const uint32_t W = 1280, H = 720, pitch = 1280;
    const RenderScale scales[] = {
        { kHalf, kHalf }, { kHalf, { 1, 1 } }, { { 2, 1 }, kHalf }, RenderScale::Whole(2, 2), RenderScale::Whole(3, 2),
        { { 1, 1 }, { 10, 9 } }, { { 3, 2 }, { 5, 3 } }, { { 2, 1 }, { 20, 9 } }, { { 3, 1 }, { 10, 3 } }, { { 1, 1 }, { 803, 720 } },
        { { 2, 1 }, { 1607, 720 } }, { { 1, 1 }, { 4, 3 } }, { { 43, 32 }, { 1, 1 } }, { { 43, 16 }, { 2, 1 } }, { { 1561, 1280 }, { 1, 1 } },
        { { 5, 2 }, { 1, 1 } },
    };
    for (const RenderScale& r : scales)
        for (bool coalesced : { false, true })
            for (bool offsetStrips : { false, true })
            {
                const bool texel = r.x.Texel() || r.y.Texel();
                if (offsetStrips && !texel)
                    continue;  // (the model's block path counts the source from the rectangle)
                const uint32_t HW = r.x.Size(W), HH = r.y.Size(H);
                std::vector<uint32_t> src(size_t(HW) * HH);
                for (uint32_t j = 0; j < HH; j++)
                    for (uint32_t i = 0; i < HW; i++)
                        src[size_t(j) * HW + i] = (j << 16) | i;
                Pool pool(r, 1024);
                const uint32_t base = 0x00400000;
                std::vector<uint32_t> image(size_t(HW) * HH, 0xDEADBEEF);
                std::vector<uint8_t> stores(image.size(), 0);
                uint32_t outside = 0;
                Hosted t{ 0, 0, W, H, HW, &image, &stores, &outside };
                Resolve(pool, src, HW, base, pitch, 0, 0, W, 256, coalesced, &t);
                if (offsetStrips)
                {
                    // Rows 256 and 512 through bases 8 and 16 tile rows in
                    // (40 tiles a row), each rectangle from row 0.
                    Hosted t2 = t, t3 = t;
                    t2.texY = 256;
                    t3.texY = 512;
                    Resolve(pool, src, HW, base + 8 * 40 * 4096, pitch, 0, 0, W, 256, coalesced, &t2, 256);
                    Resolve(pool, src, HW, base + 16 * 40 * 4096, pitch, 0, 0, W, H - 512, coalesced, &t3, 512);
                }
                else
                {
                    Resolve(pool, src, HW, base, pitch, 0, 256, 640, 512, coalesced, &t);
                    Resolve(pool, src, HW, base, pitch, 640, 256, W, 512, coalesced, &t);
                    Resolve(pool, src, HW, base, pitch, 0, 512, W, H, coalesced, &t);
                }
                uint32_t wrong = 0, first = ~0u;
                for (uint32_t j = 0; j < HH; j++)
                    for (uint32_t i = 0; i < HW; i++)
                        if (image[size_t(j) * HW + i] != Read(pool, base, pitch, i, j) && wrong++ == 0)
                            first = (j << 16) | i;
                CHECK(wrong == 0, "%s %s%s: %u of %ux%u host texels differ from the untile's, the first at %u,%u (%08X, untile %08X)",
                    gpu::ScaleName(r).c_str(), coalesced ? "coalesced" : "per thread", offsetStrips ? ", strips through offset bases" : "",
                    wrong, HW, HH, first & 0xFFFF, first >> 16, first == ~0u ? 0 : image[size_t(first >> 16) * HW + (first & 0xFFFF)],
                    first == ~0u ? 0 : Read(pool, base, pitch, first & 0xFFFF, first >> 16));
                uint32_t twice = 0;
                for (uint8_t n : stores)
                    twice += n > 1;
                CHECK(twice == 0 && outside == 0, "%s %s%s: %u host texels stored more than once, %u stores outside the image",
                    gpu::ScaleName(r).c_str(), coalesced ? "coalesced" : "per thread", offsetStrips ? ", offset strips" : "", twice, outside);
                // And the picture's own pixels (the untile's are those, test above).
                wrong = 0;
                for (uint32_t j = 0; j < HH; j++)
                    for (uint32_t i = 0; i < HW; i++)
                        wrong += image[size_t(j) * HW + i] != src[size_t(j) * HW + i];
                CHECK(wrong == 0, "%s %s%s: %u host texels aren't the picture's", gpu::ScaleName(r).c_str(),
                    coalesced ? "coalesced" : "per thread", offsetStrips ? ", offset strips" : "", wrong);
            }
}

// The scaled memory's default size: 512 MB on a discrete GPU (as before);
// on an integrated one 16,384 pages' slots, at most 512 MB.
static void TestPoolSize()
{
    printf("scaled memory: default size\n");
    const RenderScale deck{ { 1, 1 }, { 10, 9 } };
    CHECK(gpu::ScaledPoolMegabytes(deck, false) == 128, "Steam Deck 1x10/9: %llu MB", (unsigned long long)gpu::ScaledPoolMegabytes(deck, false));
    CHECK(gpu::ScaledPoolMegabytes(RenderScale::Whole(2, 2), false) == 256, "2x2 integrated");
    CHECK(gpu::ScaledPoolMegabytes(RenderScale{ kHalf, kHalf }, false) == 144, "1.5x integrated: %llu",
        (unsigned long long)gpu::ScaledPoolMegabytes(RenderScale{ kHalf, kHalf }, false));
    CHECK(gpu::ScaledPoolMegabytes(RenderScale::Whole(3, 2), false) == 384, "3x2 integrated");
    CHECK(gpu::ScaledPoolMegabytes(RenderScale::Whole(3, 3), false) == 512, "3x3 integrated: capped");
    for (const RenderScale& r : { deck, RenderScale::Whole(2, 2), RenderScale::Whole(4, 4) })
        CHECK(gpu::ScaledPoolMegabytes(r, true) == 512, "%s discrete", gpu::ScaleName(r).c_str());
    // Always at least the 16,384 pages (or the 512 MB cap).
    for (const RenderScale& r : { deck, RenderScale{ { 2, 1 }, { 20, 9 } }, RenderScale{ { 43, 32 }, { 1, 1 } }, RenderScale{ kHalf, { 5, 3 } } })
    {
        const uint64_t mb = gpu::ScaledPoolMegabytes(r, false);
        CHECK(mb == 512 || (mb << 20) / (4ull * r.SlotWords()) >= 16384, "%s: %llu MB holds too few pages", gpu::ScaleName(r).c_str(),
            (unsigned long long)mb);
    }
}

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
    // A texel axis's resolve and front shaders take the scale as constants
    // (SB_FIXED_*: divisions by num and den folded; Steam Deck 1x10/9 spent
    // ~2 ms a frame dividing by the push constants).
    for (const char* shader : { "kResolveGlsl", "kFrontGlsl" })
    {
        size_t at = text.find(std::string("const char* ") + shader + " = R\"(");
        size_t end = at == std::string::npos ? at : text.find("\n)\";", at);
        CHECK(at != std::string::npos && text.substr(at, end - at).find("#ifdef SB_FIXED_NUM_X") != std::string::npos,
            "%s doesn't take SB_FIXED_*", shader);
    }
    CHECK(text.find("#define SB_FIXED_NUM_X {}u") != std::string::npos, "no pipeline defines SB_FIXED_*");
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
    TestTexelAxis();
    TestTexelRoundTrip();
    TestHostedTexture();
    TestPoolSize();
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
