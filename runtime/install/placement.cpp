// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See placement.h.
#include "placement.h"
#include "locate.h"

#include <user/paths.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <string_view>
#include <system_error>

// The folder reads are std::filesystem's; what it has no word for (the
// device a folder is on, permission to write, the file system's name, macOS
// ACLs) is POSIX, and assumed fine on Windows (placement.h).
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <TargetConditionals.h>
#include <mach-o/dyld.h>
#include <sys/mount.h>
#include <sys/param.h>
#if !TARGET_OS_IOS
#include <CoreFoundation/CoreFoundation.h>
#include <sys/acl.h>
#endif
#endif

namespace install
{
    namespace
    {
        namespace fs = std::filesystem;

        // Absolute, normalized, no trailing separator.
        fs::path Normal(const fs::path& path)
        {
            fs::path p = path.lexically_normal();
            while (p.has_relative_path() && p.filename().empty())
                p = p.parent_path();
            return p;
        }

        fs::path Resolved(const fs::path& path)
        {
            std::error_code ec;
            fs::path absolute = fs::absolute(path, ec);
            if (ec)
                absolute = path;
            fs::path canonical = fs::weakly_canonical(absolute, ec);
            return Normal(ec ? absolute : canonical);
        }

        // The errno an error code stands for (std::filesystem's errors are
        // the system's: errno on POSIX, mapped to it on Windows).
        int ErrnoOf(const std::error_code& ec)
        {
            if (!ec)
                return 0;
            std::error_condition condition = ec.default_error_condition();
            return condition.category() == std::generic_category() ? condition.value() : EIO;
        }

        bool IsRoot(const fs::path& path)
        {
            return path.has_root_path() && Normal(path) == path.root_path();
        }

        Result Refusal(Error error, std::string message, const fs::path& path)
        {
            Result r;
            r.error = error;
            r.message = std::move(message);
            r.file = path.string();
            return r;
        }

        std::string Lower(std::string_view text)
        {
            std::string out(text);
            std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            return out;
        }

        bool IsFat(std::string_view fileSystem)
        {
            std::string fs = Lower(fileSystem);
            return fs == "vfat" || fs == "msdos" || fs == "fat" || fs == "fat32" || fs == "fat16";
        }

        // The name a player knows the format by.
        std::string FormatName(std::string_view fileSystem)
        {
            std::string fs = Lower(fileSystem);
            if (IsFat(fs))
                return "FAT32";
            if (fs == "exfat")
                return "exFAT";
            if (fs == "fuseblk" || fs.starts_with("ntfs"))
                return "NTFS";
            return std::string(fileSystem);
        }

        // Why a folder can't be read, and what to do.
        std::string CantRead(const fs::path& path, int error, bool macOS)
        {
            if (macOS && error == EPERM)
                return std::format("macOS doesn't let SpeedBreaker use {}. Allow it in System Settings > Privacy & Security > "
                    "Files and Folders, or choose another folder.", path.string());
            return std::format("SpeedBreaker can't read {} ({}). Choose another folder.", path.string(),
                strerror(error ? error : EACCES));
        }

        // The refusals every game folder gets, picked or exact. Ok if none.
        // `picked` is the folder the player chose (gameDir, or the folder
        // gameDir would be made in): the messages name it, not a subfolder
        // the player never saw.
        Result Refuse(const fs::path& gameDir, const fs::path& picked, const PlacementContext& context)
        {
            if (IsRoot(gameDir))
                return Refusal(Error::DestinationNotWritable, std::format("{} is the system's root folder. Choose a folder in "
                    "your home folder or on a drive.", gameDir.string()), gameDir);
            if (!context.home.empty() && gameDir == Normal(context.home))
                return Refusal(Error::DestinationNotWritable, std::format("{} is your home folder itself. Choose a folder in "
                    "it instead.", gameDir.string()), gameDir);
            if (fs::path bundle = Normal(context.bundle); !bundle.empty() && IsAtOrInside(gameDir, bundle))
            {
                const fs::path& named = IsAtOrInside(picked, bundle) ? picked : gameDir;
                std::string message;
                if (bundle.extension() == ".app")
                    message = std::format("{} is inside the SpeedBreaker app. The game can't go there: an update replaces the "
                        "app, and files added to it stop macOS from opening it. Choose another folder.", named.string());
                else if (named == bundle)
                    message = std::format("{} is SpeedBreaker's own folder. The game can't go there: updating or removing "
                        "SpeedBreaker replaces that folder. Choose another folder.", named.string());
                else
                    message = std::format("{} is inside SpeedBreaker's own folder, {}. The game can't go there: updating or "
                        "removing SpeedBreaker replaces that folder. Choose another folder.", named.string(), bundle.string());
                return Refusal(Error::DestinationNotWritable, std::move(message), gameDir);
            }
            for (const fs::path& install : context.installs)
            {
                if (install.empty())
                    continue;
                fs::path base = Normal(install);
                if (gameDir != base && IsAtOrInside(gameDir, base))
                {
                    // The player's folder as they named it; when that is the
                    // install folder itself (the game would go in a new
                    // folder inside it), it isn't "inside" itself.
                    const fs::path& named = IsAtOrInside(picked, base) ? picked : gameDir;
                    return Refusal(Error::DestinationNotWritable, named == base
                        ? std::format("{} is the game's own install folder. Choose a folder outside it.", base.string())
                        : std::format("{} is inside the game installed in {}. Choose a folder outside it.", named.string(),
                            base.string()), gameDir);
                }
            }
            return {};
        }

        // The file system's rules and notes, for a game folder that passed
        // Refuse. `picked`: as for Refuse.
        Result FileSystemRules(const fs::path& gameDir, const fs::path& picked, const FolderFacts& facts,
            const PlacementContext& context, std::vector<std::string>& notes)
        {
            if (context.macOS)
            {
                // iCloud Drive (and Desktop and Documents, when the Mac keeps
                // them there: the system says so), or a cloud service's
                // folder (Dropbox, OneDrive, Google Drive...).
                bool cloud = facts.cloud;
                if (!context.home.empty())
                    for (const char* sub : { "Library/Mobile Documents", "Library/CloudStorage" })
                        cloud = cloud || IsAtOrInside(gameDir, Normal(context.home) / sub);
                if (cloud)
                    return Refusal(Error::DestinationNotWritable, std::format("{} is kept in iCloud Drive or another cloud "
                        "service. The game can't go there: macOS would upload its files and can remove the copies on this Mac "
                        "to free space, while the game reads them as it plays. Choose a folder outside it.", picked.string()),
                        gameDir);
            }
            if (IsMemoryFileSystem(facts.fileSystem))
                return Refusal(Error::DestinationNotWritable, std::format("{} is kept in memory ({}), which is emptied when "
                    "{} restarts. Choose a folder on a drive.", picked.string(), facts.fileSystem,
                    context.steamOS ? "SteamOS" : "the computer"), gameDir);
            constexpr uint64_t kFatFileLimit = 1ull << 32;  // FAT32: a file is at most 4 GB - 1 byte
            if (IsFat(facts.fileSystem) && context.largestFile >= kFatFileLimit)
                return Refusal(Error::DestinationNotWritable, std::format("{} is on a drive formatted FAT32, which can't hold "
                    "files of 4 GB or more, and one of the game's files is {}. Choose a drive formatted exFAT, NTFS or ext4.",
                    picked.string(), FormatSize(context.largestFile)), gameDir);
            if (context.macOS && Lower(facts.fileSystem) == "ntfs")
                return Refusal(Error::DestinationNotWritable, std::format("{} is on an NTFS drive, which macOS can only read. "
                    "Choose another drive, or one formatted exFAT.", picked.string()), gameDir);
            if (IsWindowsFileSystem(facts.fileSystem))
                notes.push_back(context.steamOS
                    ? std::format("This drive is formatted for Windows ({}). That's fine for the game's files, but SteamOS "
                        "doesn't mount such drives in Game Mode by itself, so the game would start only in Desktop Mode.",
                        FormatName(facts.fileSystem))
                    : std::format("This drive is formatted for Windows ({}). That's fine: the game only reads its files.",
                        FormatName(facts.fileSystem)));
            if (context.macOS)
            {
                // Folders macOS asks about before an app may use them (the
                // open panel only grants this run).
                bool asks = gameDir.parent_path() != "/Volumes" && IsAtOrInside(gameDir, "/Volumes");
                if (!context.home.empty())
                    for (const char* sub : { "Desktop", "Documents", "Downloads" })
                        asks = asks || IsAtOrInside(gameDir, Normal(context.home) / sub);
                if (asks)
                    notes.push_back("macOS may ask once whether SpeedBreaker can use this folder when the game starts: choose "
                                    "Allow.");
            }
            return {};
        }

        // A folder's state from its status and a listing that stops at the
        // first visible entry. `hidden`: an Empty folder holds hidden files.
        FolderState Classify(const fs::path& path, int& error, bool* hidden = nullptr)
        {
            error = 0;
            if (hidden)
                *hidden = false;
            std::error_code ec;
            fs::file_status status = fs::status(path, ec);
            if (status.type() == fs::file_type::not_found)
            {
                error = ec ? ErrnoOf(ec) : ENOENT;
                return FolderState::Missing;
            }
            if (ec)
            {
                error = ErrnoOf(ec);
                return error == EACCES || error == EPERM ? FolderState::Unreadable : FolderState::Missing;
            }
            if (!fs::is_directory(status))
                return FolderState::NotAFolder;
            fs::directory_iterator it(path, ec);
            if (ec)
            {
                error = ErrnoOf(ec);
                return FolderState::Unreadable;
            }
            bool visible = false, anyHidden = false;
            for (fs::directory_iterator end; !ec && it != end; it.increment(ec))
            {
                const fs::path name = it->path().filename();
                if (!name.empty() && name.native()[0] == fs::path::value_type('.'))
                    anyHidden = true;
                else
                {
                    visible = true;
                    break;
                }
            }
            if (ReadInstallMarker(path))
                return FolderState::Install;
            if (hidden && !visible)
                *hidden = anyHidden;
            return visible ? FolderState::Other : FolderState::Empty;
        }

        // `path` is on another device than `parent`: the top of a mounted
        // drive. (Windows: a drive's top is its root, IsRoot.)
        bool OnAnotherDevice(const fs::path& path, const fs::path& parent)
        {
#ifndef _WIN32
            struct stat self{}, above{};
            return stat(path.c_str(), &self) == 0 && stat(parent.c_str(), &above) == 0 && self.st_dev != above.st_dev;
#else
            (void)path;
            (void)parent;
            return false;
#endif
        }

        // A new file or folder can be made in `folder`. (Windows: access()
        // ignores ACLs there; CheckDestination is left to tell.)
        bool CanWrite(const fs::path& folder)
        {
#ifndef _WIN32
            return access(folder.c_str(), W_OK | X_OK) == 0;
#else
            (void)folder;
            return true;
#endif
        }

        // macOS: Install may move `folder` aside (rename it). Not with a
        // "deny delete" in its ACL (fresh ~/Music, ~/Pictures, ~/Movies and
        // ~/Desktop have "group:everyone deny delete") or an immutable or
        // append-only flag. Any deny-delete entry counts, whoever it names:
        // the cost of a wrong guess is only a subfolder.
        bool CanMoveAside(const fs::path& folder)
        {
#if defined(__APPLE__) && !TARGET_OS_IOS
            struct stat st{};
            if (stat(folder.c_str(), &st) == 0 && (st.st_flags & (UF_IMMUTABLE | SF_IMMUTABLE | UF_APPEND | SF_APPEND)))
                return false;
            acl_t acl = acl_get_file(folder.c_str(), ACL_TYPE_EXTENDED);
            if (!acl)
                return true;
            bool denied = false;
            acl_entry_t entry;
            for (int which = ACL_FIRST_ENTRY; !denied && acl_get_entry(acl, which, &entry) == 0; which = ACL_NEXT_ENTRY)
            {
                acl_tag_t tag;
                acl_permset_t permissions;
                denied = acl_get_tag_type(entry, &tag) == 0 && tag == ACL_EXTENDED_DENY
                    && acl_get_permset(entry, &permissions) == 0 && acl_get_perm_np(permissions, ACL_DELETE) == 1;
            }
            acl_free(acl);
            return !denied;
#else
            (void)folder;
            return true;
#endif
        }

        // The nearest folder at or above `path` that is there.
        fs::path NearestExisting(fs::path path)
        {
            std::error_code ec;
            while (!path.empty() && !fs::exists(path, ec) && path != path.parent_path())
                path = path.parent_path();
            return path;
        }

#if !defined(__APPLE__) && !defined(_WIN32)
        // /proc/self/mountinfo escapes spaces and the like as \ooo.
        std::string Unescape(std::string_view text)
        {
            std::string out;
            for (size_t i = 0; i < text.size(); i++)
            {
                if (text[i] == '\\' && i + 3 < text.size())
                {
                    int value = 0;
                    bool octal = true;
                    for (size_t j = 1; j <= 3; j++)
                    {
                        if (text[i + j] < '0' || text[i + j] > '7')
                        {
                            octal = false;
                            break;
                        }
                        value = value * 8 + (text[i + j] - '0');
                    }
                    if (octal)
                    {
                        out += char(value);
                        i += 3;
                        continue;
                    }
                }
                out += text[i];
            }
            return out;
        }
#endif

        std::string FileSystemOf(const fs::path& path)
        {
#if defined(_WIN32)
            (void)path;
            return {};
#elif defined(__APPLE__)
            struct statfs sfs{};
            if (statfs(path.c_str(), &sfs) != 0)
                return {};
            return sfs.f_fstypename;
#else
            // The longest mount point holding `path`.
            std::ifstream mounts("/proc/self/mountinfo");
            std::string line, best, type;
            while (std::getline(mounts, line))
            {
                // id parent major:minor root mount-point options [optional...] - type source super-options
                size_t dash = line.find(" - ");
                if (dash == std::string::npos)
                    continue;
                std::string_view head(line.data(), dash);
                size_t field = 0, at = 0;
                std::string_view mountPoint;
                while (at <= head.size() && field <= 4)
                {
                    size_t end = head.find(' ', at);
                    if (end == std::string_view::npos)
                        end = head.size();
                    if (field == 4)
                        mountPoint = head.substr(at, end - at);
                    field++;
                    at = end + 1;
                }
                std::string point = Unescape(mountPoint);
                if (point.empty() || !IsAtOrInside(path, point) || point.size() < best.size())
                    continue;
                std::string_view tail(line.data() + dash + 3, line.size() - dash - 3);
                best = point;
                type = std::string(tail.substr(0, tail.find(' ')));
            }
            return type;
#endif
        }

        // macOS: is `path` (or, if it isn't there, the nearest folder above it)
        // in iCloud Drive? A metadata read: no privacy prompt, no download.
        bool InICloud(fs::path path)
        {
#if defined(__APPLE__) && !TARGET_OS_IOS
            path = NearestExisting(path);
            std::string text = path.string();
            CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(text.data()),
                CFIndex(text.size()), true);
            if (!url)
                return false;
            CFTypeRef value = nullptr;
            bool ubiquitous = CFURLCopyResourcePropertyForKey(url, kCFURLIsUbiquitousItemKey, &value, nullptr) && value
                && CFGetTypeID(value) == CFBooleanGetTypeID() && CFBooleanGetValue(static_cast<CFBooleanRef>(value));
            if (value)
                CFRelease(value);
            CFRelease(url);
            return ubiquitous;
#else
            (void)path;
            return false;
#endif
        }

        bool IsSteamOS()
        {
#if defined(__linux__)
            std::ifstream release("/etc/os-release");
            std::string line;
            while (std::getline(release, line))
                if (line == "ID=steamos" || line == "ID=\"steamos\"")
                    return true;
#endif
            return false;
        }

        fs::path& ProgramPath()
        {
            static fs::path path;
            return path;
        }

        // Where the running program is, without argv[0].
        fs::path ExecutablePath()
        {
#if defined(_WIN32)
            return {};  // argv[0] (SetProgramPath)
#elif defined(__APPLE__)
            char buffer[MAXPATHLEN];
            uint32_t size = sizeof(buffer);
            if (_NSGetExecutablePath(buffer, &size) == 0)
                return Resolved(buffer);
            return {};
#else
            std::error_code ec;
            fs::path exe = fs::read_symlink("/proc/self/exe", ec);
            // Started through the bundled C library's loader, that's the
            // loader, not the game.
            if (ec || exe.filename().string().starts_with("ld-"))
                return {};
            return Normal(exe);
#endif
        }
    }

    std::filesystem::path SubfolderName(bool driveTop)
    {
        return driveTop ? fs::path(kUserFolderName) / "game" : fs::path(kPickedSubfolder);
    }

    std::filesystem::path ResolvedFolder(const std::filesystem::path& path)
    {
        return path.empty() ? path : Resolved(path);
    }

    bool SameFolder(const std::filesystem::path& a, const std::filesystem::path& b)
    {
        return !a.empty() && !b.empty() && Resolved(a) == Resolved(b);
    }

    bool IsAtOrInside(const std::filesystem::path& path, const std::filesystem::path& base)
    {
        fs::path p = Normal(path), b = Normal(base);
        if (p.empty() || b.empty())
            return false;
        fs::path relative = p.lexically_relative(b);
        if (relative.empty())
            return false;
        return relative == "." || *relative.begin() != "..";
    }

    bool IsWindowsFileSystem(std::string_view fileSystem)
    {
        std::string fs = Lower(fileSystem);
        return IsFat(fs) || fs == "exfat" || fs == "ntfs" || fs == "ntfs3" || fs == "fuseblk";
    }

    bool IsMemoryFileSystem(std::string_view fileSystem)
    {
        std::string fs = Lower(fileSystem);
        return fs == "tmpfs" || fs == "ramfs";
    }

    FolderChoice PlaceInPicked(const FolderFacts& picked, const PlacementContext& context)
    {
        FolderChoice choice;
        const fs::path path = Normal(picked.path);
        choice.picked = path;
        choice.gameDir = path;
        auto refuse = [&](Error error, std::string message, const fs::path& file) {
            choice.result = Refusal(error, std::move(message), file);
            return choice;
        };

        switch (picked.state)
        {
        case FolderState::Missing:
            return refuse(Error::NotFound, std::format("{} isn't there any more. If it's on a drive, connect it, or choose "
                "another folder.", path.string()), path);
        case FolderState::NotAFolder:
            return refuse(Error::DestinationNotEmpty, std::format("{} is a file. Choose a folder.", path.string()), path);
        case FolderState::Unreadable:
            return refuse(Error::DestinationNotWritable, CantRead(path, picked.error, context.macOS), path);
        default:
            break;
        }
        if (IsRoot(path))
            return refuse(Error::DestinationNotWritable, std::format("{} is the system's root folder. Choose a folder in your "
                "home folder or on a drive.", path.string()), path);

        // In the folder itself, or in a new folder inside it.
        const bool home = !context.home.empty() && path == Normal(context.home);
        if (picked.driveTop)
            choice.subfolder = SubfolderReason::DriveTop;
        else if (home)
            choice.subfolder = SubfolderReason::Home;
        else if (picked.state == FolderState::Install)
            choice.subfolder = SubfolderReason::None;
        else if (picked.state == FolderState::Empty)
            choice.subfolder = !picked.parentWritable ? SubfolderReason::ParentReadOnly
                : picked.hidden && !picked.movable ? SubfolderReason::KeptInPlace
                                                   : SubfolderReason::None;
        else
            choice.subfolder = SubfolderReason::NotEmpty;
        if (choice.subfolder != SubfolderReason::None)
        {
            choice.gameDir = path / SubfolderName(picked.driveTop);
            const SubfolderFacts& sub = picked.subfolder;
            // A link would take the game somewhere the rules below never
            // looked (SpeedBreaker's own folder, an install, iCloud Drive).
            if (sub.link)
            {
                // A link that leads nowhere resolves to itself: say where it
                // points (or that it points nowhere), not "a link to" itself.
                if (sub.dangling)
                    return refuse(Error::DestinationNotEmpty, sub.target.empty()
                        ? std::format("{} is a link that leads nowhere. Remove it, or choose another folder.",
                            choice.gameDir.string())
                        : std::format("{} is a link to {}, which isn't there. Remove the link, or choose another folder.",
                            choice.gameDir.string(), sub.target.string()), choice.gameDir);
                if (sub.state == FolderState::NotAFolder)
                    return refuse(Error::DestinationNotEmpty, std::format("{} is a link to a file{}{}. Move it, or choose "
                        "another folder.", choice.gameDir.string(), sub.target.empty() ? "" : ", ", sub.target.string()),
                        choice.gameDir);
                return refuse(Error::DestinationNotEmpty, std::format("{} is a link to {}. Choose that folder itself, or "
                    "another folder.", choice.gameDir.string(), sub.target.empty() ? std::string("another folder")
                    : sub.target.string()), choice.gameDir);
            }
            switch (sub.state)
            {
            case FolderState::NotAFolder:
                return refuse(Error::DestinationNotEmpty, std::format("{} is a file, so the game can't go in a folder of that "
                    "name. Move it, or choose another folder.", choice.gameDir.string()), choice.gameDir);
            case FolderState::Other:
                return refuse(Error::DestinationNotEmpty, std::format("{} already has a folder named \"{}\" with other files "
                    "in it. Empty it or move it, or choose another folder.", path.string(),
                    SubfolderName(picked.driveTop).string()), choice.gameDir);
            case FolderState::Unreadable:
                return refuse(Error::DestinationNotWritable, CantRead(choice.gameDir, sub.error, context.macOS),
                    choice.gameDir);
            default:
                break;  // new, empty, or a previous install (replaced)
            }
        }

        if (Result r = Refuse(choice.gameDir, path, context); !r.Ok())
        {
            choice.result = std::move(r);
            return choice;
        }
        choice.result = FileSystemRules(choice.gameDir, path, picked, context, choice.notes);
        if (!choice.result.Ok())
            return choice;
        // A folder that is there is replaced, or moved aside first: macOS
        // refuses either for a folder its owner can't write, after the copy.
        const bool there = choice.subfolder == SubfolderReason::None || picked.subfolder.state == FolderState::Empty
            || picked.subfolder.state == FolderState::Install;
        const bool writable = choice.subfolder == SubfolderReason::None ? picked.writable : picked.subfolder.writable;
        if (there && !writable)
            return refuse(Error::DestinationNotWritable, std::format("SpeedBreaker can't write to {}. Choose another folder.",
                choice.gameDir.string()), choice.gameDir);
        return choice;
    }

    FolderChoice PlaceExact(const FolderFacts& gameDir, bool parentExists, const PlacementContext& context)
    {
        FolderChoice choice;
        const fs::path path = Normal(gameDir.path);
        choice.picked = path;
        choice.gameDir = path;
        auto refuse = [&](Error error, std::string message) {
            choice.result = Refusal(error, std::move(message), path);
            return choice;
        };
        if (!parentExists && gameDir.state == FolderState::Missing)
            return refuse(Error::NotFound, std::format("{} isn't there now. If it's on a drive or SD card, connect it, or "
                "choose another folder.", path.string()));
        if (gameDir.state == FolderState::NotAFolder)
            return refuse(Error::DestinationNotEmpty, std::format("{} is a file. Choose a folder.", path.string()));
        if (gameDir.state == FolderState::Unreadable)
            return refuse(Error::DestinationNotWritable, CantRead(path, gameDir.error, context.macOS));
        if (Result r = Refuse(path, path, context); !r.Ok())
        {
            choice.result = std::move(r);
            return choice;
        }
        if (gameDir.driveTop)
            return refuse(Error::DestinationNotWritable, std::format("{} is a whole drive. Choose a folder on it instead.",
                path.string()));
        choice.result = FileSystemRules(path, path, gameDir, context, choice.notes);
        if (!choice.result.Ok())
            return choice;
        const bool usable = gameDir.state == FolderState::Empty || gameDir.state == FolderState::Install;
        if (usable && !gameDir.writable)
            return refuse(Error::DestinationNotWritable, std::format("SpeedBreaker can't write to {}. Choose another folder.",
                path.string()));
        if (gameDir.state == FolderState::Empty && gameDir.hidden && !gameDir.movable)
            return refuse(Error::DestinationNotWritable, std::format("macOS keeps {} in place, so the game can't take its "
                "place. Choose a new folder inside it, such as {}.", path.string(), (path / kPickedSubfolder).string()));
        return choice;
    }

    std::string SubfolderExplanation(const FolderChoice& choice)
    {
        std::string name = choice.picked.filename().string();
        if (name.empty())
            name = choice.picked.string();
        switch (choice.subfolder)
        {
        case SubfolderReason::NotEmpty:
            return std::format("{} already holds other files, so the game goes in a new folder inside it.", name);
        case SubfolderReason::DriveTop:
            return std::format("That's the top of the drive {}, so the game goes in a folder on it.", name);
        case SubfolderReason::Home:
            return "That's your home folder, so the game goes in a new folder inside it.";
        case SubfolderReason::ParentReadOnly:
            return std::format("SpeedBreaker can't write next to {}, so the game goes in a new folder inside it.", name);
        case SubfolderReason::KeptInPlace:
            return std::format("macOS keeps {} in place, so the game goes in a new folder inside it.", name);
        case SubfolderReason::None:
            break;
        }
        return {};
    }

    FolderFacts ProbeFolder(const std::filesystem::path& path)
    {
        FolderFacts facts;
        facts.path = Resolved(path);
        facts.state = Classify(facts.path, facts.error, &facts.hidden);
        facts.cloud = InICloud(facts.path);
        fs::path parent = facts.path.parent_path();
        if (facts.state == FolderState::Missing)
        {
            // What it would be made on (--dest makes missing folders).
            facts.fileSystem = FileSystemOf(NearestExisting(facts.path));
            facts.parentWritable = CanWrite(NearestExisting(parent));
            return facts;
        }
        if (facts.state == FolderState::NotAFolder)
            return facts;
        facts.driveTop = IsRoot(facts.path) || OnAnotherDevice(facts.path, parent);
        facts.parentWritable = CanWrite(parent);
        facts.fileSystem = FileSystemOf(facts.path);
        if (facts.state == FolderState::Unreadable)
            return facts;
        facts.writable = CanWrite(facts.path);
        if (facts.hidden)
            facts.movable = CanMoveAside(facts.path);

        // The folder the game would go in inside it, and whether a link on
        // the way leads elsewhere.
        const fs::path name = SubfolderName(facts.driveTop);
        const fs::path sub = facts.path / name;
        SubfolderFacts& inner = facts.subfolder;
        fs::path at = facts.path;
        for (const fs::path& part : name)
        {
            at /= part;
            std::error_code ec;
            if (fs::is_symlink(fs::symlink_status(at, ec)))
            {
                inner.link = true;
                inner.target = Resolved(sub);
                // A link that leads nowhere: weakly_canonical stops at it and
                // gives the link back. Where it points instead (relative to
                // the link's folder), and the rest of the subfolder's name.
                std::error_code there;
                if (!fs::exists(at, there))
                {
                    inner.dangling = true;
                    std::error_code read;
                    fs::path to = fs::read_symlink(at, read);
                    inner.target.clear();
                    if (!read && !to.empty())
                        inner.target = Normal((to.is_absolute() ? to : at.parent_path() / to) / sub.lexically_relative(at));
                }
                break;
            }
        }
        inner.state = Classify(sub, inner.error);
        if (inner.state == FolderState::Empty || inner.state == FolderState::Install)
            inner.writable = CanWrite(sub);
        return facts;
    }

    PlacementContext CurrentPlacementContext(const Manifest& manifest)
    {
        PlacementContext context;
#ifdef _WIN32
        const char* home = std::getenv("USERPROFILE");  // not $HOME: Git Bash and MSYS2 set their own
#else
        const char* home = std::getenv("HOME");
#endif
        if (home && *home)
            context.home = Resolved(home);
        if (fs::path bundle = BundleFolder(); !bundle.empty())
            context.bundle = Resolved(bundle);
        context.installs.push_back(Resolved(DefaultInstallPath()));
        if (std::optional<fs::path> recorded = RecordedInstallPath())
            context.installs.push_back(Resolved(*recorded));
        for (const ManifestFile& file : manifest.files)
            context.largestFile = std::max(context.largestFile, file.size);
#if defined(__APPLE__) && !TARGET_OS_IOS
        context.macOS = true;
#endif
        context.steamOS = IsSteamOS();
        return context;
    }

    FolderChoice CheckPickedFolder(const std::filesystem::path& picked, const Manifest& manifest)
    {
        FolderChoice choice = PlaceInPicked(ProbeFolder(picked), CurrentPlacementContext(manifest));
        if (choice.result.Ok())
            choice.result = CheckDestination(choice.gameDir, &choice.info, manifest);
        return choice;
    }

    FolderChoice CheckExactFolder(const std::filesystem::path& gameDir, bool mayCreateParents, const Manifest& manifest)
    {
        FolderFacts facts = ProbeFolder(gameDir);
        std::error_code ec;
        bool parentExists = mayCreateParents || fs::is_directory(facts.path.parent_path(), ec);
        FolderChoice choice = PlaceExact(facts, parentExists, CurrentPlacementContext(manifest));
        if (choice.result.Ok())
            choice.result = CheckDestination(choice.gameDir, &choice.info, manifest);
        return choice;
    }

    void SetProgramPath(const char* argv0)
    {
        // A bare name came from $PATH: the platform knows better.
#ifdef _WIN32
        const bool hasFolder = argv0 && std::strpbrk(argv0, "/\\");
#else
        const bool hasFolder = argv0 && std::strchr(argv0, '/');
#endif
        if (hasFolder)
            ProgramPath() = Resolved(argv0);
        else
            ProgramPath() = ExecutablePath();
    }

    std::filesystem::path BundleFolder()
    {
        fs::path program = ProgramPath().empty() ? ExecutablePath() : ProgramPath();
        if (program.empty())
            return {};
        fs::path dir = program.parent_path();
#ifdef __APPLE__
        for (fs::path p = dir; !p.empty() && p != p.root_path(); p = p.parent_path())
            if (p.extension() == ".app")
                return p;
#else
        // The SteamOS bundle keeps speedbreaker.sh at its top; the program is
        // beside it, or in its lib/.
        std::error_code ec;
        if (dir.filename() == "lib" && fs::exists(dir.parent_path() / "speedbreaker.sh", ec))
            return dir.parent_path();
#endif
        return dir;
    }

    std::string FolderPickerHint(bool gameMode, bool macOS, bool noPickerApp, const std::string& program,
        const std::string& image)
    {
        if (macOS)
            return std::format("The folder picker couldn't open. Install from Terminal instead: {} --install {} --dest <folder>",
                program, image);
        if (gameMode)
            return std::format("The folder picker can't open in Game Mode. Choose a folder in Desktop Mode, or install from a "
                "terminal: {} --install {} --dest <folder>", program, image);
        if (noPickerApp)
            return std::format("The folder picker couldn't open (it needs a file chooser portal or zenity). Install from a "
                "terminal instead: {} --install {} --dest <folder>", program, image);
        return std::format("The folder picker couldn't open. Install from a terminal instead: {} --install {} --dest <folder>",
            program, image);
    }

    RecordedInstall CheckRecordedInstall(const Manifest& manifest)
    {
        RecordedInstall recorded;
        std::optional<fs::path> path = RecordedInstallPath();
        if (!path)
            return recorded;
        recorded.path = *path;
        std::error_code ec;
        fs::file_status status = fs::status(*path, ec);
        if (status.type() == fs::file_type::not_found || ec)
        {
            recorded.error = ec ? ErrnoOf(ec) : ENOENT;
            recorded.state = recorded.error == EACCES || recorded.error == EPERM ? RecordedState::NotAllowed
                                                                                : RecordedState::Missing;
            return recorded;
        }
        if (IsUsableInstall(*path, true, manifest))
        {
            recorded.state = RecordedState::Usable;
            return recorded;
        }
        // There, but not usable: unreadable (macOS privacy settings can
        // allow the status and refuse the listing), or not a current install.
        fs::directory_iterator listing(*path, ec);
        recorded.error = ErrnoOf(ec);
        recorded.state = recorded.error == EACCES || recorded.error == EPERM ? RecordedState::NotAllowed
                                                                            : RecordedState::NotCurrent;
        return recorded;
    }

    std::string DescribeRecordedInstall(const RecordedInstall& recorded, const std::string& shown, bool macOS)
    {
        switch (recorded.state)
        {
        case RecordedState::Missing:
            return std::format("The game is installed in {}, which isn't there now. If it's on a drive or SD card, connect it "
                "and start SpeedBreaker again, or install the game again here.", shown);
        case RecordedState::NotAllowed:
            return macOS
                ? std::format("SpeedBreaker isn't allowed to read {}, where the game is installed. Allow it in System "
                    "Settings > Privacy & Security > Files and Folders and start SpeedBreaker again, or install the game "
                    "again here.", shown)
                : std::format("SpeedBreaker can't read {}, where the game is installed ({}). Fix its permissions and start "
                    "SpeedBreaker again, or install the game again here.", shown, strerror(recorded.error ? recorded.error : EACCES));
        case RecordedState::NotCurrent:
            return std::format("The game in {} is incomplete or from an older version. Install it again here: it goes back "
                "into the same folder.", shown);
        case RecordedState::None:
        case RecordedState::Usable:
            break;
        }
        return {};
    }
}
