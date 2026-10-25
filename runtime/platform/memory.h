// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The guest's 4 GB address space where the operating system needs its own
// way of making it (kernel/memory.cpp keeps the POSIX one: an anonymous 4 GB
// mapping with one shared object mapped over the physical windows).
#pragma once
#include <cstddef>
#include <cstdint>

namespace platform
{
#ifdef _WIN32
    // The 4 GB space (at 0x100000000 if it's free) as one placeholder
    // reservation, split at 0xA0000000 and 0xC0000000 into four: [0,
    // 0xA0000000) committed private memory, and the three physical windows
    // 0xA, 0xC and 0xE, each a full 512 MB view of one pagefile-backed
    // section, so they alias. A view must fill its placeholder exactly (the
    // P0 probe: offset 0x1000 or a view 4 KB short both fail), so the 0xE
    // window is unshifted, as on Apple (Memory::eWindowShift 0). Page 0 is
    // left inaccessible (null guest pointers fault). About 3 GB of commit:
    // 2.5 GB private plus the 512 MB section. nullptr when it fails (logged).
    uint8_t* MapGuestSpace();

    // Fresh zero-filled pages for [address, address + size) in the private
    // part (below 0xA0000000): decommitted, then committed again, which also
    // drops any protection. False if the decommit failed (nothing changed;
    // the caller zeroes them by hand). A commit that fails after it (the
    // system at its commit limit) is retried for about 2 s, then aborts.
    bool ResetPrivatePages(void* address, size_t size);
#endif
}
