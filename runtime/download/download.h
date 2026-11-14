// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Downloads: one big file at a time on its own thread, for a Task.
// Phases and what stays on disk: README.md.
#pragma once
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace download
{
    enum class Phase : uint8_t
    {
        Idle,
        Contacting,   // resolve, the SHA-256 and the size
        Ready,        // size known: Download, or Resume when `have` > 0
        Downloading,
        Checking,     // the SHA-256 of the whole file
        Installing,   // finish: filesDone of files
        Done,
        Failed,       // `message`
    };

    enum class Error : uint8_t
    {
        None,
        Offline,       // no connection, a timeout, TLS, a cut-off transfer
        NotAvailable,  // 404, no .sha256, nothing to resolve to
        Refused,       // another HTTP error: `status`
        Damaged,       // hash mismatch, a broken archive
        NoSpace,
        CantWrite,
        Rejected,      // not what the task takes
        NoCurl,        // Linux without curl
        Cancelled,     // a hook saw Cancelled(): back to Ready, nothing failed
    };

    // Failure returned by a hook.
    struct Failure
    {
        Error error = Error::None;
        std::string log;      // for the log
        int status = 0;       // Refused: the HTTP status
        std::string message;  // line for the player; empty: the default one for the error
    };
    // NoSpace and CantWrite with the lines for the player.
    Failure NoSpace(uint64_t need, uint64_t free, std::string log);
    Failure CantWrite(const std::filesystem::path& path, std::string log);

    // What to download.
    struct Target
    {
        std::string url;
        std::string name;          // a plain file name (unpack.h): <folder>/<name>.part
        std::string sha256;        // hex; empty: sha256sum line for `name`, <url>.sha256
        std::string version;       // after the name in the dialog ("v2"); can be empty
        // Disk space needed, in file sizes: 2 when finish unpacks it next to itself.
        uint32_t spaceFactor = 1;
    };

    struct JobData;
    // What the task hooks get, on the job thread.
    class Context
    {
    public:
        explicit Context(const JobData& job) : m_job(job) {}
        // Dialog closed or the game is exiting: return Cancelled soon.
        bool Cancelled() const;
        // The game is exiting: no time for slow work (leave it for next start).
        bool Exiting() const;
        // Installing: `done` of `total` files.
        void Progress(uint64_t done, uint64_t total) const;
        // GETs a small file (up to maxBytes). False and `failure` on anything
        // but a 2xx answer. `what` is for the log.
        bool Get(const std::string& url, size_t maxBytes, std::string& body,
            Failure& failure, const char* what, bool followRedirects = true) const;

    private:
        const JobData& m_job;
    };

    struct Task
    {
        std::string label;            // name for it in the log ("hd_hud")
        std::string host = "GitHub";  // "Contacting GitHub...", "Couldn't reach GitHub."
        std::filesystem::path folder; // the part and its .sha256
        std::string userAgent;        // for every request
        // Contacting: what to fetch. False: `failure`.
        std::function<bool(const Context&, Target&, Failure&)> resolve;
        // Installing: the verified file. True: done, `log` says what happened
        // to it. False: `failure` (Cancelled keeps the file for the next try).
        std::function<bool(const Context&, const std::filesystem::path& file,
            std::string& log, Failure&)> finish;
    };

    struct Status
    {
        Phase phase = Phase::Idle;
        std::string label;
        std::string host;
        std::string version;      // from the target
        uint64_t size = 0;        // file size in bytes
        uint64_t have = 0;        // bytes in part (Ready, Downloading); hashed (Checking)
        bool stopping = false;    // Downloading: Stop pressed, winding down to Ready
        uint64_t files = 0;       // Installing (0 until finish says)
        uint64_t filesDone = 0;
        std::string message;      // Failed: line for the player
    };

    // Main thread. Starts a job for `task` (Contacting) and drops any earlier
    // one (its thread winds down in the background).
    void Start(Task task);
    // Ready -> Downloading. Anything else: nothing.
    void Download();
    // Downloading: stops and keeps the part (back to Ready when the transfer
    // ends, `stopping` untill then). Anything else: nothing.
    void Stop();
    // Dialog closed: stops whatever is running (part is kept), back to Idle.
    void Close();
    // Any thread.
    Status GetStatus();
    // Quit paths (they end in std::_Exit, no atexit): stops the
    // running job and gives it up to 3 s to end the transfer.
    void Shutdown();

    // Free bytes for `folder` if known (on iOS
    // what it allows for important downloads).
    std::optional<uint64_t> FreeSpace(const std::filesystem::path& folder);

    // https://github.com/<repo>/releases/download/<tag>/<file>
    inline std::string GitHubReleaseUrl(std::string_view repo, std::string_view tag,
        std::string_view file)
    {
        return std::format("https://github.com/{}/releases/download/{}/{}", repo, tag,
            file);
    }
}
