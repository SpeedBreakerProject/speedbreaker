// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// iOS: what UIKit needs told for the runtime's own screens (ios_ui.mm).
// Main thread.
#pragma once
#include <string>

struct SDL_Window;

namespace platform::ios
{
    // Turns iOS's editing gestures off over `window`. Since iOS 13 a
    // three-finger tap brings up the edit menu and a three-finger swipe or
    // double tap is undo/redo, and the system takes those touches from the
    // app (they arrive cancelled), so the three-finger tap that opens the
    // settings would never get through. UIKit asks the first responder,
    // which SDL leaves unset outside text input, whether to: here the game's
    // view becomes it and answers no (its view controller answers no too).
    // Call once the window is up and then every frame (ui::Tick): something
    // else can take first responder for a while (SDL's hidden text field
    // while text input is on, the Files picker), and this takes it back
    // after. Cheap once done: a check that the view still has it.
    void DisableSystemEditGestures(SDL_Window* window);

    // The present scale (video/presenter.cpp: Screen Resolution, or
    // NFSMW_PRESENT_SCALE): the game's Metal view at `fraction` of the
    // screen's pixels each way (its contentScaleFactor; a hair less where
    // the drawable sizes would round two ways), which Core Animation scales
    // up to the screen. SDL sizes the drawables from it, and MoltenVK's
    // surface extent follows them. Call after SDL_Vulkan_CreateSurface
    // (which makes the view), before each swapchain: it snaps again only
    // when the view's bounds or `fraction` changed (1: the screen's own
    // scale again). False: no Metal view.
    bool SetDrawableScale(SDL_Window* window, float fraction);

    // The default Metal device's name, as MoltenVK names the Vulkan device
    // ("Apple A17 Pro GPU", "Apple M2 GPU"), before Vulkan starts (Frame
    // Rate's default follows it: main.cpp). "" without one. Any thread.
    std::string GpuName();

    // An iPhone (UIDevice's interface idiom), not an iPad: Frame Rate's
    // default (main.cpp). Main thread.
    bool IsPhone();
}
