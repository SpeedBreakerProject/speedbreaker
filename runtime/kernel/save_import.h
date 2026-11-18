// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
#pragma once
// Bringing a save in from outside (GitHub issues #83, #105): a file a player
// puts in the save folder becomes a save the game lists.
//
// The runtime keeps a save as a folder <NAME>/ holding the game's own file
// <NAME>, with <NAME>.xcontent (an XCONTENT_DATA, 308 bytes) beside it
// (kernel/xam.cpp, ScanSavedContent). An Xbox 360 keeps the same file inside
// an STFS package ("CON ": signed by the console; "LIVE"/"PIRS": by
// Microsoft). So, once per start, before the saves are listed:
//   - a package in the save folder (for this game's title ID, a saved game)
//     has its file taken out;
//   - a bare save file (the game's file, already taken out of a package by
//     a tool) is taken as it is;
//   - a save's folder that lost its .xcontent (copied from another install
//     without it) gets a new one;
//   - a save in the Xenia emulator's layout (a dropped 454107D9/ or
//     00000001/ folder: GitHub #105's save had it) is taken as a bare file.
// The game's file is EA's "MC02" memory card file: a header, two parts, and
// a CRC over each; the game's own data ends in an MD5 of itself. Only a file
// whose three CRCs and MD5 match and whose first part says Most Wanted's
// version and size is brought in (the game's own checks, so a save it would
// call damaged is never listed by this); the console's signature
// and the package's hash tables are not checked (nothing here trusts the
// package: the CRCs say whether the file inside came out whole). The profile
// and console IDs in a package's header are not part of the save and are
// ignored: the game never stored either in its file.
//
// What this never does: overwrite or delete a save, or delete what was
// dropped. A save already there under the same name wins (the dropped file
// stays where it is, with a line in import-log.txt); a file that can't be
// read stays where it is; a file brought in is MOVED to imported/ in the
// folder it was put in (it must leave its place: a console names the package
// after the save, the name the save's folder needs). On iPhone and iPad the
// save folder is out of the player's reach, so the app's Documents folder
// (Files > On My iPhone > SpeedBreaker) is looked at too, as a drop folder:
// ImportDroppedSaves' `dropDir`; its import-log.txt and imported/ are there.
// tests/save_import_test.cpp.
//
// Header only, standard library only (the unit test builds it alone).
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace save_import
{
    inline constexpr uint32_t kTitleId = 0x454107D9;        // Need for Speed Most Wanted (USA and PAL)
    inline constexpr uint32_t kSaveVersion = 269;           // guest 8228C4F0: part 1's first word
    inline constexpr uint32_t kSaveSize = 62652;            // guest 822C53A0: the game's blob
    inline constexpr uint32_t kKnownDataTag = 0x55ADA3B6;   // the tag every save made here carries (USA, PAL-ES)
    inline constexpr size_t kTagOffset = 0x5A2D;            // in the file: 28 + 8 + 16 + 23033
    inline constexpr size_t kAliasOffset = 0x5A31;          // 32 bytes, the profile's name
    inline constexpr size_t kMd5From = 0x34;                // guest 822C3CD8: MD5 of the file from here to its
    inline constexpr size_t kMd5Bytes = 16;                 // last 16 bytes, which hold it
    inline constexpr uintmax_t kMaxPackageBytes = 64u << 20;
    inline constexpr const char* kImportedFolder = "imported";
    inline constexpr const char* kLogName = "import-log.txt";

    inline uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
    inline uint32_t Le24(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16; }
    inline uint32_t Be24(const uint8_t* p) { return uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2]; }

    // EA's CRC-32 (guest sub_825B7EE0): polynomial 04C11DB7, most significant
    // bit first, the first four bytes loaded inverted, the result inverted.
    inline uint32_t EaCrc(const uint8_t* p, size_t n)
    {
        if (n < 4)
            return 0;
        static const auto table = [] {
            std::vector<uint32_t> t(256);
            for (uint32_t i = 0; i < 256; i++)
            {
                uint32_t c = i << 24;
                for (int k = 0; k < 8; k++)
                    c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : c << 1;
                t[i] = c;
            }
            return t;
        }();
        uint32_t c = ~Be32(p);
        for (size_t i = 4; i < n; i++)
            c = table[c >> 24] ^ (c << 8 | p[i]);
        return ~c;
    }

    // MD5 (RFC 1321), for the save's own last 16 bytes.
    inline std::array<uint8_t, 16> Md5(const uint8_t* data, size_t size)
    {
        static const uint32_t kSine[64] = {
            0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
            0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
            0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
            0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
            0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
            0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
            0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
            0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
        static const uint8_t kTurn[64] = {
            7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
            4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };
        std::vector<uint8_t> m(data, data + size);
        m.push_back(0x80);
        while (m.size() % 64 != 56)
            m.push_back(0);
        const uint64_t bits = uint64_t(size) * 8;
        for (int i = 0; i < 8; i++)
            m.push_back(uint8_t(bits >> (8 * i)));
        uint32_t h[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
        for (size_t at = 0; at < m.size(); at += 64)
        {
            uint32_t w[16];
            for (int i = 0; i < 16; i++)
                w[i] = uint32_t(m[at + 4 * i]) | uint32_t(m[at + 4 * i + 1]) << 8 | uint32_t(m[at + 4 * i + 2]) << 16
                    | uint32_t(m[at + 4 * i + 3]) << 24;
            uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
            for (uint32_t i = 0; i < 64; i++)
            {
                uint32_t f, g;
                if (i < 16) { f = (b & c) | (~b & d); g = i; }
                else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
                else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
                else { f = c ^ (b | ~d); g = (7 * i) % 16; }
                const uint32_t x = a + f + kSine[i] + w[g];
                a = d; d = c; c = b;
                b += x << kTurn[i] | x >> (32 - kTurn[i]);
            }
            h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        }
        std::array<uint8_t, 16> out;
        for (int i = 0; i < 16; i++)
            out[i] = uint8_t(h[i / 4] >> (8 * (i % 4)));
        return out;
    }

    // The game's file, checked the way the game checks it before it reads the
    // career (Realmc: guest sub_825B05B0; the memory card class: sub_8228C400;
    // the career's own MD5: sub_822C3A60).
    struct SaveCheck
    {
        bool ok = false;       // whole, and Most Wanted's version and size
        bool otherGame = false; // whole, but another EA game's (or another version's)
        std::string why;       // when !ok
        uint32_t version = 0, blobSize = 0, tag = 0;
        std::string alias;     // the profile's name inside the file
    };

    inline SaveCheck CheckSave(const std::vector<uint8_t>& f)
    {
        SaveCheck r;
        char text[160];
        if (f.size() < 28 || memcmp(f.data(), "MC02", 4) != 0)
        {
            r.why = "not the game's save file (no MC02 header)";
            return r;
        }
        const uint64_t total = Be32(&f[4]), size1 = Be32(&f[8]), size2 = Be32(&f[12]);
        if (total != 28 + size1 + size2 || total != f.size())
        {
            snprintf(text, sizeof(text), "its header says %llu bytes (%llu + %llu + 28), the file has %llu",
                (unsigned long long)total, (unsigned long long)size1, (unsigned long long)size2, (unsigned long long)f.size());
            r.why = text;
            return r;
        }
        const uint8_t* b = f.data();
        if (EaCrc(b, 24) != Be32(b + 24))
        {
            r.why = "its header's checksum does not match (damaged)";
            return r;
        }
        if (EaCrc(b + 28, size_t(size1)) != Be32(b + 16) || EaCrc(b + 28 + size_t(size1), size_t(size2)) != Be32(b + 20))
        {
            r.why = "its data's checksum does not match (damaged, or edited without fixing the checksum)";
            return r;
        }
        if (size1 >= 8)
        {
            r.version = Be32(b + 28);
            r.blobSize = Be32(b + 32);
        }
        if (size1 != 8 || r.version != kSaveVersion || r.blobSize != kSaveSize || size2 != kSaveSize)
        {
            snprintf(text, sizeof(text), "a whole EA save, but not this game's (version %u, %llu bytes; Most Wanted's is version %u, %u bytes)",
                r.version, (unsigned long long)size2, kSaveVersion, kSaveSize);
            r.why = text;
            r.otherGame = true;
            return r;
        }
        const std::array<uint8_t, 16> md5 = Md5(b + kMd5From, f.size() - kMd5From - kMd5Bytes);
        if (memcmp(md5.data(), b + f.size() - kMd5Bytes, kMd5Bytes) != 0)
        {
            r.why = "its last check (the MD5 at its end) does not match: the game would say it is damaged "
                    "(edited without fixing it, or not whole)";
            return r;
        }
        r.tag = Be32(&f[kTagOffset]);
        for (size_t i = 0; i < 32 && f[kAliasOffset + i]; i++)
            r.alias.push_back(char(f[kAliasOffset + i]));
        r.ok = true;
        return r;
    }

    // --- STFS, read only: enough to take a file out of a package's root. ---
    struct StfsFile
    {
        std::string name;
        bool directory = false, contiguous = false;
        uint32_t startBlock = 0, size = 0;
        int32_t parent = -1;
    };

    struct StfsPackage
    {
        const std::vector<uint8_t>* data = nullptr;
        char magic[5] = {};
        uint32_t headerSize = 0, contentType = 0, titleId = 0;
        uint8_t consoleId[5] = {}, profileId[8] = {};
        uint8_t blockSeparation = 0;
        uint32_t tableBlocks = 0, tableStart = 0, totalBlocks = 0;
        uint64_t dataBase = 0;  // the header rounded up to a block
        uint32_t shift = 0;     // 1: each hash table is a pair (packages a console writes)
        std::u16string displayName;
    };

    inline bool IsStfsMagic(const uint8_t* p)
    {
        return memcmp(p, "CON ", 4) == 0 || memcmp(p, "LIVE", 4) == 0 || memcmp(p, "PIRS", 4) == 0;
    }

    inline bool ParseStfs(const std::vector<uint8_t>& d, StfsPackage& p, std::string& why)
    {
        if (d.size() < 0x971A || !IsStfsMagic(d.data()))
        {
            why = "not an Xbox 360 package (or cut short)";
            return false;
        }
        p.data = &d;
        memcpy(p.magic, d.data(), 4);
        p.headerSize = Be32(&d[0x340]);
        p.contentType = Be32(&d[0x344]);
        p.titleId = Be32(&d[0x360]);
        memcpy(p.consoleId, &d[0x36C], 5);
        memcpy(p.profileId, &d[0x371], 8);
        if (d[0x379] != 0x24 || p.headerSize < 0x971A || p.headerSize > 0x100000)
        {
            why = "an Xbox 360 package of a kind that holds no files this can read";
            return false;
        }
        p.blockSeparation = d[0x37B];
        p.tableBlocks = uint32_t(d[0x37C]) | uint32_t(d[0x37D]) << 8;
        p.tableStart = Le24(&d[0x37E]);
        p.totalBlocks = Be32(&d[0x395]);
        p.dataBase = (uint64_t(p.headerSize) + 0xFFF) & ~uint64_t(0xFFF);
        p.shift = (p.dataBase >> 12) == 0xB ? 0 : 1;
        for (size_t i = 0; i < 0x80; i++)
        {
            const char16_t c = char16_t(d[0x411 + i * 2] << 8 | d[0x412 + i * 2]);
            if (!c)
                break;
            p.displayName.push_back(c);
        }
        return true;
    }

    // Where data block `block` is in the package (hash tables sit between runs
    // of 0xAA data blocks, with more above each 0x70E4 and 0x4AF768).
    inline uint64_t StfsBlockOffset(const StfsPackage& p, uint32_t block)
    {
        uint64_t n = block + (uint64_t((block + 0xAA) / 0xAA) << p.shift);
        if (block >= 0xAA)
        {
            n += uint64_t((block + 0x70E4) / 0x70E4) << p.shift;
            if (block >= 0x70E4)
                n += uint64_t((block + 0x4AF768) / 0x4AF768) << p.shift;
        }
        return p.dataBase + (n << 12);
    }

    // The block after `block` in its file's chain, from the level 0 hash
    // table (`second`: the other table of a pair). 24 bytes an entry: SHA-1,
    // a status byte, the next block.
    inline bool StfsNextBlock(const StfsPackage& p, uint32_t block, bool second, uint32_t& next)
    {
        uint64_t n = uint64_t(block / 0xAA) * (p.shift ? 0xAC : 0xAB);
        if (block >= 0xAA)
        {
            n += uint64_t(block / 0x70E4 + 1) << p.shift;
            if (block >= 0x70E4)
                n += uint64_t(1) << p.shift;
        }
        uint64_t off = p.dataBase + (n << 12) + ((second && p.shift) ? 0x1000 : 0) + uint64_t(block % 0xAA) * 0x18;
        if (off + 0x18 > p.data->size())
            return false;
        next = Be24(&(*p.data)[size_t(off) + 0x15]);
        return true;
    }

    enum class StfsChain { Contiguous, TableA, TableB };

    // `blocks` blocks from `start`, by the chain asked for. False when the
    // chain leaves the package.
    inline bool StfsRead(const StfsPackage& p, uint32_t start, uint32_t blocks, StfsChain chain, std::vector<uint8_t>& out)
    {
        out.clear();
        if (blocks > 0x4000)  // 64 MB: no save is near it
            return false;
        out.reserve(size_t(blocks) << 12);
        uint32_t block = start;
        for (uint32_t i = 0; i < blocks; i++)
        {
            const uint64_t off = StfsBlockOffset(p, block);
            if (block >= 0xFFFFF0 || off >= p.data->size())
                return false;
            const size_t have = size_t(std::min<uint64_t>(0x1000, p.data->size() - off));
            out.insert(out.end(), p.data->begin() + size_t(off), p.data->begin() + size_t(off) + have);
            out.resize(size_t(i + 1) << 12, 0);
            if (chain == StfsChain::Contiguous)
                block++;
            else if (i + 1 < blocks && !StfsNextBlock(p, block, chain == StfsChain::TableB, block))
                return false;
        }
        return true;
    }

    // The package's files, as the file table lists them (0x40 bytes each).
    inline std::vector<StfsFile> StfsFiles(const StfsPackage& p, StfsChain chain)
    {
        std::vector<StfsFile> files;
        std::vector<uint8_t> table;
        if (!StfsRead(p, p.tableStart, p.tableBlocks, chain, table))
            return files;
        for (size_t at = 0; at + 0x40 <= table.size(); at += 0x40)
        {
            const uint8_t* e = &table[at];
            if (!e[0])
                break;
            StfsFile f;
            const uint8_t flags = e[0x28];
            f.name.assign(reinterpret_cast<const char*>(e), std::min<size_t>(flags & 0x3F, 0x28));
            f.contiguous = flags & 0x40;
            f.directory = flags & 0x80;
            f.startBlock = Le24(e + 0x2F);
            f.parent = int16_t(uint16_t(e[0x32]) << 8 | e[0x33]);
            f.size = Be32(e + 0x34);
            files.push_back(std::move(f));
        }
        return files;
    }

    // The save inside a package: the first file in its root that reads back
    // as a whole Most Wanted save by any chain (a package's block chain can
    // be in either table of a pair; the save's own checksums say which read
    // is right). `why` says what was found when there is none.
    inline bool StfsFindSave(const StfsPackage& p, std::string& name, std::vector<uint8_t>& save, SaveCheck& check, std::string& why)
    {
        static const StfsChain kChains[] = { StfsChain::TableA, StfsChain::TableB, StfsChain::Contiguous };
        why = "no file in the package";
        for (StfsChain tableChain : kChains)
        {
            for (const StfsFile& f : StfsFiles(p, tableChain))
            {
                if (f.directory || f.parent != -1 || f.size < 28 || f.size > kMaxPackageBytes)
                    continue;
                const uint32_t blocks = (f.size + 0xFFF) >> 12;
                why = "\"" + f.name + "\" inside it is cut short (the package is not whole)";
                for (StfsChain chain : kChains)
                {
                    std::vector<uint8_t> bytes;
                    if (!StfsRead(p, f.startBlock, blocks, chain, bytes))
                        continue;
                    bytes.resize(f.size);
                    SaveCheck c = CheckSave(bytes);
                    if (c.ok)
                    {
                        name = f.name;
                        save = std::move(bytes);
                        check = std::move(c);
                        return true;
                    }
                    why = "\"" + f.name + "\" inside it: " + c.why;
                }
            }
        }
        return false;
    }

    // --- The save folder. ---
    enum class Outcome
    {
        Imported,      // a package's or a bare file's save is now <NAME>/<NAME> + <NAME>.xcontent
        Adopted,       // a save's folder got its missing .xcontent
        NameTaken,     // a save of that name is there: nothing touched
        NotThisGame,   // a package or save of another game or kind: nothing touched
        Unreadable,    // damaged or cut short: nothing touched
        Failed,        // the disk said no: put back as it was
    };

    struct Report
    {
        std::filesystem::path source;
        std::string name;  // the save's name, when one was found
        Outcome outcome = Outcome::Failed;
        std::string detail;
    };

    inline const char* OutcomeText(Outcome o)
    {
        switch (o)
        {
        case Outcome::Imported: return "imported";
        case Outcome::Adopted: return "listed again";
        case Outcome::NameTaken: return "not imported (a save of that name is already here)";
        case Outcome::NotThisGame: return "not imported (not a Most Wanted Xbox 360 save)";
        case Outcome::Unreadable: return "not imported (the file can't be read)";
        default: return "not imported (the disk refused)";
        }
    }

    // A save's name becomes a folder's and a file's: the characters the game
    // lets a player type are letters, digits and a few signs; anything a
    // file system could take for a path is refused.
    inline bool UsableName(const std::string& name)
    {
        if (name.empty() || name.size() > 41 || name[0] == '.' || name.back() == ' ' || name.back() == '.' || name == kImportedFolder)
            return false;
        for (unsigned char c : name)
            if (c < 0x20 || c > 0x7E || strchr("/\\:*?\"<>|", c))
                return false;
        return true;
    }

    // A path's text as UTF-8 (path::string() can throw on Windows).
    inline std::string Utf8(const std::filesystem::path& path)
    {
        const std::u8string text = path.u8string();
        return std::string(text.begin(), text.end());
    }

    inline bool ReadWhole(const std::filesystem::path& path, std::vector<uint8_t>& out)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f)
            return false;
        out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        return !f.bad();
    }

    inline bool WriteNew(const std::filesystem::path& path, const void* data, size_t size)
    {
        std::error_code ec;
        if (std::filesystem::exists(std::filesystem::symlink_status(path, ec)))
            return false;  // never over something
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f.write(static_cast<const char*>(data), std::streamsize(size));
        f.close();
        return bool(f);
    }

    // XCONTENT_DATA as the runtime stores it (kernel/xam.cpp): device 1,
    // content type 1 (saved game), the display name (UTF-16, big-endian, 128),
    // the file name (42), two bytes of padding. 308 bytes, all big-endian.
    inline std::vector<uint8_t> MakeXContent(const std::string& name, const std::u16string& displayName)
    {
        std::vector<uint8_t> x(308, 0);
        x[3] = 1;
        x[7] = 1;
        std::u16string shown = displayName;
        if (shown.empty())
            shown.assign(name.begin(), name.end());
        for (size_t i = 0; i < shown.size() && i < 127; i++)
        {
            x[8 + i * 2] = uint8_t(shown[i] >> 8);
            x[9 + i * 2] = uint8_t(shown[i]);
        }
        memcpy(&x[0x108], name.data(), std::min<size_t>(name.size(), 41));
        return x;
    }

    // A place in `in`/imported/ for `file` that holds nothing yet. `in` is
    // the folder `file` was put in (the save folder, or a drop folder).
    inline std::filesystem::path FreeImportedPath(const std::filesystem::path& in, const std::filesystem::path& file)
    {
        const std::filesystem::path dir = in / kImportedFolder;
        std::error_code ec;
        for (int n = 1; n < 1000; n++)
        {
            std::filesystem::path to = dir / file.filename();
            if (n > 1)
                to += "-" + std::to_string(n);
            if (!std::filesystem::exists(std::filesystem::symlink_status(to, ec)))
                return to;
        }
        return {};
    }

    // One dropped file. Nothing is changed unless everything it needs is in
    // hand: the save's bytes checked, the name free. Brought in, it moves to
    // imported/ in the folder it was put in (the save folder, or a drop
    // folder: ImportDroppedSaves). `keep`: the file stays where it is (a save
    // inside a dropped folder, which is moved to imported/ as a whole
    // afterwards: ImportXeniaFolder, ImportSaveFolder); `shownName`, a bare
    // save's display name when something beside it says one.
    inline Report ImportFile(const std::filesystem::path& saveDir, const std::filesystem::path& file, bool keep = false,
        const std::u16string& shownName = {})
    {
        namespace fs = std::filesystem;
        Report r;
        r.source = file;
        std::error_code ec;
        std::vector<uint8_t> bytes;
        if (!ReadWhole(file, bytes) || bytes.size() < 4)
        {
            r.outcome = Outcome::Unreadable;
            r.detail = "could not read it";
            return r;
        }

        std::vector<uint8_t> save;
        SaveCheck check;
        std::u16string displayName = shownName;
        char text[200];
        if (IsStfsMagic(bytes.data()))
        {
            StfsPackage package;
            if (!ParseStfs(bytes, package, r.detail))
            {
                r.outcome = Outcome::Unreadable;
                return r;
            }
            if (package.titleId != kTitleId || package.contentType != 1)
            {
                r.outcome = Outcome::NotThisGame;
                snprintf(text, sizeof(text), "an Xbox 360 package for title %08X, content type %X (a Most Wanted save is title %08X, type 1)",
                    package.titleId, package.contentType, kTitleId);
                r.detail = text;
                return r;
            }
            std::string why;
            if (!StfsFindSave(package, r.name, save, check, why))
            {
                r.outcome = Outcome::Unreadable;
                r.detail = "a Most Wanted package, but no whole save came out of it: " + why;
                return r;
            }
            displayName = package.displayName;
        }
        else
        {
            if (memcmp(bytes.data(), "20CM", 4) == 0)
            {
                r.outcome = Outcome::NotThisGame;
                r.detail = "a save from the PC version of the game; this is the Xbox 360 version and can't read it";
                return r;
            }
            check = CheckSave(bytes);
            if (!check.ok)
            {
                r.outcome = check.otherGame ? Outcome::NotThisGame : Outcome::Unreadable;
                r.detail = check.why;
                return r;
            }
            save = std::move(bytes);
            r.name = UsableName(check.alias) ? check.alias : Utf8(file.filename());
        }

        if (!UsableName(r.name))
        {
            r.outcome = Outcome::Unreadable;
            r.detail = "the save's name can't be a folder's name here";
            return r;
        }

        // The save's folder may be where the dropped file is (a console names
        // the package after the save): only then is the place not "taken".
        const fs::path root = saveDir / r.name;
        const fs::path xcontent = saveDir / (r.name + ".xcontent");
        const bool sourceIsRoot = fs::equivalent(root, file, ec);
        ec.clear();
        if ((!sourceIsRoot && fs::exists(fs::symlink_status(root, ec))) || fs::exists(fs::symlink_status(xcontent, ec)))
        {
            r.outcome = Outcome::NameTaken;
            r.detail = "\"" + r.name + "\" is already a save here; nothing was changed. Rename or move that one away first";
            return r;
        }

        // Staged beside its place, then renamed: a start that stops halfway
        // leaves the dropped file or the finished save, never half of one.
        const fs::path stage = saveDir / (".import-" + r.name + ".tmp");
        const fs::path moved = keep ? fs::path() : FreeImportedPath(file.parent_path(), file);
        auto fail = [&](const char* what) {
            fs::remove(stage / r.name, ec);  // only what this made: one file, its folder
            fs::remove(stage, ec);
            r.outcome = Outcome::Failed;
            r.detail = what;
            return r;
        };
        fs::remove(stage / r.name, ec);
        fs::remove(stage, ec);
        if (!keep)
            fs::create_directories(file.parent_path() / kImportedFolder, ec);
        if ((!keep && moved.empty()) || !fs::create_directory(stage, ec) || !WriteNew(stage / r.name, save.data(), save.size()))
            return fail("could not write in the save folder");
        if (!keep)
        {
            fs::rename(file, moved, ec);
            if (ec)
                return fail("could not move the dropped file to imported/");
        }
        fs::rename(stage, root, ec);
        if (ec)
        {
            std::error_code back;
            if (!keep)
                fs::rename(moved, file, back);  // as it was
            return fail("could not make the save's folder");
        }
        const std::vector<uint8_t> x = MakeXContent(r.name, displayName);
        if (!WriteNew(xcontent, x.data(), x.size()))
        {
            // The save's folder is whole and the next start gives it its
            // .xcontent (AdoptFolder); the dropped file stays in imported/.
            r.outcome = Outcome::Failed;
            r.detail = "the save is in place but its .xcontent could not be written; it is tried again at the next start";
            return r;
        }
        r.outcome = Outcome::Imported;
        if (keep)
            snprintf(text, sizeof(text), "profile \"%s\"", check.alias.c_str());
        else
            snprintf(text, sizeof(text), "profile \"%s\"; the dropped file is now imported/%s", check.alias.c_str(),
                Utf8(moved.filename()).c_str());
        r.detail = text;
        if (check.tag != kKnownDataTag)
        {
            snprintf(text, sizeof(text), ". Its data tag is %08X, not this game's usual %08X: the game may say it is damaged "
                "(a save from another edition of the game)", check.tag, kKnownDataTag);
            r.detail += text;
        }
        return r;
    }

    // A folder <NAME>/ with a whole save <NAME> in it and no <NAME>.xcontent:
    // a save copied from another install without it. Writes the .xcontent.
    inline bool AdoptFolder(const std::filesystem::path& saveDir, const std::filesystem::path& folder, Report& r)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const std::string name = Utf8(folder.filename());
        const fs::path inner = folder / name;
        if (!UsableName(name) || fs::exists(fs::symlink_status(saveDir / (name + ".xcontent"), ec)) || !fs::is_regular_file(inner, ec)
            || fs::file_size(inner, ec) > kMaxPackageBytes)
            return false;
        std::vector<uint8_t> bytes;
        if (!ReadWhole(inner, bytes))
            return false;
        r.source = folder;
        r.name = name;
        const SaveCheck check = CheckSave(bytes);
        if (!check.ok)
        {
            r.outcome = Outcome::Unreadable;
            r.detail = "a folder without its .xcontent, and the file in it: " + check.why + ". Nothing was changed";
            return true;
        }
        const std::vector<uint8_t> x = MakeXContent(name, {});
        const bool wrote = WriteNew(saveDir / (name + ".xcontent"), x.data(), x.size());
        r.outcome = wrote ? Outcome::Adopted : Outcome::Failed;
        r.detail = wrote ? "the folder had no .xcontent; one was written" : "could not write its .xcontent";
        return true;
    }

    // Xenia's .header beside a save: its first 308 bytes are the save's
    // XCONTENT_DATA (device, content type 1, the display name), the title's
    // fields after. The display name, or nothing when it isn't one. (A
    // <NAME>.xcontent is the same 308 bytes alone: ImportSaveFolder.)
    inline std::u16string XeniaDisplayName(const std::filesystem::path& header)
    {
        std::vector<uint8_t> h;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(header, ec) || std::filesystem::file_size(header, ec) > 4096 || !ReadWhole(header, h)
            || h.size() < 308 || h[4] != 0 || h[5] != 0 || h[6] != 0 || h[7] != 1)
            return {};
        std::u16string name;
        for (size_t i = 0; i < 128; i++)
        {
            const char16_t c = char16_t(h[8 + i * 2] << 8 | h[9 + i * 2]);
            if (c == 0)
                break;
            if (c < 0x20)
                return {};
            name += c;
        }
        return name;
    }

    // A save in the Xenia emulator's layout (GitHub #105: the one real save a
    // player sent had it): <title 454107D9>/<content type 00000001>/<NAME>/<NAME>,
    // with <title>/Headers/00000001/<NAME>.header beside. A dropped 454107D9/
    // folder, or its 00000001/ alone: each <NAME>/<NAME> in it whose first
    // bytes are a save's is brought in as a bare file is (its display name
    // from the .header when there is one), and the dropped folder moves to
    // imported/ once every save in it is in (otherwise it stays, and says why
    // in import-log.txt). False when the folder holds no such save.
    inline bool ImportXeniaFolder(const std::filesystem::path& saveDir, const std::filesystem::path& folder, std::vector<Report>& reports)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        char title[9];
        snprintf(title, sizeof(title), "%08X", kTitleId);
        std::string name = Utf8(folder.filename());
        for (char& c : name)
            c = char(toupper((unsigned char)c));
        fs::path saves, headers;
        if (name == title)
        {
            saves = folder / "00000001";
            headers = folder / "Headers" / "00000001";
        }
        else if (name == "00000001")
            saves = folder;
        else
            return false;
        std::vector<fs::path> found;
        for (const auto& entry : fs::directory_iterator(saves, ec))
        {
            const fs::path inner = entry.path() / entry.path().filename();
            uint8_t magic[4] = {};
            std::ifstream f(inner, std::ios::binary);
            if (fs::is_directory(entry.symlink_status(ec)) && fs::is_regular_file(fs::symlink_status(inner, ec))
                && fs::file_size(inner, ec) <= kMaxPackageBytes && f.read(reinterpret_cast<char*>(magic), 4)
                && (memcmp(magic, "MC02", 4) == 0 || memcmp(magic, "20CM", 4) == 0))
                found.push_back(inner);
        }
        if (found.empty())
            return false;
        std::sort(found.begin(), found.end());
        const size_t first = reports.size();
        bool allIn = true;
        for (const fs::path& inner : found)
        {
            const std::u16string shown = headers.empty() ? std::u16string() : XeniaDisplayName(headers / (Utf8(inner.filename()) + ".header"));
            Report r = ImportFile(saveDir, inner, true, shown);
            r.source = folder;
            const fs::path shownPath = fs::relative(inner, folder.parent_path(), ec);
            r.detail = Utf8(ec || shownPath.empty() ? inner.filename() : shownPath) + " (Xenia's layout): " + r.detail;
            allIn = allIn && r.outcome == Outcome::Imported;
            reports.push_back(std::move(r));
        }
        if (allIn)
        {
            // imported/ beside it: in the save folder, or in a drop folder.
            fs::create_directories(folder.parent_path() / kImportedFolder, ec);
            const fs::path to = FreeImportedPath(folder.parent_path(), folder);
            if (!to.empty())
                fs::rename(folder, to, ec);
            const std::string where = to.empty() || ec ? std::string("; the dropped folder stays where it is (it could not be moved)")
                                                       : "; the dropped folder is now imported/" + Utf8(to.filename());
            for (size_t i = first; i < reports.size(); i++)
                reports[i].detail += where;
        }
        return true;
    }

    // A drop folder's <NAME>/ holding a save <NAME>: a save's own folder,
    // copied from the save folder of another computer (with or without the
    // <NAME>.xcontent beside it). Its save comes in as a bare file does (the
    // display name from that .xcontent when there is one); then the folder,
    // and that .xcontent, move to imported/ beside them. False for any other
    // folder (the game's files, anything else): nothing in it is read.
    inline bool ImportSaveFolder(const std::filesystem::path& saveDir, const std::filesystem::path& folder, Report& r)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const std::string name = Utf8(folder.filename());
        const fs::path inner = folder / name;
        if (!UsableName(name) || !fs::is_regular_file(fs::symlink_status(inner, ec)) || fs::file_size(inner, ec) > kMaxPackageBytes || ec)
            return false;
        {
            uint8_t magic[4] = {};
            std::ifstream f(inner, std::ios::binary);
            if (!f.read(reinterpret_cast<char*>(magic), 4) || (memcmp(magic, "MC02", 4) != 0 && memcmp(magic, "20CM", 4) != 0))
                return false;
        }
        const fs::path xcontent = folder.parent_path() / (name + ".xcontent");
        r = ImportFile(saveDir, inner, true, XeniaDisplayName(xcontent));
        r.source = folder;
        r.detail = name + "/" + name + " (a save's folder): " + r.detail;
        if (r.outcome != Outcome::Imported)
            return true;
        fs::create_directories(folder.parent_path() / kImportedFolder, ec);
        const fs::path to = FreeImportedPath(folder.parent_path(), folder);
        if (to.empty() || (fs::rename(folder, to, ec), ec))
        {
            r.detail += "; the dropped folder stays where it is (it could not be moved)";
            return true;
        }
        r.detail += "; the dropped folder is now imported/" + Utf8(to.filename());
        if (fs::is_regular_file(fs::symlink_status(xcontent, ec)))
        {
            const fs::path x = FreeImportedPath(folder.parent_path(), xcontent);
            if (!x.empty() && (fs::rename(xcontent, x, ec), !ec))
                r.detail += ", its .xcontent imported/" + Utf8(x.filename());
        }
        return true;
    }

    // The game's own files in a drop folder (Save Bug Report's .zip on iOS:
    // report/bug_report.cpp), not named in the log.
    inline constexpr const char* kBugReportPrefix = "speedbreaker-bug-report-";

    // A drop folder's file that is neither a save nor a package, for the log:
    // the player put it where saves go and is told why nothing happened.
    inline Report NotASave(const std::filesystem::path& file, uintmax_t size, const uint8_t* magic)
    {
        Report r;
        r.source = file;
        r.outcome = Outcome::NotThisGame;
        if (size < 28)
            r.detail = std::to_string(size) + " bytes, too short for a save or a save package";
        else if (memcmp(magic, "PK\3\4", 4) == 0)
            r.detail = "a zip file: unzip it in this folder (in the Files app: tap it), then start the game again";
        else
            r.detail = "it doesn't start as a save (\"MC02\") or an Xbox 360 package (\"CON \", \"LIVE\", \"PIRS\") does";
        return r;
    }

    // Every start, before the saves are listed (kernel/xam.cpp). Looks at
    // what is directly in the save folder (and inside a folder in Xenia's
    // layout: ImportXeniaFolder); reads a file's first four bytes
    // before anything more. Returns what it did, one entry per thing it
    // took for a save (nothing for other files).
    // `dropDir`, when given, is another folder looked at instead (iOS: the
    // app's Documents, which the Files app shows; the save folder itself is
    // out of the player's reach), whose saves are brought into `saveDir`. Its
    // files are looked at the same way; a file brought in moves to imported/
    // in `dropDir`, where the player sees it; every other file there that
    // could be a save by its size (64 MB at most; not hidden, not a .png or
    // .xcontent, not the game's own bug report) gets a line saying why it
    // was not taken. Of its folders, only a save's own (<NAME>/<NAME>:
    // ImportSaveFolder) and Xenia's layout are taken; any other folder (the
    // game files live in folders there) is left unread.
    inline std::vector<Report> ImportDroppedSaves(const std::filesystem::path& saveDir, const std::filesystem::path& dropDir = {})
    {
        namespace fs = std::filesystem;
        std::vector<Report> reports;
        std::error_code ec;
        std::vector<fs::path> files, folders;
        const bool elsewhere = !dropDir.empty();
        for (const auto& entry : fs::directory_iterator(elsewhere ? dropDir : saveDir, ec))
        {
            const std::string name = Utf8(entry.path().filename());
            if (name.empty() || name[0] == '.' || name == kImportedFolder || name == kLogName)
                continue;
            const fs::file_status status = entry.symlink_status(ec);
            if (fs::is_directory(status))
                folders.push_back(entry.path());
            else if (fs::is_regular_file(status) && entry.path().extension() != ".xcontent" && entry.path().extension() != ".png"
                && !(elsewhere && name.rfind(kBugReportPrefix, 0) == 0))
                files.push_back(entry.path());
        }
        std::sort(files.begin(), files.end());
        std::sort(folders.begin(), folders.end());

        for (const fs::path& file : files)
        {
            const uintmax_t size = fs::file_size(file, ec);
            if (ec || size > kMaxPackageBytes)
                continue;
            uint8_t magic[4] = {};
            bool gotMagic = false;
            if (size >= 28)
            {
                std::ifstream f(file, std::ios::binary);
                gotMagic = bool(f.read(reinterpret_cast<char*>(magic), 4));
            }
            if (!gotMagic || (!IsStfsMagic(magic) && memcmp(magic, "MC02", 4) != 0 && memcmp(magic, "20CM", 4) != 0))
            {
                if (elsewhere && (gotMagic || size < 28))
                    reports.push_back(NotASave(file, size, magic));
                continue;
            }
            reports.push_back(ImportFile(saveDir, file));
        }
        for (const fs::path& folder : folders)
        {
            if (ImportXeniaFolder(saveDir, folder, reports))
                continue;
            Report r;
            if (elsewhere ? ImportSaveFolder(saveDir, folder, r) : AdoptFolder(saveDir, folder, r))
                reports.push_back(std::move(r));
        }
        return reports;
    }

    // What the last start found, for the player (and for a bug report): one
    // line each, in import-log.txt in the folder looked at (the save folder;
    // on iOS also the Documents folder, for what was put there). Written only
    // when there is something to say.
    inline std::string ReportLine(const Report& r)
    {
        std::string line = Utf8(r.source.filename()) + ": " + OutcomeText(r.outcome);
        if (!r.name.empty() && r.outcome == Outcome::Imported)
            line += " as \"" + r.name + "\"";
        if (!r.detail.empty())
            line += ": " + r.detail;
        return line;
    }

    inline void WriteLog(const std::filesystem::path& saveDir, const std::vector<Report>& reports)
    {
        if (reports.empty())
            return;
        std::ofstream f(saveDir / kLogName, std::ios::trunc);
        f << "SpeedBreaker: what the last start did with the files put in this folder.\n"
             "A save file or an Xbox 360 save package put here is brought in at the next start;\n"
             "the file itself is moved to the \"imported\" folder. Nothing here is ever overwritten or deleted.\n\n";
        for (const Report& r : reports)
            f << ReportLine(r) << "\n";
    }
}
