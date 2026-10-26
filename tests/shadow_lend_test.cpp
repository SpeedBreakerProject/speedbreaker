// Tests for gpu::shadow_lend (runtime/gpu/shadow_lend.h): which resolves are
// deferred, what a later resolve or pass does with a deferred one, the depth
// view's swizzle; and a model of renderer.cpp's deferred shadow resolves
// (ShadowLend, RunLend, LendTargetWrite, LendGuestRead, LendTextureWrite)
// driven by random frames against the resolve-every-time behaviour of
// v0.1.9: every sample, guest read and CPU store must see what v0.1.9's would.
// From the repo root:
//   clang++ -std=c++20 -O2 -Iruntime tests/shadow_lend_test.cpp -o build/shadow_lend_test
//   build/shadow_lend_test
#include <gpu/shadow_lend.h>

#include <cstdio>
#include <map>
#include <random>
#include <vector>

using namespace gpu::shadow_lend;

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

// The sun shadow map's resolve as the game issues it (Resolve's values).
static Candidate SunMap()
{
    Candidate c;
    c.depthSource = true;
    c.format = 22;
    c.bppLog2 = 2;
    c.fusedExact = true;
    c.texConvert = 4;
    c.texWidth = c.texHeight = 1600;
    c.srcWidth = c.srcHeight = 1600;
    c.rectWidth = c.rectHeight = 1600;
    return c;
}

static void TestEligibility()
{
    CHECK(Ineligible(SunMap()) == nullptr, "the sun map is deferred");
    struct Case { const char* what; void (*change)(Candidate&); };
    const Case cases[] = {
        { "colour resolve", [](Candidate& c) { c.depthSource = false; } },
        { "float depth (k_24_8_FLOAT)", [](Candidate& c) { c.format = 23; } },
        { "16-bit destination", [](Candidate& c) { c.bppLog2 = 1; } },
        { "scaled source", [](Candidate& c) { c.scaledSource = true; } },
        { "scaled write", [](Candidate& c) { c.scaledWrite = true; } },
        { "no exact texture (partial fusion or none)", [](Candidate& c) { c.fusedExact = false; } },
        { "texture not D24 -> float", [](Candidate& c) { c.texConvert = 0; } },
        { "cube", [](Candidate& c) { c.texCube = true; } },
        { "mips", [](Candidate& c) { c.texLevels = 2; } },
        { "layers", [](Candidate& c) { c.texLayers = 6; } },
        { "texture smaller than the target (other normalized coordinates)", [](Candidate& c) { c.texWidth = c.texHeight = 1024; c.rectWidth = c.rectHeight = 1024; } },
        { "target taller than the texture", [](Candidate& c) { c.srcHeight = 1616; } },
        { "rectangle not at x 0", [](Candidate& c) { c.rectX0 = 8; } },
        { "rectangle not at y 0", [](Candidate& c) { c.rectY0 = 8; } },
        { "tile shift", [](Candidate& c) { c.srcOriginY = 256; } },
        { "rectangle narrower than the texture", [](Candidate& c) { c.rectWidth = 1592; } },
        { "rectangle shorter than the texture", [](Candidate& c) { c.rectHeight = 800; } },
    };
    for (const Case& k : cases)
    {
        Candidate c = SunMap();
        k.change(c);
        CHECK(Ineligible(c) != nullptr, "%s must not be deferred", k.what);
    }
    // A rectangle past the texture (8-pixel rounding) still covers it.
    Candidate c = SunMap();
    c.rectWidth = 1600;
    c.rectHeight = 1608;
    c.srcHeight = 1600;
    CHECK(Ineligible(c) == nullptr, "a rectangle reaching past the texture is fine");
}

static void TestOverlap()
{
    const WriteSet a{ 0x03A8A000, 1600, 22, 0, 0, 1600, 1600 };
    const uint32_t bytes = 1600 * 1600 * 4;
    CHECK(ResolveOverlap(a, 0x03A8A000, bytes, a, 0x03A8A000, bytes) == Overlap::kSuperseded, "the same resolve again");
    WriteSet half = a;
    half.y1 = 800;
    CHECK(ResolveOverlap(a, 0x03A8A000, bytes, half, 0x03A8A000, bytes / 2) == Overlap::kRunFirst, "half of it: the rest must be written");
    WriteSet other = a;
    other.format = 6;
    CHECK(ResolveOverlap(a, 0x03A8A000, bytes, other, 0x03A8A000, bytes) == Overlap::kRunFirst, "another format over it");
    const WriteSet b{ 0x0444E000, 1600, 22, 0, 0, 1600, 1600 };
    CHECK(ResolveOverlap(a, 0x03A8A000, bytes, b, 0x0444E000, bytes) == Overlap::kNone, "the other map (0444E000 starts past 03A8A000's end)");
    CHECK(ResolveOverlap(a, 0x03A8A000, bytes, b, 0x03A8A000 + bytes, 4096) == Overlap::kNone, "adjoining after");
    CHECK(ResolveOverlap(a, 0x03A8A000, bytes, b, 0x03A8A000 - 4096, 4096) == Overlap::kNone, "adjoining before");
    CHECK(ResolveOverlap(a, 0x03A8A000, bytes, b, 0x03A8A000 + bytes - 4096, 8192) == Overlap::kRunFirst, "one page in");
}

static void TestTargetWrite()
{
    CHECK(BeforeTargetWrite(false, false) == TargetWrite::kInPlace, "not lent: in place");
    CHECK(BeforeTargetWrite(false, true) == TargetWrite::kInPlace, "not lent, cleared: in place");
    CHECK(BeforeTargetWrite(true, true) == TargetWrite::kFreshImage, "lent, cleared whole: a fresh image");
    CHECK(BeforeTargetWrite(true, false) == TargetWrite::kRunDeferred, "lent, loaded: run the deferred resolves");
}

static void TestSwizzle()
{
    auto sw = [](uint32_t x, uint32_t y, uint32_t z, uint32_t w) { return x | (y << 3) | (z << 6) | (w << 9); };
    using C = Component;
    auto is = [](std::array<C, 4> a, C x, C y, C z, C w) { return a[0] == x && a[1] == y && a[2] == z && a[3] == w; };
    // R32F samples as (r, 0, 0, 1): each guest swizzle must give the same from a depth view.
    CHECK(is(DepthSwizzle(sw(0, 1, 2, 3)), C::kR, C::kZero, C::kZero, C::kOne), "xyzw");
    CHECK(is(DepthSwizzle(sw(0, 0, 0, 0)), C::kR, C::kR, C::kR, C::kR), "xxxx");
    CHECK(is(DepthSwizzle(sw(4, 5, 0, 3)), C::kZero, C::kOne, C::kR, C::kOne), "01xw");
    CHECK(is(DepthSwizzle(sw(6, 7, 6, 7)), C::kR, C::kZero, C::kZero, C::kOne), "identity components");
    CHECK(is(DepthSwizzle(sw(3, 2, 1, 0)), C::kOne, C::kZero, C::kZero, C::kR), "wzyx");
    // The sun maps' fetch constant (logged by the renderer): 0xB48 = x 0 (red), y 1 (green), z 5 (one), w 5 (one).
    CHECK(is(DepthSwizzle(0xB48), C::kR, C::kZero, C::kOne, C::kOne), "the sun maps' B48");
}

// ---- The model ------------------------------------------------------------
// Content is a version number; 0 is "never written". Guest memory of each map
// has two halves (a partial resolve writes the first). A CPU store writes a
// negative version.
struct Model
{
    std::mt19937 rng;
    int next = 1;
    // v0.1.9: every resolve runs at once.
    struct Truth { int target = 0; int texture[2] = {}; int guest[2][2] = {}; } truth;
    // Ours.
    std::map<int, int> image;  // image id -> content
    int img = 0, nextImg = 1;
    struct Lend { int src; int map; bool texture; bool guestOwed; bool cpuWritten; };
    std::vector<Lend> lends;
    int textureOwn[2] = {};           // a texture's own image
    int textureLend[2] = { -1, -1 };  // the image it samples, or -1
    bool textureStale[2] = {};        // written since its load (CPU store): reloads at its next use
    // A CPU store into a map's memory while a deferred resolve still owed it:
    // the resolve's guest part is dropped (the store came after it and must
    // win), so the bytes the store didn't cover keep what was there before
    // instead of the resolve's: the one way this differs from v0.1.9
    // (shadow_lend.h). Until the map's next whole resolve, only what the
    // store wrote is compared.
    bool repurposed[2] = {};
    int cpuHalf[2] = { -1, -1 };
    uint64_t repurposes = 0;
    int guest[2][2] = {};
    uint64_t steps = 0, deferred = 0, fresh = 0, ran = 0, superseded = 0, dropped = 0;

    explicit Model(uint32_t seed) : rng(seed) { image[img] = 0; }

    bool Lent(int id) const
    {
        for (const Lend& L : lends)
            if (L.src == id)
                return true;
        return textureLend[0] == id || textureLend[1] == id;
    }
    void Collect()
    {
        std::erase_if(lends, [](const Lend& L) { return !L.texture && !L.guestOwed; });
    }
    void Run(Lend& L, bool texture, bool guestPart)
    {
        if (guestPart && L.guestOwed && L.cpuWritten)
        {
            L.guestOwed = false;  // the CPU's store wins
            dropped++;
        }
        if (texture && L.texture)
        {
            textureOwn[L.map] = image[L.src];
            textureLend[L.map] = -1;
            L.texture = false;
            ran++;
        }
        if (guestPart && L.guestOwed)
        {
            guest[L.map][0] = guest[L.map][1] = image[L.src];
            L.guestOwed = false;
            ran++;
        }
    }
    void GuestRead(int map)
    {
        for (Lend& L : lends)
            if (L.map == map && L.guestOwed)
                Run(L, false, true);
        Collect();
    }
    void TextureWrite(int map, bool whole)
    {
        for (Lend& L : lends)
            if (L.map == map && L.texture)
            {
                if (!whole)
                    Run(L, true, false);
                L.texture = false;
            }
        textureLend[map] = -1;
        Collect();
    }

    // A pass drawing into the target (clearing all of it first, or not).
    void Pass(bool clearWhole)
    {
        // What the pass leaves: new content, over the old unless it cleared
        // all of it first (a fresh image's old content is garbage).
        int v = next++;
        auto after = [&](int old) { return clearWhole ? v : int((uint32_t(old) * 2654435761u) ^ uint32_t(v)) & 0x3FFFFFFF; };
        truth.target = after(truth.target);
        switch (BeforeTargetWrite(Lent(img), clearWhole))
        {
        case TargetWrite::kInPlace: break;
        case TargetWrite::kFreshImage: img = nextImg++; image[img] = 0x7FFFFFF0 - img; fresh++; break;
        case TargetWrite::kRunDeferred:
            for (Lend& L : lends)
                if (L.src == img)
                    Run(L, true, true);
            Collect();
            CHECK(!Lent(img), "image %d still lent after its resolves ran", img);
            break;
        }
        image[img] = after(image[img]);
    }
    // A resolve of the target into map `m`, whole (deferrable) or its first half.
    void Resolve(int m, bool whole, bool deferOn)
    {
        truth.texture[m] = whole ? truth.target : truth.texture[m];  // (a half resolve isn't fused: the texture reloads)
        truth.guest[m][0] = truth.target;
        if (whole)
            truth.guest[m][1] = truth.target;
        const WriteSet full{ uint32_t(m), 1600, 22, 0, 0, 1600, 1600 }, half{ uint32_t(m), 1600, 22, 0, 0, 1600, 800 };
        const WriteSet& ws = whole ? full : half;
        for (Lend& L : lends)
            if (L.guestOwed)
                switch (ResolveOverlap(full, uint32_t(L.map) << 24, 1u << 20, ws, uint32_t(m) << 24, whole ? 1u << 20 : 1u << 19))
                {
                case Overlap::kNone: break;
                case Overlap::kSuperseded: L.guestOwed = false; superseded++; break;
                case Overlap::kRunFirst: Run(L, false, true); break;
                }
        Collect();
        if (!whole)
        {
            // Writes guest memory only; the texture is stale (reloads at its next use).
            guest[m][0] = image[img];
            textureStale[m] = true;
            return;
        }
        repurposed[m] = false;
        cpuHalf[m] = -1;
        Candidate c = SunMap();
        bool defer = deferOn && !Ineligible(c);
        TextureWrite(m, true);
        textureStale[m] = false;
        if (defer)
        {
            lends.push_back({ img, m, true, true, false });
            textureLend[m] = img;
            deferred++;
        }
        else
        {
            textureOwn[m] = image[img];
            guest[m][0] = guest[m][1] = image[img];
        }
    }
    void Sample(int m)
    {
        if (textureStale[m])
        {
            // A reload: guest memory first (owed parts), then the whole own image.
            GuestRead(m);
            TextureWrite(m, true);
            textureOwn[m] = guest[m][0] == guest[m][1] ? guest[m][0] : guest[m][0] * 100000 + guest[m][1];
            textureStale[m] = false;
            // (v0.1.9 reloads the same bytes.)
            truth.texture[m] = truth.guest[m][0] == truth.guest[m][1] ? truth.guest[m][0] : truth.guest[m][0] * 100000 + truth.guest[m][1];
        }
        int got = textureLend[m] >= 0 ? image[textureLend[m]] : textureOwn[m];
        if (!repurposed[m])
            CHECK(got == truth.texture[m], "step %llu: map %d samples %d, v0.1.9 %d", (unsigned long long)steps, m, got, truth.texture[m]);
    }
    void ReadGuest(int m)  // another texture's load, a scaled fill, a vertex fetch
    {
        GuestRead(m);
        for (int h = 0; h < 2; h++)
            if (!repurposed[m] || h == cpuHalf[m])
            CHECK(guest[m][h] == truth.guest[m][h], "step %llu: map %d guest half %d is %d, v0.1.9 %d", (unsigned long long)steps, m, h,
                guest[m][h], truth.guest[m][h]);
    }
    void CpuStore(int m, int half)
    {
        int v = -(next++);
        truth.guest[m][half] = v;
        guest[m][half] = v;
        for (Lend& L : lends)
            if (L.map == m)
            {
                if (L.guestOwed && !repurposed[m])
                {
                    repurposed[m] = true;
                    cpuHalf[m] = half;
                    repurposes++;
                }
                L.cpuWritten = true;
            }
        if (repurposed[m] && cpuHalf[m] != half)
            cpuHalf[m] = -1;  // (both halves stored: all of it is the CPU's)
        if (repurposed[m] && cpuHalf[m] == -1)
            repurposed[m] = false;
        textureStale[m] = true;
        // What the CPU wrote is never written over afterwards.
    }
    void Frame(bool deferOn, bool rare)
    {
        // The game's frame: world pass (cleared), its resolve, the cars on top, theirs, then the scene samples both.
        Pass(true);
        Resolve(0, true, deferOn);
        if (rng() % 4)
            Pass(false);  // (no cars in the front end: no second pass)
        Resolve(1, true, deferOn);
        Sample(0);
        Sample(1);
        if (rare)
        {
            switch (rng() % 6)
            {
            case 0: ReadGuest(rng() % 2); break;
            case 1: CpuStore(rng() % 2, rng() % 2); break;
            case 2: Resolve(rng() % 2, false, deferOn); break;
            case 3: Pass(rng() % 2); break;
            case 4: Sample(rng() % 2); break;
            case 5: Pass(false); Resolve(rng() % 2, true, deferOn); break;
            }
        }
        steps++;
    }
};

static void TestModel()
{
    for (uint32_t seed = 1; seed <= 4; seed++)
    {
        Model m(seed);
        for (int i = 0; i < 20000; i++)
        {
            bool deferOn = seed == 4 ? (i / 120) % 2 == 1 : true;  // seed 4: alternating windows (NFSMW_SHADOW_DEFER=alt)
            m.Frame(deferOn, m.rng() % 5 == 0);
            if (m.rng() % 50 == 0)
            {
                m.ReadGuest(0);
                m.ReadGuest(1);
            }
        }
        printf("  model seed %u: %llu frames, %llu deferred, %llu fresh images, %llu parts run, %llu superseded, %llu dropped for a CPU store "
            "(%llu with bytes it didn't cover)\n",
            seed, (unsigned long long)m.steps, (unsigned long long)m.deferred, (unsigned long long)m.fresh, (unsigned long long)m.ran,
            (unsigned long long)m.superseded, (unsigned long long)m.dropped, (unsigned long long)m.repurposes);
        CHECK(m.deferred > 0 && m.fresh > 0 && m.ran > 0 && m.superseded > 0, "every path taken");
    }
}

int main()
{
    TestEligibility();
    TestOverlap();
    TestTargetWrite();
    TestSwizzle();
    TestModel();
    if (g_failures)
    {
        printf("shadow_lend_test: %d failures\n", g_failures);
        return 1;
    }
    printf("shadow_lend_test: all passed\n");
    return 0;
}
