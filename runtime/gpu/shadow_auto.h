// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Settings > Graphics > Shadow Resolution's Auto (the default since v0.2.4,
// GitHub #90), as gpu/renderer.cpp applies it: 2x while the scene renders at
// 2x or more, else 1x. Since v0.2.2 the 1600x1600 map is fitted to the whole
// view (#81), about half the 360's texels per metre at 16:9; 2x gives them
// back. Its cost follows the GPU that Internal Resolution's Auto already
// judged: M1 Pro at 2x ~+2 ms of GPU, 60 fps held (issues/81); a GTX 1070
// held 60 at 2x/2x (#84); an Intel UHD 620 on Windows, already GPU-bound at
// ~28 fps at 1x, lost 14.8 ms at 2x (issues/90), and Auto keeps it at 1x
// there (its Internal Resolution budget is 1). Vulkan-free, for
// tests/shadow_auto_test.cpp.
//
// "2x or more" is the Internal Resolution step: the axis Vert+ or Hor+
// doesn't stretch (gpu/anisotropy_auto.h), so the smaller of the two (2x on
// 16:10 is 2x2.222, on 21:9 2.6875x2; Auto's whole 3x2 at 3440x1440 is 2x;
// Auto's 2x1 at 2560x720 is 1x).
//
// Not on a Steam Deck or a Steam Frame (video/steam_device.h): 1x there at
// any output. Docked to a 1080p screen both render at 2x2, and 2x shadows
// cost the Deck ~5.4 ms of GPU and ~12 fps in races (53 -> 41 fps; the Frame
// is hotter and slower still at 1080p: issues/90/v024/dev).
#pragma once
#include <gpu/render_scale.h>
#include <video/steam_device.h>

#include <cstdint>

namespace gpu
{
    // Shadow Resolution's Auto in half steps (user/settings.h: 2 is 1x, 4 is
    // 2x) for a scene rendered at `scene` on `device`.
    inline uint32_t AutoShadowHalves(const RenderScale& scene, video::SteamDevice device = video::SteamDevice::Other)
    {
        if (device != video::SteamDevice::Other)
            return 2;
        const auto atLeastTwo = [](const AxisScale& a) { return a.num >= 2 * a.den; };
        return atLeastTwo(scene.x) && atLeastTwo(scene.y) ? 4 : 2;
    }
}
