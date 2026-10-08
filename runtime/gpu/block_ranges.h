// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The free ranges of one block of device memory, for images bound at
// offsets into a few large allocations instead of an allocation each
// (gpu/renderer.cpp, image memory). Best fit over the free ranges, the
// range's start rounded up to the alignment asked for (the gap before it
// stays free); a freed range merges with free neighbours, so a block whose
// images are all gone is one free range again. Every range in a block is an
// optimally tiled image, so bufferImageGranularity never applies between
// neighbours. Vulkan-free: tests/block_ranges_test.cpp checks it alone.
#pragma once
#include <cstdint>
#include <iterator>
#include <map>

namespace gpu
{
    class BlockRanges
    {
    public:
        static constexpr uint64_t kNone = ~0ull;

        explicit BlockRanges(uint64_t size) : m_size(size)
        {
            if (size)
                m_free.emplace(0, size);
        }

        // The offset of `size` bytes at a multiple of `alignment` (a power of
        // two; 0 counts as 1), or kNone when no free range holds them. Of the
        // ranges that do, the one that leaves the least over (the lowest
        // offset among equals).
        uint64_t Allocate(uint64_t size, uint64_t alignment)
        {
            if (size == 0 || size > m_size - m_used)
                return kNone;
            if (alignment == 0)
                alignment = 1;
            auto best = m_free.end();
            uint64_t bestStart = 0, bestLeft = ~0ull;
            for (auto it = m_free.begin(); it != m_free.end(); ++it)
            {
                uint64_t end = it->first + it->second;
                uint64_t start = (it->first + alignment - 1) & ~(alignment - 1);
                if (start < it->first || start >= end || end - start < size)
                    continue;
                uint64_t left = it->second - size;
                if (left < bestLeft)
                {
                    best = it;
                    bestStart = start;
                    bestLeft = left;
                    if (left == 0)
                        break;
                }
            }
            if (best == m_free.end())
                return kNone;
            uint64_t offset = best->first, end = best->first + best->second;
            m_free.erase(best);
            if (bestStart > offset)
                m_free.emplace(offset, bestStart - offset);
            if (bestStart + size < end)
                m_free.emplace(bestStart + size, end - (bestStart + size));
            m_used += size;
            m_ranges++;
            return bestStart;
        }

        // Gives back a range Allocate returned. False (and nothing changes)
        // when it isn't one: outside the block or over a free range.
        bool Free(uint64_t offset, uint64_t size)
        {
            if (size == 0 || offset >= m_size || size > m_size - offset || size > m_used)
                return false;
            auto next = m_free.lower_bound(offset);
            if (next != m_free.end() && next->first < offset + size)
                return false;
            auto prev = next == m_free.begin() ? m_free.end() : std::prev(next);
            if (prev != m_free.end() && prev->first + prev->second > offset)
                return false;
            uint64_t start = offset, end = offset + size;
            if (prev != m_free.end() && prev->first + prev->second == start)
            {
                start = prev->first;
                m_free.erase(prev);
            }
            if (next != m_free.end() && next->first == end)
            {
                end = next->first + next->second;
                m_free.erase(next);
            }
            m_free.emplace(start, end - start);
            m_used -= size;
            m_ranges--;
            return true;
        }

        uint64_t Size() const { return m_size; }
        uint64_t Used() const { return m_used; }      // bytes in ranges handed out
        uint32_t Ranges() const { return m_ranges; }  // ranges handed out
        bool Empty() const { return m_ranges == 0; }
        size_t FreeRanges() const { return m_free.size(); }
        uint64_t LargestFree() const
        {
            uint64_t largest = 0;
            for (const auto& [offset, size] : m_free)
                largest = size > largest ? size : largest;
            return largest;
        }
        // offset -> size, merged: no two touch.
        const std::map<uint64_t, uint64_t>& FreeList() const { return m_free; }

    private:
        std::map<uint64_t, uint64_t> m_free;
        uint64_t m_size, m_used = 0;
        uint32_t m_ranges = 0;
    };
}
