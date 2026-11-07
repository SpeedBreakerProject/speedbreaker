// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The startup check's toast ("SpeedBreaker v0.2.5 is available: open
// Settings ... to update"), frame by frame (update/updater.cpp Tick): what it
// does next. Header only, so tests/startup_toast_test.cpp covers it.
//   - It waits for the check, and for no panel (the installer, What's new)
//     to be up; a version announced before gets none.
//   - Opening Settings is what it asks for: once Settings is open (the
//     update dialog opens only inside it), a toast still on screen or
//     waiting its turn is taken down, and one not shown yet never is (the
//     version counts as announced: the footer's "Update to v0.2.5" offers
//     it). Other toasts aren't touched.
#pragma once
#include <cstdint>

namespace update
{
    enum class ToastStep : uint8_t
    {
        Wait,      // nothing yet: the check is running, or a panel is up
        Show,      // show it now (and note the version as announced)
        Skip,      // never: Settings is already open (note it as announced)
        Withdraw,  // take the shown toast down: Settings opened
        Done,      // nothing more to do (no newer version, announced before, or already handled)
    };

    struct ToastInputs
    {
        bool resultReady = false;      // the startup check has finished
        bool available = false;        // ...and found a newer version
        bool announcedBefore = false;  // that version's toast was shown (or seen) before
        bool modal = false;            // a full-screen panel is up
        bool settingsOpen = false;     // the Settings menu is open
        bool shown = false;            // this launch showed the toast
    };

    inline ToastStep NextToastStep(const ToastInputs& in)
    {
        if (in.shown)
            return in.settingsOpen ? ToastStep::Withdraw : ToastStep::Wait;
        if (!in.resultReady)
            return ToastStep::Wait;
        if (!in.available || in.announcedBefore)
            return ToastStep::Done;
        if (in.settingsOpen)
            return ToastStep::Skip;
        if (in.modal)
            return ToastStep::Wait;
        return ToastStep::Show;
    }
}
