// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See updater.h.
#include <stdafx.h>
#include "updater.h"
#include "fetch.h"
#include "startup_toast.h"

#include <cpu/guest_thread.h>
#include <report/report.h>
#include <ui/ui.h>
#include <user/paths.h>
#include <user/settings.h>

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
            Kind kind = Kind::Pressed;      // the running or last check's
            bool pressedWhileRunning = false;  // a startup check the player then asked for too
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

        // The startup check (main thread only).
        bool s_afterUpdate = false;
        bool s_windowUp = false;
        std::chrono::steady_clock::time_point s_windowUpAt;
        bool s_startupSettled = false;  // started, or skipped for this launch
        bool s_loggedOff = false;       // "off" logged while waiting for the setting
        bool s_toastSettled = false;    // the startup result's toast handled for this launch
        std::string s_toastText;        // the startup toast shown ("": none yet)
        std::chrono::steady_clock::time_point s_toastAt;  // ...when
        bool s_announcedRead = false, s_announcedBefore = false;  // NotifiedTag() read once

        std::string UserAgent()
        {
            return std::format("SpeedBreaker/{}", report::Version());
        }

        bool IsHttp(std::string_view s)
        {
            return s.starts_with("http://") || s.starts_with("https://");
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

        // How to open the settings, as the rest of the game words it.
        const char* SettingsKeys()
        {
#if defined(__APPLE__) && TARGET_OS_IOS
            return "View + Menu, or a three-finger tap";
#elif defined(__APPLE__)
            return "F1, Cmd+, or Back + Start";
#else
            return "F1 or Back + Start";
#endif
        }

        std::string StartupToastText(const Result& r)
        {
            return std::format(SB_APP_NAME " {} is available: open Settings ({}) to update.", r.label, SettingsKeys());
        }

        std::string ToastText(const Result& r)
        {
            switch (r.outcome)
            {
            case Outcome::UpToDate: return r.headline + ".";
            case Outcome::Available: return std::format(SB_APP_NAME " {}: Settings > Advanced > Check for Updates.", r.headline);
            default: return r.headline + ": " + r.message;
            }
        }

        // The release the startup toast last announced (<data>/updates/notified.txt).
        std::filesystem::path NotifiedPath()
        {
            return GetUserPath() / "updates" / "notified.txt";
        }

        std::string NotifiedTag()
        {
            std::ifstream file(NotifiedPath(), std::ios::binary);
            std::string tag;
            if (file)
                std::getline(file, tag);
            if (!tag.empty() && tag.back() == '\r')
                tag.pop_back();
            return tag;
        }

        void RecordNotified(const std::string& tag)
        {
            if (tag.empty() || NotifiedTag() == tag)
                return;
            std::error_code ec;
            const std::filesystem::path path = NotifiedPath();
            std::filesystem::create_directories(path.parent_path(), ec);
            const std::filesystem::path temp = path.parent_path() / "notified.txt.new";
            {
                std::ofstream out(temp, std::ios::binary | std::ios::trunc);
                out << tag << "\n";
                if (!out.flush())
                {
                    fprintf(stderr, "[update] couldn't note %s as announced (%s): it may be announced again\n", tag.c_str(),
                        temp.string().c_str());
                    return;
                }
            }
            std::filesystem::rename(temp, path, ec);
            if (ec)
                fprintf(stderr, "[update] couldn't note %s as announced (%s): it may be announced again\n", tag.c_str(), ec.message().c_str());
        }

        void Check()
        {
            SetHostThreadName("nfsmw-update");
            const auto started = std::chrono::steady_clock::now();
            const Running running = RunningBuild();
            const bool list = running.device.platform == Platform::Windows;
            const std::vector<std::pair<std::string, std::string>> headers = { { "Accept", "application/vnd.github+json" },
                { "X-GitHub-Api-Version", "2022-11-28" } };
            std::string source;
            Fetched fetched;
            const char* override = std::getenv("NFSMW_UPDATE_URL");
            const char* feed = std::getenv("NFSMW_UPDATE_FEED");
            if (override && *override)
            {
                std::string target = override;
                if (IsHttp(target))
                {
                    source = " from NFSMW_UPDATE_URL " + target;
                    fetched = Fetch({ .url = target, .userAgent = UserAgent(), .headers = headers, .allowHttp = true });
                }
                else
                {
                    if (target.starts_with("file://"))
                        target.erase(0, 7);
                    source = " from the file " + target;
                    fetched = ReadCanned(target);
                }
            }
            else if (feed && IsHttp(feed))
            {
                std::string base = feed;
                if (!base.ends_with('/'))
                    base += '/';
                const std::string url = base + (list ? kListPath : kLatestPath);
                source = " from NFSMW_UPDATE_FEED " + url;
                fetched = Fetch({ .url = url, .userAgent = UserAgent(), .headers = headers, .allowHttp = true });
            }
            else if (feed && *feed)
            {
                // A test run's mistake never falls back to GitHub itself.
                source = std::string(" from NFSMW_UPDATE_FEED ") + feed;
                fetched.transport = Transport::Failed;
                fetched.detail = "NFSMW_UPDATE_FEED isn't an http(s) address";
            }
            else
                fetched = Fetch({ .url = std::string(kApiBase) + (list ? kListPath : kLatestPath), .userAgent = UserAgent(), .headers = headers });

            Result result = Evaluate(fetched, running, int64_t(std::time(nullptr)));
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            const bool tested = running.version != report::Version();

            Kind kind;
            {
                Shared& state = State();
                std::lock_guard lock(state.mutex);
                if (state.pressedWhileRunning)
                    state.kind = Kind::Pressed;
                kind = state.kind;
            }
            fprintf(stderr, "[update] %s%s (%.2f s%s%s)\n", kind == Kind::Startup ? "startup check: " : "", result.log.c_str(), seconds,
                source.c_str(), tested ? ", as NFSMW_TEST_VERSION" : "");

            // A pressed check whose dialog was closed meanwhile says how it
            // went in a toast; the startup check's toast waits for Tick.
            const bool showing = s_dialogShowing.load();
            std::string toast = showing || kind == Kind::Startup ? std::string() : ToastText(result);
            {
                Shared& state = State();
                std::lock_guard lock(state.mutex);
                state.result = std::make_shared<const Result>(std::move(result));
                state.phase = Phase::Done;
            }
            if (!toast.empty())
            {
                std::lock_guard lock(ExitMutex());
                if (!s_exiting)
                    ui::Toast(toast, 10.0);
            }
        }

        void Start(Kind kind)
        {
            static const bool exitHandler = (std::atexit(NoteExit), true);
            (void)exitHandler;
            Shared& state = State();
            {
                std::lock_guard lock(state.mutex);
                if (state.phase == Phase::Checking)
                {
                    if (kind == Kind::Pressed)
                        state.pressedWhileRunning = true;
                    return;
                }
                state.phase = Phase::Checking;
                state.checks++;
                state.kind = kind;
                state.pressedWhileRunning = false;
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

        // A scripted run (another job's smoke test, a capture plan): what
        // gives it away, or nullptr.
        const char* ScriptedBy()
        {
            for (const char* name : { "NFSMW_VIRTUAL_PAD", "NFSMW_UI_KEYS", "NFSMW_INPUT_SCRIPT", "NFSMW_CHECK_PRESENT_SEC", "NFSMW_TEST_NO_BROWSER" })
                if (const char* v = std::getenv(name); v && *v)
                    return name;
            return nullptr;
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

    Running RunningBuild()
    {
        Running r;
        r.version = RunningVersion();
        r.tag = report::ReleaseTag();
        // NFSMW_TEST_VERSION: a tagged build compares as that version's tag.
        if (!r.tag.empty() && r.version != report::Version())
            r.tag = "v" + r.version;
        r.devBuild = r.tag.empty() || report::Dirty();
        r.device = ThisDevice();
        return r;
    }

    void StartCheck()
    {
        Start(Kind::Pressed);
    }

    Status GetStatus()
    {
        Shared& state = State();
        std::lock_guard lock(state.mutex);
        return { state.phase, state.checks, state.result, state.kind };
    }

    void SetDialogShowing(bool showing)
    {
        s_dialogShowing.store(showing);
    }

    void NoteSeen(const std::string& tag)
    {
        RecordNotified(tag);
    }

    void NoteAfterUpdate()
    {
        s_afterUpdate = true;
    }

    void NoteWindowUp()
    {
        if (s_windowUp)
            return;
        s_windowUp = true;
        s_windowUpAt = std::chrono::steady_clock::now();
    }

    void Tick()
    {
        if (!s_windowUp)
            return;
        if (!s_startupSettled &&
            std::chrono::duration<double>(std::chrono::steady_clock::now() - s_windowUpAt).count() >= kStartupDelaySeconds)
        {
            const char* explicitly = std::getenv("NFSMW_UPDATE_CHECK");
            const bool asked = explicitly && explicitly[0] == '1';
            const char* feedVar = std::getenv("NFSMW_UPDATE_FEED");
            const char* urlVar = std::getenv("NFSMW_UPDATE_URL");
            const bool feed = (feedVar && *feedVar) || (urlVar && *urlVar);  // (an unusable feed fails: never GitHub)
            if (s_afterUpdate)
            {
                fprintf(stderr, "[update] startup check: skipped (the game just restarted after an update)\n");
                s_startupSettled = true;
            }
            else if (GetStatus().checks > 0)
                s_startupSettled = true;  // the player already pressed Check for Updates
            else if (!settings::GetBool(settings::Id::UpdateCheck))
            {
                // Waits: turning the setting on later in this launch checks then.
                if (!s_loggedOff)
                {
                    const char* env = settings::EnvOverride(settings::Id::UpdateCheck);
                    fprintf(stderr, "[update] startup check: off (%s)\n",
                        env ? std::format("{}=0", env).c_str() : "Settings > Advanced > Check for Updates at Startup");
                    s_loggedOff = true;
                }
            }
            else if (const char* by = ScriptedBy(); by && !feed && !asked)
            {
                fprintf(stderr, "[update] startup check: skipped in a scripted run (%s is set; NFSMW_UPDATE_FEED or NFSMW_UPDATE_CHECK=1 checks)\n",
                    by);
                s_startupSettled = true;
            }
            else
            {
                fprintf(stderr, "[update] startup check: asking %s\n",
                    feed ? "the test feed" : (ThisDevice().platform == Platform::Windows ? "GitHub for the ten newest releases" : "GitHub for the latest release"));
                Start(Kind::Startup);
                s_startupSettled = true;
            }
        }
        // The startup check found a newer version: one toast, once per
        // version, once no panel (the installer, What's new) is up; taken
        // down for good once Settings opens (update/startup_toast.h).
        if (s_startupSettled && !s_toastSettled)
        {
            const Status status = GetStatus();
            if (status.phase == Phase::Checking)
                return;
            if (status.phase != Phase::Done && status.checks == 0)
            {
                s_toastSettled = true;  // no startup check this launch
                return;
            }
            ToastInputs in;
            in.resultReady = status.phase == Phase::Done;
            in.available = in.resultReady && status.kind == Kind::Startup && status.result && status.result->outcome == Outcome::Available;
            in.modal = ui::HasModal();
            in.settingsOpen = ui::SettingsOpen();
            in.shown = !s_toastText.empty();
            if (in.available && !in.shown)
            {
                if (!s_announcedRead)
                {
                    s_announcedBefore = NotifiedTag() == status.result->latestTag;
                    s_announcedRead = true;
                }
                in.announcedBefore = s_announcedBefore;
            }
            switch (NextToastStep(in))
            {
            case ToastStep::Wait:
                // Its time on screen is over (12 s; a little more for a queue): nothing left to take down.
                if (in.shown && std::chrono::steady_clock::now() - s_toastAt > std::chrono::seconds(30))
                    s_toastSettled = true;
                break;
            case ToastStep::Done:
                if (in.available && in.announcedBefore)
                    fprintf(stderr, "[update] startup check: %s was announced before: no toast (the Settings footer still offers it)\n",
                        status.result->latestTag.c_str());
                s_toastSettled = true;
                break;
            case ToastStep::Skip:
                fprintf(stderr, "[update] startup check: Settings is open: no toast (its footer offers %s)\n", status.result->latestTag.c_str());
                RecordNotified(status.result->latestTag);
                s_toastSettled = true;
                break;
            case ToastStep::Show:
                s_toastText = StartupToastText(*status.result);
                fprintf(stderr, "[update] toast: %s\n", s_toastText.c_str());
                ui::Toast(s_toastText, 12.0);
                s_toastAt = std::chrono::steady_clock::now();
                RecordNotified(status.result->latestTag);
                break;
            case ToastStep::Withdraw:
                ui::WithdrawToast(s_toastText);
                fprintf(stderr, "[update] Settings opened: the update toast is taken down\n");
                s_toastSettled = true;
                break;
            }
        }
    }

    std::string FooterLabel()
    {
        const Status status = GetStatus();
        if (!status.result || status.result->outcome != Outcome::Available)
            return {};
        return "Update to " + status.result->label;
    }
}
