// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
#include "../stdafx.h"
#include "arena.h"

#include <mutex>

namespace arena
{
    static std::mutex s_mutex;
    static uint32_t s_next = BASE;

    uint32_t Alloc(size_t size, size_t align)
    {
        std::lock_guard lock(s_mutex);
        uint32_t addr = (s_next + uint32_t(align - 1)) & ~uint32_t(align - 1);
        if (uint64_t(addr) + size > END)
        {
            fprintf(stderr, "[arena] out of guest memory (%zu bytes requested)\n", size);
            std::abort();
        }
        s_next = addr + uint32_t(size);
        memset(g_memory.Translate(addr), 0, size);
        return addr;
    }

    void* AllocHost(size_t size, size_t align)
    {
        return g_memory.Translate(Alloc(size, align));
    }
}
