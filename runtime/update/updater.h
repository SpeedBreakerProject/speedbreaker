// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Check for Updates, at startup and on a press. Each check runs on a thread
// of its own (the transport, update/fetch.h, with its timeout) and never
// holds up the game or the menu; the menu reads the result when it's there.
// Each check writes one log line ("[update] ...") with its result.
//
//   - At startup: once per launch, a few seconds after the window is up
//     (kStartupDelaySeconds, counted from NoteWindowUp), when Settings >
//     Advanced > Check for Updates at Startup is on (the default;
//     NFSMW_UPDATE_CHECK=0 turns it off for a run). It applies at once: off
//     before the check starts, no check; turned on later in the launch, the
//     check starts then. Not at all right after an update's restart
//     (NoteAfterUpdate: the update just checked), nor in a scripted run (a
//     virtual pad, scripted keys or input, timed captures) unless the run
//     names a feed (NFSMW_UPDATE_FEED or NFSMW_UPDATE_URL) or asks with
//     NFSMW_UPDATE_CHECK=1: automated runs never use GitHub's 60 checks an
//     hour by accident. A newer version gets one toast, once per version
//     (remembered in <data>/updates/notified.txt, never in settings.toml);
//     a failed check shows nothing (the log line, and the Settings row's
//     "Couldn't check (offline)").
//   - On a press: Settings > Advanced > Check for Updates
//     (ui/update_dialog.cpp), whatever the setting says.
//   - Either way, a newer version puts "Update to v0.2.5" first in the
//     Settings menu's footer (FooterLabel), which opens the dialog with it.
//
// Test hooks (environment variables):
//   NFSMW_TEST_VERSION=0.0.9    compare as this version instead of the
//                               build's (the real v0.1.0 then shows as
//                               available; "0.2.3-windows-beta.6" a Windows
//                               beta); a tagged build compares as that
//                               version's tag. The [build] line keeps the real one
//   NFSMW_UPDATE_FEED=<base>    ask this http(s) address instead of
//                               https://api.github.com/, with the same paths
//                               after it (repos/<repo>/releases/latest, or
//                               on Windows repos/<repo>/releases?per_page=10):
//                               a local stand-in for GitHub (anything else
//                               fails the check: a test never reaches GitHub)
//   NFSMW_UPDATE_URL=<url>      ask this http(s) address, whole, instead
//                               (a local server: 403s, slow answers,
//                               refused connections)
//   NFSMW_UPDATE_URL=<file>     answer from this file, read whole, no network:
//                               a raw HTTP answer ("HTTP/1.1 403 rate limit
//                               exceeded", headers, a blank line, the body, as
//                               `curl --dump-header -` prints one), or else a
//                               body for an HTTP 200; "offline" and "timeout"
//                               on its first line stand for those failures
//   NFSMW_UPDATE_CURL=<path>    Linux: run this curl (a missing one: the
//                               "needs curl" message)
//   NFSMW_TEST_NO_BROWSER=1     "Open download page" logs the address
//                               instead of opening it (automated runs)
#pragma once
#include "check.h"

#include <cstdint>
#include <memory>
#include <string>

namespace update
{
    enum class Phase : uint8_t { Idle, Checking, Done };
    enum class Kind : uint8_t { Pressed, Startup };

    struct Status
    {
        Phase phase = Phase::Idle;
        uint64_t checks = 0;                   // checks started this session
        std::shared_ptr<const Result> result;  // the last check's (Done)
        Kind kind = Kind::Pressed;             // the last check's
    };

    // Main thread: starts a check (the press) unless one is running (then
    // that one counts as the press: its result shows in the dialog).
    void StartCheck();
    // Any thread.
    Status GetStatus();
    // The update dialog shows the result as it comes (true), or isn't open,
    // so a pressed check that finishes then tells the player with a toast (false).
    void SetDialogShowing(bool showing);
    // The dialog showed this release (its tag): the startup toast won't
    // announce it again.
    void NoteSeen(const std::string& tag);

    // The version a check compares: the build's, or NFSMW_TEST_VERSION's.
    std::string RunningVersion();
    // The running build as the check compares it: that version, its release
    // tag ("" untagged), whether it is a development build, this device.
    Running RunningBuild();

    // The startup check.
    inline constexpr double kStartupDelaySeconds = 4.0;
    // Main thread, before the window's first frame: this launch is an
    // update's restart (--after-update): no startup check.
    void NoteAfterUpdate();
    // Main thread: the window is up and the game is starting (the delay
    // counts from here). Headless runs never call it, so never check.
    void NoteWindowUp();
    // Main thread, every UI frame: starts the startup check when it's due,
    // and shows its toast once no panel is up.
    void Tick();

    // "Update to v0.2.5" ("Update to v0.2.4 beta 7") while the last check
    // found a newer version; "" otherwise.
    std::string FooterLabel();
}
