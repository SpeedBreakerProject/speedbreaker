// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// HFR (v018-hfr, path A of the 120 fps study): pictures between the world's
// 1/60 s steps. Off unless NFSMW_HFR=1 (see game/hfr.cpp).
#pragma once
#include <cstdint>

namespace hfr
{
    // At launch, after the settings load (main.cpp): Settings > Display >
    // Smooth Motion (NFSMW_HFR=1), for this run (a Restart setting).
    void Init();
    // Smooth Motion (NFSMW_HFR=1): each world step is drawn more than once where the display
    // can show it (video::HfrPicturesPerStep), first between the last step
    // and this one, then as it is.
    bool Enabled();
    // Enabled(), or NFSMW_HFR_RENDER2=1 (the study's diagnostic: drawn twice,
    // the same both times).
    bool Render2();
    // Any thread: an in-between picture's render is under way (the main loop
    // thread draws it, the render thread plays its passes back, and both see
    // this until it has been played).
    bool InBetween();
    // Its fraction of the step (0.5: halfway); 0 when there is none.
    float InBetweenFraction();
    // Any thread: a step drawn only between (a display that isn't a whole
    // multiple of 60 Hz): its own picture's fraction while it is drawn
    // (its swap is the step's, released by the vblank); 0 otherwise.
    float OwnFraction();
    // Any thread: the refresh (steady ns) the picture being drawn is for, on a
    // display at a rate that isn't a whole multiple of 60 Hz (video::HfrGrid);
    // 0 otherwise.
    int64_t PictureTarget();
    // The command processor reached a swap (NFSMW_HFR_TIMELINE's lines).
    void NoteSwap(float between);
    // Command processor, at each real picture's swap: the command
    // processor's busy time and the GPU's since the last one (ms), and
    // whether an in-between picture was drawn between them. Decides whether
    // the next frames can afford in-between pictures (game/hfr.cpp).
    void NoteFrameCost(double cpMs, double gpuMs, bool hadBetween);
    // Presenter (off Apple's systems): a picture of Smooth Motion's was
    // shown on another refresh than the others (a ghost; one merely ready
    // late is only counted). The game goes to an even 60 at once, and back
    // only after a clean while (game/hfr.cpp, RequestSixty).
    void NoteLatePicture();
    // Presenter (off Apple's systems): the vblank lock moved new frames to
    // another refresh (VblankLock::AdaptHold, a refresh later or earlier):
    // a cut of ours, an even 60 for a moment, never counted as a ghost.
    void NoteHoldChange();
    // Command processor, at every swap: that picture's own busy time (ms),
    // draws and passes (the work since the last swap), for the [hfr] line
    // that compares in-between pictures' cost with real ones'.
    void NotePictureCost(bool between, double cpMs, uint64_t draws, uint64_t passes);
}

namespace hfrdiag
{
    // game/hfr_diag.cpp (diagnostics; each off unless its variable is set).
    void AfterRenderEntry(uint8_t* base);
    void OnViewSetup(uint8_t* base, uint32_t view);
    // game/hfr_writeset.cpp (NFSMW_HFR_WRITESET): the phase of a measurement
    // a frame plays (0 none, 1 in-between, 2 control, 3 noise), and its steps.
    int WriteSetPhase(uint64_t frame);
    void WriteSetBeforeBetween(uint8_t* base);
    void WriteSetAfterBetween(uint8_t* base);
    void WriteSetAfterReal(uint8_t* base);
    void WriteSetControlBefore(uint8_t* base);
    void WriteSetControlAfter(uint8_t* base);
    void WriteSetNoise(uint8_t* base);
}
