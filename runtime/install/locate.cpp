// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See locate.h.
#include "locate.h"
#include "installer.h"
#include "placement.h"

#include <user/paths.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <set>

#include <pwd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#ifdef __APPLE__
#include <sys/mount.h>
#endif
#include <unistd.h>

namespace install
{
    namespace
    {
        namespace fs = std::filesystem;

        bool Hidden(const fs::path& path)
        {
            std::string name = path.filename().string();
            return !name.empty() && name[0] == '.';
        }

#ifndef __APPLE__
        std::string UserName()
        {
            if (const char* user = std::getenv("USER"); user && *user)
                return user;
            if (const passwd* pw = getpwuid(getuid()))
                return pw->pw_name;
            return {};
        }
#endif

        // default.xex in `dir` with the manifest's size (and, if asked, the
        // current marker beside it), as spelled on disk.
        std::optional<fs::path> UsableXex(const fs::path& dir, bool requireMarker, const Manifest& manifest)
        {
            std::error_code ec;
            fs::path xex = dir / "default.xex";
            if (!fs::is_regular_file(xex, ec))
            {
                // DEFAULT.XEX from a tool that upper-cases names, on a
                // case-sensitive file system.
                xex.clear();
                std::error_code listEc;
                for (fs::directory_iterator it(dir, listEc), end; !listEc && it != end; it.increment(listEc))
                {
                    std::error_code fileEc;
                    if (PathEqualsIgnoreCase(it->path().filename().string(), "default.xex") && it->is_regular_file(fileEc))
                    {
                        xex = it->path();
                        break;
                    }
                }
                if (xex.empty())
                    return std::nullopt;
            }
            uint64_t size = fs::file_size(xex, ec);
            if (ec || size != manifest.Xex().size)
                return std::nullopt;
            if (requireMarker && ReadInstallMarker(dir) != std::optional<std::string>(manifest.version))
                return std::nullopt;
            return xex;
        }

        GameInstall Found(const fs::path& dir, const fs::path& xex, InstallOrigin origin, const Manifest& manifest)
        {
            std::error_code ec;
            GameInstall install;
            fs::path absolute = fs::absolute(dir, ec);
            install.path = (ec ? dir : absolute).lexically_normal();
            install.xex = install.path / xex.filename();
            install.origin = origin;
            install.marker = ReadInstallMarker(dir) == std::optional<std::string>(manifest.version);
            return install;
        }

        void Consider(const fs::path& file, uint64_t minSize, const Manifest& manifest,
            std::set<fs::path>& seen, std::vector<DiscImageCandidate>& found)
        {
            std::string extension = file.extension().string();
            if (!PathEqualsIgnoreCase(extension, ".iso"))
                return;
            std::error_code ec;
            uint64_t size = fs::file_size(file, ec);
            if (ec || size < minSize)
                return;
            fs::path key = fs::weakly_canonical(file, ec);
            if (!seen.insert(ec ? file : key).second)
                return;

            // Only XDVDFS images whose tree reads count; a truncated one is kept
            // (with its status) so the UI can say why it won't install rather
            // than leave the user wondering where their image went.
            std::unique_ptr<DiscSource> source;
            Result opened = OpenDiscSource(file, source, { .allowTruncated = true });
            if (!source || !source->Find("default.xex"))
                return;

            DiscImageCandidate candidate;
            candidate.path = file;
            candidate.size = size;
            XexIdentity identity;
            Result checked = CheckGameVersion(*source, manifest, &identity);
            candidate.titleId = identity.titleId;
            candidate.xexMatches = checked.Ok();
            candidate.status = opened.Ok() ? checked : opened;
            found.push_back(std::move(candidate));
        }
    }

    bool IsUsableInstall(const std::filesystem::path& dir, bool requireMarker, const Manifest& manifest)
    {
        return UsableXex(dir, requireMarker, manifest).has_value();
    }

    std::optional<GameInstall> FindGameInstall(const Manifest& manifest)
    {
        if (const char* env = std::getenv("NFSMW_GAME_DIR"); env && *env)
        {
            if (std::optional<fs::path> xex = UsableXex(env, false, manifest))
                return Found(env, *xex, InstallOrigin::Environment, manifest);
            fprintf(stderr, "[install] NFSMW_GAME_DIR=%s has no default.xex of %llu bytes; ignoring it\n", env,
                (unsigned long long)manifest.Xex().size);
        }
        if (std::optional<fs::path> xex = UsableXex("game/files", false, manifest))
            return Found("game/files", *xex, InstallOrigin::Development, manifest);

        // An install the player put on another drive.
        if (std::optional<fs::path> recorded = RecordedInstallPath())
        {
            RecoverInterruptedInstall(*recorded, manifest);
            if (std::optional<fs::path> xex = UsableXex(*recorded, true, manifest))
                return Found(*recorded, *xex, InstallOrigin::User, manifest);
            // Why, for the log (the installer screen says it to the player:
            // DescribeRecordedInstall).
            std::error_code ec;
            fs::file_status status = fs::status(*recorded, ec);
            std::string why = status.type() == fs::file_type::not_found ? "isn't there now (a drive not connected?)"
                : ec ? std::format("can't be read ({})", ec.message())
                     : "isn't a current install (files missing, or an older version)";
            fprintf(stderr, "[install] the game was installed in %s, which %s\n", recorded->string().c_str(), why.c_str());
        }

        fs::path user = DefaultInstallPath();
        // A crash in the middle of a reinstall can leave the old install at
        // .previous, with nothing (or the new install) in its place.
        RecoverInterruptedInstall(user, manifest);
        if (std::optional<fs::path> xex = UsableXex(user, true, manifest))
            return Found(user, *xex, InstallOrigin::User, manifest);
        std::error_code ec;
        if (fs::exists(user / "default.xex", ec))
        {
            std::optional<std::string> marker = ReadInstallMarker(user);
            fprintf(stderr, "[install] %s is not a current install (marker %s, manifest %.*s): reinstall\n",
                user.c_str(), marker ? marker->c_str() : "missing", int(manifest.version.size()), manifest.version.data());
        }
        return std::nullopt;
    }

    namespace
    {
        fs::path LocationFile()
        {
            return GetUserPath() / "game-location";
        }

        // A folder that is the root of a mounted file system.
        bool IsMountPoint(const fs::path& dir)
        {
            struct stat self{}, parent{};
            if (stat(dir.c_str(), &self) != 0 || stat(dir.parent_path().c_str(), &parent) != 0)
                return false;
            return S_ISDIR(self.st_mode) && self.st_dev != parent.st_dev;
        }
    }

    std::optional<std::filesystem::path> RecordedInstallPath()
    {
        FILE* f = fopen(LocationFile().c_str(), "rb");
        if (!f)
            return std::nullopt;
        char buffer[4096];
        size_t n = fread(buffer, 1, sizeof(buffer) - 1, f);
        fclose(f);
        std::string text(buffer, n);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
            text.pop_back();
        if (text.empty() || text.front() != '/')
            return std::nullopt;
        // No trailing separator (records before v0.1.1 kept `--dest dir/`'s).
        fs::path path = fs::path(text).lexically_normal();
        while (path.has_relative_path() && path.filename().empty())
            path = path.parent_path();
        return path;
    }

    bool RecordInstallPath(const std::filesystem::path& dir)
    {
        std::error_code ec;
        fs::path file = LocationFile();
        // As the installer compares folders: absolute, links resolved, no
        // trailing separator (`--dest dir/` and `dir` are one record).
        fs::path folder = ResolvedFolder(dir);
        if (SameFolder(folder, DefaultInstallPath()))
        {
            fs::remove(file, ec);
            return !ec;
        }
        fs::create_directories(file.parent_path(), ec);
        fs::path temp = file;
        temp += ".tmp";
        FILE* f = fopen(temp.c_str(), "wb");
        if (!f)
            return false;
        std::string text = folder.string() + "\n";
        bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
        ok = fflush(f) == 0 && ok;
        fsync(fileno(f));
        fclose(f);
        if (ok)
            fs::rename(temp, file, ec);
        return ok && !ec;
    }

    std::vector<InstallDestination> InstallDestinations()
    {
        std::vector<InstallDestination> out;
        out.push_back({ "Internal storage", DefaultInstallPath(), false });
        auto addVolume = [&](const fs::path& volume)
        {
            if (Hidden(volume) || !IsMountPoint(volume))
                return;
            // Not read-only drives (a disc), nor the system's hidden volumes
            // (macOS Recovery, Preboot, VM).
            struct statvfs vfs{};
            if (statvfs(volume.c_str(), &vfs) != 0 || (vfs.f_flag & ST_RDONLY))
                return;
#ifdef __APPLE__
            struct statfs sfs{};
            if (statfs(volume.c_str(), &sfs) != 0 || (sfs.f_flags & MNT_DONTBROWSE))
                return;
#endif
            std::string label = volume.filename().string();
            out.push_back({ label.empty() ? volume.string() : label, volume / kUserFolderName / "game", true });
        };
        std::error_code ec;
#ifdef __APPLE__
        for (fs::directory_iterator it("/Volumes", ec), end; !ec && it != end; it.increment(ec))
        {
            std::error_code sameEc;
            if (!fs::equivalent(it->path(), "/", sameEc))
                addVolume(it->path());
        }
#else
        std::string user = UserName();
        std::vector<fs::path> parents = { "/run/media", "/media" };
        if (!user.empty())
        {
            parents.push_back(fs::path("/run/media") / user);
            parents.push_back(fs::path("/media") / user);
        }
        for (const fs::path& parent : parents)
            for (fs::directory_iterator it(parent, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec))
                addVolume(it->path());
#endif
        return out;
    }

    std::vector<std::filesystem::path> DiscImageSearchFolders()
    {
        std::vector<fs::path> folders;
        auto add = [&](const fs::path& dir)
        {
            std::error_code ec;
            if (fs::is_directory(dir, ec) && std::find(folders.begin(), folders.end(), dir) == folders.end())
                folders.push_back(dir);
        };
        if (const char* home = std::getenv("HOME"); home && *home)
        {
            fs::path h = home;
            add(h);
            for (const char* sub : { "Downloads", "Desktop", "Documents", "Games" })
                add(h / sub);
        }
#ifdef __APPLE__
        // /Volumes also lists the startup disk (a link to /): skip it, a scan of
        // the whole system disk is what the folders above avoid.
        std::error_code ec;
        for (fs::directory_iterator it("/Volumes", ec), end; !ec && it != end; it.increment(ec))
        {
            std::error_code sameEc;
            if (!Hidden(it->path()) && !fs::equivalent(it->path(), "/", sameEc))
                add(it->path());
        }
#else
        // SteamOS mounts SD cards and USB drives at /run/media/<user>/<label>
        // (older releases: /run/media/<device>); other distributions use
        // /run/media/<user> or /media/<user>, or /media/<label>. Every folder
        // inside those is a mount point to search.
        auto addChildren = [&](const fs::path& dir)
        {
            std::error_code ec;
            for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec))
                if (!Hidden(it->path()))
                    add(it->path());
        };
        std::string user = UserName();
        addChildren("/run/media");
        if (!user.empty())
            addChildren(fs::path("/run/media") / user);
        addChildren("/media");
        if (!user.empty())
            addChildren(fs::path("/media") / user);
#endif
        return folders;
    }

    std::vector<DiscImageCandidate> FindDiscImages(const std::vector<std::filesystem::path>& folders,
        uint64_t minSize, const std::atomic<bool>* cancel, const Manifest& manifest)
    {
        std::vector<DiscImageCandidate> found;
        std::set<fs::path> seenFiles, listedDirs;
        fs::path library;
        if (const char* home = std::getenv("HOME"); home && *home)
            library = fs::path(home) / "Library";
        auto cancelled = [&] { return cancel && cancel->load(std::memory_order_relaxed); };

        // Lists `dir`'s .iso files (once per directory, however it is reached)
        // and, for a search folder, collects its subfolders.
        auto list = [&](const fs::path& dir, std::vector<fs::path>* subdirs)
        {
            std::error_code ec;
            fs::path key = fs::weakly_canonical(dir, ec);
            bool filesListed = !listedDirs.insert(ec ? dir : key).second;
            if (filesListed && !subdirs)
                return;
            for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
                 !ec && it != end && !cancelled(); it.increment(ec))
            {
                const fs::path& path = it->path();
                if (Hidden(path))
                    continue;
                std::error_code typeEc;
                if (it->is_directory(typeEc))
                {
                    // ~/Library holds no disc images, and its subfolders are
                    // where macOS privacy prompts come from.
                    if (subdirs && path != library)
                        subdirs->push_back(path);
                }
                else if (!filesListed && it->is_regular_file(typeEc))
                {
                    Consider(path, minSize, manifest, seenFiles, found);
                }
            }
        };

        for (const fs::path& folder : folders)
        {
            std::vector<fs::path> subdirs;
            list(folder, &subdirs);
            std::sort(subdirs.begin(), subdirs.end());
            for (const fs::path& sub : subdirs)
            {
                if (cancelled())
                    return found;
                list(sub, nullptr);
            }
        }

        std::sort(found.begin(), found.end(), [](const DiscImageCandidate& a, const DiscImageCandidate& b)
        {
            if (a.status.Ok() != b.status.Ok())
                return a.status.Ok();
            return a.path < b.path;
        });
        return found;
    }
}
