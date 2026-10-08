// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Bump allocator for runtime-owned kernel objects (data exports, thread
// blocks). Game allocations go through kernel/vmem.
#pragma once
#include <cstddef>
#include <cstdint>

namespace arena
{
    // Kernel-owned objects live in 0x7F000000-0x7FFFFFFF, the gap Xenia also
    // leaves between the 64 KB virtual heap and the XEX image (0x82000000).
    constexpr uint32_t BASE = 0x7F000000;
    constexpr uint32_t END = 0x7FC00000;  // 0x7FC80000+ is the GPU register page (gpu/command_processor)

    // Returns a zeroed guest address aligned to `align` (a power of two).
    uint32_t Alloc(size_t size, size_t align = 16);
    void* AllocHost(size_t size, size_t align = 16);
}
