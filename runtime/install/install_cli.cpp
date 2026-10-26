// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See install_cli.h.
#include "install_cli.h"
#include "installer.h"
#include "locate.h"
#include "placement.h"
#include "portable_switch.h"

#include <user/paths.h>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <format>
#include <string>
#include <vector>

#ifdef _WIN32
#include <platform/os.h>
#else
#include <unistd.h>
#endif

namespace install
{
    namespace
    {
        namespace fs = std::filesystem;
        using Clock = std::chrono::steady_clock;

        std::atomic<bool> s_cancel{ false };

        void OnInterrupt(int)
        {
            s_cancel.store(true, std::memory_order_relaxed);
        }

        // Ctrl-C (or a SIGTERM) asks the install to stop between chunks, so it
        // deletes its partial copy instead of leaving gigabytes behind. The
        // handler resets itself: a second Ctrl-C kills at once.
        class InterruptGuard
        {
        public:
            InterruptGuard()
            {
                s_cancel.store(false);
#ifdef _WIN32
                // The C runtime's SIGINT is Ctrl-C (and Ctrl-Break), through
                // a console control handler, and it resets itself as
                // SA_RESETHAND does.
                previousInt = signal(SIGINT, OnInterrupt);
                previousTerm = signal(SIGTERM, OnInterrupt);
#else
                struct sigaction sa{};
                sa.sa_handler = OnInterrupt;
                sa.sa_flags = SA_RESETHAND;
                sigemptyset(&sa.sa_mask);
                sigaction(SIGINT, &sa, &previousInt);
                sigaction(SIGTERM, &sa, &previousTerm);
#endif
            }

            ~InterruptGuard()
            {
#ifdef _WIN32
                signal(SIGINT, previousInt);
                signal(SIGTERM, previousTerm);
#else
                sigaction(SIGINT, &previousInt, nullptr);
                sigaction(SIGTERM, &previousTerm, nullptr);
#endif
            }

        private:
#ifdef _WIN32
            void (*previousInt)(int) = SIG_DFL;
            void (*previousTerm)(int) = SIG_DFL;
#else
            struct sigaction previousInt{}, previousTerm{};
#endif
        };

        // One rewritten status line on a terminal; a line per file otherwise
        // (a log file or the Steam launcher's output).
        class ProgressPrinter
        {
        public:
            void operator()(const Progress& p)
            {
                Clock::time_point now = Clock::now();
                bool newFile = p.currentFile != lastFile;
                if (newFile)
                    lastFile = std::string(p.currentFile);
                if (p.phase == Progress::Phase::Checking || p.phase == Progress::Phase::Finalizing)
                    return;
                int percent = p.bytesTotal ? int(p.bytesDone * 100 / p.bytesTotal) : 0;
                if (!tty)
                {
                    if (newFile && !lastFile.empty())
                        printf("  %3d%%  %s\n", percent, lastFile.c_str());
                    return;
                }
                if (!newFile && now - lastPrint < std::chrono::milliseconds(200))
                    return;
                lastPrint = now;
                double seconds = std::chrono::duration<double>(now - start).count();
                double rate = seconds > 0.5 ? double(p.bytesDone) / seconds : 0.0;
                printf("\r  %3d%%  %s / %s  %s/s  %s\x1b[K", percent, FormatSize(p.bytesDone).c_str(),
                    FormatSize(p.bytesTotal).c_str(), FormatSize(uint64_t(rate)).c_str(), lastFile.c_str());
                fflush(stdout);
                printed = true;
            }

            void Finish()
            {
                if (printed)
                    printf("\n");
                fflush(stdout);
            }

            double Seconds() const
            {
                return std::chrono::duration<double>(Clock::now() - start).count();
            }

        private:
            bool tty = isatty(STDOUT_FILENO);
            bool printed = false;
            std::string lastFile;
            Clock::time_point start = Clock::now(), lastPrint{};
        };

        int Failed(const Result& r)
        {
            fprintf(stderr, "error (%s): %s\n", ErrorName(r.error), r.message.c_str());
            return r.error == Error::Cancelled ? 130 : 1;
        }

        int Usage(const char* message)
        {
            fprintf(stderr, "%s\n"
                "usage: SpeedBreaker --install <image|folder> [--dest <dir>]\n"
                "       SpeedBreaker --verify <image|folder>\n"
                "       SpeedBreaker --find-images [folder...]\n"
                "       SpeedBreaker --where\n"
                "       SpeedBreaker --portable on|off\n", message);
            return 2;
        }

        Result OpenAndCheck(const char* path, std::unique_ptr<DiscSource>& source)
        {
            Result r = OpenDiscSource(path, source);
            if (!r.Ok())
                return r;
            printf("Source: %s (%s, %zu files)\n", source->Path().string().c_str(),
                source->Kind() == SourceKind::Image ? "disc image" : "folder", source->Files().size());
            return {};
        }

        // `named`: the player gave the folder (--dest). Without --dest it's
        // the default folder, which gets what the installer screen's
        // "Internal storage" entry gets: only Install's own checks.
        int RunInstall(const char* sourcePath, const fs::path& destination, bool named)
        {
            // --dest: into exactly that folder (no subfolder, unlike the
            // installer screen's "Choose a folder..."), with the screen's
            // refusals for a folder the player picks: not SpeedBreaker's own
            // folder, inside an install, the home folder, iCloud Drive, a
            // folder kept in memory... (install/placement.h). Then the folder
            // as the checks saw it (absolute, links resolved, no trailing
            // separator) is installed into and recorded as such.
            fs::path folder = destination;
            std::vector<std::string> notes;
            if (named)
            {
                FolderChoice where = CheckExactFolder(destination, true);
                if (!where.result.Ok())
                    return Failed(where.result);
                folder = where.gameDir;
                notes = std::move(where.notes);
            }
            std::unique_ptr<DiscSource> source;
            if (Result r = OpenAndCheck(sourcePath, source); !r.Ok())
                return Failed(r);
            const Manifest& manifest = SourceManifest(*source);
            // The folder holds another region's install, which this disc
            // would replace (install/placement.h's KeepOtherRegion): without
            // --dest the game goes beside it ("game PAL-ES" next to "game");
            // --dest named that folder, so the player is told where instead.
            if (RegionKeep keep = KeepOtherRegion(folder, manifest); keep.kept)
            {
                if (named)
                    return Failed(Result{ Error::DestinationNotEmpty,
                        std::format("{} holds the {} install, which this {} disc would replace. Install it beside it with "
                                    "--dest \"{}\" (or delete that folder first to replace it).",
                            folder.string(), keep.kept->regionName, manifest.regionName, keep.gameDir.string()),
                        folder.string() });
                notes.push_back(RegionKeepNote(keep, manifest, folder.string(), keep.gameDir.string()));
                folder = keep.gameDir;
            }
            for (const std::string& note : notes)
                printf("Note: %s\n", note.c_str());
            printf("Installing %s (%s) to %s\n", FormatSize(manifest.totalBytes).c_str(), std::string(manifest.regionName).c_str(),
                folder.string().c_str());

            InterruptGuard guard;
            ProgressPrinter printer;
            Result r = Install(*source, folder, std::ref(printer), &s_cancel);
            printer.Finish();
            if (!r.Ok())
                return Failed(r);
            printf("Installed and verified in %.0f s: %s\n", printer.Seconds(), folder.string().c_str());
            // So the game finds it there (the default location clears the record).
            if (!RecordInstallPath(folder))
                fprintf(stderr, "Couldn't record the install location; start the game with NFSMW_GAME_DIR=%s\n",
                    folder.string().c_str());
            return 0;
        }

        int RunVerify(const char* sourcePath)
        {
            std::unique_ptr<DiscSource> source;
            if (Result r = OpenAndCheck(sourcePath, source); !r.Ok())
                return Failed(r);

            InterruptGuard guard;
            ProgressPrinter printer;
            Result r = Verify(*source, std::ref(printer), &s_cancel);
            printer.Finish();
            if (!r.Ok())
                return Failed(r);
            const Manifest& manifest = SourceManifest(*source);
            printf("All %zu files match the original disc, %s (%.0f s).\n", manifest.files.size(),
                std::string(manifest.regionName).c_str(), printer.Seconds());
            return 0;
        }

        int RunFindImages(const std::vector<fs::path>& folders)
        {
            std::vector<fs::path> where = folders.empty() ? DiscImageSearchFolders() : folders;
            printf("Looking in %zu folders (and one level below them)...\n", where.size());
            InterruptGuard guard;
            std::vector<DiscImageCandidate> images = FindDiscImages(where, 1ull << 30, &s_cancel);
            for (const DiscImageCandidate& image : images)
            {
                printf("  %-6s %s  (%s)\n", image.status.Ok() ? "ready" : "no", image.path.string().c_str(),
                    FormatSize(image.size).c_str());
                if (!image.status.Ok())
                    printf("         %s: %s\n", ErrorName(image.status.error), image.status.message.c_str());
            }
            if (images.empty())
                printf("No Xbox 360 disc images found.\n");
            return images.empty() ? 1 : 0;
        }

        // Where settings, saves and caches are, and whether that's portable mode.
        void PrintDataFolders()
        {
            const portable::Layout& layout = portable::Current();
            if (layout.Active())
                printf("Portable mode (%s in %s): data in %s, caches in %s\n", portable::kMarker, layout.root.string().c_str(),
                    GetUserPath().string().c_str(), GetCachePath().string().c_str());
            else
                printf("Data in %s, caches in %s\n", GetUserPath().string().c_str(), GetCachePath().string().c_str());
        }

        int RunWhere()
        {
            std::optional<GameInstall> game = FindGameInstall();
            if (!game)
            {
                // The installer screen's notice: a recorded folder that isn't there now, say.
                RecordedInstall recorded = CheckRecordedInstall();
#if defined(__APPLE__) && !TARGET_OS_IOS
                std::string notice = DescribeRecordedInstall(recorded, recorded.path.string(), true);
#else
                std::string notice = DescribeRecordedInstall(recorded, recorded.path.string(), false);
#endif
                if (!notice.empty())
                    printf("%s\n", notice.c_str());
                printf("No game install found. Install one with --install <image|folder> [--dest <dir>].\n");
                PrintDataFolders();
                return 1;
            }
            const char* origin = game->origin == InstallOrigin::Environment ? "NFSMW_GAME_DIR"
                : game->origin == InstallOrigin::Development ? "development checkout" : "installed";
            printf("%s (%s%s; %s)\n", game->path.string().c_str(), origin, game->marker ? ", verified by the installer" : "",
                game->manifest ? std::string(game->manifest->regionName).c_str() : "?");
            PrintDataFolders();
            return 0;
        }

        // --portable on|off (install/portable_switch.h): the copy, with the
        // game's copy shown as --install shows it.
        int RunPortable(bool on)
        {
            if (!portable::kSupported)
            {
                fprintf(stderr, "Portable mode isn't available on macOS or iOS: SpeedBreaker keeps its data in the system's "
                                "folder for apps there.\n");
                return 1;
            }
            InterruptGuard guard;
            GameCopier copier = [](const fs::path& from, const fs::path& to) -> Result
            {
                std::unique_ptr<DiscSource> source;
                if (Result r = OpenDiscSource(from, source); !r.Ok())
                    return r;
                printf("Copying the game from %s (%s), checking every file:\n", from.string().c_str(),
                    FormatSize(SourceManifest(*source).totalBytes).c_str());
                ProgressPrinter printer;
                Result r = Install(*source, to, std::ref(printer), &s_cancel);
                printer.Finish();
                return r;
            };
            std::vector<std::string> said;
            Result r = on ? MakePortable(BundleFolder(), HomeUserPath(), copier, said)
                          : LeavePortable(portable::Current(), HomeUserPath(), copier, said);
            for (const std::string& line : said)
                printf("%s\n", line.c_str());
            if (!r.Ok())
                return Failed(r);
            return 0;
        }
    }

    std::optional<int> RunInstallCommand(int argc, char** argv)
    {
        for (int i = 1; i < argc; i++)
        {
            std::string_view arg = argv[i];
            if (arg == "--install")
            {
                if (i + 1 >= argc)
                    return Usage("--install needs the disc image or folder to install from.");
                const char* source = argv[i + 1];
                fs::path destination = DefaultInstallPath();
                bool named = false;
                for (int j = i + 2; j < argc; j++)
                {
                    if (std::string_view(argv[j]) == "--dest" && j + 1 < argc)
                    {
                        destination = argv[++j];
                        named = true;
                    }
                    else
                        return Usage((std::string("unexpected argument: ") + argv[j]).c_str());
                }
                return RunInstall(source, destination, named);
            }
            if (arg == "--verify")
            {
                if (i + 1 >= argc)
                    return Usage("--verify needs the disc image or folder to check.");
                return RunVerify(argv[i + 1]);
            }
            if (arg == "--find-images")
                return RunFindImages(std::vector<fs::path>(argv + i + 1, argv + argc));
            if (arg == "--where")
                return RunWhere();
            if (arg == "--portable")
            {
                std::string_view what = i + 1 < argc ? std::string_view(argv[i + 1]) : std::string_view();
                if ((what != "on" && what != "off") || i + 2 < argc)
                    return Usage("--portable takes on (copy this computer's SpeedBreaker data and game into SpeedBreaker's "
                                 "folder, and keep them there from now on) or off (copy them back).");
                return RunPortable(what == "on");
            }
        }
        return std::nullopt;
    }
}
