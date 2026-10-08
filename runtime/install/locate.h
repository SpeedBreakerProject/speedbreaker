// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Finding things before any UI question is asked: an installed game (so
// startup can skip the installer), and disc images already on the machine
// (so a controller-only first run can offer "Install from <image>" instead of
// a file browser).
#pragma once
#include "manifest.h"
#include "result.h"

#include <atomic>
#include <filesystem>
#include <optional>
#include <vector>

namespace install
{
    enum class InstallOrigin
    {
        Environment,    // NFSMW_GAME_DIR
        Development,    // ./game/files, extracted by hand in a checkout
        User,           // GetUserPath()/game, made by Install
    };

    struct GameInstall
    {
        std::filesystem::path path;     // absolute; the directory holding default.xex
        // default.xex itself, as spelled on disk: a hand-extracted folder on
        // a case-sensitive file system may hold DEFAULT.XEX, which
        // path / "default.xex" would not open. Load the executable from here.
        std::filesystem::path xex;
        InstallOrigin origin = InstallOrigin::User;
        bool marker = false;            // holds Install's marker for the current manifest
    };

    // The first usable game directory of:
    //   1. NFSMW_GAME_DIR, if set (skipped with a log line if it isn't usable)
    //   2. ./game/files (a development checkout)
    //   3. GetUserPath()/game (DefaultInstallPath(), where Install puts it)
    // Each needs default.xex with the manifest's size. The third also needs
    // Install's marker for the current manifest version: it is the one the
    // installer owns, and an old marker means the manifest changed since, so
    // reinstall. The first two were set up by hand and are trusted without a
    // marker. nullopt: nothing usable, so run the installer.
    // Before looking at the third, finishes a reinstall a crash interrupted
    // (RecoverInterruptedInstall).
    // Cheap: a few stat calls and one small read; no hashing.
    std::optional<GameInstall> FindGameInstall(const Manifest& manifest = GameManifest());

    // The same check for one directory.
    bool IsUsableInstall(const std::filesystem::path& dir, bool requireMarker, const Manifest& manifest = GameManifest());

    // Where the player chose to install when it wasn't DefaultInstallPath()
    // (an SD card, a USB drive): recorded in GetUserPath()/game-location by
    // the installer, and searched by FindGameInstall after NFSMW_GAME_DIR and
    // ./game/files. Recording the default path removes the record.
    std::optional<std::filesystem::path> RecordedInstallPath();
    bool RecordInstallPath(const std::filesystem::path& dir);

    struct InstallDestination
    {
        std::string label;              // "Internal storage", or the drive's name
        std::filesystem::path path;     // the game folder to install into
        bool removable = false;
    };

    // Where the installer offers to put the game: DefaultInstallPath() first,
    // then a folder on each mounted removable drive (<drive>/speedbreaker/game; SpeedBreaker on macOS).
    std::vector<InstallDestination> InstallDestinations();

    struct DiscImageCandidate
    {
        std::filesystem::path path;
        uint64_t size = 0;
        bool xexMatches = false;    // default.xex is the supported one (Install still checks every file)
        uint32_t titleId = 0;       // default.xex's title ID (0 if unreadable)
        Result status;              // Ok, or why it won't install: UnsupportedVersion, WrongGame, TruncatedImage, ReadError
    };

    // Where FindDiscImages looks by default: the home folder, Downloads,
    // Desktop, Documents and Games in it, removable drives and SD cards
    // (/run/media/*, /run/media/$USER/*, /media/*, /media/$USER/* on Linux;
    // /Volumes/* except the startup disk on macOS). Only existing folders.
    std::vector<std::filesystem::path> DiscImageSearchFolders();

    // .iso files of at least `minSize` bytes directly in each folder or one
    // folder below it, that hold an XDVDFS game partition with a default.xex.
    // Each costs a directory read and hashing default.xex (a few MB). Sorted:
    // installable images first, then by path. Blocks; run it off the UI thread.
    // On macOS, reading Desktop, Documents, Downloads or a removable volume
    // makes the system ask the user for permission the first time.
    std::vector<DiscImageCandidate> FindDiscImages(const std::vector<std::filesystem::path>& folders = DiscImageSearchFolders(),
        uint64_t minSize = 1ull << 30, const std::atomic<bool>* cancel = nullptr, const Manifest& manifest = GameManifest());
}
