// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See install_cli.h.
#include "install_cli.h"
#include "installer.h"
#include "locate.h"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

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
                struct sigaction sa{};
                sa.sa_handler = OnInterrupt;
                sa.sa_flags = SA_RESETHAND;
                sigemptyset(&sa.sa_mask);
                sigaction(SIGINT, &sa, &previousInt);
                sigaction(SIGTERM, &sa, &previousTerm);
            }

            ~InterruptGuard()
            {
                sigaction(SIGINT, &previousInt, nullptr);
                sigaction(SIGTERM, &previousTerm, nullptr);
            }

        private:
            struct sigaction previousInt{}, previousTerm{};
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
                "       SpeedBreaker --where\n", message);
            return 2;
        }

        Result OpenAndCheck(const char* path, std::unique_ptr<DiscSource>& source)
        {
            Result r = OpenDiscSource(path, source);
            if (!r.Ok())
                return r;
            printf("Source: %s (%s, %zu files)\n", source->Path().c_str(),
                source->Kind() == SourceKind::Image ? "disc image" : "folder", source->Files().size());
            return {};
        }

        int RunInstall(const char* sourcePath, const fs::path& destination)
        {
            std::unique_ptr<DiscSource> source;
            if (Result r = OpenAndCheck(sourcePath, source); !r.Ok())
                return Failed(r);
            printf("Installing %s to %s\n", FormatSize(GameManifest().totalBytes).c_str(), destination.c_str());

            InterruptGuard guard;
            ProgressPrinter printer;
            Result r = Install(*source, destination, std::ref(printer), &s_cancel);
            printer.Finish();
            if (!r.Ok())
                return Failed(r);
            printf("Installed and verified in %.0f s: %s\n", printer.Seconds(), destination.c_str());
            // So the game finds it there (the default location clears the record).
            if (!RecordInstallPath(destination))
                fprintf(stderr, "Couldn't record the install location; start the game with NFSMW_GAME_DIR=%s\n", destination.c_str());
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
            printf("All %zu files match the original disc (%.0f s).\n", GameManifest().files.size(), printer.Seconds());
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
                printf("  %-6s %s  (%s)\n", image.status.Ok() ? "ready" : "no", image.path.c_str(),
                    FormatSize(image.size).c_str());
                if (!image.status.Ok())
                    printf("         %s: %s\n", ErrorName(image.status.error), image.status.message.c_str());
            }
            if (images.empty())
                printf("No Xbox 360 disc images found.\n");
            return images.empty() ? 1 : 0;
        }

        int RunWhere()
        {
            std::optional<GameInstall> game = FindGameInstall();
            if (!game)
            {
                printf("No game install found. Install one with --install <image|folder>.\n");
                return 1;
            }
            const char* origin = game->origin == InstallOrigin::Environment ? "NFSMW_GAME_DIR"
                : game->origin == InstallOrigin::Development ? "development checkout" : "installed";
            printf("%s (%s%s)\n", game->path.c_str(), origin, game->marker ? ", verified by the installer" : "");
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
                for (int j = i + 2; j < argc; j++)
                {
                    if (std::string_view(argv[j]) == "--dest" && j + 1 < argc)
                        destination = argv[++j];
                    else
                        return Usage((std::string("unexpected argument: ") + argv[j]).c_str());
                }
                return RunInstall(source, destination);
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
        }
        return std::nullopt;
    }
}
