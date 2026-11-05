// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Settings > Graphics > Anisotropic Filtering's Auto, as gpu/renderer.cpp's
// GetSampler applies it. The device's own Auto (16x on a discrete GPU or an
// M-series iPad, 4x on other integrated GPUs, 2x on Windows' Intel iGPUs) is
// raised to 16x on a Mac while the scene renders at 1x Internal Resolution:
// there each texel covers the most screen and anisotropy decides how roads
// and markings read (M1 Pro, 1x, 4x vs 16x, 3+3 runs: road 20-80 m ahead
// Laplacian x1.56, centre dashes x3.3, crosswalk x1.4; mean RGB within 0.1;
// GPU time within the run-to-run noise). Above 1x the scale already sharpens
// and 16x measured ~+0.5 ms for +3% (2x2), so Auto keeps the device's value.
// Other platforms: unchanged until measured there. Vulkan-free, for
// tests/anisotropy_auto_test.cpp.
//
// "1x" is the Internal Resolution's 1x, at every screen shape: on a screen
// taller than 16:9 (Vert+: a 16:10 MacBook's 1280x800 is 1x10/9) the scale
// keeps 1 across and stretches only its rows to the screen's shape, on a
// wider one (Hor+: an ultrawide's 1720x720 is 1.34375x1) it keeps 1 down
// (video::NativeShape, video::NativeAuto). So the scene is at 1x when either
// axis is exactly 1 (SceneAtOneX), not only when both are
// (RenderScale::IsOne: 16:9). At 1.5x and above neither axis is 1 at any
// shape (1.5x on 16:10: 1.5x1.667; 2x on 21:9: 2.6875x2).
#pragma once
#include <gpu/render_scale.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace gpu
{
    // `deviceAuto`: the device's Auto; `mac`: macOS (not iOS); `oneX`: the
    // scene renders at 1x (SceneAtOneX).
    constexpr int AutoAnisotropy(int deviceAuto, bool mac, bool oneX)
    {
        return mac && oneX && deviceAuto < 16 ? 16 : deviceAuto;
    }

    // The scale is Internal Resolution's 1x on a screen of any shape: 1 on
    // the axis Vert+ or Hor+ doesn't stretch (neither is below 1).
    inline bool SceneAtOneX(const RenderScale& s)
    {
        return s.x.IsOne() || s.y.IsOne();
    }

#if defined(__APPLE__) && !TARGET_OS_IOS
    constexpr bool kAnisotropyMac = true;
#else
    constexpr bool kAnisotropyMac = false;
#endif
}
