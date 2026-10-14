#import <AppKit/AppKit.h>
#include <SDL3/SDL.h>

#include "macos_window.h"

namespace platform::macos
{
    void ConfigureWindow(SDL_Window* window)
    {
        if (!window)
            return;

        NSWindow* nsWindow = (__bridge NSWindow*)SDL_GetPointerProperty(
            SDL_GetWindowProperties(window),
            SDL_PROP_WINDOW_COCOA_WINDOW_POINTER,
            nullptr);

        if (!nsWindow)
            return;

        nsWindow.titleVisibility = NSWindowTitleHidden;
        nsWindow.titlebarAppearsTransparent = YES;
        nsWindow.styleMask |= NSWindowStyleMaskFullSizeContentView;
    }
}
