// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The update check's transport: one HTTP GET, each platform its own way,
// with no library of ours linked for it.
//   - macOS and iOS: NSURLSession, an ephemeral session (no cookies, no
//     cache, no stored credentials): platform/apple/update_fetch.mm.
//   - Linux and SteamOS (x86-64 and the Steam Frame's arm64): the system's
//     curl, run with a fixed argument list (no shell, no ~/.curlrc) and a
//     small environment, its output capped: update/fetch_curl.cpp. Without
//     curl the answer says so (Transport::Unavailable).
//   - Windows: not yet (Transport::Unavailable): update/fetch_windows.cpp,
//     which says how a WinHTTP port fills this in.
//
// The request carries the URL, a User-Agent, the headers below and nothing
// else: no cookies, no credentials, no body.
#pragma once
#include "check.h"

#include <string>
#include <utility>
#include <vector>

namespace update
{
    struct Request
    {
        std::string url;
        std::string userAgent;
        std::vector<std::pair<std::string, std::string>> headers;
        double timeoutSeconds = kTimeoutSeconds;  // the whole exchange
        size_t maxBytes = kMaxResponseBytes;      // the body (curl's headers get a little more)
        bool allowHttp = false;                   // the test hook's local server; the real check is https only
    };

    // Blocking, on the update thread (never the main thread): returns within
    // timeoutSeconds plus about two seconds, whatever the network does.
    Fetched Fetch(const Request& request);
}
