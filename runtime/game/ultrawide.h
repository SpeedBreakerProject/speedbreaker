// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Screens of another shape than 16:9 (Aspect Ratio on Auto, "Fill Screen";
// video/picture_fit decides): the game keeps rendering its 1280x720 frame,
// the presenter stretches it to fill the screen, and the view is changed so
// the stretch doesn't distort it.
//   - Hor+, a screen wider than 16:9 (ultrawide): the projection's horizontal
//     scale is narrowed by scaleX = (16/9) / screen aspect, so the view
//     widens to the screen's aspect.
//   - Vert+, a screen narrower than 16:9 (a 16:10 Steam Deck or Mac, a 4:3
//     iPad): the projection's vertical scale is narrowed by scaleY = screen
//     aspect / (16/9), so the view grows taller; the horizontal view stays.
// The HUD's matrix is narrowed by the same factor, so after the stretch it
// keeps its shape and size in a centred 16:9 block, and so is the rear-view
// mirror's picture. Movies are fitted by the movie player, told the screen's
// aspect: pillarboxed on a wider screen, letterboxed on a narrower one.
#pragma once

namespace game
{
    // The view scales: (16/9) / screen aspect in x (Hor+; 0.744 at
    // 3440x1440), screen aspect / (16/9) in y (Vert+; 0.75 on a 4:3 iPad, 0.9
    // on a Steam Deck); 1 and 1 for 16:9 (no change). At most one is below 1.
    // Any thread (the presenter sets them with each new frame; the game's
    // render thread reads them). Guest memory must be set up (it also writes
    // the movie player's display aspect).
    void SetAspectScale(float scaleX, float scaleY);
    float AspectScaleX();
    float AspectScaleY();
    // The hooks have run (the recompiled code has them): without them a
    // stretched frame would just be distorted.
    bool HooksActive();
    // The hooks that also take the vertical scale have run. A ppc/ generated
    // from a config/nfsmw.toml before Vert+ has the horizontal ones only: a
    // screen narrower than 16:9 is then letterboxed as before.
    bool TallHooksActive();
    // The front-end (HUD and menu) matrix's x and y scales as
    // UltrawideFrontEnd last left them (0 before the first front-end frame):
    // the renderer recognises front-end draws by them.
    float FrontEndScaleX();
    float FrontEndScaleY();
}
