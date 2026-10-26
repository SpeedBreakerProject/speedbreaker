// Diagnostics from the high-frame-rate (Smooth Motion) work, for testing
// only: nothing here is on unless its variable is set.
//
// NFSMW_HFR_COUNT=<frames>: every <frames> calls of the frame's render entry
// (sub_823AFC68), one "[hfr]" line: how many times each render-side guest
// function ran per render entry and per world step (the stepper's step
// counter, [0x82A2CB6C]+40), the RealTimeFrames advance, and the views set up
// (sub_8243EAC0, by view id).
//
// NFSMW_HFR_RENDER2=1: the frame's render (sub_823AFC68: view setup, shadow
// fit, every pass and the end-of-frame callback with its Present) runs twice
// with the same world state (game/hfr.cpp: the first one's swap is presented
// at once, as an in-between picture's, so the world keeps its 60 steps a
// second at the 60 Hz guest vblank and each step is presented twice).
#include <stdafx.h>
#include <region/region.h>
#include <kernel/function.h>

#include <atomic>
#include <cstdlib>
#include <string>

#include "hfr.h"

namespace
{
    enum Id
    {
        RenderEntry, RenderFrame, ViewSetup, CamDirector, Exposure, FrameGate, FrameGateReset, Present, Flares,
        Emitters, WorldScene, TexAnim, Count
    };
    const char* kNames[Count] = { "render-entry 823AFC68", "render-frame 82441050", "view-setup 8243EAC0",
        "camera-director 82168028", "exposure-adapt 82223220", "frame-gate 8243B0B0", "frame-gate-reset",
        "present 825987E0", "RenderFlares 824E6220", "emitter-render 8222ADC0", "world-scene 82443088",
        "tex-anim 822206C0" };

    const uint32_t s_every = [] {
        const char* v = std::getenv("NFSMW_HFR_COUNT");
        return v ? uint32_t(std::max(0, std::atoi(v))) : 0u;
    }();
    std::atomic<uint64_t> s_n[Count];
    std::atomic<uint64_t> s_views[32];

    void Bump(Id id)
    {
        if (s_every)
            s_n[id].fetch_add(1, std::memory_order_relaxed);
    }

    // Main loop thread only.
    struct Last
    {
        uint64_t n[Count] = {};
        uint64_t views[32] = {};
        uint32_t steps = 0, rtf = 0;
        bool valid = false;
    } s_last;

    void Report(uint8_t* base)
    {
        const uint32_t stepper = PPC_LOAD_U32(0x82A2CB6C);
        const uint32_t steps = stepper ? PPC_LOAD_U32(stepper + 40) : 0;
        const uint32_t rtf = PPC_LOAD_U32(0x828FD86C);
        uint64_t n[Count], views[32];
        for (int i = 0; i < Count; i++)
            n[i] = s_n[i].load(std::memory_order_relaxed);
        for (int i = 0; i < 32; i++)
            views[i] = s_views[i].load(std::memory_order_relaxed);
        if (s_last.valid)
        {
            const double entries = double(n[RenderEntry] - s_last.n[RenderEntry]);
            const int64_t dSteps = int64_t(steps) - int64_t(s_last.steps);
            std::string line = std::format("[hfr] {:.0f} render entries, {} world steps, RTF +{}:", entries, dSteps,
                int64_t(rtf) - int64_t(s_last.rtf));
            for (int i = 1; i < Count; i++)
            {
                const double d = double(n[i] - s_last.n[i]);
                line += std::format(" | {} {:.2f}/entry", kNames[i], entries > 0 ? d / entries : 0.0);
                if (dSteps > 0)
                    line += std::format(" {:.2f}/step", d / double(dSteps));
            }
            line += " | views/entry:";
            for (int i = 0; i < 32; i++)
                if (views[i] != s_last.views[i])
                    line += std::format(" {}:{:.2f}", i, entries > 0 ? double(views[i] - s_last.views[i]) / entries : 0.0);
            fprintf(stderr, "%s\n", line.c_str());
        }
        for (int i = 0; i < Count; i++)
            s_last.n[i] = n[i];
        for (int i = 0; i < 32; i++)
            s_last.views[i] = views[i];
        s_last.steps = steps;
        s_last.rtf = rtf;
        s_last.valid = true;
    }
}

// The frame's render entry (sub_823AFC68, overridden in game/hfr.cpp): once per call of it, after its renders.
void hfrdiag::AfterRenderEntry(uint8_t* base)
{
    Bump(RenderEntry);
    if (s_every && s_n[RenderEntry].load(std::memory_order_relaxed) % s_every == 0)
        Report(base);
}

#define HFR_COUNT(sub, id)                \
    SB_HOOK(sub)                          \
    {                                     \
        Bump(id);                         \
        SB_ORIGINAL(sub)(ctx, base);      \
    }

HFR_COUNT(sub_82441050, RenderFrame)
HFR_COUNT(sub_82168028, CamDirector)
// NFSMW_HFR_EXPOSURE_LOG=<from s>,<count>: each eye adaptation (sub_82223220, on the render thread when it plays the
// end-of-frame pass): adapted before and after, measured, and whether it was the in-between picture's.
namespace
{
    std::atomic<int> s_exposureLeft = [] {
        const char* v = std::getenv("NFSMW_HFR_EXPOSURE_LOG");
        const char* c = v ? std::strchr(v, ',') : nullptr;
        return v ? (c ? std::atoi(c + 1) : 120) : 0;
    }();
    const double s_exposureFrom = [] { const char* v = std::getenv("NFSMW_HFR_EXPOSURE_LOG"); return v ? std::atof(v) : 0.0; }();
    const auto s_exposureT0 = std::chrono::steady_clock::now();
    float LoadF(uint8_t* base, uint32_t a)
    {
        const uint32_t bits = PPC_LOAD_U32(a);
        float f;
        memcpy(&f, &bits, 4);
        return f;
    }
}
extern "C" PPC_FUNC(__imp__sub_82223220);
SB_HOOK(sub_82223220)
{
    Bump(Exposure);
    const bool log = s_exposureLeft.load() > 0 &&
        std::chrono::duration<double>(std::chrono::steady_clock::now() - s_exposureT0).count() >= s_exposureFrom;
    const float before = LoadF(base, 0x82908AB8), measured = LoadF(base, 0x82908ABC);
    const bool between = hfr::InBetween();
    SB_ORIGINAL(sub_82223220)(ctx, base);
    if (log && s_exposureLeft.fetch_sub(1) > 0)
        fprintf(stderr, "[hfrexpo] %s adapted %.6f -> %.6f measured %.6f\n", between ? "between" : "real", before,
            LoadF(base, 0x82908AB8), measured);
}
HFR_COUNT(sub_825987E0, Present)
HFR_COUNT(sub_824E6220, Flares)
HFR_COUNT(sub_8222ADC0, Emitters)
HFR_COUNT(sub_82443088, WorldScene)
HFR_COUNT(sub_822206C0, TexAnim)

// NFSMW_HFR_LOGVIEW=<from s>,<count>: from <from s> after launch, <count> lines: each setup of view 1 (the player's) with
// the stepper's step counter, RealTimeFrames, the render entries so far, and its camera's matrix (view+64 -> camera, 16
// floats; the layout as an earlier diagnostic logged it).
namespace
{
    struct ViewLog
    {
        double fromS = -1;
        int left = 0;
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        ViewLog()
        {
            if (const char* v = std::getenv("NFSMW_HFR_LOGVIEW"))
            {
                fromS = std::atof(v);
                const char* c = std::strchr(v, ',');
                left = c ? std::atoi(c + 1) : 40;
            }
        }
    } s_viewLog;

    void LogView(uint8_t* base, uint32_t view)
    {
        if (s_viewLog.left <= 0 || std::chrono::duration<double>(std::chrono::steady_clock::now() - s_viewLog.t0).count() < s_viewLog.fromS)
            return;
        if (PPC_LOAD_U32(view + 4) != 1)
            return;
        s_viewLog.left--;
        const uint32_t cam = PPC_LOAD_U32(view + 64), stepper = PPC_LOAD_U32(0x82A2CB6C);
        std::string m;
        for (int i = 0; cam && i < 16; i++)
        {
            const uint32_t bits = PPC_LOAD_U32(cam + i * 4);
            float f;
            memcpy(&f, &bits, 4);
            m += std::format("{}{:.6g}", i ? "," : "", f);
        }
        // The player's car (item 0 of the vehicle render list at 0x82C84D38): its matrix's position row as drawn.
        std::string car = "-";
        const uint32_t items = PPC_LOAD_U32(0x82C84D3C), count = PPC_LOAD_U32(0x82C84D44);
        if (items && count)
            if (const uint32_t object = PPC_LOAD_U32(items))
            {
                float p[3];
                for (int i = 0; i < 3; i++)
                {
                    const uint32_t bits = PPC_LOAD_U32(object + 816 + 48 + i * 4);
                    memcpy(&p[i], &bits, 4);
                }
                car = std::format("{:08X} {:.4f},{:.4f},{:.4f}", object, p[0], p[1], p[2]);
            }
        fprintf(stderr, "[hfrview] steps %u rtf %u entry %llu %s fov %u cam %08X M %s car %s\n", stepper ? PPC_LOAD_U32(stepper + 40) : 0,
            PPC_LOAD_U32(0x828FD86C), (unsigned long long)s_n[RenderEntry].load(), hfr::InBetween() ? "between" : "real",
            cam ? unsigned(PPC_LOAD_U16(cam + 196)) : 0u, cam, m.c_str(), car.c_str());
    }
}

// A view setup (sub_8243EAC0, overridden in game/hfr.cpp), with the camera as it will be used.
void hfrdiag::OnViewSetup(uint8_t* base, uint32_t view)
{
    if (s_every)
    {
        Bump(ViewSetup);
        const uint32_t id = PPC_LOAD_U32(view + 4);
        if (id < 32)
            s_views[id].fetch_add(1, std::memory_order_relaxed);
    }
    LogView(base, view);
}

extern "C" PPC_FUNC(__imp__sub_8243B0B0);
SB_HOOK(sub_8243B0B0)
{
    Bump(FrameGate);
    SB_ORIGINAL(sub_8243B0B0)(ctx, base);
    if (ctx.r3.u32 == 1)
        Bump(FrameGateReset);
}
