// Adapted from Unleashed Recompiled (https://github.com/hedge-dev/UnleashedRecomp),
// GPL-3.0-or-later. Modified for SpeedBreaker.
#include "../stdafx.h"
#include "memory.h"

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach.h>
#endif

bool Memory::MapPhysicalMirrors()
{
    constexpr size_t PHYSICAL_SIZE = 0x20000000;  // 512 MB
#ifdef __APPLE__
    // A named memory entry, mapped three times with vm_map. The iOS sandbox
    // refuses shm_open (EPERM); this works there and on macOS alike.
    memory_object_size_t entrySize = PHYSICAL_SIZE;
    mach_port_t entry = MACH_PORT_NULL;
    kern_return_t kr = mach_make_memory_entry_64(mach_task_self(), &entrySize, 0,
        MAP_MEM_NAMED_CREATE | VM_PROT_READ | VM_PROT_WRITE, &entry, MACH_PORT_NULL);
    if (kr != KERN_SUCCESS)
    {
        fprintf(stderr, "[memory] mach_make_memory_entry_64: %s\n", mach_error_string(kr));
        return false;
    }
    bool ok = true;
#else
    char name[64];
    snprintf(name, sizeof(name), "/speedbreaker-%d", int(getpid()));
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
    {
        perror("[memory] shm_open");
        return false;
    }
    shm_unlink(name);  // the mappings keep it alive
    bool ok = ftruncate(fd, PHYSICAL_SIZE) == 0;
#endif

    struct View { uint32_t guest; size_t offset; size_t length; };
    const View views[] = {
        { 0xA0000000, 0, PHYSICAL_SIZE },
        { 0xC0000000, 0, PHYSICAL_SIZE },
        { 0xE0000000, 0x1000, PHYSICAL_SIZE - 0x1000 },
    };
    for (const View& v : views)
    {
        if (!ok)
            break;
        void* at = base + v.guest;
        // mmap offsets must be host-page aligned. On 16 KB-page hosts (Apple
        // Silicon) the 0xE0000000 window's 4 KB shift can't be expressed, so
        // it is mapped unshifted there; MmGetPhysicalAddress uses
        // eWindowShift so addresses stay consistent. 4 KB-page hosts (x86
        // Linux, the Steam Deck) get the exact hardware layout.
        size_t offset = v.offset;
        if (offset % size_t(sysconf(_SC_PAGESIZE)) != 0)
            offset = 0;
        if (v.guest == 0xE0000000)
            eWindowShift = uint32_t(offset);
#ifdef __APPLE__
        vm_address_t address = vm_address_t(at);
        kr = vm_map(mach_task_self(), &address, v.length, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, entry,
            vm_offset_t(offset), FALSE, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE);
        if (kr != KERN_SUCCESS)
        {
            fprintf(stderr, "[memory] vm_map physical view: %s\n", mach_error_string(kr));
            ok = false;
        }
#else
        if (mmap(at, v.length, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, off_t(offset)) == MAP_FAILED)
        {
            perror("[memory] mmap physical view");
            ok = false;
        }
#endif
    }
#ifdef __APPLE__
    mach_port_deallocate(mach_task_self(), entry);  // the mappings keep it alive
#else
    close(fd);
#endif
    return ok;
}
#endif

Memory::Memory()
{
#ifdef _WIN32
    base = (uint8_t*)VirtualAlloc((void*)0x100000000ull, PPC_MEMORY_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    if (base == nullptr)
        base = (uint8_t*)VirtualAlloc(nullptr, PPC_MEMORY_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    if (base == nullptr)
        return;

    DWORD oldProtect;
    VirtualProtect(base, 4096, PAGE_NOACCESS, &oldProtect);
#else
    base = (uint8_t*)mmap((void*)0x100000000ull, PPC_MEMORY_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);

    if (base == (uint8_t*)MAP_FAILED)
        base = (uint8_t*)mmap(NULL, PPC_MEMORY_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);

    if (base == (uint8_t*)MAP_FAILED)
    {
        base = nullptr;
        return;
    }

    mprotect(base, 4096, PROT_NONE);

    // SpeedBreaker: the 360 exposes its 512 MB of physical memory through three
    // windows that alias each other: 0xA0000000 and 0xC0000000 map physical 0,
    // 0xE0000000 maps physical +0x1000 (Xenia's map_info). D3D writes records
    // through one window and reads them through another, so all three must be
    // the same host memory: one shared object, mapped three times.
    if (!MapPhysicalMirrors())
    {
        munmap(base, PPC_MEMORY_SIZE);
        base = nullptr;
        return;
    }
#endif

    for (size_t i = 0; PPCFuncMappings[i].guest != 0; i++)
    {
        if (PPCFuncMappings[i].host != nullptr)
            InsertFunction(PPCFuncMappings[i].guest, PPCFuncMappings[i].host);
    }
}

void* MmGetHostAddress(uint32_t ptr)
{
    return g_memory.Translate(ptr);
}
