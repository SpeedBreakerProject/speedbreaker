// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Settings > Graphics > Upscaling's FSR 1: AMD FidelityFX Super Resolution 1
// (runtime/thirdparty/fsr1, MIT, unmodified), as two passes after the
// anti-aliasing (video/presenter.cpp):
//   - EASU (compute), the frame image enlarged into an image the size of
//     the picture on the screen: AMD's FsrEasuF (32-bit), whose 12 taps
//     come from 4 gathers, or under Hor+ and Vert+ from fetches kept in the
//     pixel's span of the 16:9 band's three (video/band_filter.h);
//   - RCAS (fragment, into the swapchain image, where bicubic's pass draws),
//     AMD's FsrRcasF: sharpening of that image, its 4 neighbours each kept
//     in the pixel's span too; then Brightness and the 360's display gamma
//     (the tone variant, as for the other scale shaders), then the UI.
// Their constants come from AMD's own CPU code (FsrEasuCon, FsrRcasCon).
#pragma once
#include <array>
#include <cstdint>
#include <string>

namespace video::fsr1
{
    // EASU's push constants: FsrEasuCon's con0..con3 for a srcW x srcH
    // image enlarged to dstW x dstH.
    std::array<uint32_t, 16> EasuConstants(uint32_t srcW, uint32_t srcH, uint32_t dstW, uint32_t dstH);
    // RCAS's: FsrRcasCon's con for a strength in stops below its maximum
    // (video::upscale::RcasStops; infinity gives 0: no sharpening).
    std::array<uint32_t, 4> RcasConstants(float stops);

    // The shaders' GLSL: AMD's two headers between ours (the RCAS one has a
    // single main(), so the tone variant can wrap it).
    std::string EasuGlsl();
    std::string RcasGlsl();
    std::string UiEasuGlsl();  // UI-C's pass (presenter.cpp, UiPass): EASU where the UI mask is set
}
