// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The first-run installer screen: pick the player's own disc image (found
// automatically, or browsed for), check it, copy and verify it with progress,
// then start the game. The blocking work (the scan, the check, the install)
// runs on a worker thread; this file only draws and reacts.
#include <stdafx.h>
#include "installer_screen.h"
#include "ui.h"

#include <install/installer.h>
#include <install/locate.h>
#include <install/placement.h>
#include <video/presenter.h>

#include <imgui.h>
#include <imgui_internal.h>

#include <SDL3/SDL.h>

#include <condition_variable>
#include <future>
#include <thread>
#include <utility>

#if defined(__APPLE__) && TARGET_OS_IOS
#include <platform/ios_files.h>
#elif defined(__APPLE__)
#include <platform/file_dialog.h>
#endif

namespace ui
{
    namespace
    {
        // File dialogs (SDL's; on iOS, the Files app's picker) may answer
        // after the screen is gone (the window closed first), or after the
        // player went on another way, so their result lives here, tagged
        // with the request it answers.
        std::mutex s_dialogMutex;
        uintptr_t s_dialogRequest = 0;  // the latest Browse; older answers are dropped
        std::optional<std::string> s_dialogPick;
        uint64_t s_pickAccess = 0;      // iOS: the Files app's grant to read s_dialogPick (ios_files.h)
        bool s_dialogFailed = false;
        std::string s_dialogError;      // SDL's word on why, when s_dialogFailed
        bool s_dialogCancelled = false; // the player closed the dialog without choosing
        std::string s_dialogNotice;     // iOS: the pick isn't on the device yet; what to do instead
        // What the latest dialog is for: the disc image or folder to install
        // from (the Choose page), or the folder to install into (Confirm's
        // "Choose a folder...").
        enum class DialogFor { Source, Destination };
        DialogFor s_dialogFor = DialogFor::Source;

        // Gives back a grant to read a picked file (iOS; elsewhere there are
        // none, and `access` is 0).
        void ReleaseAccess(uint64_t access)
        {
#if defined(__APPLE__) && TARGET_OS_IOS
            platform::ios::ReleaseFileAccess(access);
#else
            (void)access;
#endif
        }

        // Drops a pick nobody will check, and its grant. s_dialogMutex held.
        void ForgetPick()
        {
            s_dialogPick.reset();
            ReleaseAccess(std::exchange(s_pickAccess, 0));
        }

#if !(defined(__APPLE__) && TARGET_OS_IOS)
        void SDLCALL OnDialog(void* request, const char* const* files, int)
        {
            std::lock_guard lock(s_dialogMutex);
            if (reinterpret_cast<uintptr_t>(request) != s_dialogRequest)
                return;
            if (files && files[0])
                s_dialogPick = files[0];
            else if (!files)
            {
                s_dialogFailed = true;
                s_dialogError = SDL_GetError();
                fprintf(stderr, "[installer] file dialog failed: %s\n", s_dialogError.c_str());
            }
            else
            {
                s_dialogCancelled = true;
                fprintf(stderr, "[installer] file dialog cancelled\n");
            }
        }
#endif

        // Opens a file (or folder) dialog for a new request.
        void Browse(bool folder, DialogFor purpose = DialogFor::Source)
        {
            void* request;
            {
                std::lock_guard lock(s_dialogMutex);
                request = reinterpret_cast<void*>(++s_dialogRequest);
                ForgetPick();
                s_dialogFailed = false;
                s_dialogError.clear();
                s_dialogCancelled = false;
                s_dialogNotice.clear();
                s_dialogFor = purpose;
            }
#if defined(__APPLE__) && TARGET_OS_IOS
            // SDL has no file dialog on iOS: the Files app's document picker,
            // which answers on the main thread (inside SDL_PollEvent) with the
            // file and a grant to read it where it is.
            bool shown = platform::ios::PickDiscImage(GetWindow(), folder,
                [request](std::optional<platform::ios::PickedFile> picked) {
                    std::lock_guard lock(s_dialogMutex);
                    if (reinterpret_cast<uintptr_t>(request) != s_dialogRequest)
                    {
                        if (picked)
                            ReleaseAccess(picked->access);
                        return;
                    }
                    if (picked && !picked->notHere.empty())
                        s_dialogNotice = std::move(picked->notHere);  // nothing to read yet (no grant held)
                    else if (picked)
                    {
                        ForgetPick();
                        s_dialogPick = std::move(picked->path);
                        s_pickAccess = picked->access;
                    }
                });
            if (!shown)
            {
                std::lock_guard lock(s_dialogMutex);
                s_dialogFailed = true;
            }
#else
            static const SDL_DialogFileFilter filters[] = { { "Xbox 360 disc image", "iso" }, { "All files", "*" } };
            if (purpose == DialogFor::Destination)
            {
                // Where to install: titled as such (where the platform shows a
                // title: the portal's and zenity's windows; macOS's panel shows
                // the accept label). Not the accept label elsewhere: SDL hands
                // it to zenity as --ok-label, which zenity 3.44 (SteamOS)
                // refuses for a file chooser, so no picker would open at all.
                SDL_PropertiesID props = SDL_CreateProperties();
                SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, GetWindow());
                if (const char* home = std::getenv("HOME"))
                    SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_LOCATION_STRING, home);
                SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, "Choose where to install the game");
#ifdef __APPLE__
                SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_ACCEPT_STRING, "Choose");
#endif
                SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_OPENFOLDER, OnDialog, request, props);
                SDL_DestroyProperties(props);
            }
            else if (folder)
                SDL_ShowOpenFolderDialog(OnDialog, request, GetWindow(), std::getenv("HOME"), false);
            else
                SDL_ShowOpenFileDialog(OnDialog, request, GetWindow(), filters, 2, std::getenv("HOME"), false);
#endif
        }

        // Forget any dialog still open: the player chose another way.
        void DropDialog()
        {
            std::lock_guard lock(s_dialogMutex);
            ++s_dialogRequest;
            ForgetPick();
            s_dialogFailed = false;
            s_dialogError.clear();
            s_dialogCancelled = false;
        }

        std::string FileName(const std::filesystem::path& path)
        {
            std::string name = path.filename().string();
            return name.empty() ? path.string() : name;
        }

        // A Files picker on screen, or on its way on or off it (iOS). The
        // Choose page acts on nothing meanwhile: a controller still reaches
        // it behind the picker, and UIKit refuses a second picker while one
        // slides in or out, which would lose the pick.
        bool PickerShown()
        {
#if defined(__APPLE__) && TARGET_OS_IOS
            return platform::ios::IsPickerShown();
#else
            return false;
#endif
        }

        // The home folder as ~.
        std::string HomeRelative(const std::filesystem::path& path)
        {
            std::string text = path.string();
            if (const char* home = std::getenv("HOME"); home && *home)
            {
                std::string_view h = home;
                while (h.size() > 1 && h.back() == '/')
                    h.remove_suffix(1);
                if (text.starts_with(h) && (text.size() == h.size() || text[h.size()] == '/'))
                    text = "~" + text.substr(h.size());
            }
            return text;
        }

        // A folder for display, shortened from the left with "..." to fit `width`.
        std::string DisplayPath(const std::filesystem::path& path, float width)
        {
            std::string text = HomeRelative(path);
            if (ImGui::CalcTextSize(text.c_str()).x <= width)
                return text;
            size_t cut = 0;
            while (cut < text.size() && ImGui::CalcTextSize(("..." + text.substr(cut)).c_str()).x > width)
                cut++;
            return "..." + text.substr(cut);
        }

        std::string Duration(double seconds)
        {
            if (seconds < 60)
                return std::format("{} s", std::max(1, int(seconds + 0.5)));
            int minutes = int(seconds / 60 + 0.5);
            return minutes < 60 ? std::format("{} min", minutes) : std::format("{} h {} min", minutes / 60, minutes % 60);
        }

        // Folders the scan reads without asking. On macOS, Desktop,
        // Documents, Downloads and removable volumes each raise a privacy
        // prompt on first access, so they are left to the file dialog there;
        // so is the home folder, whose one-level-down scan would open them.
        std::vector<std::filesystem::path> ScanFolders()
        {
#if defined(__APPLE__) && TARGET_OS_IOS
            // iOS: the app's own Documents folder, the one the Files app shows
            // (On My iPad > SpeedBreaker) and Finder copies into, and its
            // Inbox, where iOS puts a file another app hands over. (Nothing
            // does yet: Info.plist declares no document types, so SpeedBreaker
            // isn't offered in Open in or AirDrop.)
            std::vector<std::filesystem::path> folders;
            if (const char* home = std::getenv("HOME"); home && *home)
                for (const char* sub : { "Documents", "Documents/Inbox" })
                {
                    std::error_code ec;
                    std::filesystem::path dir = std::filesystem::path(home) / sub;
                    if (std::filesystem::is_directory(dir, ec))
                        folders.push_back(dir);
                }
            return folders;
#else
            std::vector<std::filesystem::path> folders = install::DiscImageSearchFolders();
#endif
#if defined(__APPLE__) && !TARGET_OS_IOS
            const char* home = std::getenv("HOME");
            std::erase_if(folders, [home](const std::filesystem::path& folder) {
                std::string name = folder.filename().string();
                return name == "Desktop" || name == "Documents" || name == "Downloads" || folder.parent_path() == "/Volumes" ||
                    (home && std::filesystem::path(home).lexically_normal() == folder.lexically_normal());
            });
#endif
            // EmuDeck's ROM folder, in the home folder and on SD cards.
            std::vector<std::filesystem::path> extra;
            for (const auto& folder : folders)
            {
                std::error_code ec;
                std::filesystem::path roms = folder / "Emulation" / "roms" / "xbox360";
                if (std::filesystem::is_directory(roms, ec))
                    extra.push_back(roms);
            }
            folders.insert(folders.end(), extra.begin(), extra.end());
            return folders;
        }

#if defined(__APPLE__) && TARGET_OS_IOS
        const char* kWhereToPut = "Choose Browse for image... to pick your .iso where it is (iCloud Drive, a USB drive, "
                                  "another app's folder), or copy it into SpeedBreaker's folder in the Files app (On My "
                                  "iPad or On My iPhone > SpeedBreaker) and choose Rescan.";
        const char* kScanning = "Looking for disc images in SpeedBreaker's folder...";
#elif defined(__APPLE__)
        const char* kWhereToPut = "Put your .iso file in a Games folder in your home folder and choose Rescan, or browse for it.";
        const char* kScanning = "Looking for disc images in ~/Games...";
#else
        const char* kWhereToPut = "Put your .iso file in your home folder, Downloads, a Games folder, or on an SD card or USB "
                                  "drive and choose Rescan, or browse for it.";
        const char* kScanning = "Looking for disc images in your home folder, Games and removable drives...";
#endif
#if defined(__APPLE__) && TARGET_OS_IOS
        const char* kNoDialog = "The Files picker couldn't be shown. Copy your .iso into SpeedBreaker's folder in the Files "
                                "app (On My iPad or On My iPhone > SpeedBreaker) and choose Rescan.";
#else
        const char* kNoDialog = "The file browser isn't available here (in Game Mode, for example). Put the .iso in Downloads, "
                                "a Games folder or on an SD card and choose Rescan, or type its path below (Steam + X shows "
                                "the keyboard).";
#endif

#if !(defined(__APPLE__) && TARGET_OS_IOS)
        // A path to type in a terminal: ~ for home, quoted (and absolute) if
        // it has a space or another character the shell would split at.
        std::string ShellPath(const std::filesystem::path& path)
        {
            std::string text = path.string();
            if (text.find_first_of(" \t'\"$`\\;&|<>()*?[]!#") == std::string::npos)
                return HomeRelative(path);
            std::string quoted = "'";
            for (char c : text)
                quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
            return quoted + "'";
        }

        // The hint when the folder picker can't be shown (install::FolderPickerHint),
        // with the command for this install: the bundle's launcher or the Mac
        // app's binary, and the image already chosen. Game Mode is SteamOS's
        // (on a Mac only NFSMW_TEST_PICK_FOLDER=gamemode gets there): its
        // words and launcher wherever it shows.
        // `noPickerApp`: SDL found neither a file chooser portal nor zenity.
        std::string FolderPickerHint(bool gameMode, const std::filesystem::path& source, bool noPickerApp = false)
        {
            std::filesystem::path bundle = install::BundleFolder();
            std::string image = source.empty() ? std::string("<image>") : ShellPath(source);
            std::error_code ec;
            std::string launcher = !bundle.empty() && std::filesystem::exists(bundle / "speedbreaker.sh", ec)
                ? ShellPath(bundle / "speedbreaker.sh") : std::string("speedbreaker.sh");
#ifdef __APPLE__
            if (!gameMode)
                return install::FolderPickerHint(false, true, false, bundle.extension() == ".app"
                    ? ShellPath(bundle / "Contents/MacOS/SpeedBreaker") : std::string("SpeedBreaker"), image);
#endif
            return install::FolderPickerHint(gameMode, false, noPickerApp, launcher, image);
        }
#else
        // iOS: no "Choose a folder..." (the game stays in the app's storage).
        std::string FolderPickerHint(bool, const std::filesystem::path&, bool = false)
        {
            return {};
        }
#endif

        const char* StatusText(const install::DiscImageCandidate& c)
        {
            using install::Error;
            switch (c.status.error)
            {
            case Error::None: return "Ready to install";
            case Error::UnsupportedVersion: return "Unsupported version";
            case Error::WrongGame: return "Another game";
            case Error::TruncatedImage: return "Incomplete image";
            case Error::ReadError: return "Can't be read";
            default: return "Can't be installed";
            }
        }

        const char* FailureTitle(install::Error error)
        {
            using install::Error;
            switch (error)
            {
            case Error::NotFound: return "Not found";
            case Error::NotADiscImage: return "Not an Xbox 360 disc image";
            case Error::UnsupportedFormat: return "Unsupported format";
            case Error::TruncatedImage: return "The image is incomplete";
            case Error::CorruptImage: return "The image is damaged";
            case Error::WrongGame: return "This is another game";
            case Error::UnsupportedVersion: return "Unsupported version of the game";
            case Error::MissingFile:
            case Error::CorruptFile: return "The disc image has bad files";
            case Error::ReadError: return "Couldn't read the disc image";
            case Error::DestinationNotWritable:
            case Error::DestinationNotEmpty: return "Can't install there";
            case Error::NotEnoughSpace: return "Not enough free space";
            case Error::WriteError: return "Couldn't write the game files";
            case Error::Cancelled: return "Install cancelled";
            default: return "Install failed";
            }
        }

        // A check of a disc image or folder, on a thread of its own. A read
        // can't be interrupted, and one can wait for minutes (a file a cloud
        // service is still downloading, a drive that stopped answering), so
        // Back leaves the thread behind instead of waiting for it. Left
        // behind, it drops its result and gives the grant to read the file
        // back itself once the read returns: not before, while it still reads.
        struct CheckJob
        {
            std::mutex mutex;
            std::condition_variable finished;
            bool done = false;
            bool abandoned = false;
            uint64_t access = 0;  // once abandoned: the grant the thread gives back
            install::Result result;
            std::unique_ptr<install::DiscSource> source;

            static std::shared_ptr<CheckJob> Start(const std::filesystem::path& path)
            {
                auto job = std::make_shared<CheckJob>();
                std::thread([job, path] {
                    std::unique_ptr<install::DiscSource> opened;
                    install::Result r = install::OpenDiscSource(path, opened);
                    if (r.Ok())
                        r = install::CheckSource(*opened);
                    uint64_t release = 0;
                    {
                        std::lock_guard lock(job->mutex);
                        job->done = true;
                        if (job->abandoned)
                        {
                            release = std::exchange(job->access, 0);
                            fprintf(stderr, "[installer] the check of %s that was left behind has returned\n", path.c_str());
                        }
                        else
                        {
                            if (r.Ok())
                                job->source = std::move(opened);
                            job->result = std::move(r);
                        }
                    }
                    job->finished.notify_all();
                    opened.reset();  // the file closed before its grant goes
                    ReleaseAccess(release);
                }).detach();
                return job;
            }
        };
    }

    struct InstallerScreen::State
    {
        enum class Page { Choose, Checking, Confirm, Installing, Done, Failed };
        Page page = Page::Choose;

        // Scan.
        std::future<std::vector<install::DiscImageCandidate>> scan;
        std::vector<install::DiscImageCandidate> images;
        bool scanned = false;
        std::atomic<bool> scanCancel{ false };

        // Check and install (one at a time): the check as `check` (CheckJob),
        // the install as `task`.
        std::shared_ptr<CheckJob> check;
        std::future<install::Result> task;
        std::unique_ptr<install::DiscSource> source;
        std::filesystem::path sourcePath;
        // iOS: the Files app's grant to read sourcePath, when it was picked
        // outside SpeedBreaker's folder. Held while anything may still read
        // it: from the check through the install, and on a failed install's
        // page (Try again); given back on the way to Choose or Done.
        uint64_t access = 0;
        std::atomic<bool> cancel{ false };
        install::Result result;
        std::filesystem::path destination = install::DefaultInstallPath();

        // Where to install: internal storage or a removable drive, each
        // checked (space, permissions) when the Confirm page opens, then
        // (not on iOS, whose app keeps the game in its own storage, nor yet
        // on Windows: install/placement.h) a folder the player picks.
        struct Destination
        {
            install::InstallDestination where;  // the custom entry's path: where in the picked folder the game goes
            install::Result check;
            install::DestinationInfo info;
            bool custom = false;                // "Choose a folder..."
            install::FolderChoice choice;       // custom, once a folder is picked
        };
        std::vector<Destination> destinations;
        size_t chosen = 0;

        // "Choose a folder...": the folder picked (kept across visits to the
        // Confirm page), or the one recorded last time when it isn't one of
        // the others (`exact`: offered as it is, no subfolder).
        std::optional<std::filesystem::path> customFolder;
        bool customExact = false;
        bool preferCustom = false;      // the player picked it: keep it chosen
        enum class Pick { Idle, Waiting, Unavailable };
        Pick pick = Pick::Idle;         // the folder picker: open, or couldn't be shown (pickHint says why)
        std::string pickHint;
        // The system's picker asked for (not the test hook's), and when:
        // NFSMW_TEST_DIALOG_CANCEL cancels it after that many seconds (macOS).
        bool pickShown = false;
        std::chrono::steady_clock::time_point pickStart{};

        Destination CustomDestination()
        {
            Destination d;
            d.custom = true;
            d.where.label = "Choose a folder...";
            if (!customFolder)
            {
                d.check.error = install::Error::DestinationNotWritable;  // nothing chosen yet (not shown as an error)
                return d;
            }
            d.choice = customExact ? install::CheckExactFolder(*customFolder, false) : install::CheckPickedFolder(*customFolder);
            d.where.label = customExact ? "Folder chosen last time" : "Chosen folder";
            d.where.path = d.choice.gameDir;
            d.check = d.choice.result;
            d.info = d.choice.info;
            return d;
        }

        void LoadDestinations()
        {
            destinations.clear();
            for (auto& where : install::InstallDestinations())
            {
                Destination d{ where };
                d.check = install::CheckDestination(where.path, &d.info);
                destinations.push_back(std::move(d));
            }
            std::optional<std::filesystem::path> recorded = install::RecordedInstallPath();
#if !(defined(__APPLE__) && TARGET_OS_IOS) && !defined(_WIN32)
            // A folder chosen last time (not internal storage or a drive's
            // own folder) comes back as the chosen folder, as it is. The same
            // folder however it was spelled (`--dest dir/`, or through a link).
            if (!customFolder && recorded && std::none_of(destinations.begin(), destinations.end(),
                    [&](const Destination& d) { return install::SameFolder(d.where.path, *recorded); }))
            {
                customFolder = *recorded;
                customExact = true;
            }
            destinations.push_back(CustomDestination());
#endif
            // The folder the player just picked, else the one used last time,
            // if it still works; else internal storage, else the first that works.
            auto preferred = [&](const Destination& d) {
                if (!d.check.Ok())
                    return false;
                if (preferCustom)
                    return d.custom;
                return recorded && install::SameFolder(d.where.path, *recorded);
            };
            chosen = 0;
            for (size_t i = 0; i < destinations.size(); i++)
                if (preferred(destinations[i]))
                {
                    chosen = i;
                    break;
                }
            if (!destinations[chosen].check.Ok())
                for (size_t i = 0; i < destinations.size(); i++)
                    if (destinations[i].check.Ok())
                    {
                        chosen = i;
                        break;
                    }
            destination = destinations[chosen].where.path;
        }

        // The player picked `folder` for "Choose a folder...": check it and choose it.
        void PickFolder(const std::filesystem::path& folder)
        {
            customFolder = folder;
            customExact = false;
            preferCustom = true;
            pick = Pick::Idle;
            pickShown = false;
            for (size_t i = 0; i < destinations.size(); i++)
                if (destinations[i].custom)
                {
                    destinations[i] = CustomDestination();
                    chosen = i;
                    const install::FolderChoice& c = destinations[i].choice;
                    fprintf(stderr, "[installer] folder picked: %s; the game would go to %s%s%s\n", folder.string().c_str(),
                        c.gameDir.string().c_str(), c.result.Ok() ? "" : ": refused, ", c.result.Ok() ? "" : c.result.message.c_str());
                }
            destination = destinations[chosen].where.path;
            focusPage = true;
        }

        // "Choose a folder...": the system's folder picker, or the hint when
        // it can't be shown. Never waits: the answer comes in a later frame.
        void RequestFolder()
        {
#if !(defined(__APPLE__) && TARGET_OS_IOS) && !defined(_WIN32)
            // The system's picker is already open: A again (a controller
            // still reaches the page behind a macOS sheet) waits for it. A
            // second request would make the open one's answer a stale one,
            // and on the Mac queue a second sheet that never comes.
            if (pick == Pick::Waiting && pickShown)
            {
                fprintf(stderr, "[installer] the folder picker is already open\n");
                return;
            }
            pickShown = false;
            // NFSMW_TEST_PICK_FOLDER stands in for the picker (automated runs):
            // a folder (absolute, or ~/...) is the player's pick; "cancel",
            // "fail" (no dialog could open), "gamemode" (as in Game Mode) and
            // "wait" (it never answers) are the other ways a picker ends.
            // Several, split by |, answer one request each (the last repeats).
            if (const char* test = std::getenv("NFSMW_TEST_PICK_FOLDER"); test && *test)
            {
                static size_t answered = 0;
                std::string_view rest = test;
                std::string value;
                for (size_t i = 0; i <= answered && !rest.empty(); i++)
                {
                    size_t bar = rest.find('|');
                    value = std::string(rest.substr(0, bar));
                    rest = bar == std::string_view::npos ? std::string_view() : rest.substr(bar + 1);
                }
                answered++;
                fprintf(stderr, "[installer] NFSMW_TEST_PICK_FOLDER stands in for the folder picker: %s\n", value.c_str());
                if (value.starts_with("~/"))
                    if (const char* home = std::getenv("HOME"))
                        value = std::string(home) + value.substr(1);
                if (value == "gamemode" || value == "fail")
                {
                    DropDialog();
                    pick = Pick::Unavailable;
                    pickHint = FolderPickerHint(value == "gamemode", sourcePath);
                }
                else if (value == "cancel")
                {
                    DropDialog();
                    pick = Pick::Idle;
                }
                else
                {
                    std::lock_guard lock(s_dialogMutex);
                    ++s_dialogRequest;
                    ForgetPick();
                    s_dialogFailed = s_dialogCancelled = false;
                    s_dialogFor = DialogFor::Destination;
                    pick = Pick::Waiting;
                    if (value != "wait")
                        s_dialogPick = value;
                }
                return;
            }
            if (video::GameMode())
            {
                // Nothing of the desktop shows under gamescope: a portal's
                // window would open where the player can't see or reach it.
                DropDialog();
                pick = Pick::Unavailable;
                pickHint = FolderPickerHint(true, sourcePath);
                fprintf(stderr, "[installer] Game Mode: the folder picker isn't tried (%s)\n", pickHint.c_str());
                return;
            }
            pick = Pick::Waiting;
            pickShown = true;
            pickStart = std::chrono::steady_clock::now();
            Browse(true, DialogFor::Destination);
#endif
        }

        // Progress, written by the install thread.
        std::mutex progressMutex;
        install::Progress progress;
        std::string currentFile;
        std::chrono::steady_clock::time_point copyStart{};
        uint64_t copyStartBytes = 0;

        char typedPath[1024] = {};
        bool focusPage = true;  // put the controller's cursor on the new page's main button
        std::string notice;     // one line under the title of the Choose page

        void StartScan()
        {
            if (scan.valid())
                return;
            scanCancel = false;
            scan = std::async(std::launch::async, [this] {
                return install::FindDiscImages(ScanFolders(), 1ull << 30, &scanCancel);
            });
        }

        void Go(Page next)
        {
            // Off the Confirm page: a folder picker still open answers nothing.
            // On the Mac it closes (it is a sheet on the game's window, and
            // would stay over the next page); elsewhere it is a window of its
            // own that SDL can't close, so the Choose page says so.
            if (page == Page::Confirm && next != Page::Confirm && pick != Pick::Idle)
            {
                const bool shown = pick == Pick::Waiting && pickShown;
                DropDialog();
                pick = Pick::Idle;
                pickShown = false;
#if defined(__APPLE__) && !TARGET_OS_IOS
                if (shown)
                    fprintf(stderr, "[installer] closed %d folder picker(s) on leaving the page\n",
                        platform::file_dialog::CancelOpenPanels());
#else
                if (shown && next == Page::Choose)
                    notice = "The folder picker is still open: close it. A folder chosen in it now isn't used.";
#endif
            }
            page = next;
            focusPage = true;
            if (next == Page::Choose || next == Page::Done || (next == Page::Failed && !source))
                ReleaseAccess(std::exchange(access, 0));
        }

        // `grant`: the Files app's grant to read `path` (iOS), held from now on.
        void StartCheck(const std::filesystem::path& path, uint64_t grant = 0)
        {
            DropDialog();
            LeaveCheck();
            sourcePath = path;
            source.reset();
            ReleaseAccess(std::exchange(access, grant));
            notice.clear();
            Go(Page::Checking);
            check = CheckJob::Start(path);
        }

        // The check's result, into `result` and `source`, once it has one.
        bool CollectCheck()
        {
            if (!check)
                return false;
            {
                std::lock_guard lock(check->mutex);
                if (!check->done)
                    return false;
                result = std::move(check->result);
                source = std::move(check->source);
            }
            check.reset();
            return true;
        }

        // Stops waiting for the check. One still reading is left to return on
        // its own, with the grant to read the file (CheckJob); call before
        // anything else gives `access` back.
        void LeaveCheck()
        {
            if (!check)
                return;
            {
                std::lock_guard lock(check->mutex);
                if (!check->done)
                {
                    check->abandoned = true;
                    check->access = std::exchange(access, 0);
                    fprintf(stderr, "[installer] left the check of %s still reading\n", sourcePath.c_str());
                }
            }
            check.reset();
        }

        void StartInstall()
        {
            cancel = false;
            {
                std::lock_guard lock(progressMutex);
                progress = {};
                currentFile.clear();
                copyStart = {};
            }
            Go(Page::Installing);
            task = std::async(std::launch::async, [this] {
                return install::Install(*source, destination, [this](const install::Progress& p) {
                    std::lock_guard lock(progressMutex);
                    if (p.phase == install::Progress::Phase::Copying && copyStart.time_since_epoch().count() == 0)
                    {
                        copyStart = std::chrono::steady_clock::now();
                        copyStartBytes = p.bytesDone;
                    }
                    progress = p;
                    currentFile.assign(p.currentFile);
                    progress.currentFile = {};
                }, &cancel);
            });
        }

        bool TaskReady()
        {
            return task.valid() && task.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        }

        void Stop()
        {
            cancel = true;
            scanCancel = true;
            // A check normally returns in moments, and is waited for, so it
            // doesn't still run while the app exits; one stuck in a read is
            // left behind.
            if (check)
            {
                std::unique_lock lock(check->mutex);
                check->finished.wait_for(lock, std::chrono::seconds(2), [this] { return check->done; });
            }
            LeaveCheck();
            if (task.valid())
                task.wait();
            if (scan.valid())
                scan.wait();
        }
    };

    InstallerScreen::InstallerScreen() : state(std::make_unique<State>())
    {
        state->StartScan();
        // Installed in a folder FindGameInstall passed over: a drive that
        // isn't connected, a folder macOS won't let the game read, or an
        // install that is incomplete or older. Said at the top of the page,
        // not left for the player to guess from the installer appearing.
        install::RecordedInstall recorded = install::CheckRecordedInstall();
#if defined(__APPLE__) && !TARGET_OS_IOS
        constexpr bool macOS = true;
#else
        constexpr bool macOS = false;
#endif
        state->notice = install::DescribeRecordedInstall(recorded, HomeRelative(recorded.path), macOS);
        if (!state->notice.empty())
            fprintf(stderr, "[installer] %s\n", state->notice.c_str());
    }

    InstallerScreen::~InstallerScreen()
    {
        state->Stop();
        // Nothing reads the picked file now; nor will anything collect a
        // pick still waiting.
        ReleaseAccess(std::exchange(state->access, 0));
        DropDialog();
    }

    void InstallerScreen::Draw()
    {
        using Page = State::Page;
        State& st = *state;
        float s = Scale();

        // Collect finished work.
        if (st.scan.valid() && st.scan.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            st.images = st.scan.get();
            st.scanned = true;
            // Onto the first image, unless the player is typing a path.
            if (st.page == Page::Choose && !ImGui::IsAnyItemActive())
                st.focusPage = true;
        }
        if (st.page == Page::Checking && st.CollectCheck())
        {
            if (st.result.Ok())
                st.LoadDestinations();
            st.Go(st.result.Ok() ? Page::Confirm : Page::Failed);
        }
        if (st.page == Page::Installing && st.TaskReady())
        {
            st.result = st.task.get();
            if (st.result.Ok())
            {
                installedPath = st.destination;
                // So the next launch finds it (internal storage clears the record).
                if (!install::RecordInstallPath(st.destination))
                    fprintf(stderr, "[installer] couldn't record the install location %s\n", st.destination.c_str());
                st.Go(Page::Done);
            }
            else if (st.result.error == install::Error::Cancelled)
            {
                st.notice = "Install cancelled. Nothing was left behind.";
                st.Go(Page::Choose);
            }
            else
                st.Go(Page::Failed);
        }
        {
            std::optional<std::string> pick, folder;
            uint64_t access = 0;
            {
                std::lock_guard lock(s_dialogMutex);
                if (s_dialogFor == DialogFor::Destination)
                {
                    // "Choose a folder..." on the Confirm page (Go drops the
                    // dialog when the player leaves it).
                    if (st.page == Page::Confirm)
                    {
                        folder = std::move(s_dialogPick);
                        s_dialogPick.reset();
                        if (s_dialogFailed)
                        {
                            // SDL's word when it found no portal and no zenity
                            // (SDL_unixdialog.c); any other failure is logged.
                            st.pick = State::Pick::Unavailable;
                            st.pickShown = false;
                            st.pickHint = FolderPickerHint(false, st.sourcePath,
                                s_dialogError.starts_with("File dialog driver unsupported"));
                        }
                        else if (s_dialogCancelled && st.pick == State::Pick::Waiting)
                        {
                            st.pick = State::Pick::Idle;
                            st.pickShown = false;
                        }
                    }
                    s_dialogFailed = s_dialogCancelled = false;
                    s_dialogError.clear();
                }
                else
                {
                    if (st.page == Page::Choose)
                    {
                        pick = std::move(s_dialogPick);
                        s_dialogPick.reset();
                        access = std::exchange(s_pickAccess, 0);
                    }
                    if (s_dialogFailed)
                    {
                        s_dialogFailed = false;
                        st.notice = kNoDialog;
                    }
                    s_dialogCancelled = false;
                    if (!s_dialogNotice.empty())
                    {
                        st.notice = std::move(s_dialogNotice);
                        s_dialogNotice.clear();
                    }
                }
            }
            if (pick)
                st.StartCheck(*pick, access);
            if (folder)
                st.PickFolder(*folder);
        }
#if defined(__APPLE__) && !TARGET_OS_IOS
        // NFSMW_TEST_DIALOG_CANCEL=<seconds> (automated runs): the system's
        // folder picker, open that long, is cancelled as its Cancel button
        // would (its answer then comes as the player's would).
        if (st.page == Page::Confirm && st.pick == State::Pick::Waiting && st.pickShown)
        {
            static const double cancelAfter = [] {
                const char* v = std::getenv("NFSMW_TEST_DIALOG_CANCEL");
                return v ? std::atof(v) : 0.0;
            }();
            double open = std::chrono::duration<double>(std::chrono::steady_clock::now() - st.pickStart).count();
            if (cancelAfter > 0 && open >= cancelAfter)
            {
                int n = platform::file_dialog::CancelOpenPanels();
                fprintf(stderr, "[installer] NFSMW_TEST_DIALOG_CANCEL: cancelled %d folder picker(s) open for %.1f s\n", n, open);
                st.pickShown = false;
            }
        }
#endif

        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImVec2 size(std::min(viewport->WorkSize.x - 2 * kSpaceL * s, 980.0f * s), std::min(viewport->WorkSize.y - 2 * kSpaceL * s, 640.0f * s));
        ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(size, ImGuiCond_Always);
        if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow))
            ImGui::SetNextWindowFocus();  // a click outside the panel mustn't strand the controller
        constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
        if (!ImGui::Begin("Install", nullptr, flags))
        {
            ImGui::End();
            return;
        }
        TitleText("SpeedBreaker");
        ImGui::TextDisabled("First-time setup for Need for Speed: Most Wanted (Xbox 360)");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        auto mainButton = [&](const char* label) {
            bool pressed = ImGui::Button(label, ImVec2(0, 0));
            if (st.focusPage)
            {
                ImGui::SetItemDefaultFocus();
                ImGui::SetKeyboardFocusHere(-1);
                st.focusPage = false;
            }
            return pressed;
        };

        switch (st.page)
        {
        case Page::Choose:
        {
            const bool inert = PickerShown();  // nothing acts while a Files picker is up
            ImGui::TextWrapped("The game is installed from your own copy: an Xbox 360 disc image (.iso) or the files "
                               "extracted from one. No game data comes with this port.");
            if (!st.notice.empty())
            {
                PushAccentText();
                ImGui::TextWrapped("%s", st.notice.c_str());
                PopAccentText();
            }
            ImGui::Spacing();
            // Under the list, to the window's bottom edge: two lines for why
            // the focused image won't install, a gap, the path to type, a
            // gap, the buttons.
            const float line = ImGui::GetTextLineHeightWithSpacing();
            const float detailHeight = line * 2 + kSpaceS * s;
#if defined(__APPLE__) && TARGET_OS_IOS
            const float pathRow = 0;  // no path to type on iOS (no keyboard, nor paths to know)
#else
            const float pathRow = line + ImGui::GetFrameHeightWithSpacing();
#endif
            const float below = ImGui::GetStyle().ItemSpacing.y + detailHeight + pathRow + kSpaceS * s + ImGui::GetFrameHeight();
            std::string detail;  // why the focused image can't be installed
            if (ImGui::BeginChild("images", ImVec2(0, -below), ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened))
            {
                if (!st.scanned)
                    ImGui::TextDisabled("%s", kScanning);
                else if (st.images.empty())
                {
                    ImGui::TextDisabled("No disc images found.");
                    ImGui::TextWrapped("%s", kWhereToPut);
                }
                bool focused = false;
                for (size_t i = 0; i < st.images.size(); i++)
                {
                    const auto& c = st.images[i];
                    ImGui::PushID(int(i));
                    bool ok = c.status.Ok();
                    float rowHeight = ImGui::GetTextLineHeightWithSpacing() * 2 + 6 * s;
                    // Rows that can't be installed stay focusable, so a
                    // controller can reach the reason (shown under the list).
                    bool pick = ImGui::Selectable("##image", false, ImGuiSelectableFlags_None, ImVec2(0, rowHeight));
                    if (!ok && (ImGui::IsItemFocused() || ImGui::IsItemHovered()))
                        detail = c.status.message;
                    if (ok && st.focusPage && !focused)
                    {
                        // Straight onto the row: SetKeyboardFocusHere() from inside
                        // this (nav-flattened) child left the cursor where it was,
                        // on Browse when the scan ended after the first frame.
                        ImGui::SetItemDefaultFocus();
                        ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
                        ImGui::ScrollToItem(ImGuiScrollFlags_KeepVisibleEdgeY);
                        focused = true;
                        st.focusPage = false;
                    }
                    // The name over its folder, the two lines centred in the
                    // row's highlight (its item rect) and kSpaceL inside it.
                    ImDrawList* draw = ImGui::GetWindowDrawList();
                    ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
                    float left = min.x + kSpaceL * s, right = max.x - kSpaceL * s;
                    float lineGap = kSpaceXS * s;
                    float top = std::round((min.y + max.y - (ImGui::GetFontSize() * 2 + lineGap)) * 0.5f);
                    ImU32 text = ImGui::GetColorU32(ok ? ImGuiCol_Text : ImGuiCol_TextDisabled);
                    ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
                    draw->AddText(ImVec2(left, top), text, FileName(c.path).c_str());
                    draw->AddText(ImVec2(left, top + ImGui::GetFontSize() + lineGap), dim,
                        DisplayPath(c.path.parent_path(), right - left).c_str());
                    std::string status = std::format("{}   {}", install::FormatSize(c.size), StatusText(c));
                    float w = ImGui::CalcTextSize(status.c_str()).x;
                    draw->AddText(ImVec2(right - w, top), ok ? ImGui::GetColorU32(ImGuiCol_PlotLines) : dim, status.c_str());
                    if (pick && ok && !inert)
                        st.StartCheck(c.path);
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
            {
                // Two lines under the list: why the focused image won't install.
                float top = ImGui::GetCursorPosY();
                if (!detail.empty())
                {
                    PushAccentText();
                    ImGui::TextWrapped("%s", detail.c_str());
                    PopAccentText();
                }
                ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), top + detailHeight));
            }

#if !(defined(__APPLE__) && TARGET_OS_IOS)
            ImGui::TextDisabled("Or type the path of an image or folder:");
            ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Use").x - ImGui::GetStyle().FramePadding.x * 2 - ImGui::GetStyle().ItemSpacing.x);
            bool enter = ImGui::InputText("##path", st.typedPath, sizeof(st.typedPath), ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            ImGui::BeginDisabled(st.typedPath[0] == 0);
            if (ImGui::Button("Use") || (enter && st.typedPath[0]))
            {
                std::string path = st.typedPath;
                if (path.starts_with("~/"))
                    if (const char* home = std::getenv("HOME"))
                        path = std::string(home) + path.substr(1);
                st.StartCheck(path);
            }
            ImGui::EndDisabled();
#endif

            ButtonRowAtBottom();
            // The first installable image has the cursor; otherwise Browse.
            // (Never disabled: a dialog the compositor doesn't show would
            // otherwise lock them for good. A new Browse replaces it.)
            if (mainButton("Browse for image...") && !inert)
                Browse(false);
            ImGui::SameLine();
            if (ImGui::Button("Browse for folder...") && !inert)
                Browse(true);
            ImGui::SameLine();
            ImGui::BeginDisabled(!st.scanned);
            if (ImGui::Button("Rescan") && !inert)
            {
                st.scanned = false;
                st.images.clear();
                st.StartScan();
            }
            ImGui::EndDisabled();
            // No Quit on iOS: apps there don't quit themselves (the player
            // leaves with the home gesture).
#if !(defined(__APPLE__) && TARGET_OS_IOS)
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize("Quit").x - ImGui::GetStyle().FramePadding.x * 2);
            if (ImGui::Button("Quit"))
                quit = true;
#endif
            break;
        }

        case Page::Checking:
            ImGui::Text("Checking %s...", FileName(st.sourcePath).c_str());
            ImGui::TextDisabled("Reading the game's version and file list.");
            ImGui::ProgressBar(-float(ImGui::GetTime()), ImVec2(-1, 0), "");
            ButtonRowAtBottom();
            // A read that waits (a drive that stopped answering, a file still
            // coming from a cloud service) mustn't hold the player here: Back
            // leaves it to return on its own. Not focused, as on Installing:
            // A pressed twice on an image mustn't come straight back.
            st.focusPage = false;
            if (ImGui::Button("Back"))
            {
                st.LeaveCheck();
                st.Go(Page::Choose);
            }
            break;

        case Page::Confirm:
        {
            PushAccentText();
            ImGui::TextUnformatted("Ready to install");
            PopAccentText();
            ImGui::Spacing();
            const auto& manifest = install::GameManifest();
            ImGui::TextDisabled("From");
            ImGui::TextWrapped("%s", HomeRelative(st.sourcePath).c_str());
            (void)manifest;
            ImGui::Spacing();
            ImGui::TextDisabled("To");
            const State::Destination& dest = st.destinations[st.chosen];
            auto describe = [](const State::Destination& d) {
                std::string text = d.where.label;
                if (d.info.available != UINT64_MAX)
                    text += "   " + install::FormatSize(d.info.available) + " free";
                return text;
            };
            if (st.destinations.size() > 1)
            {
                // Internal storage, a removable drive or a folder of the
                // player's: left/right (or the arrows).
                int delta = 0;
                std::string label = describe(dest);
                bool focusRow = st.focusPage && !dest.check.Ok();
                if (focusRow)
                    ImGui::SetKeyboardFocusHere();
                // A (or Return, or a click) on "Choose a folder..." opens the
                // folder picker; moving onto it doesn't (a controller passes
                // through it on the way round).
                bool activated = ImGui::Selectable(("##dest"), false, ImGuiSelectableFlags_AllowOverlap, ImVec2(0, ImGui::GetFrameHeight()));
                if (activated && dest.custom)
                    st.RequestFolder();
                if (focusRow)
                    st.focusPage = false;
                bool focused = ImGui::IsItemFocused();
                if (focused)
                {
                    if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
                        delta = -1;
                    if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickRight) || ImGui::IsKeyPressed(ImGuiKey_RightArrow))
                        delta = 1;
                }
                // The arrows and the label centred in the row's highlight (its
                // item rect, half an item spacing bigger than the row all
                // round), the left arrow's point kSpaceL inside it, as in a
                // settings row. (At the rect's corner they sat half a
                // spacing high and 11 points in.)
                ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
                float arrow = ImGui::GetFrameHeight(), font = ImGui::GetFontSize();
                float inset = std::max(0.0f, kSpaceL * s - ((arrow - font) * 0.5f + font * 0.2f));
                ImGui::SetCursorScreenPos(ImVec2(min.x + inset, std::round((min.y + max.y - arrow) * 0.5f)));
                ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
                if (ImGui::ArrowButton("##prev", ImGuiDir_Left))
                    delta = -1;
                ImGui::SameLine();
                if (focused)
                    PushAccentText();
                ImGui::TextUnformatted(label.c_str());
                if (focused)
                    PopAccentText();
                ImGui::SameLine();
                if (ImGui::ArrowButton("##next", ImGuiDir_Right))
                    delta = 1;
                ImGui::PopItemFlag();
                if (delta)
                {
                    st.chosen = (st.chosen + st.destinations.size() + size_t(delta)) % st.destinations.size();
                    st.destination = st.destinations[st.chosen].where.path;
                }
            }
            else
                ImGui::TextUnformatted(describe(dest).c_str());
            const State::Destination& d = st.destinations[st.chosen];
            // "Choose a folder..." before a folder is picked (or after one
            // was, the picker open again): its own lines, not an error.
            const bool unpicked = d.custom && !st.customFolder;
            // The exact folder the game goes to, wrapped (not cut off) when long.
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            if (unpicked)
                ImGui::TextWrapped("Any folder: the game goes in it, or in a new folder inside it if it holds other files.");
            else
                ImGui::TextWrapped("%s", HomeRelative(d.where.path).c_str());
            ImGui::PopStyleColor();
            ImGui::Spacing();
            if (d.custom && st.pick == State::Pick::Waiting)
                ImGui::TextDisabled("Waiting for the folder picker...");
            else if (d.custom && st.pick == State::Pick::Unavailable)
            {
                PushAccentText();
                ImGui::TextWrapped("%s", st.pickHint.c_str());
                PopAccentText();
            }
            else if (unpicked)
            {
                // Nothing chosen yet: the line above says what happens.
            }
            else if (!d.check.Ok())
            {
                PushAccentText();
                ImGui::TextWrapped("%s", d.check.message.c_str());
                PopAccentText();
            }
            else
            {
                if (d.custom)
                {
                    if (std::string why = install::SubfolderExplanation(d.choice); !why.empty())
                        ImGui::TextWrapped("%s", why.c_str());
                    for (const std::string& note : d.choice.notes)
                        ImGui::TextWrapped("%s", note.c_str());
                }
                ImGui::TextDisabled("%s needed", install::FormatSize(d.info.required).c_str());
                if (d.info.replacing)
                    ImGui::TextWrapped("Replaces the game already there. DLC and other files you added to it are kept.");
                ImGui::TextWrapped("This takes a few minutes. Every file is checked against the known-good disc as it is copied.");
            }
            ButtonRowAtBottom();
            // The cursor: Install when it can, else "Choose a folder..." on
            // that entry, else Back (nowhere to install).
            ImGui::BeginDisabled(!d.check.Ok());
            if (d.check.Ok() ? mainButton("Install") : ImGui::Button("Install"))
                st.StartInstall();
            ImGui::EndDisabled();
            if (d.custom)
            {
                ImGui::SameLine();
                const char* label = st.customFolder ? "Choose another folder..." : "Choose a folder...";
                if (d.check.Ok() ? ImGui::Button(label) : mainButton(label))
                    st.RequestFolder();
            }
            ImGui::SameLine();
            if (d.check.Ok() || d.custom ? ImGui::Button("Back") : mainButton("Back"))
                st.Go(Page::Choose);
            break;
        }

        case Page::Installing:
        {
            install::Progress p;
            std::string file;
            std::chrono::steady_clock::time_point copyStart;
            uint64_t copyStartBytes;
            {
                std::lock_guard lock(st.progressMutex);
                p = st.progress;
                file = st.currentFile;
                copyStart = st.copyStart;
                copyStartBytes = st.copyStartBytes;
            }
            const char* phase = p.phase == install::Progress::Phase::Finalizing ? "Finishing..." : "Installing...";
            PushAccentText();
            ImGui::TextUnformatted(phase);
            PopAccentText();
            ImGui::Spacing();
            float fraction = p.bytesTotal ? float(double(p.bytesDone) / double(p.bytesTotal)) : 0.0f;
            ImGui::ProgressBar(fraction, ImVec2(-1, 28 * s));
            std::string line = std::format("{} of {}", install::FormatSize(p.bytesDone), install::FormatSize(p.bytesTotal));
            if (copyStart.time_since_epoch().count() != 0)
            {
                double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - copyStart).count();
                double rate = seconds > 1.0 ? double(p.bytesDone - copyStartBytes) / seconds : 0.0;
                if (rate > 0)
                {
                    line += std::format("   {}/s", install::FormatSize(uint64_t(rate)));
                    if (p.bytesTotal > p.bytesDone)
                        line += "   about " + Duration(double(p.bytesTotal - p.bytesDone) / rate) + " left";
                }
            }
            ImGui::TextUnformatted(line.c_str());
            ImGui::TextDisabled("%s (%zu of %zu files)", file.c_str(), std::min(p.filesDone + 1, p.filesTotal), p.filesTotal);
#if defined(__APPLE__) && TARGET_OS_IOS
            ImGui::Spacing();
            ImGui::TextDisabled("Keep SpeedBreaker open until it's done: iOS pauses apps in the background.");
#endif
            ButtonRowAtBottom();
            // Not focused by default: a stray A press mustn't throw away minutes of copying.
            st.focusPage = false;
            ImGui::BeginDisabled(st.cancel.load());
            if (ImGui::Button(st.cancel ? "Cancelling..." : "Cancel"))
                st.cancel = true;
            // But any direction lands on it. (ImGui's own navigation starts
            // from where the Confirm page's Install was, so only Right
            // reached it: a controller pressing down, the natural way to a
            // button at the bottom, couldn't cancel at all.)
            if (!st.cancel && !ImGui::IsItemFocused())
            {
                static constexpr ImGuiKey kDirections[] = { ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadDpadDown,
                    ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown,
                    ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, ImGuiKey_UpArrow, ImGuiKey_DownArrow,
                    ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_Tab };
                if (std::any_of(std::begin(kDirections), std::end(kDirections), [](ImGuiKey key) { return ImGui::IsKeyPressed(key, false); }))
                {
                    ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
                    ImGui::SetNavCursorVisible(true);
                }
            }
            ImGui::EndDisabled();
            break;
        }

        case Page::Done:
            PushAccentText();
            ImGui::TextUnformatted("Installed");
            PopAccentText();
            ImGui::Spacing();
            ImGui::TextWrapped("Every file matched the known-good disc. The game is in %s.", HomeRelative(st.destination).c_str());
            ImGui::TextWrapped("You can delete the disc image now if you like; the game doesn't need it any more.");
            ImGui::Spacing();
#if defined(__APPLE__) && TARGET_OS_IOS
            ImGui::TextDisabled("In game, press View + Menu on a controller, or tap with three fingers, to open the settings.");
#elif defined(__APPLE__)
            ImGui::TextDisabled("In game, F1 or Cmd+, (or Back + Start on a controller) opens the settings.");
#else
            ImGui::TextDisabled("In game, F1 (or Back + Start on a controller) opens the settings.");
#endif
            ButtonRowAtBottom();
            if (mainButton("Start the game"))
                finished = true;
            break;

        case Page::Failed:
            PushAccentText();
            ImGui::TextUnformatted(FailureTitle(st.result.error));
            PopAccentText();
            ImGui::Spacing();
            ImGui::TextWrapped("%s", st.result.message.c_str());
            if (!st.result.file.empty())
                ImGui::TextDisabled("File: %s", st.result.file.c_str());
            if (!st.result.foundHash.empty())
                ImGui::TextDisabled("SHA-256: %s", st.result.foundHash.c_str());
            ButtonRowAtBottom();
            if (mainButton("Back"))
                st.Go(Page::Choose);
            if (st.source)
            {
                // Back to the Confirm page: maybe another drive this time.
                ImGui::SameLine();
                if (ImGui::Button("Try again"))
                {
                    st.LoadDestinations();
                    st.Go(Page::Confirm);
                }
            }
            break;
        }
        ImGui::End();
    }

    std::optional<std::filesystem::path> RunInstaller(const std::function<bool()>& runFrame)
    {
        InstallerScreen screen;
        SetModal([&screen] { screen.Draw(); });
        bool open = true;
        while (!screen.finished && !screen.quit)
            if (!runFrame())
            {
                open = false;
                break;
            }
        SetModal(nullptr);
        if (!open || screen.quit)
            return std::nullopt;
        return screen.installedPath;
    }
}
