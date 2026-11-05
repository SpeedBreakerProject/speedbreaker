// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Which pass draws the game's picture into the window (video/presenter.cpp),
// and Settings > Graphics > Sharpening as FSR 1's RCAS strength. Pure
// functions, for tests/upscale_test.cpp.
//
// Settings > Graphics > Upscaling: Bicubic (Catmull-Rom, sharpened and
// clamped to the 2x2 texels around each sample), Bilinear, or FSR 1 (AMD
// FidelityFX Super Resolution 1: EASU, an edge-adaptive Lanczos-like
// upscale, then RCAS, a contrast-adaptive sharpening pass; video/fsr1.h).
// FSR 1 is made for enlarging (AMD: 1x to 4x the pixels): it is used only
// when the picture on the screen is larger than the frame image, along one
// axis or both, and smaller along neither (1x Internal Resolution on a
// 1080p, 1440p or MacBook screen; the Steam Deck's 720 rows stretched to
// 800). A frame shown at its own size, or shrunk (2x on a 1080p screen),
// takes the bicubic pass exactly as before FSR 1 existed.
//
// Settings > Graphics > Movie Upscaling (MoviePass): while a movie plays
// (game::LastMovieFrameNs() fresh) and its frame is shown at 1x (the XMV
// player's 1280x720 picture, which reaches the presenter unscaled), Sharper
// enlarges it with FSR 1's EASU (RCAS off: kMovieRcas) where FSR 1 is
// offered, or a clamped Lanczos-2 (the scale pass's third mode) where it
// isn't (iPhone, iPad). Original: the Upscaling setting's pass, as before.
// Above 1x the movie is drawn into the scaled target (its quad enlarges it),
// so the presenter sees a host-resolution frame: there gpu/renderer.cpp
// gives the movie quad Sharper UI's filter instead (gpu/sharp_ui.h).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace video::upscale
{
    // Settings > Graphics > Upscaling's values (settings::ScalingMode).
    enum class Mode : int32_t { Bicubic = 0, Linear = 1, Fsr1 = 2 };

    enum class Pass : uint8_t { Linear, Bicubic, Fsr1, Lanczos };
    constexpr int kPassCount = 4;

    inline const char* PassName(Pass pass)
    {
        return pass == Pass::Fsr1 ? "FSR 1" : pass == Pass::Bicubic ? "bicubic" : pass == Pass::Lanczos ? "Lanczos" : "bilinear";
    }

    // FSR 1 isn't offered on iPhone and iPad (user/settings.cpp): there
    // Movie Upscaling's Sharper is the Lanczos pass.
#if defined(__APPLE__) && TARGET_OS_IOS
    constexpr bool kFsr1Offered = false;
#else
    constexpr bool kFsr1Offered = true;
#endif

    // The picture enlarged: dstW x dstH (its place on the screen) is larger
    // than the srcW x srcH image along one axis and smaller along neither.
    inline bool Enlarged(uint32_t srcW, uint32_t srcH, int32_t dstW, int32_t dstH)
    {
        if (!srcW || !srcH || dstW <= 0 || dstH <= 0)
            return false;
        const bool notSmaller = uint32_t(dstW) >= srcW && uint32_t(dstH) >= srcH;
        return notSmaller && (uint32_t(dstW) > srcW || uint32_t(dstH) > srcH);
    }

    // The pass for this frame. An unknown mode is bicubic, the default.
    inline Pass Choose(int32_t mode, uint32_t srcW, uint32_t srcH, int32_t dstW, int32_t dstH)
    {
        switch (Mode(mode))
        {
        case Mode::Linear:
            return Pass::Linear;
        case Mode::Fsr1:
            return Enlarged(srcW, srcH, dstW, dstH) ? Pass::Fsr1 : Pass::Bicubic;
        default:
            return Pass::Bicubic;
        }
    }

    // Sharpening (0..1, default 0.3) for RCAS: the strength AMD's FsrRcasCon
    // takes as stops below its maximum (0: the most it sharpens, each stop
    // half as much), here as the linear factor it becomes (2^-stops), which
    // multiplies RCAS's sharpening lobe: Sharpening 1 is RCAS's maximum,
    // 0.5 half of it, and 0 turns RCAS off (EASU's picture as it is), as 0
    // turns bicubic's sharpening off.
    inline float RcasAmount(float sharpening)
    {
        if (!(sharpening > 0.0f))  // 0, negative or NaN
            return 0.0f;
        return std::min(sharpening, 1.0f);
    }

    // The same as FsrRcasCon's stops (infinity: off).
    inline float RcasStops(float sharpening)
    {
        const float amount = RcasAmount(sharpening);
        return amount > 0.0f ? -std::log2(amount) : INFINITY;
    }

    // Movie Upscaling's values (settings::MovieUpscalingMode).
    enum class MovieMode : int32_t { Original = 0, Sharper = 1 };

    // RCAS's strength for a movie's EASU (Sharpening's scale: 0..1): none.
    // The WMV3 pictures carry 8x8 block edges in dark gradients; measured on
    // the attract movie at 4K (NOTES "Movie Upscaling"), RCAS at 0.1 brought
    // the block grid in a dark gradient back to bicubic's level (EASU alone
    // softens it a little) and moved mean RGB by up to 0.3 levels, for ~10%
    // more sharpness than EASU alone, which keeps mean RGB within 0.03.
    constexpr float kMovieRcas = 0.0f;

    // A movie is playing: its last frame rendered within this long (they
    // come every 33 ms at 30 fps; a held frame or a 2-vblank swap stays
    // well inside).
    constexpr int64_t kMovieFreshNs = 250'000'000;

    inline bool MoviePlaying(int64_t lastMovieFrameNs, int64_t nowNs)
    {
        return lastMovieFrameNs > 0 && nowNs >= lastMovieFrameNs && nowNs - lastMovieFrameNs <= kMovieFreshNs;
    }

    // The frame is at 1x: the image is the frame's own size (no internal
    // resolution scaling; aspect 0: the image's own).
    inline bool AtOneX(uint32_t imageW, uint32_t imageH, uint32_t aspectW, uint32_t aspectH)
    {
        return (aspectW == 0 || aspectW == imageW) && (aspectH == 0 || aspectH == imageH);
    }

    // The pass for a frame, Movie Upscaling included: Sharper while a movie
    // plays at 1x and is enlarged (EASU only enlarges, as Upscaling's FSR
    // 1); every other frame takes Choose's pass, unchanged. `fsrOffered`:
    // FSR 1 exists on this platform (kFsr1Offered).
    inline Pass ChooseWithMovie(int32_t mode, int32_t movieMode, bool movie, bool atOneX, bool fsrOffered, uint32_t srcW,
        uint32_t srcH, int32_t dstW, int32_t dstH)
    {
        if (movie && atOneX && MovieMode(movieMode) == MovieMode::Sharper && Enlarged(srcW, srcH, dstW, dstH))
            return fsrOffered ? Pass::Fsr1 : Pass::Lanczos;
        return Choose(mode, srcW, srcH, dstW, dstH);
    }
}
