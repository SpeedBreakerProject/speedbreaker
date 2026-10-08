// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// xboxkrnl memory exports. Semantics follow Xenia's xboxkrnl_memory.cc.
#include "../stdafx.h"
#include "function.h"
#include "status.h"
#include "vmem.h"

using namespace vmem;

namespace
{
    constexpr uint32_t PAGE_EXECUTE_ANY = 0x10 | 0x20 | 0x40 | 0x80;

    PageHeap& VirtualHeapFor(uint32_t type)
    {
        return (type & X_MEM_LARGE_PAGES) ? Virtual64K() : Virtual4K();
    }
}

uint32_t NtAllocateVirtualMemory(be<uint32_t>* baseAddress, be<uint32_t>* regionSize, uint32_t type,
    uint32_t protect, uint32_t debugMemory)
{
    if (baseAddress == nullptr || regionSize == nullptr || *regionSize == 0)
        return STATUS_INVALID_PARAMETER;
    if (!(type & (X_MEM_COMMIT | X_MEM_RESET | X_MEM_RESERVE)))
        return STATUS_INVALID_PARAMETER;
    if ((type & X_MEM_RESET) && (type & ~X_MEM_RESET))
        return STATUS_INVALID_PARAMETER;
    if (type & X_MEM_RESET)
        return STATUS_SUCCESS;  // contents may be discarded; keeping them is valid
    protect &= ~PAGE_EXECUTE_ANY;

    uint32_t requested = *baseAddress;
    // Some games pass negative sizes (Xenia).
    uint32_t size = int32_t(uint32_t(*regionSize)) < 0 ? uint32_t(-int32_t(uint32_t(*regionSize))) : uint32_t(*regionSize);

    PageHeap* heap;
    uint32_t addr = 0;
    if (requested != 0)
    {
        heap = HeapFor(requested);
        if (heap == nullptr || heap == &Physical())
            return STATUS_INVALID_PARAMETER;
        uint32_t page = heap->PageSize();
        uint32_t base = requested - requested % page;
        size = (requested + size - base + page - 1) / page * page;
        if (heap->AllocFixed(base, size, type, protect, nullptr))
            addr = base;
    }
    else
    {
        heap = &VirtualHeapFor(type);
        uint32_t page = heap->PageSize();
        size = (size + page - 1) / page * page;
        addr = heap->Alloc(size, page, type, protect, (type & X_MEM_TOP_DOWN) != 0);
    }

    if (addr == 0)
    {
        fprintf(stderr, "[vmem] NtAllocateVirtualMemory failed: base %08X size %08X type %08X\n", requested, size, type);
        return STATUS_NO_MEMORY;
    }

    *baseAddress = addr;
    *regionSize = size;
    return STATUS_SUCCESS;
}

uint32_t NtFreeVirtualMemory(be<uint32_t>* baseAddress, be<uint32_t>* regionSize, uint32_t type, uint32_t debugMemory)
{
    uint32_t addr = baseAddress ? uint32_t(*baseAddress) : 0;
    if (addr == 0)
        return STATUS_MEMORY_NOT_ALLOCATED;
    PageHeap* heap = HeapFor(addr);
    if (heap == nullptr || heap == &Physical())
        return STATUS_INVALID_PARAMETER;

    uint32_t size = regionSize ? uint32_t(*regionSize) : 0;
    bool ok;
    if (type == X_MEM_DECOMMIT)
    {
        uint32_t page = heap->PageSize();
        size = (size + page - 1) / page * page;
        ok = size != 0 && heap->Decommit(addr, size);
    }
    else
    {
        ok = heap->Release(addr, &size);
    }
    if (!ok)
        return STATUS_UNSUCCESSFUL;

    *baseAddress = addr;
    if (regionSize)
        *regionSize = size;
    return STATUS_SUCCESS;
}

struct X_MEMORY_BASIC_INFORMATION
{
    be<uint32_t> baseAddress;
    be<uint32_t> allocationBase;
    be<uint32_t> allocationProtect;
    be<uint32_t> regionSize;
    be<uint32_t> state;
    be<uint32_t> protect;
    be<uint32_t> type;
};

uint32_t NtQueryVirtualMemory(uint32_t address, X_MEMORY_BASIC_INFORMATION* info)
{
    PageHeap* heap = HeapFor(address);
    RegionInfo r;
    if (heap == nullptr || info == nullptr || !heap->Query(address, &r))
        return STATUS_INVALID_PARAMETER;
    info->baseAddress = r.baseAddress;
    info->allocationBase = r.allocationBase;
    info->allocationProtect = r.allocationProtect;
    info->regionSize = r.regionSize;
    info->state = r.state;
    info->protect = r.protect;
    info->type = X_MEM_PRIVATE;
    return STATUS_SUCCESS;
}

uint32_t NtProtectVirtualMemory(be<uint32_t>* baseAddress, be<uint32_t>* regionSize, uint32_t protect,
    be<uint32_t>* oldProtect, uint32_t debugMemory)
{
    uint32_t addr = baseAddress ? uint32_t(*baseAddress) : 0;
    PageHeap* heap = HeapFor(addr);
    if (heap == nullptr || regionSize == nullptr)
        return STATUS_INVALID_PARAMETER;
    uint32_t page = heap->PageSize();
    uint32_t base = addr - addr % page;
    uint32_t size = (addr + uint32_t(*regionSize) - base + page - 1) / page * page;
    uint32_t old = 0;
    if (!heap->SetProtect(base, size, protect & ~PAGE_EXECUTE_ANY, &old))
        return STATUS_INVALID_PARAMETER;
    *baseAddress = base;
    *regionSize = size;
    if (oldProtect)
        *oldProtect = old;
    return STATUS_SUCCESS;
}

// Physical memory: flags carry the page size (X_MEM_LARGE_PAGES / X_MEM_16MB_PAGES)
// in `protect`, as Xenia observed. min/max are physical addresses.
uint32_t MmAllocatePhysicalMemoryEx(uint32_t flags, uint32_t size, uint32_t protect, uint32_t minAddress,
    uint32_t maxAddress, uint32_t alignment)
{
    if (!(protect & (0x02 | 0x04)))  // PAGE_READONLY | PAGE_READWRITE
        return 0;
    uint32_t page = (protect & X_MEM_LARGE_PAGES) ? 0x10000 : (protect & X_MEM_16MB_PAGES) ? 0x1000000 : 0x1000;
    size = (size + page - 1) / page * page;
    alignment = std::max(page, (alignment + page - 1) / page * page);

    PageHeap& heap = Physical();
    uint32_t lo = heap.Base() + std::min(minAddress, 0x1FFFFFFFu);
    uint32_t hi = heap.Base() + std::min(maxAddress, 0x1FFFFFFFu);
    uint32_t addr = heap.Alloc(size, alignment, X_MEM_RESERVE | X_MEM_COMMIT, protect & 0xFFFF, true, lo, hi);
    if (addr == 0)
        fprintf(stderr, "[vmem] MmAllocatePhysicalMemoryEx failed: size %08X align %08X range %08X-%08X\n",
            size, alignment, minAddress, maxAddress);
    return addr;
}

uint32_t MmAllocatePhysicalMemory(uint32_t flags, uint32_t size, uint32_t protect)
{
    return MmAllocatePhysicalMemoryEx(flags, size, protect, 0, 0xFFFFFFFF, 0);
}

void MmFreePhysicalMemory(uint32_t type, uint32_t baseAddress)
{
    if (baseAddress != 0)
        Physical().Release(baseAddress, nullptr);
}

uint32_t MmGetPhysicalAddress(uint32_t address)
{
    uint32_t physical = address & 0x1FFFFFFF;
    if (address >= 0xE0000000)
        physical += g_memory.eWindowShift;
    return physical;
}

uint32_t MmQueryAddressProtect(uint32_t address)
{
    PageHeap* heap = HeapFor(address);
    if (heap != nullptr)
        return heap->Protect(address);
    // The XEX image and kernel objects are ordinary read/write memory.
    return (address >= 0x7F000000 && address < 0xA0000000) ? 0x04 : 0;
}

uint32_t MmQueryAllocationSize(uint32_t address)
{
    PageHeap* heap = HeapFor(address);
    return heap ? heap->AllocationSize(address) : 0;
}

void MmSetAddressProtect(uint32_t address, uint32_t size, uint32_t protect)
{
    if (PageHeap* heap = HeapFor(address))
        heap->SetProtect(address, size, protect & ~PAGE_EXECUTE_ANY, nullptr);
}

// Kernel pool: ordinary committed virtual memory.
uint32_t ExAllocatePoolWithTag(uint32_t size, uint32_t tag)
{
    return Virtual4K().Alloc(size, 0x1000, X_MEM_RESERVE | X_MEM_COMMIT, 0x04, false);
}

uint32_t ExAllocatePool(uint32_t size)
{
    return ExAllocatePoolWithTag(size, 0);
}

void ExFreePool(uint32_t address)
{
    if (address != 0)
        Virtual4K().Release(address, nullptr);
}

struct X_MM_QUERY_STATISTICS_SECTION
{
    be<uint32_t> availablePages;
    be<uint32_t> totalVirtualMemoryBytes;
    be<uint32_t> reservedVirtualMemoryBytes;
    be<uint32_t> physicalPages;
    be<uint32_t> poolPages;
    be<uint32_t> stackPages;
    be<uint32_t> imagePages;
    be<uint32_t> heapPages;
    be<uint32_t> virtualPages;
    be<uint32_t> pageTablePages;
    be<uint32_t> cachePages;
};

struct X_MM_QUERY_STATISTICS_RESULT
{
    be<uint32_t> size;
    be<uint32_t> totalPhysicalPages;
    be<uint32_t> kernelPages;
    X_MM_QUERY_STATISTICS_SECTION title;
    X_MM_QUERY_STATISTICS_SECTION system;
    be<uint32_t> highestPhysicalPage;
};
static_assert(sizeof(X_MM_QUERY_STATISTICS_RESULT) == 104);

// Constants follow Xenia (mostly guesses there too); available pages are real.
uint32_t MmQueryStatistics(X_MM_QUERY_STATISTICS_RESULT* stats)
{
    if (stats == nullptr)
        return STATUS_INVALID_PARAMETER;
    if (stats->size != sizeof(X_MM_QUERY_STATISTICS_RESULT))
        return 0xC0000023; // STATUS_BUFFER_TOO_SMALL

    memset(stats, 0, sizeof(*stats));
    stats->size = sizeof(X_MM_QUERY_STATISTICS_RESULT);
    stats->totalPhysicalPages = 0x00020000;  // 512 MB / 4 KB
    stats->kernelPages = 0x00000300;
    uint32_t used = Physical().UsedPages();  // 4 KB pages
    stats->title.availablePages = 0x00020000 - std::min<uint32_t>(used, 0x0001FFFF);
    stats->title.totalVirtualMemoryBytes = 0x2FFF0000;
    stats->title.reservedVirtualMemoryBytes = 0x00160000;
    stats->title.physicalPages = 0x00001000;
    stats->title.poolPages = 0x00000010;
    stats->title.stackPages = 0x00000100;
    stats->title.imagePages = 0x00000100;
    stats->title.heapPages = 0x00000100;
    stats->title.virtualPages = 0x00000100;
    stats->title.pageTablePages = 0x00000100;
    stats->title.cachePages = 0x00000100;
    stats->highestPhysicalPage = 0x0001FFFF;
    return STATUS_SUCCESS;
}

GUEST_FUNCTION_HOOK(__imp__MmQueryStatistics, MmQueryStatistics);
GUEST_FUNCTION_HOOK(__imp__NtAllocateVirtualMemory, NtAllocateVirtualMemory);
GUEST_FUNCTION_HOOK(__imp__NtFreeVirtualMemory, NtFreeVirtualMemory);
GUEST_FUNCTION_HOOK(__imp__NtQueryVirtualMemory, NtQueryVirtualMemory);
GUEST_FUNCTION_HOOK(__imp__NtProtectVirtualMemory, NtProtectVirtualMemory);
GUEST_FUNCTION_HOOK(__imp__MmAllocatePhysicalMemoryEx, MmAllocatePhysicalMemoryEx);
GUEST_FUNCTION_HOOK(__imp__MmAllocatePhysicalMemory, MmAllocatePhysicalMemory);
GUEST_FUNCTION_HOOK(__imp__MmFreePhysicalMemory, MmFreePhysicalMemory);
GUEST_FUNCTION_HOOK(__imp__MmGetPhysicalAddress, MmGetPhysicalAddress);
GUEST_FUNCTION_HOOK(__imp__MmQueryAddressProtect, MmQueryAddressProtect);
GUEST_FUNCTION_HOOK(__imp__MmQueryAllocationSize, MmQueryAllocationSize);
GUEST_FUNCTION_HOOK(__imp__MmSetAddressProtect, MmSetAddressProtect);
GUEST_FUNCTION_HOOK(__imp__ExAllocatePoolWithTag, ExAllocatePoolWithTag);
GUEST_FUNCTION_HOOK(__imp__ExAllocatePool, ExAllocatePool);
GUEST_FUNCTION_HOOK(__imp__ExFreePool, ExFreePool);
