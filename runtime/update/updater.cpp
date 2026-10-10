// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See updater.h.
#include <stdafx.h>
#include "updater.h"
#include "fetch.h"

#include <cpu/guest_thread.h>
#include <report/report.h>
#include <ui/ui.h>

#include <ctime>
#include <fstream>

namespace update
{
    namespace
    {
        // Never destroyed: the check's thread is detached and may still be
        // finishing when the game exits.
        struct Shared
        {
            std::mutex mutex;
            Phase phase = Phase::Idle;
            uint64_t checks = 0;
            std::shared_ptr<const Result> result;
        };
        Shared& State()
        {
            static Shared* shared = new Shared;
            return *shared;
        }
        std::atomic<bool> s_dialogShowing{ false };

        // The game can exit while a check is still waiting on the network
        // (up to the timeout): from then on its thread must not touch the UI,
        // whose objects are being destroyed. An exit handler, registered at
        // the first check (so it runs before those objects' destructors),
        // takes this lock and sets the flag; the thread toasts only under it.
        std::mutex& ExitMutex()
        {
            static std::mutex* mutex = new std::mutex;
            return *mutex;
        }
        bool s_exiting = false;  // under ExitMutex()
        void NoteExit()
        {
            std::lock_guard lock(ExitMutex());
            s_exiting = true;
        }

        std::string UserAgent()
        {
            return std::format("SpeedBreaker/{}", report::Version());
        }

        // NFSMW_UPDATE_URL=<file>: the canned answer.
        Fetched ReadCanned(const std::string& path)
        {
            Fetched fetched;
            std::ifstream file(path, std::ios::binary);
            if (!file)
            {
                fetched.detail = "NFSMW_UPDATE_URL: can't read " + path;
                return fetched;
            }
            std::string raw(kMaxResponseBytes + 1, '\0');
            file.read(raw.data(), std::streamsize(raw.size()));
            raw.resize(size_t(file.gcount()));
            std::string_view first(raw);
            first = first.substr(0, first.find_first_of("\r\n"));
            if (first == "offline" || first == "timeout")
            {
                fetched.transport = first == "offline" ? Transport::Offline : Transport::Timeout;
                fetched.detail = "canned";
                return fetched;
            }
            if (raw.size() > kMaxResponseBytes)
            {
                fetched.transport = Transport::TooLarge;
                fetched.detail = "canned";
                return fetched;
            }
            if (raw.starts_with("HTTP/"))
            {
                if (ParseHttpResponse(raw, fetched.response))
                    fetched.transport = Transport::Ok;
                else
                {
                    fetched.transport = Transport::Garbled;
                    fetched.detail = "canned";
                }
                return fetched;
            }
            fetched.transport = Transport::Ok;
            fetched.response.status = 200;
            fetched.response.body = std::move(raw);
            return fetched;
        }

        std::string ToastText(const Result& r)
        {
            switch (r.outcome)
            {
            case Outcome::UpToDate: return r.headline + ".";
            case Outcome::Available: return std::format("SpeedBreaker {}: Settings > Advanced > Check for Updates.", r.headline);
            default: return r.headline + ": " + r.message;
            }
        }

        void Check()
        {
            SetHostThreadName("nfsmw-update");
            const auto started = std::chrono::steady_clock::now();
            const std::string running = RunningVersion();
            std::string source;
            Fetched fetched;
            const char* override = std::getenv("NFSMW_UPDATE_URL");
            if (override && *override)
            {
                std::string target = override;
                if (target.starts_with("http://") || target.starts_with("https://"))
                {
                    source = " from NFSMW_UPDATE_URL " + target;
                    fetched = Fetch({ .url = target, .userAgent = UserAgent(),
                        .headers = { { "Accept", "application/vnd.github+json" }, { "X-GitHub-Api-Version", "2022-11-28" } },
                        .allowHttp = true });
                }
                else
                {
                    if (target.starts_with("file://"))
                        target.erase(0, 7);
                    source = " from the file " + target;
                    fetched = ReadCanned(target);
                }
            }
            else
                fetched = Fetch({ .url = kApiUrl, .userAgent = UserAgent(),
                    .headers = { { "Accept", "application/vnd.github+json" }, { "X-GitHub-Api-Version", "2022-11-28" } } });

            Result result = Evaluate(fetched, running, int64_t(std::time(nullptr)));
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            const bool tested = running != report::Version();
            fprintf(stderr, "[update] %s (%.2f s%s%s)\n", result.log.c_str(), seconds, source.c_str(),
                tested ? ", as NFSMW_TEST_VERSION" : "");

            const bool showing = s_dialogShowing.load();
            std::string toast = showing ? std::string() : ToastText(result);
            {
                Shared& state = State();
                std::lock_guard lock(state.mutex);
                state.result = std::make_shared<const Result>(std::move(result));
                state.phase = Phase::Done;
            }
            // The dialog was closed meanwhile: say how it went.
            if (!toast.empty())
            {
                std::lock_guard lock(ExitMutex());
                if (!s_exiting)
                    ui::Toast(toast, 10.0);
            }
        }
    }

    std::string RunningVersion()
    {
        static const std::string version = [] {
            std::string v = report::Version();
            if (const char* test = std::getenv("NFSMW_TEST_VERSION"); test && *test)
            {
                Semver parsed;
                if (ParseSemver(test, parsed))
                    v = FormatSemver(parsed);
                else
                    fprintf(stderr, "[update] NFSMW_TEST_VERSION=%s isn't a version: ignored\n", test);
            }
            return v;
        }();
        return version;
    }

    void StartCheck()
    {
        static const bool exitHandler = (std::atexit(NoteExit), true);
        (void)exitHandler;
        Shared& state = State();
        {
            std::lock_guard lock(state.mutex);
            if (state.phase == Phase::Checking)
                return;
            state.phase = Phase::Checking;
            state.checks++;
        }
        try
        {
            std::thread(Check).detach();
        }
        catch (const std::system_error& e)
        {
            fprintf(stderr, "[update] check failed: no thread for it (%s)\n", e.what());
            auto result = std::make_shared<Result>();
            result->outcome = Outcome::Unavailable;
            result->current = RunningVersion();
            result->headline = "Couldn't check for updates";
            result->message = "The game couldn't start the check. Try again later.";
            result->url = kReleasesPage;
            std::lock_guard lock(state.mutex);
            state.result = std::move(result);
            state.phase = Phase::Done;
        }
    }

    Status GetStatus()
    {
        Shared& state = State();
        std::lock_guard lock(state.mutex);
        return { state.phase, state.checks, state.result };
    }

    void SetDialogShowing(bool showing)
    {
        s_dialogShowing.store(showing);
    }
}
