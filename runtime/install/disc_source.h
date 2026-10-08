// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Where the installer copies the game from: an Xbox 360 disc image (.iso, an
// XDVDFS game partition) or a folder holding an extracted disc. Both list
// their files with disc-relative paths and read them in bounded chunks, so a
// 7.8 GB image is never mapped or loaded whole (a Steam Deck has 16 GB of RAM
// shared with the GPU, and the image may sit on an SD card or USB stick).
//
// Names keep the disc's case (Movies/ is lower case, NFS/ upper case). The
// runtime doesn't care (vfs::Resolve matches case-insensitively), but it tries
// the exact name first, so the installer writes the manifest's spelling.
#pragma once
#include "result.h"
#include "sha256.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace install
{
    struct DiscFile
    {
        std::string path;       // relative to the disc root, '/'-separated, disc case
        uint64_t size = 0;
        uint64_t offset = 0;    // image: where the data starts in the image (read in this order); folder: 0
    };

    enum class SourceKind
    {
        Image,
        Folder,
    };

    class FileReader
    {
    public:
        virtual ~FileReader() = default;

        // Reads exactly `size` bytes at `offset` within the file. False on an
        // I/O error or if the data ends early, with `error` saying why.
        virtual bool Read(uint64_t offset, void* buffer, size_t size, std::string& error) = 0;
    };

    class DiscSource
    {
    public:
        virtual ~DiscSource() = default;

        SourceKind Kind() const { return kind; }
        // The image file, or the folder that holds default.xex (possibly one
        // level below the folder that was opened).
        const std::filesystem::path& Path() const { return path; }
        // Every file, sorted by path; directories are implied by the paths.
        const std::vector<DiscFile>& Files() const { return files; }
        // Exact match first, then ASCII case-insensitive; nullptr if absent.
        const DiscFile* Find(std::string_view relativePath) const;
        // Only with OpenOptions::allowTruncated: the image ends before some of
        // its files do. CheckSource/Install refuse such a source.
        bool Truncated() const { return truncated; }

        // Readers are independent of each other (pread on a shared descriptor,
        // or a descriptor of their own) and may outlive the source.
        virtual std::unique_ptr<FileReader> Open(const DiscFile& file, std::string& error) const = 0;

    protected:
        SourceKind kind = SourceKind::Folder;
        std::filesystem::path path;
        std::vector<DiscFile> files;
        bool truncated = false;
    };

    struct OpenOptions
    {
        // A truncated image normally fails to open. With this set, its listing
        // is still returned beside the TruncatedImage error (FindDiscImages
        // uses it to show the image as incomplete rather than hide it); files
        // past the end fail to read.
        bool allowTruncated = false;
    };

    // Opens `path`: a disc image (XDVDFS game partition at offset 0, 0xFB20,
    // 0x20600, 0x2080000 (XGD3) or 0xFD90000 (XGD2)), an extracted disc
    // folder (default.xex at its root or one folder down; linked folders
    // inside it are followed, each real folder once), or default.xex itself
    // (meaning its folder). On failure `source` is null, except as
    // OpenOptions::allowTruncated describes.
    Result OpenDiscSource(const std::filesystem::path& path, std::unique_ptr<DiscSource>& source, const OpenOptions& options = {});

    // Streams a file through SHA-256 in fixed-size chunks.
    Result HashFile(const DiscSource& source, const DiscFile& file, Sha256Digest& digest, const std::atomic<bool>* cancel = nullptr);

    struct XexIdentity
    {
        const DiscFile* file = nullptr;  // default.xex in the source
        Sha256Digest sha256{};
        uint32_t titleId = 0;            // from the XEX2 execution info; 0 if absent
    };

    // Finds default.xex at the source's root, hashes it and reads its title
    // ID (one pass). MissingFile if there is none.
    Result IdentifyXex(const DiscSource& source, XexIdentity& identity);

    // The title ID in an XEX2 header's execution info; 0 if it has none.
    uint32_t XexTitleId(const uint8_t* data, size_t size);
}
