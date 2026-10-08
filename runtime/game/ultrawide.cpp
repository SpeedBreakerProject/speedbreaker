// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See ultrawide.h.
//
// Three mid-asm hooks (config/nfsmw.toml):
//   - UltrawideProjection, before `stfs f12,0(r30)` at 0x8243EE18 in the view
//     setup (sub_8243EAC0, CreateViewMatricies in the PC symbols): f12 is the
//     projection's x scale, P[0][0] = 1 / tan(half the horizontal FOV),
//     stored once and not used again as a register; f10 is 1 / tan(half the
//     vertical FOV), which the next instructions negate (`fneg f9,f10`) and
//     store as P[1][1] (`stfs f9,84(r31)`; the game's y is flipped) and
//     which is overwritten before it is read again. Narrowing f10's magnitude
//     by scaleY widens the vertical FOV and leaves the horizontal one. The
//     view-projection and the frustum planes are built from the stored
//     matrix afterwards, so culling widens with the view (the top and bottom
//     planes from its column 1); the LOD distance scale (sub_8243EA08, from
//     the horizontal FOV only) stays. r28 is the view (the function's second
//     argument); its id is at +4. Only the views the game itself widens for
//     16:9 are changed: 1, 2, 4, 5 and 17 (the player's camera, the road
//     reflection and the shadow frustum among them), so the shadow maps
//     (13-16) and the env cube (18-23) keep theirs.
//   - UltrawideFrontEnd, at the branch at 0x82440E4C that ends the
//     widescreen case of the front-end matrix (sub_82440D80): f13 is its x
//     scale (0.92 safe area x 0.75, a 4:3 canvas centred in 16:9) and f0 its
//     y scale (0.92), stored at m00 and m11 (shader constants c0 and c5).
//   - UltrawideMirror, at the branch at 0x8245CED8 that ends the widescreen
//     case of the rear-view mirror draw (sub_8245CDF0). The mirror is not a
//     view: its picture is one quad whose pixel shader (ps_40d7f669a326708d)
//     samples the car's environment cube map (views 18-23) along per-corner
//     directions fixed in the vertex data, drawn through a pass-through
//     vertex shader (vs_58da94362438089c) in clip coordinates the CPU builds
//     (sub_8245CA98: x = 2 * f1 - 1, width = 2 * f3; 0.638 and -0.275, so x
//     runs from +0.276 to -0.274, mirrored; y = 2 * f2 - 1, height = 2 * f4).
//     The front-end matrix never touches it, so without this the picture
//     stays 16:9-wide while its frame (a HUD sprite) narrows, and the stretch
//     puts it 1.34x past the frame. Narrowed about the screen centre like the
//     HUD; the directions stay, so the picture keeps its proportions after
//     the stretch.
//
// A ppc/ generated before Vert+ calls the hooks with the x registers only
// (the overloads below): Hor+ keeps working and TallHooksActive() stays
// false, so the presenter letterboxes a narrower screen as before instead
// of the build failing on an undefined symbol.
#include <stdafx.h>
#include "ultrawide.h"

#include <kernel/function.h>

namespace
{
    // Movies (the XDK's XMV player; every file in Movies/ is 16:9 WMV3) are
    // fitted by the player itself. At each movie's start its renderer setup
    // (sub_826D8078) loads the display aspect, 16/9, from this .rdata float
    // (`lfs f0` at 0x826D8114, its only reader) and keeps display aspect /
    // backbuffer aspect at renderer+140; the auto-fit (sub_826D8640, run from
    // the format callback sub_826DB140 before the first frame) pillarboxes a
    // movie narrower than that and letterboxes one wider (loc_826D86C8: the
    // full width, centred rows). Given the aspect the 1280x720 frame is shown
    // at, it draws the movie into the centre 1280 * scaleX columns or 720 *
    // scaleY rows, as the HUD is, so after the presenter's stretch every frame
    // of it is 16:9 between black bars, with no per-frame decision.
    constexpr uint32_t kMovieDisplayAspect = 0x8200FAC0;

    // Both scales in one atomic, so a hook never sees one from before a change
    // and the other from after it.
    struct Scales
    {
        float x, y;
    };
    std::atomic<Scales> s_scales{ Scales{ 1.0f, 1.0f } };
    std::atomic<bool> s_hooksSeen{ false };
    std::atomic<bool> s_tallHooksSeen{ false };
    std::atomic<float> s_frontEndX{ 0.0f };
    std::atomic<float> s_frontEndY{ 0.0f };

    // Log each view id once, with its target size (the per-view table at
    // 0x82C3F590 + id * 384: target pointer at +368, width/height at +16/+20).
    void LogView(uint32_t id, bool widened)
    {
        static std::atomic<uint64_t> seen{ 0 };
        if (id >= 64 || (seen.fetch_or(1ull << id) & (1ull << id)))
            return;
        uint32_t target = ByteSwap(*static_cast<const uint32_t*>(g_memory.Translate(0x82C3F590 + id * 384 + 368)));
        uint32_t w = 0, h = 0;
        if (target)
        {
            w = ByteSwap(*static_cast<const uint32_t*>(g_memory.Translate(target + 16)));
            h = ByteSwap(*static_cast<const uint32_t*>(g_memory.Translate(target + 20)));
        }
        fprintf(stderr, "[uw] view %u: target %ux%u%s\n", id, w, h, widened ? ", widened" : "");
    }

    // The view at `view` (guest address) is one the game widens for 16:9.
    bool WidenedView(uint32_t view)
    {
        uint32_t id = ByteSwap(*static_cast<const uint32_t*>(g_memory.Translate(view + 4)));
        bool widen = id == 1 || id == 2 || id == 4 || id == 5 || id == 17;
        LogView(id, widen);
        return widen;
    }

    // `stfs` stores the register as a float.
    void ScaleRegister(PPCRegister& f, float scale)
    {
        f.f64 = double(float(f.f64) * scale);
    }

    // About the screen centre (0.5 of it), for the mirror's 0..1 edge.
    void ScaleAboutCentre(PPCRegister& f, float scale)
    {
        f.f64 = double(0.5f + (float(f.f64) - 0.5f) * scale);
    }
}

namespace game
{
    void SetAspectScale(float scaleX, float scaleY)
    {
        const Scales next{ std::clamp(scaleX, 0.1f, 1.0f), std::clamp(scaleY, 0.5f, 1.0f) };
        const Scales last = s_scales.exchange(next, std::memory_order_relaxed);
        if (last.x != next.x || last.y != next.y)
            fprintf(stderr, "[uw] view scale x %.4f y %.4f\n", next.x, next.y);
        // The movie player's display aspect, stored with every decision (one
        // 4-byte store per presented frame) so nothing depends on when the
        // image was loaded. Read once per movie, at its start: a change made
        // while a movie plays applies from the next movie.
        float aspect = (16.0f / 9.0f) * next.y / next.x;
        uint32_t bits;
        memcpy(&bits, &aspect, sizeof(bits));
        *static_cast<uint32_t*>(g_memory.Translate(kMovieDisplayAspect)) = ByteSwap(bits);
    }

    float AspectScaleX()
    {
        return s_scales.load(std::memory_order_relaxed).x;
    }

    float AspectScaleY()
    {
        return s_scales.load(std::memory_order_relaxed).y;
    }

    bool HooksActive()
    {
        return s_hooksSeen.load(std::memory_order_relaxed);
    }

    bool TallHooksActive()
    {
        return s_tallHooksSeen.load(std::memory_order_relaxed);
    }

    float FrontEndScaleX()
    {
        return s_frontEndX.load(std::memory_order_relaxed);
    }

    float FrontEndScaleY()
    {
        return s_frontEndY.load(std::memory_order_relaxed);
    }
}

void UltrawideProjection(PPCRegister& r28, PPCRegister& f12, PPCRegister& f10)
{
    if (!s_tallHooksSeen.load(std::memory_order_relaxed))
    {
        s_hooksSeen.store(true, std::memory_order_relaxed);
        s_tallHooksSeen.store(true, std::memory_order_relaxed);
    }
    const Scales scale = s_scales.load(std::memory_order_relaxed);
    if (scale.x == 1.0f && scale.y == 1.0f)
        return;
    if (!WidenedView(r28.u32))
        return;
    if (scale.x != 1.0f)
        ScaleRegister(f12, scale.x);
    if (scale.y != 1.0f)
        ScaleRegister(f10, scale.y);  // its magnitude: negated into P[1][1] after this
}

void UltrawideFrontEnd(PPCRegister& f13, PPCRegister& f0)
{
    const Scales scale = s_scales.load(std::memory_order_relaxed);
    if (scale.x != 1.0f)
        ScaleRegister(f13, scale.x);
    if (scale.y != 1.0f)
        ScaleRegister(f0, scale.y);
    s_frontEndX.store(float(f13.f64), std::memory_order_relaxed);
    s_frontEndY.store(float(f0.f64), std::memory_order_relaxed);
}

void UltrawideMirror(PPCRegister& f1, PPCRegister& f2, PPCRegister& f3, PPCRegister& f4)
{
    const Scales scale = s_scales.load(std::memory_order_relaxed);
    if (scale.x != 1.0f)
    {
        ScaleAboutCentre(f1, scale.x);
        ScaleRegister(f3, scale.x);
    }
    if (scale.y != 1.0f)
    {
        ScaleAboutCentre(f2, scale.y);
        ScaleRegister(f4, scale.y);
    }
}

// The hooks as a ppc/ generated before Vert+ calls them: x only.
void UltrawideProjection(PPCRegister& r28, PPCRegister& f12)
{
    if (!s_hooksSeen.load(std::memory_order_relaxed))
        s_hooksSeen.store(true, std::memory_order_relaxed);
    const float scale = s_scales.load(std::memory_order_relaxed).x;
    if (scale == 1.0f)
        return;
    if (WidenedView(r28.u32))
        ScaleRegister(f12, scale);
}

void UltrawideFrontEnd(PPCRegister& f13)
{
    const float scale = s_scales.load(std::memory_order_relaxed).x;
    if (scale != 1.0f)
        ScaleRegister(f13, scale);
    s_frontEndX.store(float(f13.f64), std::memory_order_relaxed);
}

void UltrawideMirror(PPCRegister& f1, PPCRegister& f3)
{
    const float scale = s_scales.load(std::memory_order_relaxed).x;
    if (scale == 1.0f)
        return;
    ScaleAboutCentre(f1, scale);
    ScaleRegister(f3, scale);
}
