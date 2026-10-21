// SpeedBreaker Windows port. GPL-3.0-or-later.
#undef _INC_WINDOWS
#include <windows.h>
#include <io.h>
#include <cstdio>
#include <memoryapi.h>
#include "win_mem.h"
#include "sys/mman.h"

long sysconf(int)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return long(si.dwPageSize);
}

int mprotect(void* addr, size_t len, int prot)
{
    DWORD p = prot == 0 ? PAGE_NOACCESS : (prot & 2) ? PAGE_READWRITE : PAGE_READONLY;
    char* cur = static_cast<char*>(addr);
    char* end = cur + len;
    while (cur < end)
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(cur, &mbi, sizeof(mbi)) == 0)
            return -1;
        char* regionEnd = static_cast<char*>(mbi.BaseAddress) + mbi.RegionSize;
        char* stop = regionEnd < end ? regionEnd : end;
        DWORD old;
        if (!VirtualProtect(cur, size_t(stop - cur), p, &old))
            return -1;
        cur = stop;
    }
    return 0;
}

namespace winmem
{
    namespace
    {
        using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
        using MapViewOfFile3Fn = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);

        constexpr size_t kPhysicalSize = 0x20000000;  // 512 MB
        constexpr ULONG kReservePlaceholder = 0x00040000;  // MEM_RESERVE_PLACEHOLDER
        constexpr ULONG kReplacePlaceholder = 0x00004000;  // MEM_REPLACE_PLACEHOLDER
        constexpr ULONG kPreservePlaceholder = 0x00000002;  // MEM_PRESERVE_PLACEHOLDER
    }

    uint8_t* CreateGuestMemory(size_t size, int* step, int* error)
    {
        HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
        auto va2 = kb ? reinterpret_cast<VirtualAlloc2Fn>(GetProcAddress(kb, "VirtualAlloc2")) : nullptr;
        auto map3 = kb ? reinterpret_cast<MapViewOfFile3Fn>(GetProcAddress(kb, "MapViewOfFile3")) : nullptr;
        if (!va2 || !map3)
        {
            *step = 1;
            *error = ERROR_PROC_NOT_FOUND;
            return nullptr;
        }
        HANDLE self = GetCurrentProcess();

        // The whole window as one placeholder, at 4 GB if free (the Linux
        // build's address too), else anywhere.
        uint8_t* base = static_cast<uint8_t*>(va2(self, reinterpret_cast<void*>(0x100000000ull), size,
            MEM_RESERVE | kReservePlaceholder, PAGE_NOACCESS, nullptr, 0));
        if (!base)
            base = static_cast<uint8_t*>(va2(self, nullptr, size, MEM_RESERVE | kReservePlaceholder, PAGE_NOACCESS, nullptr, 0));
        if (!base)
        {
            *step = 1;
            *error = int(GetLastError());
            return nullptr;
        }

        HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
            DWORD(uint64_t(kPhysicalSize) >> 32), DWORD(kPhysicalSize & 0xFFFFFFFF), nullptr);
        if (!section)
        {
            *step = 2;
            *error = int(GetLastError());
            VirtualFree(base, 0, MEM_RELEASE);
            return nullptr;
        }

        // Windows maps views at 64 KB multiples: the 0xE0000000 window can't
        // start 0x1000 into physical memory, so it starts at 0 (the same as
        // the Apple Silicon build; Memory::eWindowShift stays 0).
        struct View { uint32_t guest; size_t length; };
        const View views[] = {
            { 0xA0000000, kPhysicalSize },
            { 0xC0000000, kPhysicalSize },
            { 0xE0000000, kPhysicalSize - 0x1000 },
        };
        for (const View& v : views)
        {
            uint8_t* at = base + v.guest;
            // Cut the view's range out of the big placeholder, then map over it.
            if (!VirtualFree(at, v.length, MEM_RELEASE | kPreservePlaceholder) ||
                !map3(section, self, at, 0, v.length, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0))
            {
                *step = 2;
                *error = int(GetLastError());
                CloseHandle(section);
                return nullptr;
            }
        }
        CloseHandle(section);  // the views keep it alive

        // Everything else: private zeroed read/write memory.
        struct Gap { uint32_t start; uint64_t end; };
        const Gap gaps[] = { { 0, 0xA0000000ull }, { 0xFFFFF000u, 0x100000000ull } };
        for (const Gap& g : gaps)
        {
            if (!va2(self, base + g.start, size_t(g.end - g.start), MEM_RESERVE | MEM_COMMIT | kReplacePlaceholder,
                    PAGE_READWRITE, nullptr, 0))
            {
                *step = 1;
                *error = int(GetLastError());
                return nullptr;
            }
        }
        DWORD old;
        VirtualProtect(base, 4096, PAGE_NOACCESS, &old);
        return base;
    }

    bool ResetPages(void* host, size_t bytes)
    {
        if (!VirtualFree(host, bytes, MEM_DECOMMIT))
            return false;
        return VirtualAlloc(host, bytes, MEM_COMMIT, PAGE_READWRITE) != nullptr;
    }
}

ssize_t pread(int fd, void* buf, size_t count, long long offset)
{
    HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    OVERLAPPED ov{};
    ov.Offset = DWORD(offset & 0xFFFFFFFF);
    ov.OffsetHigh = DWORD(uint64_t(offset) >> 32);
    DWORD got = 0;
    if (!ReadFile(h, buf, DWORD(count), &got, &ov))
    {
        if (GetLastError() == ERROR_HANDLE_EOF)
            return 0;
        return -1;
    }
    return ssize_t(got);
}

ssize_t pwrite(int fd, const void* buf, size_t count, long long offset)
{
    HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    OVERLAPPED ov{};
    ov.Offset = DWORD(offset & 0xFFFFFFFF);
    ov.OffsetHigh = DWORD(uint64_t(offset) >> 32);
    DWORD put = 0;
    if (!WriteFile(h, buf, DWORD(count), &put, &ov))
        return -1;
    return ssize_t(put);
}

namespace winseh
{
    namespace
    {
        FaultFn s_fn = nullptr;

        LONG CALLBACK Veh(EXCEPTION_POINTERS* ep)
        {
            const EXCEPTION_RECORD* r = ep->ExceptionRecord;
            if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2 && s_fn)
            {
                bool write = r->ExceptionInformation[0] == 1;
                const void* addr = reinterpret_cast<const void*>(r->ExceptionInformation[1]);
                if (s_fn(addr, write, reinterpret_cast<const void*>(ep->ContextRecord->Rip)))
                    return EXCEPTION_CONTINUE_EXECUTION;
            }
            return EXCEPTION_CONTINUE_SEARCH;
        }

        LONG WINAPI Unhandled(EXCEPTION_POINTERS* ep)
        {
            const EXCEPTION_RECORD* r = ep->ExceptionRecord;
            char buf[4096];
            uintptr_t imageBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(imageBase);
            auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(imageBase + dos->e_lfanew);
            uintptr_t imageEnd = imageBase + nt->OptionalHeader.SizeOfImage;
            uintptr_t rip = ep->ContextRecord->Rip;
            uintptr_t rsp = ep->ContextRecord->Rsp;
            int n = snprintf(buf, sizeof(buf),
                "[crash] exception 0x%08lX at rip=%p, address=%p (%s) rsp=%p\n[crash] image base=%p size=0x%lX, rip RVA=0x%llX\n",
                r->ExceptionCode, reinterpret_cast<void*>(rip),
                r->NumberParameters >= 2 ? reinterpret_cast<void*>(r->ExceptionInformation[1]) : nullptr,
                r->NumberParameters >= 1 ? (r->ExceptionInformation[0] == 1 ? "write" : r->ExceptionInformation[0] == 8 ? "execute" : "read") : "-",
                reinterpret_cast<void*>(rsp), reinterpret_cast<void*>(imageBase), nt->OptionalHeader.SizeOfImage,
                (unsigned long long)(rip - imageBase));
            // A call through a null pointer leaves its return address on top of the
            // stack: list every stack word that points into the exe, as RVAs.
            n += snprintf(buf + n, sizeof(buf) - size_t(n), "[crash] code addresses on the stack (RVA, slot):");
            const uintptr_t* sp = reinterpret_cast<const uintptr_t*>(rsp);
            int found = 0;
            for (int i = 0; i < 512 && found < 40 && n < int(sizeof(buf)) - 64; i++)
            {
                if (IsBadReadPtr(sp + i, sizeof(uintptr_t)))
                    break;
                uintptr_t v = sp[i];
                if (v >= imageBase && v < imageEnd)
                {
                    n += snprintf(buf + n, sizeof(buf) - size_t(n), " 0x%llX@%d", (unsigned long long)(v - imageBase), i);
                    found++;
                }
            }
            n += snprintf(buf + n, sizeof(buf) - size_t(n), "\n");
            fwrite(buf, 1, size_t(n), stderr);
            fflush(stderr);
            if (FILE* f = fopen("SpeedBreaker-crash.txt", "a"))
            {
                fwrite(buf, 1, size_t(n), f);
                fclose(f);
            }
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }

    void Install(FaultFn fn)
    {
        s_fn = fn;
        AddVectoredExceptionHandler(1, Veh);
        SetUnhandledExceptionFilter(Unhandled);
    }
}

// libstdc++'s std::call_once keeps its callable in these thread-locals and
// calls it through __once_proxy. The copies in libstdc++.a use emulated TLS;
// clang's code uses native TLS, so with those the proxy would see a different
// (empty) variable and call null. Defining __once_proxy here together with the
// variables keeps the library's copy out of the link entirely.
namespace std
{
    __thread void* __once_callable = nullptr;
    __thread void (*__once_call)() = nullptr;
}
extern "C" void __once_proxy(void)
{
    std::__once_call();
}
