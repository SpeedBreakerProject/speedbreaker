// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Stacks and the names in them, on Windows (Windows only; report/crash.cpp
// and report/watchdog.cpp use it for crash and hang reports). macOS and
// Linux take theirs from backtrace(), dladdr and the executable's .symtab.
//
// - Names come from DbgHelp and SpeedBreaker.pdb, the PDB next to the .exe
//   (found by its name in the .exe's folder only: never the build machine's
//   path the .exe records, a symbol server or _NT_SYMBOL_PATH, so nothing
//   is fetched from anywhere). The runtime's functions have full debug
//   information; the recompiled code has only public symbols (__imp__sub_...
//   and the sub_... wrappers), which name it just as well. Without the PDB,
//   only the system DLLs' exports have names. DbgHelp loads it on a thread of
//   its own at startup (about 0.1 s and 25-35 MB on the ThinkPad, for a 49 MB
//   PDB), so a crash finds it ready.
// - Stacks are walked with StackWalk64 from a thread's registers, using the
//   unwind tables every x64 function has (RtlLookupFunctionEntry: any module,
//   loaded with DbgHelp or not).
// - Another thread's stack (a hang report): Capture suspends it, copies its
//   registers and the top of its stack into a buffer allocated beforehand,
//   and resumes it, calling nothing that allocates or takes a lock while it
//   is suspended (it may hold the heap's lock); the walk then reads the
//   copy, so the thread is stopped for a moment only.
// - DbgHelp is single-threaded: every call here takes one lock. The crash
//   handler keeps it once it has it (EnterCrash) and gives up on DbgHelp if
//   it can't have it soon, or if the crash happened inside DbgHelp; a fault
//   inside DbgHelp during a crash is caught, and the frame keeps a plainer
//   name.
#pragma once
#ifdef _WIN32
#include <platform/os.h>

#include <cstddef>
#include <cstdint>

namespace platform::stack
{
    // Loads the symbols (blocking: call it on a thread of its own). Each of
    // `warm` (code addresses in the program, may be null) is looked up once,
    // so the PDB's tables the crash handler would need are read now.
    void LoadSymbols(const void* const* warm, int warmCount);
    // LoadSymbols has finished (whether or not it found the PDB).
    bool SymbolsReady();
    // What LoadSymbols found, for reports: "SpeedBreaker.pdb", or why there
    // are no names ("no SpeedBreaker.pdb next to SpeedBreaker.exe"; DbgHelp
    // didn't start, failed in an earlier walk, or wasn't to be had in a crash).
    const char* SymbolsState();

    // A thread's registers and the top of its stack, as Capture found them.
    struct Snapshot
    {
        CONTEXT context;
        uint8_t* buffer = nullptr;  // allocated by the caller
        size_t capacity = 0;
        uintptr_t low = 0;          // the stack address of buffer[0] (its rsp)
        size_t size = 0;            // bytes copied
    };
    // Suspends `thread` (a handle with THREAD_SUSPEND_RESUME and
    // THREAD_GET_CONTEXT), takes its registers and its stack from rsp up to
    // `stackHigh` (at most `capacity` bytes), and resumes it. False if it
    // couldn't be suspended or read (it has ended).
    bool Capture(HANDLE thread, uintptr_t stackHigh, Snapshot& snapshot);

    // The stack from `context`: frames[0] is the context's rip, then the
    // return addresses, innermost first. With `snapshot`, stack memory comes
    // from its copy where it has it. 0 if DbgHelp isn't usable (not loaded
    // yet, busy past the crash handler's wait, or broken by the crash).
    int Walk(const CONTEXT& context, void** frames, int max, const Snapshot* snapshot = nullptr);
    // The same walk without DbgHelp, for when Walk can't (a hang report's
    // threads): RtlVirtualUnwind and the unwind tables over the snapshot's
    // copy of the stack, never the live stack. Ends where the copy does.
    int Unwind(const Snapshot& snapshot, void** frames, int max);

    // The function `pc` is in: its name (undecorated, cut to fit `size`), its
    // start, and its size (0 for a public symbol or an export: only its start
    // is known). False if there is no name, or DbgHelp isn't usable. Outside a
    // crash, a module DbgHelp hasn't seen yet (a driver's DLL) is loaded first.
    bool Name(const void* pc, char* name, size_t size, uintptr_t& start, uint32_t& length);

    // The crash handler, before it walks or names anything: DbgHelp for this
    // thread from now on, waiting up to `waitMs` for another thread to finish
    // with it. False (and DbgHelp unused from then on) if it isn't to be had,
    // the crash happened inside it on this thread, or it was unusable already;
    // SymbolsState then says which.
    bool EnterCrash(unsigned waitMs);
}
#endif
