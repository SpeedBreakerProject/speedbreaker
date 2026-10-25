// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See platform/stack.h.
#include <platform/stack.h>

#include <dbghelp.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <iterator>

// Newer SDKs name it: no guessing that a folder in the path is a symbol store.
#ifndef SYMOPT_DISABLE_SYMSRV_AUTODETECT
#define SYMOPT_DISABLE_SYMSRV_AUTODETECT 0x02000000
#endif

namespace platform::stack
{
    namespace
    {
        // How long anyone but the crash handler waits for DbgHelp (a crash
        // handler holding it never lets go).
        constexpr DWORD kWaitMs = 10000;

        std::atomic<bool> s_ready{ false };
        // DbgHelp isn't to be used: it failed to start, faulted, or a crash
        // couldn't have it.
        std::atomic<bool> s_unusable{ false };
        std::atomic<bool> s_crashing{ false };  // EnterCrash was called
        char s_state[256] = "not loaded yet";

        // `text` (a literal) as the state, copied a byte at a time: a crash
        // may call this.
        void SetState(const char* text)
        {
            size_t i = 0;
            for (; text[i] && i + 1 < sizeof(s_state); i++)
                s_state[i] = text[i];
            s_state[i] = 0;
        }

        // A fault inside DbgHelp: unusable from now on. Outside a crash the
        // state says so, for every later report's "names" line (a crash's
        // own report has printed its line already).
        void MarkFaulted()
        {
            s_unusable.store(true, std::memory_order_relaxed);
            if (!s_crashing.load(std::memory_order_relaxed))
                SetState("DbgHelp failed in an earlier stack walk: only recompiled functions are named");
        }

        // DbgHelp's lock: the owning thread, and its depth (the owner's own).
        std::atomic<DWORD> s_owner{ 0 };
        int s_depth = 0;
        const Snapshot* s_walking = nullptr;  // under the lock: Walk's copy of the stack

        bool Lock(DWORD waitMs)
        {
            const DWORD self = GetCurrentThreadId();
            if (s_owner.load(std::memory_order_acquire) == self)
            {
                s_depth++;
                return true;
            }
            const ULONGLONG deadline = GetTickCount64() + waitMs;
            while (true)
            {
                DWORD expected = 0;
                if (s_owner.compare_exchange_strong(expected, self, std::memory_order_acquire))
                {
                    s_depth = 1;
                    return true;
                }
                if (GetTickCount64() >= deadline)
                    return false;
                Sleep(1);
            }
        }

        void Unlock()
        {
            if (--s_depth == 0)
                s_owner.store(0, std::memory_order_release);
        }

        // StackWalk64's memory: the copy of the stack where it has the bytes,
        // else the process's own memory (the code's unwind tables; in a crash,
        // the stack itself). ReadProcessMemory fails, rather than faults, on
        // an address that isn't readable.
        BOOL CALLBACK ReadMemory(HANDLE, DWORD64 address, PVOID buffer, DWORD size, LPDWORD read)
        {
            if (const Snapshot* s = s_walking; s && address >= s->low && address - s->low <= s->size && size <= s->size - (address - s->low))
            {
                memcpy(buffer, s->buffer + (address - s->low), size);
                *read = size;
                return TRUE;
            }
            SIZE_T got = 0;
            BOOL ok = ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), buffer, size, &got);
            *read = DWORD(got);
            return ok;
        }

        // Unwind data from the loaded image itself: any module, whether or
        // not DbgHelp has loaded it.
        PVOID CALLBACK FunctionTable(HANDLE, DWORD64 pc)
        {
            DWORD64 base = 0;
            return RtlLookupFunctionEntry(pc, &base, nullptr);
        }

        DWORD64 CALLBACK ModuleBase(HANDLE, DWORD64 pc)
        {
            DWORD64 base = 0;
            if (RtlLookupFunctionEntry(pc, &base, nullptr))
                return base;
            PVOID header = nullptr;
            return reinterpret_cast<DWORD64>(RtlPcToFileHeader(reinterpret_cast<PVOID>(pc), &header));
        }

        // DbgHelp's calls, with a fault inside them caught (it may meet a heap
        // or a stack that the crash left broken): DbgHelp is unusable after.
        BOOL GuardedStackWalk(STACKFRAME64* frame, CONTEXT* context)
        {
            __try
            {
                return StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), GetCurrentThread(), frame, context, ReadMemory,
                    FunctionTable, ModuleBase, nullptr);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                MarkFaulted();
                return FALSE;
            }
        }

        DWORD GuardedUnDecorate(const char* name, char* out, DWORD size)
        {
            __try
            {
                return UnDecorateSymbolName(name, out, size, UNDNAME_NAME_ONLY);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                MarkFaulted();
                return 0;
            }
        }

        BOOL GuardedSymFromAddr(DWORD64 address, DWORD64* displacement, SYMBOL_INFO* symbol)
        {
            __try
            {
                return SymFromAddr(GetCurrentProcess(), address, displacement, symbol);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                MarkFaulted();
                return FALSE;
            }
        }

        // One RtlVirtualUnwind step (Unwind: over a copy of the stack). A
        // read past the copy that faults ends the walk.
        bool GuardedUnwind(DWORD64 imageBase, PRUNTIME_FUNCTION function, CONTEXT* context)
        {
            __try
            {
                void* handlerData = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context->Rip, function, context, &handlerData, &establisher, nullptr);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        size_t ImageSize(HMODULE module)
        {
            auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
            auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(module) + dos->e_lfanew);
            return nt->OptionalHeader.SizeOfImage;
        }

        // Under the lock. A module DbgHelp doesn't have yet: its PDB if one is
        // in the search path (only ours is), else its exports.
        DWORD64 LoadModule(HMODULE module)
        {
            wchar_t path[2048];
            DWORD n = GetModuleFileNameW(module, path, DWORD(std::size(path)));
            if (!module || n == 0 || n >= std::size(path))
                return 0;
            return SymLoadModuleExW(GetCurrentProcess(), nullptr, path, nullptr, reinterpret_cast<DWORD64>(module), DWORD(ImageSize(module)),
                nullptr, 0);
        }

        void Utf8(const wchar_t* wide, char* out, size_t size)
        {
            if (size == 0)
                return;
            out[0] = 0;
            if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, int(size), nullptr, nullptr) == 0)
                out[size - 1] = 0;
        }

        const wchar_t* BaseName(const wchar_t* path)
        {
            const wchar_t* base = path;
            for (const wchar_t* p = path; *p; p++)
                if (*p == L'\\' || *p == L'/')
                    base = p + 1;
            return base;
        }
    }

    void LoadSymbols(const void* const* warm, int warmCount)
    {
        if (!Lock(INFINITE))
            return;
        HANDLE process = GetCurrentProcess();
        wchar_t exe[2048];
        DWORD n = GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe)));
        wchar_t folder[2048];
        if (n == 0 || n >= std::size(exe))
            exe[0] = 0;
        wcscpy_s(folder, exe);
        if (wchar_t* slash = wcsrchr(folder, L'\\'))
            *slash = 0;
        // Nothing but the .exe's folder searched (not the PDB path the .exe
        // recorded where it was built, nor a symbol server); no dialogs, ever.
        // Public symbols' names as they are (Name undecorates C++ ones):
        // SYMOPT_UNDNAME would also drop a C name's first underscore, as if
        // it were 32-bit cdecl's (__imp___xstart read _imp___xstart).
        SymSetOptions(SYMOPT_AUTO_PUBLICS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS | SYMOPT_IGNORE_CVREC |
            SYMOPT_DISABLE_SYMSRV_AUTODETECT);
        if (!exe[0] || !SymInitializeW(process, folder, FALSE))
        {
            snprintf(s_state, sizeof(s_state), "DbgHelp didn't start (error %lu): only recompiled functions are named", GetLastError());
            s_unusable.store(true);
            s_ready.store(true, std::memory_order_release);
            Unlock();
            return;
        }
        HMODULE self = GetModuleHandleW(nullptr);
        DWORD64 base = LoadModule(self);
        IMAGEHLP_MODULEW64 info{};
        info.SizeOfStruct = sizeof(info);
        char exeName[256], pdbName[256];
        Utf8(BaseName(exe), exeName, sizeof(exeName));
        if (base && SymGetModuleInfoW64(process, base, &info) && info.SymType == SymPdb)
        {
            Utf8(BaseName(info.LoadedPdbName), pdbName, sizeof(pdbName));
            snprintf(s_state, sizeof(s_state), "%s", pdbName);
        }
        else
        {
            // The PDB the .exe asks for, by name (else the .exe's own name
            // with .pdb): missing, or another build's.
            wchar_t stem[512];
            wcscpy_s(stem, BaseName(exe));
            if (wchar_t* dot = wcsrchr(stem, L'.'))
                *dot = 0;
            wchar_t wanted[2048];
            if (info.CVData[0])
                swprintf(wanted, std::size(wanted), L"%ls\\%ls", folder, BaseName(info.CVData));
            else
                swprintf(wanted, std::size(wanted), L"%ls\\%ls.pdb", folder, stem);
            Utf8(BaseName(wanted), pdbName, sizeof(pdbName));
            if (GetFileAttributesW(wanted) == INVALID_FILE_ATTRIBUTES)
                snprintf(s_state, sizeof(s_state), "no %s next to %s: only recompiled functions are named", pdbName, exeName);
            else
                snprintf(s_state, sizeof(s_state), "%s next to %s is another build's: only recompiled functions are named", pdbName, exeName);
        }
        // The system's own frames (a thread waiting, the threads' start).
        for (const wchar_t* dll : { L"ntdll.dll", L"kernelbase.dll", L"kernel32.dll" })
            if (HMODULE module = GetModuleHandleW(dll))
                LoadModule(module);
        // The first lookups read the PDB's tables: now, not in a crash.
        for (int i = 0; i < warmCount; i++)
            if (warm && warm[i])
            {
                alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 64];
                auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
                symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
                symbol->MaxNameLen = 64;
                DWORD64 displacement = 0;
                GuardedSymFromAddr(reinterpret_cast<DWORD64>(warm[i]), &displacement, symbol);
            }
        s_ready.store(true, std::memory_order_release);
        Unlock();
    }

    bool SymbolsReady()
    {
        return s_ready.load(std::memory_order_acquire);
    }

    const char* SymbolsState()
    {
        return s_ready.load(std::memory_order_acquire) ? s_state : "DbgHelp hadn't loaded the symbols yet: only recompiled functions are named";
    }

    bool Capture(HANDLE thread, uintptr_t stackHigh, Snapshot& snapshot)
    {
        snapshot.low = 0;
        snapshot.size = 0;
        if (SuspendThread(thread) == DWORD(-1))
            return false;
        // Until ResumeThread: nothing that allocates or takes a lock (the
        // thread may hold the heap's). GetThreadContext also waits for the
        // suspension to take effect; ReadProcessMemory is a plain system
        // call that fails on memory it can't read.
        snapshot.context.ContextFlags = CONTEXT_FULL;
        const bool ok = GetThreadContext(thread, &snapshot.context) != 0;
        if (ok && snapshot.buffer && snapshot.capacity && snapshot.context.Rsp < stackHigh)
        {
            const uintptr_t sp = snapshot.context.Rsp;
            SIZE_T got = 0;
            ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(sp), snapshot.buffer,
                std::min<size_t>(stackHigh - sp, snapshot.capacity), &got);  // a part copied counts
            snapshot.low = sp;
            snapshot.size = got;
        }
        ResumeThread(thread);
        return ok;
    }

    int Walk(const CONTEXT& context, void** frames, int max, const Snapshot* snapshot)
    {
        if (max <= 0 || !s_ready.load(std::memory_order_acquire) || s_unusable.load(std::memory_order_relaxed))
            return 0;
        if (!Lock(s_crashing.load(std::memory_order_relaxed) ? 0 : kWaitMs))
            return 0;
        // One walk at a time (the lock): static, so a stack overflow's crash
        // report doesn't need 1.3 KB more of the stack it ran out of.
        static CONTEXT c;
        static STACKFRAME64 frame;
        c = context;
        frame = {};
        frame.AddrPC.Offset = c.Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrStack.Offset = c.Rsp;
        frame.AddrStack.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = c.Rbp;
        frame.AddrFrame.Mode = AddrModeFlat;
        s_walking = snapshot;
        int n = 0;
        DWORD64 lastSp = 0, lastPc = 0;
        while (n < max && !s_unusable.load(std::memory_order_relaxed) && GuardedStackWalk(&frame, &c))
        {
            const DWORD64 pc = frame.AddrPC.Offset, sp = frame.AddrStack.Offset;
            // The end, or a stack that runs backwards or in place (broken).
            if (pc == 0 || (n > 0 && (sp < lastSp || (sp == lastSp && pc == lastPc))))
                break;
            frames[n++] = reinterpret_cast<void*>(pc);
            lastSp = sp;
            lastPc = pc;
        }
        s_walking = nullptr;
        Unlock();
        return n;
    }

    int Unwind(const Snapshot& snapshot, void** frames, int max)
    {
        if (max <= 0)
            return 0;
        // The copied part of the stack at its own addresses, and where the
        // copy is. Every register that points into that part (rsp, and a
        // frame pointer) is moved into the copy for each step, and back.
        const uintptr_t low = snapshot.low, high = snapshot.low + snapshot.size;
        const uintptr_t copy = reinterpret_cast<uintptr_t>(snapshot.buffer);
        CONTEXT c = snapshot.context;
        DWORD64* const registers[] = { &c.Rax, &c.Rcx, &c.Rdx, &c.Rbx, &c.Rsp, &c.Rbp, &c.Rsi, &c.Rdi, &c.R8, &c.R9, &c.R10,
            &c.R11, &c.R12, &c.R13, &c.R14, &c.R15 };
        int n = 0;
        while (n < max && c.Rip)
        {
            frames[n++] = reinterpret_cast<void*>(c.Rip);
            const DWORD64 sp = c.Rsp;
            if (!snapshot.buffer || sp < low || sp + 8 > high)
                break;
            DWORD64 imageBase = 0;
            if (PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr))
            {
                for (DWORD64* r : registers)
                    if (*r >= low && *r < high)
                        *r = *r - low + copy;
                const bool ok = GuardedUnwind(imageBase, function, &c);
                for (DWORD64* r : registers)
                    if (*r >= copy && *r < copy + snapshot.size)
                        *r = *r - copy + low;
                if (!ok)
                    break;
            }
            else
            {
                // A leaf function (no unwind data): its return address is at rsp.
                memcpy(&c.Rip, snapshot.buffer + (sp - low), sizeof(c.Rip));
                c.Rsp = sp + 8;
            }
            // A stack that runs backwards or in place (broken), or a return
            // address read from beyond the copy.
            if (c.Rsp <= sp || c.Rsp > high)
                break;
        }
        return n;
    }

    bool Name(const void* pc, char* name, size_t size, uintptr_t& start, uint32_t& length)
    {
        if (size == 0 || !s_ready.load(std::memory_order_acquire) || s_unusable.load(std::memory_order_relaxed))
            return false;
        const bool crashing = s_crashing.load(std::memory_order_relaxed);
        if (!Lock(crashing ? 0 : kWaitMs))
            return false;
        const DWORD64 address = reinterpret_cast<DWORD64>(pc);
        if (!crashing && SymGetModuleBase64(GetCurrentProcess(), address) == 0)
        {
            HMODULE module = nullptr;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    static_cast<LPCWSTR>(pc), &module))
                LoadModule(module);
        }
        // (Static under the lock, as in Walk.)
        alignas(SYMBOL_INFO) static char storage[sizeof(SYMBOL_INFO) + 512];
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
        memset(storage, 0, sizeof(storage));
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 512;
        DWORD64 displacement = 0;
        const bool found = GuardedSymFromAddr(address, &displacement, symbol) && symbol->Name[0];
        if (found)
        {
            // A decorated C++ name (a public symbol: no debug information, as
            // the recompiled code and the static libraries have) undecorated:
            // ?sub_82441190@@YAXAEIAUPPCContext@@PEAE@Z is sub_82441190.
            // Debug information's own names come undecorated already.
            static char undecorated[512];
            const char* shown = symbol->Name;
            if (shown[0] == '?' && GuardedUnDecorate(shown, undecorated, sizeof(undecorated)))
                shown = undecorated;
            size_t n = std::min<size_t>(strnlen(shown, 511), size - 1);
            memcpy(name, shown, n);
            name[n] = 0;
            start = uintptr_t(symbol->Address);
            length = symbol->Size;
        }
        Unlock();
        return found;
    }

    bool EnterCrash(unsigned waitMs)
    {
        s_crashing.store(true);
        // Unusable before the crash: the state says why (it didn't start, or
        // failed in an earlier walk).
        if (s_unusable.load())
            return false;
        // A crash inside DbgHelp: it can't be trusted, nor its lock waited for.
        if (s_owner.load(std::memory_order_acquire) == GetCurrentThreadId() || !Lock(waitMs))
        {
            s_unusable.store(true);
            SetState("DbgHelp unavailable in this crash: only recompiled functions are named");
            return false;
        }
        // Kept until the process ends.
        return !s_unusable.load();
    }
}
