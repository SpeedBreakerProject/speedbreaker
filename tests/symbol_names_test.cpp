// report::detail::GuestName (runtime/report/symbol_names.h): which symbol
// names are recompiled functions, and the sub_<address> each is shown as in
// crash and hang reports. Header only. From the repo root:
//   clang++ -std=c++20 -O2 -Iruntime tests/symbol_names_test.cpp -o build/symbol_names_test
//   build/symbol_names_test
// - The ELF and Mach-O names (Linux's .symtab, macOS's dladdr) give what
//   they gave before the Windows forms were added, byte for byte (the body's
//   name past "__imp__" whatever follows it, as a compiler's .cold split).
// - The PDB's names as DbgHelp gives them on Windows (the ThinkPad, 92306bf:
//   "_imp__sub_825987E0", "weak.?sub_82441190YAXAEIAUPPCContextPEAEZ.default
//   .__xmm", a hook's "sub_82441190") are recompiled functions too.
// - Nothing else is: runtime functions, near misses (7 or 9 digits,
//   lowercase hex), names that merely contain "sub_".
#include <report/symbol_names.h>

#include <cstdio>
#include <cstring>

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void Expect(const char* name, const char* want)
{
    char buffer[13];
    memset(buffer, 'x', sizeof(buffer));
    const char* got = report::detail::GuestName(name, buffer);
    if (!want)
        CHECK(got == nullptr, "%s -> %s, expected not a recompiled function", name, got ? got : "(null)");
    else
        CHECK(got && strcmp(got, want) == 0, "%s -> %s, expected %s", name, got ? got : "(null)", want);
}

int main()
{
    printf("1. ELF and Mach-O names, as before\n");
    Expect("__imp__sub_825987E0", "sub_825987E0");
    Expect("__imp__sub_825987E0.cold", "sub_825987E0.cold");  // as before: everything past __imp__
    Expect("_Z12sub_82441190R10PPCContextPh", "sub_82441190");
    Expect("_Z12sub_82441190R10PPCContextS_", nullptr);  // another signature: not one
    Expect("_ZN6report9TestCrashEv", nullptr);
    Expect("main", nullptr);

    printf("2. A PDB's names through DbgHelp (Windows)\n");
    Expect("_imp__sub_825987E0", "sub_825987E0");                 // a body's public symbol
    Expect("sub_82441190", "sub_82441190");                       // a hook, or a wrapper's public symbol undecorated
    Expect("?sub_82441190@@YAXAEIAUPPCContext@@PEAE@Z", "sub_82441190");  // decorated
    Expect("weak.?sub_82441190YAXAEIAUPPCContextPEAEZ.default.__xmm", "sub_82441190");  // lld-link's weak alias
    Expect(".weak.?sub_82441190@@YAXAEIAUPPCContext@@PEAE@Z.default.__xmm@00000000000000500000000000000090", "sub_82441190");

    printf("3. Not recompiled functions\n");
    Expect("report::TestCrash", nullptr);
    Expect("report::`anonymous namespace'::Watchdog", nullptr);
    Expect("sub_8244119", nullptr);       // 7 digits
    Expect("sub_824411900", nullptr);     // 9
    Expect("sub_82441l90", nullptr);      // not hex
    Expect("sub_825987e0", nullptr);      // XenonRecomp's are uppercase
    Expect("_imp__sub_8259", nullptr);
    Expect("?sub_8244@@YAXXZ", nullptr);
    Expect("RtlUserThreadStart", nullptr);
    Expect("my_sub_82441190", nullptr);
    Expect("", nullptr);

    printf(g_failures ? "%d failure(s)\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
