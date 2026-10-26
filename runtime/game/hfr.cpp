// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// HFR (v018-hfr; path A of the 120 fps study: v015/hfr-scoping, v017/hfr-spike
// and v018/hfr in the work notes). NFSMW_HFR=1: the world keeps its 60 steps
// a second and its 60 Hz guest vblank, and each step is drawn as many times as
// the display shows pictures between two guest vblanks
// (video::HfrPicturesPerStep: 2 at 120 Hz), first at fractions of the way
// from the last step to this one, then as it is. The in-between pictures'
// swaps wait for no guest vblank (as with D3DPRESENT_INTERVAL_IMMEDIATE), and
// the presenter shows each at its share of the vblank before the real one.
// The world never sees an in-between pose.
//
// The frame (sub_823AFE20) steps the world, updates the cameras (the camera
// director, sub_82168028) and the cars' render copies (sub_824E9C38 copies the
// body's matrix to the render object's +816), then renders (sub_823AFC68 ->
// sub_82441050). For an in-between picture at fraction t:
//   - each car's +816 is blended t of the way from its value before this
//     frame's update to its value after it; each source camera's view matrix
//     (and FOV) from the one the last real picture used to its current one.
//     Cameras the render derives from those (the sun-shadow views, the
//     reflections, the environment views) follow by themselves;
//   - what the render changes every time it runs is put back afterwards, so
//     the real picture is drawn exactly as it is at 60: the per-frame render
//     flags (the particle buffer's gate keyed on RealTimeFrames, sub_8243B0B0:
//     else the real picture draws no particles), the eye adaptation
//     (sub_82223220 moves 2% per call) and its luminance ping-pong, the
//     cameras' own history (sub_82161000 keeps the last call's pose and
//     derives velocity from it), the camera-update counter, the render's clock,
//     and the nitrous-trail rings (RenderFlares writes one per render);
//   - the game renders on two threads ([0x828FBB50] set, as in races and
//     cutscenes): this one records each pass as a callback, and the XEX's
//     entry thread plays them back (sub_82441B60 -> sub_823C8290) while this
//     one goes on (the next frame waits in sub_82441DB0 until [0x82A2CF40],
//     "a frame being played", clears; this thread sets it as it starts a
//     frame, sub_82441EF0). The passes read live state when they are played,
//     and the end-of-frame pass adapts the exposure and presents, so the
//     in-between picture's state is put back only once it has been played:
//     this thread waits for it, as the game's next frame would.
// Everything is put back before the real picture's render starts. A frame
// that runs no world step (or several: a hitch) draws no in-between picture.
#include <stdafx.h>
#include <region/region.h>
#include "hfr.h"
#include "hfr_late_hold.h"
#include "hfr_blend.h"
#include "hfr_grid.h"
#include "frame_time.h"

#include <kernel/function.h>
#include <kernel/write_watch.h>
#include <user/settings.h>
#include <ui/ui.h>
#include <cpu/guest_time.h>
#include <video/presenter.h>
#include <video/vblank_lock.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <iterator>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    // Settings > Display > Smooth Motion (or NFSMW_HFR=1), as it was at
    // launch: hfr::Init, after the settings load and before the game runs.
    bool s_enabled = false;
    bool s_render2 = false;
    // NFSMW_HFR_PICTURES=<n> (2-4): that many pictures a step whatever the
    // display (tests: headless, or a display the presenter can't time).
    const int s_forcedPictures = [] {
        const char* v = std::getenv("NFSMW_HFR_PICTURES");
        const int n = v ? std::atoi(v) : 0;
        return n >= 2 && n <= 4 ? n : 0;
    }();
    // NFSMW_HFR_FRACTIONS="<f>,<f>|<f>|...": the fractions each step draws,
    // step after step in turn, the last of a step its own picture (1: the
    // real one; less: a step drawn only between, as on a display that isn't
    // a whole multiple of 60 Hz: "0.667|0.333,1" is 90 Hz's three pictures
    // every two steps). DIAGNOSTIC, any display.
    const std::vector<std::vector<float>> s_forcedFractions = [] {
        std::vector<std::vector<float>> out;
        if (const char* v = std::getenv("NFSMW_HFR_FRACTIONS"))
        {
            std::vector<float> step;
            const char* p = v;
            while (*p)
            {
                char* end = nullptr;
                const float f = std::strtof(p, &end);
                if (end == p)
                    break;
                if (f > 0.0f && f <= 1.0f && step.size() < 4)
                    step.push_back(f);
                p = end;
                if (*p == ',')
                    p++;
                else if (*p == '|' || !*p)
                {
                    if (!step.empty())
                        out.push_back(step);
                    step.clear();
                    if (*p)
                        p++;
                }
            }
            if (!step.empty())
                out.push_back(step);
            fprintf(stderr, "[hfr] NFSMW_HFR_FRACTIONS: %zu steps in turn (DIAGNOSTIC)\n", out.size());
        }
        return out;
    }();
    uint32_t s_fractionCycle = 0;
    // NFSMW_HFR_BLEND=0: the in-between picture without the blending (the per-render state still put back);
    // =cars, =cameras or =particles blends only those (DIAGNOSTIC: M1's launch A/B, M2's particles).
    const char* const s_blendMode = std::getenv("NFSMW_HFR_BLEND");
    const bool s_blendAll = !s_blendMode || std::strcmp(s_blendMode, "1") == 0;
    const bool s_blendCars = s_blendAll || (s_blendMode && std::strcmp(s_blendMode, "cars") == 0);
    const bool s_blendCameras = s_blendAll || (s_blendMode && std::strcmp(s_blendMode, "cameras") == 0);
    const bool s_blendParticles = s_blendAll || (s_blendMode && std::strcmp(s_blendMode, "particles") == 0);
    const bool s_blend = s_blendCars || s_blendCameras || s_blendParticles;

    std::atomic<uint64_t> s_staleLuminance{ 0 };  // real pictures that took the last luminance reading (M3)
    const bool s_vbSlot = [] { const char* v = std::getenv("NFSMW_HFR_VBSLOT"); return v && v[0] == '1'; }();
    // Set by the main loop thread around an in-between picture; read by the
    // render thread too (the picture's swap: sub_82598670 here, and VdSwap).
    std::atomic<bool> s_inBetween{ false };
    std::atomic<float> s_fraction{ 0.0f };
    // A step whose own picture is drawn between too (its fraction < 1: no
    // real picture), while it is drawn; 0 otherwise.
    std::atomic<float> s_ownFraction{ 0.0f };
    // On a grid that isn't a multiple of 60 Hz (video::HfrGrid): the step's
    // pictures' refreshes (steady ns), and the one being drawn (its swap
    // carries it to the presenter); 0 elsewhere.
    int64_t s_targets[4] = {};
    double s_gridFrom = 0;  // when the last step's state is shown (main loop thread)
    std::atomic<int64_t> s_target{ 0 };
    // How long after the guest vblank that releases a step its last picture
    // is shown there: a step more than the vblank (pictures of a step are
    // drawn after it, and the earliest falls just after the last step's) and
    // the time to draw them.
    constexpr int64_t kGridLeadNs = 5'000'000;
    // Main loop thread only.
    uint32_t s_step = 0;
    uint32_t s_lastRealStep = 0;  // the step the last real picture showed
    uint32_t s_threadedFrames = 0;  // frames since the render thread took the passes (0: not now)
    bool s_lastDrewBetween = false; // the last frame drew in-between pictures
    uint32_t s_steadyFrames = 0;    // drawn frames of exactly one world step in a row
    uint64_t s_lastFrames = 0, s_lastMultiStep = 0, s_lastNoStep = 0;  // frame_time.cpp's counts at the last drawn frame
    uint64_t s_frameCount = 0;      // render entries that rendered (NFSMW_HFR_WRITESET's frames)
    int s_writeSet = 0;             // this frame's NFSMW_HFR_WRITESET phase

    constexpr uint32_t kStepper = 0x82A2CB6C;            // the world stepper; +40: steps run
    constexpr uint32_t kRenderThreaded = 0x828FBB50;     // nonzero: passes played on the render thread
    constexpr uint32_t kFramePlaying = 0x82A2CF40;       // 1: the render thread is playing a frame
    constexpr uint32_t kRenderSkip = 0x82A2CC08;         // > 0: the render entry skips (and counts down)
    constexpr uint32_t kCarMatrix = 816;                 // vehicle render object: the body's matrix as drawn
    constexpr uint32_t kCarInfo = 68;                    // -> its CarRenderInfo
    constexpr uint32_t kTrailRing = 4512, kTrailRingBytes = 248;  // RenderFlares' 3 matrices, 3 positions, index
    constexpr uint32_t kCameraFov = 196;                 // camera object: FOV (u16)
    constexpr uint32_t kCameraBytes = 680;               // what sub_82161000 keeps (its dt at +676)
    constexpr uint32_t kDevicePresentInterval = 13772;   // D3D device: 1, or 0x80000000 (immediate)

    void Load(uint8_t* base, uint32_t a, float m[16])
    {
        for (int i = 0; i < 16; i++)
        {
            const uint32_t bits = PPC_LOAD_U32(a + i * 4);
            memcpy(&m[i], &bits, 4);
        }
    }
    void Store(uint8_t* base, uint32_t a, const float m[16])
    {
        for (int i = 0; i < 16; i++)
        {
            uint32_t bits;
            memcpy(&bits, &m[i], 4);
            PPC_STORE_U32(a + i * 4, bits);
        }
    }
    uint32_t Step(uint8_t* base)
    {
        const uint32_t stepper = PPC_LOAD_U32(kStepper);
        return stepper ? PPC_LOAD_U32(stepper + 40) : 0;
    }

    // Guest memory put back after the in-between picture (newest last).
    struct Saved
    {
        uint32_t address;
        std::vector<uint8_t> bytes;
    };
    std::vector<Saved> s_saved;
    std::unordered_set<uint32_t> s_savedCameras;
    void Save(uint8_t* base, uint32_t address, uint32_t size)
    {
        s_saved.push_back({ address, std::vector<uint8_t>(base + address, base + address + size) });
    }
    void RestoreAll(uint8_t* base)
    {
        for (auto it = s_saved.rbegin(); it != s_saved.rend(); ++it)
            memcpy(base + it->address, it->bytes.data(), it->bytes.size());
        s_saved.clear();
        s_savedCameras.clear();
    }

    // A step's own picture drawn between (BeginOwn/EndOwn): the poses
    // blended as for an in-between picture and put back once it has been
    // played, but what the render changes every time it runs kept, as the
    // real picture's would be. A camera's own history (sub_82161000 copies
    // its state, +0-223, to +224 at each render) is left as the real
    // picture would leave it, so the next step's motion is real to real, and
    // the cameras' records (the next in-between picture's start) are the
    // step's real poses.
    struct OwnCamera
    {
        uint32_t camera;
        uint8_t state[224];
    };
    std::vector<Saved> s_ownSaved;
    std::vector<OwnCamera> s_ownCameras;
    // A particle's position in a step's own picture drawn between: the step's
    // and the blended one written for the picture. The render moves some
    // particles itself (the safe house's birds, 2026-10-25: put back to the
    // step's position after every own picture, they hung in the air at 90 Hz,
    // where nearly every step's own picture is drawn between): what it moved
    // keeps its move, from the step's position.
    struct OwnParticle
    {
        uint32_t address;
        float now[3], blended[3];
    };
    std::vector<OwnParticle> s_ownParticles;
    uint64_t s_ownParticlesMoved = 0, s_ownParticlesKept = 0;  // for the [hfr] line

    // A car's render copy: before and after this frame's update.
    struct Car
    {
        float before[16], after[16];
        uint32_t step = 0;
    };
    std::unordered_map<uint32_t, Car> s_cars;  // vehicle render object -> its poses
    // A camera's view matrix and FOV as the last real picture used them.
    struct Camera
    {
        float m[16];
        uint16_t fov = 0;
        uint32_t step = 0;
    };
    std::unordered_map<uint32_t, Camera> s_cameras;

    // The emitter system's particles (smoke, dust, sparks: the emitter render
    // sub_8222ADC0 draws them from here): [0x82A34940] > 0 while any exist;
    // 66 lists (+268, 8 bytes each: a circular list's head) of nodes whose
    // +8 is an emitter, whose particles hang off its +128 (circular, next at
    // +0), each with its position at +80. The world moves them once a step;
    // an in-between picture draws each t of the way from where it was at the
    // last step, as the cars (a particle not seen then, or one that jumped,
    // a reused slot, is drawn where it is).
    constexpr uint32_t kEmitterSystem = 0x82A34940, kEmitterLists = 66, kEmitterListHeads = 268;
    constexpr uint32_t kEmitterParticles = 128, kParticlePosition = 80;
    constexpr float kParticleMaxMove = 5.0f;
    struct Particle
    {
        float pos[3];
        uint32_t step = 0;
    };
    std::unordered_map<uint32_t, Particle> s_particles;  // particle -> its position at a step
    struct ParticleNow
    {
        uint32_t address;
        float now[3], before[3];
        bool blend;
    };
    std::vector<ParticleNow> s_particlesNow;  // this frame's (main loop thread)

    template <typename F>
    void ForEachParticle(uint8_t* base, F&& fn)
    {
        if (int32_t(PPC_LOAD_U32(kEmitterSystem)) <= 0)
            return;
        uint32_t guard = 0;
        for (uint32_t i = 0; i < kEmitterLists; i++)
        {
            const uint32_t head = kEmitterSystem + kEmitterListHeads + i * 8;
            for (uint32_t node = PPC_LOAD_U32(head); node && node != head && guard < 200000; node = PPC_LOAD_U32(node), guard++)
            {
                const uint32_t emitter = PPC_LOAD_U32(node + 8);
                if (!emitter)
                    continue;
                const uint32_t list = emitter + kEmitterParticles;
                for (uint32_t p = PPC_LOAD_U32(list); p && p != list && guard < 200000; p = PPC_LOAD_U32(p), guard++)
                    fn(p);
            }
        }
    }
    void LoadVec(uint8_t* base, uint32_t a, float v[3])
    {
        for (int i = 0; i < 3; i++)
        {
            const uint32_t bits = PPC_LOAD_U32(a + i * 4);
            memcpy(&v[i], &bits, 4);
        }
    }
    void StoreVec(uint8_t* base, uint32_t a, const float v[3])
    {
        for (int i = 0; i < 3; i++)
        {
            uint32_t bits;
            memcpy(&bits, &v[i], 4);
            PPC_STORE_U32(a + i * 4, bits);
        }
    }
    // Render entry (main loop thread, the world's step done): this step's
    // particles against the last step's, and the last step's for next time.
    void TrackParticles(uint8_t* base, uint32_t step)
    {
        s_particlesNow.clear();
        ForEachParticle(base, [&](uint32_t p) {
            ParticleNow n{ p, {}, {}, false };
            LoadVec(base, p + kParticlePosition, n.now);
            Particle& last = s_particles[p];
            if (last.step + 1 == step)
            {
                const float dx = n.now[0] - last.pos[0], dy = n.now[1] - last.pos[1], dz = n.now[2] - last.pos[2];
                if (dx * dx + dy * dy + dz * dz < kParticleMaxMove * kParticleMaxMove)
                {
                    memcpy(n.before, last.pos, sizeof(n.before));
                    n.blend = true;
                }
            }
            memcpy(last.pos, n.now, sizeof(last.pos));
            last.step = step;
            s_particlesNow.push_back(n);
        });
    }

    // NFSMW_HFR_TIMELINE=<frame>,<frames> (DIAGNOSTIC): for that many frames
    // from that one, a [hfrtime] line with when the frame's steps happened,
    // in ms after the guest vblank before the frame's render entry: the
    // entry, the last real picture played (the wait before), the in-between
    // picture recorded, played (the wait after), the real one recorded. The
    // command processor's swaps get lines of their own (hfr::NoteSwap).
    struct Timeline
    {
        uint64_t from = 0, frames = 0;
    };
    const Timeline s_timeline = [] {
        Timeline t;
        if (const char* v = std::getenv("NFSMW_HFR_TIMELINE"))
        {
            unsigned long long a = 0, b = 0;
            if (sscanf(v, "%llu,%llu", &a, &b) == 2)
                t = { a, b };
        }
        return t;
    }();
    std::atomic<bool> s_timelineOn{ false };
    double SinceVblankMs(int64_t vblank)
    {
        const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        return double(now - vblank) / 1e6;
    }

    // Stats for the [hfr] line (since the last).
    struct Stats
    {
        uint64_t frames = 0, skippedNoStep = 0, skippedSteps = 0, skippedStart = 0, skippedLoad = 0, particles = 0, particlesBlended = 0, settles = 0;
        uint64_t ownPictures = 0;  // steps whose own picture was drawn between
        uint64_t pictures = 0, threaded = 0, cars = 0, carCuts = 0, cameras = 0, cameraCuts = 0, waitTimeouts = 0;
        double waitMs = 0.0, waitMaxMs = 0.0, waitBeforeMs = 0.0;
    } s_stats;

    // Limits on one step's motion beyond which it isn't blended (a cut, a reset): 200 mph is 1.5 m a step.
    constexpr float kCarMaxMove = 8.0f, kCarMaxTurn = 45.0f;
    constexpr float kCameraMaxMove = 8.0f, kCameraMaxTurn = 30.0f;

    // Wait for the render thread to have played the frame it is playing (its
    // passes, its end-of-frame pass and Present included), as sub_82441DB0
    // does before the next frame's passes are recorded. Before the in-between
    // picture: the last real one may still be playing, and its end-of-frame
    // pass adapts the exposure (and its passes read the cars' poses) after
    // this thread moves on; saving or blending before it has finished put the
    // real pictures one adaptation behind the in-between ones (the whole
    // picture 1-3 levels darker every other frame, run h3).
    void WaitPlayed(uint8_t* base, bool before)
    {
        if (!PPC_LOAD_U32(kRenderThreaded))
            return;
        if (!before)
            s_stats.threaded++;
        // As long as the game's own next frame would wait (a first frame's
        // pipelines can take seconds to build with a cold shader cache: M1's
        // 250 ms cap put the state back while the picture was still playing).
        // The cap only keeps a wedged render thread from hanging this one.
        const auto t0 = std::chrono::steady_clock::now();
        bool said = false;
        for (int spins = 0; PPC_LOAD_U32(kFramePlaying) == 1; spins++)
        {
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (ms > 250.0 && !said)
            {
                said = true;
                static int lines = 0;
                if (lines++ < 8)
                    fprintf(stderr, "[hfr] render thread still playing after 250 ms (%s the in-between): recorded %u, played %u, "
                                    "queue %u, end %u\n", before ? "before" : "after", PPC_LOAD_U32(0x82909650), PPC_LOAD_U32(0x82909654),
                        PPC_LOAD_U32(0x8290965C), unsigned(PPC_LOAD_U8(0x82909658)));
            }
            if (ms > 10'000.0)
            {
                s_stats.waitTimeouts++;
                fprintf(stderr, "[hfr] render thread still playing after 10 s: going on without it\n");
                break;
            }
            if (spins < 64)
                std::this_thread::yield();
            else
                std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        (before ? s_stats.waitBeforeMs : s_stats.waitMs) += ms;
        if (!before)
            s_stats.waitMaxMs = std::max(s_stats.waitMaxMs, ms);
    }

    void BeginInBetween(uint8_t* base, float t)
    {
        WaitPlayed(base, true);
        if (s_writeSet == 1)
            hfrdiag::WriteSetBeforeBetween(base);
        s_saved.clear();
        s_savedCameras.clear();
        // The dynamic vertex buffer manager at 0x82909730 (sub_82446830: 3 slots,
        // +0 the slot, then its write state; +0x40-0x5F the per-frame gate
        // the particles' fill keys on RealTimeFrames, sub_8243B0B0, sub_8243B100,
        // sub_82447450: else the real picture draws no particles). M2 put all of
        // it back, so the real picture rewrote the slot the in-between picture's
        // draws were still reading (the write guard stalled sub_82445198 a
        // picture, 3.8 ms each: M2's run gd2). NFSMW_HFR_VBSLOT=1 (M3) puts back
        // only the gate: the in-between picture takes a slot of its own.
        if (s_vbSlot)
            Save(base, 0x82909770, 0x20);
        else
            Save(base, 0x82909730, 0x60);
        // The eye adaptation (sub_82223220: adapted, measured at +0 and +4)
        // and the post-processing state after it, which a render writes too
        // (M2's write set ws1: 0x82908AC1-AF7 and 0x82908B42-B97 left changed
        // by the in-between picture).
        Save(base, 0x82908AB8, 0xE8);
        Save(base, 0x82A2C480, 0x50);  // its luminance targets' ping-pong (sub_822234F0)
        Save(base, 0x82A2D5E0, 4);     // the render's clock (sub_823AFC68)
        Save(base, 0x82C5E348, 4);     // the camera-update counter (sub_82161000)
        s_stats.pictures++;
        if (s_blend)
        {
            for (auto& [object, car] : s_cars)
            {
                if (!s_blendCars || car.step != s_step)
                    continue;  // not updated this frame (gone, or no world step)
                float now[16], blended[16];
                Load(base, object + kCarMatrix, now);
                if (hfr::blend::WorldCut(car.before, now, kCarMaxMove, kCarMaxTurn) || !hfr::blend::World(car.before, now, t, blended))
                {
                    s_stats.carCuts++;
                    continue;
                }
                Save(base, object + kCarMatrix, 64);
                Store(base, object + kCarMatrix, blended);
                s_stats.cars++;
            }
            for (auto& [camera, last] : s_cameras)
            {
                if (!s_blendCameras || last.step >= s_step || s_step - last.step > 2)
                    continue;  // no step since (paused), or too old to be the last picture's
                float now[16], blended[16];
                Load(base, camera, now);
                if (!memcmp(now, last.m, sizeof(now)))
                    continue;
                if (hfr::blend::ViewCut(last.m, now, kCameraMaxMove, kCameraMaxTurn) || !hfr::blend::View(last.m, now, t, blended))
                {
                    s_stats.cameraCuts++;
                    continue;
                }
                Save(base, camera, kCameraBytes);
                s_savedCameras.insert(camera);
                Store(base, camera, blended);
                const uint16_t fov = PPC_LOAD_U16(camera + kCameraFov);
                if (last.fov && fov && std::abs(int(fov) - int(last.fov)) < 4096)
                    PPC_STORE_U16(camera + kCameraFov, uint16_t(std::lround(float(last.fov) + t * float(int(fov) - int(last.fov)))));
                s_stats.cameras++;
            }
        }
        if (s_blendParticles)
            for (const ParticleNow& n : s_particlesNow)
            {
                s_stats.particles++;
                if (!n.blend)
                    continue;
                float blended[3];
                for (int i = 0; i < 3; i++)
                    blended[i] = n.before[i] + t * (n.now[i] - n.before[i]);
                Save(base, n.address + kParticlePosition, 12);
                StoreVec(base, n.address + kParticlePosition, blended);
                s_stats.particlesBlended++;
            }
        // RenderFlares writes each car's trail ring once a render.
        for (auto& [object, car] : s_cars)
            if (car.step == s_step)
                if (const uint32_t info = PPC_LOAD_U32(object + kCarInfo))
                    Save(base, info + kTrailRing, kTrailRingBytes);
        s_fraction.store(t, std::memory_order_relaxed);
        s_inBetween.store(true, std::memory_order_release);
    }

    void EndInBetween(uint8_t* base)
    {
        WaitPlayed(base, false);
        RestoreAll(base);
        s_inBetween.store(false, std::memory_order_release);
        s_fraction.store(0.0f, std::memory_order_relaxed);
    }

    void BeginOwn(uint8_t* base, float t)
    {
        WaitPlayed(base, true);
        s_ownSaved.clear();
        s_ownCameras.clear();
        s_ownParticles.clear();
        auto save = [&](uint32_t address, uint32_t size) {
            s_ownSaved.push_back({ address, std::vector<uint8_t>(base + address, base + address + size) });
        };
        if (s_blendCars)
            for (auto& [object, car] : s_cars)
            {
                if (car.step != s_step)
                    continue;
                float now[16], blended[16];
                Load(base, object + kCarMatrix, now);
                if (hfr::blend::WorldCut(car.before, now, kCarMaxMove, kCarMaxTurn) || !hfr::blend::World(car.before, now, t, blended))
                    continue;
                save(object + kCarMatrix, 64);
                Store(base, object + kCarMatrix, blended);
            }
        if (s_blendCameras)
            for (auto& [camera, last] : s_cameras)
            {
                if (last.step >= s_step || s_step - last.step > 2)
                    continue;
                float now[16], blended[16];
                Load(base, camera, now);
                if (!memcmp(now, last.m, sizeof(now)))
                    continue;
                if (hfr::blend::ViewCut(last.m, now, kCameraMaxMove, kCameraMaxTurn) || !hfr::blend::View(last.m, now, t, blended))
                    continue;
                OwnCamera c{ camera, {} };
                memcpy(c.state, base + camera, sizeof(c.state));
                s_ownCameras.push_back(c);
                const uint16_t fov = PPC_LOAD_U16(camera + kCameraFov);
                Store(base, camera, blended);
                if (last.fov && fov && std::abs(int(fov) - int(last.fov)) < 4096)
                    PPC_STORE_U16(camera + kCameraFov, uint16_t(std::lround(float(last.fov) + t * float(int(fov) - int(last.fov)))));
                // The next in-between picture starts from this step's real pose.
                memcpy(last.m, now, sizeof(now));
                last.fov = fov;
                last.step = s_step;
            }
        if (s_blendParticles)
            for (const ParticleNow& n : s_particlesNow)
            {
                if (!n.blend)
                    continue;
                float blended[3];
                for (int i = 0; i < 3; i++)
                    blended[i] = n.before[i] + t * (n.now[i] - n.before[i]);
                OwnParticle o{ n.address, {}, {} };
                memcpy(o.now, n.now, sizeof(o.now));
                memcpy(o.blended, blended, sizeof(o.blended));
                s_ownParticles.push_back(o);
                StoreVec(base, n.address + kParticlePosition, blended);
            }
        s_stats.ownPictures++;
        s_ownFraction.store(t, std::memory_order_release);
    }

    void EndOwn(uint8_t* base)
    {
        WaitPlayed(base, false);
        for (auto it = s_ownSaved.rbegin(); it != s_ownSaved.rend(); ++it)
            memcpy(base + it->address, it->bytes.data(), it->bytes.size());
        s_ownSaved.clear();
        for (const OwnParticle& o : s_ownParticles)
        {
            float cur[3], put[3];
            LoadVec(base, o.address + kParticlePosition, cur);
            const bool moved = memcmp(cur, o.blended, sizeof(cur)) != 0;
            for (int i = 0; i < 3; i++)
                put[i] = moved ? o.now[i] + (cur[i] - o.blended[i]) : o.now[i];
            StoreVec(base, o.address + kParticlePosition, put);
            (moved ? s_ownParticlesMoved : s_ownParticlesKept)++;
        }
        s_ownParticles.clear();
        for (const OwnCamera& c : s_ownCameras)
        {
            memcpy(base + c.camera, c.state, sizeof(c.state));
            memcpy(base + c.camera + 224, c.state, sizeof(c.state));
        }
        s_ownCameras.clear();
        s_ownFraction.store(0.0f, std::memory_order_release);
    }

    // Load: in-between pictures only while the command processor and the
    // GPU can do a step's pictures in a step. A frame whose work outlasts
    // its vblank runs two world steps the next time, and the world plays
    // differently for it (M2's A/B: 39-50 such frames at the race start on
    // the M1 Pro, two pictures of 9.4 ms CP and 9.3 ms GPU each, and the
    // launch, Razor and the coast-down moved with them; a 60 fps run beside
    // a build overran as often and moved the same way). So when kLateOfRecent
    // of the last kRecent frames with in-between pictures took the command
    // processor or the GPU (execution, timestamps) more than kBudgetMs, the
    // frames go on without them (60 fps, still even) until a step's pictures
    // at twice the real picture's cost would take less than kResumeMs for 90%
    // of the last kResumeWindow frames, `hold` (2 s at first, twice as long
    // when that overloads again within kRecentResume, up to 32 s). M2's run
    // dm5 found single frames' costs too noisy for anything stricter: one
    // cutscene start turned them off for good; its race held 120 fps evenly
    // with 15.8 ms of the command processor's and 11.2 of the GPU's a step,
    // so the budget is nearly the whole step. A frame with in-between
    // pictures that ran two world steps (it overran) after a second of frames
    // of one step each turns them off at once (two-step frames in loads,
    // movies and 30 fps menus come at 60 fps too: M2's run dm7 counted them,
    // and their backoff kept HFR off for the race).
    // NFSMW_HFR_LOAD=0: no check.
    // M2's dm11: the race that held 120 evenly without the check (dm3: 0
    // overruns) went over 16.2 ms in 4 of 8 frames now and then, and was
    // held at 60 for 32 s at a time. The command processor's busy time isn't
    // a step's capacity to the millisecond, so cost alone turns them off only
    // past what no step can hold (kBudgetMs); overruns do the rest.
    constexpr double kBudgetMs = 19.0, kResumeMs = 16.0;
    constexpr uint32_t kRecent = 8, kLateOfRecent = 4, kResumeWindow = 120;
    std::atomic<bool> s_overran{ false };  // main loop thread: frames with them overran (twice within kOverrunWindow)
    // An overrun alone doesn't count: one comes with a load's or a pipeline's
    // hitch at 60 fps too, and the first frames after in-between pictures
    // come back have one now and then (M2's dm13 turned them off for good
    // that way, in a race that held 120 with 0 overruns when left alone:
    // gd1). Two within kOverrunWindow do, and none in kResumeGrace after a
    // resume.
    constexpr int64_t kOverrunWindowNs = 2'000'000'000, kResumeGraceNs = 2'000'000'000;
    int64_t s_lastOverrunNs = 0;  // main loop thread
    std::atomic<int64_t> s_resumedNs{ 0 };
    // The hold doubles when they overload again within kRecentResume of
    // coming back, and starts over at a lone overrun: that is as often a
    // load's or a pipeline's hitch, which comes at 60 fps too (M2's dm10:
    // hitches in the menus and at the race start grew the hold to 32 s, and
    // the race, which held 120 evenly in dm3, waited 64 s for them).
    // M3: kRecentResume 5 -> 30 s, and a lone overrun starts the hold over
    // only kHoldResetNs after the last overload: a play-test race
    // (2026-10-18) overloaded again 4-14 s after each of six resumes, each
    // hold started over at 2 s, and it switched 12 times in 70 s.
    constexpr int64_t kFirstHoldNs = 2'000'000'000, kMaxHoldNs = 16'000'000'000, kRecentResumeNs = 30'000'000'000,
                      kHoldResetNs = 60'000'000'000;
    // M3: an in-between picture's command-processor time as a share of a
    // real one's (EMA while both are drawn: the passes it leaves out make it
    // 0.3-0.6): a step's two pictures while overloaded are estimated at
    // (1 + share) times the real picture's cost, not twice it.
    std::atomic<float> s_betweenShare{ 1.0f };
    const bool s_loadCheck = [] { const char* v = std::getenv("NFSMW_HFR_LOAD"); return !(v && v[0] == '0'); }();
    std::atomic<bool> s_overloaded{ false };
    // Off Apple's systems: an even 60 asked for (any thread) for at least
    // this long (ns; the largest pending), and why: 1 a picture shown on
    // another refresh than the others (NoteLatePicture), 2 a cut (our settings open or just closed, a
    // resume from suspension, the refresh grid measured anew). The command
    // processor thread takes it at the next frame (NoteFrameCost) and comes
    // back from it as from an overload: after the hold, and only once 120
    // frames' costs say two pictures a step fit (kResumeMs).
    std::atomic<int64_t> s_forceHoldNs{ 0 };
    std::atomic<int> s_forceWhy{ 0 };
    void RequestSixty(int64_t holdNs, int why)
    {
        int64_t cur = s_forceHoldNs.load(std::memory_order_relaxed);
        while (cur < holdNs && !s_forceHoldNs.compare_exchange_weak(cur, holdNs, std::memory_order_relaxed))
        {
        }
        int w = s_forceWhy.load(std::memory_order_relaxed);
        if (why == 1 || w == 0)
            s_forceWhy.store(why, std::memory_order_relaxed);
    }
    // A late picture's hold (game/hfr_late_hold.h): 8 s, twice the last one
    // when pictures are late again within 10 s of coming back, at most 32 s;
    // a lone one after a clean stretch or near a load starts over at 8 s.
    constexpr int64_t kFirstLateHoldNs = hfr::late::kFirstNs, kCutHoldNs = 2'000'000'000;
    // The last load's or long hitch's time (StepFractions' settle; main loop
    // thread writes, the command processor thread reads).
    std::atomic<int64_t> s_hitchNs{ 0 };
    struct LoadState  // command processor thread only
    {
        bool late[kRecent] = {};
        uint32_t next = 0;
        float costs[kResumeWindow] = {};  // overloaded: twice each frame's cost
        uint32_t costCount = 0, costNext = 0;
        int64_t since = 0, hold = kFirstHoldNs, resumedAt = 0, overloads = 0, overloadedAt = 0;
        // The lite step (s_level 1): since when, how long before full
        // in-between pictures are tried again, and when they last were.
        int64_t liteSince = 0, liteHold = 0, fullTriedAt = 0;
        float liteCosts[kResumeWindow] = {};  // lite: each step's estimated cost with full in-between pictures
        uint32_t liteCount = 0, liteNext = 0;
        // Off Apple's systems: the frames' cost with in-between pictures,
        // averaged (an EMA over about 30 frames), and how many fed it since
        // the last step change.
        double ema = 0;
        uint32_t emaFrames = 0;
        hfr::late::Hold ghost;  // the last late picture's hold, and when
    } s_load;
    // Off Apple's systems, a step's frames that take the command processor or
    // the GPU more than kSustainMs on average (s_load.ema) are an overload
    // too: on a 90 Hz screen a step's 1.5 pictures that take 17-19 ms each
    // step, under kBudgetMs's single-frame test, came out late every other
    // frame and on the wrong refreshes for a minute and a half (the Deck OLED
    // after a long pause, 2026-10-25: the car "ghosted badly"; the CP 13 ms a
    // real picture there, 6 an in-between one).
#if defined(__APPLE__)
    constexpr bool kSustainCheck = false;  // the Mac as M3 measured it
#else
    constexpr bool kSustainCheck = true;
#endif
    constexpr double kSustainMs = 17.0;
    constexpr uint32_t kSustainFrames = 60;
    // M3's ladder: full in-between pictures go lite at an overload, back
    // to full after kFirstLiteHold of lite ones without one (twice that, at
    // most kMaxLiteHold, when the full ones overload again within
    // kRecentResume of the try); lite ones that overload go to none (the
    // hold above), and come back lite.
    constexpr int64_t kFirstLiteHoldNs = 10'000'000'000, kMaxLiteHoldNs = 120'000'000'000;
    int64_t SteadyNs()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // A step's pictures: their fractions of the way from the last step, in
    // order, the last its own (1: the real picture). The display's
    // (video::HfrPicturesPerStep: n pictures, i/n), NFSMW_HFR_PICTURES's or
    // NFSMW_HFR_FRACTIONS's; one, the real one, while overloaded; 1/2, 1 for
    // NFSMW_HFR_RENDER2.
    constexpr int64_t kSettleGapNs = 100'000'000;
    constexpr int kSettleSteps = 60;
    int s_settle = 0;  // steps left without pictures between (main loop thread)
    int StepFractions(float f[4], uint32_t steps)
    {
        if (!s_enabled)
        {
            f[0] = 0.5f;
            f[1] = 1.0f;
            return 2;
        }
        // After a load or a long hitch (a render entry more than kSettleGapNs
        // after the last): kSettleSteps steps with no picture between. Free
        // roam's start on the Deck OLED at 90 Hz (2026-10-25: "the car ghosted
        // a bit, then it resolved itself"): the cars and the camera just
        // placed, the first frames' shaders and the command processor over
        // budget (20-22 ms a frame) until the load check went lite 0.3 s in.
        {
            static int64_t last = 0;
            const int64_t now = SteadyNs();
#if defined(__APPLE__)
            constexpr bool kSettle = false;  // the Mac as M3 measured it
#else
            constexpr bool kSettle = true;
#endif
            if (kSettle && last && now - last > kSettleGapNs)
            {
                if (s_settle == 0)
                    s_stats.settles++;
                s_settle = kSettleSteps;
                s_hitchNs.store(now, std::memory_order_relaxed);
            }
            last = now;
        }
#if !defined(__APPLE__)
        {
            // Cuts: our settings open (the game runs under them) or closed
            // since the last frame, a resume from suspension, the refresh grid
            // measured anew: an even 60 until the timing has held again.
            static bool settingsWere = false;
            static int64_t resumeSeen = 0;
            static bool gridWas = false;
            const bool settings = ui::SettingsOpen();
            const int64_t resumed = guesttime::LastResumeNs();
            double gp = 0;
            int64_t gg = 0;
            const bool grid = video::HfrGrid(gp, gg);
            if (settings || settings != settingsWere || resumed != resumeSeen || (grid && !gridWas))
                RequestSixty(kCutHoldNs, 2);
            settingsWere = settings;
            resumeSeen = resumed;
            gridWas = grid;
        }
#endif
        if (s_settle > 0)
        {
            s_settle--;
            f[0] = 1.0f;
            return 1;
        }
        if (s_overloaded.load(std::memory_order_relaxed))
        {
            f[0] = 1.0f;
            return 1;
        }
        if (!s_forcedFractions.empty())
        {
            const std::vector<float>& step = s_forcedFractions[s_fractionCycle++ % s_forcedFractions.size()];
            for (size_t i = 0; i < step.size(); i++)
                f[i] = step[i];
            return int(step.size());
        }
        const int n = s_forcedPictures ? s_forcedPictures : std::clamp(video::HfrPicturesPerStep(), 1, 4);
        double period = 0;
        int64_t grid = 0;
        if (n == 1 && video::HfrGrid(period, grid))
        {
            // A picture for each refresh between the last step's last picture
            // (shown at the vblank before, plus a vblank, plus kGridLeadNs)
            // and this step's: 90 Hz's 2/3 | 1/3, 1, 144 Hz's 2-3 a step.
            // `from` moves on a step at a time, so each step's pictures follow
            // the last one's: the guest vblank before a render entry can be
            // the same for two frames in a row (an entry just after it, the
            // next just before the following one), and M2's run dg1 then drew
            // a step's in-between picture for an earlier refresh than the
            // last step's own. Back on the vblank's when that's half a step off.
            // The step's own picture goes out two vblanks after the one before
            // this render entry (M2's tl2: an entry 15 ms after a vblank, its
            // real picture released by the vblank after next), so `from` (the
            // last step's) is a vblank and kGridLeadNs past the next one: dg3
            // asked for times 5 ms before the pictures were released.
            const double step = video::GuestVblank().PeriodNs();
            const double byVblank = double(video::LastGuestVblank()) + 2.0 * step + double(kGridLeadNs);
            s_gridFrom += step * double(std::max<uint32_t>(steps, 1));
            // The vblank before an entry flips between two (entries close to
            // it): only further off than that is the schedule resynced.
            if (!(std::fabs(s_gridFrom - byVblank) <= 1.5 * step))
                s_gridFrom = byVblank;
            if (const int count = hfr::grid::Pictures(double(grid), period, step, s_gridFrom, f, s_targets))
                return count;
        }
        for (int i = 0; i < n; i++)
        {
            f[i] = float(i + 1) / float(n);
            s_targets[i] = 0;
        }
        return n;
    }

}

namespace
{
    enum SkipPass
    {
        kSkipShadow = 1, kSkipView4 = 2, kSkipEnv = 4, kSkipLum = 8
    };
    // Which passes an in-between picture leaves out: none at first (the
    // left-out passes' contents are half a step old, and M3's Studio bursts
    // (s9a) found the in-between pictures up to 1 px off halfway in some NIS
    // shots with the shadow and environment maps left out); the "lite" set
    // while the load check finds the full pictures too heavy (a ladder: full
    // in-between pictures, lite ones, none), as on the M1 Pro, whose NIS
    // drew in-between pictures in 85% of its frames instead of 59% with them
    // left out (run d4e). NFSMW_HFR_SKIP=<list> leaves its passes out always,
    // NFSMW_HFR_LITE=<list> picks the lite set (default shadow, env, mirror;
    // none: no lite step): shadow, view4 or mirror, env, lum, all, none.
    uint32_t ParseSkip(const char* v)
    {
        const std::string s = v;
        uint32_t m = 0;
        if (s.find("shadow") != std::string::npos || s == "all")
            m |= kSkipShadow;
        if (s.find("view4") != std::string::npos || s.find("mirror") != std::string::npos || s == "all")
            m |= kSkipView4;
        if (s.find("env") != std::string::npos || s == "all")
            m |= kSkipEnv;
        if (s.find("lum") != std::string::npos)
            m |= kSkipLum;
        return m;
    }
    // Always left out by default: the luminance measurement, whose reading
    // an in-between picture never uses (sub_82223308 returns the last real
    // one's, and the exposure is put back): its resolve went into the target
    // the real picture reads when the GPU is behind (M3's run d8a: the real
    // pictures' luminance reads still stalled 6 times a second).
    const uint32_t s_forcedSkip = [] { const char* v = std::getenv("NFSMW_HFR_SKIP"); return v ? ParseSkip(v) : uint32_t(kSkipLum); }();
    const uint32_t s_liteSkip = [] {
        const char* v = std::getenv("NFSMW_HFR_LITE");
        // Off Apple's systems the shadow maps are kept since 2026-10-25: half
        // a step old in a lite in-between picture, the car's own shadow
        // flickered as it moved (the Deck OLED at 90 Hz, lite most of free
        // roam); the Steam Machine's in-between picture took 5.8 ms of command
        // processor with them, 4.2 without (real 7.0). NFSMW_HFR_LITE=shadow,
        // env,mirror: M3's set.
#if defined(__APPLE__)
        return v ? ParseSkip(v) : uint32_t(kSkipShadow | kSkipEnv | kSkipView4);  // the Mac as M3 measured it
#else
        return v ? ParseSkip(v) : uint32_t(kSkipEnv | kSkipView4);
#endif
    }();
    // The load check's step (command processor thread writes, main loop
    // thread reads): 0 full in-between pictures, 1 lite ones (s_overloaded:
    // none). And what this in-between picture leaves out (main loop thread,
    // latched at each frame's render entry so a step change can't split one).
    std::atomic<int> s_level{ 0 };
    // A second lite step (s_level 2, the shadow maps left out too) was
    // tried on the Deck OLED (108c505, 2026-10-25) and taken out: the cars'
    // shadows on the road ghosted under it. The ladder is full -> lite -> 60.
    const uint32_t s_lite2Skip = s_liteSkip;
    int TopLevel() { return (s_lite2Skip & ~s_liteSkip & ~s_forcedSkip) ? 2 : (s_liteSkip & ~s_forcedSkip) ? 1 : 0; }
    std::atomic<uint32_t> s_activeSkip{ 0 };
    uint32_t ActiveSkip()
    {
        const int level = s_level.load(std::memory_order_relaxed);
        return s_forcedSkip | (level >= 2 ? s_lite2Skip : level == 1 ? s_liteSkip : 0u);
    }
    std::string SkipNames(uint32_t m)
    {
        std::string out;
        for (auto [bit, name] : { std::pair{ kSkipShadow, "the shadow maps" }, std::pair{ kSkipEnv, "the environment map" },
                 std::pair{ kSkipView4, "the mirror" }, std::pair{ kSkipLum, "the luminance measurement" } })
            if (m & bit)
                out += (out.empty() ? "" : ", ") + std::string(name);
        return out.empty() ? "nothing" : out;
    }
    // A pass left out of an in-between picture leaves its targets as the
    // real picture drew them, but the views it draws from are set up again
    // from the in-between picture's cameras, and the main view looks the
    // targets up through them: the shadows and what the environment map
    // lights moved off halfway, 0.6-1.6 px at 1280 px in a slow NIS pan (M3
    // runs s8e, s8s; s8n with nothing left out: 0.18). So those views keep
    // the real picture's setup: views 13-16 (the shadow maps': sub_82454DC8
    // fits 13 and sub_82456030 15 to view 1, 14 and 16 to view 2) and 18-23
    // (the environment map's faces). NFSMW_HFR_KEEP_VIEWS=0: set up anyway.
    constexpr uint32_t kViews = 0x82A38070, kViewBytes = 112;
    const bool s_keepViews = [] { const char* v = std::getenv("NFSMW_HFR_KEEP_VIEWS"); return !(v && v[0] == '0'); }();
    bool KeepView(uint32_t view)
    {
        if (!s_enabled || !s_keepViews || !s_inBetween.load(std::memory_order_acquire) || view < kViews || view >= kViews + 24 * kViewBytes)
            return false;
        const uint32_t index = (view - kViews) / kViewBytes;
        const uint32_t m = s_activeSkip.load(std::memory_order_relaxed);
        return ((m & kSkipShadow) && index >= 13 && index <= 16) || ((m & kSkipEnv) && index >= 18);
    }
    bool KeepShadowFit()
    {
        return s_enabled && s_keepViews && (s_activeSkip.load(std::memory_order_relaxed) & kSkipShadow) && s_inBetween.load(std::memory_order_acquire);
    }
    std::atomic<uint64_t> s_skipped[4];
    bool Skip(uint32_t pass)
    {
        if (!(s_activeSkip.load(std::memory_order_relaxed) & pass) || !s_inBetween.load(std::memory_order_acquire))
            return false;
        s_skipped[pass == kSkipShadow ? 0 : pass == kSkipView4 ? 1 : pass == kSkipEnv ? 2 : 3].fetch_add(1, std::memory_order_relaxed);
        return true;
    }
}

namespace hfr
{
    void Init()
    {
        s_enabled = settings::GetBool(settings::Id::SmoothMotion);
        if (s_enabled)
            fprintf(stderr, "[hfr] Smooth Motion (NFSMW_HFR): each world step drawn as often as the display shows pictures between two "
                            "guest vblanks, first between the last step and this one; in-between pictures leave out %s (NFSMW_HFR_SKIP), and "
                            "%s more while the load check finds them too heavy (NFSMW_HFR_LITE)\n",
                SkipNames(s_forcedSkip).c_str(), SkipNames(s_liteSkip & ~s_forcedSkip).c_str());
        const char* v = std::getenv("NFSMW_HFR_RENDER2");
        s_render2 = s_enabled || (v && v[0] == '1');
        if (!s_enabled && s_render2)
            fprintf(stderr, "[hfr] NFSMW_HFR_RENDER2=1: the render (sub_823AFC68) runs twice a frame (DIAGNOSTIC)\n");
    }
    bool Enabled() { return s_enabled; }
    void NoteLatePicture()
    {
#if !defined(__APPLE__)
        if (s_enabled && s_loadCheck && !s_overloaded.load(std::memory_order_relaxed))
            RequestSixty(kFirstLateHoldNs, 1);
#endif
    }
    void NoteHoldChange()
    {
#if !defined(__APPLE__)
        if (s_enabled && s_loadCheck)
            RequestSixty(kCutHoldNs, 2);
#endif
    }
    bool Render2() { return s_render2; }
    bool InBetween() { return s_inBetween.load(std::memory_order_acquire); }
    float InBetweenFraction() { return s_fraction.load(std::memory_order_relaxed); }
    float OwnFraction() { return s_ownFraction.load(std::memory_order_acquire); }
    int64_t PictureTarget() { return s_target.load(std::memory_order_relaxed); }

    void NoteFrameCost(double cpMs, double gpuMs, bool hadBetween)
    {
        if (!s_enabled || !s_loadCheck)
            return;
        LoadState& l = s_load;
        const int64_t now = SteadyNs();
        const double cost = std::max(cpMs, gpuMs);
        if (const int64_t req = s_forceHoldNs.exchange(0, std::memory_order_relaxed))
        {
            const int why = s_forceWhy.exchange(0, std::memory_order_relaxed);
            int64_t hold = req;
            hfr::late::Kind kind = hfr::late::Kind::kFirst;
            if (why == 1)
            {
                kind = hfr::late::Next(l.ghost, now, l.resumedAt, s_hitchNs.load(std::memory_order_relaxed));
                hold = std::max(hold, l.ghost.hold);
            }
            const bool was = s_overloaded.load(std::memory_order_relaxed);
            if (!was || hold > l.hold - (now - l.since))
            {
                l.since = now;
                l.hold = hold;
                l.costCount = l.costNext = 0;
            }
            if (!was)
            {
                l.overloadedAt = now;
                l.emaFrames = 0;
                std::fill(std::begin(l.late), std::end(l.late), false);
                l.overloads++;
                s_overloaded.store(true, std::memory_order_relaxed);
                fprintf(stderr, "[hfr] no in-between pictures for now: %s%s%s; back after %.0f s once two pictures a step would take < %.1f ms in "
                                "90%% of %u frames\n", why == 1 ? "a picture was shown on another refresh than the others (a ghost" : "a cut (the settings open or just closed, a resume, the refresh grid measured anew)",
                    why == 1 ? ", " : "", why == 1 ? std::format("{})", hfr::late::Name(kind)).c_str() : "",
                    double(hold) / 1e9, kResumeMs, kResumeWindow);
            }
            return;
        }
        if (!s_overloaded.load(std::memory_order_relaxed))
        {
            const bool overran = s_overran.exchange(false, std::memory_order_relaxed);
            if (!hadBetween && !overran)
                return;
            l.late[l.next++ % kRecent] = cost > kBudgetMs;
            uint32_t late = 0;
            for (bool b : l.late)
                late += b;
            bool sustained = false;
            if (kSustainCheck && hadBetween)
            {
                l.ema = l.emaFrames ? l.ema * (29.0 / 30.0) + cost / 30.0 : cost;
                l.emaFrames++;
                sustained = l.emaFrames >= kSustainFrames && l.ema > kSustainMs;
            }
            if (late < kLateOfRecent && !overran && !sustained)
            {
                // Lite: full ones again once a step's two full pictures look
                // affordable (this step's cost, its in-between picture's
                // share of a real one's swapped for a whole one), or as a try
                // after the lite hold without an overload.
                if (const int level = s_level.load(std::memory_order_relaxed); level >= 1)
                {
                    const double share = s_betweenShare.load(std::memory_order_relaxed);
                    l.liteCosts[l.liteNext++ % kResumeWindow] = float(cost * 2.0 / (1.0 + share));
                    l.liteCount = std::min(l.liteCount + 1, kResumeWindow);
                    float p90 = 1e9f;
                    if (l.liteCount == kResumeWindow)
                    {
                        float sorted[kResumeWindow];
                        std::copy(std::begin(l.liteCosts), std::end(l.liteCosts), sorted);
                        std::nth_element(sorted, sorted + kResumeWindow * 9 / 10, sorted + kResumeWindow);
                        p90 = sorted[kResumeWindow * 9 / 10];
                    }
                    const bool affordable = p90 < kResumeMs && now - l.liteSince >= kFirstHoldNs;
                    if (affordable || now - l.liteSince >= l.liteHold)
                    {
                        s_level.store(level - 1, std::memory_order_relaxed);
                        l.emaFrames = 0;
                        if (level == 2)
                        {
                            // Lite with the shadow maps again (the full step's try as below).
                            const double held = double(now - l.liteSince) / 1e9;
                            l.liteSince = now;
                            l.liteCount = l.liteNext = 0;
                            l.fullTriedAt = now;
                            s_resumedNs.store(now, std::memory_order_relaxed);
                            fprintf(stderr, "[hfr] lite in-between pictures with the shadow maps again: %s (%.0f s without an overload)\n",
                                affordable ? "affordable" : "trying", held);
                            return;
                        }
                        l.fullTriedAt = now;
                        s_resumedNs.store(now, std::memory_order_relaxed);  // the overrun grace after a change
                        fprintf(stderr, "[hfr] full in-between pictures again: %s (lite for %.0f s without an overload; two full pictures a "
                                        "step would have taken %.1f ms or less in 90%% of the last %u frames)\n",
                            affordable ? "affordable" : "trying", double(now - l.liteSince) / 1e9, p90, kResumeWindow);
                    }
                }
                return;
            }
            // Full ones too heavy: lite ones first (the lite set left out).
            if (s_level.load(std::memory_order_relaxed) == 0 && (s_liteSkip & ~s_forcedSkip))
            {
                l.emaFrames = 0;
                if (l.fullTriedAt && now - l.fullTriedAt < kRecentResumeNs)
                    l.liteHold = std::min(l.liteHold * 2, kMaxLiteHoldNs);
                else if (!l.liteHold || now - l.liteSince >= kHoldResetNs)
                    l.liteHold = kFirstLiteHoldNs;
                l.liteSince = now;
                l.liteCount = l.liteNext = 0;
                std::fill(std::begin(l.late), std::end(l.late), false);
                s_level.store(1, std::memory_order_relaxed);
                s_resumedNs.store(now, std::memory_order_relaxed);  // the overrun grace after a change: d8a dropped lite to 60 5 s after
                fprintf(stderr, "[hfr] lite in-between pictures for now: %s (the last frame: %.1f ms of the command processor, %.1f of the "
                                "GPU); they leave out %s, full ones again after %.0f s without an overload\n",
                    overran ? "a frame with full ones ran two world steps" : sustained ? std::format("{:.1f} ms a frame on average with them", l.ema).c_str() : std::format("{} of the last {} frames with full ones took more than "
                        "{:.1f} ms of the command processor or the GPU", late, kRecent, kBudgetMs).c_str(),
                    cpMs, gpuMs, SkipNames(s_liteSkip & ~s_forcedSkip).c_str(), double(l.liteHold) / 1e9);
                return;
            }
            if (s_level.load(std::memory_order_relaxed) == 1 && TopLevel() == 2)
            {
                if (l.fullTriedAt && now - l.fullTriedAt < kRecentResumeNs)
                    l.liteHold = std::min(std::max(l.liteHold, kFirstLiteHoldNs) * 2, kMaxLiteHoldNs);
                else if (!l.liteHold)
                    l.liteHold = kFirstLiteHoldNs;
                l.liteSince = now;
                l.liteCount = l.liteNext = 0;
                l.emaFrames = 0;
                std::fill(std::begin(l.late), std::end(l.late), false);
                s_level.store(2, std::memory_order_relaxed);
                s_resumedNs.store(now, std::memory_order_relaxed);
                fprintf(stderr, "[hfr] lite in-between pictures without the shadow maps for now: %s (the last frame: %.1f ms of the command "
                                "processor, %.1f of the GPU)\n", sustained ? std::format("{:.1f} ms a frame on average", l.ema).c_str()
                                    : overran ? "a frame with them ran two world steps" : "frames over budget", cpMs, gpuMs);
                return;
            }
            // Again soon after a resume: twice the hold (at most kMaxHold); a
            // lone overrun (a load's, a pipeline's hitch) a while after the
            // last overload starts it over.
            if (l.resumedAt && now - l.resumedAt < kRecentResumeNs)
                l.hold = std::min(l.hold * 2, kMaxHoldNs);
            else if (overran && now - l.overloadedAt >= kHoldResetNs)
                l.hold = kFirstHoldNs;
            l.overloadedAt = now;
            std::fill(std::begin(l.late), std::end(l.late), false);
            l.since = now;
            l.costCount = l.costNext = 0;
            l.overloads++;
            l.emaFrames = 0;
            s_overloaded.store(true, std::memory_order_relaxed);
            fprintf(stderr, "[hfr] no in-between pictures for now: %s (the last frame: %.1f ms of the command processor, %.1f of the GPU); "
                            "back when two pictures would take < %.1f ms in 90%% of %u frames (an in-between picture %.2f of a real one), after %.0f s at least (overload %lld)\n",
                overran ? "a frame with them ran two world steps" : sustained ? std::format("{:.1f} ms a frame on average with them", l.ema).c_str() : std::format("{} of the last {} frames with them took more than {:.1f} ms "
                    "of the command processor or the GPU", late, kRecent, kBudgetMs).c_str(),
                cpMs, gpuMs, kResumeMs, kResumeWindow, double(s_betweenShare.load(std::memory_order_relaxed)), double(l.hold) / 1e9,
                (long long)l.overloads);
            return;
        }
        // Overloaded: one picture a step; with its in-between picture, the
        // step would take (1 + its measured share) times this.
        l.costs[l.costNext++ % kResumeWindow] = float((1.0 + s_betweenShare.load(std::memory_order_relaxed)) * cost);
        l.costCount = std::min(l.costCount + 1, kResumeWindow);
        if (l.costCount < kResumeWindow || now - l.since < l.hold)
            return;
        float sorted[kResumeWindow];
        std::copy(std::begin(l.costs), std::end(l.costs), sorted);
        std::nth_element(sorted, sorted + kResumeWindow * 9 / 10, sorted + kResumeWindow);
        const float p90 = sorted[kResumeWindow * 9 / 10];
        // Twice a real picture's cost overstates a step's two (M2's dm8: the
        // race that held 120 evenly in dm3 never looked affordable): after
        // twice the hold, try them anyway (a try that overloads again within
        // kRecentResume doubles the hold).
        const bool tryAnyway = now - l.since >= 2 * l.hold;
        if (p90 >= kResumeMs && !tryAnyway)
            return;
        l.resumedAt = now;
        s_resumedNs.store(now, std::memory_order_relaxed);
        // Back lite (when there is a lite step), full ones after its hold.
        if (s_liteSkip & ~s_forcedSkip)
        {
            s_level.store(TopLevel(), std::memory_order_relaxed);
            l.liteSince = now;
            l.liteCount = l.liteNext = 0;
            if (!l.liteHold)
                l.liteHold = kFirstLiteHoldNs;
        }
        s_overloaded.store(false, std::memory_order_relaxed);
        fprintf(stderr, "[hfr] in-between pictures again%s: %s (two pictures a step would have taken %.1f ms or less in 90%% of the last %u "
                        "frames)\n", s_level.load(std::memory_order_relaxed) >= 1 ? ", lite" : "", p90 < kResumeMs ? "affordable" : "trying", p90,
            kResumeWindow);
    }

    void NoteSwap(float between)
    {
        if (s_timelineOn.load(std::memory_order_relaxed))
            fprintf(stderr, "[hfrtime] swap %s at %.2f ms after the guest vblank\n", between != 0.0f ? "in-between" : "real",
                SinceVblankMs(video::LastGuestVblank()));
    }
}

// The frame's render entry (sub_823AFC68: skips while [0x82A2CC08] > 0, else
// times and calls the render frame sub_82441050). Called from the frame
// (sub_823AFE20) after the world steps and the cameras' and cars' updates.
extern "C" PPC_FUNC(__imp__sub_823AFC68);
SB_HOOK(sub_823AFC68)
{
    s_writeSet = 0;
    float ownFraction = 1.0f;  // < 1: this step's own picture drawn between (no real picture)
    bool drew = false;         // in-between (or own) pictures this frame
    int64_t lastTarget = 0;    // the step's last picture's refresh (a grid's; 0: the usual way)
    const int64_t vblank = video::LastGuestVblank();
    double times[5] = { SinceVblankMs(vblank), -1, -1, -1, -1 };
    const bool timeline = s_timeline.frames && s_frameCount + 1 >= s_timeline.from && s_frameCount + 1 < s_timeline.from + s_timeline.frames;
    s_timelineOn.store(timeline, std::memory_order_relaxed);
    if (s_render2 && PPC_LOAD_U32(kRenderSkip) == 0)
    {
        s_step = Step(base);
        s_writeSet = s_enabled ? hfrdiag::WriteSetPhase(++s_frameCount) : 0;
        // An overrun: this game frame (the one right after the last drawn)
        // ran two world steps or more, after a second of frames of one
        // (frame_time.cpp's counts: a frame whose render the game skipped,
        // [0x82A2CC08], left the step count two apart without an overrun:
        // M2's run dm12 took those for overruns).
        if (s_enabled)
        {
            const game::FrameTimeStats ft = game::GetFrameTimeStats();
            const bool next = ft.frames == s_lastFrames + 1, multi = ft.multiStep != s_lastMultiStep;
            if (next && multi && s_lastDrewBetween && s_steadyFrames >= 60)
            {
                const int64_t now = SteadyNs();
                if (now - s_resumedNs.load(std::memory_order_relaxed) >= kResumeGraceNs)
                {
                    if (s_lastOverrunNs && now - s_lastOverrunNs < kOverrunWindowNs)
                        s_overran.store(true, std::memory_order_relaxed);
                    s_lastOverrunNs = now;
                }
            }
            s_steadyFrames = next && !multi && ft.noStep == s_lastNoStep ? s_steadyFrames + 1 : 0;
            s_lastFrames = ft.frames;
            s_lastMultiStep = ft.multiStep;
            s_lastNoStep = ft.noStep;
        }
        if (s_enabled && s_blendParticles)
            TrackParticles(base, s_step);
        float fractions[4];
        const int pictures = StepFractions(fractions, s_step - s_lastRealStep);
        s_activeSkip.store(s_enabled ? ActiveSkip() : 0u, std::memory_order_relaxed);  // this frame's in-between pictures leave these out
        const bool ownBetween = fractions[pictures - 1] < 0.999f;
        if (s_enabled && pictures == 1 && !ownBetween)
        {
            s_stats.frames++;
            if (s_overloaded.load(std::memory_order_relaxed))
                s_stats.skippedLoad++;
        }
        if (pictures > 1 || ownBetween)
        {
            s_stats.frames++;
            // One world step since the last real picture: in-between pictures
            // of it. None (paused, or a frame given no vblank: it would show
            // the last picture again) or several (a hitch: the frame is late
            // already) gets the real picture only.
            const uint32_t steps = s_step - s_lastRealStep;
            // Nor the first frames the render thread plays (a race or a
            // cutscene starting: they build what they draw first).
            s_threadedFrames = PPC_LOAD_U32(kRenderThreaded) ? s_threadedFrames + 1 : 0;
            if (s_threadedFrames == 1 || s_threadedFrames == 2)
                s_stats.skippedStart++;
            else if (s_writeSet == 2 || s_writeSet == 3)
            {
                // NFSMW_HFR_WRITESET's control frame (a real render alone) and noise frame.
                WaitPlayed(base, true);
                if (s_writeSet == 2)
                    hfrdiag::WriteSetControlBefore(base);
                else
                    hfrdiag::WriteSetNoise(base);
            }
            else if (steps == 1 || !s_enabled)
            {
                if (s_enabled && ownBetween)
                    ownFraction = fractions[pictures - 1];
                lastTarget = s_targets[pictures - 1];
                drew = pictures > 1 || ownFraction < 1.0f;
                for (int i = 0; i + 1 < pictures; i++)
                {
                    const float t = fractions[i];
                    if (s_enabled)
                    {
                        BeginInBetween(base, t);
                        // After the last picture has been played: its swap reads the target too.
                        s_target.store(s_targets[i], std::memory_order_relaxed);
                        times[1] = SinceVblankMs(vblank);
                    }
                    else
                    {
                        WaitPlayed(base, true);
                        s_fraction.store(t, std::memory_order_relaxed);
                        s_inBetween.store(true, std::memory_order_release);
                    }
                    SB_ORIGINAL(sub_823AFC68)(ctx, base);
                    times[2] = SinceVblankMs(vblank);
                    if (s_enabled)
                    {
                        EndInBetween(base);
                        times[3] = SinceVblankMs(vblank);
                        if (s_writeSet == 1)
                            hfrdiag::WriteSetAfterBetween(base);
                    }
                    else
                    {
                        WaitPlayed(base, false);
                        s_inBetween.store(false, std::memory_order_release);
                    }
                }
            }
            else if (steps == 0)
                s_stats.skippedNoStep++;
            else
                s_stats.skippedSteps++;
        }
        s_lastRealStep = s_step;
        s_lastDrewBetween = drew;
    }
    if (ownFraction < 1.0f)
        BeginOwn(base, ownFraction);
    // The step's own picture's refresh. Its swap is played after this thread
    // has moved on, so the value stays until the next picture's is set, each
    // time after the last picture has been played (M2's dg1/dg2 lost it: the
    // own pictures went out at the vblank's time instead of their refresh).
    if (lastTarget != s_target.load(std::memory_order_relaxed))
    {
        if (ownFraction >= 1.0f)
            WaitPlayed(base, true);
        s_target.store(lastTarget, std::memory_order_relaxed);
    }
    SB_ORIGINAL(sub_823AFC68)(ctx, base);
    if (ownFraction < 1.0f)
        EndOwn(base);
    times[4] = SinceVblankMs(vblank);
    if (timeline)
        fprintf(stderr, "[hfrtime] frame %llu step %u: entry %.2f, last real played %.2f, in-between recorded %.2f, played %.2f, "
                        "real recorded %.2f (ms after the guest vblank)\n", (unsigned long long)s_frameCount, s_step, times[0], times[1],
            times[2], times[3], times[4]);
    if (s_writeSet == 1 || s_writeSet == 2)
    {
        WaitPlayed(base, false);
        if (s_writeSet == 1)
            hfrdiag::WriteSetAfterReal(base);
        else
            hfrdiag::WriteSetControlAfter(base);
    }
    hfrdiag::AfterRenderEntry(base);
    // M3 (the load check's signal): how long the frame's render took on this
    // thread (its in-between pictures and their waits included), and the
    // main loop's frame (render entry to render entry), in frames with
    // in-between pictures and without. The loop paces itself on the swaps
    // the command processor releases at vblanks, not on the vblank (game/
    // frame_time.cpp), so the render's share of the frame is the margin.
    static std::vector<float> s_renderMs[2], s_frameMs[2];
    static int64_t s_lastEntryNs = 0;
    {
        const int64_t nowNs = SteadyNs(), entryNs = nowNs - int64_t((times[4] - times[0]) * 1e6);
        if (s_enabled && PPC_LOAD_U32(kRenderSkip) == 0)
        {
            s_renderMs[drew ? 1 : 0].push_back(float(times[4] - times[0]));
            if (s_lastEntryNs)
                s_frameMs[drew ? 1 : 0].push_back(float(double(entryNs - s_lastEntryNs) / 1e6));
        }
        s_lastEntryNs = entryNs;
    }
    // A line every 10 s of frames.
    if (s_enabled && s_stats.frames >= 600)
    {
        std::string share;
        for (int k = 1; k >= 0; k--)
        {
            std::vector<float>& r = s_renderMs[k];
            std::vector<float>& f = s_frameMs[k];
            if (r.empty())
                continue;
            std::sort(r.begin(), r.end());
            std::sort(f.begin(), f.end());
            share += std::format("{} {}: the render median {:.1f} ms, p90 {:.1f}, of frames median {:.1f}, p90 {:.1f}{}",
                k ? "with in-between pictures" : "without", r.size(), r[r.size() / 2], r[r.size() * 9 / 10], f.empty() ? 0.0f : f[f.size() / 2],
                f.empty() ? 0.0f : f[f.size() * 9 / 10], k ? "; " : "");
            r.clear();
            f.clear();
        }
        if (!share.empty())
            fprintf(stderr, "[hfr] this thread's frames: %s\n", share.c_str());
        fprintf(stderr, "[hfr] %llu frames, %llu in-between pictures, %llu steps drawn only between (none in %llu frames without a world step, %llu with several, %llu starting "
                        "the render thread, %llu for the load; "
                        "%llu on the render thread: waited %.2f ms each before, %.2f after, max %.2f, %llu timeouts): "
                        "%.2f cars blended a picture (%llu cuts), %.2f cameras (%llu cuts), %.1f particles of %.1f; tracking %zu cars, %zu cameras\n",
            (unsigned long long)s_stats.frames, (unsigned long long)s_stats.pictures, (unsigned long long)s_stats.ownPictures,
            (unsigned long long)s_stats.skippedNoStep,
            (unsigned long long)s_stats.skippedSteps, (unsigned long long)s_stats.skippedStart, (unsigned long long)s_stats.skippedLoad,
            (unsigned long long)s_stats.threaded,
            s_stats.threaded ? s_stats.waitBeforeMs / double(s_stats.threaded) : 0.0,
            s_stats.threaded ? s_stats.waitMs / double(s_stats.threaded) : 0.0, s_stats.waitMaxMs, (unsigned long long)s_stats.waitTimeouts,
            s_stats.pictures ? double(s_stats.cars) / double(s_stats.pictures) : 0.0, (unsigned long long)s_stats.carCuts,
            s_stats.pictures ? double(s_stats.cameras) / double(s_stats.pictures) : 0.0, (unsigned long long)s_stats.cameraCuts,
            s_stats.pictures ? double(s_stats.particlesBlended) / double(s_stats.pictures) : 0.0,
            s_stats.pictures ? double(s_stats.particles) / double(s_stats.pictures) : 0.0, s_cars.size(), s_cameras.size());
        if (s_ownParticlesMoved || s_ownParticlesKept)
            fprintf(stderr, "[hfr] steps' own pictures drawn between: %llu particle positions the render moved itself (kept, from the step's), "
                            "%llu it didn't\n", (unsigned long long)s_ownParticlesMoved, (unsigned long long)s_ownParticlesKept);
        s_ownParticlesMoved = s_ownParticlesKept = 0;
        if (s_stats.settles)
            fprintf(stderr, "[hfr] %llu loads or hitches over %lld ms: %d steps after each drawn without pictures between\n",
                (unsigned long long)s_stats.settles, (long long)(kSettleGapNs / 1'000'000), kSettleSteps);
        s_stats = {};
        // Forget what wasn't seen for a few seconds (render objects are freed and reused).
        const uint32_t now = s_step;
        std::erase_if(s_cars, [&](const auto& kv) { return now - kv.second.step > 300; });
        std::erase_if(s_cameras, [&](const auto& kv) { return now - kv.second.step > 300; });
        std::erase_if(s_particles, [&](const auto& kv) { return now - kv.second.step > 2; });
    }
}

// A car's render update: copies the body's matrix (its pointer at +52) to
// +816, deriving the render's velocities from the change (dt in f1).
extern "C" PPC_FUNC(__imp__sub_824E9C38);
SB_HOOK(sub_824E9C38)
{
    if (!s_enabled)
        return SB_ORIGINAL(sub_824E9C38)(ctx, base);
    const uint32_t object = ctx.r3.u32;
    Car& car = s_cars[object];
    Load(base, object + kCarMatrix, car.before);
    SB_ORIGINAL(sub_824E9C38)(ctx, base);
    Load(base, object + kCarMatrix, car.after);
    car.step = Step(base);
}

// A view setup: records the camera each view of the real picture used.
extern "C" PPC_FUNC(__imp__sub_8243EAC0);
SB_HOOK(sub_8243EAC0)
{
    const uint32_t view = ctx.r4.u32;
    // M3: an in-between picture that leaves a pass out keeps that pass's
    // views as the real picture set them up (KeepView).
    if (KeepView(view))
        return;
    if (s_enabled && !s_inBetween.load(std::memory_order_acquire) && s_ownFraction.load(std::memory_order_acquire) == 0.0f && view)
        if (const uint32_t camera = PPC_LOAD_U32(view + 64))
        {
            Camera& c = s_cameras[camera];
            Load(base, camera, c.m);
            c.fov = PPC_LOAD_U16(camera + kCameraFov);
            c.step = Step(base);
        }
    hfrdiag::OnViewSetup(base, view);
    SB_ORIGINAL(sub_8243EAC0)(ctx, base);
}

// A camera's update (sub_82161000: keeps its last pose at +224 and derives
// its motion from the change over dt, f1; dt kept at +676). During an
// in-between picture the camera is put back afterwards, and it is given t of
// the dt: it moved t of a step since the last real picture, so its velocity
// (the motion blur's) is the step's, not t of it (run h5: the in-between
// pictures' road streaks in a cutscene alternated by up to 14 levels).
extern "C" PPC_FUNC(__imp__sub_82161000);
SB_HOOK(sub_82161000)
{
    if (s_enabled && s_inBetween.load(std::memory_order_acquire) && ctx.r3.u32)
    {
        if (s_savedCameras.insert(ctx.r3.u32).second)
            Save(base, ctx.r3.u32, kCameraBytes);
        if (s_blendCameras)
            ctx.f1.f64 *= s_fraction.load(std::memory_order_relaxed);
    }
    else if (const float own = s_ownFraction.load(std::memory_order_acquire); s_enabled && own > 0.0f && s_blendCameras)
        ctx.f1.f64 *= own;
    SB_ORIGINAL(sub_82161000)(ctx, base);
}

// D3D's swap sync (sub_82598670, from the swap sub_82598DD0, before the
// packet VdSwap writes): with the device's presentation interval [+13772] at
// 1 it has the command processor raise an interrupt whose callback
// (sub_825985A0) sets the vblanks to wait, then wait for them (WAIT_REG_MEM):
// the swap goes out at the first vblank after the command processor reaches
// it. An in-between picture's swap waits for none, as with
// D3DPRESENT_INTERVAL_IMMEDIATE (0x80000000: the function then emits no
// packets), so the guest vblank, the game's frame clock, still releases one
// swap a step: the real picture's.
extern "C" PPC_FUNC(__imp__sub_82598670);
SB_HOOK(sub_82598670)
{
    if (!s_inBetween.load(std::memory_order_acquire))
        return SB_ORIGINAL(sub_82598670)(ctx, base);
    const uint32_t device = ctx.r3.u32;
    const uint32_t interval = PPC_LOAD_U32(device + kDevicePresentInterval);
    PPC_STORE_U32(device + kDevicePresentInterval, 0x80000000u);
    SB_ORIGINAL(sub_82598670)(ctx, base);
    PPC_STORE_U32(device + kDevicePresentInterval, interval);
}

// D3D's command buffers (sub_82597FF0, device creation: the parameters at r4,
// D3DRING_BUFFER_PARAMETERS-shaped: +4 the primary ring's size (0: 32 KB),
// +8 its memory, +12 the secondary buffer's size (0: 2 MB), +16 its memory,
// +20 the secondary's segments (0: 32)). The render thread can write
// commands only as far ahead of the command processor as the secondary
// buffer holds, and the command processor stops at each real picture's
// vblank wait: M2's run tl1 found the render thread spinning for ring space
// (sub_82597070 -> sub_82597498) through the in-between picture's playback,
// until the vblank (12 ms of every 16.7 ms frame the main loop waited for
// it). With HFR on, a step's two pictures need twice the room: the
// secondary buffer is made kHfrSecondaryFactor times larger, in as many more
// segments of the same size (when D3D allocates it itself: no memory given).
// NFSMW_HFR_SECONDARY=<n> sets the factor (1: the game's own).
extern "C" PPC_FUNC(__imp__sub_82597FF0);
SB_HOOK(sub_82597FF0)
{
    const uint32_t params = ctx.r4.u32;
    if (params)
    {
        const uint32_t primary = PPC_LOAD_U32(params + 4), secondary = PPC_LOAD_U32(params + 12);
        const uint32_t secondaryMemory = PPC_LOAD_U32(params + 16), segments = PPC_LOAD_U32(params + 20);
        fprintf(stderr, "[hfr] D3D command buffers: primary %u bytes%s, secondary %u bytes in %u segments%s\n", primary ? primary : 32768u,
            PPC_LOAD_U32(params + 8) ? " (the game's memory)" : "", secondary ? secondary : 2u << 20, segments ? segments : 32u,
            secondaryMemory ? " (the game's memory)" : "");
        static const uint32_t factor = [] {
            const char* v = std::getenv("NFSMW_HFR_SECONDARY");
            return v ? uint32_t(std::clamp(std::atoi(v), 1, 8)) : 4u;
        }();
        if (s_enabled && factor > 1 && !secondaryMemory)
        {
            const uint32_t size = (secondary ? secondary : 2u << 20) * factor, count = (segments ? segments : 32u) * factor;
            PPC_STORE_U32(params + 12, size);
            PPC_STORE_U32(params + 20, count);
            fprintf(stderr, "[hfr] D3D's secondary command buffer made %u times larger for two pictures a step: %u bytes in %u segments\n",
                factor, size, count);
        }
        // The primary ring too (NFSMW_HFR_PRIMARY=<n>, default 4; 1: the game's).
        static const uint32_t primaryFactor = [] {
            const char* v = std::getenv("NFSMW_HFR_PRIMARY");
            return v ? uint32_t(std::clamp(std::atoi(v), 1, 8)) : 4u;
        }();
        if (s_enabled && primaryFactor > 1 && !PPC_LOAD_U32(params + 8))
        {
            const uint32_t size = (primary ? primary : 32768u) * primaryFactor;
            PPC_STORE_U32(params + 4, size);
            fprintf(stderr, "[hfr] D3D's primary ring made %u times larger: %u bytes\n", primaryFactor, size);
        }
    }
    SB_ORIGINAL(sub_82597FF0)(ctx, base);
}

// The eye adaptation's luminance readback (sub_82223308, from sub_822234F0:
// the average of a 4 KB luminance target the GPU resolved, in f1). An
// in-between picture's would read it right after the last real picture's
// resolve, and the write guard stalled that read on the GPU (M2's run gd1:
// 237 of 400 logged stalls, 3-6 ms each; dm3's race 0.89 stalls and 4.07 ms
// stalled a picture against 0.11 ms a frame at 60). Its result is put back
// with the exposure after the picture anyway: an in-between picture uses
// the last real one's.
//   M3: a real picture's own read waits too when the GPU hasn't resolved
// the target yet: at 120 fps the GPU is behind by the in-between picture's
// work, and the guard stalled the render thread there 2.5 ms a second on the
// M1 Pro (M3's run d4a: 141-180 stalls of 3-4 ms in 200 s; 33 at 60 fps). With
// Smooth Motion the real picture then reads the last real picture's target
// (a frame old: the exposure moves 2% a frame toward it, which doesn't show).
// NFSMW_HFR_STALE_LUMINANCE=0: it waits, as before.
extern "C" PPC_FUNC(__imp__sub_82223308);
SB_HOOK(sub_82223308)
{
    static double last = 0.0;  // the last real picture's (whichever thread plays it)
    if (s_enabled && s_inBetween.load(std::memory_order_acquire))
    {
        ctx.f1.f64 = last;
        return;
    }
    static const bool staleOk = [] { const char* v = std::getenv("NFSMW_HFR_STALE_LUMINANCE"); return !(v && v[0] == '0'); }();
    // The game reads the target it has just resolved, of two it takes in
    // turn: while the GPU is still on its way there (run d5a: 130-450 of
    // 600 real pictures), the other one, which the last real picture
    // resolved and the GPU has finished, is read instead. Failing that the
    // last reading, at most kStaleReadings frames in a row.
    constexpr int kStaleReadings = 3;
    static int stale = 0;
    static uint32_t targets[2] = {};  // the last two targets read (most recent first)
    const uint32_t target = ctx.r3.u32;
    if (target != targets[0])
    {
        targets[1] = targets[0];
        targets[0] = target;
    }
    if (s_enabled && staleOk && writewatch::GpuWritePending(target & 0x1FFFFFFF, 4096))
    {
        const uint32_t other = targets[1];
        if (other && other != target && !writewatch::GpuWritePending(other & 0x1FFFFFFF, 4096))
        {
            s_staleLuminance.fetch_add(1, std::memory_order_relaxed);
            ctx.r3.u32 = other;
            SB_ORIGINAL(sub_82223308)(ctx, base);
            last = ctx.f1.f64;
            stale = 0;
            return;
        }
        if (stale < kStaleReadings)
        {
            stale++;
            s_staleLuminance.fetch_add(1, std::memory_order_relaxed);
            ctx.f1.f64 = last;
            return;
        }
    }
    stale = 0;
    SB_ORIGINAL(sub_82223308)(ctx, base);
    last = ctx.f1.f64;
}

// M3 (the in-between picture's cost): render passes it can leave out. The
// frame's render (sub_824454F8) prepares the views (sub_82440728), then draws
// the sun's shadow maps (sub_824439B0: worldShadows, worldModelShadows,
// smackableShadows; views 13-14), view 4 and 5's pass (sub_82444178: world and
// cars), the environment map's faces (sub_82444520: views 18-23, each while
// its enable byte, +8, is set), and the main view (sub_82443088). A pass left
// out of an in-between picture leaves its targets as the last real picture
// drew them, and the main view samples those. Left out while the load
// check holds the lite step (s_level, s_liteSkip), or always (s_forcedSkip);
// counted on the [hfr] command processor line. Measured on the Studio's NIS (M3 runs s3a-s3e): an
// in-between picture's command processor 5.0 ms, 3.2 without the shadow
// maps, 3.8 without the environment map, 4.5 without the mirror, 1.6
// without all three.
extern "C" PPC_FUNC(__imp__sub_824439B0);
SB_HOOK(sub_824439B0)
{
    if (Skip(kSkipShadow))
        return;
    SB_ORIGINAL(sub_824439B0)(ctx, base);
}

// The shadow views' fit to the player's view (render setup sub_82440F98:
// sub_82454DC8 sets up view 13 or 14 from view 1 or 2, sub_82456030 view 15
// or 16): kept from the real picture with the shadow maps (KeepView).
extern "C" PPC_FUNC(__imp__sub_82454DC8);
SB_HOOK(sub_82454DC8)
{
    if (KeepShadowFit())
        return;
    SB_ORIGINAL(sub_82454DC8)(ctx, base);
}

extern "C" PPC_FUNC(__imp__sub_82456030);
SB_HOOK(sub_82456030)
{
    if (KeepShadowFit())
        return;
    SB_ORIGINAL(sub_82456030)(ctx, base);
}

extern "C" PPC_FUNC(__imp__sub_82444178);
SB_HOOK(sub_82444178)
{
    if (Skip(kSkipView4))
        return;
    SB_ORIGINAL(sub_82444178)(ctx, base);
}

// The eye adaptation's luminance measurement (sub_822234F0: downsamples the
// picture, resolves it into one of two small targets, reads the other back,
// sub_82223308, and adapts the exposure, sub_82223220). An in-between picture's
// reading is the last real one's anyway, and its exposure is put back after it;
// and M2's run gd2 found the real picture's readback stalled 182 times (3.4 ms
// each) where the game at 60 stalls there a few times: the guess is that it
// waits for the in-between picture's resolve, in the same pages.
// NFSMW_HFR_SKIP=lum (M3): none in in-between pictures.
extern "C" PPC_FUNC(__imp__sub_822234F0);
SB_HOOK(sub_822234F0)
{
    if (Skip(kSkipLum))
        return;
    SB_ORIGINAL(sub_822234F0)(ctx, base);
}

extern "C" PPC_FUNC(__imp__sub_82444520);
SB_HOOK(sub_82444520)
{
    if (Skip(kSkipEnv))
        return;
    SB_ORIGINAL(sub_82444520)(ctx, base);
}

// M3: each picture's own command-processor time and draws (command processor
// thread, at each swap: the work since the last swap is this picture's), in-between and real
// pictures apart, a line every 1200 pictures.
void hfr::NotePictureCost(bool between, double cpMs, uint64_t draws, uint64_t passes)
{
    if (!s_enabled)
        return;
    struct Sum
    {
        uint64_t n = 0, draws = 0, passes = 0;
        double cp = 0, cpMax = 0;
    };
    static Sum sums[2];
    static uint64_t pictures = 0;
    // The in-between picture's share of a real one's cost (the load check's estimate).
    static double emaBetween = 0, emaReal = 0, lastBetween = -1;
    if (between)
        lastBetween = cpMs;
    else if (lastBetween >= 0)
    {
        emaBetween = emaBetween ? emaBetween * 0.98 + lastBetween * 0.02 : lastBetween;
        emaReal = emaReal ? emaReal * 0.98 + cpMs * 0.02 : cpMs;
        lastBetween = -1;
        if (emaReal > 0.5)
            s_betweenShare.store(float(std::clamp(emaBetween / emaReal, 0.25, 1.25)), std::memory_order_relaxed);
    }
    Sum& s = sums[between ? 1 : 0];
    s.n++;
    s.draws += draws;
    s.passes += passes;
    s.cp += cpMs;
    s.cpMax = std::max(s.cpMax, cpMs);
    if (++pictures % 1200)
        return;
    auto avg = [](const Sum& x, double v) { return x.n ? v / double(x.n) : 0.0; };
    fprintf(stderr, "[hfr] command processor per picture: in-between %llu, %.2f ms (max %.2f), %.0f draws, %.1f passes; real %llu, "
                    "%.2f ms (max %.2f), %.0f draws, %.1f passes; passes left out: %llu shadow, %llu view 4, %llu environment, %llu luminance; %llu real pictures took the last luminance reading\n",
        (unsigned long long)sums[1].n, avg(sums[1], sums[1].cp), sums[1].cpMax, avg(sums[1], double(sums[1].draws)),
        avg(sums[1], double(sums[1].passes)), (unsigned long long)sums[0].n, avg(sums[0], sums[0].cp), sums[0].cpMax,
        avg(sums[0], double(sums[0].draws)), avg(sums[0], double(sums[0].passes)), (unsigned long long)s_skipped[0].exchange(0),
        (unsigned long long)s_skipped[1].exchange(0), (unsigned long long)s_skipped[2].exchange(0), (unsigned long long)s_skipped[3].exchange(0),
        (unsigned long long)s_staleLuminance.exchange(0));
    sums[0] = sums[1] = {};
}
