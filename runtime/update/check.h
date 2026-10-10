// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Check for Updates (Settings > Advanced): what the game makes of GitHub's
// answer. The only time the game goes online, and only when the player
// presses it: one GET of the releases API's "latest" release, with a
// User-Agent and an Accept header, nothing else (update/updater.h runs it,
// update/fetch.h is each platform's transport).
//
// Here only the logic, no I/O, so tests/update_check_test.cpp covers all of
// it: Semantic Versioning, a strict reader for the release's JSON, the raw
// HTTP answer curl prints (and the test hook's canned files), the release
// notes as plain lines, and the one-line result for every outcome.
#pragma once
#include <user/version_check.h>  // version::NoteLine, as the "What's new" note draws them

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace update
{
    // GitHub's newest release that is neither a draft nor a pre-release.
    inline constexpr const char kApiUrl[] = "https://api.github.com/repos/SpeedBreakerProject/speedbreaker/releases/latest";
    // Where "Open download page" goes when the answer names no page of this
    // project's releases.
    inline constexpr const char kReleasesPage[] = "https://github.com/SpeedBreakerProject/speedbreaker/releases/latest";
    inline constexpr const char kPagePrefix[] = "https://github.com/SpeedBreakerProject/speedbreaker/releases/";
    // The whole exchange, connecting included.
    inline constexpr double kTimeoutSeconds = 10.0;
    // A release's JSON is a few KB (its notes are at most 125,000 characters).
    inline constexpr size_t kMaxResponseBytes = 1 << 20;

    // --- Versions: Semantic Versioning 2.0.0 ---

    struct Semver
    {
        uint64_t major = 0, minor = 0, patch = 0;
        std::vector<std::string> pre;  // "rc.1" -> { "rc", "1" }; empty for a release
        std::string build;             // after '+'; ignored when comparing
    };
    // "0.1.2", "v0.1.2", "1.0.0-rc.1+build.5". Strict: three numbers without
    // leading zeros, identifiers of [0-9A-Za-z-] (a numeric pre-release one
    // without leading zeros), nothing else.
    bool ParseSemver(std::string_view text, Semver& out);
    // -1, 0 or 1 by precedence (a pre-release comes before its release;
    // build metadata doesn't count).
    int CompareSemver(const Semver& a, const Semver& b);
    // "0.1.2" or "1.0.0-rc.1" (no build metadata).
    std::string FormatSemver(const Semver& v);

    // --- The release, from GitHub's JSON ---

    struct Release
    {
        bool hasTag = false;
        std::string tag;      // tag_name: "v0.1.2"
        std::string name;     // "" when null
        std::string url;      // html_url: the release's page
        std::string body;     // the notes (Markdown); "" when null
        std::string message;  // an error answer's "message"
        bool draft = false, prerelease = false;
    };
    // Reads the top-level members above out of a JSON object. Strict: the
    // whole document must be valid JSON (RFC 8259, UTF-8, at most 64 levels
    // deep) with nothing after it, and each member read must have the right
    // type, once. False, with what was wrong and where, otherwise.
    bool ParseRelease(std::string_view json, Release& out, std::string& error);

    // --- The HTTP answer ---

    struct HttpResponse
    {
        int status = 0;
        std::vector<std::pair<std::string, std::string>> headers;  // names in lower case
        std::string body;
    };
    // Header blocks then the body, as `curl --dump-header -` prints them (a
    // proxy's or an interim block first, the last one counts), or a canned
    // file of the test hook. False without a whole status line and header block.
    bool ParseHttpResponse(std::string_view raw, HttpResponse& out);
    // A header's value (any case), or "".
    std::string_view Header(const HttpResponse& response, std::string_view name);

    // --- Release notes as plain lines ---

    struct Notes
    {
        std::vector<version::NoteLine> lines;
        bool truncated = false;  // cut at maxChars or maxLines
    };
    // GitHub Markdown to the lines the dialog draws: headings, bullets,
    // paragraphs joined, links as their text, code and emphasis marks,
    // HTML tags and comments, images and tables' rules dropped, entities
    // decoded, control characters gone. At most maxChars of text in
    // maxLines lines (whole UTF-8 characters).
    Notes CleanNotes(std::string_view markdown, size_t maxChars = 6000, size_t maxLines = 150);
    // One plain line (a release's name): as a notes line, at most maxChars.
    std::string CleanLine(std::string_view text, size_t maxChars);

    // --- The outcome ---

    // How the transport (update/fetch.h) ended.
    enum class Transport : uint8_t
    {
        Ok,           // an HTTP answer, any status: `response`
        Offline,      // no connection (no network, no DNS, refused)
        Timeout,      // nothing whole within kTimeoutSeconds
        Secure,       // TLS failed (a certificate, the clock)
        TooLarge,     // more than kMaxResponseBytes
        Garbled,      // an answer that isn't HTTP
        Unavailable,  // no transport here (no curl; Windows for now)
        Failed,       // anything else
    };
    struct Fetched
    {
        Transport transport = Transport::Failed;
        HttpResponse response;
        std::string detail;  // for the log, and for Unavailable the player's message
    };

    enum class Outcome : uint8_t { UpToDate, Available, Offline, Timeout, RateLimited, Unreadable, ServerError, Unavailable };
    struct Result
    {
        Outcome outcome = Outcome::Unreadable;
        std::string current;   // the running version: "0.1.1"
        std::string latest;    // the newest release's: "0.1.2" ("" if unknown)
        std::string headline;  // "v0.1.2 is available", "You're up to date (v0.1.1)", "Couldn't check for updates"
        std::string message;   // one line: what happened, what to do
        std::string name;      // the release's name, one plain line
        std::string url;       // the download page (SafePageUrl)
        Notes notes;
        std::string log;       // the log line after "[update] " (the caller adds the time)
    };
    // `current`: the running version; `now`: Unix seconds (a rate limit's reset).
    Result Evaluate(const Fetched& fetched, std::string_view current, int64_t now);
    // The page to open and show as a QR code: `url` if it is a page of this
    // project's releases (plain characters, a sane length), else kReleasesPage.
    std::string SafePageUrl(std::string_view url);
}
