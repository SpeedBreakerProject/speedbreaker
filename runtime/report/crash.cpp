// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See report.h.
//
// Crash reports. The handler (main.cpp's CrashHandler, after the write
// watch declined the fault) runs on the crashing thread in a signal handler,
// so everything here that it calls is async-signal-safe: the report file is
// opened at startup (logs/.crash-pending-<session>, empty unless the session
// crashes: crash_marker.h, removed when the session ends cleanly) and renamed
// to crash-<session>.txt when written; its header, the
// system summary and the symbol table are ready beforehand; text goes out
// with write(2). The one call outside POSIX's list is dladdr (and the
// backtrace() it follows), which glibc and macOS implement without
// allocating; backtrace() is called once at startup so glibc has loaded
// libgcc's unwinder before it is needed.
//
// Names for code addresses: macOS's dladdr reads the executable's whole
// symbol table. On Linux dladdr sees only exported symbols, which an
// executable has none of, so a thread reads the ELF .symtab of
// /proc/self/exe at startup (functions with sizes and names, ~5 MB) and the
// handler looks addresses up there. A stripped build falls back to
// PPCFuncMappings, which names the recompiled functions.
//
// Windows: the unhandled-exception filter (platform/win32/fault.cpp) calls
// the handler on the crashing thread, not in a signal handler, but the same
// care holds: nothing it calls allocates or takes the C runtime's locks
// (WriteFile on handles opened at startup, a buffer of its own). The stack
// is walked with StackWalk64 from the exception's registers and named from
// SpeedBreaker.pdb by DbgHelp (platform/stack.h), loaded at startup on a
// thread of its own; DbgHelp is the one exception, as dladdr is above (its
// own heap, its faults caught). Without the PDB, or if DbgHelp is busy or
// the crash broke it, PPCFuncMappings names the recompiled functions and the
// RtlVirtualUnwind walk below gives the frames.
#include <stdafx.h>
#include <region/region.h>
#include "report.h"
#include "internal.h"
#include "crash_marker.h"

#include "symbol_names.h"

#include <gpu/command_processor.h>
#include <ui/ui.h>

#ifndef _WIN32
#include <cxxabi.h>
#include <dlfcn.h>
#include <execinfo.h>
#endif
#include <fcntl.h>
#include <fstream>
#ifdef _WIN32
#include <platform/file_io.h>
#include <platform/stack.h>
#else
#include <unistd.h>
#endif
#ifdef __linux__
#include <elf.h>
#include <link.h>
#endif

namespace report
{
    namespace
    {
        // Recompiled functions by host address (PPCFuncMappings, sorted).
        // XenonRecomp emits each function's body as __imp__sub_<address>
        // and the table points at sub_<address>, a weak wrapper (a hook can
        // replace it) that is only a tail jump to that body: the table
        // holds the jump's target, where the code a stack passes through is.
        struct GuestSymbol
        {
            uintptr_t host;
            uint32_t guest;
            bool body;  // reached through the wrapper's jump: recompiled code
        };
        GuestSymbol* s_symbols = nullptr;
        size_t s_symbolCount = 0;
        // The recompiled bodies are one run of functions in .text (the
        // library is linked after the runtime's objects). Entries that
        // aren't bodies are hooks, runtime functions standing in for guest
        // ones, whose sizes aren't known: they only name addresses just
        // after their start.
        uintptr_t s_regionBegin = 0, s_regionEnd = 0;
        constexpr uintptr_t kHookSpan = 2048;

#ifdef __linux__
        // The executable's functions from its .symtab, sorted, once loaded.
        struct ElfFunction
        {
            uintptr_t start;
            uint32_t size;
            uint32_t name;  // offset in s_elfNames
        };
        const ElfFunction* s_elfFunctions = nullptr;
        size_t s_elfCount = 0;
        const char* s_elfNames = nullptr;
        std::atomic<bool> s_elfReady{ false };

        void LoadElfSymbols()
        {
            int fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
            if (fd < 0)
                return;
            auto readAt = [fd](void* to, size_t size, uint64_t offset) {
                return pread(fd, to, size, off_t(offset)) == ssize_t(size);
            };
            Elf64_Ehdr header;
            std::vector<Elf64_Shdr> sections;
            std::vector<Elf64_Sym> symbols;
            char* names = nullptr;
            bool ok = readAt(&header, sizeof(header), 0) && memcmp(header.e_ident, ELFMAG, SELFMAG) == 0 &&
                header.e_ident[EI_CLASS] == ELFCLASS64 && header.e_shentsize == sizeof(Elf64_Shdr) && header.e_shnum > 0;
            if (ok)
            {
                sections.resize(header.e_shnum);
                ok = readAt(sections.data(), sections.size() * sizeof(Elf64_Shdr), header.e_shoff);
            }
            const Elf64_Shdr* symtab = nullptr;
            for (const Elf64_Shdr& section : sections)
                if (ok && section.sh_type == SHT_SYMTAB && section.sh_link < sections.size())
                    symtab = &section;
            if (symtab)
            {
                const Elf64_Shdr& strtab = sections[symtab->sh_link];
                symbols.resize(symtab->sh_size / sizeof(Elf64_Sym));
                names = new char[strtab.sh_size + 1];
                names[strtab.sh_size] = 0;
                if (!readAt(symbols.data(), symbols.size() * sizeof(Elf64_Sym), symtab->sh_offset) ||
                    !readAt(names, strtab.sh_size, strtab.sh_offset))
                    symbols.clear();
                // Symbol values are link-time addresses: add the load bias
                // (a PIE's load address).
                uintptr_t bias = 0;
                dl_iterate_phdr([](dl_phdr_info* info, size_t, void* out) {
                    *static_cast<uintptr_t*>(out) = info->dlpi_addr;
                    return 1;  // the first object is the executable
                }, &bias);
                std::vector<ElfFunction> functions;
                functions.reserve(symbols.size());
                for (const Elf64_Sym& sym : symbols)
                    if (ELF64_ST_TYPE(sym.st_info) == STT_FUNC && sym.st_shndx != SHN_UNDEF && sym.st_value && sym.st_size &&
                        sym.st_name < strtab.sh_size)
                        functions.push_back({ bias + uintptr_t(sym.st_value), uint32_t(std::min<uint64_t>(sym.st_size, UINT32_MAX)),
                            sym.st_name });
                std::sort(functions.begin(), functions.end(), [](const ElfFunction& a, const ElfFunction& b) { return a.start < b.start; });
                if (!functions.empty())
                {
                    auto* table = new ElfFunction[functions.size()];
                    std::copy(functions.begin(), functions.end(), table);
                    s_elfFunctions = table;
                    s_elfCount = functions.size();
                    s_elfNames = names;
                    s_elfReady.store(true, std::memory_order_release);
                    names = nullptr;
                }
            }
            delete[] names;
            close(fd);
        }

        const char* FindElf(uintptr_t pc, uintptr_t& start)
        {
            if (!s_elfReady.load(std::memory_order_acquire))
                return nullptr;
            size_t lo = 0, hi = s_elfCount;
            while (lo < hi)
            {
                size_t mid = (lo + hi) / 2;
                if (s_elfFunctions[mid].start <= pc)
                    lo = mid + 1;
                else
                    hi = mid;
            }
            if (lo == 0)
                return nullptr;
            const ElfFunction& f = s_elfFunctions[lo - 1];
            if (pc - f.start >= f.size)
                return nullptr;
            start = f.start;
            return s_elfNames + f.name;
        }
#endif

        std::atomic<int> s_reportFd{ -1 };  // -1 once the session has ended cleanly (ClearCrashMarker)
        char s_pendingPath[1024];
        char s_finalPath[1024];
#ifdef _WIN32
        wchar_t s_pendingPathW[1024];  // the same, for MoveFileExW
        wchar_t s_finalPathW[1024];
#endif
        char s_header[2048];
        size_t s_headerSize = 0;
        struct timespec s_started{};
        std::atomic<bool> s_crashing{ false };
        thread_local bool t_reporting = false;  // this thread is writing the report

#ifdef _WIN32
        const char* ExceptionName(DWORD code)
        {
            switch (code)
            {
            case EXCEPTION_ACCESS_VIOLATION: return "EXCEPTION_ACCESS_VIOLATION (invalid memory access)";
            case EXCEPTION_IN_PAGE_ERROR: return "EXCEPTION_IN_PAGE_ERROR (a mapped file couldn't be read)";
            case EXCEPTION_ILLEGAL_INSTRUCTION: return "EXCEPTION_ILLEGAL_INSTRUCTION (illegal instruction)";
            case EXCEPTION_PRIV_INSTRUCTION: return "EXCEPTION_PRIV_INSTRUCTION (privileged instruction)";
            case EXCEPTION_INT_DIVIDE_BY_ZERO: return "EXCEPTION_INT_DIVIDE_BY_ZERO (arithmetic error)";
            case EXCEPTION_INT_OVERFLOW: return "EXCEPTION_INT_OVERFLOW (arithmetic error)";
            case EXCEPTION_STACK_OVERFLOW: return "EXCEPTION_STACK_OVERFLOW (stack overflow)";
            case EXCEPTION_BREAKPOINT: return "EXCEPTION_BREAKPOINT (trap)";
            case 0x40000015: return "abort (a failed check)";
            // Only as a raised exception: a real __fastfail (a /GS stack
            // cookie, the C runtime's own) skips every handler, this one too,
            // and goes to Windows Error Reporting.
            case 0xC0000409: return "STATUS_STACK_BUFFER_OVERRUN (fail fast)";
            case 0xE06D7363: return "an uncaught C++ exception";
            default: return "an exception";
            }
        }
#else
        const char* SignalName(int sig)
        {
            switch (sig)
            {
            case SIGSEGV: return "SIGSEGV (invalid memory access)";
            case SIGBUS: return "SIGBUS (bus error)";
            case SIGILL: return "SIGILL (illegal instruction)";
            case SIGTRAP: return "SIGTRAP (trap)";
            case SIGFPE: return "SIGFPE (arithmetic error)";
            case SIGABRT: return "SIGABRT (abort: a failed check)";
            default: return "a signal";
            }
        }
#endif

        // The target of an unconditional jump at `code`, or `code`.
        uintptr_t FollowJump(uintptr_t code)
        {
#if defined(__aarch64__)
            uint32_t insn;
            memcpy(&insn, reinterpret_cast<const void*>(code), 4);
            if ((insn & 0xFC000000u) == 0x14000000u)  // B imm26
                return code + uintptr_t(int64_t(int32_t(insn << 6) >> 6) * 4);
#elif defined(__x86_64__)
            const uint8_t* p = reinterpret_cast<const uint8_t*>(code);
            if (p[0] == 0xF3 && p[1] == 0x0F && p[2] == 0x1E && p[3] == 0xFA)  // endbr64
                p += 4;
            int32_t rel;
            if (p[0] == 0xE9)  // jmp rel32
            {
                memcpy(&rel, p + 1, 4);
                return reinterpret_cast<uintptr_t>(p + 5) + uintptr_t(int64_t(rel));
            }
            if (p[0] == 0xEB)  // jmp rel8
                return reinterpret_cast<uintptr_t>(p + 2) + uintptr_t(int64_t(int8_t(p[1])));
#endif
            return code;
        }

        bool FindGuest(uintptr_t pc, uint32_t& guest, uintptr_t& start)
        {
            if (s_symbolCount == 0)
                return false;
            // The last entry at or below pc.
            size_t lo = 0, hi = s_symbolCount;
            while (lo < hi)
            {
                size_t mid = (lo + hi) / 2;
                if (s_symbols[mid].host <= pc)
                    lo = mid + 1;
                else
                    hi = mid;
            }
            if (lo == 0)
                return false;
            const GuestSymbol& s = s_symbols[lo - 1];
            bool inRegion = s.body && s.host >= s_regionBegin && s.host < s_regionEnd;
            if (inRegion ? pc >= s_regionEnd : pc - s.host >= kHookSpan)
                return false;
            guest = s.guest;
            start = s.host;
            return true;
        }

        const char* BaseName(const char* path)
        {
            const char* base = path;
            for (const char* p = path; p && *p; p++)
#ifdef _WIN32
                if (*p == '/' || *p == '\\')
#else
                if (*p == '/')
#endif
                    base = p + 1;
            return base;
        }

        void AppendElapsed(detail::SafeBuf& b)
        {
            struct timespec now{};
            clock_gettime(CLOCK_MONOTONIC, &now);
            int64_t ms = (int64_t(now.tv_sec) - s_started.tv_sec) * 1000 + (now.tv_nsec - s_started.tv_nsec) / 1000000;
            b.Dec(ms / 1000).Char('.').Dec((ms % 1000) / 100).Str(" s");
        }
    }

    namespace detail
    {
        void BuildSymbols(bool elf)
        {
#ifdef __linux__
            if (elf)
                std::thread(LoadElfSymbols).detach();
#else
            (void)elf;
#endif
            std::vector<GuestSymbol> symbols;
            const PPCFuncMapping* const mappings = region::FuncMappings();
            for (size_t i = 0; mappings[i].guest != 0; i++)
                if (mappings[i].host != nullptr)
                {
                    uintptr_t entry = reinterpret_cast<uintptr_t>(mappings[i].host), body = FollowJump(entry);
                    symbols.push_back({ body, uint32_t(mappings[i].guest), body != entry });
                }
            std::sort(symbols.begin(), symbols.end(), [](const GuestSymbol& a, const GuestSymbol& b) { return a.host < b.host; });
            symbols.erase(std::unique(symbols.begin(), symbols.end(), [](const GuestSymbol& a, const GuestSymbol& b) { return a.host == b.host; }),
                symbols.end());
            if (symbols.empty())
                return;
            // The bodies' span; the last one gets as much room as the largest before it.
            uintptr_t first = UINTPTR_MAX, last = 0, largest = kHookSpan, previous = 0;
            for (const GuestSymbol& symbol : symbols)
            {
                if (!symbol.body)
                    continue;
                if (previous)
                    largest = std::max(largest, symbol.host - previous);
                previous = symbol.host;
                first = std::min(first, symbol.host);
                last = std::max(last, symbol.host);
            }
            if (last)
            {
                s_regionBegin = first;
                s_regionEnd = last + largest;
            }
            s_symbols = new GuestSymbol[symbols.size()];
            std::copy(symbols.begin(), symbols.end(), s_symbols);
            s_symbolCount = symbols.size();
#ifdef _WIN32
            // DbgHelp and SpeedBreaker.pdb, as Linux's .symtab: on a thread of
            // its own. Its first lookups, a runtime function and a recompiled
            // body, read the tables a crash would need.
            static const void* warm[2] = { reinterpret_cast<const void*>(&TestCrash), reinterpret_cast<const void*>(s_regionBegin) };
            std::thread([] {
                auto started = std::chrono::steady_clock::now();
                platform::stack::LoadSymbols(warm, 2);
                // The log says whether reports will have names (a tester's
                // folder without SpeedBreaker.pdb, another build's).
                fprintf(stderr, "[crash] names for crash and hang reports: %s (%.2f s)\n", platform::stack::SymbolsState(),
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
            }).detach();
#endif
        }

        void* ContextPc(void* context)
        {
#if defined(__APPLE__) && defined(__aarch64__)
            return reinterpret_cast<void*>(static_cast<ucontext_t*>(context)->uc_mcontext->__ss.__pc);
#elif defined(__linux__) && defined(__x86_64__)
            return reinterpret_cast<void*>(static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_RIP]);
#elif defined(__linux__) && defined(__aarch64__)
            return reinterpret_cast<void*>(static_cast<ucontext_t*>(context)->uc_mcontext.pc);
#elif defined(_WIN32) && defined(_M_X64)
            return reinterpret_cast<void*>(static_cast<CONTEXT*>(context)->Rip);
#else
            (void)context;
            return nullptr;
#endif
        }

        int SignalFrames(void* const* frames, int count)
        {
#ifdef _WIN32
            // No signal frames: Windows' stacks come from the exception's
            // own registers (WalkStack).
            (void)frames;
            (void)count;
            return 0;
#else
            for (int i = 0; i < count && i < 6; i++)
            {
                Dl_info info{};
                if (dladdr(frames[i], &info) && info.dli_sname &&
                    (strcmp(info.dli_sname, "_sigtramp") == 0 || strcmp(info.dli_sname, "__restore_rt") == 0 ||
                     strcmp(info.dli_sname, "__kernel_rt_sigreturn") == 0))
                    return i + 1;
#if defined(__linux__) && defined(__x86_64__)
                // glibc's __restore_rt isn't exported: know it by its code,
                // mov $15 (rt_sigreturn), %rax; syscall.
                static const uint8_t kRestoreRt[] = { 0x48, 0xC7, 0xC0, 0x0F, 0x00, 0x00, 0x00, 0x0F, 0x05 };
                if (frames[i] && memcmp(frames[i], kRestoreRt, sizeof(kRestoreRt)) == 0)
                    return i + 1;
#endif
            }
            return std::min(count, 1);
#endif
        }

        void FormatFrame(SafeBuf& out, const void* pc, bool returnAddress, bool demangle)
        {
            const uintptr_t shown = reinterpret_cast<uintptr_t>(pc);
            const uintptr_t address = returnAddress && shown ? shown - 1 : shown;
            out.Str("0x").Hex(shown, 12).Str("  ");
#ifdef _WIN32
            // SpeedBreaker.pdb's name (undecorated: platform::stack::Name), the
            // recompiled ones as sub_<address>; else the generated table's.
            // A public symbol, which has no size, that starts before the
            // recompiled function the table puts there is another function.
            (void)demangle;
            uint32_t guest, length = 0;
            uintptr_t start = 0, guestStart = 0;
            char name[256], guestName[13];
            const bool isGuest = FindGuest(address, guest, guestStart);
            const char* recompiled = nullptr;
            bool named = platform::stack::Name(reinterpret_cast<const void*>(address), name, sizeof(name), start, length);
            if (named)
            {
                recompiled = detail::GuestName(name, guestName);
                if (!recompiled && isGuest && length == 0 && guestStart > start)
                    named = false;
            }
            if (named)
                out.Str(recompiled ? recompiled : name).Str("+0x").Hex(shown - start);
            else if (isGuest)
                out.Str("sub_").Hex(guest, 8, true).Str("+0x").Hex(shown - guestStart);
            else
                out.Str("?");
            HMODULE module = nullptr;
            char file[MAX_PATH];
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCSTR>(address), &module) && GetModuleFileNameA(module, file, sizeof(file)))
                out.Str("  (").Str(BaseName(file)).Str("+0x").Hex(shown - reinterpret_cast<uintptr_t>(module)).Char(')');
#else
            Dl_info info{};
            bool found = dladdr(reinterpret_cast<const void*>(address), &info) != 0;
            uint32_t guest;
            uintptr_t start;
#ifdef __linux__
            // The executable's own symbol table first (exact, with sizes).
            if (const char* name = FindElf(address, start))
            {
                char* readable = nullptr;
                int status = -1;
                char guestName[13];
                if (const char* recompiled = detail::GuestName(name, guestName))
                    name = recompiled;
                else if (demangle && (readable = abi::__cxa_demangle(name, nullptr, nullptr, &status)) && status == 0)
                    name = readable;
                out.Str(name).Str("+0x").Hex(shown - start);
                free(readable);
                if (found && info.dli_fname && info.dli_fbase)
                    out.Str("  (").Str(BaseName(info.dli_fname)).Str("+0x").Hex(shown - reinterpret_cast<uintptr_t>(info.dli_fbase)).Char(')');
                return;
            }
#endif
            bool isGuest = FindGuest(address, guest, start);
            // A symbol closer than the guest function's start is another function.
            if (isGuest && found && info.dli_saddr && reinterpret_cast<uintptr_t>(info.dli_saddr) > start)
                isGuest = false;
            if (isGuest)
                out.Str("sub_").Hex(guest, 8, true).Str("+0x").Hex(shown - start);
            else if (found && info.dli_sname && info.dli_saddr)
            {
                char* readable = nullptr;
                int status = -1;
                char guestName[13];
                const char* name = info.dli_sname;
                if (const char* recompiled = detail::GuestName(name, guestName))
                    name = recompiled;  // one the table didn't name
                else if (demangle && (readable = abi::__cxa_demangle(name, nullptr, nullptr, &status)) && status == 0)
                    name = readable;
                out.Str(name);
                free(readable);
                out.Str("+0x").Hex(shown - reinterpret_cast<uintptr_t>(info.dli_saddr));
            }
            else
                out.Str("?");
            if (found && info.dli_fname && info.dli_fbase)
                out.Str("  (").Str(BaseName(info.dli_fname)).Str("+0x").Hex(shown - reinterpret_cast<uintptr_t>(info.dli_fbase)).Char(')');
#endif
        }
    }

    void RefreshGuestSymbols()
    {
        detail::BuildSymbols(false);
    }

    void PrepareCrashReports()
    {
#ifndef _WIN32
        // Loads glibc's unwinder now: the first backtrace() dlopens it.
        void* frames[4];
        backtrace(frames, 4);
#endif
        detail::BuildSymbols();
        clock_gettime(CLOCK_MONOTONIC, &s_started);

        std::filesystem::path dir = LogDirectory();
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::string pending = (dir / (std::string(".crash-pending-") + SessionName())).string();
        std::string finalPath = (dir / (std::string("crash-") + SessionName() + ".txt")).string();
        if (pending.size() < sizeof(s_pendingPath) && finalPath.size() < sizeof(s_finalPath))
        {
            memcpy(s_pendingPath, pending.c_str(), pending.size() + 1);
            memcpy(s_finalPath, finalPath.c_str(), finalPath.size() + 1);
#ifdef _WIN32
            std::wstring pendingW = (dir / (std::string(".crash-pending-") + SessionName())).wstring();
            std::wstring finalW = (dir / (std::string("crash-") + SessionName() + ".txt")).wstring();
            if (pendingW.size() < std::size(s_pendingPathW) && finalW.size() < std::size(s_finalPathW))
            {
                wmemcpy(s_pendingPathW, pendingW.c_str(), pendingW.size() + 1);
                wmemcpy(s_finalPathW, finalW.c_str(), finalW.size() + 1);
                s_reportFd = marker::Arm(s_pendingPathW);
            }
#else
            s_reportFd = marker::Arm(s_pendingPath);
#endif
        }
        if (s_reportFd < 0)
            fprintf(stderr, "[crash] can't prepare a crash report file in %s: %s\n", dir.string().c_str(), strerror(errno));

        std::string header = std::format(
            "SpeedBreaker (NFS: Most Wanted, Xbox 360): crash report\n"
            "Build: SpeedBreaker {}\n"
            "Session: {} (log: {}.log), started {}\n",
            BuildString(), SessionName(), SessionName(), detail::TimeStamp());
        s_headerSize = std::min(header.size(), sizeof(s_header));
        memcpy(s_header, header.data(), s_headerSize);
    }

#ifdef _WIN32
    namespace
    {
        // The stack from the exception's registers, a frame at a time
        // (RtlVirtualUnwind, from the unwind tables every x64 function has;
        // a leaf function has none, and its return address is at rsp).
        // Allocates nothing. Stops at the edge of this thread's stack.
        int WalkStack(CONTEXT context, void** frames, int max)
        {
            ULONG_PTR low = 0, high = 0;
            GetCurrentThreadStackLimits(&low, &high);
            int n = 0;
            while (n < max && context.Rip)
            {
                frames[n++] = reinterpret_cast<void*>(context.Rip);
                DWORD64 imageBase = 0;
                if (PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr))
                {
                    void* handlerData = nullptr;
                    DWORD64 establisher = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData, &establisher, nullptr);
                }
                else
                {
                    if (context.Rsp < low || context.Rsp + 8 > high)
                        break;
                    context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
                    context.Rsp += 8;
                }
                if (context.Rsp < low || context.Rsp >= high)
                    break;
            }
            return n;
        }
    }

    void WriteCrashReport(_EXCEPTION_POINTERS* info)
    {
        if (s_crashing.exchange(true))
        {
            // As on POSIX: another thread's report is being written; wait for
            // it to end the process (or, on the reporting thread, return).
            if (!t_reporting)
                Sleep(10000);
            return;
        }
        t_reporting = true;
        // What was written just before (an assert's message) reaches the tail.
        FlushLog();
        // DbgHelp for this thread from here on, if another one (its loader,
        // the watchdog naming a hang report's frames) lets go of it soon.
        const bool dbghelp = platform::stack::EnterCrash(2000);

        static char text[24 * 1024];
        detail::SafeBuf b{ text, sizeof(text) - 1 };
        const EXCEPTION_RECORD* record = info->ExceptionRecord;
        const CONTEXT* context = info->ContextRecord;
        b.Str("\n[crash] ").Str(ExceptionName(record->ExceptionCode)).Str(", exception 0x").Hex(record->ExceptionCode, 8, true);
        if ((record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || record->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
            record->NumberParameters >= 2)
        {
            auto* fault = reinterpret_cast<uint8_t*>(record->ExceptionInformation[1]);
            ULONG_PTR kind = record->ExceptionInformation[0];
            b.Str(kind == 1 ? " (a write)" : kind == 8 ? " (an execution)" : " (a read)");
            if (g_memory.IsInMemoryRange(fault))
                b.Str(" at guest address ").Hex(uint32_t(fault - g_memory.base), 8, true);
            else
                b.Str(" at host address 0x").Hex(reinterpret_cast<uintptr_t>(fault));
        }
        const char* thread = detail::CurrentThreadName();
        b.Str(" in thread ").Str(*thread ? thread : "(unnamed)");
        b.Str(", ");
        AppendElapsed(b);
        b.Str(" into the session, ").Dec(int64_t(gpu::FrameCount())).Str(" game frames\n");
        if (PPCContext* ctx = GetPPCContext())
        {
            b.Str("[crash] guest registers: lr=").Hex(uint32_t(ctx->lr), 8, true).Str(" r1=").Hex(ctx->r1.u32, 8, true)
                .Str(" r3=").Hex(ctx->r3.u32, 8, true).Str(" r4=").Hex(ctx->r4.u32, 8, true).Str(" r5=").Hex(ctx->r5.u32, 8, true)
                .Str(" r31=").Hex(ctx->r31.u32, 8, true).Char('\n');
        }
        // As Linux's line, then the rest of the integer registers.
        b.Str("[crash] host registers: rip=0x").Hex(context->Rip).Str(" rsp=0x").Hex(context->Rsp)
            .Str(" rbp=0x").Hex(context->Rbp).Char('\n');
        const struct
        {
            const char* name;
            DWORD64 value;
        } registers[] = { { "rax", context->Rax }, { "rbx", context->Rbx }, { "rcx", context->Rcx }, { "rdx", context->Rdx },
            { "rsi", context->Rsi }, { "rdi", context->Rdi }, { "r8", context->R8 }, { "r9", context->R9 }, { "r10", context->R10 },
            { "r11", context->R11 }, { "r12", context->R12 }, { "r13", context->R13 }, { "r14", context->R14 }, { "r15", context->R15 },
            { "eflags", context->EFlags } };
        for (size_t i = 0; i < std::size(registers); i++)
        {
            b.Str(i % 4 == 0 ? "[crash]   " : " ").Str(registers[i].name).Str("=0x").Hex(registers[i].value);
            if (i % 4 == 3 || i + 1 == std::size(registers))
                b.Char('\n');
        }
        // Where the names come from: SpeedBreaker.pdb, or why not (without
        // DbgHelp, why: it didn't start, failed in an earlier walk, or isn't
        // to be had in this crash).
        b.Str("[crash] names: ").Str(platform::stack::SymbolsState()).Char('\n');
        // Innermost first: the faulting code, then its callers.
        b.Str("[crash] host stack (recompiled guest functions are sub_<guest address>):\n");
        void* frames[64];
        int count = dbghelp ? platform::stack::Walk(*context, frames, 64) : 0;
        if (count == 0)
            count = WalkStack(*context, frames, 64);
        for (int n = 0; n < count; n++)
        {
            b.Str("  #").Dec(n).Str(n < 10 ? "   " : "  ");
            detail::FormatFrame(b, frames[n], n > 0);
            b.Char('\n');
        }
        detail::WriteDirect(text, b.size);

        if (const int fd = s_reportFd.load(); fd >= 0)
        {
            detail::WriteAll(fd, s_header, s_headerSize);
            detail::WriteAll(fd, text + 1, b.size - 1);  // (without the leading blank line)
            static const char recent[] = "\nThe log's last lines:\n";
            detail::WriteAll(fd, recent, sizeof(recent) - 1);
            detail::WriteRecentOutput(fd, 300);
            static const char system[] = "\nSystem (at startup):\n";
            detail::WriteAll(fd, system, sizeof(system) - 1);
            const char* summary = detail::LoggedSystemSummary();
            detail::WriteAll(fd, summary, strlen(summary));
            // The handle itself (the C runtime's _commit would take its lock).
            FlushFileBuffers(reinterpret_cast<HANDLE>(_get_osfhandle(fd)));
            // (Shared for deleting, so it moves while still open.)
            bool renamed = MoveFileExW(s_pendingPathW, s_finalPathW, MOVEFILE_REPLACE_EXISTING) != 0;
            b.size = 0;
            b.Str("[crash] crash report: ").Str(renamed ? s_finalPath : s_pendingPath).Char('\n');
            detail::WriteDirect(text, b.size);
        }
    }
#else
    void WriteCrashReport(int sig, siginfo_t* info, void* context)
    {
        if (s_crashing.exchange(true))
        {
            // Another thread crashing while one report is written (a crash
            // often takes others with it, and the report waits up to a
            // quarter of a second for the log): wait for that report to
            // finish, whose _exit ends this thread too, rather than exit
            // under it and leave it cut short. A fault while reporting, on
            // the reporting thread itself: the caller exits.
            if (!t_reporting)
                for (int i = 0; i < 100; i++)
                {
                    struct timespec wait{ 0, 100000000 };
                    nanosleep(&wait, nullptr);
                }
            return;
        }
        t_reporting = true;
        // What was written just before (an assert's message) reaches the tail.
        FlushLog();

        static char text[24 * 1024];
        detail::SafeBuf b{ text, sizeof(text) - 1 };
        auto* fault = static_cast<uint8_t*>(info->si_addr);
        b.Str("\n[crash] ").Str(SignalName(sig)).Str(", signal ").Dec(sig).Str(" code ").Dec(info->si_code);
        // (si_addr means something for faults the CPU raised only, si_code
        // > 0; an abort's, or a signal another process sent, holds a pid.)
        if ((sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE) && info->si_code > 0)
        {
            if (g_memory.IsInMemoryRange(fault))
                b.Str(" at guest address ").Hex(uint32_t(fault - g_memory.base), 8, true);
            else
                b.Str(" at host address 0x").Hex(reinterpret_cast<uintptr_t>(fault));
        }
        const char* thread = detail::CurrentThreadName();
        b.Str(" in thread ").Str(*thread ? thread : "(unnamed)");
        b.Str(", ");
        AppendElapsed(b);
        b.Str(" into the session, ").Dec(int64_t(gpu::FrameCount())).Str(" game frames\n");
        if (PPCContext* ctx = GetPPCContext())
        {
            b.Str("[crash] guest registers: lr=").Hex(uint32_t(ctx->lr), 8, true).Str(" r1=").Hex(ctx->r1.u32, 8, true)
                .Str(" r3=").Hex(ctx->r3.u32, 8, true).Str(" r4=").Hex(ctx->r4.u32, 8, true).Str(" r5=").Hex(ctx->r5.u32, 8, true)
                .Str(" r31=").Hex(ctx->r31.u32, 8, true).Char('\n');
        }
        void* pc = detail::ContextPc(context);
#if defined(__APPLE__) && defined(__aarch64__)
        auto* mc = static_cast<ucontext_t*>(context)->uc_mcontext;
        b.Str("[crash] host registers: pc=0x").Hex(mc->__ss.__pc).Str(" lr=0x").Hex(mc->__ss.__lr).Str(" sp=0x").Hex(mc->__ss.__sp).Char('\n');
#elif defined(__linux__) && defined(__x86_64__)
        auto& gregs = static_cast<ucontext_t*>(context)->uc_mcontext.gregs;
        b.Str("[crash] host registers: rip=0x").Hex(uint64_t(gregs[REG_RIP])).Str(" rsp=0x").Hex(uint64_t(gregs[REG_RSP]))
            .Str(" rbp=0x").Hex(uint64_t(gregs[REG_RBP])).Char('\n');
#elif defined(__linux__) && defined(__aarch64__)
        auto& mc = static_cast<ucontext_t*>(context)->uc_mcontext;
        b.Str("[crash] host registers: pc=0x").Hex(uint64_t(mc.pc)).Str(" lr=0x").Hex(uint64_t(mc.regs[30]))
            .Str(" sp=0x").Hex(uint64_t(mc.sp)).Char('\n');
#endif
        // Innermost first: the faulting code, then its callers (the
        // handler's own frames left out).
        b.Str("[crash] host stack (recompiled guest functions are sub_<guest address>):\n");
        void* frames[64];
        int count = backtrace(frames, 64);
        int n = 0;
        auto frame = [&](void* address) {
            b.Str("  #").Dec(n).Str(n < 10 ? "   " : "  ");
            detail::FormatFrame(b, address, n > 0);
            b.Char('\n');
            n++;
        };
        if (pc)
            frame(pc);
        for (int i = detail::SignalFrames(frames, count); i < count; i++)
            if (frames[i] != pc)
                frame(frames[i]);
        detail::WriteDirect(text, b.size);

        if (const int fd = s_reportFd.load(); fd >= 0)
        {
            detail::WriteAll(fd, s_header, s_headerSize);
            detail::WriteAll(fd, text + 1, b.size - 1);  // (without the leading blank line)
            static const char recent[] = "\nThe log's last lines:\n";
            detail::WriteAll(fd, recent, sizeof(recent) - 1);
            detail::WriteRecentOutput(fd, 300);
            static const char system[] = "\nSystem (at startup):\n";
            detail::WriteAll(fd, system, sizeof(system) - 1);
            const char* summary = detail::LoggedSystemSummary();
            detail::WriteAll(fd, summary, strlen(summary));
            fsync(fd);
            bool renamed = rename(s_pendingPath, s_finalPath) == 0;
            b.size = 0;
            b.Str("[crash] crash report: ").Str(renamed ? s_finalPath : s_pendingPath).Char('\n');
            detail::WriteDirect(text, b.size);
        }
    }
#endif

    void ClearCrashMarker()
    {
        // A crash being reported is how the session ends: its report stays.
        if (s_crashing.load())
            return;
        if (const int fd = s_reportFd.exchange(-1); fd >= 0)
#ifdef _WIN32
            marker::Clear(fd, s_pendingPathW);
#else
            marker::Clear(fd, s_pendingPath);
#endif
    }

    void NoticePreviousCrash(bool lastClearsMarker)
    {
        std::filesystem::path dir = LogDirectory();
        std::error_code ec;
        // Markers of earlier sessions (crash_marker.h): empty (removed), or a
        // report the handler couldn't rename.
        for (const marker::Leftover& left : marker::Find(dir, SessionName()))
        {
            std::error_code fileError;
            if (left.ended == marker::Ended::Crashed)
            {
                std::filesystem::rename(left.path, dir / ("crash-" + left.session + ".txt"), fileError);
                continue;
            }
            std::filesystem::remove(left.path, fileError);
            // A version before v0.1.6 left one after every session.
            if (lastClearsMarker)
#ifdef _WIN32
                fprintf(stderr, "[crash] an earlier session (%s) ended without quitting: it was stopped from outside (Task Manager's End "
                    "task, Windows ending it, the power going) or crashed too hard to write a report; there is no report to send\n",
                    left.session.c_str());
#else
                fprintf(stderr, "[crash] an earlier session (%s) ended without quitting: it was stopped from outside (SIGKILL: a forced "
                    "quit, the power going) or crashed too hard to write a report; there is no report to send\n", left.session.c_str());
#endif
        }
        detail::KeepNewest(dir.string(), "crash-", ".txt", 5);

        std::string newest;
        for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        {
            std::string name = it->path().filename().string();
            if (name.starts_with("crash-") && name.ends_with(".txt") && name > newest)
                newest = name;
        }
        if (newest.empty())
            return;
        std::filesystem::path seenPath = dir / ".crash-seen";
        std::string seen;
        {
            std::ifstream in(seenPath);
            std::getline(in, seen);
        }
        if (seen == newest)
            return;
        fprintf(stderr, "[crash] an earlier session crashed: %s\n", (dir / newest).string().c_str());
        ui::Toast(std::format("The game crashed last time and saved a crash report. To send it: {}.", detail::kSaveBugReportHint), 15.0);
        std::ofstream out(seenPath, std::ios::trunc);
        out << newest << "\n";
    }

#ifdef _WIN32
    namespace
    {
        // NFSMW_TEST_CRASH=<s>:overflow: recursion until the stack runs out,
        // each call a little deeper (not a tail call): the fault comes with
        // the stack pointer at the very end of the stack, the hardest case
        // for the report (a frame of a page or more stops short of it, in
        // __chkstk).
        __declspec(noinline) int Overflow(int depth)
        {
            volatile char frame[192];
            frame[depth % 192] = char(depth);
            return depth >= 0 ? Overflow(depth + 1) + frame[0] : 0;
        }
    }
#endif

    [[noreturn]] void TestCrash()
    {
        const char* v = std::getenv("NFSMW_TEST_CRASH");
        if (v && strstr(v, ":abort"))
        {
            fprintf(stderr, "[test] NFSMW_TEST_CRASH: a failed check, then abort()\n");
            abort();
        }
#ifdef _WIN32
        // Windows only: a crash report needs room on the stack that ran out
        // (SetThreadStackGuarantee, watchdog.cpp). Elsewhere the crash
        // handler has no stack of its own (no sigaltstack) to report one.
        if (v && strstr(v, ":overflow"))
        {
            fprintf(stderr, "[test] NFSMW_TEST_CRASH: a stack overflow\n");
            Overflow(0);
        }
#endif
        fprintf(stderr, "[test] NFSMW_TEST_CRASH: a write to host address 0x10\n");
        static volatile uintptr_t address = 0x10;
        *reinterpret_cast<volatile int*>(address) = 1;
        __builtin_trap();
    }
}
