// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Check for Updates: what the game makes of GitHub's answer. The game asks
// once at startup (Settings > Advanced > Check for Updates at Startup, on by
// default) and whenever the player presses Check for Updates: one GET of the
// releases API, with a User-Agent and an Accept header, nothing else
// (update/updater.h runs it, update/fetch.h is each platform's transport).
//   - Mac, Linux and SteamOS, iPhone and iPad: the "latest" release, which
//     is never a draft or a pre-release.
//   - Windows: the ten newest releases, because its betas are pre-releases
//     ("v0.2.4-windows-beta.7"), which "latest" never returns.
//
// Here only the logic, no I/O, so tests/update_check_test.cpp covers all of
// it: Semantic Versioning, the release tags and their Windows beta order, a
// strict reader for GitHub's JSON, which of a release's files is this
// device's and whether the release is complete for it, the raw HTTP answer
// curl prints (and the test hook's canned files), the release notes as plain
// lines, and the one-line result for every outcome.
#pragma once
#include "release_names.h"       // SB_GITHUB_REPO, SB_ASSET_PREFIX: each game's own releases
#include <user/version_check.h>  // version::NoteLine, as the "What's new" note draws them

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace update
{
    // GitHub's API; NFSMW_UPDATE_FEED=<http(s) base> puts a local stand-in
    // in its place (update/updater.h). The two questions, after it:
    inline constexpr const char kApiBase[] = "https://api.github.com/";
    // The newest release that is neither a draft nor a pre-release, of this
    // build's game (release_names.h: Most Wanted's speedbreaker repository,
    // NFS: Carbon's speedbreaker-carbon).
    inline constexpr const char kLatestPath[] = "repos/" SB_GITHUB_REPO "/releases/latest";
    // Windows: the ten newest releases, pre-releases (its betas) included.
    inline constexpr const char kListPath[] = "repos/" SB_GITHUB_REPO "/releases?per_page=10";
    inline constexpr const char kApiUrl[] = "https://api.github.com/repos/" SB_GITHUB_REPO "/releases/latest";
    // Where "Open download page" goes when the answer names no page of this
    // project's releases.
    inline constexpr const char kReleasesPage[] = "https://github.com/" SB_GITHUB_REPO "/releases/latest";
    inline constexpr const char kPagePrefix[] = "https://github.com/" SB_GITHUB_REPO "/releases/";
    // The whole exchange, connecting included.
    inline constexpr double kTimeoutSeconds = 10.0;
    // A release's JSON is a few KB (its notes are at most 125,000 characters);
    // Windows' ten measured 295 KB (2026-11-01).
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

    // --- Release tags ---

    // The tags releases are published as (NFSMW_BUILD_TAG is the running
    // build's): "v0.2.4", and for Windows' betas "v0.2.4-windows-beta.7".
    // Strict: a "v", three numbers without leading zeros, and for a beta
    // "-windows-beta." and a number from 1, without leading zeros. Anything
    // else ("v0.2.0-test-issue67", "0.2.4", "v0.2.4-rc.1") isn't one.
    struct ReleaseTag
    {
        Semver version;     // no pre-release or build part
        uint64_t beta = 0;  // windowsBeta: its number (0: an untagged Windows build, before every beta)
        bool windowsBeta = false;
    };
    bool ParseReleaseTag(std::string_view tag, ReleaseTag& out);
    // By version, then betas by number, and a version's release after every
    // one of its betas: v0.2.3-windows-beta.6 < v0.2.4-windows-beta.7 <
    // v0.2.4-windows-beta.8 < v0.2.4. (A Windows beta's exe knows only its
    // version, "0.2.4", which Semantic Versioning ranks above every beta of
    // it: the tag, NFSMW_BUILD_TAG, carries the beta number.)
    int CompareReleaseTags(const ReleaseTag& a, const ReleaseTag& b);
    std::string FormatReleaseTag(const ReleaseTag& t);  // "v0.2.4-windows-beta.7"
    std::string ReleaseLabel(const ReleaseTag& t);      // as the player reads it: "v0.2.4 beta 7", "v0.2.4"

    // --- The release, from GitHub's JSON ---

    struct Asset
    {
        std::string name;    // "SpeedBreaker-macos.dmg"
        std::string state;   // "uploaded" once the file is whole ("open" while it is being uploaded)
        int64_t size = -1;   // bytes; -1 when not given
        std::string digest;  // "sha256:<hex>" (GitHub works it out at upload); "" when null
    };

    struct Release
    {
        bool hasTag = false;
        std::string tag;      // tag_name: "v0.1.2"
        std::string name;     // "" when null
        std::string url;      // html_url: the release's page
        std::string body;     // the notes (Markdown); "" when null
        std::string message;  // an error answer's "message"
        bool draft = false, prerelease = false;
        std::vector<Asset> assets;  // its files, in GitHub's order
    };
    // Reads the top-level members above out of a JSON object, and of each of
    // "assets"' objects their name, state, size and digest. Strict: the
    // whole document must be valid JSON (RFC 8259, UTF-8, at most 64 levels
    // deep) with nothing after it, and each member read must have the right
    // type, once (an asset's too; "assets" is an array of objects). False,
    // with what was wrong and where, otherwise.
    bool ParseRelease(std::string_view json, Release& out, std::string& error);
    // A list of releases (Windows' question): a JSON array of release
    // objects, each read as ParseRelease reads one, as strictly.
    bool ParseReleases(std::string_view json, std::vector<Release>& out, std::string& error);

    // --- This device's file ---

    // What the running build is, all known when it was compiled (never
    // guessed from the system): ThisDevice(). Tests pass any.
    enum class Platform : uint8_t { MacOS, Linux, Windows, IOS, Other };
    enum class Arch : uint8_t { Arm64, X86_64, Other };
    struct Device
    {
        Platform platform = Platform::Other;
        Arch arch = Arch::Other;
        std::string_view prefix = SB_ASSET_PREFIX;  // "SpeedBreaker", "SpeedBreaker-Carbon"
    };
    Device ThisDevice();
    // The release files this device downloads, the preferred one first
    // (empty: none is made for it). `version`: the release's, "0.2.4";
    // `windowsBeta`: a Windows beta's pre-release.
    //   Mac (Apple silicon)  <prefix>-macos.dmg
    //   Linux, SteamOS       <prefix>-steamos-x86_64.tar.xz / -steamos-arm64.tar.xz (the Steam Frame)
    //   Windows x64          <prefix>-<version>-windows-x64-beta.zip in a beta; in a release
    //                        <prefix>-<version>-windows-x64.zip, or the -beta one
    //   iPhone, iPad         <prefix>-<version>-ios.ipa (a notice and a link only)
    std::vector<std::string> DeviceAssetNames(const Device& device, std::string_view version, bool windowsBeta);

    // A newer release counts only if it is complete for this device: its
    // file and the file's .sha256 are both among its assets, "uploaded",
    // and not empty. (Phase 2 adds the signed update list,
    // <prefix>-update.json and its .minisig: `requireUpdateList`, which
    // kUpdateListRequired turns on for every check once the release side
    // makes them. Until then the check reports the list's absence in the log
    // and doesn't hold it against the release.) An incomplete release is
    // still "available", with the download page only: a release without the
    // Steam Frame's build, or one still being uploaded.
    inline constexpr bool kUpdateListRequired = false;
    std::string UpdateListName(const Device& device);  // "<prefix>-update.json"
    struct Completeness
    {
        bool complete = false;
        std::string asset;     // this device's file in it ("" none)
        std::string missing;   // for the log when not complete: what isn't there (whole), or "no file for this device"
        bool updateList = false;  // the update list and its signature are there (uploaded)
    };
    Completeness CheckComplete(const std::vector<Asset>& assets, const Device& device, std::string_view version, bool windowsBeta,
        bool requireUpdateList = kUpdateListRequired);

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
    // Why the dialog offers no Update Now for an available release (one line
    // in it says so). Phase 1 offers it nowhere: a complete release for a
    // release build on a device that updates in place is NotYet, the one
    // later phases turn into Update Now.
    enum class NoUpdateNow : uint8_t
    {
        DevBuild,    // no release tag, or uncommitted changes: "This is a development build"
        Incomplete,  // the release lacks this device's file or its .sha256 (or, from Phase 2, the update list)
        Phone,       // iPhone and iPad: installed with the player's own sideloading tool
        NotYet,      // everything is there; this version doesn't install updates itself yet
    };
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
        // Available only:
        std::string latestTag;  // the release's tag: "v0.1.2", "v0.2.4-windows-beta.7" (once per version: the toast)
        std::string label;      // as the player reads it: "v0.1.2", "v0.2.4 beta 7" (the footer's "Update to ...")
        bool complete = false;  // CheckComplete
        std::string asset;      // this device's file in it
        NoUpdateNow why = NoUpdateNow::NotYet;
        std::string whyLine;    // the dialog's one line for it
    };

    // The running build, as the check compares it.
    struct Running
    {
        std::string version;   // "0.2.4" (NFSMW_TEST_VERSION's instead: "0.2.3", "0.2.3-windows-beta.6")
        std::string tag;       // the tag it was published as (NFSMW_BUILD_TAG): "v0.2.4", "v0.2.4-windows-beta.7"; "" untagged
        bool devBuild = true;  // untagged or dirty: never installs an update, only shows the link
        Device device;
    };
    // `now`: Unix seconds (a rate limit's reset). On Windows (device) the
    // answer is the list of releases (kListPath), elsewhere the latest one.
    Result Evaluate(const Fetched& fetched, const Running& running, int64_t now);
    // A build of no known platform, `current` its version (the tests of
    // the "latest" rules).
    Result Evaluate(const Fetched& fetched, std::string_view current, int64_t now);
    // The page to open and show as a QR code: `url` if it is a page of this
    // project's releases (plain characters, a sane length), else kReleasesPage.
    std::string SafePageUrl(std::string_view url);
}
