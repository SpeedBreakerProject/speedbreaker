// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Unpacking what downloads bring: sha256sum lines and stored zips.
#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace download
{
    // A plain file name: [A-Za-z0-9._-], 1-128 of them, not starting with '.'.
    bool PlainFileName(std::string_view name);

    // A sha256sum line: 64 hex digits, two spaces (or " *"), the file name and
    // maybe a line end. The name has to be `file`. Lower-case hex, else "".
    std::string ParseSha256Line(std::string_view text, std::string_view file);

    // --- Stored zips ---

    // The last bytes of a file hold the end of central directory
    // record (22 bytes plus a comment of up to 65535).
    inline constexpr size_t kZipTailBytes = 22 + 65535;
    inline constexpr size_t kZipLocalHeaderBytes = 30;
    // Central directories this big aren't from a download (a few thousand entries).
    inline constexpr uint64_t kMaxCentralDirectory = 16 << 20;

    enum class ZipError : uint8_t
    {
        None,
        Damaged,   // not a zip, or one cut short
        Rejected,  // a zip, but not one a download accepts (see ReadZipDirectory)
    };
    struct ZipCheck
    {
        ZipError error = ZipError::None;
        std::string detail;  // for the log
    };

    struct ZipDirectory
    {
        uint64_t offset = 0;  // of the central directory
        uint64_t size = 0;
        uint64_t count = 0;   // entries
    };
    // From the last bytes of the file (`tail`, up to kZipTailBytes
    // of a file of `fileSize` bytes). Zip64, multi-disk archives
    // and directories over kMaxCentralDirectory get Rejected.
    ZipCheck FindZipDirectory(std::string_view tail, uint64_t fileSize,
        ZipDirectory& out);

    struct ZipEntry
    {
        std::string name;     // as stored: "hd_hud/fonts/a.zst", "hd_hud/fonts/"
        // Below the root: "fonts/a.zst", "fonts" ("" for "<root>/").
        std::string path;
        bool directory = false;
        uint64_t size = 0;    // stored, so packed = unpacked
        uint64_t headerOffset = 0;  // its local header
    };

    struct ZipContents
    {
        std::vector<ZipEntry> entries;  // in archive order
        uint64_t files = 0;
        uint64_t unpackedSize = 0;      // sum of the file sizes
    };

    // Bytes of the central directory (`dir.size` of them, from `dir.offset`).
    // What gets accepted: README.md.
    ZipCheck ReadZipDirectory(std::string_view bytes, const ZipDirectory& dir,
        std::string_view root, ZipContents& out);

    // A local header (kZipLocalHeaderBytes at entry.headerOffset): where the
    // entry data starts, else 0 (Damaged: not a header, not stored, or the
    // data goes past `limit`, which is the central directory offset).
    uint64_t LocalDataOffset(std::string_view header, const ZipEntry& entry,
        uint64_t limit);

    // A zip file on disk with its directory read and checked.
    struct ZipArchive
    {
        std::filesystem::path file;
        ZipDirectory directory;
        ZipContents contents;
    };

    ZipCheck OpenZip(const std::filesystem::path& file, std::string_view root,
        ZipArchive& out);

    enum class Extract : uint8_t { Ok, Damaged, CantWrite, Cancelled };

    struct Extracted
    {
        Extract result = Extract::Ok;
        std::string detail;  // for the log; CantWrite: the path
    };

    // Writes each entry into `target` (replaced) at its path below the root.
    // Calls progress(files written) after every file and asks `cancelled`
    // before each entry. Anything but Ok removes `target`.
    Extracted ExtractZip(const ZipArchive& zip, const std::filesystem::path& target,
        const std::function<bool()>& cancelled,
        const std::function<void(uint64_t)>& progress);
}
