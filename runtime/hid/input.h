// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Controller input (Phase 6): SDL3 gamepads and the keyboard, presented to
// the game as XInput controller 0 through XamInputGetState.
//
//   - Sticks get a radial deadzone with rescaling (Settings > Input, or
//     NFSMW_DEADZONE; default 0.08 of full deflection), so worn or drifting
//     sticks rest at exactly zero while small movements still ramp up
//     smoothly from the edge of the deadzone. Triggers get their own
//     (NFSMW_TRIGGER_DEADZONE, default 0.02).
//   - Keyboard: arrows = D-pad + left stick, W/S = gas/brake (RT/LT), A/D =
//     steer, Space = A, Backspace = B, Left Shift = X, Left Ctrl = Y,
//     Q/E = LB/RB, Enter = START, Esc = BACK, C = right stick click.
//   - NFSMW_INPUT_SCRIPT="12:START,15.5:A,30-90/4:DOWN,100~160:RT": taps
//     (150 ms) at seconds since launch, taps every `step` seconds over a
//     range, or inputs held over a range; RT/LT (triggers) and LL/LR/LU/LD
//     (left stick) besides the buttons. For automated runs; works headless.
//     It feeds the game after the deadzones.
//   - NFSMW_VIRTUAL_PAD="2:LX=0.05,4:RT=0.06": an SDL virtual controller whose
//     axes take those values at those seconds, through the deadzones, and
//     whose rumble is logged with what the game asked for (tests of the
//     Input settings without a person). Buttons too ("5:BACK=1", "5.2:BACK=0",
//     "6:DOWN" for a tap), which reach the menus as a real controller's do.
//     Both count seconds of guest time (cpu/guest_time.h): a suspension
//     doesn't count, so what falls due during one happens after it.
//   - NFSMW_INPUT_LOG=1: the left stick and triggers once a second, raw and
//     as the game gets them.
//   - iOS: with no controller (and no keyboard attached) a toast says the
//     game needs one, 5 s after the game starts (NoteGameStarting) and 1 s
//     after the last one goes; one connecting takes it down.
#pragma once
#include <SDL3/SDL_events.h>

namespace hid
{
    // Main thread, after SDL video is up.
    void Initialize();
    // Main thread, as the game itself starts (after the installer, if it
    // ran): on iOS, the "needs a controller" notice is due 5 s later if
    // there is none by then. Elsewhere nothing.
    void NoteGameStarting();
    void HandleEvent(const SDL_Event& event);
    // Main thread, once per presented frame: sample devices.
    void Update();
    // While set (a menu is open), the game sees an idle controller, and
    // after it is cleared until every control has been released.
    void SetBlocked(bool blocked);
    // At exit: stop any rumble (the process may outlive nothing that would).
    void Shutdown();
    // Main thread, from the frame loop (not UIKit's callback), once the game
    // is suspended (iOS: the app isn't active) and once it's back: Suspend
    // stops any rumble, Resume sends the game's last request again.
    void Suspend();
    void Resume();
}
