// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Occlusion query reports (EVENT_WRITE_ZPD), as this game's D3D reads them:
// the bookkeeping of gpu/renderer.cpp's counting (NFSMW_OCCLUSION=1) and the
// store order of an END report. Vulkan-free: tests/zpd_report_test.cpp
// checks it alone.
//
// D3D's Issue writes each query's reports into a 64-byte block per EDRAM
// tile: the BEGIN ZPD at block+0x20, the END ZPD at block+0x00 (the latched
// RB_SAMPLE_COUNT_ADDR). A report is 8 dwords, Total, ZFail, ZPass and
// StencilFail, each an A and a B lane, read little-endian (lwbrx). Issue
// BEGIN stores 0xFFFFFEED (big-endian) in the A lanes (+0/+8/+16/+24) of the
// LAST tile's END report; GetData is pending while all four still hold it,
// then returns the sum over tiles of END.ZPass(A+B) - BEGIN.ZPass(A+B). The
// game takes a count below 2^24 only. Every BEGIN report is all zeros, so an
// END report holds the samples of its own bracket: valid whenever it lands,
// even after the next BEGIN (a running counter would then go negative).
#pragma once
#include <array>
#include <atomic>
#include <cstdint>

namespace gpu::zpd
{
    enum class Kind { Begin, End, Unknown };

    // By the report's offset in its block.
    inline Kind Classify(uint32_t address)
    {
        switch (address & 0x3F)
        {
        case 0x20: return Kind::Begin;
        case 0x00: return Kind::End;
        default: return Kind::Unknown;
        }
    }

    // Guest samples for `raw` host samples: `num` guest samples per host
    // pixel (the guest surface's MSAA; the host renders 1 sample), over
    // `den` host pixels per guest pixel (the render scale, sx * sy). Rounded
    // to nearest, at least 1 when anything passed (as Xenia does).
    inline uint64_t Normalize(uint64_t raw, uint32_t num, uint32_t den)
    {
        if (raw == 0)
            return 0;
        if (den == 0)
            den = 1;
        uint64_t guest = (raw * num + den / 2) / den;
        return guest ? guest : 1;
    }

    // An END's count from its segments' sum. A runaway one is rejected by
    // the game, which keeps its value (as from the console), so it is
    // saturated at 2^24, never cut to 32 bits: GetData keeps the low 32 bits
    // of its sum over tiles, which no sum of up to 255 saturated tiles wraps
    // into a count the game would take.
    constexpr uint64_t kRunaway = 1u << 24;
    inline uint32_t EndCount(uint64_t sum)
    {
        return uint32_t(sum < kRunaway ? sum : kRunaway);
    }

    // An END report of `count` samples, in host order (the guest reads it
    // byte-swapped: no swap here). Total_A = ZPass_A = count, the rest 0.
    inline std::array<uint32_t, 8> ComposeEnd(uint32_t count)
    {
        return { count, 0, 0, 0, count, 0, 0, 0 };
    }

    // The order of its stores: the B lanes (no marker there), then ZPass_A,
    // which makes the report ready with its count in place, then the other
    // A lanes; a release fence between the three. GetData's loads are
    // unordered (no barrier in it), so a read racing them sees the marker in
    // all four A lanes (pending), the marker in a lane it sums (0xEDFEFFFF
    // read little-endian: >= 2^24, which the game rejects) or real counts:
    // every B lane and BEGIN report is 0, so each tile adds its ZPass_A, this
    // END's or (a stale load) the one before. Untiled only tile 0 counts:
    // this END's count or the last. With NFSMW_TILING=1 one sum can mix
    // tiles of adjacent frames (also when a late END lands after the next
    // BEGIN's marker and makes that query ready). Indices into the 8
    // dwords; phases of 4, 1 and 3.
    constexpr uint8_t kEndOrder[8] = { 1, 3, 5, 7, 4, 0, 2, 6 };
    constexpr uint8_t kEndPhaseEnd[3] = { 4, 5, 8 };

    // `store(offset, value)` stores one dword of the report (offset in bytes;
    // the callers open the write watch's guard once around all eight).
    template<typename Store>
    void StoreEnd(uint32_t count, Store&& store)
    {
        const std::array<uint32_t, 8> v = ComposeEnd(count);
        uint32_t i = 0;
        for (uint32_t phase = 0; phase < 3; phase++)
        {
            if (phase)
                std::atomic_thread_fence(std::memory_order_release);
            for (; i < kEndPhaseEnd[phase]; i++)
                store(uint32_t(kEndOrder[i]) * 4, v[kEndOrder[i]]);
        }
    }

    // A query bracket: BEGIN seen, END not yet. `s0` is the first counting
    // segment that can belong to it (segments are numbered in recording
    // order and never span a ZPD), `frame` when it was opened.
    struct Bracket
    {
        uint32_t beginAddress = 0;
        uint64_t s0 = 0, frame = 0;
    };

    // The open brackets, oldest first; the command processor's.
    class BracketBook
    {
    public:
        static constexpr uint32_t kMax = 8;
        enum class Opened { New, Replaced, DroppedOldest };

        // A BEGIN at `beginAddress`. One already open there (a query issued
        // again before its END) is replaced; beyond kMax the oldest is
        // dropped. `old`: the replaced or dropped bracket.
        Opened Open(uint32_t beginAddress, uint64_t s0, uint64_t frame, Bracket* old = nullptr)
        {
            Opened result = Opened::New;
            for (uint32_t i = 0; i < m_count; i++)
                if (m_open[i].beginAddress == beginAddress)
                {
                    if (old)
                        *old = m_open[i];
                    Erase(i);
                    result = Opened::Replaced;
                    break;
                }
            if (result == Opened::New && m_count == kMax)
            {
                if (old)
                    *old = m_open[0];
                Erase(0);
                result = Opened::DroppedOldest;
            }
            m_open[m_count++] = { beginAddress, s0, frame };
            return result;
        }

        // The END at `endAddress` closes the bracket its BEGIN opened 0x20
        // above it. False when none is open (its count is then 0).
        bool Close(uint32_t endAddress, Bracket* closed = nullptr)
        {
            for (uint32_t i = 0; i < m_count; i++)
                if (m_open[i].beginAddress == endAddress + 0x20)
                {
                    if (closed)
                        *closed = m_open[i];
                    Erase(i);
                    return true;
                }
            return false;
        }

        // Drops the brackets opened before `frame`: how many; `first`, the
        // oldest of them.
        uint32_t DropBefore(uint64_t frame, Bracket* first = nullptr)
        {
            uint32_t dropped = 0;
            for (uint32_t i = 0; i < m_count;)
            {
                if (m_open[i].frame >= frame)
                {
                    i++;
                    continue;
                }
                if (first && dropped == 0)
                    *first = m_open[i];
                dropped++;
                Erase(i);
            }
            return dropped;
        }

        bool Empty() const { return m_count == 0; }
        uint32_t Size() const { return m_count; }
        const Bracket& operator[](uint32_t i) const { return m_open[i]; }

    private:
        void Erase(uint32_t i)
        {
            for (; i + 1 < m_count; i++)
                m_open[i] = m_open[i + 1];
            m_count--;
        }

        Bracket m_open[kMax];
        uint32_t m_count = 0;
    };

    // Finished segments' counts by segment id, the last kSize of them (the
    // completion thread's). Each entry carries its id: a bracket summing an
    // id that was never stored, or was overwritten, counts it as lost.
    // Ids start at 1 (an empty entry's id is 0).
    class ResultRing
    {
    public:
        static constexpr uint32_t kSize = 4096;

        void Put(uint64_t id, uint64_t guest, uint64_t raw)
        {
            m_entries[id % kSize] = { id, guest, raw };
        }

        // Guest samples of segments [s0, s1); `lost` and `raw` (host
        // samples) are added to.
        uint64_t Sum(uint64_t s0, uint64_t s1, uint32_t& lost, uint64_t* raw = nullptr) const
        {
            uint64_t sum = 0;
            if (s1 > s0 && s1 - s0 > kSize)
            {
                lost += uint32_t(s1 - s0 - kSize);
                s0 = s1 - kSize;
            }
            for (uint64_t id = s0; id < s1; id++)
            {
                const Entry& e = m_entries[id % kSize];
                if (e.id != id || id == 0)
                {
                    lost++;
                    continue;
                }
                sum += e.guest;
                if (raw)
                    *raw += e.raw;
            }
            return sum;
        }

    private:
        struct Entry
        {
            uint64_t id = 0, guest = 0, raw = 0;
        };
        Entry m_entries[kSize];
    };
}
