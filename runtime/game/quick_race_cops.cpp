// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See quick_race_cops.h.
#include <stdafx.h>
#include "quick_race_cops.h"

#include <kernel/heap.h>
#include <kernel/memory.h>
#include <region/region.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    using namespace game::quick_race_cops;

    const bool s_enabled = [] {
        const char* v = std::getenv("NFSMW_QUICK_RACE_COPS");
        const bool on = !(v && v[0] == '0');
        if (!on)
            fprintf(stderr, "[cops] NFSMW_QUICK_RACE_COPS=0: no Cops option in Quick Race's race options\n");
        return on;
    }();

    // The game's functions and data this file uses (USA addresses; region::Addr).
    constexpr uint32_t kMalloc = 0x8215CB08;            // operator new
    constexpr uint32_t kToggleWidgetCreate = 0x822E46B8; // FEToggleWidget(this, bool)
    constexpr uint32_t kAddToggleOption = 0x822E23D0;   // UIWidgetMenu::AddToggleOption(menu, widget, bool)
    constexpr uint32_t kSetLanguageHash = 0x822824C8;   // FEngSetLanguageHash(object, hash)
    constexpr uint32_t kTrackDirectionVtable = 0x820A5BE0;
    constexpr uint32_t kVtableBytes = 15 * 4;
    constexpr uint32_t kWidgetBytes = 92;
    constexpr uint32_t kVtableBlinkArrows = 56;         // slot offsets
    constexpr uint32_t kVtableDraw = 12;
    constexpr uint32_t kWidgetLabel = 44;               // FEObject* fields
    constexpr uint32_t kWidgetValue = 48;
    constexpr uint32_t kWidgetChanged = 42;             // byte the game's Acts set

    // The Cops rows' vtable: a copy of Track Direction's, in guest memory.
    std::atomic<uint32_t> s_vtable{0};

    uint32_t CopsVtable(uint8_t* base)
    {
        uint32_t vt = s_vtable.load(std::memory_order_acquire);
        if (vt)
            return vt;
        void* host = g_userHeap.Alloc(kVtableBytes);
        if (!host)
            return 0;
        memcpy(host, base + region::Addr(kTrackDirectionVtable), kVtableBytes);
        vt = g_memory.MapVirtual(host);
        s_vtable.store(vt, std::memory_order_release);
        fprintf(stderr, "[cops] Quick Race's race options get a Cops row (NFSMW_QUICK_RACE_COPS=0 for none)\n");
        return vt;
    }

    bool IsCopsWidget(uint8_t* base, uint32_t widget)
    {
        const uint32_t vt = s_vtable.load(std::memory_order_acquire);
        return vt != 0 && widget != 0 && PPC_LOAD_U32(widget) == vt;
    }

    void Call(PPCContext& ctx, uint8_t* base, uint32_t usa)
    {
        g_memory.FindFunction(region::Addr(usa))(ctx, base);
    }

    void CallIndirect(PPCContext& ctx, uint8_t* base, uint32_t guest)
    {
        g_memory.FindFunction(guest)(ctx, base);
    }

    uint32_t Database(uint8_t* base)
    {
        return PPC_LOAD_U32(region::Addr(kDatabasePointer));
    }

    bool OfferedNow(uint8_t* base)
    {
        const uint32_t db = Database(base);
        if (!db)
            return false;
        return Offered(PPC_LOAD_U8(db), PPC_LOAD_U32(db + kFlagsOffset));
    }

    uint32_t CurrentCopsByte(uint8_t* base)
    {
        const uint32_t db = Database(base);
        if (!db)
            return 0;
        const uint32_t type = PPC_LOAD_U32(db + kRaceTypeOffset);
        if (type >= 11)
            return 0;
        return CopsByte(db, type);
    }

    // After a setup built its rows: one more, Cops, at the bottom. `menu` is
    // the UIQRTrackOptions screen the setup was given.
    void AddCopsRow(PPCContext& ctx, uint8_t* base, uint32_t menu)
    {
        if (!s_enabled || !OfferedNow(base))
            return;
        const uint32_t vt = CopsVtable(base);
        if (!vt)
            return;
        const uint64_t lr = ctx.lr;

        ctx.r3.u64 = kWidgetBytes;
        ctx.r4.u64 = 0;
        ctx.r5.u64 = 0;
        ctx.r6.u64 = 0;
        Call(ctx, base, kMalloc);
        const uint32_t widget = ctx.r3.u32;
        if (widget)
        {
            ctx.r3.u64 = widget;
            ctx.r4.u64 = 1;
            Call(ctx, base, kToggleWidgetCreate);
            PPC_STORE_U32(widget, vt);
            ctx.r3.u64 = menu;
            ctx.r4.u64 = widget;
            ctx.r5.u64 = 1;
            Call(ctx, base, kAddToggleOption);
        }
        ctx.lr = lr;
    }

    // Cops row's Act (r3 = widget, r5 = message): left or right flips the
    // byte, then the arrows blink and the row redraws, as Catch Up's.
    void CopsAct(PPCContext& ctx, uint8_t* base)
    {
        const uint32_t widget = ctx.r3.u32;
        const uint32_t message = ctx.r5.u32;
        const uint64_t lr = ctx.lr;
        if (message == kMessageLeft || message == kMessageRight)
        {
            if (const uint32_t cops = CurrentCopsByte(base))
            {
                const uint8_t on = PPC_LOAD_U8(cops) ? 0 : 1;
                PPC_STORE_U8(cops, on);
                // The heat level the race forces with cops (byte +9, 1 by
                // default): never 0.
                const uint32_t heat = cops - kSettingsCops + kSettingsHeat;
                if (on && PPC_LOAD_U8(heat) == 0)
                    PPC_STORE_U8(heat, 1);
            }
        }
        const uint32_t vt = PPC_LOAD_U32(widget);
        PPC_STORE_U8(widget + kWidgetChanged, 1);
        ctx.r3.u64 = widget;
        ctx.r4.u64 = message;
        CallIndirect(ctx, base, PPC_LOAD_U32(vt + kVtableBlinkArrows));
        ctx.r3.u64 = widget;
        CallIndirect(ctx, base, PPC_LOAD_U32(vt + kVtableDraw));
        ctx.lr = lr;
    }

    // Cops row's Draw (r3 = widget): "COPS", "ON" or "OFF".
    void CopsDraw(PPCContext& ctx, uint8_t* base)
    {
        const uint32_t widget = ctx.r3.u32;
        const uint64_t lr = ctx.lr;
        const uint32_t cops = CurrentCopsByte(base);
        const bool on = cops && PPC_LOAD_U8(cops) != 0;
        ctx.r3.u64 = PPC_LOAD_U32(widget + kWidgetValue);
        ctx.r4.u64 = on ? kValueOn : kValueOff;
        Call(ctx, base, kSetLanguageHash);
        ctx.r3.u64 = PPC_LOAD_U32(widget + kWidgetLabel);
        ctx.r4.u64 = kLabelCops;
        Call(ctx, base, kSetLanguageHash);
        ctx.lr = lr;
    }
}

namespace game::quick_race_cops
{
    bool Enabled()
    {
        return s_enabled;
    }
}

// The hooks need each function's PAL name (region/pal_es_names.h) in a build
// with the PAL code; until survey/region_map.py has mapped them, a PAL build
// leaves the option out.
#if !SB_PAL_ES || defined(SB_PAL_ES_sub_82561F00)

// UIQRTrackOptions' setups (r3 = the screen). Drag (sub_82562408) is left as is.
#define SB_COPS_SETUP(name) \
    SB_HOOK(name) \
    { \
        const uint32_t menu = ctx.r3.u32; \
        SB_ORIGINAL(name)(ctx, base); \
        AddCopsRow(ctx, base, menu); \
    }

SB_COPS_SETUP(sub_825621A8)  // sprint
SB_COPS_SETUP(sub_82561F00)  // circuit (and the default)
SB_COPS_SETUP(sub_82562620)  // lap knockout
SB_COPS_SETUP(sub_825629B8)  // speedtrap
SB_COPS_SETUP(sub_82562C48)  // tollbooth

#undef SB_COPS_SETUP

// Track Direction's Act and Draw: the Cops rows' when the vtable is the copy.
SB_HOOK(sub_825616E8)
{
    if (IsCopsWidget(base, ctx.r3.u32))
    {
        CopsAct(ctx, base);
        return;
    }
    SB_ORIGINAL(sub_825616E8)(ctx, base);
}

SB_HOOK(sub_825617E8)
{
    if (IsCopsWidget(base, ctx.r3.u32))
    {
        CopsDraw(ctx, base);
        return;
    }
    SB_ORIGINAL(sub_825617E8)(ctx, base);
}

// cFrontendDatabase::FillCustomRace (r3 = the race, r4 = its settings): no
// cops in split screen, which doesn't offer the row, even when the race
// type's settings (shared with single player) have them on.
SB_HOOK(sub_822C4FB8)
{
    const uint32_t settings = ctx.r4.u32;
    if (!s_enabled || settings == 0 || OfferedNow(base))
    {
        SB_ORIGINAL(sub_822C4FB8)(ctx, base);
        return;
    }
    const uint8_t cops = PPC_LOAD_U8(settings + kSettingsCops);
    PPC_STORE_U8(settings + kSettingsCops, 0);
    SB_ORIGINAL(sub_822C4FB8)(ctx, base);
    PPC_STORE_U8(settings + kSettingsCops, cops);
}

#endif
