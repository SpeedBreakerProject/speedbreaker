// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See picture_fit.h.
#include "picture_fit.h"

#include <algorithm>
#include <cmath>

namespace video
{
    PictureFit FitPicture(uint32_t screenW, uint32_t screenH, float frameW, float frameH, bool fillWide, bool fillTall)
    {
        float scaleX = 1.0f, scaleY = 1.0f;
        if (screenW && screenH)
        {
            const float screenAspect = float(screenW) / float(screenH);
            const float frameAspect = frameW / frameH;
            if (fillWide && screenAspect > frameAspect * 1.01f)
                scaleX = frameAspect / screenAspect;
            else if (fillTall && screenAspect < frameAspect / kTallMinStretch)
            {
                // At most 4:3 (a 2732x2048 iPad, 1.334, is 4:3 enough).
                const float aspect = screenAspect < kTallMinAspect * 0.999f ? kTallMinAspect : screenAspect;
                scaleY = aspect / frameAspect;
            }
        }
        return PlacePicture(screenW, screenH, frameW, frameH, scaleX, scaleY);
    }

    PictureFit PlacePicture(uint32_t screenW, uint32_t screenH, float frameW, float frameH, float scaleX, float scaleY)
    {
        PictureFit fit{ scaleX, scaleY };
        if (scaleX == 1.0f && scaleY == 1.0f)
        {
            // The frame as it is, letterboxed (the presenter's arithmetic
            // from before Vert+, so these pictures don't move by a pixel).
            const float scale = std::min(float(screenW) / frameW, float(screenH) / frameH);
            fit.width = int32_t(frameW * scale);
            fit.height = int32_t(frameH * scale);
            return fit;
        }
        // The frame shows a view of its own aspect x scaleY / scaleX: stretched
        // to that shape, as large as the screen takes.
        const double aspect = double(frameW) / double(frameH) * double(scaleY) / double(scaleX);
        double w = double(screenW), h = w / aspect;
        if (h > double(screenH))
        {
            h = double(screenH);
            w = h * aspect;
        }
        fit.width = int32_t(std::lround(w));
        fit.height = int32_t(std::lround(h));
        // Within a pixel of the screen (the scales' float rounding): all of
        // it, without a 1-pixel bar.
        if (fit.width >= int32_t(screenW) - 1 && fit.height >= int32_t(screenH) - 1)
        {
            fit.width = int32_t(screenW);
            fit.height = int32_t(screenH);
        }
        return fit;
    }

    void AutoRenderScale(uint32_t regionW, uint32_t regionH, uint32_t budget, uint32_t& sx, uint32_t& sy)
    {
        // Enough host pixels to cover the screen area the frame fills
        // (3440x1440 filled: 3x2; a 1440p 16:9 box: 2x2).
        sx = std::clamp((regionW + 1279) / 1280, 1u, 3u);
        sy = std::clamp((regionH + 719) / 720, 1u, 3u);
        // A picture narrower than 16:9 is Vert+ (a letterboxed 16:9 box is
        // 16:9 within a pixel's rounding): its 720 rows are stretched taller
        // than the 16:9 box of the same width, and a stretch of up to 1.125
        // of a scale's rows keeps that scale (1280x800: 1x1, not twice the
        // pixel work for an 11% taller picture; 2560x1600: 2x2, not 2x3).
        if (regionW && regionH && float(regionW) / float(regionH) < (16.0f / 9.0f) / 1.005f)
            sy = uint32_t(std::clamp(int(std::ceil(double(regionH) / (720.0 * 1.125))), 1, 3));
        // Within the budget of sx x sy host pixels per guest pixel.
        while (sx * sy > budget)
        {
            if (sx >= sy)
                sx--;
            else
                sy--;
        }
    }
}
