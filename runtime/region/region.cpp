// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See region.h.
#include <stdafx.h>
#include "region.h"

#include <kernel/memory.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <iterator>
#include <mutex>
#include <set>

extern uint64_t g_indirectCodeEnd;  // cpu/indirect.cpp

#if SB_PAL_ES
namespace pal_es
{
    extern PPCFuncMapping PPCFuncMappings[];
    extern const uint64_t kImageBase, kImageSize, kCodeBase, kCodeSize;
}
#endif
#if SB_PAL_DE
namespace pal_de
{
    extern PPCFuncMapping PPCFuncMappings[];
    extern const uint64_t kImageBase, kImageSize, kCodeBase, kCodeSize;
}
#endif

namespace region
{
    namespace
    {
        constexpr AddressPair s_palEs[] = {
#define SB_PAL_ES_ADDRESS(usa, pal) { usa, pal },
#include "pal_es_addresses.inc"
#undef SB_PAL_ES_ADDRESS
        };

#if SB_PAL_DE
        // survey/region_map.py table --region pal-de, from the player's own
        // USA and German default.xex (a build with ppc-pal-de/ has it).
        constexpr AddressPair s_palDe[] = {
#define SB_PAL_DE_ADDRESS(usa, pal) { usa, pal },
#include "pal_de_addresses.inc"
#undef SB_PAL_DE_ADDRESS
        };
        constexpr size_t kPalDeCount = std::size(s_palDe);
#else
        constexpr AddressPair s_palDe[] = { { 0, 0 } };  // unused: no German code in this build
        constexpr size_t kPalDeCount = 0;
#endif

        template <size_t N>
        constexpr bool Sorted(const AddressPair (&table)[N])
        {
            for (size_t i = 1; i < N; i++)
                if (table[i - 1].usa >= table[i].usa)
                    return false;
            return true;
        }
        static_assert(Sorted(s_palEs), "pal_es_addresses.inc: not sorted by USA address, or a duplicate");
        static_assert(Sorted(s_palDe), "pal_de_addresses.inc: not sorted by USA address, or a duplicate");

        // A PAL release's code, as scripts/region_ppc.py namespaced it.
        struct Code
        {
            const PPCFuncMapping* mappings;
            uint64_t imageBase, imageSize, codeBase, codeSize;
        };

        bool CodeOf(Release release, Code& code)
        {
#if SB_PAL_ES
            if (release == Release::PalEs)
            {
                code = { pal_es::PPCFuncMappings, pal_es::kImageBase, pal_es::kImageSize, pal_es::kCodeBase, pal_es::kCodeSize };
                return true;
            }
#endif
#if SB_PAL_DE
            if (release == Release::PalDe)
            {
                code = { pal_de::PPCFuncMappings, pal_de::kImageBase, pal_de::kImageSize, pal_de::kCodeBase, pal_de::kCodeSize };
                return true;
            }
#endif
            (void)release;
            (void)code;
            return false;
        }

        uint32_t Lookup(const AddressPair* table, size_t count, uint32_t usa, std::string_view name)
        {
            const AddressPair* end = table + count;
            const AddressPair* it = std::lower_bound(table, end, usa, [](const AddressPair& p, uint32_t a) { return p.usa < a; });
            if (it != end && it->usa == usa)
                return it->pal;
            // Not one the sources name (the table is complete for those):
            // say so once per address.
            static std::mutex mutex;
            static std::set<uint32_t> said;
            std::lock_guard lock(mutex);
            if (said.size() < 64 && said.insert(usa).second)
                fprintf(stderr, "[region] %08X has no %.*s address in the table: used as is\n", usa, int(name.size()), name.data());
            return usa;
        }
    }

    namespace detail
    {
        uint32_t PalAddr(uint32_t usa)
        {
            if (g_release == Release::PalDe)
                return Lookup(s_palDe, kPalDeCount, usa, "PAL (German)");
            return Lookup(s_palEs, std::size(s_palEs), usa, "PAL (Spanish)");
        }
    }

    bool Available(Release release)
    {
        Code code;
        return release == Release::Usa || CodeOf(release, code);
    }

    std::string_view Id(Release release)
    {
        switch (release)
        {
        case Release::PalEs: return "pal-es";
        case Release::PalDe: return "pal-de";
        default: return "usa";
        }
    }

    std::string_view Name(Release release)
    {
        switch (release)
        {
        case Release::PalEs: return "Europe (PAL, Spanish)";
        case Release::PalDe: return "Europe (PAL, German)";
        default: return "USA";
        }
    }

    bool FromId(std::string_view id, Release& release)
    {
        if (id == "usa")
            release = Release::Usa;
        else if (id == "pal-es")
            release = Release::PalEs;
        else if (id == "pal-de")
            release = Release::PalDe;
        else
            return false;
        return true;
    }

    void Select(Release release)
    {
        Code code;
        if (release == detail::g_release || !CodeOf(release, code))
            return;
        if (code.imageBase != PPC_IMAGE_BASE || code.imageSize != PPC_IMAGE_SIZE || code.codeBase != PPC_CODE_BASE)
        {
            fprintf(stderr, "[region] the %.*s code's layout (image %08llX+%llX, code %08llX) isn't the USA's: can't run it\n",
                int(Name(release).size()), Name(release).data(), (unsigned long long)code.imageBase,
                (unsigned long long)code.imageSize, (unsigned long long)code.codeBase);
            std::abort();
        }
        // The USA functions were put in the indirect-call table when guest
        // memory was mapped (kernel/memory.cpp): replace them all.
        const uint64_t span = std::max<uint64_t>(PPC_CODE_SIZE, code.codeSize) * 2;
        memset(g_memory.base + PPC_IMAGE_BASE + PPC_IMAGE_SIZE, 0, span);
        detail::g_release = release;
        g_indirectCodeEnd = CodeBase() + CodeSize();
        for (const PPCFuncMapping* m = FuncMappings(); m->guest != 0; m++)
            if (m->host != nullptr)
                g_memory.InsertFunction(m->guest, m->host);
        fprintf(stderr, "[region] %.*s: its own recompiled code (code %08llX+%llX)\n", int(Name(release).size()),
            Name(release).data(), (unsigned long long)CodeBase(), (unsigned long long)CodeSize());
    }

    const PPCFuncMapping* FuncMappings()
    {
        Code code;
        if (CodeOf(detail::g_release, code))
            return code.mappings;
        return PPCFuncMappings;
    }

    uint64_t CodeBase()
    {
        return PPC_CODE_BASE;
    }

    uint64_t CodeSize()
    {
        Code code;
        if (CodeOf(detail::g_release, code))
            return code.codeSize;
        return PPC_CODE_SIZE;
    }

    const AddressPair* PalEsTable(size_t& count)
    {
        count = std::size(s_palEs);
        return s_palEs;
    }

    const AddressPair* PalDeTable(size_t& count)
    {
        count = kPalDeCount;
        return s_palDe;
    }
}
