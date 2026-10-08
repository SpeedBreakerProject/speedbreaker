// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Guest virtual/physical memory, modelled on Xenia's heaps:
//   0x00010000-0x3FFFFFFF  virtual, 4 KB pages
//   0x40000000-0x7EFFFFFF  virtual, 64 KB pages (X_MEM_LARGE_PAGES)
//   0xA0000000-0xBFFFFFFF  physical, one window for every page size
// The 360 mirrors physical memory at 0xA0000000/0xC0000000/0xE0000000; we
// keep a single window (as Unleashed Recompiled does) until the GPU needs
// the others. Physical address = guest address & 0x1FFFFFFF.
#pragma once
#include <cstdint>
#include <mutex>
#include <vector>

namespace vmem
{
    enum : uint32_t
    {
        X_MEM_COMMIT = 0x00001000,
        X_MEM_RESERVE = 0x00002000,
        X_MEM_DECOMMIT = 0x00004000,
        X_MEM_RELEASE = 0x00008000,
        X_MEM_FREE = 0x00010000,
        X_MEM_PRIVATE = 0x00020000,
        X_MEM_RESET = 0x00080000,
        X_MEM_TOP_DOWN = 0x00100000,
        X_MEM_NOZERO = 0x00800000,
        X_MEM_LARGE_PAGES = 0x20000000,
        X_MEM_HEAP = 0x40000000,
        X_MEM_16MB_PAGES = 0x80000000,
    };

    struct RegionInfo
    {
        uint32_t baseAddress;       // page containing the queried address
        uint32_t allocationBase;
        uint32_t allocationProtect;
        uint32_t regionSize;        // run of pages from baseAddress with the same state
        uint32_t state;             // X_MEM_COMMIT / X_MEM_RESERVE / X_MEM_FREE
        uint32_t protect;
    };

    class PageHeap
    {
    public:
        PageHeap(uint32_t base, uint32_t size, uint32_t pageSize);

        uint32_t Base() const { return m_base; }
        uint32_t End() const { return m_base + m_size; }
        uint32_t PageSize() const { return m_pageSize; }
        bool Contains(uint32_t addr) const { return addr >= m_base && addr - m_base < m_size; }

        // Returns the base address, or 0 when nothing fits. `alignment` is
        // rounded up to the page size. [lo, hi) bounds the search.
        uint32_t Alloc(uint32_t size, uint32_t alignment, uint32_t type, uint32_t protect, bool topDown,
            uint32_t lo = 0, uint32_t hi = 0);
        // Reserves and/or commits exactly [addr, addr+size). Pages may already
        // be reserved by the same allocation (the usual reserve-then-commit).
        bool AllocFixed(uint32_t addr, uint32_t size, uint32_t type, uint32_t protect, bool* wasCommitted);
        bool Decommit(uint32_t addr, uint32_t size);
        // `addr` must be an allocation base; frees the whole allocation.
        bool Release(uint32_t addr, uint32_t* releasedSize);
        bool Query(uint32_t addr, RegionInfo* info);
        uint32_t AllocationSize(uint32_t addr);
        uint32_t Protect(uint32_t addr);
        // Records new protection for committed pages; returns the old one.
        // Not enforced on the host.
        uint32_t UsedPages();
        bool SetProtect(uint32_t addr, uint32_t size, uint32_t protect, uint32_t* oldProtect);

    private:
        enum : uint8_t { FREE = 0, RESERVED = 1, COMMITTED = 2 };
        struct Page
        {
            uint8_t state = FREE;
            uint32_t protect = 0;
            uint32_t allocBase = 0;     // guest address of the owning allocation
            uint32_t allocPages = 0;    // only meaningful on the allocation's first page
            uint32_t allocProtect = 0;
        };

        uint32_t PageIndex(uint32_t addr) const { return (addr - m_base) / m_pageSize; }
        void ResetHostPages(uint32_t firstPage, uint32_t count);

        uint32_t m_base, m_size, m_pageSize;
        std::vector<Page> m_pages;
        std::mutex m_mutex;
    };

    PageHeap* HeapFor(uint32_t addr);
    PageHeap& Virtual4K();
    PageHeap& Virtual64K();
    PageHeap& Physical();
}
