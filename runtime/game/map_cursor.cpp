// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See map_cursor.h.
//
// One function override (the generated functions are weak; no ppc/
// regeneration): sub_822534F0, the World Map's MoveCursor (r3 = the screen,
// f1/f2 = the step). Called from the cursor step (ctx.lr = kStepReturn) with
// the screen's velocity, the step is scaled by the frame's game time over the
// 360's 2/60 s frame (at most 1); then MoveCursor runs as always.
// NFSMW_MAP_CURSOR=0 leaves the step as the game takes it (an A/B switch: the
// cursor twice as fast at 60 fps as at 30).
// NFSMW_MAP_CURSOR_LOG=1: one line per step (stderr): the frame clock and the
// frame's time, the game's step and the one MoveCursor got, and the cursor's
// position before and after (front end units).
#include <stdafx.h>
#include <region/region.h>
#include "map_cursor.h"
#include "frame_time.h"

#include <kernel/function.h>

#include <cmath>
#include <cstring>
#include <mutex>

extern "C" PPC_FUNC(__imp__sub_822534F0);

namespace
{
    using namespace game::map_cursor;

    const bool s_enabled = [] {
        const char* v = std::getenv("NFSMW_MAP_CURSOR");
        const bool on = !(v && v[0] == '0');
        if (!on)
            fprintf(stderr, "[mapcursor] NFSMW_MAP_CURSOR=0: the World Map cursor moves the game's step every frame "
                            "(twice the 360's speed at 60 fps)\n");
        return on;
    }();
    const bool s_log = [] {
        const char* v = std::getenv("NFSMW_MAP_CURSOR_LOG");
        return v && v[0] && v[0] != '0';
    }();

    float LoadFloat(uint8_t* base, uint32_t address)
    {
        const uint32_t bits = PPC_LOAD_U32(address);
        float f;
        memcpy(&f, &bits, sizeof(f));
        return f;
    }

    // The cursor object's position as sub_82280D88 reads it for every object
    // type but a string's (2, placed by its text); false for that.
    bool CursorAt(uint8_t* base, uint32_t screen, float& x, float& y)
    {
        const uint32_t obj = PPC_LOAD_U32(screen + kCursorObject);
        if (!obj)
            return false;
        const uint32_t type = PPC_LOAD_U32(obj + kObjectType);
        const uint32_t data = PPC_LOAD_U32(obj + kObjectData);
        if (!data || type == kStringType)
            return false;
        x = LoadFloat(base, data + kDataX);
        y = LoadFloat(base, data + kDataY);
        return true;
    }
}

SB_HOOK(sub_822534F0)
{
    if (static_cast<uint32_t>(ctx.lr) != region::Addr(kStepReturn))
        return SB_ORIGINAL(sub_822534F0)(ctx, base);
    const uint32_t screen = ctx.r3.u32;
    const double gameX = ctx.f1.f64, gameY = ctx.f2.f64;
    const double frameMs = game::CurrentFrameMs();
    // The game's code as read (sub_82253248 passes the screen's velocity):
    // anything else, leave it be.
    const bool asRead = float(gameX) == LoadFloat(base, screen + kVelocityX) && float(gameY) == LoadFloat(base, screen + kVelocityY);
    if (!asRead)
    {
        static std::once_flag once;
        std::call_once(once, [&] {
            fprintf(stderr, "[mapcursor] unexpected World Map step (screen %08X, step %g %g): the cursor left as the game moves it\n",
                screen, gameX, gameY);
        });
    }
    else if (s_enabled)
    {
        const double scale = StepScale(frameMs);
        ctx.f1.f64 = double(float(gameX * scale));
        ctx.f2.f64 = double(float(gameY * scale));
        static std::once_flag once;
        std::call_once(once, [] {
            fprintf(stderr, "[mapcursor] World Map cursor at the 360's speed: its step a frame scaled by the frame's game time "
                            "over %.1f ms, the 360's frame (NFSMW_MAP_CURSOR=0: the game's step every frame)\n",
                kConsoleFrameMs);
        });
    }
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    const bool had = s_log && CursorAt(base, screen, x0, y0);
    const double stepX = ctx.f1.f64, stepY = ctx.f2.f64;
    SB_ORIGINAL(sub_822534F0)(ctx, base);
    if (s_log)
    {
        const bool has = CursorAt(base, screen, x1, y1);
        fprintf(stderr, "[mapcursor] clock %.3f frame %.3f ms: game step %.3f %.3f, moved by %.3f %.3f; cursor %s%.2f %.2f -> %.2f %.2f\n",
            game::FrameClockMs(), frameMs, gameX, gameY, stepX, stepY, had && has ? "" : "(not read) ", x0, y0, x1, y1);
    }
}
