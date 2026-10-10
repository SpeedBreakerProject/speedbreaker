// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Where the game's picture goes on a screen of any shape, and the view
// scales it is rendered with (game/ultrawide). Pure functions, for
// tests/picture_fit_test.cpp.
//
// The game renders a 16:9 frame (1280x720). With Aspect Ratio on Auto ("Fill
// Screen") a screen of another shape is filled: the frame is stretched to it
// and the game's view is changed so the stretch doesn't distort it.
//   - Wider than 16:9 by more than 1% (an ultrawide, a 19.5:9 iPhone): Hor+,
//     scaleX = 16:9 / screen aspect.
//   - Narrower than 16:9 by more than kTallMinStretch, 1% (16:10: the Steam
//     Deck, 16:10 Macs; a 3:2 screen, a 4:3 iPad): Vert+, scaleY = screen
//     aspect / 16:9, the same horizontal view with more above and below it.
//     Down to 4:3: a screen narrower still (5:4, portrait) shows a 4:3
//     picture, letterboxed.
// Otherwise (16:9 within those margins, the 16:9 setting, or a ppc/ without
// the hooks) the 16:9 frame is letterboxed as it always was.
#pragma once
#include <cstdint>

namespace video
{
    // The narrowest picture Vert+ makes: 4:3, at most 4/3 the vertical
    // view's tan of the half-angle.
    constexpr float kTallMinAspect = 4.0f / 3.0f;
    // Vert+ when 16:9 is wider than the screen by more than this factor,
    // the same 1% margin as Hor+: 16:10 (1.11: the Steam Deck's 1280x800,
    // 16:10 Macs), 3:2 (1.19) and a 4:3 iPad (1.33) fill. 16:9 itself, and
    // a screen within 1% of it (1360x768), keeps the letterboxed 16:9 frame.
    // On the Deck, AutoRenderScale keeps 1x1, so the frame's 720 rows are
    // stretched to 800 (1.11) rather than drawn 1:1 in a 720-row box.
    constexpr float kTallMinStretch = 1.01f;

    struct PictureFit
    {
        // The view scales the frame is rendered with (game::SetAspectScale):
        // at most one is below 1.
        float scaleX = 1.0f, scaleY = 1.0f;
        // The picture on the screen, centred.
        int32_t width = 0, height = 0;
    };

    // The fit for a new frame of frameW x frameH (its shape: 1280x720 at any
    // internal resolution) on a screenW x screenH swapchain. fillWide and
    // fillTall: the setting is Auto and the game has the hooks for Hor+ and
    // Vert+ (game::HooksActive, game::TallHooksActive).
    PictureFit FitPicture(uint32_t screenW, uint32_t screenH, float frameW, float frameH, bool fillWide, bool fillTall);

    // Where a frame rendered with the view scales (scaleX, scaleY) goes: the
    // frame letterboxed for 1, 1; else stretched to the shape of the view it
    // shows, which fills the screen (within a pixel) unless it was rendered
    // for another one (a redraw after a resize) or clamped to 4:3.
    PictureFit PlacePicture(uint32_t screenW, uint32_t screenH, float frameW, float frameH, float scaleX, float scaleY);

    // Auto internal resolution: the render scale (sx x sy host pixels per
    // guest pixel, 1-3 each) for a picture covering regionW x regionH of the
    // screen (video::FrameRegion), within `budget` host pixels per guest
    // pixel.
    void AutoRenderScale(uint32_t regionW, uint32_t regionH, uint32_t budget, uint32_t& sx, uint32_t& sy);
}
