// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The CPU check on Windows (PLAN.md 1.3, Unleashed's pattern raised to
// x86-64-v3). Everything else is built with -march=x86-64-v3, so on an
// older CPU the first AVX2, BMI or MOVBE instruction would crash before
// main. This runs first: built without -march (runtime/CMakeLists.txt) and
// using no templates, so no v3 copy of anything can be picked for it, at
// constructor priority 101 (".CRT$XCA00101", before every ordinary static
// initializer in .CRT$XCU). Without the instruction sets it says so in a
// message box and on stderr, and exits.
#include <windows.h>
#include <intrin.h>
#include <stdio.h>

namespace
{
    bool HasX86_64_V3(char* missing, size_t size)
    {
        int r[4];
        __cpuid(r, 0);
        int maxLeaf = r[0];
        __cpuid(r, 1);
        unsigned ecx1 = unsigned(r[2]);
        unsigned ebx7 = 0;
        if (maxLeaf >= 7)
        {
            __cpuidex(r, 7, 0);
            ebx7 = unsigned(r[1]);
        }
        __cpuid(r, int(0x80000000));
        unsigned ecxExt = 0;
        if (unsigned(r[0]) >= 0x80000001u)
        {
            __cpuid(r, int(0x80000001));
            ecxExt = unsigned(r[2]);
        }
        // The OS must save the YMM registers: OSXSAVE, then XCR0 bits 1-2.
        bool osAvx = (ecx1 & (1u << 27)) && (_xgetbv(0) & 6) == 6;
        struct Feature { const char* name; bool present; };
        const Feature features[] = {
            { "AVX", (ecx1 & (1u << 28)) && osAvx },
            { "AVX2", (ebx7 & (1u << 5)) && osAvx },
            { "FMA", (ecx1 & (1u << 12)) != 0 },
            { "F16C", (ecx1 & (1u << 29)) != 0 },
            { "MOVBE", (ecx1 & (1u << 22)) != 0 },
            { "BMI1", (ebx7 & (1u << 3)) != 0 },
            { "BMI2", (ebx7 & (1u << 8)) != 0 },
            { "LZCNT", (ecxExt & (1u << 5)) != 0 },
        };
        missing[0] = 0;
        for (const Feature& f : features)
            if (!f.present)
            {
                if (missing[0])
                    strncat_s(missing, size, ", ", _TRUNCATE);
                strncat_s(missing, size, f.name, _TRUNCATE);
            }
        return missing[0] == 0;
    }
}

__attribute__((constructor(101))) static void CheckCpu()
{
    char missing[128];
    if (HasX86_64_V3(missing, sizeof(missing)))
        return;
    char text[512];
    _snprintf_s(text, sizeof(text), _TRUNCATE,
        "SpeedBreaker needs a processor with the x86-64-v3 instruction sets (Intel Haswell or AMD Excavator, "
        "2013-2015, and newer). This one lacks: %s.", missing);
    fprintf(stderr, "[runtime] %s\n", text);
    fflush(stderr);
    MessageBoxA(nullptr, text, "SpeedBreaker", MB_OK | MB_ICONERROR);
    ExitProcess(1);
}
