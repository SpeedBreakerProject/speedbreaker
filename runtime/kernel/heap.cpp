// Adapted from Unleashed Recompiled (https://github.com/hedge-dev/UnleashedRecomp),
// GPL-3.0-or-later. Modified for SpeedBreaker: the heaps' guest regions are carved out
// of kernel/vmem instead of fixed ranges, so they can't collide with the
// game's own NtAllocateVirtualMemory / MmAllocatePhysicalMemory allocations.
// Unleashed's hooks on Sonic's heap functions are dropped; NFS's own CRT heap
// runs unmodified on top of vmem.
#include <stdafx.h>
#include "heap.h"
#include "memory.h"
#include "function.h"
#include "vmem.h"

Heap g_userHeap;

// Runtime-owned allocations (thread blocks, XamAlloc, ...): virtual memory.
// Kernel objects must sit above 0x80000000 (IsKernelObject), so they come
// from the physical heap.
constexpr uint32_t USER_HEAP_SIZE = 256 * 1024 * 1024;
constexpr uint32_t PHYSICAL_HEAP_SIZE = 64 * 1024 * 1024;

void Heap::Init()
{
    using namespace vmem;
    uint32_t user = Virtual64K().Alloc(USER_HEAP_SIZE, 0x10000, X_MEM_RESERVE | X_MEM_COMMIT, 0x04, true);
    uint32_t physical = Physical().Alloc(PHYSICAL_HEAP_SIZE, 0x10000, X_MEM_RESERVE | X_MEM_COMMIT, 0x04, true);
    if (user == 0 || physical == 0)
    {
        fprintf(stderr, "[heap] could not reserve runtime heaps\n");
        std::abort();
    }
    heap = o1heapInit(g_memory.Translate(user), USER_HEAP_SIZE);
    physicalHeap = o1heapInit(g_memory.Translate(physical), PHYSICAL_HEAP_SIZE);
    fprintf(stderr, "[heap] user %08X-%08X, physical %08X-%08X\n",
        user, user + USER_HEAP_SIZE, physical, physical + PHYSICAL_HEAP_SIZE);
}

void* Heap::Alloc(size_t size)
{
    std::lock_guard lock(mutex);

    return o1heapAllocate(heap, std::max<size_t>(1, size));
}

void* Heap::AllocPhysical(size_t size, size_t alignment)
{
    size = std::max<size_t>(1, size);
    alignment = alignment == 0 ? 0x1000 : std::max<size_t>(16, alignment);

    std::lock_guard lock(physicalMutex);

    void* ptr = o1heapAllocate(physicalHeap, size + alignment);
    size_t aligned = ((size_t)ptr + alignment) & ~(alignment - 1);

    *((void**)aligned - 1) = ptr;
    *((size_t*)aligned - 2) = size + O1HEAP_ALIGNMENT;

    return (void*)aligned;
}

void Heap::Free(void* ptr)
{
    if (ptr >= physicalHeap)
    {
        std::lock_guard lock(physicalMutex);
        o1heapFree(physicalHeap, *((void**)ptr - 1));
    }
    else
    {
        std::lock_guard lock(mutex);
        o1heapFree(heap, ptr);
    }
}

size_t Heap::Size(void* ptr)
{
    if (ptr)
        return *((size_t*)ptr - 2) - O1HEAP_ALIGNMENT; // relies on fragment header in o1heap.c

    return 0;
}
