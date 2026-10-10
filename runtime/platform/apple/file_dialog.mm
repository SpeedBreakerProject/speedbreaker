// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See ../file_dialog.h.
//
// macOS only (AppKit); built for iOS too, as every platform/apple/*.mm is,
// where it is empty. Manual reference counting like the rest of the runtime.
#include <TargetConditionals.h>

#if TARGET_OS_OSX
#include <platform/file_dialog.h>

#import <AppKit/AppKit.h>

namespace platform::file_dialog
{
    int CancelOpenPanels()
    {
        @autoreleasepool
        {
            int cancelled = 0;
            // A copy: a cancelled sheet leaves the window list.
            NSArray<NSWindow*>* windows = [[[NSApp windows] copy] autorelease];
            for (NSWindow* window in windows)
                if ([window isKindOfClass:[NSSavePanel class]] && [window isVisible])
                {
                    [(NSSavePanel*)window cancel:nil];
                    cancelled++;
                }
            return cancelled;
        }
    }
}
#endif
