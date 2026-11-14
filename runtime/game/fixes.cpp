// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See fixes.h.
//
// Settings > Fixes: community-sourced fixes ported from PC,
// one option each (the [fixes] table).
#include <stdafx.h>
#include <game/fe.h>
#include <game/fixes.h>
#include <game/mods.h>
#include <region/region.h>

#include <bit>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>

#include <kernel/function.h>
#include <kernel/heap.h>
#include <kernel/memory.h>
#include <ui/quit_game.h>
#include <user/settings.h>

// Shared by several fixes.
namespace
{
    namespace fe = game::fe;

    constexpr uint32_t kFeDatabase = 0x82A2C684;  // its +0x10: player 0 settings

    uint32_t Load32(uint32_t address)
    {
        return *static_cast<const be<uint32_t>*>(g_memory.Translate(address));
    }
}

namespace
{
    // Black Edition Content: the game's Black Edition flag, a byte the code
    // only reads (0 in the base game). Ported from the Xenia patch by tuhana.
    constexpr uint32_t kBlackEditionFlag = 0x82A2CE06;
}

namespace
{
    // Fix Timebug: the sim clock (Sim::GetTime), a float each step is added to.
    constexpr uint32_t kSimClock = 0x82A153B8;
    bool s_fixTimebug = false;
}

// Fix Timebug: as the float sim clock grows, each 1/60 s step added to it
// is rounded, so timers drift (5% faster after 5 hours). It's counted in
// double and stored rounded instead. Ported from WFP (OnSimStep).
void SimClockStepped(PPCRegister& f1)
{
    if (!s_fixTimebug)
        return;
    static double exact = 0.0;
    static float written = 0.0f;
    auto& clock = *static_cast<be<float>*>(g_memory.Translate(region::Addr(kSimClock)));

    // Not our last value plus the step: the game reset the clock, count on from it.
    exact = clock == written + float(f1.f64) ? exact + f1.f64 : double(clock);
    written = float(exact);
    clock = written;
}

// Fix Stuck Weather: cutscene time-of-day events set an overcast override
// (>= 0 replaces the cycle) that is rarely cleared. Cleared in the FE.
// Ported from X360 Stuff Pack (ResetOvercastOverrideAfterNIS).
namespace
{
    constexpr uint32_t kOvercastOverride = 0x82900CCC;  // float, < 0 = off
    constexpr uint32_t kGameFlowState = 0x82A39AD8;     // 3 = in FE
}

// Time-of-day lighting update (per frame).
SB_HOOK(sub_822265D0)
{
    if (settings::GetBool(settings::Id::FixStuckWeather)
        && PPC_LOAD_U32(region::Addr(kGameFlowState)) == 3)
    {
        const uint32_t at = region::Addr(kOvercastOverride);
        auto& overcast = *static_cast<be<float>*>(g_memory.Translate(at));
        if (overcast >= 0.0f)
        {
            fprintf(stderr, "[fixes] Stuck weather: cleared %.3f\n", float(overcast));
            overcast = -1.0f;
        }
    }
    SB_ORIGINAL(sub_822265D0)(ctx, base);
}

// Fix Busted Scenes: the arrest-scene chooser (sub_82135F98) falls through from
// heat 2 and 3 into the next heat's pick, so heat 4+ scenes replace their
// ArrestM04/F23/M07/M14/F14 (and "b") scenes. Ported from NFSMWExOpts
// (BustedNISFix). The hooks (config/nfsmw.toml) end the chooser right after the
// heat 2 or 3 pick, so the next one doesn't overwrite it. r31 is the heat level,
// r30 the picked scene's name.
namespace
{
    bool KeepBustedScene(bool pickedEarlier, const PPCRegister& r31, const PPCRegister& r30)
    {
        if (!pickedEarlier || !settings::GetBool(settings::Id::FixBustedScenes))
            return false;
        fprintf(stderr, "[fixes] Busted scene: heat %u keeps %.32s\n", r31.u32,
            static_cast<const char*>(g_memory.Translate(r30.u32)));
        return true;
    }
}

bool BustedSceneBeforeHeat3Pick(PPCRegister& r31, PPCRegister& r30)
{
    return KeepBustedScene(r31.u32 < 3, r31, r30);
}

bool BustedSceneBeforeHeat4Pick(PPCRegister& r31, PPCRegister& r30)
{
    return KeepBustedScene(r31.u32 < 4, r31, r30);
}

// Fix Leaderboard Refresh: the race leaderboard names/gaps countdown
// (+552, 90) ticks once per 1/30 s of real time, as on a 30 fps 360.
// Inspired by WFP (#1218). r26 = widget, before the game's decrement.
void LeaderboardCountdown(PPCRegister& r26)
{
    struct Pace { uint64_t lastNs; double carry; };
    static std::unordered_map<uint32_t, Pace> s_paces;  // per widget (split screen)
    if (!settings::GetBool(settings::Id::FixLeaderboardRefresh))
    {
        s_paces.clear();
        return;
    }
    uint64_t now = SDL_GetTicksNS();
    Pace& pace = s_paces.try_emplace(r26.u32, Pace{ now, 0.0 }).first->second;
    pace.carry += std::min((now - pace.lastNs) / 1e9, 0.25);  // pauses don't burst
    pace.lastNs = now;
    int ticks = int(pace.carry * 30.0);
    pace.carry -= ticks / 30.0;

    uint8_t* base = g_memory.base;
    int32_t counter = int32_t(PPC_LOAD_U32(r26.u32 + 552));
    if (counter > 0)
        // the game takes 1 off next
        PPC_STORE_U32(r26.u32 + 552, uint32_t(std::max(1, counter - ticks + 1)));
}

namespace
{
    // Fix Cop Spoiler Damage: COPSPORTHENCH and COPSPORTGHOST spoilers never get
    // damaged, as the damage zone map lacks their slot (4). Four of its duplicate
    // entries become spoiler entries. Spotted in NFSMW Unlimiter, done from scratch.
    constexpr uint32_t kDamageZoneMap = 0x828F9E88;
    constexpr uint32_t kCopSpoilerSlot = 4;
    struct ZoneSlotEntry
    {
        uint32_t index;  // a duplicate entry: zone, slot
        uint32_t zone;
    };
    constexpr ZoneSlotEntry kCopSpoilerEntries[] = {
        { 5, 1 },   // FRONT, BODY (also entry 2)        -> REAR
        { 38, 6 },  // LEFT, slot 17 (also entry 30)     -> LREAR
        { 39, 7 },  // LEFT, slot 18 (also entry 29)     -> RREAR
        { 64, 8 },  // LFRONT, slot 18 (also entry 61)   -> TOP
    };
}

// Fix Pursuit Crashes: a roadblock is built from a list of cop cars, and
// a car wrecked or despawned meanwhile (back to its abstract vtable)
// gets called. Such cars are skiped. Ported from WFP (RoadblockCarHook).
namespace
{
    constexpr uint32_t kPureCall = 0x826B87F8;      // _purecall
    constexpr uint32_t kFeListeners = 0x82C6F08C;   // vector: begin +4, count +0xC
}

bool RoadblockCarLoaded(PPCRegister& r30)
{
    if (!settings::GetBool(settings::Id::FixPursuitCrashes))
        return false;
    uint8_t* base = g_memory.base;
    const uint32_t car = r30.u32;
    const uint32_t vtable = car ? PPC_LOAD_U32(car) : 0;
    if (vtable && PPC_LOAD_U32(vtable + 0x80) != region::Addr(kPureCall))
        return false;
    fprintf(stderr, "[fixes] Pursuit crash: roadblock skips a destroyed cop car\n");
    return true;
}

// Fix Pursuit Crashes: package-change listeners (pursuit HUD parts as well)
// can leave the list while it's notified, and the loop ran past its end.
// They're notified from a copy, each only while still listed.
// Ported from WFP (NotifyListeners).
SB_HOOK(sub_822AA780)
{
    if (!settings::GetBool(settings::Id::FixPursuitCrashes))
    {
        SB_ORIGINAL(sub_822AA780)(ctx, base);
        return;
    }
    const uint32_t list = region::Addr(kFeListeners);
    auto listed = [&](uint32_t listener) {
        const uint32_t begin = PPC_LOAD_U32(list + 4), count = PPC_LOAD_U32(list + 0xC);
        for (uint32_t i = 0; i < count; i++)
            if (PPC_LOAD_U32(begin + i * 4) == listener)
                return true;
        return false;
    };
    std::vector<uint32_t> listeners(PPC_LOAD_U32(list + 0xC));
    for (uint32_t i = 0; i < listeners.size(); i++)
        listeners[i] = PPC_LOAD_U32(PPC_LOAD_U32(list + 4) + i * 4);
    for (uint32_t listener : listeners)
    {
        if (!listener || !listed(listener))
            continue;
        PPCContext call = ctx;
        call.r3.u64 = listener;
        g_memory.FindFunction(PPC_LOAD_U32(PPC_LOAD_U32(listener) + 0xC))(call, base);
    }
    if (PPC_LOAD_U32(list + 0xC) < listeners.size())
        fprintf(stderr, "[fixes] Pursuit crash: FE listener left while notified\n");
}

// Intro Videos skip (ported from WFP): a skipped movie file is reported
// missing; the game then ends it itself, as it does for any missing movie.
namespace
{
    // Return addresses of sub_82282F18's two movie file probes.
    constexpr uint32_t kMovieProbe1 = 0x82282FAC;
    constexpr uint32_t kMovieProbe2 = 0x82282FD0;

    std::atomic<bool> s_titleReached = false;
    thread_local bool t_skipMovie = false;

    bool SkipMovie(const PPCContext& ctx, uint8_t* base)
    {
        using enum settings::IntroVideos;
        const auto mode = settings::IntroVideos(settings::GetInt(settings::Id::IntroVideos));
        const uint32_t event = ctx.r4.u32;
        if (mode == Original || s_titleReached || !event || PPC_LOAD_U32(event + 0x18) != 7)  // start movie
            return false;
        const uint32_t nameAddress = PPC_LOAD_U32(event + 0x24);  // pointer to the movie name
        if (!nameAddress)
            return false;
        const char* name = static_cast<const char*>(g_memory.Translate(nameAddress));
        return mode == SkipAll
            || (mode == EaLogoOnly && strncasecmp(name, "ealogo", 6) != 0)  // prefix for other locales
            || (mode == SkipEaHd && strcasecmp(name, "eahd_bumper") == 0);
    }
}

// The start movie handler.
SB_HOOK(sub_82282F18)
{
    t_skipMovie = SkipMovie(ctx, base);
    SB_ORIGINAL(sub_82282F18)(ctx, base);
    t_skipMovie = false;
}

// The file probe: 0 (missing) when the handler probes a skipped movie.
SB_HOOK(sub_823BCA60)
{
    const uint32_t lr = static_cast<uint32_t>(ctx.lr);
    if (t_skipMovie && (lr == region::Addr(kMovieProbe1) || lr == region::Addr(kMovieProbe2)))
        ctx.r3.u64 = 0;
    else
        SB_ORIGINAL(sub_823BCA60)(ctx, base);
}

// The title screen message handler.
SB_HOOK(sub_822B8988)
{
    s_titleReached = true;
    SB_ORIGINAL(sub_822B8988)(ctx, base);
}

// The boot sequence's next screen: a late movie-finished signal can arrive
// after the game deleted the sequence (r3 = 0). Ported from WFP NextScreen fix.
SB_HOOK(sub_822B8458)
{
    if (ctx.r3.u32)
        SB_ORIGINAL(sub_822B8458)(ctx, base);
}

// Longer Alias Names: the alias keyboard takes up to 15 characters (same
// A-Z/0-9 set), and the alias load list keeps saves with names that long.
// Ported from NFSMWExOpts (AllowLongerProfileNames).
constexpr uint32_t kLongAliasName = 15;

static bool LongerAliasNames() { return settings::GetBool(settings::Id::LongerAliasNames); }

// Alias keyboard setup (sub_8228EB30), r5 = max length.
void AliasNameLengthSet(PPCRegister& r5)
{
    if (LongerAliasNames())
        r5.u64 = kLongAliasName;
}

// Load list builder (sub_822926F0), r11 = a save's name length; true lists it.
bool AliasListNameFits(PPCRegister& r11)
{
    return LongerAliasNames() && r11.u32 <= kLongAliasName;
}

// Modern Gamepad Layouts: all 10 control types get modernized layouts,
// steering with the left stick; the D-pad keeps the map and messages.
// The bindings are attrib data (class "controller"), swapped in only
// while the game builds a type's input map, and in the labels shown.
// With Skip EA Trax In-Game on, types 1, 2, 6, and 7 skip on RB and
// the D-pad reverts to its original behavior.
namespace
{
    enum Input : uint8_t
    {
        A, B, X, Y, LB, RB, LT, RT, RsUp, RsDown, RsClick, LsLeft, LsRight,
        WheelLeft, WheelRight, DpadUp, DpadDown, DpadLeft, DpadRight, kInputs
    };

    struct InputRecord
    {
        uint32_t hash[3];  // the input's ids in the attrib record
        const char* name;  // its name there ("Black" is LB, "White" RB)
    };
    constexpr InputRecord kInputRecords[kInputs] = {
        { { 0x61E1B0BB, 0x80F12F70, 0x48FC13CF }, "A" },
        { { 0x2B5A6A57, 0xADF315F3, 0xB2ED475C }, "B" },
        { { 0x2CDDC4B6, 0xFB6A597B, 0xE1338132 }, "X" },
        { { 0x9C3A7275, 0x10D0A3BD, 0x4D917D0F }, "Y" },
        { { 0xFCA03E74, 0x510FD477, 0xB2AE322C }, "Black" },
        { { 0xB0267E91, 0x93C47837, 0x30BF88B3 }, "White" },
        { { 0xD163F591, 0xE00E5C1A, 0xE9195336 }, "L" },
        { { 0x11B730F9, 0xBEE9739B, 0xB62B540F }, "R" },
        { { 0x67C2F1B5, 0x73B1CF5C, 0x7C1D0B82 }, "ARY_U" },
        { { 0x0CF006D1, 0x01109F79, 0x41A218C3 }, "ARY_D" },
        { { 0xDA6456EF, 0xE751C392, 0xAFA0BD9E }, "RThumb" },
        { { 0xD9F3C916, 0xBE968E6C, 0xEB417D4D }, "ALX_L" },
        { { 0x5D2463C5, 0x3625647C, 0x8B9108C0 }, "ALX_R" },
        { { 0x3EDDC237, 0x4FDEE699, 0x211AB403 }, "W_LEFT" },
        { { 0x49CCDB49, 0x929BDA0B, 0x428C7C6A }, "W_RIGHT" },
        { { 0x076010AA, 0x264C6514, 0x5AC3F396 }, "DUp" },
        { { 0xE7367324, 0x3C719305, 0x3FB3C47C }, "DDown" },
        { { 0xA7041223, 0x59B11FE6, 0x3859BBB1 }, "DLeft" },
        { { 0x71A5F5D0, 0x058A2BD5, 0x574BE632 }, "DRight" },
    };

    // An action: its attrib key, and the flag /
    // deadzone the original types use for it.
    struct Action
    {
        uint32_t key;
        uint32_t flag;
        float deadzone;
    };
    constexpr Action kGas{ 0x06525329, 0, 0.0f }, kBrake{ 0x82A9B7D8, 0, 0.0f },
        kNitrous{ 0xABC46A0F, 0, 0.0f }, kEbrake{ 0x1AAF2C13, 0, 0.0f },
        kSpeedbreaker{ 0x98D605B8, 1, 0.0f }, kLookbackAction{ 0x8DC18FAF, 0, 0.3f },
        kCamera{ 0x2F478795, 1, 0.3f }, kShiftUp{ 0x06DBD3C3, 1, 0.0f },
        kShiftDown{ 0x50638735, 1, 0.0f },
        kSteerLeft{ 0x37B1B6A8, 0, 0.2f }, kSteerRight{ 0xB3A37C41, 0, 0.2f },
        kTurnLeft{ 0x77570455, 1, 0.0f }, kTurnRight{ 0xFA647F8F, 1, 0.0f },
        kHudEngage{ 0xBBAB56EF, 1, 0.0f }, kHudPadLeft{ 0xE2D51BDC, 1, 0.0f },
        kHudPadRight{ 0x713C8F5C, 1, 0.0f }, kHudPadDown{ 0x112B5D2D, 1, 0.0f },
        kHudNextSong{ 0x5A1E0EB7, 1, 0.0f },
        kStickLookback{ 0x8DC18FAF, 0, 0.5f };  // RS down: clear of RS click

    struct Record
    {
        Input input = A;
        uint32_t flag = 0;
        float deadzone = 0.0f;
    };

    struct Binding
    {
        uint32_t action = 0;
        Record records[2];
        uint8_t count = 0;
    };
    constexpr Record Rec(Action a, Input i) { return { i, a.flag, a.deadzone }; }
    constexpr Binding Bind(Action a, Input i) { return { a.key, { Rec(a, i) }, 1 }; }
    constexpr Binding Bind(Action a, Input i, Input j) { return { a.key, { Rec(a, i), Rec(a, j) }, 2 }; }
    // Wheel records: what the stick types of configs 2-4 have besides the stick.
    constexpr Binding Wheel(Action a, Input stick, Input wheel)
    {
        return { a.key, { Rec(a, stick), { wheel, a.flag, 0.0f } }, 2 };
    }

    // An attrib collection and the actions it gets instead.
    constexpr size_t kMaxBindings = 13;
    struct Override
    {
        uint32_t collection = 0;
        Binding bindings[kMaxBindings];
        size_t count = 0;
    };
    constexpr Override Make(uint32_t collection, std::initializer_list<Binding> list)
    {
        Override o{ collection };
        for (const Binding& b : list)
            o.bindings[o.count++] = b;
        return o;
    }
    // Steering: types 1-5 keep theirs; 6-10 steer with the D-pad originally
    // and get the stick (8-10 plus wheel records).
    enum Steer : uint8_t { KeepSteer, Stick, StickWheel };
    struct SteerAction
    {
        Action action;
        Input stick, wheel;
    };
    constexpr SteerAction kSteerActions[] = { { kSteerLeft, LsLeft, WheelLeft },
        { kSteerRight, LsRight, WheelRight }, { kTurnLeft, LsLeft, WheelLeft },
        { kTurnRight, LsRight, WheelRight } };

    // A control type: gas RT, brake LT, nitrous A, and the rest.
    constexpr Override Type(uint32_t collection, Steer steer, Binding ebrake, Input speedbreaker,
        Input lookback, Input camera, Input shiftUp, Input shiftDown)
    {
        Override o = Make(collection, { Bind(kGas, RT), Bind(kBrake, LT), Bind(kNitrous, A), ebrake,
            Bind(kSpeedbreaker, speedbreaker),
            Bind(lookback == RsDown ? kStickLookback : kLookbackAction, lookback),
            Bind(kCamera, camera), Bind(kShiftUp, shiftUp), Bind(kShiftDown, shiftDown) });
        if (steer != KeepSteer)
            for (const SteerAction& s : kSteerActions)
                o.bindings[o.count++] = steer == StickWheel
                    ? Wheel(s.action, s.stick, s.wheel) : Bind(s.action, s.stick);
        return o;
    }

    // kHudMap: the stick types' HUD map (D-pad: map, messages).
    // kHudMapTraxOnRb: the D-pad types' one, unused by stick
    // types, rebuilt as the same plus next song on RB.
    constexpr uint32_t kHudMap = 0x26190AC6;
    constexpr uint32_t kHudMapTraxOnRb = 0xCAAE372F;

    constexpr Override kOverrides[] = {
        // Steering, e-brake, speedbreaker, look back, camera, shift up, shift down.
        Type(0xAD37E45A, KeepSteer,  Bind(kEbrake, X),    B,  LB,     Y,       RsUp, RsDown),  // 1
        Type(0x58341EC2, KeepSteer,  Bind(kEbrake, B),    X,  LB,     Y,       RsUp, RsDown),  // 2
        Type(0x58CB8BB8, KeepSteer,  Bind(kEbrake, X, B), RB, LB,     Y,       RsUp, RsDown),  // 3
        Type(0xCFA829CC, KeepSteer,  Bind(kEbrake, X, B), LB, RB,     Y,       RsUp, RsDown),  // 4
        Type(0x2BC75215, KeepSteer,  Bind(kEbrake, X, B), Y,  LB,     RB,      RsUp, RsDown),  // 5
        Type(0x73928352, Stick,      Bind(kEbrake, X),    LB, RsDown, RsClick, Y,    B),       // 6
        Type(0x35229E26, Stick,      Bind(kEbrake, B),    LB, RsDown, RsClick, Y,    X),       // 7
        Type(0xEAA1761D, StickWheel, Bind(kEbrake, X),    RB, LB,     RsClick, Y,    B),       // 8
        Type(0xE7901ECC, StickWheel, Bind(kEbrake, B),    RB, LB,     RsClick, Y,    X),       // 9
        Type(0x80D3785A, StickWheel, Bind(kEbrake, RB),   B,  LB,     RsClick, Y,    X),       // 10
        Make(kHudMapTraxOnRb, { Bind(kHudEngage, DpadUp), Bind(kHudPadLeft, DpadLeft),
            Bind(kHudPadRight, DpadRight), Bind(kHudPadDown, DpadDown), Bind(kHudNextSong, RB) }),
    };
    constexpr size_t kOverrideCount = std::size(kOverrides);

    // Labels (LABELS.BIN) of the controls screen's 17 slots.
    enum Slot : uint8_t
    {
        SlotLT, SlotLS, SlotRT, SlotY, SlotB, SlotA, SlotX,
        SlotDpadUp, SlotDpadLeft, SlotDpadRight, SlotDpadDown,
        SlotLB, SlotRB, SlotBack, SlotRsUp, SlotRsDown, SlotStart, kSlots
    };

    // Type labels: the face buttons, bumpers and RS; triggers and the rest stay.
    struct TypeLabels
    {
        uint32_t shownAs;  // config whose RS icons the screen shows (1, 3: RS click)
        const char* y, * b, * a, * x, * lb, * rb, * rsUp, * rsDown;
    };
    constexpr const char* kPov = "BUTTON_CHANGE_POV", * kEb = "BUTTON_EBRAKE",
        * kNos = "BUTTON_NITROUS", * kSb = "BUTTON_GAMEBREAKER", * kLook = "BUTTON_LOOKBACK",
        * kUp = "BUTTON_SHIFTUP", * kDown = "BUTTON_SHIFTDOWN", * kNone = "BUTTON_NOT_USED";
    constexpr TypeLabels kTypeLabels[10] = {
        { 0, kPov, kSb, kNos, kEb, kLook, kNone, kUp, kDown },
        { 0, kPov, kEb, kNos, kSb, kLook, kNone, kUp, kDown },
        { 2, kPov, kEb, kNos, kEb, kLook, kSb, kUp, kDown },
        { 2, kPov, kEb, kNos, kEb, kSb, kLook, kUp, kDown },
        { 4, kSb, kEb, kNos, kEb, kLook, kPov, kUp, kDown },
        { 1, kUp, kDown, kNos, kEb, kSb, kNone, kPov, kLook },
        { 1, kUp, kEb, kNos, kDown, kSb, kNone, kPov, kLook },
        { 3, kUp, kDown, kNos, kEb, kLook, kSb, kPov, kNone },
        { 3, kUp, kEb, kNos, kDown, kLook, kSb, kPov, kNone },
        { 1, kUp, kSb, kNos, kDown, kLook, kEb, kPov, kNone },
    };

    constexpr uint32_t kLabelTable = 0x828FFBB0;    // 17 slots x 5 configs
    constexpr uint32_t kControlsPlayer = 0x82A2C518;  // the controls screen's player
    // The HUD map builds (sub_822A9798, sub_82478F50) from HUD key table;
    // r9 = the player's config * 2 there.
    constexpr uint32_t kHudMapBuildReturn = 0x822A98C4;
    constexpr uint32_t kHudMapBuildReturn2 = 0x82479058;
    constexpr uint32_t kHudInputMap = 48;  // the HUD's input map; +628 its key
    constexpr uint32_t kInputMapKey = 628;

    // Guest copies of the attrib nodes the build reads instead (+4 data, +0xB flags).
    uint32_t s_nodes[kOverrideCount][kMaxBindings];
    bool s_modernLayouts = false;  // set at boot (Restart)
    thread_local int t_building = -1;

    uint32_t GuestAlloc(size_t size)
    {
        void* p = g_userHeap.Alloc(size);
        memset(p, 0, size);
        return g_memory.MapVirtual(p);
    }

    // The arrays and nodes, built once: {u16 count, u16 count, u16 record
    // size, u16 0} and the records {3 ids, name, flag, deadzone, 0, 0}.
    void BuildOverrideData(uint8_t* base)
    {
        if (s_nodes[0][0])
            return;
        constexpr uint32_t kRecordSize = 0x20;
        uint32_t names[kInputs];
        for (int i = 0; i < kInputs; i++)
        {
            const size_t len = strlen(kInputRecords[i].name) + 1;
            names[i] = GuestAlloc(len);
            memcpy(base + names[i], kInputRecords[i].name, len);
        }
        for (size_t o = 0; o < kOverrideCount; o++)
            for (size_t b = 0; b < kOverrides[o].count; b++)
            {
                const Binding& bind = kOverrides[o].bindings[b];
                const uint32_t array = GuestAlloc(8 + kRecordSize * bind.count);
                PPC_STORE_U16(array, uint16_t(bind.count));
                PPC_STORE_U16(array + 2, uint16_t(bind.count));
                PPC_STORE_U16(array + 4, uint16_t(kRecordSize));
                for (int i = 0; i < bind.count; i++)
                {
                    const Record& rec = bind.records[i];
                    const uint32_t r = array + 8 + kRecordSize * i;
                    for (int h = 0; h < 3; h++)
                        PPC_STORE_U32(r + 4 * h, kInputRecords[rec.input].hash[h]);
                    PPC_STORE_U32(r + 12, names[rec.input]);
                    PPC_STORE_U32(r + 16, rec.flag);
                    PPC_STORE_U32(r + 20, std::bit_cast<uint32_t>(rec.deadzone));
                }
                s_nodes[o][b] = GuestAlloc(16);
                PPC_STORE_U32(s_nodes[o][b] + 4, array);
            }
    }

    int OverrideForCollection(uint32_t key)
    {
        for (size_t o = 0; o < kOverrideCount; o++)
            if (kOverrides[o].collection == key)
                return int(o);
        return -1;
    }

    // Confibs 0 and 1 are types 1, 2, 6 and 7.
    bool TraxOnRb(uint32_t config)
    {
        return config <= 1 && settings::GetBool(settings::Id::InGameTraxSkip);
    }

    bool HudSkipsTraxOnRb(uint32_t hud)
    {
        return s_modernLayouts
            && Load32(hud + kHudInputMap + kInputMapKey) == kHudMapTraxOnRb;
    }

    // The player's joystick settings: +7 stick steering, +8 config (0-4).
    uint32_t JoystickSettings(uint8_t* base, uint32_t player)
    {
        const uint32_t db = PPC_LOAD_U32(region::Addr(kFeDatabase));
        const uint32_t players = db ? PPC_LOAD_U32(db + 0x10) : 0;
        return players ? players + 20 * player + 136 : 0;
    }

    // Runs a controls picture label fill with the type's labels in the
    // table, shown as the stick picture of the config with its RS icons.
    template <typename Fill>
    void WithTypeLabels(uint8_t* base, uint32_t player, Fill fill)
    {
        const uint32_t js = s_modernLayouts ? JoystickSettings(base, player) : 0;
        const uint32_t config = js ? PPC_LOAD_U32(js + 8) : ~0u;
        if (config > 4)
            return fill();
        const uint8_t stick = PPC_LOAD_U8(js + 7);
        const TypeLabels& t = kTypeLabels[stick ? config : config + 5];
        const char* labels[kSlots] = {};
        labels[SlotY] = t.y;
        labels[SlotB] = t.b;
        labels[SlotA] = t.a;
        labels[SlotX] = t.x;
        labels[SlotLB] = t.lb;
        labels[SlotRB] = t.rb;
        labels[SlotRsUp] = t.rsUp;
        labels[SlotRsDown] = t.rsDown;
        labels[SlotLT] = "BUTTON_BRAKE_REVERSE";
        labels[SlotRT] = "BUTTON_THROTTLE";
        if (TraxOnRb(config))
            labels[SlotRB] = "OPT_EATRAX";

        const uint32_t table = region::Addr(kLabelTable);
        const uint32_t column = table + 4 * t.shownAs;
        uint32_t saved[kSlots];
        for (int s = 0; s < kSlots; s++)
        {
            const uint32_t at = column + 20 * s;
            saved[s] = PPC_LOAD_U32(at);
            PPC_STORE_U32(at, labels[s] ? fe::Hash(labels[s]) : PPC_LOAD_U32(table + 20 * s + 4 * config));
        }
        PPC_STORE_U8(js + 7, 1);  // I'm a teapot
        PPC_STORE_U32(js + 8, t.shownAs);
        fill();
        PPC_STORE_U8(js + 7, stick);
        PPC_STORE_U32(js + 8, config);
        for (int s = 0; s < kSlots; s++)
            PPC_STORE_U32(column + 20 * s, saved[s]);
    }
}

// Builds an input map from a controller collection (r5: its key).
SB_HOOK(sub_8239D8F0)
{
    if (s_modernLayouts)
        if (ctx.lr == region::Addr(kHudMapBuildReturn) || ctx.lr == region::Addr(kHudMapBuildReturn2))
            ctx.r5.u64 = TraxOnRb(ctx.r9.u32 / 2) ? kHudMapTraxOnRb : kHudMap;
    t_building = s_modernLayouts ? OverrideForCollection(ctx.r5.u32) : -1;
    if (t_building >= 0)
        BuildOverrideData(base);  // here: the guest heap isn't up at boot
    SB_ORIGINAL(sub_8239D8F0)(ctx, base);
    t_building = -1;
}

// Attrib attribute lookup (r3: the result { .., .., node, single value },
// r5: the attribute key). While an override builds, its actions get our arrays.
SB_HOOK(sub_8214B758)
{
    const uint32_t out = ctx.r3.u32;
    const uint32_t key = ctx.r5.u32;
    SB_ORIGINAL(sub_8214B758)(ctx, base);
    if (t_building < 0)
        return;
    const Override& o = kOverrides[t_building];
    for (size_t b = 0; b < o.count; b++)
    {
        if (o.bindings[b].action != key)
            continue;
        const uint32_t node = s_nodes[t_building][b];
        const uint32_t original = PPC_LOAD_U32(out + 8);
        if (original)  // copy the original node except its data pointer
            for (uint32_t i = 0; i < 16; i += 4)
                if (i != 4)
                    PPC_STORE_U32(node + i, PPC_LOAD_U32(original + i));
        PPC_STORE_U8(node + 0xB, (PPC_LOAD_U8(node + 0xB) & ~0x30) | 0x02);
        PPC_STORE_U32(out + 8, node);
        PPC_STORE_U32(out + 12, 0);
        return;
    }
}

// The controller picture: modern layouts show the stick variant only.
namespace
{
    constexpr uint32_t kX360Stick = 0xEC3C0443;  // LOADING_CONTROLLER_X360, D-pad +1

    uint32_t ControllerPicture(uint32_t picture)
    {
        return s_modernLayouts ? kX360Stick : picture;
    }
}

// The texture hash where the screens pick the picture (config/nfsmw.toml).
void ControllerPictureChosen(PPCRegister& picture)
{
    picture.u64 = ControllerPicture(picture.u32);
}

// ...and the controls screen's getter for it.
SB_HOOK(sub_8223FC18)
{
    SB_ORIGINAL(sub_8223FC18)(ctx, base);
    ctx.r3.u64 = ControllerPicture(ctx.r3.u32);
}

// Controls screen (Options > Controller): the picture labels.
SB_HOOK(sub_8223F708)
{
    const uint32_t screen = ctx.r3.u32;
    WithTypeLabels(base, PPC_LOAD_U32(region::Addr(kControlsPlayer)),
        [&] { SB_ORIGINAL(sub_8223F708)(ctx, base); });
    mods::HideGuideText(ctx, base, screen);
}

// Loading screen's controller picture (player 0).
SB_HOOK(sub_822BA768)
{
    const uint32_t screen = ctx.r3.u32;
    WithTypeLabels(base, 0, [&] { SB_ORIGINAL(sub_822BA768)(ctx, base); });
    mods::HideGuideText(ctx, base, screen);
}

// Skip EA Trax In-Game: Engage Event (D-pad up) plays the next track, in free
// roam when there's no event to engage, in races always; the standings (race
// or pursuit event) move to D-pad Down. Uses the menu next-song function with
// its "in the menus" check passed. Inspired by NFSMWExOpts (SkipTrackAnywhere).
namespace
{
    constexpr uint32_t kNextSong = 0x822862A0;
    constexpr uint32_t kNotifyChyron = 0x82203438;    // shows the EA Trax banner
    constexpr uint32_t kTraxSlot = 0x150;             // the music object's slot ...
    constexpr uint32_t kSlotSongs = 0x82A327C0;       // ... its song: slot * 24 here

    // The banner screen waits 5s in game (ticks at 4000Hz) from its start time.
    constexpr uint32_t kTraxScreenName = 0x8206E47C;  // "EA_Trax.fng"
    constexpr uint32_t kFindScreen = 0x822D7528;
    constexpr uint32_t kScreenStart = 0x2C;
    constexpr uint32_t kTicksNow = 0x82A399AC;
    constexpr uint32_t kBannerWaitTicks = 5 * 4000;

    // Race status: nonzero +0x1A28 in a race (free roam: 0).
    constexpr uint32_t kRaceStatus = 0x82A2CB18;
    constexpr uint32_t kRaceStatusMode = 0x1A28;
    constexpr uint32_t kHudPursuitStandings = 0x2BC;  // Engage shows PostRace_Pursuit
    constexpr uint32_t kActionEngage = 0x2E;
    constexpr uint32_t kActionPadDown = 0x30;
    constexpr uint32_t kActionNextSong = 0x33;

    std::atomic<uint32_t> s_traxSong = 0;
    std::atomic<uint32_t> s_chyronBaseline = 0;
    std::atomic<bool> s_chyronPending = false;

    // What stops Engage Event in pursuits; D-pad up in race checks it too.
    constexpr uint32_t kPursuitCounts[] = { 0x82A2D848, 0x82A2D86C };
    constexpr uint32_t kEngageBlocked = 0x82C5A261;

    bool InRace()
    {
        const uint32_t status = Load32(region::Addr(kRaceStatus));
        return status && Load32(status + kRaceStatusMode) != 0;
    }

    bool InPursuit()
    {
        for (uint32_t count : kPursuitCounts)
            if (int32_t(Load32(region::Addr(count))) > 0)
                return true;
        const void* blocked = g_memory.Translate(region::Addr(kEngageBlocked));
        return *static_cast<const uint8_t*>(blocked) != 0;
    }

    thread_local bool t_skipTrax = false;
    thread_local bool t_skippingTrax = false;
    thread_local bool t_traxOnRb = false;  // the HUD skips on RB, the D-pad stays
}

void EngageAvailabilityChecked(PPCRegister& r8)
{
    if (r8.u32 == 0 && !t_traxOnRb)
        t_skipTrax = true;
}

void HudActionDispatched(PPCRegister& r11, PPCRegister& r28)
{
    if (!settings::GetBool(settings::Id::InGameTraxSkip))
        return;
    t_traxOnRb = HudSkipsTraxOnRb(r28.u32);
    if (r11.u32 == kActionNextSong && t_traxOnRb)
    {
        t_skipTrax = !InPursuit();
        r11.u64 = 0;  // no action
        return;
    }
    if ((r11.u32 != kActionEngage && r11.u32 != kActionPadDown) || t_traxOnRb)
        return;
    const uint8_t* pursuitStandings = static_cast<const uint8_t*>(
        g_memory.Translate(r28.u32 + kHudPursuitStandings));
    if (!InRace() && *pursuitStandings == 0)
        return;
    if (r11.u32 == kActionPadDown)
    {
        r11.u64 = kActionEngage;  // the standings
        return;
    }
    t_skipTrax = !InPursuit();
    r11.u64 = 0;  // no action
}

void NextSongInMenus(PPCRegister& r11)
{
    if (t_skippingTrax)
        r11.u64 = 1;
}

// HUD action handler (pause, Engage Event, D-pad left/right/down).
SB_HOOK(sub_822A9798)
{
    t_skipTrax = false;
    t_traxOnRb = false;
    SB_ORIGINAL(sub_822A9798)(ctx, base);
    const bool skip = t_skipTrax;
    t_skipTrax = false;
    if (!skip || !settings::GetBool(settings::Id::InGameTraxSkip))
        return;
    PPCContext call = ctx;
    t_skippingTrax = true;
    g_memory.FindFunction(region::Addr(kNextSong))(call, base);
    t_skippingTrax = false;
    s_chyronBaseline = s_traxSong.load();
    s_chyronPending = true;
}

// The EA Trax music object update (r3: the object). Once the skip
// happens, shows the banner at once, as LS in the menus does.
SB_HOOK(sub_82202968)
{
    const uint32_t trax = ctx.r3.u32;
    SB_ORIGINAL(sub_82202968)(ctx, base);
    const uint32_t slot = PPC_LOAD_U32(trax + kTraxSlot);
    const uint32_t songs = region::Addr(kSlotSongs);
    const uint32_t song = slot < 64 ? PPC_LOAD_U32(songs + slot * 24) : ~0u;
    s_traxSong = song;
    if (!s_chyronPending || song == s_chyronBaseline)
        return;
    s_chyronPending = false;
    PPCContext call = ctx;
    call.r3.u64 = trax;
    g_memory.FindFunction(region::Addr(kNotifyChyron))(call, base);
    call.r3.u64 = region::Addr(kTraxScreenName);
    g_memory.FindFunction(region::Addr(kFindScreen))(call, base);
    const uint32_t now = PPC_LOAD_U32(region::Addr(kTicksNow));
    if (const uint32_t screen = call.r3.u32)
        PPC_STORE_U32(screen + kScreenStart, now - kBannerWaitTicks);
}

// Max Performance in Career: skips the button's "career mode" check in
// the customize menu's input handler and button setup; the Performance
// screen check stays. Ported from NFSMWExOpts (EnableMaxPerformanceOnShop).
bool MaxPerformanceCareerCheck()
{
    return settings::GetBool(settings::Id::MaxPerformanceOnShop);
}

namespace
{
    constexpr uint32_t kCareerFlags = 300;          // FEDatabase: bit 0 = Career
    constexpr uint32_t kCategory = 332;             // screen: category
    constexpr uint32_t kPerformanceCategory = 0x802;
    constexpr uint32_t kScreenPackage = 16;
    constexpr uint32_t kMaxPerfButton = 0x5AEC8D91;
    constexpr uint32_t kFindObject = 0x8227FD58;    // (package, hash)
    constexpr uint32_t kSetPosition = 0x823018D8;   // (object, xyz*)
    constexpr uint32_t kObjectData = 44;            // object -> data; +28: xyz
    constexpr float kRowsDown = 100.0f;             // Cash/Total row -> below B/A
    uint32_t s_movedObject = 0;
    float s_movedY = 0.0f;
}

// In Career the button sits under the Cash/Total box; move it to its own row
// below the A/B hints. Ported from NFSMWExOpts (EnableMaxPerformanceOnShop).
SB_HOOK(sub_82575C80)
{
    const uint32_t screen = ctx.r3.u32;
    SB_ORIGINAL(sub_82575C80)(ctx, base);
    if (!settings::GetBool(settings::Id::MaxPerformanceOnShop))
        return;
    const uint32_t db = PPC_LOAD_U32(region::Addr(kFeDatabase));
    if (!db || !(PPC_LOAD_U32(db + kCareerFlags) & 1)
        || PPC_LOAD_U32(screen + kCategory) != kPerformanceCategory)
        return;

    PPCContext call = ctx;
    call.r3.u64 = PPC_LOAD_U32(screen + kScreenPackage);
    call.r4.u64 = kMaxPerfButton;
    g_memory.FindFunction(region::Addr(kFindObject))(call, base);
    const uint32_t object = call.r3.u32;
    if (!object)
        return;
    const uint32_t pos = PPC_LOAD_U32(object + kObjectData) + 28;
    const float y = std::bit_cast<float>(uint32_t(PPC_LOAD_U32(pos + 4)));
    if (object == s_movedObject && y == s_movedY)  // setup ran again
        return;
    s_movedObject = object;
    s_movedY = y + kRowsDown;

    // xyz on a fresh stack frame below the caller.
    call = ctx;
    call.r1.u32 = ctx.r1.u32 - 0x80;
    PPC_STORE_U32(call.r1.u32, ctx.r1.u32);  // back chain
    const uint32_t xyz = call.r1.u32 + 0x50;
    PPC_STORE_U32(xyz, PPC_LOAD_U32(pos));
    PPC_STORE_U32(xyz + 4, std::bit_cast<uint32_t>(s_movedY));
    PPC_STORE_U32(xyz + 8, PPC_LOAD_U32(pos + 8));
    call.r3.u64 = object;
    call.r4.u64 = xyz;
    g_memory.FindFunction(region::Addr(kSetPosition))(call, base);
}

// World Map During Events: skips the in-game map key's "event active"
// check (sub_822A9798). Ported from NFSMWExOpts (WorldMapAnywhere).
bool WorldMapEventCheck()
{
    return settings::GetBool(settings::Id::WorldMapAnywhere);
}

// Carbon-Style Race Progress: 1-lap races show percentage. Ported
// from NFSMWExOpts (ShowPercentOn1LapRaces). Race progress text
// (sub_8229FDD8), r3 = race type (0 = no race), r31 = widget.
bool RaceProgressLapText(PPCRegister& r3, PPCRegister& r31)
{
    uint8_t* base = g_memory.base;
    return settings::GetBool(settings::Id::CarbonStyleLapProgress)
        && r3.u32 != 0 && PPC_LOAD_U32(r31.u32 + 60) == 1;  // lap total
}

// Main Menu Quit: B on the main menu (unused there) opens the Yes/No
// dialog with QUIT_GAME; Yes quits as Settings > Quit Game does.
namespace
{
    constexpr uint32_t kMainMenuScreen = 0x82079AAC;  // MainMenu's screen vtable
    constexpr uint32_t kQuitYes = fe::Hash("SB_QUIT_GAME_YES");

    uint32_t s_pressedScreen = 0;  // B was pressed in the main menu root

    void NoteMainMenuBack(uint32_t message, uint32_t package)
    {
        if (message != fe::kPadBack)
            return;
        const uint32_t screen = fe::ScreenOf(package);
        if (fe::IsScreen(screen, region::Addr(kMainMenuScreen)))
            s_pressedScreen = screen;
    }
}

// The main menu's message handler (r3: the screen, r4: the message).
SB_HOOK(sub_82237498)
{
    const uint32_t screen = ctx.r3.u32;
    const uint32_t message = ctx.r4.u32;
    SB_ORIGINAL(sub_82237498)(ctx, base);
    if (screen == s_pressedScreen)
    {
        s_pressedScreen = 0;
        fe::ShowYesNoDialog(ctx, base, screen,
            fe::Hash("QUIT_GAME"), kQuitYes, fe::Hash("SB_QUIT_GAME_NO"));
    }
    else if (message == kQuitYes)
    {
        SDL_Event e{ .type = SDL_EVENT_QUIT };  // SDL stamps the time
        if (SDL_PushEvent(&e))
            fprintf(stderr, "[fixes] Main menu quit: quitting\n");
    }
}

// Hide Xbox Live Entry: the main menu builder (sub_82237900) skips
// the Xbox Live row. Inspired by NFSMWExOpts (ShowOnline).
bool HideXboxLiveEntry()
{
    return settings::GetBool(settings::Id::HideXboxLiveEntry);
}

namespace
{
    // Hide ESRB Disclaimer: the USA build splash screen (sub_822B8500) plays
    // "SHOW" on the "Game Experience May Change During Online Play" notice
    // object; the other builds don't. "On" skips that call on the USA build.
    // Matched by object name hash (FEObject +0x10), not return address: the
    // PAL builds changed the splash code.
    constexpr uint32_t kEsrbNoticeObject = 0xBDE7FA72;
    constexpr uint32_t kShowScriptHash = 0x1CA7C0;  // "SHOW"

    bool HideEsrbNotice(const PPCContext& ctx, uint8_t* base)
    {
        return region::Current() == region::Release::Usa
            && ctx.r4.u32 == kShowScriptHash
            && ctx.r3.u32 != 0
            && PPC_LOAD_U32(ctx.r3.u32 + 0x10) == kEsrbNoticeObject
            && settings::GetBool(settings::Id::HideEsrbDisclaimer);
    }
}

// Plays a script on a screen object (r4: the script's hash).
// Fixes that skip one of its calls check here.
SB_HOOK(sub_8227FF60)
{
    if (HideEsrbNotice(ctx, base))
        return;
    SB_ORIGINAL(sub_8227FF60)(ctx, base);
}

// Show HUD on Look Back: the HUD stays up; the mirror keeps its own
// lookback check. Ported from NFSMWExOpts (ShowHUDOnLookBack).
namespace
{
    // View table, 112 bytes per view. View 1 is player 0 (the first
    // player's) screen; its action list sits at +180 (view 1 + 68).
    constexpr uint32_t kViews = 0x82A38070;
    constexpr uint32_t kActionType = 12, kDriveAction = 1;  // handler type; 1 = driving
    constexpr uint32_t kLookback = 177;                     // byte: holding lookback

    bool ShowHudOnLookback() { return settings::GetBool(settings::Id::ShowHudOnLookback); }

    // Player 0's drive action lookback flag, as the mirror check reads it.
    bool Player0LookingBack(uint8_t* base)
    {
        const uint32_t head = region::Addr(kViews) + 180;
        const uint32_t node = PPC_LOAD_U32(head);
        if (node == head || node == 0)
            return false;
        const uint32_t action = node - 4;
        return PPC_LOAD_U32(action + kActionType) == kDriveAction
            && PPC_LOAD_U8(action + kLookback) != 0;
    }
}

// HUD visibility builder (sub_822A9DC0), r3 = the view's lookback flag.
void HudLookbackRead(PPCRegister& r3)
{
    if (ShowHudOnLookback())
        r3.u64 = 0;
}

// Mirror in Chase Cams: other releases show the mirror only in bumper and hood.
// Hides the 3D mirror and the radar's mirror frame; idle when the mirror is off.
// Reverse of NFSMWExOpts (EnableCameras, the rearview mirror addition).
namespace
{
    constexpr uint32_t kSettingsCamera = 0x94;    // 0 bumper, 1 hood

    bool HideMirrorForCamera(uint8_t* base)
    {
        if (settings::MirrorInChaseCams(settings::GetInt(settings::Id::MirrorInChaseCams))
            != settings::MirrorInChaseCams::Hidden)
            return false;
        const uint32_t db = PPC_LOAD_U32(region::Addr(kFeDatabase));
        const uint32_t player = db ? PPC_LOAD_U32(db + 0x10) : 0;
        return player && PPC_LOAD_U32(player + kSettingsCamera) >= 2;
    }
}

SB_HOOK(sub_822AA818)
{
    SB_ORIGINAL(sub_822AA818)(ctx, base);
    if (ctx.r3.u32 != 0 && HideMirrorForCamera(base))
        ctx.r3.u64 = 0;
}

// Radar update (sub_82297840), r10 = the mirror option. Drops the mirror
// frame when the mirror is hidden (chase cam, or looking back with the HUD up).
void RadarMirrorOptionRead(PPCRegister& r10)
{
    if (r10.u32 == 0)
        return;
    if (HideMirrorForCamera(g_memory.base)
        || (ShowHudOnLookback() && Player0LookingBack(g_memory.base)))
        r10.u64 = 0;
}

// Tips on Loading Screens: tip choice of the loading screen picker.
// 28 = plain Loading.fng (27 = controller picture, else a tip idx).
SB_HOOK(sub_822B9980)
{
    if (settings::TipsOnLoadscreens(settings::GetInt(settings::Id::TipsOnLoadscreens))
        == settings::TipsOnLoadscreens::Hidden)
        ctx.r3.u64 = 28;
    else
        SB_ORIGINAL(sub_822B9980)(ctx, base);
}

// Hide Subtitle Background: hides the background behind the subtitle text.
constexpr uint32_t kSubtitleBackground = 0x10;  // subtitles -> background object
constexpr uint32_t kHideObject = 0x8227FE08;    // FEng hide

// Subtitle message handler (r3 = subtitles), runs every frame of a movie.
SB_HOOK(sub_822877A0)
{
    if (settings::GetBool(settings::Id::HideSubtitleBackground))
    {
        if (uint32_t background = PPC_LOAD_U32(ctx.r3.u32 + kSubtitleBackground))
        {
            PPCContext call = ctx;
            call.r3.u64 = background;
            g_memory.FindFunction(region::Addr(kHideObject))(call, base);
        }
    }
    SB_ORIGINAL(sub_822877A0)(ctx, base);
}

// Subtitles: sub_822874C8 loads a movie's subtitles only when the game language
// differs from the VO or the movie is a tutorial. The hooks (config/nfsmw.toml):
// Force On skips both checks (ported from NFSMWExOpts ShowSubs),
// Force Off the language one, so only tutorials keep them (ported from WFP).
static bool SubtitlesMode(settings::Subtitles mode)
{
    return settings::Subtitles(settings::GetInt(settings::Id::Subtitles)) == mode;
}

bool SubtitlesForceOn()
{
    return SubtitlesMode(settings::Subtitles::ForceOn);
}

bool SubtitlesForceOff()
{
    return SubtitlesMode(settings::Subtitles::ForceOff);
}

// NFS:U2 Loyalty Bonus: at a new Career this function shows the "thanks for
// playing NFS:U2" dialog and adds $10k when its save-found flag is set; on
// the 360 nothing sets it. Inspired by NFSMWExOpts (AwardedCash).
SB_HOOK(sub_8228A790)
{
    constexpr uint32_t kSaveFound = 44;
    constexpr uint32_t kCareerFlags = 0xB0 + 4;  // bit 1: bonus handled
    const uint32_t db = PPC_LOAD_U32(region::Addr(kFeDatabase));
    const uint32_t player = db ? PPC_LOAD_U32(db + 0x10) : 0;
    const uint32_t object = ctx.r3.u32;
    const uint8_t found = PPC_LOAD_U8(object + kSaveFound);
    if (player && !(PPC_LOAD_U32(player + kCareerFlags) & 2)
        && settings::GetBool(settings::Id::Nfsu2LoyaltyBonus))
    {
        PPC_STORE_U8(object + kSaveFound, 1);
        fprintf(stderr, "[fixes] NFSU2 loyalty bonus: $10,000 granted\n");
    }
    SB_ORIGINAL(sub_8228A790)(ctx, base);
    PPC_STORE_U8(object + kSaveFound, found);  // no stale flag for the next Career
}

namespace
{
    // Unlock All Content: the game's debug UnlockAllThings byte;
    // the unlock checks pass while it's set. RAM only, not saved.
    // Ported from NFSMWExOpts (UnlockAllThings).
    constexpr uint32_t kUnlockAllThings = 0x82A2CE00;

    void ApplyUnlockAllThings()
    {
        bool on = settings::GetBool(settings::Id::UnlockAllThings);
        *static_cast<uint8_t*>(g_memory.Translate(region::Addr(kUnlockAllThings))) = on;
        fprintf(stderr, "[fixes] Unlock all content: %s\n", on ? "on" : "off");
    }
}

// Fixes that don't log when they act: announced at boot when
// changed from the default, and whenever they are switched.
namespace
{
    constexpr settings::Id kAnnounced[] = {
        settings::Id::FixLeaderboardRefresh, settings::Id::LongerAliasNames,
        settings::Id::InGameTraxSkip, settings::Id::MaxPerformanceOnShop,
        settings::Id::WorldMapAnywhere, settings::Id::CarbonStyleLapProgress,
        settings::Id::ShowHudOnLookback, settings::Id::MirrorInChaseCams,
        settings::Id::TipsOnLoadscreens, settings::Id::HideSubtitleBackground,
        settings::Id::Subtitles,
    };

    const char* StateText(settings::Id id)
    {
        const settings::Option& o = settings::Info(id);
        if (o.type == settings::Type::Bool)
            return settings::GetBool(id) ? "on" : "off";
        for (const settings::EnumEntry& e : o.entries)
            if (e.value == settings::GetInt(id))
                return e.label;
        return "?";
    }

    void AnnounceFix(settings::Id id)
    {
        fprintf(stderr, "[fixes] %s: %s\n", settings::Info(id).label, StateText(id));
    }
}

namespace fixes
{
    void ApplyAtBoot()
    {
        static double announced[std::size(kAnnounced)];
        for (size_t i = 0; i < std::size(kAnnounced); i++)
        {
            announced[i] = settings::GetValue(kAnnounced[i]);
            if (announced[i] != settings::DefaultValue(kAnnounced[i]))
                AnnounceFix(kAnnounced[i]);
        }
        static const uint32_t announceListener = settings::Subscribe([](settings::Id id) {
            for (size_t i = 0; i < std::size(kAnnounced); i++)
                if (kAnnounced[i] == id && settings::GetValue(id) != announced[i])
                {
                    announced[i] = settings::GetValue(id);
                    AnnounceFix(id);
                }
        });
        (void)announceListener;
        if (settings::GetBool(settings::Id::BlackEditionContent))
        {
            *static_cast<uint8_t*>(g_memory.Translate(region::Addr(kBlackEditionFlag))) = 1;
            fprintf(stderr, "[fixes] Black Edition content: on\n");
        }
        if (settings::GetBool(settings::Id::FixTimebug))
        {
            s_fixTimebug = true;
            fprintf(stderr, "[fixes] Timebug: on\n");
        }
        if (settings::GetBool(settings::Id::FixCopSpoilerDamage))
        {
            auto* map = static_cast<be<uint32_t>*>(g_memory.Translate(region::Addr(kDamageZoneMap)));
            for (const ZoneSlotEntry& e : kCopSpoilerEntries)
            {
                map[e.index * 2] = e.zone;
                map[e.index * 2 + 1] = kCopSpoilerSlot;
            }
            fprintf(stderr, "[fixes] Cop spoiler damage: on\n");
        }
        if (settings::GetBool(settings::Id::ModernGamepadLayouts))
        {
            s_modernLayouts = true;
            fprintf(stderr, "[fixes] Modern gamepad layouts: on\n");
        }
        if (ui::quit::kOffered && settings::GetBool(settings::Id::MainMenuQuit))
        {
            fe::OnUnhandledButton(NoteMainMenuBack);
            fprintf(stderr, "[fixes] Main menu quit: on\n");
        }
        if (settings::GetBool(settings::Id::UnlockAllThings))
            ApplyUnlockAllThings();
        static const uint32_t unlockListener = settings::Subscribe([](settings::Id id) {
            if (id == settings::Id::UnlockAllThings)
                ApplyUnlockAllThings();
        });
        (void)unlockListener;
    }
}
