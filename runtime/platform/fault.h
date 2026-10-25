// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Faults on Windows (main.cpp installs POSIX's signal handlers itself): a
// vectored exception handler, first in line, gives access violations to
// the write watch (writewatch::HandleFault: a store to a watched texture
// page, or an access to one the GPU still uses), which unprotects the page
// so the access runs again. Anything else goes on to the usual handlers (a
// C++ throw, GuestThreadExit included, passes untouched), and what nothing
// handles writes the crash report (report::WriteCrashReport) and ends the
// process. abort() (a failed assert) reports too, and never shows a dialog.
#pragma once

namespace platform
{
#ifdef _WIN32
    void InstallFaultHandlers();
    // The crash report's filter installed again, in case something loaded
    // since (a driver, an overlay) put its own in its place: false if it had.
    bool KeepCrashFilter();
#endif
}
