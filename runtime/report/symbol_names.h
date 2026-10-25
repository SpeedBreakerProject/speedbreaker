// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See report.h.
//
// Recompiled functions' names as symbol tables spell them (report/crash.cpp
// names stack frames with it). XenonRecomp emits each guest function's body
// as `extern "C" __imp__sub_<address>` and a weak C++ wrapper
// `sub_<address>(PPCContext&, uint8_t*)` (a hook in the runtime replaces the
// wrapper). Header only, for tests/symbol_names_test.cpp.
#pragma once
#include <cstring>

namespace report::detail
{
    namespace symbol_names
    {
        inline bool IsGuestAddress(const char* hex)
        {
            for (int i = 0; i < 8; i++)
                if (!((hex[i] >= '0' && hex[i] <= '9') || (hex[i] >= 'A' && hex[i] <= 'F')))
                    return false;
            return true;
        }

        // "sub_" and 8 uppercase hex digits at `p`.
        inline bool IsSub(const char* p)
        {
            return strncmp(p, "sub_", 4) == 0 && strnlen(p, 12) == 12 && IsGuestAddress(p + 4);
        }
    }

    // A recompiled function's own name ("sub_825987E0") for a symbol that
    // is one, else nullptr. Async-signal-safe; `buffer` holds 13 bytes.
    //   ELF and Mach-O (dladdr, .symtab):
    //     __imp__sub_825987E0                  the body
    //     _Z12sub_82441190R10PPCContextPh      the C++ wrapper or a hook (the
    //                                          compiler may put the body inline)
    //   A PDB, through DbgHelp (Windows; platform/stack.h):
    //     __imp__sub_825987E0                  the body (a public symbol)
    //     sub_82441190                         a hook's own debug symbol, or the
    //                                          wrapper's public one undecorated
    //     ?sub_82441190@@YAXAEIAUPPCContext@@PEAE@Z   the wrapper, decorated
    //     .weak.?sub_82441190@@YAXAEI...       lld-link's alias for a weak
    //                                          wrapper
    //   and the same as DbgHelp's SYMOPT_UNDNAME would spell them (the
    //   ThinkPad's first look, 92306bf): _imp__sub_825987E0 (a C name's first
    //   underscore dropped), weak.?sub_82441190YAXAEI...
    inline const char* GuestName(const char* name, char* buffer)
    {
        using namespace symbol_names;
        if (strncmp(name, "__imp__sub_", 11) == 0)
            return name + 7;
        if (strncmp(name, "_Z12sub_", 8) == 0 && strncmp(name + 16, "R10PPCContextPh", 15) == 0)
        {
            memcpy(buffer, name + 4, 12);
            buffer[12] = 0;
            return buffer;
        }
        if (strncmp(name, "_imp__sub_", 10) == 0 && IsSub(name + 6))
            return name + 6;
        if (IsSub(name) && name[12] == 0)
            return name;
        const char* decorated = nullptr;
        if (name[0] == '?')
            decorated = name + 1;
        else if (strncmp(name, "weak.?", 6) == 0)
            decorated = name + 6;
        else if (strncmp(name, ".weak.?", 7) == 0)
            decorated = name + 7;
        if (decorated && IsSub(decorated))
        {
            memcpy(buffer, decorated, 12);
            buffer[12] = 0;
            return buffer;
        }
        return nullptr;
    }
}
