// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
#include "../stdafx.h"
#include "vmem.h"

namespace vmem
{
    PageHeap::PageHeap(uint32_t base, uint32_t size, uint32_t pageSize)
        : m_base(base), m_size(size), m_pageSize(pageSize), m_pages(size / pageSize)
    {
    }

    // Returns pages to fresh zero-filled memory. Remapping is lazy and cheap
    // even for hundreds of megabytes, unlike memset.
    void PageHeap::ResetHostPages(uint32_t firstPage, uint32_t count)
    {
        void* host = g_memory.Translate(m_base + firstPage * m_pageSize);
        size_t bytes = size_t(count) * m_pageSize;
        // Physical memory is a shared object mirrored at three windows:
        // remapping would break the aliasing, so zero it in place.
        if (m_base >= 0xA0000000 ||
            mmap(host, bytes, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_ANON | MAP_PRIVATE, -1, 0) == MAP_FAILED)
            memset(host, 0, bytes);
    }

    uint32_t PageHeap::Alloc(uint32_t size, uint32_t alignment, uint32_t type, uint32_t protect, bool topDown,
        uint32_t lo, uint32_t hi)
    {
        if (size == 0)
            return 0;
        std::lock_guard lock(m_mutex);

        uint32_t pages = (size + m_pageSize - 1) / m_pageSize;
        uint32_t alignPages = std::max<uint32_t>(1, (alignment + m_pageSize - 1) / m_pageSize);
        uint32_t first = lo > m_base ? (std::min(lo, End()) - m_base + m_pageSize - 1) / m_pageSize : 0;
        uint32_t last = hi != 0 && hi < End() ? (hi - m_base) / m_pageSize + 1 : uint32_t(m_pages.size());
        if (last < first + pages)
            return 0;

        auto fits = [&](uint32_t start) {
            for (uint32_t i = start; i < start + pages; i++)
                if (m_pages[i].state != FREE)
                    return false;
            return true;
        };

        int64_t found = -1;
        if (topDown)
        {
            int64_t start = int64_t(last - pages) / alignPages * alignPages;
            for (; start >= int64_t(first); start -= alignPages)
                if (fits(uint32_t(start))) { found = start; break; }
        }
        else
        {
            uint32_t start = (first + alignPages - 1) / alignPages * alignPages;
            for (; start + pages <= last; start += alignPages)
                if (fits(start)) { found = start; break; }
        }
        if (found < 0)
            return 0;

        uint32_t start = uint32_t(found);
        uint32_t addr = m_base + start * m_pageSize;
        uint8_t state = (type & X_MEM_COMMIT) ? COMMITTED : RESERVED;
        for (uint32_t i = start; i < start + pages; i++)
            m_pages[i] = { state, protect, addr, 0, protect };
        m_pages[start].allocPages = pages;
        return addr;
    }

    bool PageHeap::AllocFixed(uint32_t addr, uint32_t size, uint32_t type, uint32_t protect, bool* wasCommitted)
    {
        std::lock_guard lock(m_mutex);
        uint32_t start = PageIndex(addr);
        uint32_t end = PageIndex(addr + size - 1) + 1;
        if (end > m_pages.size())
            return false;

        bool anyFree = false, anyUsed = false, allCommitted = true;
        for (uint32_t i = start; i < end; i++)
        {
            anyFree |= m_pages[i].state == FREE;
            anyUsed |= m_pages[i].state != FREE;
            allCommitted &= m_pages[i].state == COMMITTED;
        }
        if (wasCommitted)
            *wasCommitted = allCommitted;

        if (anyFree && anyUsed)
            return false;  // straddles an existing allocation

        if (anyFree)
        {
            // New allocation at a fixed address.
            uint8_t state = (type & X_MEM_COMMIT) ? COMMITTED : RESERVED;
            uint32_t base = m_base + start * m_pageSize;
            for (uint32_t i = start; i < end; i++)
                m_pages[i] = { state, protect, base, 0, protect };
            m_pages[start].allocPages = end - start;
            return true;
        }

        // Committing (part of) an existing reservation.
        if (!(type & X_MEM_COMMIT))
            return true;
        for (uint32_t i = start; i < end; i++)
        {
            m_pages[i].state = COMMITTED;
            m_pages[i].protect = protect;
        }
        return true;
    }

    bool PageHeap::Decommit(uint32_t addr, uint32_t size)
    {
        std::lock_guard lock(m_mutex);
        uint32_t start = PageIndex(addr);
        uint32_t end = PageIndex(addr + size - 1) + 1;
        if (end > m_pages.size())
            return false;
        for (uint32_t i = start; i < end; i++)
            if (m_pages[i].state == FREE)
                return false;
        for (uint32_t i = start; i < end; i++)
            m_pages[i].state = RESERVED;
        ResetHostPages(start, end - start);
        return true;
    }

    bool PageHeap::Release(uint32_t addr, uint32_t* releasedSize)
    {
        std::lock_guard lock(m_mutex);
        uint32_t start = PageIndex(addr);
        if (start >= m_pages.size() || m_pages[start].state == FREE || m_pages[start].allocBase != addr)
            return false;
        uint32_t pages = m_pages[start].allocPages;
        for (uint32_t i = start; i < start + pages; i++)
            m_pages[i] = {};
        ResetHostPages(start, pages);
        if (releasedSize)
            *releasedSize = pages * m_pageSize;
        return true;
    }

    bool PageHeap::Query(uint32_t addr, RegionInfo* info)
    {
        std::lock_guard lock(m_mutex);
        uint32_t start = PageIndex(addr);
        if (start >= m_pages.size())
            return false;
        const Page& p = m_pages[start];
        uint32_t end = start + 1;
        while (end < m_pages.size() && m_pages[end].state == p.state && m_pages[end].protect == p.protect
            && m_pages[end].allocBase == p.allocBase)
            end++;
        info->baseAddress = m_base + start * m_pageSize;
        info->allocationBase = p.allocBase;
        info->allocationProtect = p.allocProtect;
        info->regionSize = (end - start) * m_pageSize;
        info->state = p.state == COMMITTED ? X_MEM_COMMIT : p.state == RESERVED ? X_MEM_RESERVE : X_MEM_FREE;
        info->protect = p.state == COMMITTED ? p.protect : 0;
        return true;
    }

    uint32_t PageHeap::AllocationSize(uint32_t addr)
    {
        std::lock_guard lock(m_mutex);
        uint32_t i = PageIndex(addr);
        if (i >= m_pages.size() || m_pages[i].state == FREE)
            return 0;
        return m_pages[PageIndex(m_pages[i].allocBase)].allocPages * m_pageSize;
    }

    uint32_t PageHeap::Protect(uint32_t addr)
    {
        std::lock_guard lock(m_mutex);
        uint32_t i = PageIndex(addr);
        return i < m_pages.size() && m_pages[i].state == COMMITTED ? m_pages[i].protect : 0;
    }

    bool PageHeap::SetProtect(uint32_t addr, uint32_t size, uint32_t protect, uint32_t* oldProtect)
    {
        std::lock_guard lock(m_mutex);
        uint32_t start = PageIndex(addr);
        uint32_t end = PageIndex(addr + std::max<uint32_t>(size, 1) - 1) + 1;
        if (end > m_pages.size())
            return false;
        for (uint32_t i = start; i < end; i++)
            if (m_pages[i].state != COMMITTED)
                return false;
        if (oldProtect)
            *oldProtect = m_pages[start].protect;
        for (uint32_t i = start; i < end; i++)
            m_pages[i].protect = protect;
        return true;
    }

    uint32_t PageHeap::UsedPages()
    {
        std::lock_guard lock(m_mutex);
        uint32_t n = 0;
        for (const Page& p : m_pages)
            n += p.state != FREE;
        return n;
    }

    PageHeap& Virtual4K() { static PageHeap h(0x00010000, 0x40000000 - 0x00010000, 0x1000); return h; }
    PageHeap& Virtual64K() { static PageHeap h(0x40000000, 0x3F000000, 0x10000); return h; }
    PageHeap& Physical() { static PageHeap h(0xA0000000, 0x20000000, 0x1000); return h; }

    PageHeap* HeapFor(uint32_t addr)
    {
        for (PageHeap* h : { &Virtual4K(), &Virtual64K(), &Physical() })
            if (h->Contains(addr))
                return h;
        return nullptr;
    }
}
