// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// macOS: the game's window with its picture under a transparent title bar
// (apple/macos_window.mm; GitHub pull request #23 by aleszczynskig). The
// title isn't shown, the title bar is drawn over the top of the picture
// (NSWindowStyleMaskFullSizeContentView), and the close, minimise and zoom
// buttons sit over it only while the mouse is in use there (ui.cpp shows and
// hides them with the pointer). Fullscreen is macOS's own, unchanged. Main
// thread only (AppKit).
#pragma once

struct SDL_Window;

namespace platform::macos
{
    // Once, after SDL_CreateWindow: watches the window's fullscreen
    // transitions (for RestoreAfterFullscreen) and, when `windowed` (not
    // made fullscreen), puts the title bar over the picture now. The content
    // keeps its size (-setStyleMask: keeps the content rect), so the picture
    // is the size the window was made at, the saved size, and the frame
    // loses the title bar's height. A window made fullscreen gets the look
    // when it leaves fullscreen: not while its Space is opening.
    void ConfigureWindow(SDL_Window* window, bool windowed);

    // The look again after the window left fullscreen: SDL resets the
    // window's style around every fullscreen Space (SDL 3.4's Cocoa
    // windowWillEnterFullScreen and windowDidExitFullScreen), which takes
    // the full-size content view off. Call it from the game's event loop on
    // SDL_EVENT_WINDOW_LEAVE_FULLSCREEN, not from an event watch: those run
    // inside SDL's windowDidExitFullScreen, where putting it back left the
    // window one title bar shorter after every round trip (AppKit has just
    // restored the frame without it, and -setStyleMask: keeps the smaller
    // content) and the next entry into fullscreen sometimes crashed in
    // AppKit (SDL's reset then landing in the middle of the transition).
    // Here it keeps the window's frame. Does nothing (false) while a
    // fullscreen transition is under way (a toggle queued behind another):
    // the next SDL_EVENT_WINDOW_LEAVE_FULLSCREEN brings it.
    bool RestoreAfterFullscreen(SDL_Window* window);

    // Shows or hides the close, minimise and zoom buttons.
    void ShowWindowButtons(SDL_Window* window, bool shown);

    // The height in points of the title bar's strip over the picture (0
    // when there is none: fullscreen, or before the look is applied), for
    // laying the UI out below it.
    float TitleStripHeight(SDL_Window* window);

    // The window's screen refreshes at one fixed rate (macOS's minimum and
    // maximum refresh intervals agree) and isn't the Mac's built-in one: not
    // a ProMotion panel, nor a monitor with adaptive sync on. `minHz` and
    // `maxHz`, if given, get the range (for the log). False without a screen.
    bool DisplayRefreshFixed(SDL_Window* window, double* minHz = nullptr, double* maxHz = nullptr);

    // The window's screen's current EDR headroom (the brightest EDR colour
    // component over SDR white: 1 on a standard-range display, or without a
    // screen), for HDR Output's highlight range. Main thread.
    float DisplayHeadroom(SDL_Window* window);

    // The screen's potential EDR headroom: what it can show once an EDR layer
    // is up, even while the current headroom is still 1. 1 without a screen
    // or on a standard-range display. Main thread.
    float DisplayPotentialHeadroom(SDL_Window* window);

    // Before SDL_DestroyWindow: stops watching the window.
    void ForgetWindow(SDL_Window* window);
}
