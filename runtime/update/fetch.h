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
//   - Windows: WinHTTP, a session of its own (no cookies, no Windows
//     credentials), the system's proxy settings and certificates:
//     update/fetch_windows.cpp.
//
// The request carries the URL, a User-Agent, the headers below and nothing
// else: no cookies, no credentials, no body. Redirects are followed only if
// asked for (release files redirect to the GitHub file host), https to https
// only (Linux and Windows: macOS and iOS only have Fetch, see below).
#pragma once
#include "check.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
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
        bool followRedirects = false;             // at most kMaxRedirects, https only
        // Fetch and Head: once set, the exchange ends in a moment (Failed,
        // "cancelled"). Download takes its own one.
        const std::atomic<bool>* cancel = nullptr;
    };
    inline constexpr int kMaxRedirects = 5;

    // Blocking, on the update thread (never the main thread): returns within
    // timeoutSeconds plus about two seconds, whatever the network does.
    Fetched Fetch(const Request& request);

    // Downloads a big file into `part`, resuming from its size (Range request),
    // with no total timeout and no size cap: request.timeoutSeconds is how
    // long the transfer can stall (under kStallBytesPerSecond) or take to
    // connect; maxBytes isn't used. An error status leaves `part` as it was.
    inline constexpr double kStallSeconds = 30.0;
    inline constexpr int kStallBytesPerSecond = 1;
    struct Downloaded
    {
        Transport transport = Transport::Failed;  // Ok: an HTTP answer, see status
        // 200/206: `part` is whole; 416: nothing past its end
        // (already whole, or too long).
        int status = 0;
        // A Range request got the whole file: delete `part`, start over.
        bool rangeRefused = false;
        bool cancelled = false;    // `cancel` was set, `part` is kept
        bool writeFailed = false;  // couldn't write `part` (disk full, no permission)
        std::string detail;        // for the log
    };
    // Blocking, on the download job thread. progress(bytes in `part`) runs
    // on that thread a few times a second; `cancel` is polled just as often
    // and the call returns about two seconds after it's set.
#ifndef __APPLE__
    Downloaded Download(const Request& request, const std::filesystem::path& part,
        const std::function<void(uint64_t)>& progress, const std::atomic<bool>& cancel);
    // Same as a HEAD request: status and headers, no body.
    Fetched Head(const Request& request);
#else
    // Not on macOS and iOS (platform/apple/update_fetch.mm only does Fetch,
    // with no redirects or cancel): never called there (mods::kAvailable).
    inline Fetched Head(const Request&)
    {
        return { .transport = Transport::Unavailable, .response = {},
            .detail = "not on this platform" };
    }
    inline Downloaded Download(const Request&, const std::filesystem::path&,
        const std::function<void(uint64_t)>&, const std::atomic<bool>&)
    {
        return { .transport = Transport::Unavailable, .detail = "not on this platform" };
    }
#endif
}
