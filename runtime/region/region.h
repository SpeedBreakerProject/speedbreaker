// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Which release of the game runs: the USA disc's default.xex, or the
// European (PAL, Spanish) one, chosen at startup by the installed
// default.xex's SHA-256 (install/manifest.h) before any guest code runs.
//
// The PAL default.xex is the USA build's source built for Europe: the same
// functions but 19, laid out in 17 pieces shifted by up to 0x200 bytes, .data
// and .bss where they were. Its own recompiled code (ppc-pal-es/, generated
// from the player's PAL disc with config/pal-es/, put in namespace pal_es by
// scripts/region_ppc.py) links beside the USA's (ppc/) when the build has it
// (SB_PAL_ES=1). The runtime is written against the USA addresses:
//   - a hook is SB_HOOK(sub_X) { ... SB_ORIGINAL(sub_X)(ctx, base); ... }:
//     the PAL code's calls to its own copy of the function reach the same
//     hook (a forwarder overrides the PAL copy), and SB_ORIGINAL calls the
//     running region's recompiled body;
//   - a guest address the runtime names (a global, a return address, a
//     constant) goes through region::Addr(usa), the running region's
//     (pal_es_addresses.inc: survey/region_map.py maps every one the sources
//     name and checks the PAL bytes there match; tests/region_addresses_check.py
//     keeps the table complete).
// With the USA release running, Addr is the address itself and SB_ORIGINAL the
// USA body: the USA game behaves exactly as before.
#pragma once
#include <cstdint>
#include <string_view>

struct PPCFuncMapping;

namespace region
{
    enum class Release : uint8_t
    {
        Usa,
        PalEs,
    };

    namespace detail
    {
        inline Release g_release = Release::Usa;
        uint32_t PalEsAddr(uint32_t usa);
    }

    inline Release Current()
    {
        return detail::g_release;
    }

    inline bool IsPalEs()
    {
        return detail::g_release == Release::PalEs;
    }

    // Whether this build has the release's recompiled code.
    bool Available(Release release);

    // "usa", "pal-es" (install/manifest.h's region ids); "USA", "Europe (PAL,
    // Spanish)".
    std::string_view Id(Release release);
    std::string_view Name(Release release);
    // The release with that manifest region id, if any.
    bool FromId(std::string_view id, Release& release);

    // Before any guest code runs (main.cpp, after reading default.xex), and
    // only for an Available release: switches the indirect-call table
    // (kernel/memory.cpp) to the release's functions.
    void Select(Release release);

    // The running release's guest address for a USA one the runtime names.
    // Every address the sources name is in the PAL table; one that isn't (a
    // computed address passed by mistake) is logged once and returned as is.
    inline uint32_t Addr(uint32_t usa)
    {
        return IsPalEs() ? detail::PalEsAddr(usa) : usa;
    }

    // The running release's recompiled functions (guest -> host), and its
    // code's extent (the indirect-call table covers it).
    const PPCFuncMapping* FuncMappings();
    uint64_t CodeBase();
    uint64_t CodeSize();

    // The PAL table itself (tests).
    struct AddressPair
    {
        uint32_t usa, pal;
    };
    const AddressPair* PalEsTable(size_t& count);
}

#define SB_CAT2(a, b) a##b
#define SB_CAT(a, b) SB_CAT2(a, b)

#if SB_PAL_ES
// SB_PAL_ES_sub_<USA> -> sub_<PAL> for every function the runtime names.
#include "pal_es_names.h"
#define SB_PAL_ES_SUB(name) SB_CAT(SB_PAL_ES_, name)
#define SB_PAL_ES_IMP(name) SB_CAT(__imp__, SB_PAL_ES_SUB(name))

// A hook of the recompiled function `name` (sub_<USA address>), followed by
// its body: declares the original in every release, and makes the PAL code's
// own copy of the function (weak) call this hook.
#define SB_HOOK(name) \
    extern "C" PPC_FUNC(__imp__##name); \
    PPC_FUNC(name); \
    namespace pal_es \
    { \
        PPC_FUNC(SB_PAL_ES_IMP(name)); \
        PPC_FUNC(SB_PAL_ES_SUB(name)) { ::name(ctx, base); } \
    } \
    PPC_FUNC(name)

// The running release's recompiled body of the hooked function `name`.
#define SB_ORIGINAL(name) (::region::IsPalEs() ? pal_es::SB_PAL_ES_IMP(name) : __imp__##name)
#else
#define SB_HOOK(name) \
    extern "C" PPC_FUNC(__imp__##name); \
    PPC_FUNC(name)
#define SB_ORIGINAL(name) __imp__##name
#endif
