// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// A session's crash marker, logs/.crash-pending-<session> (crash.cpp): made
// empty at startup and kept open for the crash handler, which writes the
// crash report into it and renames it crash-<session>.txt. A session that
// ends the way it is meant to (the window's close button, Quit Game, Cmd+Q,
// a requested termination: platform/quit_signals.h) removes it, so a marker
// left in the logs folder is a session that hasn't ended or didn't end
// cleanly: empty, it was stopped from outside (SIGKILL, the power went, the
// system ended it) or crashed too hard to report; written, the handler's
// rename didn't happen. The next launch reports what it finds (crash.cpp's
// NoticePreviousCrash). Until v0.1.6 every session left its empty marker,
// even after Quit Game, until the next launch removed it, so a marker after
// the game had gone (a Steam Frame session its player closed from Steam,
// 2026-10-15) said nothing about how it ended.
//
// POSIX file calls only (Windows: the platform layer's, on wide paths, so a
// user folder of any name works; a session there that Task Manager ends, or
// that Windows ends without asking it, keeps its marker as SIGKILL's does).
// Header only (tests/clean_exit_test.cpp, POSIX only).
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <platform/file_io.h>
#include <platform/os.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace report::marker
{
    inline constexpr const char kPrefix[] = ".crash-pending-";

    inline std::filesystem::path PathFor(const std::filesystem::path& dir, const std::string& session)
    {
        return dir / (kPrefix + session);
    }

    // Startup: the session's empty marker, open for the crash handler (-1:
    // it couldn't be made).
#ifdef _WIN32
    inline int Arm(const wchar_t* path)
    {
        return platform::OpenFile(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    }
#else
    inline int Arm(const char* path)
    {
        return open(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    }
#endif

    // The session ended cleanly: the marker closed and removed.
#ifdef _WIN32
    inline void Clear(int fd, const wchar_t* path)
    {
        if (fd >= 0)
            _close(fd);
        platform::RemoveFile(path);
    }
#else
    inline void Clear(int fd, const char* path)
    {
        if (fd >= 0)
            close(fd);
        unlink(path);
    }
#endif

    // An earlier session's marker, as a launch finds it.
    enum class Ended
    {
        WithoutQuitting,  // empty: stopped from outside, or no report could be written
        Crashed,          // the handler's report (it couldn't rename it)
    };
    struct Leftover
    {
        std::filesystem::path path;
        std::string session;
        Ended ended;
    };

    // The markers in `dir` other than the `current` session's.
    inline std::vector<Leftover> Find(const std::filesystem::path& dir, const std::string& current)
    {
        std::vector<Leftover> found;
        std::error_code ec;
        for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        {
            const std::string name = it->path().filename().string();
            if (!name.starts_with(kPrefix) || name == kPrefix + current)
                continue;
            std::error_code sizeError;
            const uintmax_t size = std::filesystem::file_size(it->path(), sizeError);
            found.push_back({ it->path(), name.substr(sizeof(kPrefix) - 1),
                (size == 0 || sizeError) ? Ended::WithoutQuitting : Ended::Crashed });
        }
        return found;
    }
}
