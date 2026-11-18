// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Settings > Graphics > Shadow Resolution's Auto (the default since v0.2.4,
// GitHub #90), as gpu/renderer.cpp applies it: while the scene renders at 2x
// or more, the wider map, 9/4 x 1 (3600x1600), on Apple GPUs and the 2x map
// (3200x3200) on every other GPU; else 1x (1600x1600). Since v0.2.2 the
// 1600x1600 map is fitted to the whole view (#81): about 2.25 times the reach
// across the view (the map's x), the same along it (its y), so about 43 % of
// v0.2.1's texels per metre across near the car. 9/4 across gives 94 to 100 %
// of them back with 2.25 times the map's texels; 2x gives 83 to 89 % across
// and twice v0.2.1's along with 4 times (#90).
//
// Which one follows what each GPU had room for: the one that improves the
// picture without costing performance (#90). On an M1 Pro at 2560x1440 and
// 2x the 2x map was +3.4 to +3.9 ms of GPU a race frame and +190 to +280 MB
// over 1x, the room the frame had; 9/4 x 1 at 1920x1080 and 2x +0.9 to +1.0
// ms where 2x was +2.1 to +2.3. The Steam Machine at 3440x1440 (Auto
// 2.688x2) pays +1.2 to +1.6 ms for the 2x map and still holds 60.0 fps with
// no late frames (GPU 9.6 of 16.7 ms), with the better picture; a GTX 1070
// held 60 at 2x/2x (#84). An Intel UHD 620 on Windows, already GPU-bound at
// ~28 fps at 1x, lost 14.8 ms at the 2x map (#90): Auto keeps it at 1x
// there (its Internal Resolution budget is 1). Vulkan-free, for
// tests/shadow_auto_test.cpp.
//
// "2x or more" is the Internal Resolution step: the axis Vert+ or Hor+
// doesn't stretch (gpu/anisotropy_auto.h), so the smaller of the two (2x on
// 16:10 is 2x2.222, on 21:9 2.6875x2; Auto's whole 3x2 at 3440x1440 is 2x;
// Auto's 2x1 at 2560x720 is 1x).
//
// Not on a Steam Deck or a Steam Frame (video/steam_device.h): 1x there at
// any output. Docked to a 1080p screen both render at 2x2, and the 2x map
// cost the Deck ~5.4 ms of GPU and ~12 fps in races (53 -> 41 fps; the Frame
// is hotter and slower still at 1080p; #90); the wider map still costs frames
// in its heaviest city scene (#90).
//
// Shadow Edges' Auto (ShadowEdgesSmooth, GitHub #90): Smooth (the receivers'
// PCF) everywhere but a Steam Deck. Measured with the early out and shared
// weights: on the Steam Machine and the Steam Frame PCF costs less GPU time
// than the game's own 9 samples (its 4 gathers fetch less); on the Deck races
// hold 60 with ~0.1 ms more, but its busiest in-engine city scenes, already
// near the frame's budget, get ~11 one-frame hitches in 50 s instead of ~1,
// so Auto keeps the 360's edges there.
#pragma once
#include <gpu/render_scale.h>
#include <video/steam_device.h>

#include <cstdint>

namespace gpu
{
    // Auto's map on Apple GPUs where it isn't 1x: 9/4 across the view (the
    // map's x), 1 along it: 3600x1600.
    inline constexpr RenderScale kAutoShadowWide{ { 9, 4 }, { 1, 1 } };

    // Shadow Resolution's Auto for a scene rendered at `scene` on an Apple GPU
    // or not (appleGpu, as HostScaledResolves takes it) and on `device`: the
    // map's scale, an axis each. (The setting's own steps, 1x / 1.5x / 2x,
    // are ShadowScale's and square.)
    inline RenderScale AutoShadowScale(const RenderScale& scene, bool appleGpu, video::SteamDevice device = video::SteamDevice::Other)
    {
        if (device != video::SteamDevice::Other)
            return RenderScale::Whole(1, 1);
        const auto atLeastTwo = [](const AxisScale& a) { return a.num >= 2 * a.den; };
        if (!atLeastTwo(scene.x) || !atLeastTwo(scene.y))
            return RenderScale::Whole(1, 1);
        return appleGpu ? kAutoShadowWide : RenderScale::Whole(2, 2);
    }

    // Shadow Edges (user/settings.h ShadowEdgesMode: 0 Auto, 1 Smooth, 2
    // Classic): whether the sun shadow receivers take their PCF translation.
    inline bool ShadowEdgesSmooth(int32_t setting, video::SteamDevice device = video::SteamDevice::Other)
    {
        if (setting == 1 || setting == 2)
            return setting == 1;
        return device != video::SteamDevice::Deck;
    }
}
