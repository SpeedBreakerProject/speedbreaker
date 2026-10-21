// SpeedBreaker Windows port: guest address space on Windows. GPL-3.0-or-later.
#pragma once
#include <cstddef>
#include <cstdint>
namespace winmem
{
    // Reserves `size` bytes (4 GB) for the guest at 0x100000000 if possible,
    // commits it as zeroed read/write memory, makes the first page
    // inaccessible, and maps the 360's 512 MB of physical memory three times
    // (0xA0000000, 0xC0000000, 0xE0000000) over it, all views of one
    // pagefile-backed section. Returns the base, or nullptr with *error set
    // (a Windows error code) and *step (1: the reservation, 2: the mirrors).
    uint8_t* CreateGuestMemory(size_t size, int* step, int* error);
    // Returns pages to fresh zero-filled read/write memory.
    bool ResetPages(void* host, size_t bytes);
}

namespace winseh
{
    // Called on an access violation: the faulting address, whether it was a
    // write, and the faulting instruction. true: handled, retry the access.
    using FaultFn = bool (*)(const void* address, bool write, const void* pc);
    void Install(FaultFn fn);
}
