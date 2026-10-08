// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See installer.h.
#include "installer.h"

#include <user/paths.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
#if defined(__APPLE__) && TARGET_OS_IOS
#include <platform/ios_files.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <format>
#include <fstream>
#include <set>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace install
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr const char* kGameName = "Need for Speed: Most Wanted";
        // The release manifest.inc was generated from (config/nfsmw.toml).
        constexpr const char* kRelease = "the USA Xbox 360 release";
        constexpr size_t kChunk = 4 << 20;
        // Room beyond the files themselves: directory blocks, the marker, and
        // not filling the user's disk to the last byte.
        constexpr uint64_t kSpaceMargin = 64ull << 20;
        // Marks "<destination>.partial" as the installer's own, so a leftover
        // from a crash is deleted on the next attempt and nothing else is.
        constexpr const char* kPartialTag = ".nfsmw-install-partial";

        Result Fail(Error error, std::string message, std::string file = {})
        {
            Result r;
            r.error = error;
            r.message = std::move(message);
            r.file = std::move(file);
            return r;
        }

        void Report(const ProgressCallback& progress, const Progress& p)
        {
            if (progress)
                progress(p);
        }

        bool Cancelled(const std::atomic<bool>* cancel)
        {
            return cancel && cancel->load(std::memory_order_relaxed);
        }

        const char* What(const DiscSource& source)
        {
            return source.Kind() == SourceKind::Image ? "This disc image" : "This folder";
        }

        bool WriteAll(int fd, const void* data, size_t size)
        {
            auto p = static_cast<const uint8_t*>(data);
            while (size)
            {
                ssize_t n = write(fd, p, std::min<size_t>(size, 1u << 30));
                if (n < 0)
                {
                    if (errno == EINTR)
                        continue;
                    return false;
                }
                p += n;
                size -= size_t(n);
            }
            return true;
        }

        // A failed write or create, by what the user can do about it.
        Result WriteFailure(int error, const fs::path& path)
        {
            if (error == ENOSPC || error == EDQUOT)
                return Fail(Error::NotEnoughSpace, std::format("The disk filled up while installing ({}). Free some space "
                    "and try again.", path.string()), path.string());
            if (error == EACCES || error == EPERM || error == EROFS)
                return Fail(Error::DestinationNotWritable, std::format("Cannot write {}: {}. Choose another folder.",
                    path.string(), strerror(error)), path.string());
            return Fail(Error::WriteError, std::format("Cannot write {}: {}.", path.string(), strerror(error)), path.string());
        }

        void SyncDirectory(const fs::path& dir)
        {
            int fd = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (fd >= 0)
            {
                fsync(fd);
                close(fd);
            }
        }

        // Anything at `path`, a dangling link included.
        bool Present(const fs::path& path)
        {
            std::error_code ec;
            return fs::exists(fs::symlink_status(path, ec));
        }

        bool IsEmptyDirectory(const fs::path& dir)
        {
            std::error_code ec;
            return fs::is_directory(dir, ec) && fs::is_empty(dir, ec) && !ec;
        }

        // Only hidden entries (or none): what Finder (.DS_Store, .localized)
        // or a copy to an exFAT drive (._ files) leaves in a folder the
        // player thinks is empty.
        bool HoldsOnlyHidden(const fs::path& dir)
        {
            std::error_code ec;
            for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
            {
                std::string name = it->path().filename().string();
                if (name.empty() || name[0] != '.')
                    return false;
            }
            return !ec;
        }

        // Files in the order they're read: disc order for an image (sequential
        // reads, which matters on a USB stick or a hard drive), then by path.
        std::vector<std::pair<const ManifestFile*, const DiscFile*>> ReadOrder(const DiscSource& source, const Manifest& manifest)
        {
            std::vector<std::pair<const ManifestFile*, const DiscFile*>> order;
            for (const ManifestFile& mf : manifest.files)
                order.push_back({ &mf, source.Find(mf.path) });
            std::sort(order.begin(), order.end(), [](const auto& a, const auto& b)
            {
                if (a.second->offset != b.second->offset)
                    return a.second->offset < b.second->offset;
                return a.first->path < b.first->path;
            });
            return order;
        }

        // Streams one file through SHA-256 in fixed chunks, writing each
        // chunk to `fd` too when it is valid: the copy and the check are one
        // read of the source.
        Result StreamFile(const DiscSource& source, const DiscFile& file, const ManifestFile& expected, int fd,
            const fs::path& target, std::vector<uint8_t>& buffer, Progress& p, const ProgressCallback& progress,
            const std::atomic<bool>* cancel)
        {
            std::string error;
            std::unique_ptr<FileReader> reader = source.Open(file, error);
            if (!reader)
                return Fail(Error::ReadError, std::format("Cannot open {} in the source: {}.", file.path, error), file.path);

            Sha256 sha;
            for (uint64_t at = 0; at < file.size;)
            {
                if (Cancelled(cancel))
                    return Fail(Error::Cancelled, "Installation cancelled.");
                size_t n = size_t(std::min<uint64_t>(buffer.size(), file.size - at));
                if (!reader->Read(at, buffer.data(), n, error))
                    return Fail(Error::ReadError, std::format("Cannot read {} from the source: {}. If the disc image is on "
                        "a removable drive, check that it is still connected.", file.path, error), file.path);
                sha.Update(buffer.data(), n);
                if (fd >= 0 && !WriteAll(fd, buffer.data(), n))
                    return WriteFailure(errno, target);
                at += n;
                p.bytesDone += n;
                Report(progress, p);
            }

            std::string found = ToHex(sha.Finish());
            if (found != expected.sha256)
            {
                Result r = Fail(Error::CorruptFile, std::format("{} does not match the original disc: its SHA-256 is {}, "
                    "expected {}. The dump is damaged; dump the disc again.", std::string(expected.path), found,
                    std::string(expected.sha256)), std::string(expected.path));
                r.foundHash = found;
                return r;
            }
            return {};
        }

        // Install's own directories: "<dest>.partial" while copying,
        // "<dest>.previous" for the old install while the new one moves in.
        struct Layout
        {
            fs::path destination, parent, partial, previous;
        };

        // Where `requested` really is: absolute, with links resolved (a game
        // folder linked to an SD card installs beside the link's target, on
        // the same file system, where the final rename is atomic), and not
        // something an install can't take the place of. Changes nothing.
        Result ResolveDestination(const fs::path& requested, Layout& layout)
        {
            std::error_code ec;
            if (requested.empty())
                return Fail(Error::DestinationNotWritable, "Choose a folder to install into.");
            fs::path dest = fs::absolute(requested, ec);
            if (ec)
                return Fail(Error::DestinationNotWritable, std::format("Cannot use {}: {}.", requested.string(),
                    ec.message()), requested.string());
            if (dest.filename().empty())
                dest = dest.parent_path();
            // A link to a folder that isn't there (a drive that isn't
            // connected): the final rename would replace the link, and
            // following it would create folders where the drive mounts.
            if (fs::is_symlink(dest, ec) && !fs::exists(dest, ec))
            {
                std::error_code linkEc;
                fs::path target = fs::read_symlink(dest, linkEc);
                return Fail(Error::DestinationNotWritable, std::format("{} is a link to {}, which does not exist. Connect "
                    "the drive it points to, or choose another folder.", dest.string(), target.string()), dest.string());
            }
            fs::path resolved = fs::weakly_canonical(dest, ec);
            dest = ec ? dest.lexically_normal() : resolved;
            if (dest.filename().empty())
                dest = dest.parent_path();
            if (dest.empty() || dest == dest.root_path())
                return Fail(Error::DestinationNotWritable, "Choose a folder to install into, not the root of a drive.");
            layout.destination = dest;
            layout.parent = dest.parent_path();
            layout.partial = layout.parent / (dest.filename().string() + ".partial");
            layout.previous = layout.parent / (dest.filename().string() + ".previous");

            // The top of a mounted drive can't be renamed, and its siblings
            // are on the drive above it: the copy would fill the wrong disk
            // (on SteamOS, /run/media is in RAM) and then fail to move over.
            struct stat destStat, parentStat;
            if (stat(dest.c_str(), &destStat) == 0 && S_ISDIR(destStat.st_mode)
                && stat(layout.parent.c_str(), &parentStat) == 0 && destStat.st_dev != parentStat.st_dev)
                return Fail(Error::DestinationNotWritable, std::format("{} is a whole drive. Choose a folder on it instead, "
                    "such as {}.", dest.string(), (dest / kUserFolderName).string()), dest.string());
            return {};
        }

        // The nearest folder at or above `path` that exists: where missing
        // folders would be created. Empty if a file is in the way (`file`).
        fs::path NearestFolder(fs::path path, fs::path& file)
        {
            std::error_code ec;
            while (!path.empty())
            {
                fs::file_status status = fs::status(path, ec);
                if (fs::is_directory(status))
                    return path;
                if (fs::exists(status))
                {
                    file = path;
                    return {};
                }
                if (path == path.parent_path())
                    break;
                path = path.parent_path();
            }
            return {};
        }

        // A leftover "<dest>.previous" that Recover may put back or retire:
        // a previous install (it has the marker), or the hidden files a
        // replaced "empty" folder held.
        bool PreviousIsOurs(const fs::path& previous)
        {
            std::error_code ec;
            return !fs::is_symlink(previous, ec) && fs::is_directory(previous, ec)
                && (ReadInstallMarker(previous) || HoldsOnlyHidden(previous));
        }

        bool PartialIsOurs(const fs::path& partial)
        {
            std::error_code ec;
            return !fs::is_symlink(partial, ec) && fs::is_directory(partial, ec)
                && (Present(partial / kPartialTag) || ReadInstallMarker(partial) || IsEmptyDirectory(partial));
        }

        // The destination as Install would find it, without changing
        // anything. Before Recover has run (`recovered` false), a previous
        // install left at .previous counts as what it will become.
        Result Inspect(const Layout& layout, const Manifest& manifest, bool recovered, DestinationInfo& info)
        {
            std::error_code ec;
            const fs::path& dest = layout.destination;
            info.path = dest;
            info.required = manifest.totalBytes + kSpaceMargin;
            info.replacing = false;

            bool pendingPrevious = !recovered && PreviousIsOurs(layout.previous);
            bool destinationEmpty = true;
            fs::file_status status = fs::status(dest, ec);
            if (fs::exists(status))
            {
                if (!fs::is_directory(status))
                    return Fail(Error::DestinationNotEmpty, std::format("{} is a file. Choose a folder to install into.",
                        dest.string()), dest.string());
                // Only a previous install or an empty folder may be replaced.
                // Hidden files like .DS_Store don't count; they are kept.
                bool install = ReadInstallMarker(dest).has_value();
                if (!install && !HoldsOnlyHidden(dest))
                    return Fail(Error::DestinationNotEmpty, std::format("{} already holds other files. Choose an empty "
                        "folder or a new one.", dest.string()), dest.string());
                info.replacing = install;
                destinationEmpty = IsEmptyDirectory(dest);
            }
            info.replacing = info.replacing || pendingPrevious;

            // A non-empty destination is moved to .previous while the new
            // install takes its place, so that name must be free (a previous
            // install still waiting there is put back or retired first).
            if (!destinationEmpty && !pendingPrevious && Present(layout.previous) && !IsEmptyDirectory(layout.previous))
                return Fail(Error::DestinationNotEmpty, std::format("{} is in the way (the installer moves the current "
                    "install there while the new one takes its place). Move or delete it.", layout.previous.string()),
                    layout.previous.string());
            if (Present(layout.partial) && !PartialIsOurs(layout.partial))
                return Fail(Error::DestinationNotEmpty, std::format("{} is in the way (the installer copies into it "
                    "first). Move or delete it.", layout.partial.string()), layout.partial.string());

            fs::path file;
            fs::path folder = NearestFolder(layout.parent, file);
            if (folder.empty())
                return Fail(Error::DestinationNotWritable, file.empty()
                    ? std::format("Cannot install to {}.", dest.string())
                    : std::format("Cannot install to {}: {} is a file, not a folder.", dest.string(), file.string()),
                    dest.string());
            if (access(folder.c_str(), W_OK | X_OK) != 0)
                return Fail(Error::DestinationNotWritable, std::format("Cannot write to {}: {}. Choose another folder.",
                    folder.string(), strerror(errno)), folder.string());
            return {};
        }

        uint64_t TreeSize(const fs::path& dir)
        {
            uint64_t total = 0;
            std::error_code ec;
            for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
            {
                std::error_code fileEc;
                if (it->is_regular_file(fileEc) && !it->is_symlink(fileEc))
                    total += it->file_size(fileEc);
            }
            return total;
        }

        // Free space in `folder`; nullopt if it can't be told.
        std::optional<uint64_t> FreeSpace(const fs::path& folder)
        {
#if defined(__APPLE__) && TARGET_OS_IOS
            // iOS keeps space in purgeable caches that statvfs counts as used,
            // and frees it for something the player asked for: the figure
            // Apple says to check before a large download.
            if (std::optional<uint64_t> important = platform::ios::AvailableCapacityForImportantUsage(folder))
                return important;
#endif
            struct statvfs vfs;
            if (statvfs(folder.c_str(), &vfs) != 0)
                return std::nullopt;
            return uint64_t(vfs.f_bavail) * vfs.f_frsize;
        }

        // `reclaimable`: bytes of the installer's own leftover .partial,
        // which Install deletes before copying.
        Result CheckSpace(const Layout& layout, DestinationInfo& info, uint64_t reclaimable)
        {
            info.available = UINT64_MAX;
            fs::path file;
            fs::path folder = NearestFolder(layout.parent, file);
            std::optional<uint64_t> space = folder.empty() ? std::nullopt : FreeSpace(folder);
            if (!space)
                return {};  // can't tell; a full disk still fails cleanly mid-copy
            info.available = *space + reclaimable;
            if (info.available >= info.required)
                return {};
            return Fail(Error::NotEnoughSpace, std::format("Not enough disk space: the game needs {}, and {} has {} free. "
                "Free up {} or choose another drive.", FormatSize(info.required), folder.string(),
                FormatSize(info.available), FormatSize(info.required - info.available)), folder.string());
        }

        // One install at a time per parent folder: an exclusive flock on the
        // folder, held until Install returns. Two installers (the first-run
        // screen and --install) would otherwise delete each other's partial
        // copy, or one's cleanup would delete the other's finished install.
        // Where the file system can't lock, installs go unlocked.
        class FolderLock
        {
        public:
            explicit FolderLock(const fs::path& dir)
            {
                fd = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
                if (fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) != 0)
                {
                    busy = errno == EWOULDBLOCK;
                    close(fd);
                    fd = -1;
                }
            }

            ~FolderLock()
            {
                if (fd >= 0)
                    close(fd);
            }

            FolderLock(const FolderLock&) = delete;
            FolderLock& operator=(const FolderLock&) = delete;

            bool Busy() const { return busy; }

        private:
            int fd = -1;
            bool busy = false;
        };

        // The manifest's spelling of a folder it has files in, matched
        // case-insensitively; nullopt if `relative` isn't one.
        std::optional<std::string> ManifestFolder(const Manifest& manifest, std::string_view relative)
        {
            for (const ManifestFile& f : manifest.files)
                if (f.path.size() > relative.size() && f.path[relative.size()] == '/'
                    && PathEqualsIgnoreCase(f.path.substr(0, relative.size()), relative))
                    return std::string(f.path.substr(0, relative.size()));
            return std::nullopt;
        }

        // Moves everything in the old install `from` that isn't the game's
        // into the new install `to`: DLC (xam.cpp keeps it in the game
        // folder's dlc/), a disc image or notes the player put there, files
        // beside the game's in NFS/. Game files and the marker stay behind to
        // be deleted with the old install, and so does a link standing in for
        // one of the game's folders (only the link goes; what it points to is
        // left alone). False if anything could not be moved: the old install
        // must then be kept.
        bool CarryOver(const fs::path& from, const fs::path& to, const std::string& prefix, const Manifest& manifest)
        {
            // Listed first: entries leave the folder while it is gone through.
            std::vector<fs::directory_entry> entries;
            std::error_code ec;
            for (fs::directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec))
                entries.push_back(*it);
            if (ec)
            {
                fprintf(stderr, "[install] cannot list %s: %s\n", from.c_str(), ec.message().c_str());
                return false;
            }

            bool complete = true;
            for (const fs::directory_entry& entry : entries)
            {
                std::string name = entry.path().filename().string();
                std::string relative = prefix + name;
                if (prefix.empty() && (name == kInstallMarker || name == kPartialTag))
                    continue;
                if (manifest.Find(relative))
                    continue;
                std::error_code typeEc;
                if (std::optional<std::string> folder = ManifestFolder(manifest, relative))
                {
                    if (!entry.is_symlink(typeEc) && entry.is_directory(typeEc))
                        complete &= CarryOver(entry.path(), to / fs::path(*folder).filename(), relative + "/", manifest);
                    continue;
                }
                fs::path target = to / name;
                if (Present(target))
                {
                    fprintf(stderr, "[install] cannot move %s: the new install already has %s\n", entry.path().c_str(),
                        target.c_str());
                    complete = false;
                    continue;
                }
                fs::rename(entry.path(), target, typeEc);
                if (typeEc)
                {
                    fprintf(stderr, "[install] cannot move %s into the new install: %s\n", entry.path().c_str(),
                        typeEc.message().c_str());
                    complete = false;
                }
            }
            return complete;
        }

        // Deletes the old install at .previous once the new one is in place,
        // after carrying the player's files over. If any could not be moved,
        // it stays (logged), and the next install or startup tries again.
        void RetirePrevious(const Layout& layout, const Manifest& manifest)
        {
            if (!CarryOver(layout.previous, layout.destination, "", manifest))
            {
                fprintf(stderr, "[install] kept the previous install at %s: it still holds files that could not be moved "
                    "into %s\n", layout.previous.c_str(), layout.destination.c_str());
                return;
            }
            std::error_code ec;
            fs::remove_all(layout.previous, ec);
            if (ec)
                fprintf(stderr, "[install] could not delete the previous install at %s: %s\n", layout.previous.c_str(),
                    ec.message().c_str());
        }

        // Undoes or finishes a reinstall interrupted between MoveIntoPlace's
        // two renames, or before the old install was retired.
        void Recover(const Layout& layout, const Manifest& manifest)
        {
            if (!PreviousIsOurs(layout.previous))
                return;
            std::error_code ec;
            if (!fs::exists(layout.destination, ec) || IsEmptyDirectory(layout.destination))
            {
                // Nothing took its place: put it back (a rename replaces an
                // empty folder).
                fs::rename(layout.previous, layout.destination, ec);
                fprintf(stderr, "[install] restored %s from an interrupted reinstall%s%s\n", layout.destination.c_str(),
                    ec ? ": " : "", ec ? ec.message().c_str() : "");
            }
            else if (ReadInstallMarker(layout.destination))
            {
                // The new install made it into place: finish the job.
                RetirePrevious(layout, manifest);
            }
            // Otherwise the destination holds something else: leave both
            // alone (Inspect refuses such a destination).
        }

        // 0, or the errno of the failure.
        int WriteMarker(const fs::path& dir, const Manifest& manifest)
        {
            std::string text = std::format(
                "# SpeedBreaker: a verified install of the game. Written last by the installer,\n"
                "# after every file matched the manifest; FindGameInstall() requires it.\n"
                "manifest_version = \"{}\"\n"
                "xex_sha256 = \"{}\"\n"
                "files = {}\n"
                "bytes = {}\n",
                manifest.version, manifest.Xex().sha256, manifest.files.size(), manifest.totalBytes);
            fs::path path = dir / kInstallMarker;
            int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            if (fd < 0)
                return errno;
            int error = 0;
            if (!WriteAll(fd, text.data(), text.size()) || fsync(fd) != 0)
                error = errno ? errno : EIO;
            if (close(fd) != 0 && !error)
                error = errno ? errno : EIO;
            return error;
        }

        // Moves the finished copy into place. rename() replaces a missing or
        // empty destination atomically; anything else there (a previous
        // install, or the hidden files of an "empty" folder) is first moved
        // aside (and restored if the second rename fails), then retired.
        Result MoveIntoPlace(const Layout& layout, const Manifest& manifest)
        {
            std::error_code ec;
            bool replacing = fs::exists(layout.destination, ec) && !IsEmptyDirectory(layout.destination);
            if (replacing)
            {
                fs::rename(layout.destination, layout.previous, ec);
                if (ec)
                    return Fail(Error::WriteError, std::format("Cannot move the previous install at {} aside: {}.",
                        layout.destination.string(), ec.message()), layout.destination.string());
            }
            fs::rename(layout.partial, layout.destination, ec);
            if (ec)
            {
                std::error_code restoreEc;
                if (replacing)
                    fs::rename(layout.previous, layout.destination, restoreEc);
                return Fail(Error::WriteError, std::format("Cannot move the finished install to {}: {}.",
                    layout.destination.string(), ec.message()), layout.destination.string());
            }
            SyncDirectory(layout.parent);
            if (replacing)
                RetirePrevious(layout, manifest);
            return {};
        }
    }

    Result CheckGameVersion(const DiscSource& source, const Manifest& manifest, XexIdentity* identity)
    {
        XexIdentity local;
        XexIdentity& id = identity ? *identity : local;
        Result r = IdentifyXex(source, id);
        if (r.error == Error::MissingFile)
        {
            // Original Xbox discs boot default.xbe.
            if (source.Find("default.xbe"))
                return Fail(Error::WrongGame, std::format("{} holds an original Xbox game, not {} for Xbox 360.",
                    What(source), kGameName), "default.xbe");
            return Fail(Error::NotADiscImage, std::format("{} has no default.xex: it is not an Xbox 360 game.",
                What(source)), "default.xex");
        }
        if (!r.Ok())
            return r;

        const ManifestFile& xex = manifest.Xex();
        std::string found = ToHex(id.sha256);
        if (found == xex.sha256)
            return {};

        if (id.titleId != manifest.titleId)
        {
            r = Fail(Error::WrongGame, id.titleId
                ? std::format("{} is not {}: it holds another game (title ID {:08X}; {} is {:08X}).", What(source),
                    kGameName, id.titleId, kGameName, manifest.titleId)
                : std::format("{} is not {}: its default.xex is not an Xbox 360 game executable.", What(source), kGameName),
                id.file->path);
            r.foundHash = found;
            return r;
        }
        // Same title, different executable: another region or revision. The
        // recompiled code matches one executable byte for byte, so nothing
        // else can run, whatever else is on the disc.
        r = Fail(Error::UnsupportedVersion, std::format("{} is {}, but not the version this build supports: its "
            "default.xex has SHA-256 {}. Only {} is supported (default.xex {}){}.", What(source), kGameName, found,
            kRelease, std::string(xex.sha256),
            id.file->size == xex.size ? ", unless this one is a damaged dump" : ""), id.file->path);
        r.foundHash = found;
        return r;
    }

    Result CheckSource(const DiscSource& source, const Manifest& manifest)
    {
        if (source.Truncated())
            return Fail(Error::TruncatedImage, "This disc image is incomplete: it ends before some of its files do. "
                "Copy it again, or make a new backup from your disc.", source.Path().string());
        Result r = CheckGameVersion(source, manifest);
        if (!r.Ok())
            return r;

        const ManifestFile* missing = nullptr;
        const ManifestFile* wrongSize = nullptr;
        const DiscFile* wrongFile = nullptr;
        size_t missingCount = 0, wrongCount = 0;
        for (const ManifestFile& mf : manifest.files)
        {
            const DiscFile* f = source.Find(mf.path);
            if (!f)
            {
                missing = missing ? missing : &mf;
                missingCount++;
            }
            else if (f->size != mf.size)
            {
                if (!wrongSize)
                {
                    wrongSize = &mf;
                    wrongFile = f;
                }
                wrongCount++;
            }
        }
        auto more = [](size_t n) { return n > 1 ? std::format(" (and {} more)", n - 1) : std::string(); };
        if (missing)
            return Fail(Error::MissingFile, std::format("{} is incomplete: {} is missing{}. Dump the disc again, or copy "
                "every file of the extracted disc.", What(source), std::string(missing->path), more(missingCount)),
                std::string(missing->path));
        if (wrongSize)
            return Fail(Error::CorruptFile, std::format("{} is damaged: {} is {} bytes, but should be {}{}. Dump the "
                "disc again.", What(source), std::string(wrongSize->path), wrongFile->size, wrongSize->size,
                more(wrongCount)), std::string(wrongSize->path));
        return {};
    }

    Result CheckDestination(const std::filesystem::path& destination, DestinationInfo* info, const Manifest& manifest)
    {
        DestinationInfo local;
        DestinationInfo& out = info ? *info : local;
        out = {};
        out.required = manifest.totalBytes + kSpaceMargin;
        Layout layout;
        Result r = ResolveDestination(destination, layout);
        if (r.Ok())
            r = Inspect(layout, manifest, false, out);
        if (r.Ok())
            r = CheckSpace(layout, out, PartialIsOurs(layout.partial) ? TreeSize(layout.partial) : 0);
        return r;
    }

    void RecoverInterruptedInstall(const std::filesystem::path& destination, const Manifest& manifest)
    {
        Layout layout;
        if (!ResolveDestination(destination, layout).Ok() || !Present(layout.previous))
            return;
        FolderLock lock(layout.parent);
        if (!lock.Busy())   // otherwise an install is running and owns these folders
            Recover(layout, manifest);
    }

    Result Install(const DiscSource& source, const std::filesystem::path& destination,
        const ProgressCallback& progress, const std::atomic<bool>* cancel, const Manifest& manifest)
    {
        Progress p;
        p.bytesTotal = manifest.totalBytes;
        p.filesTotal = manifest.files.size();
        Report(progress, p);

        Result r = CheckSource(source, manifest);
        if (!r.Ok())
            return r;
        if (Cancelled(cancel))
            return Fail(Error::Cancelled, "Installation cancelled.");

        // Refuse what can be refused before anything on disk changes.
        Layout layout;
        DestinationInfo info;
        r = ResolveDestination(destination, layout);
        if (r.Ok())
            r = Inspect(layout, manifest, false, info);
        if (r.Ok())
            r = CheckSpace(layout, info, PartialIsOurs(layout.partial) ? TreeSize(layout.partial) : 0);
        if (!r.Ok())
            return r;

        std::error_code ec;
        fs::create_directories(layout.parent, ec);
        if (ec)
            return Fail(Error::DestinationNotWritable, std::format("Cannot create {}: {}.", layout.parent.string(),
                ec.message()), layout.parent.string());
        FolderLock lock(layout.parent);
        if (lock.Busy())
            return Fail(Error::DestinationNotWritable, std::format("Another install into {} is running. Wait for it to "
                "finish, then try again.", layout.parent.string()), layout.parent.string());

        // Under the lock: finish an interrupted reinstall, look again, and
        // clear this installer's own leftover copy.
        Recover(layout, manifest);
        r = Inspect(layout, manifest, true, info);
        if (!r.Ok())
            return r;
        if (Present(layout.partial))
        {
            fs::remove_all(layout.partial, ec);
            if (ec)
                return Fail(Error::DestinationNotWritable, std::format("Cannot remove the unfinished install at {}: {}.",
                    layout.partial.string(), ec.message()), layout.partial.string());
        }
        r = CheckSpace(layout, info, 0);
        if (!r.Ok())
            return r;

        if (!fs::create_directory(layout.partial, ec) || ec)
            return Fail(Error::DestinationNotWritable, std::format("Cannot create {}: {}. Choose another folder.",
                layout.partial.string(), ec ? ec.message() : "it already exists"), layout.partial.string());
        auto failed = [&](Result result)
        {
            std::error_code removeEc;
            fs::remove_all(layout.partial, removeEc);
            return result;
        };
        int tag = open((layout.partial / kPartialTag).c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (tag < 0)
            return failed(WriteFailure(errno, layout.partial / kPartialTag));
        close(tag);
#if defined(__APPLE__) && TARGET_OS_IOS
        // 7 GB the player's disc image can make again: not for iCloud or a
        // computer's backup. Marked before the copy, so a backup taken while
        // it runs skips it too.
        platform::ios::ExcludeFromBackup(layout.partial);
#endif

        std::vector<uint8_t> buffer(kChunk);
        std::set<fs::path> directories{ layout.partial };
        p.phase = Progress::Phase::Copying;
        for (auto [mf, file] : ReadOrder(source, manifest))
        {
            // The manifest's spelling, not the source's: see disc_source.h.
            fs::path target = layout.partial / fs::path(std::string(mf->path));
            if (directories.insert(target.parent_path()).second)
            {
                fs::create_directories(target.parent_path(), ec);
                if (ec)
                    return failed(WriteFailure(ec.value(), target.parent_path()));
            }
            p.currentFile = mf->path;
            Report(progress, p);

            int fd = open(target.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
            if (fd < 0)
                return failed(WriteFailure(errno, target));
            r = StreamFile(source, *file, *mf, fd, target, buffer, p, progress, cancel);
            // fsync before the rename below makes the install durable: after a
            // power cut the marker never stands beside files that weren't
            // written out.
            if (r.Ok() && fsync(fd) != 0)
                r = WriteFailure(errno, target);
            if (close(fd) != 0 && r.Ok())
                r = WriteFailure(errno, target);
            if (!r.Ok())
                return failed(r);
            p.filesDone++;
        }

        p.phase = Progress::Phase::Finalizing;
        p.currentFile = {};
        Report(progress, p);
        if (Cancelled(cancel))
            return failed(Fail(Error::Cancelled, "Installation cancelled."));
        if (int error = WriteMarker(layout.partial, manifest))
            return failed(WriteFailure(error, layout.partial / kInstallMarker));
        fs::remove(layout.partial / kPartialTag, ec);
        for (const fs::path& dir : directories)
            SyncDirectory(dir);

        r = MoveIntoPlace(layout, manifest);
        if (!r.Ok())
            return failed(r);
#if defined(__APPLE__) && TARGET_OS_IOS
        // The game folder too: the rename keeps the mark, but a lost one
        // would put 7 GB into the player's iCloud backup.
        platform::ios::ExcludeFromBackup(layout.destination);
#endif
        fprintf(stderr, "[install] installed %zu files (%s) to %s\n", manifest.files.size(),
            FormatSize(manifest.totalBytes).c_str(), layout.destination.c_str());
        return {};
    }

    Result Verify(const DiscSource& source, const ProgressCallback& progress, const std::atomic<bool>* cancel,
        const Manifest& manifest)
    {
        Progress p;
        p.bytesTotal = manifest.totalBytes;
        p.filesTotal = manifest.files.size();
        Report(progress, p);

        Result r = CheckSource(source, manifest);
        if (!r.Ok())
            return r;

        std::vector<uint8_t> buffer(kChunk);
        p.phase = Progress::Phase::Verifying;
        for (auto [mf, file] : ReadOrder(source, manifest))
        {
            p.currentFile = mf->path;
            Report(progress, p);
            r = StreamFile(source, *file, *mf, -1, {}, buffer, p, progress, cancel);
            if (!r.Ok())
                return r;
            p.filesDone++;
        }
        p.phase = Progress::Phase::Finalizing;
        p.currentFile = {};
        Report(progress, p);
        return {};
    }

    std::filesystem::path DefaultInstallPath()
    {
        return GetUserPath() / "game";
    }

    std::optional<std::string> ReadInstallMarker(const std::filesystem::path& dir)
    {
        // A few lines of text: read no more than that from whatever file
        // happens to have the name.
        std::ifstream f(dir / kInstallMarker, std::ios::binary);
        char text[4096];
        f.read(text, sizeof(text));
        std::string_view content(text, size_t(std::max<std::streamsize>(f.gcount(), 0)));
        constexpr std::string_view key = "manifest_version";
        while (!content.empty())
        {
            // manifest_version = "<version>"
            size_t end = content.find('\n');
            std::string_view line = content.substr(0, end);
            content = end == std::string_view::npos ? std::string_view() : content.substr(end + 1);
            if (!line.starts_with(key))
                continue;
            size_t first = line.find('"'), last = line.rfind('"');
            if (first == std::string_view::npos || last <= first)
                return std::nullopt;
            return std::string(line.substr(first + 1, last - first - 1));
        }
        return std::nullopt;
    }
}
