// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See black_edition.h.
#include <stdafx.h>
#include "black_edition.h"

#include <kernel/memory.h>
#include <region/region.h>
#include <user/settings.h>

#include <cstdio>

namespace game::black_edition
{
    void ApplyAtLaunch()
    {
        const uint32_t addr = region::Addr(kFlag);
        uint8_t* flag = static_cast<uint8_t*>(g_memory.Translate(addr));
        // The word the Xenia patch writes and its neighbours, as the disc has them.
        const uint8_t* row = static_cast<const uint8_t*>(g_memory.Translate(addr - 6));
        char before[3 * 8 + 1] = {};
        for (int i = 0; i < 8; i++)
            snprintf(before + 3 * i, 4, "%02X%s", row[i], i < 7 ? " " : "");
        // A Restart option: its launch value (settings::Load, before the image).
        const bool on = settings::GetBool(settings::Id::BlackEdition);
        const char* env = settings::EnvOverride(settings::Id::BlackEdition);
        if (Apply(flag, on))
            fprintf(stderr, "[blackedition] on (%s): %08X set to 1; %08X-%08X were %s\n", env ? env : "Settings > Game",
                addr, addr - 6, addr + 1, before);
        else
            fprintf(stderr, "[blackedition] off: %08X left as the disc has it (%08X-%08X: %s)\n", addr, addr - 6,
                addr + 1, before);
    }
}
