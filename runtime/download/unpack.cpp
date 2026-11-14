// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See unpack.h.
#include "unpack.h"

#include <update/json.h>

#include <algorithm>
#include <format>
#include <fstream>
#include <set>
#include <system_error>

namespace download
{
    namespace
    {
        namespace fs = std::filesystem;

        bool IsDigit(char c)
        {
            return c >= '0' && c <= '9';
        }

        char Lower(char c)
        {
            return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
        }

        int HexDigit(char c)
        {
            c = Lower(c);
            return IsDigit(c) ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        }

        // --- Zip: little-endian fields ---

        uint32_t U16(std::string_view s, size_t at)
        {
            return uint32_t(uint8_t(s[at])) | uint32_t(uint8_t(s[at + 1])) << 8;
        }

        uint32_t U32(std::string_view s, size_t at)
        {
            return U16(s, at) | U16(s, at + 2) << 16;
        }

        constexpr uint32_t kEndSig = 0x06054b50, kZip64LocatorSig = 0x07064b50;
        constexpr uint32_t kCentralSig = 0x02014b50, kLocalSig = 0x04034b50;
        constexpr size_t kEndBytes = 22, kCentralBytes = 46, kZip64LocatorBytes = 20;
        constexpr size_t kChunk = 1 << 20;

        ZipCheck Bad(ZipError e, std::string detail)
        {
            return { e, std::move(detail) };
        }

        std::string LowerAscii(std::string_view s)
        {
            std::string out(s);
            for (char& c : out)
                c = Lower(c);
            return out;
        }

        // Characters of a name: UTF-8 and none that
        // a folder can't have. Else says why not.
        bool NameCharacters(std::string_view name, std::string& why)
        {
            for (size_t i = 0; i < name.size();)
            {
                const uint8_t c = uint8_t(name[i]);
                if (c < 0x20 || c == 0x7F || c == '\\' || c == ':' || c == '<' || c == '>'
                    || c == '"' || c == '|' || c == '?' || c == '*')
                    return why = "a name with a character no folder takes", false;
                uint32_t cp = 0;
                const size_t n = update::json::DecodeUtf8(name, i, cp);
                if (n == 0)
                    return why = "a name that isn't UTF-8", false;
                i += n;
            }
            return true;
        }

        bool PlainPart(std::string_view part)
        {
            return !part.empty() && part != "." && part != ".." && part.back() != '.'
                && part.back() != ' ';
        }

        // Entry name under "<root>/": the path below the root ("" for the root
        // folder itself), else why it isn't accepted.
        bool SplitName(std::string_view name, std::string_view root, bool directory,
            std::string& path, std::string& why)
        {
            if (!NameCharacters(name, why))
                return false;
            std::string_view rest = name;
            if (directory)
                rest.remove_suffix(1);
            if (!root.empty())
            {
                if (rest.substr(0, root.size()) != root
                    || (rest.size() > root.size() && rest[root.size()] != '/'))
                    return why = std::format("an entry outside {}/", root), false;
                if (rest.size() == root.size())
                {
                    if (!directory)
                        return why = std::format("a file named {}", root), false;
                    path.clear();
                    return true;
                }
                rest.remove_prefix(root.size() + 1);
            }
            path = rest;
            while (true)
            {
                const size_t slash = rest.find('/');
                if (!PlainPart(rest.substr(0, slash)))
                    return why = "a name with an empty, '.' or '..' part", false;
                if (slash == std::string_view::npos)
                    return true;
                rest.remove_prefix(slash + 1);
            }
        }
    }

    bool PlainFileName(std::string_view name)
    {
        if (name.empty() || name.size() > 128 || name[0] == '.')
            return false;
        return std::all_of(name.begin(), name.end(), [](char c) {
            return IsDigit(c) || (Lower(c) >= 'a' && Lower(c) <= 'z') || c == '.'
                || c == '_' || c == '-';
        });
    }

    std::string ParseSha256Line(std::string_view text, std::string_view file)
    {
        if (text.size() >= 1 && text.back() == '\n')
            text.remove_suffix(1);
        if (text.size() >= 1 && text.back() == '\r')
            text.remove_suffix(1);
        if (text.size() < 66 || (text.substr(64, 2) != "  " && text.substr(64, 2) != " *")
            || text.substr(66) != file || file.empty())
            return {};
        std::string hex(text.substr(0, 64));
        for (char& c : hex)
        {
            if (HexDigit(c) < 0)
                return {};
            c = Lower(c);
        }
        return hex;
    }

    ZipCheck FindZipDirectory(std::string_view tail, uint64_t fileSize, ZipDirectory& out)
    {
        if (tail.size() > fileSize)
            return Bad(ZipError::Damaged, "more tail than file");
        if (tail.size() < kEndBytes)
            return Bad(ZipError::Damaged, "too short for a zip");
        // The last end record with a comment that
        // reaches exactly the end of the file.
        size_t at = tail.size() - kEndBytes + 1;
        while (true)
        {
            if (at-- == 0)
                return Bad(ZipError::Damaged, "no end of central directory");
            if (U32(tail, at) == kEndSig
                && at + kEndBytes + U16(tail, at + 20) == tail.size())
                break;
        }
        const uint64_t endOffset = fileSize - tail.size() + at;
        if (at >= kZip64LocatorBytes
            && U32(tail, at - kZip64LocatorBytes) == kZip64LocatorSig)
            return Bad(ZipError::Rejected, "a zip64 archive");
        const uint32_t disk = U16(tail, at + 4), dirDisk = U16(tail, at + 6);
        const uint32_t here = U16(tail, at + 8), count = U16(tail, at + 10);
        const uint32_t size = U32(tail, at + 12), offset = U32(tail, at + 16);
        if (count == 0xFFFF || here == 0xFFFF || size == 0xFFFFFFFF
            || offset == 0xFFFFFFFF)
            return Bad(ZipError::Rejected, "a zip64 archive");
        if (disk != 0 || dirDisk != 0 || here != count)
            return Bad(ZipError::Rejected, "a multi-part archive");
        if (uint64_t(offset) + size > endOffset)
            return Bad(ZipError::Damaged,
                "the central directory runs past its end record");
        if (size > kMaxCentralDirectory)
            return Bad(ZipError::Rejected,
                std::format("a {} byte central directory", size));
        out = { offset, size, count };
        return {};
    }

    ZipCheck ReadZipDirectory(std::string_view bytes, const ZipDirectory& dir,
        std::string_view root, ZipContents& out)
    {
        out = {};
        if (bytes.size() != dir.size)
            return Bad(ZipError::Damaged, "a short central directory");
        std::string why;
        if (!root.empty()
            && (root.find('/') != std::string_view::npos || !PlainPart(root)
                || !NameCharacters(root, why)))
            return Bad(ZipError::Rejected, std::format("a bad root \"{}\"", root));
        std::set<std::string> seen;   // paths, lower case
        std::set<std::string> files;  // file paths, lower case
        size_t at = 0;
        for (uint64_t n = 0; n < dir.count; n++)
        {
            if (bytes.size() - at < kCentralBytes || U32(bytes, at) != kCentralSig)
                return Bad(ZipError::Damaged,
                    std::format("entry {}: not a central directory record", n));
            const uint32_t made = U16(bytes, at + 4), flags = U16(bytes, at + 8);
            const uint32_t method = U16(bytes, at + 10);
            const uint32_t packed = U32(bytes, at + 20), size = U32(bytes, at + 24);
            const uint32_t nameLen = U16(bytes, at + 28), extraLen = U16(bytes, at + 30);
            const uint32_t commentLen = U16(bytes, at + 32);
            const uint32_t startDisk = U16(bytes, at + 34);
            const uint32_t external = U32(bytes, at + 38), local = U32(bytes, at + 42);
            const size_t total = kCentralBytes + nameLen + extraLen + commentLen;
            if (bytes.size() - at < total)
                return Bad(ZipError::Damaged, std::format("entry {}: cut short", n));
            const std::string_view name = bytes.substr(at + kCentralBytes, nameLen);
            at += total;
            if (packed == 0xFFFFFFFF || size == 0xFFFFFFFF || local == 0xFFFFFFFF
                || startDisk == 0xFFFF)
                return Bad(ZipError::Rejected, std::format("{}: zip64", name));
            if (flags & (1 | 1 << 6))
                return Bad(ZipError::Rejected, std::format("{}: encrypted", name));
            if (method != 0)
                return Bad(ZipError::Rejected,
                    std::format("{}: compressed (method {})", name, method));
            if (packed != size)
                return Bad(ZipError::Damaged,
                    std::format("{}: stored with two sizes", name));
            if (startDisk != 0)
                return Bad(ZipError::Rejected, std::format("{}: on another disk", name));
            if ((made >> 8) == 3 && ((external >> 16) & 0xF000) == 0xA000)
                return Bad(ZipError::Rejected, std::format("{}: a symlink", name));
            if (uint64_t(local) + kZipLocalHeaderBytes + nameLen + size > dir.offset)
                return Bad(ZipError::Damaged,
                    std::format("{}: data past the central directory", name));
            ZipEntry e;
            e.name = name;
            e.directory = !name.empty() && name.back() == '/';
            e.size = size;
            e.headerOffset = local;
            if (!SplitName(name, root, e.directory, e.path, why))
                return Bad(ZipError::Rejected, std::format("{}: {}", name, why));
            if (e.directory && size)
                return Bad(ZipError::Rejected,
                    std::format("{}: a folder with data", name));
            if (!seen.insert(LowerAscii(e.path)).second)
                return Bad(ZipError::Rejected,
                    std::format("{}: a second entry for it", name));
            if (!e.directory)
            {
                files.insert(LowerAscii(e.path));
                out.files++;
                out.unpackedSize += size;
            }
            out.entries.push_back(std::move(e));
        }
        if (at != bytes.size())
            return Bad(ZipError::Damaged, "bytes after the last entry");
        // A file can't also be a folder on the way to another entry.
        for (const ZipEntry& e : out.entries)
            for (size_t slash = e.path.find('/'); slash != std::string::npos;
                 slash = e.path.find('/', slash + 1))
                if (files.count(LowerAscii(std::string_view(e.path).substr(0, slash))))
                    return Bad(ZipError::Rejected,
                        std::format("{}: under a file", e.name));
        if (out.files == 0)
            return Bad(ZipError::Rejected, "no files");
        return {};
    }

    uint64_t LocalDataOffset(std::string_view header, const ZipEntry& entry,
        uint64_t limit)
    {
        if (header.size() < kZipLocalHeaderBytes || U32(header, 0) != kLocalSig
            || U16(header, 8) != 0)
            return 0;
        const uint64_t data =
            entry.headerOffset + kZipLocalHeaderBytes + U16(header, 26) + U16(header, 28);
        if (U16(header, 26) != entry.name.size() || data + entry.size > limit)
            return 0;
        return data;
    }

    namespace
    {
        bool ReadAt(std::ifstream& in, uint64_t offset, size_t size, std::string& out)
        {
            out.resize(size);
            in.clear();
            in.seekg(std::streamoff(offset));
            in.read(out.data(), std::streamsize(size));
            return size_t(in.gcount()) == size;
        }
    }

    ZipCheck OpenZip(const fs::path& file, std::string_view root, ZipArchive& out)
    {
        out = {};
        out.file = file;
        std::error_code ec;
        const uint64_t size = fs::file_size(file, ec);
        std::ifstream in(file, std::ios::binary);
        if (ec || !in)
            return Bad(ZipError::Damaged, "can't open " + file.string());
        const uint64_t tailBytes = std::min<uint64_t>(size, kZipTailBytes);
        std::string bytes;
        if (!ReadAt(in, size - tailBytes, size_t(tailBytes), bytes))
            return Bad(ZipError::Damaged, "can't read the zip's end");
        ZipCheck check = FindZipDirectory(bytes, size, out.directory);
        if (check.error != ZipError::None)
            return check;
        if (!ReadAt(in, out.directory.offset, size_t(out.directory.size), bytes))
            return Bad(ZipError::Damaged, "can't read the central directory");
        return ReadZipDirectory(bytes, out.directory, root, out.contents);
    }

    Extracted ExtractZip(const ZipArchive& zip, const fs::path& target,
        const std::function<bool()>& cancelled,
        const std::function<void(uint64_t)>& progress)
    {
        std::ifstream in(zip.file, std::ios::binary);
        if (!in)
            return { Extract::Damaged, "can't open " + zip.file.string() };
        std::error_code ec;
        fs::remove_all(target, ec);
        fs::create_directories(target, ec);
        if (ec)
            return { Extract::CantWrite, target.string() };
        auto fail = [&](Extract result, std::string detail) {
            std::error_code removeError;
            fs::remove_all(target, removeError);
            return Extracted{ result, std::move(detail) };
        };

        std::vector<char> buffer(kChunk);
        uint64_t done = 0;
        for (const ZipEntry& e : zip.contents.entries)
        {
            if (cancelled && cancelled())
                return fail(Extract::Cancelled, "cancelled");
            const fs::path path =
                target / fs::path(std::u8string(e.path.begin(), e.path.end()));
            if (e.directory)
            {
                fs::create_directories(path, ec);
                if (ec)
                    return fail(Extract::CantWrite, path.string());
                continue;
            }
            std::string header;
            if (!ReadAt(in, e.headerOffset, kZipLocalHeaderBytes, header))
                return fail(Extract::Damaged, "can't read the local header of " + e.name);
            const uint64_t data = LocalDataOffset(header, e, zip.directory.offset);
            if (!data)
                return fail(Extract::Damaged, "bad local header of " + e.name);
            fs::create_directories(path.parent_path(), ec);
            // Closed before fail() removes `target` (Windows can't delete open files).
            Extract result = Extract::Ok;
            {
                std::ofstream out(path, std::ios::binary | std::ios::trunc);
                in.clear();
                in.seekg(std::streamoff(data));
                for (uint64_t left = e.size; out && left;)
                {
                    const size_t n = size_t(std::min<uint64_t>(left, buffer.size()));
                    in.read(buffer.data(), std::streamsize(n));
                    if (size_t(in.gcount()) != n)
                    {
                        result = Extract::Damaged;
                        break;
                    }
                    out.write(buffer.data(), std::streamsize(n));
                    left -= n;
                }
                if (result == Extract::Ok && !out.flush())
                    result = Extract::CantWrite;
            }
            if (result == Extract::Damaged)
                return fail(result, "can't read " + e.name);
            if (result != Extract::Ok)
                return fail(result, path.string());
            done++;
            if (progress)
                progress(done);
        }
        return {};
    }
}
