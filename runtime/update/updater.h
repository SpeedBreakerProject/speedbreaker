// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Check for Updates: Settings > Advanced (ui/update_dialog.cpp) starts a
// check, which runs on a thread of its own (the transport, update/fetch.h,
// with its timeout) and never holds up the game or the menu; the menu
// reads the result when it's there. Nothing goes online until the player
// presses it. Each check writes one log line ("[update] ...") with its result.
//
// Test hooks (environment variables):
//   NFSMW_TEST_VERSION=0.0.9    compare as this version instead of the
//                               build's (the real v0.1.0 then shows as
//                               available); the [build] line keeps the real one
//   NFSMW_UPDATE_URL=<url>      ask this http(s) address instead of GitHub's
//                               API (a local server: 403s, slow answers,
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

namespace update
{
    enum class Phase : uint8_t { Idle, Checking, Done };

    struct Status
    {
        Phase phase = Phase::Idle;
        uint64_t checks = 0;                   // checks started this session
        std::shared_ptr<const Result> result;  // the last check's (Done)
    };

    // Main thread: starts a check unless one is running (then nothing).
    void StartCheck();
    // Any thread.
    Status GetStatus();
    // The update dialog shows the result as it comes (true), or isn't open,
    // so a check that finishes then tells the player with a toast (false).
    void SetDialogShowing(bool showing);

    // The version a check compares: the build's, or NFSMW_TEST_VERSION's.
    std::string RunningVersion();
}
