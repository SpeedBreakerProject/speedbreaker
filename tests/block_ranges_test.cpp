// Tests for gpu::BlockRanges (runtime/gpu/block_ranges.h): the free ranges
// of an image memory block. From the repo root:
//   clang++ -std=c++20 -O2 -Iruntime tests/block_ranges_test.cpp -o build/block_ranges_test
//   build/block_ranges_test
#include <gpu/block_ranges.h>

#include <algorithm>
#include <cstdio>
#include <random>
#include <vector>

using gpu::BlockRanges;

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct Range { uint64_t offset, size; };

// Every invariant, against the ranges the test holds: in the block, no two
// overlapping each other or a free range, free ranges merged, and every
// byte either held or free.
static void Validate(const BlockRanges& b, const std::vector<Range>& held, const char* when)
{
    uint64_t used = 0;
    std::vector<Range> all = held;
    for (const Range& r : held)
    {
        used += r.size;
        CHECK(r.offset + r.size <= b.Size(), "%s: range %llu+%llu past the block", when, (unsigned long long)r.offset, (unsigned long long)r.size);
    }
    CHECK(used == b.Used(), "%s: used %llu, block says %llu", when, (unsigned long long)used, (unsigned long long)b.Used());
    CHECK(held.size() == b.Ranges(), "%s: %zu held, block says %u", when, held.size(), b.Ranges());
    uint64_t lastEnd = 0;
    bool first = true;
    for (const auto& [offset, size] : b.FreeList())
    {
        CHECK(size > 0, "%s: empty free range at %llu", when, (unsigned long long)offset);
        CHECK(first || offset > lastEnd, "%s: free ranges at %llu touch or overlap the one before (ends %llu)", when,
            (unsigned long long)offset, (unsigned long long)lastEnd);
        first = false;
        lastEnd = offset + size;
        all.push_back({ offset, size });
    }
    std::sort(all.begin(), all.end(), [](const Range& a, const Range& c) { return a.offset < c.offset; });
    uint64_t at = 0;
    for (const Range& r : all)
    {
        CHECK(r.offset == at, "%s: gap or overlap at %llu (next range at %llu)", when, (unsigned long long)at, (unsigned long long)r.offset);
        at = r.offset + r.size;
    }
    CHECK(at == b.Size(), "%s: ranges end at %llu of %llu", when, (unsigned long long)at, (unsigned long long)b.Size());
}

static void Basics()
{
    printf("basics\n");
    BlockRanges b(1 << 20);
    CHECK(b.Empty() && b.FreeRanges() == 1 && b.LargestFree() == (1 << 20), "a new block is one free range");
    uint64_t a = b.Allocate(1000, 256);
    CHECK(a == 0, "first range at 0 (%llu)", (unsigned long long)a);
    uint64_t c = b.Allocate(4096, 65536);
    CHECK(c == 65536, "aligned to 64 KB (%llu)", (unsigned long long)c);
    // The gap 1000..65536 stays free, and best fit fills it before the tail.
    uint64_t d = b.Allocate(60000, 4);
    CHECK(d == 1000, "best fit takes the gap (%llu)", (unsigned long long)d);
    CHECK(b.Allocate(0, 1) == BlockRanges::kNone, "zero bytes");
    CHECK(b.Allocate(2 << 20, 1) == BlockRanges::kNone, "bigger than the block");
    CHECK(!b.Free(65536 + 1, 4096), "a range reaching into a free one refused");
    CHECK(b.Free(c, 4096), "free");
    CHECK(!b.Free(c, 4096), "double free refused");
    CHECK(!b.Free(1 << 20, 16), "outside the block refused");
    CHECK(b.Free(a, 1000) && b.Free(d, 60000), "free the rest");
    CHECK(b.Empty() && b.FreeRanges() == 1 && b.Used() == 0 && b.LargestFree() == (1 << 20), "merged back into one");
    // Exactly full, then nothing fits.
    BlockRanges f(4096);
    CHECK(f.Allocate(4096, 4096) == 0 && f.Allocate(1, 1) == BlockRanges::kNone && f.FreeRanges() == 0, "full");
    CHECK(f.Free(0, 4096) && f.FreeRanges() == 1, "empty again");
}

// Random image-like sizes and alignments: allocate until full, free a random
// part, allocate again, many rounds; then free everything.
static void Random(uint32_t seed)
{
    printf("random (seed %u)\n", seed);
    std::mt19937 rng(seed);
    const uint64_t kSize = 64ull << 20;
    BlockRanges b(kSize);
    std::vector<Range> held;
    uint64_t allocations = 0, refusals = 0;
    auto size = [&] {
        // Mostly small textures, some 1-8 MB targets.
        uint32_t kind = rng() % 16;
        if (kind < 10) return uint64_t(256 + rng() % (256 << 10));
        if (kind < 15) return uint64_t((256 << 10) + rng() % (768 << 10));
        return uint64_t((1 << 20) + rng() % (7 << 20));
    };
    auto alignment = [&] { static const uint64_t a[] = { 1, 256, 4096, 65536, 262144 }; return a[rng() % 5]; };
    for (int round = 0; round < 200; round++)
    {
        for (int i = 0; i < 400; i++)
        {
            uint64_t s = size(), al = alignment();
            uint64_t offset = b.Allocate(s, al);
            if (offset == BlockRanges::kNone)
            {
                refusals++;
                // Refused only when no free range could hold it.
                bool fits = false;
                for (const auto& [o, sz] : b.FreeList())
                {
                    uint64_t start = (o + al - 1) & ~(al - 1);
                    fits |= start < o + sz && o + sz - start >= s;
                }
                CHECK(!fits, "refused %llu (alignment %llu) though a free range holds it", (unsigned long long)s, (unsigned long long)al);
                continue;
            }
            allocations++;
            CHECK(offset % al == 0, "offset %llu not aligned to %llu", (unsigned long long)offset, (unsigned long long)al);
            held.push_back({ offset, s });
        }
        Validate(b, held, "after allocating");
        std::shuffle(held.begin(), held.end(), rng);
        size_t keep = held.size() * (rng() % 100) / 100;
        for (size_t i = keep; i < held.size(); i++)
            CHECK(b.Free(held[i].offset, held[i].size), "free %llu+%llu", (unsigned long long)held[i].offset, (unsigned long long)held[i].size);
        held.resize(keep);
        Validate(b, held, "after freeing");
        if (g_failures)
            return;
    }
    for (const Range& r : held)
        CHECK(b.Free(r.offset, r.size), "final free");
    held.clear();
    Validate(b, held, "all freed");
    CHECK(b.Empty() && b.FreeRanges() == 1, "one free range at the end (%zu)", b.FreeRanges());
    printf("  %llu allocations, %llu refused\n", (unsigned long long)allocations, (unsigned long long)refusals);
}

int main()
{
    Basics();
    for (uint32_t seed : { 1u, 2u, 3u, 42u, 1234u })
        Random(seed);
    printf(g_failures ? "%d FAILED\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
