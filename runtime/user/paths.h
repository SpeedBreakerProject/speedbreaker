// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
#pragma once
#include <cstdlib>
#include <filesystem>
#include <string>
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

// Extracted disc contents (default.xex, NFS/, Movies/): where startup found
// or installed the game (install::FindGameInstall, SetGamePath); before
// that, NFSMW_GAME_DIR or the repo layout.
inline std::filesystem::path& GamePathStorage()
{
    static std::filesystem::path path;  // set once, before the game starts
    return path;
}

inline void SetGamePath(std::filesystem::path path)
{
    GamePathStorage() = std::move(path);
}

inline std::filesystem::path GetGamePath()
{
    if (!GamePathStorage().empty())
        return GamePathStorage();
    if (const char* dir = std::getenv("NFSMW_GAME_DIR"))
        return dir;
    return "game/files";
}

// The per-user folders are named for the project: SpeedBreaker (macOS) or
// speedbreaker (Linux), nfsmw-recomp until 2026-10. MigrateUserFolders
// (user/paths.cpp, first thing at startup) renames an old folder; where that
// fails, the old one is used as it is.
#ifdef __APPLE__
inline constexpr const char* kUserFolderName = "SpeedBreaker";
#else
inline constexpr const char* kUserFolderName = "speedbreaker";
#endif
inline constexpr const char* kOldUserFolderName = "nfsmw-recomp";

// The folders' names in use (set before any other thread starts).
inline const char*& DataFolderName()
{
    static const char* name = kUserFolderName;
    return name;
}

inline const char*& CacheFolderName()
{
    static const char* name = kUserFolderName;
    return name;
}

// Where the per-user folders go.
//   macOS: ~/Library/Application Support, ~/Library/Caches
//   iOS: the same in the app's container; $XDG_DATA_HOME and
//        $XDG_CACHE_HOME when set (test runs: iOS sets HOME itself)
//   Linux: $XDG_DATA_HOME or ~/.local/share, $XDG_CACHE_HOME or ~/.cache
inline std::filesystem::path UserDataBase()
{
    const char* home = std::getenv("HOME");
    std::filesystem::path base = home ? home : ".";
#ifdef __APPLE__
#if TARGET_OS_IOS
    if (const char* xdg = std::getenv("XDG_DATA_HOME"))
        return xdg;
#endif
    return base / "Library/Application Support";
#else
    if (const char* xdg = std::getenv("XDG_DATA_HOME"))
        return xdg;
    return base / ".local/share";
#endif
}

inline std::filesystem::path CacheBase()
{
    const char* home = std::getenv("HOME");
    std::filesystem::path base = home ? home : ".";
#ifdef __APPLE__
#if TARGET_OS_IOS
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"))
        return xdg;
#endif
    return base / "Library/Caches";
#else
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"))
        return xdg;
    return base / ".cache";
#endif
}

// Per-user data: saves, settings, logs and (by default) the installed game.
inline std::filesystem::path GetUserPath()
{
    return UserDataBase() / DataFolderName();
}

// Caches that can be deleted any time (compiled shaders, pipelines).
inline std::filesystem::path GetCachePath()
{
    return CacheBase() / CacheFolderName();
}

// Renames nfsmw-recomp folders to the current name, leaving the old name as
// a link to the new folder (for older builds and scripts). Call first thing
// in main; returns what it did, for the log once that is open.
std::string MigrateUserFolders();

inline std::filesystem::path GetSavePath(bool)
{
    return GetUserPath() / "save";
}
