// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See disc_source.h.
//
// The XDVDFS walk (partition offsets, volume descriptor, the per-directory
// binary tree) is adapted from Unleashed Recompiled
// (https://github.com/hedge-dev/UnleashedRecomp), install/iso_file_system.cpp,
// GPL-3.0-or-later, which references Xenia's src/xenia/vfs/devices/disc_image_device.cc:
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2023 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
// Modified for SpeedBreaker: pread in chunks instead of mapping the image;
// every offset, size and name is bounds-checked; loops, repeated nodes and
// runaway trees (in count or in total table size) are rejected instead of
// followed; a truncated image is reported as such (the usual way a 7.8 GB
// copy goes wrong).
#include "disc_source.h"
#include "manifest.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <format>
#include <set>
#include <unordered_set>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace install
{
    namespace
    {
        namespace fs = std::filesystem;
        static_assert(sizeof(off_t) == 8, "large file offsets are required");

        constexpr uint64_t kSectorSize = 2048;
        // Where the game partition starts: plain XISO (0), older rips
        // (0xFB20, 0x20600), XGD3 (0x2080000) and XGD2 (0xFD90000). The
        // volume descriptor is sector 32 of the partition.
        constexpr uint64_t kPartitionOffsets[] = { 0x00000000, 0x0000FB20, 0x00020600, 0x02080000, 0x0FD90000 };
        constexpr char kMagic[] = "MICROSOFT*XBOX*MEDIA";
        constexpr size_t kMagicSize = sizeof(kMagic) - 1;
        constexpr uint8_t kAttributeDirectory = 0x10;

        // Garbage limits, far above any real disc (NFSMW: 3 directories, 49
        // files). A directory table is addressed by 16-bit dword offsets, so
        // it cannot usefully exceed 256 KB; allow some slack.
        constexpr uint32_t kMaxDirectoryTable = 1u << 20;
        constexpr size_t kMaxEntries = 200000;
        constexpr int kMaxDepth = 32;
        // All directory tables together. A disc with 50,000 files has a few
        // MB of them; without a total, a crafted image whose thousands of
        // directories each claim their own 1 MB table makes every open (and
        // the disc image scan, which opens each .iso it finds) read gigabytes.
        constexpr uint64_t kMaxDirectoryBytes = 64ull << 20;
        // Files and folders in an extracted disc, links followed.
        constexpr size_t kMaxFolderEntries = 200000;
        // Nothing on a disc lies past the end of a dual-layer DVD (XGD3 images
        // are 8.7 GB). Data beyond the end of a smaller file means the copy was
        // cut short; beyond this, the directory is garbage.
        constexpr uint64_t kMaxDiscSize = 0x220000000;

        constexpr size_t kHashChunk = 4 << 20;

        uint16_t Le16(const uint8_t* p)
        {
            return uint16_t(p[0] | p[1] << 8);
        }

        uint32_t Le32(const uint8_t* p)
        {
            return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
        }

        uint32_t Be32(const uint8_t* p)
        {
            return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
        }

        std::string DisplayName(const fs::path& path)
        {
            std::string name = path.filename().string();
            return name.empty() ? path.string() : name;
        }

        Result Fail(Error error, std::string message, std::string file = {})
        {
            Result r;
            r.error = error;
            r.message = std::move(message);
            r.file = std::move(file);
            return r;
        }

        // pread until `size` bytes or the end of the file. Returns the bytes
        // read (fewer only at the end), or -1 with errno set.
        int64_t PreadAll(int fd, void* buffer, size_t size, uint64_t offset)
        {
            auto p = static_cast<uint8_t*>(buffer);
            size_t done = 0;
            while (done < size)
            {
                ssize_t n = pread(fd, p + done, std::min<size_t>(size - done, 1u << 30), off_t(offset + done));
                if (n < 0)
                {
                    if (errno == EINTR)
                        continue;
                    return -1;
                }
                if (n == 0)
                    break;
                done += size_t(n);
            }
            return int64_t(done);
        }

        bool ReadExact(int fd, uint64_t base, uint64_t limit, uint64_t offset, void* buffer, size_t size, std::string& error)
        {
            if (offset > limit || size > limit - offset)
            {
                error = "read past the end of the file";
                return false;
            }
            int64_t got = PreadAll(fd, buffer, size, base + offset);
            if (got < 0)
            {
                error = strerror(errno);
                return false;
            }
            if (uint64_t(got) < size)
            {
                error = "the data ends early (the file is truncated, or changed while being read)";
                return false;
            }
            return true;
        }

        void AdviseSequential(int fd)
        {
#ifdef POSIX_FADV_SEQUENTIAL
            posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#else
            (void)fd;
#endif
        }

        // Disc images.

        struct ImageFile
        {
            int fd = -1;
            uint64_t size = 0;

            ~ImageFile()
            {
                if (fd >= 0)
                    close(fd);
            }
        };

        class ImageReader : public FileReader
        {
        public:
            ImageReader(std::shared_ptr<const ImageFile> image, uint64_t offset, uint64_t size)
                : image(std::move(image)), offset(offset), size(size)
            {
            }

            bool Read(uint64_t at, void* buffer, size_t count, std::string& error) override
            {
                return ReadExact(image->fd, offset, size, at, buffer, count, error);
            }

        private:
            std::shared_ptr<const ImageFile> image;
            uint64_t offset, size;
        };

        class ImageSource : public DiscSource
        {
        public:
            ImageSource(std::shared_ptr<const ImageFile> image, fs::path imagePath, std::vector<DiscFile> list, bool cut)
                : image(std::move(image))
            {
                kind = SourceKind::Image;
                path = std::move(imagePath);
                files = std::move(list);
                truncated = cut;
            }

            std::unique_ptr<FileReader> Open(const DiscFile& file, std::string&) const override
            {
                return std::make_unique<ImageReader>(image, file.offset, file.size);
            }

        private:
            std::shared_ptr<const ImageFile> image;
        };

        // A name must be usable as a host path component. Control characters
        // and separators only show up when the tree walk has wandered into
        // data that isn't a directory, so they mark the image as damaged.
        bool ValidName(std::string_view name)
        {
            if (name.empty() || name == "." || name == "..")
                return false;
            for (unsigned char c : name)
                if (c < 0x20 || c == 0x7F || c == '/' || c == '\\')
                    return false;
            return true;
        }

        struct TreeWalk
        {
            const ImageFile& image;
            uint64_t partition;
            std::vector<DiscFile> files;
            size_t entries = 0;
            // The first directory whose table lies past the end of the image
            // (a truncated copy): skipped, so what precedes the cut still lists.
            std::string lostDirectory;

            Result Corrupt(const std::string& detail) const
            {
                return Fail(Error::CorruptImage, "its directory is unreadable (" + detail + ")");
            }

            static std::string Shown(const std::string& prefix)
            {
                return prefix.empty() ? "the root directory" : "directory \"" + prefix + "\"";
            }

            // Reads the directory tree. A directory is a table of entries
            // forming a binary tree: each entry holds dword offsets (within the
            // table) of its left and right siblings, its data's sector and size,
            // attributes and name.
            Result Walk(uint32_t rootSector, uint32_t rootSize)
            {
                struct Directory
                {
                    uint64_t offset;
                    uint32_t size;
                    std::string prefix;
                    int depth;
                };
                std::vector<Directory> pending{ { partition + rootSector * kSectorSize, rootSize, "", 0 } };
                std::unordered_set<uint64_t> tables;
                std::vector<uint8_t> table;
                std::vector<bool> visited;
                std::vector<uint32_t> nodes;
                uint64_t tableBytes = 0;

                while (!pending.empty())
                {
                    Directory dir = std::move(pending.back());
                    pending.pop_back();
                    if (dir.size < 14 || dir.size > kMaxDirectoryTable)
                        return Corrupt(std::format("{} has size {}", Shown(dir.prefix), dir.size));
                    if (!tables.insert(dir.offset).second)
                        return Corrupt(std::format("{} loops back to another directory", Shown(dir.prefix)));
                    if (dir.offset + dir.size > kMaxDiscSize)
                        return Corrupt(std::format("{} is at byte {}, past the end of any disc", Shown(dir.prefix), dir.offset));
                    if (dir.offset + dir.size > image.size)
                    {
                        if (lostDirectory.empty())
                            lostDirectory = Shown(dir.prefix);
                        continue;
                    }
                    if ((tableBytes += dir.size) > kMaxDirectoryBytes)
                        return Corrupt(std::format("its directory tables add up to more than {}", FormatSize(kMaxDirectoryBytes)));

                    table.resize(dir.size);
                    std::string error;
                    if (!ReadExact(image.fd, 0, image.size, dir.offset, table.data(), table.size(), error))
                        return Fail(Error::ReadError, "cannot read its directory: " + error);

                    visited.assign(dir.size / 4 + 1, false);
                    nodes.assign(1, 0);
                    while (!nodes.empty())
                    {
                        uint32_t at = nodes.back();
                        nodes.pop_back();
                        if (at + 14 > dir.size)
                            return Corrupt(std::format("an entry at {} is outside {}", at, Shown(dir.prefix)));
                        if (visited[at / 4])
                            return Corrupt(std::format("the entry at {} in {} is reached twice", at, Shown(dir.prefix)));
                        visited[at / 4] = true;

                        const uint8_t* e = &table[at];
                        uint16_t left = Le16(e + 0), right = Le16(e + 2);
                        // An empty directory's table is 0xFF filler.
                        if (left == 0xFFFF && right == 0xFFFF)
                        {
                            if (at == 0)
                                continue;
                            return Corrupt(std::format("the entry at {} in {} is filler", at, Shown(dir.prefix)));
                        }
                        uint32_t sector = Le32(e + 4), size = Le32(e + 8);
                        uint8_t attributes = e[12], nameLength = e[13];
                        if (at + 14 + nameLength > dir.size)
                            return Corrupt(std::format("a name runs past the end of {}", Shown(dir.prefix)));
                        std::string_view name(reinterpret_cast<const char*>(e + 14), nameLength);
                        if (!ValidName(name))
                            return Corrupt(std::format("an invalid name in {}", Shown(dir.prefix)));
                        if (++entries > kMaxEntries)
                            return Corrupt("too many entries");
                        if (left)
                            nodes.push_back(uint32_t(left) * 4);
                        if (right)
                            nodes.push_back(uint32_t(right) * 4);

                        std::string relative = dir.prefix + std::string(name);
                        uint64_t data = partition + sector * kSectorSize;
                        if (attributes & kAttributeDirectory)
                        {
                            if (size == 0)
                                continue;   // empty directory
                            if (dir.depth + 1 > kMaxDepth)
                                return Corrupt("directories nested too deeply");
                            pending.push_back({ data, size, relative + "/", dir.depth + 1 });
                        }
                        else
                        {
                            if (data + size > kMaxDiscSize)
                                return Corrupt(std::format("\"{}\" is at byte {}, past the end of any disc", relative, data));
                            files.push_back({ std::move(relative), size, data });
                        }
                    }
                }

                std::sort(files.begin(), files.end(), [](const DiscFile& a, const DiscFile& b) { return a.path < b.path; });
                for (size_t i = 1; i < files.size(); i++)
                    if (files[i].path == files[i - 1].path)
                        return Corrupt("\"" + files[i].path + "\" is listed twice");
                return {};
            }
        };

        bool HasMagicAt(const ImageFile& image, uint64_t offset)
        {
            char magic[kMagicSize];
            std::string error;
            return offset + kMagicSize <= image.size
                && ReadExact(image.fd, 0, image.size, offset, magic, kMagicSize, error)
                && memcmp(magic, kMagic, kMagicSize) == 0;
        }

        // Why a file without an XDVDFS partition isn't usable, in terms of
        // what the user probably picked.
        Result Unrecognized(const ImageFile& image, const fs::path& path)
        {
            std::string name = DisplayName(path);
            uint8_t head[8] = {};
            std::string error;
            ReadExact(image.fd, 0, image.size, 0, head, std::min<uint64_t>(sizeof(head), image.size), error);
            uint8_t iso9660[5] = {};
            bool dvd = image.size >= 0x8006 && ReadExact(image.fd, 0, image.size, 0x8001, iso9660, 5, error)
                && memcmp(iso9660, "CD001", 5) == 0;

            if (!memcmp(head, "CON ", 4) || !memcmp(head, "LIVE", 4) || !memcmp(head, "PIRS", 4))
                return Fail(Error::UnsupportedFormat, name + " is an Xbox 360 content package (Games on Demand or Xbox Live), "
                    "not a disc image. Use an image of the game disc (.iso) or its extracted files.");
            if (!memcmp(head, "XEX2", 4))
                return Fail(Error::NotADiscImage, name + " is an Xbox 360 executable. Choose the folder that holds the "
                    "extracted disc (default.xex, Movies and NFS), or a disc image.");
            if (!memcmp(head, "PK\x03\x04", 4) || !memcmp(head, "7z\xBC\xAF", 4) || !memcmp(head, "Rar!", 4))
                return Fail(Error::NotADiscImage, name + " is a compressed archive. Extract it first, then choose the .iso inside.");
            // An XGD2 disc starts with a DVD-video partition (ISO 9660); the game
            // partition begins at 0xFD90000. A file that has the former but ends
            // before the latter was cut short; a large one is a video-only rip.
            if (dvd && image.size < kPartitionOffsets[4] + 33 * kSectorSize)
                return Fail(Error::TruncatedImage, std::format("{} is incomplete: it ends after {}, before the game data "
                    "starts. Copy it again, or make a new backup from your disc.", name, FormatSize(image.size)));
            if (dvd)
                return Fail(Error::UnsupportedFormat, name + " is a DVD image without an Xbox 360 game partition "
                    "(perhaps only the video partition was copied). Dump the whole disc.");
            return Fail(Error::NotADiscImage, name + " is not an Xbox 360 disc image.");
        }

        Result OpenImage(const fs::path& path, std::unique_ptr<DiscSource>& source, const OpenOptions& options)
        {
            auto image = std::make_shared<ImageFile>();
            image->fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
            if (image->fd < 0)
                return Fail(Error::NotFound, std::format("Cannot open {}: {}.", path.string(), strerror(errno)), path.string());
            struct stat st;
            if (fstat(image->fd, &st) != 0)
                return Fail(Error::ReadError, std::format("Cannot read {}: {}.", path.string(), strerror(errno)), path.string());
            image->size = uint64_t(st.st_size);
            AdviseSequential(image->fd);

            // Try each partition offset with the magic, first to last, and keep
            // the first whose tree reads cleanly. A later offset can hold the
            // magic by accident inside an earlier partition's files, so only a
            // tree that parses counts.
            std::string name = DisplayName(path);
            Result firstFailure;
            for (uint64_t partition : kPartitionOffsets)
            {
                uint64_t volume = partition + 32 * kSectorSize;
                if (!HasMagicAt(*image, volume))
                    continue;

                uint8_t descriptor[28];
                std::string error;
                if (!ReadExact(image->fd, 0, image->size, volume, descriptor, sizeof(descriptor), error))
                {
                    if (firstFailure.Ok())
                        firstFailure = Fail(Error::TruncatedImage, name + " is incomplete: it ends inside its volume descriptor.");
                    continue;
                }
                TreeWalk walk{ *image, partition, {}, 0, {} };
                Result r = walk.Walk(Le32(descriptor + 20), Le32(descriptor + 24));
                if (!r.Ok())
                {
                    if (firstFailure.Ok())
                    {
                        firstFailure = r;
                        firstFailure.message = name + (r.error == Error::CorruptImage ? " is damaged: " : ": ") + r.message + ".";
                    }
                    continue;
                }
                // Every file and directory must lie inside the image. When one
                // doesn't, the copy was cut short: say what was lost and how big
                // the image should be.
                const DiscFile* cut = nullptr;
                uint64_t needed = 0;
                for (const DiscFile& f : walk.files)
                {
                    if (f.offset + f.size > needed)
                        needed = f.offset + f.size;
                    if (f.offset + f.size > image->size && (!cut || f.offset < cut->offset))
                        cut = &f;
                }
                bool incomplete = cut || !walk.lostDirectory.empty();
                if (walk.files.empty() && !incomplete)
                {
                    if (firstFailure.Ok())
                        firstFailure = Fail(Error::NotADiscImage, name + " is an empty Xbox disc image.");
                    continue;
                }
                Result result;
                if (incomplete)
                {
                    result = Fail(Error::TruncatedImage, cut
                        ? std::format("{} is incomplete: it is {} bytes, but its files need at least {} ({} is cut off). ",
                            name, image->size, needed, cut->path)
                        : std::format("{} is incomplete: it is {} bytes, and its {} lies beyond that. ",
                            name, image->size, walk.lostDirectory), cut ? cut->path : std::string());
                    // FAT32 stops at 4 GB - 1, the classic way a copy loses its end.
                    result.message += image->size == 0xFFFFFFFFull
                        ? "The drive it was copied to is probably formatted FAT32, which cannot hold files over 4 GB; use exFAT or copy it elsewhere."
                        : "Copy it again, or make a new backup from your disc.";
                    if (!options.allowTruncated || walk.files.empty())
                        return result;
                }
                source = std::make_unique<ImageSource>(image, path, std::move(walk.files), incomplete);
                return result;
            }
            return firstFailure.Ok() ? Unrecognized(*image, path) : firstFailure;
        }

        // Extracted folders.

        class FolderReader : public FileReader
        {
        public:
            FolderReader(int fd, uint64_t size) : fd(fd), size(size) {}

            ~FolderReader() override
            {
                close(fd);
            }

            bool Read(uint64_t at, void* buffer, size_t count, std::string& error) override
            {
                return ReadExact(fd, 0, size, at, buffer, count, error);
            }

        private:
            int fd;
            uint64_t size;
        };

        class FolderSource : public DiscSource
        {
        public:
            FolderSource(fs::path root, std::vector<DiscFile> list)
            {
                kind = SourceKind::Folder;
                path = std::move(root);
                files = std::move(list);
            }

            std::unique_ptr<FileReader> Open(const DiscFile& file, std::string& error) const override
            {
                fs::path host = path / fs::path(file.path);
                int fd = open(host.c_str(), O_RDONLY | O_CLOEXEC);
                if (fd < 0)
                {
                    error = strerror(errno);
                    return nullptr;
                }
                AdviseSequential(fd);
                return std::make_unique<FolderReader>(fd, file.size);
            }
        };

        bool Hidden(const fs::path& path)
        {
            std::string name = path.filename().string();
            return !name.empty() && name[0] == '.';
        }

        bool HoldsXex(const fs::path& dir)
        {
            std::error_code ec;
            for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec))
            {
                std::error_code fileEc;
                if (PathEqualsIgnoreCase(it->path().filename().string(), "default.xex") && it->is_regular_file(fileEc))
                    return true;
            }
            return false;
        }

        Result OpenFolder(const fs::path& dir, std::unique_ptr<DiscSource>& source)
        {
            fs::path root = dir;
            if (!HoldsXex(dir))
            {
                // People often pick the folder around the extracted disc
                // ("NFSMW/files/default.xex"); look one level down.
                std::vector<fs::path> found;
                std::error_code ec;
                for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec))
                {
                    std::error_code dirEc;
                    if (!Hidden(it->path()) && it->is_directory(dirEc) && HoldsXex(it->path()))
                        found.push_back(it->path());
                }
                if (ec && found.empty())
                    return Fail(Error::ReadError, std::format("Cannot read the folder {}: {}.", dir.string(), ec.message()), dir.string());
                if (found.empty())
                    return Fail(Error::NotADiscImage, std::format("{} holds no game: there is no default.xex in it or in the "
                        "folders directly inside it.", dir.string()), dir.string());
                if (found.size() > 1)
                {
                    std::sort(found.begin(), found.end());
                    std::string list;
                    for (const fs::path& p : found)
                        list += (list.empty() ? "" : ", ") + DisplayName(p);
                    return Fail(Error::NotADiscImage, std::format("{} holds several extracted games ({}). Choose one of them.",
                        dir.string(), list), dir.string());
                }
                root = found[0];
            }

            // Linked folders are followed: people keep the big NFS/ folder on
            // another drive and link it in. Each folder is identified by its
            // device and inode, and a link to one already entered (a parent,
            // itself, a sibling) is not entered again, so link loops end.
            // Real folders are always entered, whatever order links come
            // in: a link met first must not hide the folder it points to.
            std::set<std::pair<uint64_t, uint64_t>> entered;
            auto firstEntry = [&](const fs::path& dir)
            {
                struct stat st;
                return stat(dir.c_str(), &st) == 0 && entered.insert({ uint64_t(st.st_dev), uint64_t(st.st_ino) }).second;
            };
            firstEntry(root);

            std::vector<DiscFile> files;
            size_t entries = 0;
            std::error_code ec;
            fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied
                | fs::directory_options::follow_directory_symlink, ec), end;
            for (; !ec && it != end; it.increment(ec))
            {
                // Hidden entries are host clutter (.DS_Store, AppleDouble "._"
                // files on exFAT drives, the install marker), never disc files.
                if (Hidden(it->path()))
                {
                    it.disable_recursion_pending();
                    continue;
                }
                if (++entries > kMaxFolderEntries)
                    return Fail(Error::NotADiscImage, root.string() + " holds far more files than a game disc.", root.string());
                std::error_code fileEc;
                if (it->is_directory(fileEc))
                {
                    bool link = it->is_symlink(fileEc);
                    if (!firstEntry(it->path()) && link)
                        it.disable_recursion_pending();
                    if (it.depth() >= kMaxDepth)
                        it.disable_recursion_pending();
                    continue;
                }
                if (!it->is_regular_file(fileEc))
                    continue;
                uint64_t size = it->file_size(fileEc);
                if (fileEc)
                    return Fail(Error::ReadError, std::format("Cannot read {}: {}.", it->path().string(), fileEc.message()), it->path().string());
                files.push_back({ it->path().lexically_relative(root).generic_string(), size, 0 });
            }
            if (ec)
                return Fail(Error::ReadError, std::format("Cannot read the folder {}: {}.", root.string(), ec.message()), root.string());

            std::sort(files.begin(), files.end(), [](const DiscFile& a, const DiscFile& b) { return a.path < b.path; });
            source = std::make_unique<FolderSource>(root, std::move(files));
            return {};
        }
    }

    const DiscFile* DiscSource::Find(std::string_view relativePath) const
    {
        auto it = std::lower_bound(files.begin(), files.end(), relativePath,
            [](const DiscFile& f, std::string_view p) { return f.path < p; });
        if (it != files.end() && it->path == relativePath)
            return &*it;
        for (const DiscFile& f : files)
            if (PathEqualsIgnoreCase(f.path, relativePath))
                return &f;
        return nullptr;
    }

    Result OpenDiscSource(const std::filesystem::path& path, std::unique_ptr<DiscSource>& source, const OpenOptions& options)
    {
        source.reset();
        std::error_code ec;
        fs::file_status status = fs::status(path, ec);
        if (status.type() == fs::file_type::not_found)
            return Fail(Error::NotFound, path.string() + " does not exist.", path.string());
        if (ec)
            return Fail(Error::NotFound, std::format("Cannot open {}: {}.", path.string(), ec.message()), path.string());
        if (fs::is_directory(status))
            return OpenFolder(path, source);
        if (!fs::is_regular_file(status))
            return Fail(Error::NotADiscImage, path.string() + " is not a file or folder.", path.string());
        if (PathEqualsIgnoreCase(path.filename().string(), "default.xex"))
            return OpenFolder(path.parent_path().empty() ? fs::path(".") : path.parent_path(), source);
        return OpenImage(path, source, options);
    }

    Result HashFile(const DiscSource& source, const DiscFile& file, Sha256Digest& digest, const std::atomic<bool>* cancel)
    {
        std::string error;
        std::unique_ptr<FileReader> reader = source.Open(file, error);
        if (!reader)
            return Fail(Error::ReadError, std::format("Cannot open {}: {}.", file.path, error), file.path);
        std::vector<uint8_t> buffer(std::min<uint64_t>(kHashChunk, std::max<uint64_t>(file.size, 1)));
        Sha256 sha;
        for (uint64_t at = 0; at < file.size; at += buffer.size())
        {
            if (cancel && cancel->load(std::memory_order_relaxed))
                return Fail(Error::Cancelled, "Cancelled.");
            size_t n = size_t(std::min<uint64_t>(buffer.size(), file.size - at));
            if (!reader->Read(at, buffer.data(), n, error))
                return Fail(Error::ReadError, std::format("Cannot read {}: {}.", file.path, error), file.path);
            sha.Update(buffer.data(), n);
        }
        digest = sha.Finish();
        return {};
    }

    Result IdentifyXex(const DiscSource& source, XexIdentity& identity)
    {
        identity = {};
        const DiscFile* xex = source.Find("default.xex");
        if (!xex)
            return Fail(Error::MissingFile, std::format("There is no default.xex at the root of {}.", DisplayName(source.Path())), "default.xex");
        identity.file = xex;

        std::string error;
        std::unique_ptr<FileReader> reader = source.Open(*xex, error);
        if (!reader)
            return Fail(Error::ReadError, std::format("Cannot open default.xex: {}.", error), xex->path);
        // The header (with the execution info) sits in the first chunk.
        std::vector<uint8_t> buffer(1 << 20);
        Sha256 sha;
        for (uint64_t at = 0; at < xex->size; at += buffer.size())
        {
            size_t n = size_t(std::min<uint64_t>(buffer.size(), xex->size - at));
            if (!reader->Read(at, buffer.data(), n, error))
                return Fail(Error::ReadError, std::format("Cannot read default.xex: {}.", error), xex->path);
            if (at == 0)
                identity.titleId = XexTitleId(buffer.data(), n);
            sha.Update(buffer.data(), n);
        }
        identity.sha256 = sha.Finish();
        return {};
    }

    uint32_t XexTitleId(const uint8_t* data, size_t size)
    {
        // XEX2 header: optional header count at 0x14, then (key, value) pairs
        // from 0x18. Key 0x00040006 is the execution info; its title ID is at +12.
        if (size < 0x18 || memcmp(data, "XEX2", 4) != 0)
            return 0;
        uint32_t count = std::min<uint32_t>(Be32(data + 0x14), 256);
        for (uint32_t i = 0; i < count; i++)
        {
            size_t at = 0x18 + size_t(i) * 8;
            if (at + 8 > size)
                break;
            if (Be32(data + at) != 0x00040006)
                continue;
            uint32_t info = Be32(data + at + 4);
            if (uint64_t(info) + 16 > size)
                return 0;
            return Be32(data + info + 12);
        }
        return 0;
    }
}
