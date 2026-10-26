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

namespace region
{
    namespace
    {
        constexpr AddressPair s_palEs[] = {
#define SB_PAL_ES_ADDRESS(usa, pal) { usa, pal },
#include "pal_es_addresses.inc"
#undef SB_PAL_ES_ADDRESS
        };

        constexpr bool Sorted()
        {
            for (size_t i = 1; i < std::size(s_palEs); i++)
                if (s_palEs[i - 1].usa >= s_palEs[i].usa)
                    return false;
            return true;
        }
        static_assert(Sorted(), "pal_es_addresses.inc: not sorted by USA address, or a duplicate");
    }

    namespace detail
    {
        uint32_t PalEsAddr(uint32_t usa)
        {
            const AddressPair* end = s_palEs + std::size(s_palEs);
            const AddressPair* it = std::lower_bound(s_palEs, end, usa, [](const AddressPair& p, uint32_t a) { return p.usa < a; });
            if (it != end && it->usa == usa)
                return it->pal;
            // Not one the sources name (the table is complete for those):
            // say so once per address.
            static std::mutex mutex;
            static std::set<uint32_t> said;
            std::lock_guard lock(mutex);
            if (said.size() < 64 && said.insert(usa).second)
                fprintf(stderr, "[region] %08X has no PAL address in the table: used as is\n", usa);
            return usa;
        }
    }

    bool Available(Release release)
    {
#if SB_PAL_ES
        return release == Release::Usa || release == Release::PalEs;
#else
        return release == Release::Usa;
#endif
    }

    std::string_view Id(Release release)
    {
        return release == Release::PalEs ? "pal-es" : "usa";
    }

    std::string_view Name(Release release)
    {
        return release == Release::PalEs ? "Europe (PAL, Spanish)" : "USA";
    }

    bool FromId(std::string_view id, Release& release)
    {
        if (id == "usa")
            release = Release::Usa;
        else if (id == "pal-es")
            release = Release::PalEs;
        else
            return false;
        return true;
    }

    void Select(Release release)
    {
        if (!Available(release) || release == detail::g_release)
            return;
#if SB_PAL_ES
        if (pal_es::kImageBase != PPC_IMAGE_BASE || pal_es::kImageSize != PPC_IMAGE_SIZE || pal_es::kCodeBase != PPC_CODE_BASE)
        {
            fprintf(stderr, "[region] the PAL code's layout (image %08llX+%llX, code %08llX) isn't the USA's: can't run it\n",
                (unsigned long long)pal_es::kImageBase, (unsigned long long)pal_es::kImageSize, (unsigned long long)pal_es::kCodeBase);
            std::abort();
        }
        // The USA functions were put in the indirect-call table when guest
        // memory was mapped (kernel/memory.cpp): replace them all.
        const uint64_t span = std::max<uint64_t>(PPC_CODE_SIZE, pal_es::kCodeSize) * 2;
        memset(g_memory.base + PPC_IMAGE_BASE + PPC_IMAGE_SIZE, 0, span);
        detail::g_release = release;
        g_indirectCodeEnd = CodeBase() + CodeSize();
        for (const PPCFuncMapping* m = FuncMappings(); m->guest != 0; m++)
            if (m->host != nullptr)
                g_memory.InsertFunction(m->guest, m->host);
        fprintf(stderr, "[region] %.*s: its own recompiled code (code %08llX+%llX)\n", int(Name(release).size()),
            Name(release).data(), (unsigned long long)CodeBase(), (unsigned long long)CodeSize());
#endif
    }

    const PPCFuncMapping* FuncMappings()
    {
#if SB_PAL_ES
        if (IsPalEs())
            return pal_es::PPCFuncMappings;
#endif
        return PPCFuncMappings;
    }

    uint64_t CodeBase()
    {
        return PPC_CODE_BASE;
    }

    uint64_t CodeSize()
    {
#if SB_PAL_ES
        if (IsPalEs())
            return pal_es::kCodeSize;
#endif
        return PPC_CODE_SIZE;
    }

    const AddressPair* PalEsTable(size_t& count)
    {
        count = std::size(s_palEs);
        return s_palEs;
    }
}
