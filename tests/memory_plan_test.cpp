// Tests for gpu::memplan (runtime/gpu/memory_plan.h): where guest memory and
// the upload ring go on the devices we know. From the repo root:
//   clang++ -std=c++20 -O2 -Iruntime tests/memory_plan_test.cpp -o build/memory_plan_test
//   build/memory_plan_test
// The memory types are the devices' own: the Steam Machine's from vulkaninfo
// (2026-10-09), the others from their logs' chosen types (flags) and their
// drivers' type lists (RADV, NVIDIA's Linux driver, ANV, Turnip, MoltenVK).
#include <gpu/memory_plan.h>

#include <cstdio>
#include <string>

using namespace gpu::memplan;

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

constexpr uint64_t MB = 1ull << 20, GB = 1ull << 30;

// "name:type,..." (import has no type; a place whose heap is too small for
// it is marked "!": tried only as a last resort).
static std::string Text(const Plan& p, bool ring)
{
    std::string s;
    for (const Candidate& c : p.order)
    {
        if (!s.empty())
            s += ",";
        s += Name(c.place, ring);
        if (c.place != Place::kImport)
            s += ":" + std::to_string(c.type);
        if (!c.fits)
            s += "!";
    }
    return s;
}

static void Expect(const char* what, const Plan& p, bool ring, const char* want, const char* whyHas = nullptr)
{
    std::string got = Text(p, ring);
    printf("%-58s %s  (%s)\n", what, got.c_str(), p.why.c_str());
    CHECK(got == want, "%s: %s, want %s", what, got.c_str(), want);
    if (whyHas)
        CHECK(p.why.find(whyHas) != std::string::npos, "%s: why \"%s\" lacks \"%s\"", what, p.why.c_str(), whyHas);
}

// --- The devices -------------------------------------------------------

// Steam Machine: RADV NAVI33, discrete, resizable BAR (vulkaninfo).
static Memory SteamMachine()
{
    return { { { 0x1, 1 }, { 0x1, 1 }, { 0x6, 0 }, { 0x7, 1 }, { 0x7, 1 }, { 0xE, 0 }, { 0xE, 0 }, { 0xC1, 1 }, { 0xC6, 0 },
                 { 0xC7, 1 }, { 0xCE, 0 } },
        { 16621477888ull, 8573157376ull } };
}

// Steam Deck: RADV VANGOGH, integrated. RADV's type list for an APU (the
// log: copy in type 2, flags 6; ring in type 3, flags 7); its device-local
// heap's budget reads 5.7 GB, the carve-out is 1 GB: either.
static Memory SteamDeck(uint64_t vramHeap)
{
    return { { { 0x1, 1 }, { 0x1, 1 }, { 0x6, 0 }, { 0x7, 1 }, { 0x7, 1 }, { 0xE, 0 }, { 0xE, 0 } }, { 7 * GB, vramHeap } };
}

// Steam Frame: Turnip (Adreno 750), unified memory, no host-memory import
// (the log: copy and ring in type 0, flags 7).
static Memory SteamFrame()
{
    return { { { 0x7, 0 }, { 0xF, 0 }, { 0xB, 0 } }, { 11 * GB } };
}

// Apple silicon (MoltenVK): private, shared (the log: ring in type 1, flags
// F), memoryless.
static Memory Apple()
{
    return { { { 0x1, 0 }, { 0xF, 0 }, { 0x11, 0 } }, { 10922 * MB } };
}

// NVIDIA's Linux driver without resizable BAR (GTX 1080 Ti; an RTX 4070
// with it off): VRAM, system memory, and the 256 MB BAR window.
static Memory NvidiaNoRebar()
{
    return { { { 0x0, 1 }, { 0x1, 0 }, { 0x6, 1 }, { 0xE, 1 }, { 0x7, 2 } }, { 11 * GB, 15 * GB, 256 * MB } };
}

// The same with resizable BAR: the window is all of VRAM.
static Memory NvidiaRebar()
{
    return { { { 0x0, 1 }, { 0x1, 0 }, { 0x6, 1 }, { 0xE, 1 }, { 0x7, 2 } }, { 12 * GB, 15 * GB, 12 * GB } };
}

// RADV on a discrete GPU without resizable BAR: VRAM split into the part the
// CPU can't map and the 256 MB it can.
static Memory RadvNoRebar()
{
    return { { { 0x1, 1 }, { 0x1, 1 }, { 0x6, 0 }, { 0x7, 2 }, { 0x7, 2 }, { 0xE, 0 }, { 0xE, 0 } }, { 15 * GB, 7936 * MB, 256 * MB } };
}

// ANV on Arrow Lake (issue #2's Zenbook): one system-memory heap, every type
// device-local (the log: ring in type 0, flags 7).
static Memory AnvArrowLake()
{
    return { { { 0x7, 0 }, { 0xF, 0 } }, { 15 * GB } };
}

static GuestInput Input(Gpu gpu, bool importable, uint32_t driver = 0, const char* shared = nullptr, const char* shadow = nullptr)
{
    GuestInput in;
    in.gpu = gpu;
    in.importable = importable;
    in.importRefusal = ImportRefusal(driver);
    in.overrides = ParseOverrides(shared, shadow, nullptr);
    return in;
}

// The copy's type when the copy is used, for PlanRing (kNoType: imported).
static uint32_t CopyType(const Plan& guest)
{
    return guest.order.front().place == Place::kImport ? kNoType : guest.order.front().type;
}

// --- Tests -------------------------------------------------------------

// The devices that play today keep exactly what v0.1.0 chose.
static void Unchanged()
{
    printf("unchanged:\n");
    {
        Memory m = SteamMachine();
        Plan g = PlanGuestMemory(m, Input(Gpu::kDiscrete, true));
        Expect("Steam Machine, guest memory", g, false, "copy-vram:3,import,copy-system:2,copy-cached:5", "resizable BAR");
        Expect("Steam Machine, ring", PlanRing(m, ~0u, CopyType(g)), true, "ring-vram:3,ring-cached:5,ring-system:2");
    }
    for (uint64_t vram : { 1 * GB, 5692 * MB })
    {
        Memory m = SteamDeck(vram);
        Plan g = PlanGuestMemory(m, Input(Gpu::kIntegrated, true));
        // RADV refuses the import (the guest's memory is a shared mapping),
        // so the copy lands in type 2, as the Deck's log says.
        Expect("Steam Deck, guest memory", g, false, "import,copy-system:2,copy-cached:5,copy-vram:3");
        Expect("Steam Deck, ring (copy in type 2)", PlanRing(m, ~0u, 2), true, "ring-vram:3,ring-cached:5,ring-system:2");
    }
    {
        Memory m = SteamFrame();
        Plan g = PlanGuestMemory(m, Input(Gpu::kIntegrated, false));
        Expect("Steam Frame, guest memory", g, false, "copy-vram:0", "can't import");
        Expect("Steam Frame, ring", PlanRing(m, ~0u, CopyType(g)), true, "ring-vram:0,ring-cached:1");
    }
    {
        Memory m = Apple();
        Plan g = PlanGuestMemory(m, Input(Gpu::kIntegrated, true, 14));  // VK_DRIVER_ID_MOLTENVK
        Expect("Apple, guest memory", g, false, "import,copy-vram:1");
        Expect("Apple, ring", PlanRing(m, ~0u, CopyType(g)), true, "ring-vram:1,ring-cached:1");
    }
    // A software rasterizer (llvmpipe: one cached host-visible type).
    {
        Memory m{ { { 0xF, 0 } }, { 16 * GB } };
        Expect("llvmpipe, guest memory", PlanGuestMemory(m, Input(Gpu::kOther, true)), false, "import,copy-vram:0");
    }
}

// Issue #2, A: a discrete GPU without resizable BAR.
static void NoRebar()
{
    printf("no resizable BAR:\n");
    {
        Memory m = NvidiaNoRebar();
        Plan g = PlanGuestMemory(m, Input(Gpu::kDiscrete, true, 4));  // VK_DRIVER_ID_NVIDIA_PROPRIETARY
        Expect("NVIDIA 256 MB BAR, guest memory", g, false, "import,copy-system:2,copy-cached:3,copy-vram:4!", "no resizable BAR");
        // What issue #2's commenter found to work: import, the ring cached.
        Expect("NVIDIA 256 MB BAR, ring (imported)", PlanRing(m, ~0u, kNoType), true, "ring-cached:3,ring-system:2,ring-vram:4!",
            "too small");
        Expect("NVIDIA 256 MB BAR, ring (copy in type 2)", PlanRing(m, ~0u, 2), true, "ring-cached:3,ring-system:2,ring-vram:4!");
    }
    {
        Memory m = NvidiaRebar();
        Plan g = PlanGuestMemory(m, Input(Gpu::kDiscrete, true, 4));
        Expect("NVIDIA resizable BAR, guest memory", g, false, "copy-vram:4,import,copy-system:2,copy-cached:3");
        Expect("NVIDIA resizable BAR, ring", PlanRing(m, ~0u, CopyType(g)), true, "ring-vram:4,ring-cached:3,ring-system:2");
    }
    {
        Memory m = RadvNoRebar();
        Plan g = PlanGuestMemory(m, Input(Gpu::kDiscrete, true));
        Expect("RADV 256 MB BAR, guest memory", g, false, "import,copy-system:2,copy-cached:5,copy-vram:3!");
        Expect("RADV 256 MB BAR, ring (copy in type 2)", PlanRing(m, ~0u, 2), true, "ring-cached:5,ring-system:2,ring-vram:3!");
    }
    // A 512 MB window: the copy doesn't fit, the ring does.
    {
        Memory m = NvidiaNoRebar();
        m.heaps[2] = 512 * MB;
        Plan g = PlanGuestMemory(m, Input(Gpu::kDiscrete, true, 4));
        Expect("NVIDIA 512 MB BAR, guest memory", g, false, "import,copy-system:2,copy-cached:3,copy-vram:4!");
        Expect("NVIDIA 512 MB BAR, ring (imported)", PlanRing(m, ~0u, kNoType), true, "ring-vram:4,ring-cached:3,ring-system:2");
    }
    // Both in one heap: the copy fits, the ring beside it doesn't.
    {
        Memory m = SteamMachine();
        m.heaps[1] = 1300 * MB;  // half: 650 MB, the copy 512, both 704
        Plan g = PlanGuestMemory(m, Input(Gpu::kDiscrete, true));
        Expect("1300 MB BAR, guest memory", g, false, "copy-vram:3,import,copy-system:2,copy-cached:5");
        Expect("1300 MB BAR, ring beside the copy", PlanRing(m, ~0u, CopyType(g)), true, "ring-cached:5,ring-system:2,ring-vram:3!",
            "beside the copy");
        Expect("1300 MB BAR, ring (copy went elsewhere)", PlanRing(m, ~0u, 2), true, "ring-vram:3,ring-cached:5,ring-system:2");
    }
    // A driver without host-memory import: the copy in system memory.
    {
        Memory m = NvidiaNoRebar();
        Expect("NVIDIA 256 MB BAR, no import", PlanGuestMemory(m, Input(Gpu::kDiscrete, false, 4)), false,
            "copy-system:2,copy-cached:3,copy-vram:4!", "can't import");
    }
    // No CPU-mapped VRAM at all.
    {
        Memory m{ { { 0x1, 0 }, { 0x6, 1 }, { 0xE, 1 } }, { 8 * GB, 16 * GB } };
        Expect("no CPU-mapped VRAM, guest memory", PlanGuestMemory(m, Input(Gpu::kDiscrete, true)), false,
            "import,copy-system:1,copy-cached:2", "without CPU-mapped VRAM");
        Expect("no CPU-mapped VRAM, ring", PlanRing(m, ~0u, kNoType), true, "ring-cached:2,ring-system:1");
    }
}

// Issue #2, B: Intel's Linux driver loses the device with imported memory.
static void Intel()
{
    printf("Intel:\n");
    CHECK(ImportRefusal(6) != nullptr, "ANV (VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA) imports");
    for (uint32_t driver : { 0u, 1u, 3u, 4u, 5u, 13u, 14u })  // unknown, AMD Windows, RADV, NVIDIA, Intel Windows, llvmpipe, MoltenVK
        CHECK(ImportRefusal(driver) == nullptr, "driver %u refused", driver);
    Memory m = AnvArrowLake();
    Plan g = PlanGuestMemory(m, Input(Gpu::kIntegrated, true, 6));
    Expect("ANV, guest memory", g, false, "copy-vram:0", "Intel's Linux driver");
    Expect("ANV, ring", PlanRing(m, ~0u, CopyType(g)), true, "ring-vram:0,ring-cached:1");
    // NFSMW_SHARED_MEMORY=import still imports (to check a newer kernel).
    Expect("ANV, NFSMW_SHARED_MEMORY=import", PlanGuestMemory(m, Input(Gpu::kIntegrated, true, 6, "import")), false,
        "import,copy-vram:0", "NFSMW_SHARED_MEMORY=import");
    // A discrete Arc card keeps its copy in VRAM with resizable BAR, and
    // doesn't import without it.
    Memory arc{ { { 0x1, 0 }, { 0x7, 0 }, { 0x6, 1 }, { 0xE, 1 } }, { 8 * GB, 16 * GB } };
    Expect("Arc, resizable BAR", PlanGuestMemory(arc, Input(Gpu::kDiscrete, true, 6)), false, "copy-vram:1,copy-system:2,copy-cached:3");
    arc.heaps[0] = 256 * MB;
    Expect("Arc, 256 MB BAR", PlanGuestMemory(arc, Input(Gpu::kDiscrete, true, 6)), false, "copy-system:2,copy-cached:3,copy-vram:1!",
        "Intel's Linux driver");
}

// The environment's overrides still lead, and the default order follows.
static void EnvOverrides()
{
    printf("overrides:\n");
    Memory sm = SteamMachine();
    Expect("SM, NFSMW_SHARED_MEMORY=import", PlanGuestMemory(sm, Input(Gpu::kDiscrete, true, 0, "import")), false,
        "import,copy-vram:3,copy-system:2,copy-cached:5");
    Expect("SM, NFSMW_SHARED_MEMORY=shadow", PlanGuestMemory(sm, Input(Gpu::kDiscrete, true, 0, "shadow")), false,
        "copy-vram:3,copy-system:2,copy-cached:5");
    Expect("SM, NFSMW_SHADOW_MEMORY=gtt", PlanGuestMemory(sm, Input(Gpu::kDiscrete, true, 0, nullptr, "gtt")), false,
        "copy-system:2,import,copy-vram:3,copy-cached:5");
    Expect("Deck, NFSMW_SHADOW_MEMORY=vram", PlanGuestMemory(SteamDeck(GB), Input(Gpu::kIntegrated, true, 0, nullptr, "vram")), false,
        "import,copy-vram:3,copy-system:2,copy-cached:5");
    Expect("Deck, shadow + vram", PlanGuestMemory(SteamDeck(GB), Input(Gpu::kIntegrated, true, 0, "shadow", "vram")), false,
        "copy-vram:3,copy-system:2,copy-cached:5");
    // Issue #2's commenter on an NVIDIA card without resizable BAR.
    Expect("NVIDIA, NFSMW_SHARED_MEMORY=import", PlanGuestMemory(NvidiaNoRebar(), Input(Gpu::kDiscrete, true, 4, "import")), false,
        "import,copy-system:2,copy-cached:3,copy-vram:4!");
    // NFSMW_SHADOW_MEMORY=vram where it can't fit: tried first anyway.
    Expect("NVIDIA, NFSMW_SHADOW_MEMORY=vram", PlanGuestMemory(NvidiaNoRebar(), Input(Gpu::kDiscrete, true, 4, nullptr, "vram")), false,
        "copy-vram:4!,import,copy-system:2,copy-cached:3");
    Overrides o = ParseOverrides(nullptr, nullptr, "cached");
    CHECK(o.ring == Place::kCached, "NFSMW_RING_MEMORY=cached");
    Expect("SM ring, NFSMW_RING_MEMORY=cached", PlanRing(sm, ~0u, 3, o.ring), true, "ring-cached:5,ring-vram:3,ring-system:2");
    // MoltenVK has no cached type outside device memory: as before, the
    // ring goes to its shared type.
    Expect("Apple ring, NFSMW_RING_MEMORY=cached", PlanRing(Apple(), ~0u, kNoType, Place::kCached), true, "ring-cached:1,ring-vram:1");
    Expect("NVIDIA ring, NFSMW_RING_MEMORY=vram", PlanRing(NvidiaNoRebar(), ~0u, kNoType, ParseOverrides(nullptr, nullptr, "vram").ring),
        true, "ring-vram:4!,ring-cached:3,ring-system:2");
    Expect("SM ring, NFSMW_RING_MEMORY=system", PlanRing(sm, ~0u, 3, ParseOverrides(nullptr, nullptr, "system").ring), true,
        "ring-system:2,ring-vram:3,ring-cached:5");
    CHECK(ParseOverrides("nonsense", "nonsense", "nonsense").shared == Overrides::Shared::kAuto, "unknown values are the default");
}

// The test hook (NFSMW_TEST_BAR_HEAP_MB): the Steam Machine plans as a GPU
// without resizable BAR, exactly as RADV's split heaps would.
static void TestHook()
{
    printf("NFSMW_TEST_BAR_HEAP_MB:\n");
    Memory m = SteamMachine();
    ShrinkMappedVram(m, 256 * MB);
    CHECK(m.heaps[1] == 256 * MB && m.heaps[0] == 16621477888ull, "heaps %llu, %llu", (unsigned long long)m.heaps[0],
        (unsigned long long)m.heaps[1]);
    Plan g = PlanGuestMemory(m, Input(Gpu::kDiscrete, true));
    Expect("SM at 256 MB, guest memory", g, false, "import,copy-system:2,copy-cached:5,copy-vram:3!", "no resizable BAR");
    Expect("SM at 256 MB, ring (copy in type 2)", PlanRing(m, ~0u, 2), true, "ring-cached:5,ring-system:2,ring-vram:3!");
    // The Deck's choice, planned on the Steam Machine (NFSMW_TEST_GPU_KIND=integrated).
    Expect("SM as integrated", PlanGuestMemory(SteamMachine(), Input(Gpu::kIntegrated, true)), false,
        "import,copy-system:2,copy-cached:5,copy-vram:3");
}

// Memory types a buffer can't use, and the special ones, are never planned.
static void Types()
{
    printf("types:\n");
    Memory sm = SteamMachine();
    CHECK(FindType(sm, ~0u, Place::kVram, false) == 3, "first CPU-mapped VRAM type");
    CHECK(FindType(sm, ~(1u << 3), Place::kVram, false) == 4, "type 3 not allowed");
    CHECK(FindType(sm, (1u << 9), Place::kVram, false) == kNoType, "AMD's device-coherent type");
    CHECK(FindType(sm, ~0u, Place::kSystem, false) == 2, "system");
    CHECK(FindType(sm, ~0u, Place::kCached, false) == 5, "cached");
    CHECK(FindType(Apple(), ~0u, Place::kCached, false) == kNoType, "no cached system memory on Apple");
    CHECK(FindType(Apple(), ~0u, Place::kCached, true) == 1, "the ring's cached type on Apple");
    CHECK(Fits(sm, 3, kGuestBytes, kRingBytes), "the Steam Machine's VRAM holds both");
    CHECK(!Fits(NvidiaNoRebar(), 4, kRingBytes), "a 256 MB window holds 192 MB at most at half");
    CHECK(Size(256 * MB) == "256 MB" && Size(8573157376ull) == "7.98 GB" && Size(16621477888ull) == "15 GB", "sizes %s %s %s",
        Size(256 * MB).c_str(), Size(8573157376ull).c_str(), Size(16621477888ull).c_str());
}

int main()
{
    Unchanged();
    NoRebar();
    Intel();
    EnvOverrides();
    TestHook();
    Types();
    if (g_failures)
    {
        printf("%d FAILED\n", g_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
