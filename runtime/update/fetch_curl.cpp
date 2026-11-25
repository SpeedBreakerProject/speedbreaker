// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See fetch.h.
//
// Linux and SteamOS: the system's curl (on SteamOS and nearly every
// distribution), so the game links no HTTP or TLS library of its own and
// uses the system's certificates. It runs with:
//   - a fixed argument list, through posix_spawn (no shell), -q first so no
//     ~/.curlrc adds anything;
//   - a small environment: PATH, the proxy variables and the CA bundle ones
//     (not LD_LIBRARY_PATH or LD_PRELOAD: the SteamOS download's bundled
//     libraries and Steam's overlay are the game's, not curl's);
//   - stdin and stderr on /dev/null, stdout a pipe read with a deadline and
//     a size cap; no other descriptor of the game's;
//   - default signal handling and an empty signal mask.
// curl stops itself at --max-time; the game kills it two seconds
// later if it hasn't, and always reaps it. Downloads have no
// --max-time: curl writes the file itself (--continue-at -,
// --fail, a stall limit) and gets killed on cancel.
//
// NFSMW_UPDATE_CURL=<path>: that curl instead (tests: a missing one).
#if !defined(__APPLE__) && !defined(_WIN32)
#include "fetch.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <format>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace update
{
    namespace
    {
        bool Executable(const std::string& path)
        {
            struct stat st;
            return !path.empty() && path[0] == '/' && stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(path.c_str(), X_OK) == 0;
        }

        std::string FindCurl()
        {
            if (const char* v = std::getenv("NFSMW_UPDATE_CURL"); v && *v)
                return Executable(v) ? v : "";
            std::vector<std::string> dirs;
            if (const char* path = std::getenv("PATH"))
                for (std::string_view rest = path; !rest.empty();)
                {
                    size_t colon = rest.find(':');
                    std::string_view dir = rest.substr(0, colon);
                    rest = colon == std::string_view::npos ? std::string_view() : rest.substr(colon + 1);
                    if (dir.starts_with('/'))  // never the current folder
                        dirs.emplace_back(dir);
                }
            for (const char* dir : { "/usr/bin", "/bin", "/usr/local/bin" })
                dirs.emplace_back(dir);
            for (const std::string& dir : dirs)
                if (std::string curl = dir + "/curl"; Executable(curl))
                    return curl;
            return {};
        }

        // curl(1), "EXIT CODES".
        Transport FromExitCode(int code)
        {
            switch (code)
            {
            case 0:
                return Transport::Ok;
            case 5:   // couldn't resolve the proxy
            case 6:   // couldn't resolve the host
            case 7:   // couldn't connect
            case 45:  // interface error
            case 52:  // nothing came back
            case 55:  // send failed
            case 56:  // receive failed (a reset connection)
                return Transport::Offline;
            case 28:
                return Transport::Timeout;
            case 35:  // the TLS handshake failed
            case 53: case 54: case 58: case 59: case 60: case 64: case 66: case 77: case 80: case 82: case 83: case 90: case 91:
                return Transport::Secure;
            case 63:  // over --max-filesize
                return Transport::TooLarge;
            case 1:   // "unsupported protocol": with the protocols fixed, a reply that isn't HTTP (HTTP/0.9 refused)
            case 8:   // a reply curl couldn't parse
                return Transport::Garbled;
            default:
                return Transport::Failed;
            }
        }

        // waitpid, through EINTR. 0: still running; -1: not ours to reap.
        pid_t Reap(pid_t pid, int& status, int flags)
        {
            pid_t r;
            do
                r = waitpid(pid, &status, flags);
            while (r < 0 && errno == EINTR);
            return r;
        }

        const char* const kNoCurl =
            "Checking needs curl, which this system doesn't have. "
            "Install curl, or see the releases page.";

        // How a curl run ended.
        struct Run
        {
            bool spawned = false;
            int spawnError = 0;
            std::string out;  // its stdout
            bool killed = false;
            Transport killedAs = Transport::Failed;
            bool cancelled = false;
            int status = 0;   // from waitpid
            pid_t reaped = 0;
            std::string detail;
        };

        // Runs curl with `args` (after its path), stdout read into Run::out up
        // to `cap` bytes. Killed at `deadline` (none: max()), or when tick()
        // (called about four times a second) returns false.
        using Clock = std::chrono::steady_clock;
        Run RunCurl(const std::string& curl, std::vector<std::string> args,
            Clock::time_point deadline, size_t cap,
            const std::function<bool()>& tick = {})
        {
            Run run;
            args.insert(args.begin(), curl);
            std::vector<char*> argv;
            for (std::string& a : args)
                argv.push_back(a.data());
            argv.push_back(nullptr);

            std::vector<std::string> env;
            for (const char* name : { "PATH", "https_proxy", "HTTPS_PROXY", "http_proxy",
                     "all_proxy", "ALL_PROXY", "no_proxy", "NO_PROXY", "SSL_CERT_FILE",
                     "SSL_CERT_DIR", "CURL_CA_BUNDLE" })
                if (const char* value = std::getenv(name))
                    env.push_back(std::string(name) + "=" + value);
            std::vector<char*> envp;
            for (std::string& e : env)
                envp.push_back(e.data());
            envp.push_back(nullptr);

            int fds[2];
            if (pipe2(fds, O_CLOEXEC) != 0)
            {
                run.spawnError = errno;
                run.detail = std::format("pipe: {}", strerror(errno));
                return run;
            }
            posix_spawn_file_actions_t actions;
            posix_spawn_file_actions_init(&actions);
            posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
            posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
            posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
            // Game descriptors without O_CLOEXEC too.
            posix_spawn_file_actions_addclosefrom_np(&actions, 3);
#endif
            posix_spawnattr_t attr;
            posix_spawnattr_init(&attr);
            sigset_t none, all;
            sigemptyset(&none);
            sigfillset(&all);
            posix_spawnattr_setsigmask(&attr, &none);
            posix_spawnattr_setsigdefault(&attr, &all);
            posix_spawnattr_setflags(&attr,
                POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
            pid_t pid = -1;
            const int spawned = posix_spawn(&pid, curl.c_str(), &actions, &attr,
                argv.data(), envp.data());
            posix_spawn_file_actions_destroy(&actions);
            posix_spawnattr_destroy(&attr);
            close(fds[1]);
            if (spawned != 0)
            {
                close(fds[0]);
                run.spawnError = spawned;
                run.detail = std::format("couldn't run curl: {}", strerror(spawned));
                return run;
            }
            run.spawned = true;

            // Its output, by the deadline and within the cap.
            char buffer[16384];
            auto nextTick = Clock::now();
            while (true)
            {
                const auto now = Clock::now();
                if (tick && now >= nextTick)
                {
                    if (!tick())
                    {
                        run.killed = run.cancelled = true;
                        run.detail = "cancelled";
                        break;
                    }
                    nextTick = now + std::chrono::milliseconds(250);
                }
                using std::chrono::milliseconds;
                auto left =
                    std::chrono::duration_cast<milliseconds>(deadline - now).count();
                if (left <= 0)
                {
                    run.killed = true, run.killedAs = Transport::Timeout;
                    run.detail = "curl was still running after the deadline";
                    break;
                }
                pollfd p{ fds[0], POLLIN, 0 };
                int ready = poll(&p, 1, int(std::min<int64_t>(left, tick ? 250 : 1000)));
                if (ready < 0 && errno == EINTR)
                    continue;
                if (ready < 0)
                {
                    run.killed = true;
                    run.detail = std::format("poll: {}", strerror(errno));
                    break;
                }
                if (ready == 0)
                    continue;
                ssize_t n = read(fds[0], buffer, sizeof(buffer));
                if (n < 0 && (errno == EINTR || errno == EAGAIN))
                    continue;
                if (n < 0)
                {
                    run.killed = true;
                    run.detail = std::format("read: {}", strerror(errno));
                    break;
                }
                if (n == 0)
                    break;  // curl closed its output: done
                if (run.out.size() + size_t(n) > cap)
                {
                    run.killed = true, run.killedAs = Transport::TooLarge;
                    run.detail = std::format("over {} bytes", cap);
                    break;
                }
                run.out.append(buffer, size_t(n));
            }
            close(fds[0]);
            if (run.killed)
                kill(pid, SIGKILL);  // still unreaped, so still ours

            // Reaped in any case: at its end of output curl
            // is exiting; give it two seconds, then the kill.
            run.reaped = Reap(pid, run.status, WNOHANG);
            for (auto until = Clock::now() + std::chrono::seconds(2);
                 run.reaped == 0 && Clock::now() < until;)
            {
                usleep(10'000);
                run.reaped = Reap(pid, run.status, WNOHANG);
            }
            if (run.reaped == 0)
            {
                kill(pid, SIGKILL);
                run.reaped = Reap(pid, run.status, 0);
                if (!run.killed)
                {
                    run.killed = true, run.killedAs = Transport::Timeout;
                    run.detail = "curl didn't exit";
                }
            }
            return run;
        }

        // The curl exit code of a run that wasn't killed, or -1 (detail set).
        int ExitCode(Run& run)
        {
            if (run.reaped < 0 || !WIFEXITED(run.status))
            {
                run.detail = run.reaped < 0
                    ? "curl's exit status was lost"
                    : std::format("curl ended by signal {}", WTERMSIG(run.status));
                return -1;
            }
            return WEXITSTATUS(run.status);
        }

        std::vector<std::string> CommonArgs(const Request& request)
        {
            std::vector<std::string> args = { "-q", "--silent", "--proto",
                request.allowHttp ? "=http,https" : "=https" };
            if (request.followRedirects)
                args.insert(args.end(), { "--location", "--proto-redir", "=https",
                                            "--max-redirs",
                                            std::to_string(kMaxRedirects) });
            args.insert(args.end(), { "--user-agent", request.userAgent });
            for (const auto& [name, value] : request.headers)
            {
                args.push_back("--header");
                args.push_back(name + ": " + value);
            }
            return args;
        }

        Fetched Exchange(const Request& request, bool head)
        {
            Fetched result;
            const std::string curl = FindCurl();
            if (curl.empty())
            {
                result.transport = Transport::Unavailable;
                result.detail = kNoCurl;
                return result;
            }

            const std::string seconds =
                std::format("{}", std::ceil(request.timeoutSeconds));
            std::vector<std::string> args = CommonArgs(request);
            args.insert(args.end(), { "--dump-header", "-", "--max-time", seconds,
                                        "--connect-timeout", seconds });
            // Headers just once, from --dump-header; --max-filesize
            // would refuse the Content-Lenght of a big file.
            if (head)
                args.insert(args.end(), { "--head", "--output", "/dev/null" });
            else
                args.insert(args.end(),
                    { "--max-filesize", std::to_string(request.maxBytes) });
            args.push_back("--url");
            args.push_back(request.url);

            const auto deadline = Clock::now()
                + std::chrono::duration_cast<Clock::duration>(
                    std::chrono::duration<double>(request.timeoutSeconds + 2.0));
            const size_t cap = request.maxBytes + 64 * 1024;  // the body plus headers
            Run run = RunCurl(curl, std::move(args), deadline, cap,
                [&] { return !(request.cancel && request.cancel->load()); });
            result.detail = run.detail;
            if (!run.spawned)
            {
                result.transport =
                    run.spawnError == ENOENT ? Transport::Unavailable : Transport::Failed;
                if (run.spawnError == ENOENT)
                    result.detail = kNoCurl;
                return result;
            }
            if (run.killed)
            {
                result.transport = run.cancelled ? Transport::Failed : run.killedAs;
                return result;
            }
            const int code = ExitCode(run);
            if (code < 0)
            {
                result.transport = Transport::Failed;
                result.detail = run.detail;
                return result;
            }
            if (Transport t = FromExitCode(code); t != Transport::Ok)
            {
                result.transport = t;
                result.detail = std::format("curl exit {}", code);
                return result;
            }
            if (!ParseHttpResponse(run.out, result.response))
            {
                result.transport = Transport::Garbled;
                result.detail =
                    std::format("curl's output isn't an HTTP answer ({} bytes)",
                        run.out.size());
                return result;
            }
            if (result.response.body.size() > request.maxBytes)
            {
                result.transport = Transport::TooLarge;
                result.detail = std::format("{} bytes", result.response.body.size());
                return result;
            }
            result.transport = Transport::Ok;
            return result;
        }

        uint64_t FileSize(const std::filesystem::path& path)
        {
            std::error_code ec;
            const auto size = std::filesystem::file_size(path, ec);
            return ec ? 0 : uint64_t(size);
        }
    }

    Fetched Fetch(const Request& request)
    {
        return Exchange(request, false);
    }

    Fetched Head(const Request& request)
    {
        return Exchange(request, true);
    }

    Downloaded Download(const Request& request, const std::filesystem::path& part,
        const std::function<void(uint64_t)>& progress, const std::atomic<bool>& cancel)
    {
        Downloaded result;
        const std::string curl = FindCurl();
        if (curl.empty())
        {
            result.transport = Transport::Unavailable;
            result.detail = "no curl";
            return result;
        }

        // --fail: an error status writes nothing to `part`. --continue-at -:
        // from its size (missing file: from the start). stdout: the status.
        const std::string seconds = std::format("{}", std::ceil(request.timeoutSeconds));
        std::vector<std::string> args = CommonArgs(request);
        args.insert(args.end(), { "--fail", "--continue-at", "-", "--connect-timeout",
                                    seconds, "--speed-limit",
                                    std::to_string(kStallBytesPerSecond), "--speed-time",
                                    seconds, "--output", part.string(), "--write-out",
                                    "%{http_code}", "--url", request.url });
        Run run = RunCurl(curl, std::move(args), Clock::time_point::max(), 64, [&] {
            if (cancel.load())
                return false;
            if (progress)
                progress(FileSize(part));
            return true;
        });
        result.detail = run.detail;
        if (!run.spawned)
        {
            result.transport =
                run.spawnError == ENOENT ? Transport::Unavailable : Transport::Failed;
            return result;
        }
        if (run.killed)
        {
            result.cancelled = run.cancelled;
            result.transport = run.cancelled ? Transport::Failed : run.killedAs;
            return result;
        }
        const int code = ExitCode(run);
        if (code < 0)
            return result;
        const std::string& out = run.out;
        auto digit = [](char c) { return c >= '0' && c <= '9'; };
        if (out.size() == 3 && std::all_of(out.begin(), out.end(), digit))
            result.status = std::stoi(out);
        if (progress)
            progress(FileSize(part));
        result.detail = std::format("curl exit {}, HTTP {}", code, result.status);
        switch (code)
        {
        case 0:   // the file (or, for a 416, nothing more)
        case 22:  // an error status: --fail wrote nothing
            result.transport = result.status ? Transport::Ok : Transport::Garbled;
            return result;
        case 33:  // Range got ignored, the whole file came back
            result.rangeRefused = true;
            result.transport = Transport::Ok;
            return result;
        case 23:  // couldn't write the output
            result.writeFailed = true;
            result.transport = Transport::Failed;
            return result;
        default:
            result.transport = FromExitCode(code);
            if (result.transport == Transport::Ok)
                result.transport = Transport::Failed;
            return result;
        }
    }
}
#endif
