// Tests for gpu::zpd (runtime/gpu/zpd_report.h): occlusion query reports as
// the game's D3D reads them, and the bookkeeping that turns counted
// segments into END reports. From the repo root:
//   clang++ -std=c++20 -O2 -Iruntime tests/zpd_report_test.cpp -o build/zpd_report_test
//   build/zpd_report_test
#include <gpu/zpd_report.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

using namespace gpu::zpd;

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

constexpr uint32_t kMarker = 0xFFFFFEED;  // Issue BEGIN's, stored big-endian
constexpr uint32_t kRejected = 1u << 24;  // the game takes counts below this only

// Guest memory around the query's blocks (offsets stand for addresses).
using Memory = std::vector<uint8_t>;

uint32_t Le32(const Memory& m, uint32_t at)
{
    return uint32_t(m[at]) | uint32_t(m[at + 1]) << 8 | uint32_t(m[at + 2]) << 16 | uint32_t(m[at + 3]) << 24;
}
uint32_t Be32(const Memory& m, uint32_t at)
{
    return uint32_t(m[at]) << 24 | uint32_t(m[at + 1]) << 16 | uint32_t(m[at + 2]) << 8 | uint32_t(m[at + 3]);
}
void StoreHost(Memory& m, uint32_t at, uint32_t value)  // a host (little-endian) store
{
    for (int i = 0; i < 4; i++)
        m[at + i] = uint8_t(value >> (8 * i));
}
void StoreBe(Memory& m, uint32_t at, uint32_t value)  // the guest's stw
{
    for (int i = 0; i < 4; i++)
        m[at + i] = uint8_t(value >> (24 - 8 * i));
}

// D3D's GetData (sub_8258F7A0): pending while the last tile's END A lanes
// all hold the marker (`check`), else the sum over tiles of END.ZPass(A+B)
// - BEGIN.ZPass(A+B) (`sum`: the two reads may see memory at different
// times, as unordered loads may).
struct GetDataResult
{
    bool ready;
    uint32_t count;
    bool Accepted() const { return ready && count < kRejected; }
};
GetDataResult GetData(const Memory& check, const Memory& sum, const std::vector<uint32_t>& blocks)
{
    uint32_t last = blocks.back();
    bool pending = true;
    for (uint32_t lane = 0; lane < 32; lane += 8)
        pending = pending && Be32(check, last + lane) == kMarker;
    if (pending)
        return { false, 1 };
    uint32_t n = 0;
    for (uint32_t b : blocks)
        n += Le32(sum, b + 16) + Le32(sum, b + 20) - Le32(sum, b + 48) - Le32(sum, b + 52);
    return { true, n };
}

// Issue BEGIN's CPU half: the marker in the last tile's END A lanes.
void Rearm(Memory& m, uint32_t lastBlock)
{
    for (uint32_t lane = 0; lane < 32; lane += 8)
        StoreBe(m, lastBlock + lane, kMarker);
}

// The renderer's flow, without Vulkan: a ZPD ends the counting segment, a
// BEGIN opens a bracket, an END closes it (its range of segments) and is
// queued; draws while any bracket is open are counted in a segment begun
// lazily, a new one when the weight changes; at completion every segment's
// guest count goes into the ring, then each END sums its range.
struct Sim
{
    BracketBook book;
    ResultRing ring;
    uint64_t segNext = 1;
    bool segOpen = false;
    uint32_t segNum = 0, segDen = 0;
    struct Segment { uint64_t id, raw; uint32_t num, den; };
    std::vector<Segment> segments;
    struct End { uint32_t address; uint64_t s0, s1; };
    std::vector<End> ends;
    uint32_t unmatched = 0;
    uint64_t frame = 1;

    // True when the ZPD is an END (stored at completion); BEGIN stores zeros.
    bool Zpd(Memory& m, uint32_t address)
    {
        segOpen = false;
        switch (Classify(address))
        {
        case Kind::Begin:
            book.Open(address, segNext, frame);
            for (uint32_t i = 0; i < 32; i += 4)
                StoreHost(m, address + i, 0);
            return false;
        case Kind::End:
        {
            Bracket b;
            uint64_t s0 = segNext;
            if (book.Close(address, &b))
                s0 = b.s0;
            else
                unmatched++;
            ends.push_back({ address, s0, segNext });
            return true;
        }
        default:
            return false;
        }
    }
    void Draw(uint64_t raw, uint32_t num, uint32_t den)
    {
        if (book.Empty())
            return;
        if (segOpen && (num != segNum || den != segDen))
            segOpen = false;
        if (!segOpen)
        {
            segments.push_back({ segNext++, 0, num, den });
            segOpen = true;
            segNum = num;
            segDen = den;
        }
        segments.back().raw += raw;
    }
    void Harvest()
    {
        for (const Segment& s : segments)
            ring.Put(s.id, Normalize(s.raw, s.num, s.den), s.raw);
        segments.clear();
    }
    uint32_t Count(const End& e, uint32_t* lostOut = nullptr) const
    {
        uint32_t lost = 0;
        uint64_t c = ring.Sum(e.s0, e.s1, lost);
        if (lostOut)
            *lostOut = lost;
        return uint32_t(c);
    }
};

void ClassifyAndNormalize()
{
    printf("classify, normalize\n");
    CHECK(Classify(0xA2550120) == Kind::Begin, "BEGIN at +0x20");
    CHECK(Classify(0xA2550100) == Kind::End, "END at +0");
    CHECK(Classify(0xA2550140) == Kind::End, "the next tile's END");
    CHECK(Classify(0xA2550110) == Kind::Unknown && Classify(0xA2550104) == Kind::Unknown, "other offsets");

    // 4/1: the 4x scene or a cube face at render scale 1.
    CHECK(Normalize(1024, 4, 1) == 4096, "32x32 px at 4x: %llu", (unsigned long long)Normalize(1024, 4, 1));
    CHECK(Normalize(1, 4, 1) == 4, "one pixel at 4x");
    // 1/4: the 1x mirror at render scale 2 (4 host pixels per guest pixel).
    CHECK(Normalize(1024, 1, 4) == 256, "16x16 guest px at scale 2: %llu", (unsigned long long)Normalize(1024, 1, 4));
    CHECK(Normalize(1, 1, 4) == 1 && Normalize(2, 1, 4) == 1 && Normalize(3, 1, 4) == 1, "anything passed counts 1 at least");
    CHECK(Normalize(5, 1, 4) == 1 && Normalize(6, 1, 4) == 2 && Normalize(10, 1, 4) == 3, "rounded to nearest");
    // 4/4: 4x at render scale 2.
    CHECK(Normalize(777, 4, 4) == 777, "4/4 is the host count");
    CHECK(Normalize(9, 4, 9) == 4, "4x at scale 3: 9 host px = 1 guest px = 4 samples");
    CHECK(Normalize(0, 4, 1) == 0 && Normalize(0, 1, 4) == 0, "nothing passed");
    CHECK(Normalize(7, 1, 0) == 7, "den 0 counts as 1");
}

void StoreOrder()
{
    printf("store order\n");
    auto v = ComposeEnd(4096);
    const uint32_t want[8] = { 4096, 0, 0, 0, 4096, 0, 0, 0 };
    CHECK(memcmp(v.data(), want, sizeof(want)) == 0, "Total_A = ZPass_A = count, the rest 0");
    std::vector<uint32_t> offsets, values;
    StoreEnd(77, [&](uint32_t offset, uint32_t value) { offsets.push_back(offset); values.push_back(value); });
    const uint32_t order[8] = { 4, 12, 20, 28, 16, 0, 8, 24 };
    CHECK(offsets.size() == 8 && std::equal(offsets.begin(), offsets.end(), order), "B lanes, ZPass_A, the other A lanes");
    for (size_t i = 0; i < offsets.size(); i++)
        CHECK(values[i] == (offsets[i] == 0 || offsets[i] == 16 ? 77u : 0u), "offset %u holds %u", offsets[i], values[i]);
}

// A frame of the scene's query (3 tiles, untiled host rendering: tile 0
// draws the quad, tiles 1-2 skip it), with every store of the ENDs
// snapshotted. `order` replaces kEndOrder to check the check.
uint32_t ThreeTiles(bool verbose, const uint8_t* order = nullptr)
{
    const std::vector<uint32_t> blocks = { 0x100, 0x140, 0x180 };
    Memory m(0x200, 0);
    // Stale reports from the frame before: tile 0 counted 1234, and tile
    // 2's ZPass_B holds garbage (stored first, so never read with the count).
    // Synthetic: at runtime every B lane is 0 (UnorderedLoads covers that);
    // the garbage shows what storing the B lanes first buys.
    StoreHost(m, 0x100, 1234);
    StoreHost(m, 0x100 + 16, 1234);
    StoreHost(m, 0x180 + 20, 5);
    Rearm(m, blocks.back());
    Sim sim;
    // Tile 0's replay: BEGIN, the quad (32x32 px fully visible, 4x), END.
    sim.Zpd(m, 0x120);
    sim.Draw(1024, 4, 1);
    sim.Zpd(m, 0x100);
    // Tiles 1 and 2: their draws are skipped (TileShift).
    sim.Zpd(m, 0x160);
    sim.Zpd(m, 0x140);
    sim.Zpd(m, 0x1A0);
    sim.Zpd(m, 0x180);
    sim.Harvest();
    uint32_t violations = 0;
    if (verbose)
    {
        CHECK(sim.ends.size() == 3 && sim.unmatched == 0, "3 ENDs, all matched");
        CHECK(sim.Count(sim.ends[0]) == 4096, "tile 0 counts the quad: %u", sim.Count(sim.ends[0]));
        CHECK(sim.Count(sim.ends[1]) == 0 && sim.Count(sim.ends[2]) == 0, "tiles 1-2 are empty");
        CHECK(GetData(m, m, blocks).ready == false, "pending before any END lands");
    }
    // The completion thread's stores, in END order, a snapshot after each.
    std::vector<Memory> snaps = { m };
    uint32_t flipAt = 0;
    for (const Sim::End& e : sim.ends)
    {
        uint32_t c = sim.Count(e);
        if (order)
        {
            auto v = ComposeEnd(c);
            for (int i = 0; i < 8; i++)
            {
                StoreHost(m, e.address + order[i] * 4, v[order[i]]);
                snaps.push_back(m);
            }
        }
        else
            StoreEnd(c, [&](uint32_t offset, uint32_t value) {
                StoreHost(m, e.address + offset, value);
                snaps.push_back(m);
                if (e.address == blocks.back() && offset == 16)
                    flipAt = uint32_t(snaps.size() - 1);
            });
    }
    // Consistent reads: pending until the last tile's ZPass_A, then exact.
    if (verbose)
    {
        CHECK(flipAt == 2 * 8 + 5, "the last tile's ZPass_A is store %u", flipAt);
        for (uint32_t k = 0; k < snaps.size(); k++)
        {
            GetDataResult r = GetData(snaps[k], snaps[k], blocks);
            CHECK(r.ready == (k >= flipAt), "snapshot %u: ready %d", k, int(r.ready));
            CHECK(!r.ready || r.count == 4096, "snapshot %u: count %u", k, r.count);
        }
    }
    // Torn reads (the readiness check and the sum from different moments,
    // either order; each a consistent snapshot): pending, a count the game
    // rejects, or the exact count.
    for (uint32_t a = 0; a < snaps.size(); a++)
        for (uint32_t b = 0; b < snaps.size(); b++)
        {
            GetDataResult r = GetData(snaps[a], snaps[b], blocks);
            bool ok = !r.ready || r.count >= kRejected || r.count == 4096;
            violations += !ok;
            if (verbose)
                CHECK(ok, "check at %u, sum at %u: count %u", a, b, r.count);
        }
    return violations;
}

void Tiles()
{
    printf("three tiles\n");
    CHECK(ThreeTiles(true) == 0, "torn reads");
    // ZPass_A before the B lanes: the garbage in ZPass_B is read with it.
    const uint8_t early[8] = { 4, 1, 3, 5, 7, 0, 2, 6 };
    CHECK(ThreeTiles(false, early) > 0, "the check catches ZPass_A stored first");
    // Total_A before ZPass_A: ready while ZPass_A still holds the marker
    // (rejected: not a wrong count, but a lost frame the order avoids).
    const uint8_t late[8] = { 1, 3, 5, 7, 0, 4, 2, 6 };
    CHECK(ThreeTiles(false, late) == 0, "a torn read there is still rejected");
}

void Unmatched()
{
    printf("END without BEGIN\n");
    const std::vector<uint32_t> blocks = { 0x100 };
    Memory m(0x200, 0);
    Rearm(m, 0x100);
    Sim sim;
    sim.Draw(1024, 4, 1);  // no bracket open: not counted
    CHECK(sim.segments.empty(), "no segment outside a bracket");
    sim.Zpd(m, 0x100);
    sim.Harvest();
    uint32_t lost = 0;
    CHECK(sim.unmatched == 1 && sim.ends.size() == 1, "one unmatched END");
    CHECK(sim.Count(sim.ends[0], &lost) == 0 && lost == 0, "counts 0, nothing lost");
    StoreEnd(sim.Count(sim.ends[0]), [&](uint32_t offset, uint32_t value) { StoreHost(m, 0x100 + offset, value); });
    GetDataResult r = GetData(m, m, blocks);
    CHECK(r.ready && r.count == 0 && r.Accepted(), "ready with 0 (as before counting)");
}

void Overlapping()
{
    printf("overlapping brackets\n");
    Memory m(0x400, 0);
    Sim sim;
    sim.Zpd(m, 0x120);     // BEGIN A
    sim.Draw(10, 1, 1);    // segment 1: A
    sim.Zpd(m, 0x160);     // BEGIN B
    sim.Draw(20, 1, 1);    // segment 2: A and B
    sim.Draw(1, 1, 1);     // the same segment
    sim.Zpd(m, 0x100);     // END A
    sim.Draw(40, 1, 1);    // segment 3: B
    sim.Zpd(m, 0x140);     // END B
    sim.Zpd(m, 0x1A0);     // BEGIN C
    sim.Draw(5, 4, 1);     // segment 4 (4x)
    sim.Draw(5, 1, 1);     // segment 5: the weight changed
    sim.Draw(8, 1, 4);     // segment 6
    sim.Zpd(m, 0x180);     // END C
    sim.Harvest();
    CHECK(sim.ends.size() == 3 && sim.unmatched == 0, "3 ENDs, all matched");
    CHECK(sim.Count(sim.ends[0]) == 31, "A: segments 1-2: %u", sim.Count(sim.ends[0]));
    CHECK(sim.Count(sim.ends[1]) == 61, "B: segments 2-3: %u", sim.Count(sim.ends[1]));
    CHECK(sim.Count(sim.ends[2]) == 20 + 5 + 2, "C: one segment per weight: %u", sim.Count(sim.ends[2]));
    CHECK(sim.segNext == 7 && sim.book.Empty(), "6 segments, nothing open");
}

void Book()
{
    printf("bracket book\n");
    BracketBook book;
    Bracket old;
    CHECK(book.Open(0x120, 1, 10) == BracketBook::Opened::New, "new");
    CHECK(book.Open(0x120, 5, 11, &old) == BracketBook::Opened::Replaced && old.s0 == 1 && old.frame == 10, "re-issued: replaced");
    CHECK(book.Size() == 1 && book[0].s0 == 5, "the new one kept");
    for (uint32_t i = 1; i < BracketBook::kMax; i++)
        book.Open(0x120 + i * 0x40, 5 + i, 11);
    CHECK(book.Size() == BracketBook::kMax, "full");
    CHECK(book.Open(0x1000, 20, 12, &old) == BracketBook::Opened::DroppedOldest && old.beginAddress == 0x120, "the oldest dropped");
    CHECK(book.Size() == BracketBook::kMax && !book.Close(0x100), "its END finds nothing");
    Bracket closed;
    CHECK(book.Close(0x140 + 0x40, &closed) && closed.beginAddress == 0x1A0 && closed.s0 == 7, "closed by END = BEGIN - 0x20");
    CHECK(!book.Close(0x180) && !book.Close(0x1A0), "closed once; an END at the BEGIN's address closes nothing");
    Bracket first;
    CHECK(book.DropBefore(12, &first) == BracketBook::kMax - 2 && first.beginAddress == 0x160, "frame 11's dropped, oldest first");
    CHECK(book.Size() == 1 && book[0].beginAddress == 0x1000, "frame 12's kept");
    CHECK(book.DropBefore(13) == 1 && book.Empty(), "then it too");
}

void Ring()
{
    printf("result ring\n");
    ResultRing ring;
    uint32_t lost = 0;
    ring.Put(1, 100, 25);
    ring.Put(2, 50, 50);
    uint64_t raw = 0;
    CHECK(ring.Sum(1, 3, lost, &raw) == 150 && lost == 0 && raw == 75, "sum");
    CHECK(ring.Sum(1, 4, lost) == 150 && lost == 1, "an id never stored is lost");
    lost = 0;
    CHECK(ring.Sum(0, 1, lost) == 0 && lost == 1, "id 0 never counts");
    lost = 0;
    ring.Put(1 + ResultRing::kSize, 7, 7);
    CHECK(ring.Sum(1, 3, lost) == 50 && lost == 1, "an id overwritten by the wrap is lost");
    lost = 0;
    CHECK(ring.Sum(5, 5, lost) == 0 && lost == 0, "an empty range");
    lost = 0;
    ring.Sum(1, 3 + 2 * ResultRing::kSize, lost);
    CHECK(lost >= ResultRing::kSize, "a range past the ring: %u lost", lost);
}

// GetData's loads are unordered (nothing in it orders them): each returns a
// value its location held since the reader's own re-arm (Issue BEGIN, the
// same thread), the two loads of the last tile's ZPass_A (the check's, then
// the sum's) in that order. The next BEGIN's re-arm may land anywhere among
// the END's stores (a late END); to such loads only each A lane's order
// against the END's store of it matters: 16 cases. Every B lane and BEGIN
// report is 0, as at runtime. Returns the accepted counts, sorted, unique;
// `bad`: those that aren't a sum of each tile's old or new count.
std::vector<uint32_t> UnorderedLoads(const uint32_t oldCount[3], const uint32_t newCount[3], uint32_t& bad)
{
    const uint32_t blocks[3] = { 0x100, 0x140, 0x180 }, last = blocks[2];
    std::vector<uint32_t> allowed;
    for (uint32_t mix = 0; mix < 8; mix++)
        allowed.push_back((mix & 1 ? newCount[0] : oldCount[0]) + (mix & 2 ? newCount[1] : oldCount[1]) + (mix & 4 ? newCount[2] : oldCount[2]));
    std::vector<uint32_t> accepted;
    for (uint32_t late = 0; late < 16; late++)  // bit i: A lane i's marker lands after the END's store of it
    {
        Memory m(0x200, 0);
        for (int t = 0; t < 3; t++)
        {
            auto v = ComposeEnd(oldCount[t]);
            for (uint32_t i = 0; i < 8; i++)
                StoreHost(m, blocks[t] + i * 4, v[i]);
        }
        Rearm(m, last);
        // Each location's values since then, in store order (little-endian).
        std::map<uint32_t, std::vector<uint32_t>> history;
        for (uint32_t at = 0; at < 0x200; at += 4)
            history[at].push_back(Le32(m, at));
        for (int t = 0; t < 3; t++)
            StoreEnd(newCount[t], [&](uint32_t offset, uint32_t value) {
                const uint32_t at = blocks[t] + offset;
                const bool lane = blocks[t] == last && offset % 8 == 0, after = (late >> (offset / 8)) & 1;
                if (lane && !after)
                {
                    StoreBe(m, at, kMarker);
                    history[at].push_back(Le32(m, at));
                }
                StoreHost(m, at, value);
                history[at].push_back(Le32(m, at));
                if (lane && after)
                {
                    StoreBe(m, at, kMarker);
                    history[at].push_back(Le32(m, at));
                }
            });
        // GetData's loads in its order: the check's A lanes, then per tile
        // END.ZPass A and B, BEGIN.ZPass A and B. Every version of each.
        std::vector<uint32_t> loads = { last, last + 8, last + 16, last + 24 };
        for (uint32_t b : blocks)
            for (uint32_t o : { 16u, 20u, 48u, 52u })
                loads.push_back(b + o);
        std::vector<size_t> pick(loads.size(), 0);
        for (;;)
        {
            bool coherent = true;  // a later load of a location: the same version or a later one
            for (size_t k = 0; k < loads.size() && coherent; k++)
                for (size_t j = 0; j < k && coherent; j++)
                    coherent = loads[j] != loads[k] || pick[j] <= pick[k];
            if (coherent)
            {
                auto at = [&](size_t k) { return history[loads[k]][pick[k]]; };
                bool pending = true;
                for (size_t k = 0; k < 4; k++)
                    pending = pending && at(k) == __builtin_bswap32(kMarker);
                uint32_t n = 0;
                for (size_t k = 4; k < loads.size(); k += 4)
                    n += at(k) + at(k + 1) - at(k + 2) - at(k + 3);
                if (!pending && n < kRejected)
                {
                    accepted.push_back(n);
                    bad += std::find(allowed.begin(), allowed.end(), n) == allowed.end();
                }
            }
            size_t k = 0;
            for (; k < loads.size(); k++)
            {
                if (++pick[k] < history[loads[k]].size())
                    break;
                pick[k] = 0;
            }
            if (k == loads.size())
                break;
        }
    }
    std::sort(accepted.begin(), accepted.end());
    accepted.erase(std::unique(accepted.begin(), accepted.end()), accepted.end());
    return accepted;
}

void Unordered()
{
    printf("unordered loads\n");
    // Untiled (the default): tile 0 alone counts, so the old count or the new.
    uint32_t bad = 0;
    const uint32_t oldUntiled[3] = { 1234, 0, 0 }, newUntiled[3] = { 4096, 0, 0 };
    std::vector<uint32_t> seen = UnorderedLoads(oldUntiled, newUntiled, bad);
    CHECK(bad == 0 && seen == std::vector<uint32_t>({ 1234, 4096 }), "untiled: %u bad, %zu counts", bad, seen.size());
    // NFSMW_TILING=1: one sum can mix tiles 0 and 1 of either frame (tile
    // 2's old count is behind the reader's own marker).
    bad = 0;
    const uint32_t oldTiled[3] = { 1234, 77, 5 }, newTiled[3] = { 4096, 300, 9 };
    seen = UnorderedLoads(oldTiled, newTiled, bad);
    CHECK(bad == 0 && seen == std::vector<uint32_t>({ 1234 + 77 + 9, 1234 + 300 + 9, 4096 + 77 + 9, 4096 + 300 + 9 }),
        "tiled: %u bad, %zu counts", bad, seen.size());
}

void Runaway()
{
    printf("runaway count\n");
    CHECK(EndCount(0) == 0 && EndCount(4096) == 4096 && EndCount(kRejected - 1) == kRejected - 1, "counts below 2^24 kept");
    CHECK(EndCount(kRejected) == kRejected && EndCount((1ull << 32) + 5) == kRejected, "2^24 and more saturate");
    const std::vector<uint32_t> blocks = { 0x100 };
    Memory m(0x200, 0);
    Rearm(m, 0x100);
    // Saturated, not cut to 32 bits (2^32 + 5 would read 5): the game
    // rejects it and keeps its value, as on hardware.
    StoreEnd(EndCount((1ull << 32) + 5), [&](uint32_t offset, uint32_t value) { StoreHost(m, 0x100 + offset, value); });
    GetDataResult r = GetData(m, m, blocks);
    CHECK(r.ready && r.count == kRejected && !r.Accepted(), "2^32 + 5 is ready and rejected: %u", r.count);
    // GetData keeps the low 32 bits of its sum over tiles: two runaway
    // tiles cut to 32 bits would sum to an accepted 21; saturated, rejected.
    const std::vector<uint32_t> tiles = { 0x100, 0x140, 0x180 };
    const uint64_t sums[3] = { (1ull << 32) + 5, 7, (1ull << 33) + 9 };
    Memory cut(0x200, 0), saturated(0x200, 0);
    for (int t = 0; t < 3; t++)
    {
        StoreEnd(uint32_t(sums[t]), [&](uint32_t offset, uint32_t value) { StoreHost(cut, tiles[t] + offset, value); });
        StoreEnd(EndCount(sums[t]), [&](uint32_t offset, uint32_t value) { StoreHost(saturated, tiles[t] + offset, value); });
    }
    CHECK(GetData(cut, cut, tiles).Accepted() && GetData(cut, cut, tiles).count == 21, "cut: accepted 21");
    r = GetData(saturated, saturated, tiles);
    CHECK(r.ready && !r.Accepted(), "saturated: rejected (%u)", r.count);
}

int main()
{
    ClassifyAndNormalize();
    StoreOrder();
    Tiles();
    Unmatched();
    Overlapping();
    Book();
    Ring();
    Unordered();
    Runaway();
    printf(g_failures ? "%d FAILED\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
