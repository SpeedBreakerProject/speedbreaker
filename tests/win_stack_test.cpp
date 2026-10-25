// platform::stack (runtime/platform/stack.h), Windows only: the stacks and
// names crash and hang reports print. Built and run by tests/CMakeLists.txt
// on Windows, with this file's debug information in win_stack_test.pdb and
// tests/win_stack_test_guest.cpp built without any, as the recompiled code is.
// 1. Before LoadSymbols nothing is named or walked (a crash that early gets
//    the generated table's names and RtlVirtualUnwind's frames instead).
// 2. LoadSymbols finds win_stack_test.pdb next to the .exe.
// 3. A stack from this thread's registers, through the "recompiled"
//    functions: each frame named, the recompiled ones (public symbols only)
//    as sub_<address> through report::detail::GuestName, a body with a
//    name of its own (__imp___xstart) exactly as it is.
// 4. A stack from an exception's registers (what the unhandled-exception
//    filter gets): the faulting function first, then its callers.
// 5. Another thread, blocked in a wait: Capture suspends it, copies its
//    registers and stack, and resumes it; the walk names where it waits.
//    The thread then runs on and overwrites that part of its stack, and the
//    old copy still walks to the old place (the walk reads the copy, not the
//    live stack); a new Capture finds the new place; the thread ends
//    normally (it was resumed every time).
// 6. A DLL DbgHelp hadn't seen is loaded for its name (its exports).
//    Unwind (no DbgHelp: RtlVirtualUnwind over the copy) finds the same
//    frames as Walk, from the old copy too.
// 7. EnterCrash: DbgHelp kept by this thread; names and walks go on, but no
//    DLL is loaded any more (nothing new is read in a crash).
// 8. A crash on another thread meanwhile can't have DbgHelp: the state says
//    so, Walk finds nothing, and Unwind still walks a blocked thread's copy
//    (what a hang report then falls back to).
#include <platform/stack.h>
#include <report/symbol_names.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

extern "C" void StackTestGuestRun(void (*callback)(void*), void* arg);  // win_stack_test_guest.cpp
volatile int g_sink = 0;

struct Walked
{
    void* frames[64];
    int count = 0;
};

// A frame's name as a report shows it: a return address looked up at the
// byte before it, a recompiled function as sub_<address>.
static std::string NameOf(void* pc, bool returnAddress)
{
    char name[256];
    uintptr_t start = 0;
    uint32_t length = 0;
    const void* at = static_cast<const uint8_t*>(pc) - (returnAddress ? 1 : 0);
    if (!platform::stack::Name(at, name, sizeof(name), start, length))
        return "?";
    char guest[13];
    if (const char* recompiled = report::detail::GuestName(name, guest))
        return recompiled;
    return name;
}

static std::vector<std::string> Names(const Walked& w)
{
    std::vector<std::string> names;
    for (int i = 0; i < w.count; i++)
        names.push_back(NameOf(w.frames[i], i > 0));
    return names;
}

static std::string Join(const std::vector<std::string>& names)
{
    std::string text;
    for (const std::string& n : names)
        text += (text.empty() ? "" : " < ") + n;
    return text;
}

// `wanted` in this order (others may come between), each a name that
// contains it.
static bool InOrder(const std::vector<std::string>& names, std::initializer_list<const char*> wanted)
{
    size_t at = 0;
    for (const char* w : wanted)
    {
        while (at < names.size() && names[at].find(w) == std::string::npos)
            at++;
        if (at == names.size())
            return false;
        at++;
    }
    return true;
}

// 3. ---------------------------------------------------------------------
__declspec(noinline) void StackTestCapture(void* out)
{
    CONTEXT context;
    RtlCaptureContext(&context);
    auto* w = static_cast<Walked*>(out);
    w->count = platform::stack::Walk(context, w->frames, 64);
    g_sink = g_sink + 1;
}

__declspec(noinline) void StackTestLevel2(Walked* w)
{
    StackTestGuestRun(StackTestCapture, w);
    g_sink = g_sink + 1;
}

__declspec(noinline) void StackTestLevel1(Walked* w)
{
    StackTestLevel2(w);
    g_sink = g_sink + 1;
}

// 4. ---------------------------------------------------------------------
static Walked g_fault;

static int OnFault(EXCEPTION_POINTERS* info)
{
    g_fault.count = platform::stack::Walk(*info->ContextRecord, g_fault.frames, 64);
    return EXCEPTION_EXECUTE_HANDLER;
}

__declspec(noinline) void StackTestFault()
{
    static volatile uintptr_t address = 0x10;
    *reinterpret_cast<volatile int*>(address) = 1;
    g_sink = g_sink + 1;
}

__declspec(noinline) void StackTestFaultCaller()
{
    __try
    {
        StackTestFault();
    }
    __except (OnFault(GetExceptionInformation()))
    {
    }
    g_sink = g_sink + 1;
}

// 5. ---------------------------------------------------------------------
struct Blocked
{
    HANDLE first = nullptr, second = nullptr;
    uintptr_t stackHigh = 0;
    std::atomic<int> phase{ 0 };
};

__declspec(noinline) void StackTestBlockedFirst(Blocked* b)
{
    b->phase = 1;
    WaitForSingleObject(b->first, INFINITE);
    g_sink = g_sink + 1;
}

// Writes over the stack below the thread's entry, where the first wait's
// frames were.
__declspec(noinline) void StackTestOverwrite()
{
    volatile char page[64 * 1024];
    for (size_t i = 0; i < sizeof(page); i++)
        page[i] = char(0xCC);
    g_sink = g_sink + page[100];
}

__declspec(noinline) void StackTestBlockedSecond(Blocked* b)
{
    b->phase = 2;
    WaitForSingleObject(b->second, INFINITE);
    g_sink = g_sink + 1;
}

DWORD WINAPI StackTestThread(void* arg)
{
    auto* b = static_cast<Blocked*>(arg);
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    b->stackHigh = high;
    StackTestBlockedFirst(b);
    StackTestOverwrite();
    StackTestBlockedSecond(b);
    g_sink = g_sink + 1;
    return 7;
}

static Walked g_firstWait;  // 5's first wait, unwound without DbgHelp

static DWORD WINAPI StackTestOtherCrash(void*)
{
    return platform::stack::EnterCrash(100) ? 1 : 0;
}

static void WaitForPhase(Blocked& b, int phase)
{
    for (int i = 0; i < 5000 && b.phase.load() != phase; i++)
        Sleep(1);
    Sleep(50);  // into the wait itself
}

int main()
{
    printf("1. before LoadSymbols: nothing named or walked\n");
    {
        Walked w;
        CONTEXT context;
        RtlCaptureContext(&context);
        w.count = platform::stack::Walk(context, w.frames, 64);
        CHECK(w.count == 0, "%d frames", w.count);
        CHECK(NameOf(reinterpret_cast<void*>(&StackTestLevel1), false) == "?", "a name already");
        CHECK(!platform::stack::SymbolsReady(), "ready already");
    }

    printf("2. LoadSymbols\n");
    const void* warm[] = { reinterpret_cast<const void*>(&StackTestCapture) };
    platform::stack::LoadSymbols(warm, 1);
    CHECK(platform::stack::SymbolsReady(), "not ready");
    printf("   %s\n", platform::stack::SymbolsState());
    CHECK(strcmp(platform::stack::SymbolsState(), "win_stack_test.pdb") == 0, "state: %s", platform::stack::SymbolsState());

    printf("3. this thread, through the recompiled functions\n");
    {
        Walked w;
        StackTestLevel1(&w);
        std::vector<std::string> names = Names(w);
        printf("   %s\n", Join(names).c_str());
        CHECK(w.count >= 7, "%d frames", w.count);
        CHECK(!names.empty() && names[0].find("StackTestCapture") != std::string::npos, "first: %s", names.empty() ? "" : names[0].c_str());
        CHECK(InOrder(names, { "StackTestCapture", "sub_82000010", "sub_82000020", "__imp___xstart", "StackTestGuestRun", "StackTestLevel2",
                                 "StackTestLevel1", "main" }),
            "not in order");
        // Exactly the names: no underscore lost (DbgHelp's SYMOPT_UNDNAME
        // drops one from a public C name), C++ undecorated.
        bool xstart = false, guest = false;
        for (const std::string& n : names)
        {
            xstart |= n == "__imp___xstart";
            guest |= n == "sub_82000020";
        }
        CHECK(xstart && guest, "__imp___xstart %d, sub_82000020 %d", xstart, guest);
    }

    printf("4. an exception's registers\n");
    StackTestFaultCaller();
    {
        std::vector<std::string> names = Names(g_fault);
        printf("   %s\n", Join(names).c_str());
        CHECK(!names.empty() && names[0].find("StackTestFault") != std::string::npos && names[0].find("Caller") == std::string::npos,
            "first: %s", names.empty() ? "" : names[0].c_str());
        CHECK(InOrder(names, { "StackTestFault", "StackTestFaultCaller", "main" }), "not in order");
    }

    printf("5. another thread, blocked\n");
    {
        Blocked b;
        b.first = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        b.second = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE thread = CreateThread(nullptr, 0, StackTestThread, &b, 0, nullptr);
        WaitForPhase(b, 1);
        std::vector<uint8_t> copy(1 << 20);
        platform::stack::Snapshot first;
        first.buffer = copy.data();
        first.capacity = copy.size();
        CHECK(platform::stack::Capture(thread, b.stackHigh, first), "Capture failed");
        CHECK(first.size > 0 && first.low == first.context.Rsp, "copied %zu bytes from %llx (rsp %llx)", first.size,
            (unsigned long long)first.low, (unsigned long long)first.context.Rsp);
        Walked w;
        w.count = platform::stack::Walk(first.context, w.frames, 64, &first);
        std::vector<std::string> names = Names(w);
        printf("   first wait: %s\n", Join(names).c_str());
        CHECK(InOrder(names, { "StackTestBlockedFirst", "StackTestThread" }), "not in order");
        Walked unwound;
        unwound.count = platform::stack::Unwind(first, unwound.frames, 64);
        std::vector<std::string> unwoundNames = Names(unwound);
        printf("   without DbgHelp: %s\n", Join(unwoundNames).c_str());
        CHECK(unwoundNames == names, "Unwind found other frames than Walk");
        g_firstWait = unwound;

        // On to the second wait, over the first one's frames.
        SetEvent(b.first);
        WaitForPhase(b, 2);
        Walked again;
        again.count = platform::stack::Walk(first.context, again.frames, 64, &first);
        std::vector<std::string> fromCopy = Names(again);
        printf("   the first copy again: %s\n", Join(fromCopy).c_str());
        CHECK(fromCopy == names, "the old copy walks elsewhere now");
        Walked unwoundAgain;
        unwoundAgain.count = platform::stack::Unwind(first, unwoundAgain.frames, 64);
        CHECK(Names(unwoundAgain) == names, "the old copy unwinds elsewhere now: %s", Join(Names(unwoundAgain)).c_str());

        std::vector<uint8_t> copy2(1 << 20);
        platform::stack::Snapshot second;
        second.buffer = copy2.data();
        second.capacity = copy2.size();
        CHECK(platform::stack::Capture(thread, b.stackHigh, second), "Capture failed");
        Walked now;
        now.count = platform::stack::Walk(second.context, now.frames, 64, &second);
        std::vector<std::string> nowNames = Names(now);
        printf("   second wait: %s\n", Join(nowNames).c_str());
        CHECK(InOrder(nowNames, { "StackTestBlockedSecond", "StackTestThread" }), "not in order");
        CHECK(Join(nowNames).find("StackTestBlockedFirst") == std::string::npos, "the first wait still there");

        SetEvent(b.second);
        DWORD waited = WaitForSingleObject(thread, 5000);
        DWORD code = 0;
        GetExitCodeThread(thread, &code);
        CHECK(waited == WAIT_OBJECT_0 && code == 7, "thread: wait %lu, exit code %lu", waited, code);
        CloseHandle(thread);
        CloseHandle(b.first);
        CloseHandle(b.second);
    }

    printf("6. a DLL DbgHelp hadn't seen: dbghelp.dll itself\n");
    {
        void* function = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"dbghelp.dll"), "SymFromAddr"));
        std::string name = function ? NameOf(function, false) : "(no SymFromAddr)";
        printf("   %s\n", name.c_str());
        CHECK(name.find("SymFromAddr") != std::string::npos, "%s", name.c_str());
    }

    printf("7. EnterCrash\n");
    {
        HMODULE version = LoadLibraryExW(L"version.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        CHECK(platform::stack::EnterCrash(1000), "EnterCrash failed");
        Walked w;
        StackTestLevel1(&w);
        std::vector<std::string> names = Names(w);
        printf("   %s\n", Join(names).c_str());
        CHECK(InOrder(names, { "StackTestCapture", "sub_82000010", "StackTestLevel1", "main" }), "not in order");
        // Code in a DLL DbgHelp hasn't seen (version.dll's entry point):
        // no name now, since nothing new is loaded in a crash.
        std::string unseen = "(no version.dll)";
        if (version)
        {
            auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(version);
            auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(version) + dos->e_lfanew);
            unseen = NameOf(reinterpret_cast<uint8_t*>(version) + nt->OptionalHeader.AddressOfEntryPoint, false);
        }
        printf("   version.dll's entry point: %s\n", unseen.c_str());
        CHECK(unseen == "?", "%s", unseen.c_str());
    }

    printf("8. a crash on another thread meanwhile\n");
    {
        HANDLE other = CreateThread(nullptr, 0, StackTestOtherCrash, nullptr, 0, nullptr);
        WaitForSingleObject(other, 5000);
        DWORD had = 2;
        GetExitCodeThread(other, &had);
        CloseHandle(other);
        CHECK(had == 0, "the other thread had DbgHelp (%lu)", had);
        printf("   %s\n", platform::stack::SymbolsState());
        CHECK(strstr(platform::stack::SymbolsState(), "unavailable in this crash") != nullptr, "state: %s",
            platform::stack::SymbolsState());
        Blocked b;
        b.first = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        b.second = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE thread = CreateThread(nullptr, 0, StackTestThread, &b, 0, nullptr);
        WaitForPhase(b, 1);
        std::vector<uint8_t> copy(1 << 20);
        platform::stack::Snapshot snapshot;
        snapshot.buffer = copy.data();
        snapshot.capacity = copy.size();
        CHECK(platform::stack::Capture(thread, b.stackHigh, snapshot), "Capture failed");
        Walked w;
        w.count = platform::stack::Walk(snapshot.context, w.frames, 64, &snapshot);
        CHECK(w.count == 0, "Walk found %d frames", w.count);
        Walked u;
        u.count = platform::stack::Unwind(snapshot, u.frames, 64);
        // The same code path as 5's first wait: the same return addresses.
        bool same = u.count == g_firstWait.count && u.count >= 3;
        for (int i = 1; same && i < u.count; i++)
            same = u.frames[i] == g_firstWait.frames[i];
        printf("   %d frames without DbgHelp, %s 5's\n", u.count, same ? "as" : "NOT as");
        CHECK(same, "%d frames against %d", u.count, g_firstWait.count);
        SetEvent(b.first);
        SetEvent(b.second);
        CHECK(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0, "the thread didn't end");
        CloseHandle(thread);
        CloseHandle(b.first);
        CloseHandle(b.second);
    }

    printf(g_failures ? "%d failure(s)\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
