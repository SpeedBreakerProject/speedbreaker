// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See platform/memory.h.
//
// VirtualAlloc2 and MapViewOfFile3 (Windows 10 1803 and newer) come from
// kernelbase.dll by GetProcAddress, not onecore.lib: no umbrella import
// library mixed with kernel32.lib, and older Windows fails here with a
// message instead of refusing to start. Everything here was tried on the
// target laptop first (laptop.md, P0 probe item b).
#include <platform/memory.h>
#include <platform/os.h>

#include <cstdio>

namespace
{
    constexpr uint64_t kGuestSize = 0x100000000ull;
    constexpr uint64_t kPhysicalSize = 0x20000000ull;  // 512 MB
    constexpr uint32_t kWindows[3] = { 0xA0000000u, 0xC0000000u, 0xE0000000u };

    using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
    using MapViewOfFile3Fn = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
}

namespace platform
{
    uint8_t* MapGuestSpace()
    {
        HMODULE kernelBase = GetModuleHandleW(L"kernelbase.dll");
        auto virtualAlloc2 = kernelBase ? reinterpret_cast<VirtualAlloc2Fn>(GetProcAddress(kernelBase, "VirtualAlloc2")) : nullptr;
        auto mapViewOfFile3 = kernelBase ? reinterpret_cast<MapViewOfFile3Fn>(GetProcAddress(kernelBase, "MapViewOfFile3")) : nullptr;
        if (!virtualAlloc2 || !mapViewOfFile3)
        {
            fprintf(stderr, "[memory] VirtualAlloc2 and MapViewOfFile3 are missing: Windows 10 (1803) or newer is needed\n");
            return nullptr;
        }
        HANDLE process = GetCurrentProcess();

        // 1. The 4 GB placeholder, at the address the other platforms ask for.
        auto* base = static_cast<uint8_t*>(virtualAlloc2(process, reinterpret_cast<void*>(0x100000000ull), kGuestSize,
            MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0));
        if (!base)
            base = static_cast<uint8_t*>(virtualAlloc2(process, nullptr, kGuestSize, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                PAGE_NOACCESS, nullptr, 0));
        if (!base)
        {
            fprintf(stderr, "[memory] reserving 4 GB of address space failed: error %lu\n", GetLastError());
            return nullptr;
        }

        // 2. Split it: [0, A) [A, C) [C, E) [E, 4G). (On a failure from here
        // on the reservation is left as it is: the caller exits.)
        if (!VirtualFree(base + kWindows[0], kPhysicalSize, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) ||
            !VirtualFree(base + kWindows[1], kPhysicalSize, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
        {
            fprintf(stderr, "[memory] splitting the guest space at the physical windows failed: error %lu\n", GetLastError());
            return nullptr;
        }

        // 3. Physical memory: one 512 MB pagefile-backed section, viewed at
        // each window (the views keep it alive once its handle is closed).
        HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT, 0, DWORD(kPhysicalSize), nullptr);
        if (!section)
        {
            fprintf(stderr, "[memory] creating 512 MB of physical memory failed: error %lu\n", GetLastError());
            return nullptr;
        }
        bool viewsOk = true;
        for (uint32_t window : kWindows)
        {
            void* view = mapViewOfFile3(section, process, base + window, 0, kPhysicalSize, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
                nullptr, 0);
            if (view != base + window)
            {
                fprintf(stderr, "[memory] mapping the physical window at %08X failed: error %lu\n", window, GetLastError());
                viewsOk = false;
                break;
            }
        }
        CloseHandle(section);
        if (!viewsOk)
            return nullptr;

        // 4. The private part, [0, 0xA0000000), committed in one call.
        void* low = virtualAlloc2(process, base, kWindows[0], MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
            nullptr, 0);
        if (low != base)
        {
            // Two steps: replace the placeholder with a reservation, then commit.
            low = virtualAlloc2(process, base, kWindows[0], MEM_RESERVE | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
            if (low == base)
                low = VirtualAlloc(base, kWindows[0], MEM_COMMIT, PAGE_READWRITE);
        }
        if (low != base)
        {
            DWORD error = GetLastError();
            fprintf(stderr, "[memory] committing 2.5 GB of guest memory failed: error %lu%s\n", error,
                error == ERROR_COMMITMENT_LIMIT ? " (the page file is too small, or too much else is running)" : "");
            return nullptr;
        }
        DWORD old;
        VirtualProtect(base, 4096, PAGE_NOACCESS, &old);
        return base;
    }

    bool ResetPrivatePages(void* address, size_t size)
    {
        if (!VirtualFree(address, size, MEM_DECOMMIT))
            return false;
        // The decommit handed the pages' commit charge back. At the system's
        // commit limit another program may take it before the commit below:
        // wait for it to come back (a program ending, the page file
        // growing), for about 2 s. After that the guest memory the game
        // owns is gone, and the game can't go on.
        for (int attempt = 0; VirtualAlloc(address, size, MEM_COMMIT, PAGE_READWRITE) != address; attempt++)
        {
            DWORD error = GetLastError();
            if (attempt == 0 || attempt == 40)
                fprintf(stderr, "[memory] recommitting %zu bytes at %p failed: error %lu%s\n", size, address, error,
                    attempt == 0 ? " (trying again)" : "; giving up");
            if (attempt == 40)
                abort();
            Sleep(50);
        }
        return true;
    }
}
