// Tests for save_import (runtime/kernel/save_import.h; GitHub issues #83,
// #105): a save put in the save folder from outside becomes a save the game
// lists, and nothing that is there is ever overwritten or deleted.
//   - the game's file (EA "MC02"): the checksum against known values; a save
//     built here read back; damaged, cut short, another game's, the PC one;
//   - STFS packages built here the way a console lays them out (a pair of
//     hash tables: "CON "; one: "LIVE"): the file found by a contiguous run,
//     by the first table's chain with its blocks out of order, by the second
//     table's when the first is stale, and past the first 0xAA blocks;
//   - the save folder: a package named after its save, a bare file, a folder
//     without its .xcontent; then everything that must change NOTHING (a
//     save of that name already there, readable or not; damaged files; other
//     games' files; other files), each checked on a byte-for-byte picture of
//     the folder before and after;
//   - a drop folder apart from the save folder (iOS: the app's Documents):
//     what was brought in moves to imported/ there and the log goes there;
//     every other file there gets its reason; a save's folder and Xenia's
//     layout come in from it; the game's files beside them are not touched.
// No game data: the saves here are made up (zeros and a counter). The
// argument is a scratch folder. `--dump <file>` reads a real package or
// save and prints what it finds (by hand; not a test). From the repo root:
//   clang++ -std=c++20 -O2 -UNDEBUG -Iruntime tests/save_import_test.cpp -o build/save_import_test
//   build/save_import_test build/save_import_scratch
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "kernel/save_import.h"

using namespace save_import;
namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

static int failures = 0;
#define CHECK(cond)                                                                  \
    do                                                                               \
    {                                                                                \
        if (!(cond))                                                                 \
        {                                                                            \
            fprintf(stderr, "%s:%d: FAILED: %s\n", __FILE__, __LINE__, #cond);       \
            failures++;                                                              \
        }                                                                            \
    } while (0)

namespace
{
    void Put32(Bytes& b, size_t at, uint32_t v)
    {
        b[at] = uint8_t(v >> 24);
        b[at + 1] = uint8_t(v >> 16);
        b[at + 2] = uint8_t(v >> 8);
        b[at + 3] = uint8_t(v);
    }

    void Put24Le(Bytes& b, size_t at, uint32_t v)
    {
        b[at] = uint8_t(v);
        b[at + 1] = uint8_t(v >> 8);
        b[at + 2] = uint8_t(v >> 16);
    }

    // A made-up save with the game's layout: header, part 1 (version, size),
    // part 2 (kSaveSize bytes: a counter, the tag, the profile's name).
    Bytes MakeSave(const std::string& alias, uint32_t tag = kKnownDataTag, uint32_t version = kSaveVersion, uint8_t seed = 1)
    {
        Bytes f(28 + 8 + kSaveSize, 0);
        memcpy(&f[0], "MC02", 4);
        Put32(f, 4, uint32_t(f.size()));
        Put32(f, 8, 8);
        Put32(f, 12, kSaveSize);
        Put32(f, 28, version);
        Put32(f, 32, kSaveSize);
        for (size_t i = 36; i < f.size(); i++)
            f[i] = uint8_t(i * 7 + seed);
        Put32(f, kTagOffset, tag);
        memset(&f[kAliasOffset], 0, 32);
        memcpy(&f[kAliasOffset], alias.data(), alias.size());
        const auto md5 = Md5(&f[kMd5From], f.size() - kMd5From - kMd5Bytes);
        memcpy(&f[f.size() - kMd5Bytes], md5.data(), kMd5Bytes);
        Put32(f, 16, EaCrc(&f[28], 8));
        Put32(f, 20, EaCrc(&f[36], kSaveSize));
        Put32(f, 24, EaCrc(&f[0], 24));
        return f;
    }

    struct PackageOptions
    {
        const char* magic = "CON ";
        uint32_t titleId = kTitleId;
        uint32_t contentType = 1;
        bool contiguousFlag = true;   // the file table says the file's blocks are in a row
        bool reversed = false;        // the file's blocks laid out last to first (only a chain finds them)
        bool chainInSecond = false;   // the chain is in the second table of each pair; the first is stale
        uint32_t fillerBlocks = 0;    // another file's blocks before the save's
        std::u16string displayName = u"Synthetic save";
    };

    // A package as a console writes one: the header (0x971A bytes signed by a
    // console, 0xAD0E by Microsoft) up to a block, then for each 0xAA data
    // blocks a hash table (a pair for a console's), and after the first 0xAA
    // the table above them. Laid out by walking the file, not by the
    // reader's formula.
    Bytes MakePackage(const Bytes& inner, const std::string& name, const PackageOptions& o = {})
    {
        const bool con = memcmp(o.magic, "CON ", 4) == 0;
        const uint32_t headerSize = con ? 0x971A : 0xAD0E;
        const size_t base = (headerSize + 0xFFF) & ~size_t(0xFFF);
        const uint32_t tables = con ? 2 : 1;
        const uint32_t fileBlocks = uint32_t((inner.size() + 0xFFF) >> 12);
        const uint32_t firstFile = 1 + o.fillerBlocks;  // block 0: the file table
        const uint32_t total = firstFile + fileBlocks;

        std::vector<uint32_t> order(fileBlocks);  // the file's i-th block is data block order[i]
        for (uint32_t i = 0; i < fileBlocks; i++)
            order[i] = o.reversed ? firstFile + fileBlocks - 1 - i : firstFile + i;

        std::vector<Bytes> blocks(total, Bytes(0x1000, 0));
        std::vector<uint32_t> next(total, 0xFFFFFF);
        Bytes& table = blocks[0];
        size_t e = 0;
        if (o.fillerBlocks)
        {
            memcpy(&table[e], "filler.bin", 10);
            table[e + 0x28] = 10 | 0x40;
            Put24Le(table, e + 0x29, o.fillerBlocks);
            Put24Le(table, e + 0x2C, o.fillerBlocks);
            Put24Le(table, e + 0x2F, 1);
            table[e + 0x32] = table[e + 0x33] = 0xFF;
            Put32(table, e + 0x34, o.fillerBlocks << 12);
            for (uint32_t i = 1; i < firstFile; i++)
            {
                next[i] = i + 1 < firstFile ? i + 1 : 0xFFFFFF;
                memset(blocks[i].data(), 0x5A, 0x1000);
            }
            e += 0x40;
        }
        memcpy(&table[e], name.data(), name.size());
        table[e + 0x28] = uint8_t(name.size()) | ((o.contiguousFlag && !o.reversed) ? 0x40 : 0);
        Put24Le(table, e + 0x29, fileBlocks);
        Put24Le(table, e + 0x2C, fileBlocks);
        Put24Le(table, e + 0x2F, order[0]);
        table[e + 0x32] = table[e + 0x33] = 0xFF;
        Put32(table, e + 0x34, uint32_t(inner.size()));
        for (uint32_t i = 0; i < fileBlocks; i++)
        {
            const size_t n = std::min<size_t>(0x1000, inner.size() - size_t(i) * 0x1000);
            memcpy(blocks[order[i]].data(), &inner[size_t(i) * 0x1000], n);
            next[order[i]] = i + 1 < fileBlocks ? order[i + 1] : 0xFFFFFF;
        }

        Bytes out(base, 0);
        memcpy(&out[0], o.magic, 4);
        Put32(out, 0x340, headerSize);
        Put32(out, 0x344, o.contentType);
        Put32(out, 0x360, o.titleId);
        const uint8_t ids[13] = { 1, 2, 3, 4, 5, 0xE0, 0, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };  // a console's and a profile's
        memcpy(&out[0x36C], ids, 13);
        out[0x379] = 0x24;
        out[0x37B] = o.chainInSecond ? 2 : 0;
        out[0x37C] = 1;  // one block of file table, at block 0
        Put32(out, 0x395, total);
        for (size_t i = 0; i < o.displayName.size(); i++)
        {
            out[0x411 + i * 2] = uint8_t(o.displayName[i] >> 8);
            out[0x412 + i * 2] = uint8_t(o.displayName[i]);
        }

        for (uint32_t group = 0; group * 0xAA < total; group++)
        {
            if (group == 1)
                out.resize(out.size() + size_t(tables) * 0x1000, 0);  // the table above the level 0 tables
            const uint32_t first = group * 0xAA, last = std::min(total, first + 0xAA);
            for (uint32_t t = 0; t < tables; t++)
            {
                Bytes hash(0x1000, 0);
                const bool live = !con || (t == 1) == o.chainInSecond;
                for (uint32_t b = first; b < last; b++)
                {
                    const size_t at = size_t(b - first) * 0x18;
                    hash[at + 0x14] = 0x80;
                    // The stale table of a pair: every block "ends" its file.
                    const uint32_t n = live ? next[b] : 0xFFFFFF;
                    hash[at + 0x15] = uint8_t(n >> 16);
                    hash[at + 0x16] = uint8_t(n >> 8);
                    hash[at + 0x17] = uint8_t(n);
                }
                out.insert(out.end(), hash.begin(), hash.end());
            }
            for (uint32_t b = first; b < last; b++)
                out.insert(out.end(), blocks[b].begin(), blocks[b].end());
        }
        return out;
    }

    void WriteFile(const fs::path& path, const Bytes& bytes)
    {
        fs::create_directories(path.parent_path());
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    }

    Bytes ReadFile(const fs::path& path)
    {
        Bytes b;
        ReadWhole(path, b);
        return b;
    }

    // Every file and folder under `dir`, with each file's bytes.
    std::map<std::string, Bytes> Picture(const fs::path& dir)
    {
        std::map<std::string, Bytes> p;
        for (const auto& entry : fs::recursive_directory_iterator(dir))
        {
            const std::string name = Utf8(fs::relative(entry.path(), dir));
            p[name] = entry.is_directory() ? Bytes{ 'd', 'i', 'r' } : ReadFile(entry.path());
            if (p[name].size() > (1u << 20))  // a stand-in disc image: its size and sum, not its bytes
            {
                const std::string sum = std::to_string(p[name].size()) + " " + std::to_string(EaCrc(p[name].data(), p[name].size()));
                p[name] = Bytes(sum.begin(), sum.end());
            }
        }
        return p;
    }

    const Report* Find(const std::vector<Report>& reports, const std::string& file)
    {
        for (const Report& r : reports)
            if (Utf8(r.source.filename()) == file)
                return &r;
        return nullptr;
    }

    fs::path Fresh(const fs::path& scratch, const char* name)
    {
        const fs::path dir = scratch / name;
        fs::create_directories(dir);
        return dir;
    }

    void FileTests()
    {
        // The checksum (tools: mc02.py's, which matched 537 saves the game wrote).
        const char* text = "SpeedBreaker save import";
        CHECK(EaCrc(reinterpret_cast<const uint8_t*>(text), strlen(text)) == 0x58E8A4D4);
        Bytes count(64);
        for (size_t i = 0; i < 64; i++)
            count[i] = uint8_t(i);
        CHECK(EaCrc(count.data(), count.size()) == 0x6FA92718);

        const Bytes save = MakeSave("ALEX");
        CHECK(save.size() == 62688);
        SaveCheck c = CheckSave(save);
        CHECK(c.ok && c.alias == "ALEX" && c.tag == kKnownDataTag && c.version == 269 && c.blobSize == 62652);

        Bytes bad = save;
        bad[40000] ^= 1;  // one bit in the career
        CHECK(!CheckSave(bad).ok && !CheckSave(bad).otherGame);
        bad = save;
        bad[9] ^= 1;  // the header
        CHECK(!CheckSave(bad).ok);
        bad = save;
        bad.resize(bad.size() - 1);  // cut short
        CHECK(!CheckSave(bad).ok);
        bad = save;
        bad.push_back(0);  // longer than its header says
        CHECK(!CheckSave(bad).ok);
        bad = save;
        memcpy(&bad[0], "20CM", 4);
        CHECK(!CheckSave(bad).ok);
        CHECK(!CheckSave(Bytes(10, 0)).ok);
        CHECK(!CheckSave(Bytes()).ok);
        // MD5 (RFC 1321's own examples), and a save whose career was changed
        // with the CRCs made right again but not the MD5: the game refuses
        // it, so this does.
        auto hex = [](const std::array<uint8_t, 16>& d) {
            std::string out;
            char two[3];
            for (uint8_t v : d)
            {
                snprintf(two, sizeof(two), "%02x", v);
                out += two;
            }
            return out;
        };
        CHECK(hex(Md5(nullptr, 0)) == "d41d8cd98f00b204e9800998ecf8427e");
        CHECK(hex(Md5(reinterpret_cast<const uint8_t*>("abc"), 3)) == "900150983cd24fb0d6963f7d28e17f72");
        const char* eighty = "12345678901234567890123456789012345678901234567890123456789012345678901234567890";
        CHECK(hex(Md5(reinterpret_cast<const uint8_t*>(eighty), 80)) == "57edf4a22be3c955ac49da2e2107b67a");
        bad = save;
        bad[40000] ^= 1;
        Put32(bad, 20, EaCrc(&bad[36], kSaveSize));
        Put32(bad, 24, EaCrc(&bad[0], 24));
        c = CheckSave(bad);
        CHECK(!c.ok && !c.otherGame && c.why.find("MD5") != std::string::npos);
        bad = save;
        bad[bad.size() - 1] ^= 1;  // the MD5 itself
        Put32(bad, 20, EaCrc(&bad[36], kSaveSize));
        Put32(bad, 24, EaCrc(&bad[0], 24));
        c = CheckSave(bad);
        CHECK(!c.ok && c.why.find("MD5") != std::string::npos);
        // Whole, but another version of the save: not this game's.
        c = CheckSave(MakeSave("ALEX", kKnownDataTag, 270));
        CHECK(!c.ok && c.otherGame);
        // Another tag is still a whole save (the game decides).
        c = CheckSave(MakeSave("ALEX", 0x12345678));
        CHECK(c.ok && c.tag == 0x12345678);
    }

    void ExpectSave(const Bytes& package, const std::string& name, const Bytes& save)
    {
        StfsPackage p;
        std::string why, found;
        Bytes out;
        SaveCheck check;
        CHECK(ParseStfs(package, p, why));
        CHECK(StfsFindSave(p, found, out, check, why));
        CHECK(found == name);
        CHECK(out == save);
        CHECK(check.ok);
    }

    void PackageTests()
    {
        const Bytes save = MakeSave("ALEX");
        // A console's package: header 0x971A, data from 0xC000 (a table pair first).
        Bytes con = MakePackage(save, "ALEX");
        CHECK(con.size() == 0xC000 + 17 * 0x1000);
        StfsPackage p;
        std::string why;
        CHECK(ParseStfs(con, p, why));
        CHECK(p.shift == 1 && p.dataBase == 0xA000 && p.titleId == kTitleId && p.contentType == 1);
        CHECK(p.displayName == u"Synthetic save" && p.profileId[0] == 0xE0 && p.consoleId[4] == 5);
        CHECK(StfsBlockOffset(p, 0) == 0xC000 && StfsBlockOffset(p, 1) == 0xD000);
        const auto files = StfsFiles(p, StfsChain::TableA);
        CHECK(files.size() == 1 && files[0].name == "ALEX" && files[0].size == save.size() && files[0].contiguous && !files[0].directory);
        ExpectSave(con, "ALEX", save);

        PackageOptions o;
        o.reversed = true;  // only the chain finds the blocks
        ExpectSave(MakePackage(save, "ALEX", o), "ALEX", save);
        o.chainInSecond = true;  // and the chain is in the pair's second table
        ExpectSave(MakePackage(save, "ALEX", o), "ALEX", save);
        o = {};
        o.contiguousFlag = false;
        ExpectSave(MakePackage(save, "ALEX", o), "ALEX", save);

        // Microsoft-signed layout (one table, data from 0xC000 as well: header 0xB000 + a table).
        o = {};
        o.magic = "LIVE";
        Bytes live = MakePackage(save, "ALEX", o);
        CHECK(ParseStfs(live, p, why) && p.shift == 0 && p.dataBase == 0xB000 && StfsBlockOffset(p, 0) == 0xC000);
        ExpectSave(live, "ALEX", save);
        o.reversed = true;
        ExpectSave(MakePackage(save, "ALEX", o), "ALEX", save);

        // The save's blocks past the first 0xAA (a second table, and the one above), both layouts.
        for (const char* magic : { "CON ", "LIVE", "PIRS" })
        {
            for (bool reversed : { false, true })
            {
                o = {};
                o.magic = magic;
                o.fillerBlocks = 0xAA + 0xAA - 9;  // the save straddles the second and third runs
                o.reversed = reversed;
                ExpectSave(MakePackage(save, "ALEX", o), "ALEX", save);
            }
        }

        // What must not parse or yield a save.
        CHECK(!ParseStfs(Bytes(100, 0), p, why));
        Bytes cut = con;
        cut.resize(0x9000);
        CHECK(!ParseStfs(cut, p, why));
        cut = con;
        cut.resize(0xC000 + 5 * 0x1000);  // the save's last blocks are gone
        std::string name;
        Bytes out;
        SaveCheck check;
        CHECK(ParseStfs(cut, p, why) && !StfsFindSave(p, name, out, check, why));
        Bytes damaged = con;
        damaged[0xC000 + 0x1000 * 6 + 17] ^= 0x40;  // one bit in the save
        CHECK(ParseStfs(damaged, p, why) && !StfsFindSave(p, name, out, check, why));
        // A file table that points outside the package.
        Bytes wild = con;
        Put24Le(wild, 0xC000 + 0x2F, 0xFFFF00);
        CHECK(ParseStfs(wild, p, why) && !StfsFindSave(p, name, out, check, why));
        Bytes huge = con;
        Put32(huge, 0xC000 + 0x34, 0xFFFFFFFF);
        CHECK(ParseStfs(huge, p, why) && !StfsFindSave(p, name, out, check, why));
    }

    void XContentTests()
    {
        // As the runtime's own (a save the game made: device 1, type 1, the
        // name in UTF-16 at 8 and in bytes at 0x108, 308 bytes).
        const Bytes x = MakeXContent("ALEX", u"");
        CHECK(x.size() == 308 && Be32(&x[0]) == 1 && Be32(&x[4]) == 1);
        CHECK(x[8] == 0 && x[9] == 'A' && x[14] == 0 && x[15] == 'X' && x[16] == 0 && x[17] == 0);
        CHECK(memcmp(&x[0x108], "ALEX\0", 5) == 0);
        const Bytes y = MakeXContent("ALEX", u"My save");
        CHECK(y[9] == 'M' && y[21] == 'e' && y[23] == 0 && memcmp(&y[0x108], "ALEX\0", 5) == 0);
        CHECK(UsableName("ALEX") && UsableName("RACER 7") && UsableName("A_B-C"));
        CHECK(!UsableName("") && !UsableName(".hidden") && !UsableName("a/b") && !UsableName("a\\b") && !UsableName("a:b")
            && !UsableName("..") && !UsableName("imported") && !UsableName(std::string(42, 'A')) && !UsableName("tab\tname")
            && !UsableName("dot."));
    }

    void FolderTests(const fs::path& scratch)
    {
        const Bytes alex = MakeSave("ALEX", kKnownDataTag, kSaveVersion, 1);
        const Bytes other = MakeSave("ALEX", kKnownDataTag, kSaveVersion, 2);  // a different career under the same name

        // 1. A console's package, named after its save as a console names it.
        {
            const fs::path dir = Fresh(scratch, "package");
            const Bytes package = MakePackage(alex, "ALEX");
            WriteFile(dir / "ALEX", package);
            const auto reports = ImportDroppedSaves(dir);
            CHECK(reports.size() == 1 && reports[0].outcome == Outcome::Imported && reports[0].name == "ALEX");
            CHECK(ReadFile(dir / "ALEX" / "ALEX") == alex);
            const Bytes x = ReadFile(dir / "ALEX.xcontent");
            CHECK(x.size() == 308 && memcmp(&x[0x108], "ALEX\0", 5) == 0 && x[9] == 'S');  // the package's display name
            CHECK(ReadFile(dir / "imported" / "ALEX") == package);  // what was dropped is kept, whole
            WriteLog(dir, reports);
            const Bytes log = ReadFile(dir / "import-log.txt");
            CHECK(std::string(log.begin(), log.end()).find("ALEX: imported as \"ALEX\"") != std::string::npos);
            // The next start: nothing more to do, nothing changes.
            const auto before = Picture(dir);
            CHECK(ImportDroppedSaves(dir).empty());
            CHECK(Picture(dir) == before);
        }

        // 2. A bare save file under any name: named for the profile inside it.
        {
            const fs::path dir = Fresh(scratch, "bare");
            WriteFile(dir / "my old save.bin", MakeSave("RACER7"));
            WriteFile(dir / "notes.txt", Bytes{ 'h', 'i' });
            WriteFile(dir / "big.dat", Bytes(200000, 0x33));
            WriteFile(dir / ".DS_Store", Bytes{ 'M', 'C', '0', '2' });
            const auto reports = ImportDroppedSaves(dir);
            CHECK(reports.size() == 1 && reports[0].outcome == Outcome::Imported && reports[0].name == "RACER7");
            CHECK(ReadFile(dir / "RACER7" / "RACER7") == MakeSave("RACER7"));
            CHECK(fs::exists(dir / "RACER7.xcontent") && fs::exists(dir / "imported" / "my old save.bin"));
            CHECK(ReadFile(dir / "notes.txt") == (Bytes{ 'h', 'i' }) && ReadFile(dir / "big.dat") == Bytes(200000, 0x33));
            CHECK(fs::exists(dir / ".DS_Store"));
        }

        // 3. A bare save file named as its save (a tool took it out of a package).
        {
            const fs::path dir = Fresh(scratch, "bare-same-name");
            WriteFile(dir / "ALEX", alex);
            const auto reports = ImportDroppedSaves(dir);
            CHECK(reports.size() == 1 && reports[0].outcome == Outcome::Imported);
            CHECK(ReadFile(dir / "ALEX" / "ALEX") == alex && ReadFile(dir / "imported" / "ALEX") == alex);
        }

        // 4. A save's folder copied from another install without its .xcontent.
        {
            const fs::path dir = Fresh(scratch, "adopt");
            WriteFile(dir / "ALEX" / "ALEX", alex);
            const auto reports = ImportDroppedSaves(dir);
            CHECK(reports.size() == 1 && reports[0].outcome == Outcome::Adopted);
            CHECK(ReadFile(dir / "ALEX" / "ALEX") == alex && ReadFile(dir / "ALEX.xcontent") == MakeXContent("ALEX", u""));
            CHECK(!fs::exists(dir / "imported"));
        }

        // 5. NOTHING changes: a save of that name is already there. Once a
        // readable one, once one the game can't read (garbage), once a
        // folder with no .xcontent at all. The dropped files stay too.
        for (int kind = 0; kind < 3; kind++)
        {
            const fs::path dir = Fresh(scratch, kind == 0 ? "taken-readable" : kind == 1 ? "taken-unreadable" : "taken-folder-only");
            WriteFile(dir / "ALEX" / "ALEX", kind == 0 ? other : Bytes(5000, 0xEE));
            WriteFile(dir / "ALEX" / "extra.bin", Bytes(10, 1));
            if (kind != 2)
                WriteFile(dir / "ALEX.xcontent", MakeXContent("ALEX", u"mine"));
            WriteFile(dir / "from-console.bin", MakePackage(alex, "ALEX"));
            WriteFile(dir / "bare.sav", alex);
            const auto before = Picture(dir);
            const auto reports = ImportDroppedSaves(dir);
            CHECK(Picture(dir) == before);
            const Report* a = Find(reports, "from-console.bin");
            const Report* b = Find(reports, "bare.sav");
            CHECK(a && a->outcome == Outcome::NameTaken && b && b->outcome == Outcome::NameTaken);
            if (kind == 2)
            {
                // The folder is looked at as well (no .xcontent): unreadable, left alone.
                const Report* f = Find(reports, "ALEX");
                CHECK(f && f->outcome == Outcome::Unreadable && !fs::exists(dir / "ALEX.xcontent"));
            }
            else
                CHECK(reports.size() == 2);
            // And again at the next start.
            ImportDroppedSaves(dir);
            CHECK(Picture(dir) == before);
        }

        // 6. NOTHING changes: files that are not a whole Most Wanted save.
        {
            const fs::path dir = Fresh(scratch, "refused");
            Bytes damaged = MakePackage(alex, "ALEX");
            damaged[0xC000 + 0x1000 * 3] ^= 1;
            WriteFile(dir / "damaged-package", damaged);
            Bytes cut = MakePackage(alex, "ALEX");
            cut.resize(0xF000);
            WriteFile(dir / "cut-package", cut);
            WriteFile(dir / "short-package", Bytes{ 'C', 'O', 'N', ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 });
            PackageOptions o;
            o.titleId = 0x454108CE;
            WriteFile(dir / "other-title", MakePackage(alex, "ALEX", o));
            o = {};
            o.contentType = 2;
            WriteFile(dir / "not-a-save-package", MakePackage(alex, "ALEX", o));
            WriteFile(dir / "bad-name-package", MakePackage(alex, "../ALEX"));
            Bytes bad = alex;
            bad[30000] ^= 0x80;
            WriteFile(dir / "damaged.sav", bad);
            Bytes pc = alex;
            memcpy(&pc[0], "20CM", 4);
            WriteFile(dir / "pc.sav", pc);
            WriteFile(dir / "other-version.sav", MakeSave("ALEX", kKnownDataTag, 270));
            WriteFile(dir / "ALEX.thumbnail.png", Bytes{ 'M', 'C', '0', '2', 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26 });
            const auto before = Picture(dir);
            const auto reports = ImportDroppedSaves(dir);
            CHECK(Picture(dir) == before);
            CHECK(reports.size() == 9);
            auto outcome = [&](const char* file) {
                const Report* r = Find(reports, file);
                CHECK(r != nullptr);
                return r ? r->outcome : Outcome::Imported;
            };
            CHECK(outcome("damaged-package") == Outcome::Unreadable);
            CHECK(outcome("cut-package") == Outcome::Unreadable);
            CHECK(outcome("short-package") == Outcome::Unreadable);
            CHECK(outcome("other-title") == Outcome::NotThisGame);
            CHECK(outcome("not-a-save-package") == Outcome::NotThisGame);
            CHECK(outcome("bad-name-package") == Outcome::Unreadable);
            CHECK(outcome("damaged.sav") == Outcome::Unreadable);
            CHECK(outcome("pc.sav") == Outcome::NotThisGame);
            CHECK(outcome("other-version.sav") == Outcome::NotThisGame);
            for (const Report& r : reports)
                CHECK(!r.detail.empty());
        }

        // 7. Two dropped files with the same save in them: the first (by
        // name) comes in, the second stays where it is.
        {
            const fs::path dir = Fresh(scratch, "twice");
            const Bytes first = MakePackage(alex, "ALEX"), second = MakePackage(other, "ALEX");
            WriteFile(dir / "a.bin", first);
            WriteFile(dir / "b.bin", second);
            const auto reports = ImportDroppedSaves(dir);
            CHECK(reports.size() == 2 && reports[0].outcome == Outcome::Imported && reports[1].outcome == Outcome::NameTaken);
            CHECK(ReadFile(dir / "ALEX" / "ALEX") == alex && ReadFile(dir / "b.bin") == second && ReadFile(dir / "imported" / "a.bin") == first);
        }

        // 8. imported/ already holds a file of that name: it is kept, the
        // new one goes beside it.
        {
            const fs::path dir = Fresh(scratch, "imported-twice");
            WriteFile(dir / "imported" / "ALEX", Bytes(7, 9));
            WriteFile(dir / "ALEX", alex);
            const auto reports = ImportDroppedSaves(dir);
            CHECK(reports.size() == 1 && reports[0].outcome == Outcome::Imported);
            CHECK(ReadFile(dir / "imported" / "ALEX") == Bytes(7, 9) && ReadFile(dir / "imported" / "ALEX-2") == alex);
            CHECK(ReadFile(dir / "ALEX" / "ALEX") == alex);
        }

        // 9. A save from another edition (another data tag) comes in; the
        // report says the game may refuse it.
        {
            const fs::path dir = Fresh(scratch, "other-tag");
            WriteFile(dir / "x.sav", MakeSave("PAL", 0x0BADF00D));
            const auto reports = ImportDroppedSaves(dir);
            CHECK(reports.size() == 1 && reports[0].outcome == Outcome::Imported && reports[0].detail.find("0BADF00D") != std::string::npos);
        }

        // 10. No save folder yet, or an empty one: nothing, and no log.
        {
            CHECK(ImportDroppedSaves(scratch / "missing").empty());
            const fs::path dir = Fresh(scratch, "empty");
            const auto reports = ImportDroppedSaves(dir);
            WriteLog(dir, reports);
            CHECK(reports.empty() && fs::is_empty(dir));
        }

        // 11. A drop folder apart from the save folder (iOS: the app's
        // Documents, which the Files app shows; the save folder is out of the
        // player's reach). Its files come in as the save folder's do; what was
        // brought in moves to imported/ IN THE DROP FOLDER and the log is
        // written there (kernel/xam.cpp), where the player sees both; the save
        // folder gets the save and nothing else. The game's files there (in
        // folders), a disc image, a bug report, captures: not read, not
        // touched, not named.
        {
            const fs::path dir = Fresh(scratch, "apart-saves");
            const fs::path drop = Fresh(scratch, "apart-drop");
            const Bytes package = MakePackage(alex, "ALEX");
            WriteFile(drop / "game" / "files" / "default.xex", Bytes{ 'X', 'E', 'X', '2', 0, 1, 2, 3 });
            WriteFile(drop / "game" / "files" / "NFS" / "GLOBALB.BUN", MakeSave("DEEP"));  // even a save's bytes in there: not read
            WriteFile(drop / "game" / "game", Bytes(100, 7));                            // <NAME>/<NAME>, but no save
            WriteFile(drop / "build" / "present_check_0.ppm", Bytes(64, 1));
            WriteFile(drop / "Inbox" / "x", Bytes(30, 'M'));
            Bytes zip(60, 0);
            memcpy(&zip[0], "PK\3\4", 4);
            WriteFile(drop / "speedbreaker-bug-report-v0.2.6-2026-11-15_10-10-10.zip", zip);
            WriteFile(drop / "shot.png", MakeSave("PNG"));
            WriteFile(drop / ".DS_Store", MakeSave("HIDDEN"));
            WriteFile(drop / "Most Wanted.iso", Bytes{ 'M', 'C', '0', '2' });
            fs::resize_file(drop / "Most Wanted.iso", kMaxPackageBytes + 1);  // a disc image's size: no save is this big
            const auto others = Picture(drop);
            WriteFile(drop / "ALEX", package);

            const auto reports = ImportDroppedSaves(dir, drop);
            CHECK(reports.size() == 1 && reports[0].outcome == Outcome::Imported && reports[0].name == "ALEX");
            CHECK(ReadFile(dir / "ALEX" / "ALEX") == alex && fs::exists(dir / "ALEX.xcontent"));
            CHECK(ReadFile(drop / "imported" / "ALEX") == package && !fs::exists(drop / "ALEX"));
            CHECK(!fs::exists(dir / "imported") && reports[0].detail.find("the dropped file is now imported/ALEX") != std::string::npos);
            WriteLog(drop, reports);
            const Bytes log = ReadFile(drop / "import-log.txt");
            CHECK(std::string(log.begin(), log.end()).find("ALEX: imported as \"ALEX\"") != std::string::npos);
            CHECK(!fs::exists(dir / "import-log.txt"));
            {
                auto after = Picture(drop);
                for (const char* added : { "imported", "imported/ALEX", "import-log.txt" })
                    CHECK(after.erase(added) == 1);
                CHECK(after == others);  // everything else in the drop folder: as it was
            }
            const std::vector<std::string> saveFolder = { "ALEX", "ALEX.xcontent", "ALEX/ALEX" };
            std::vector<std::string> inSaves;
            for (const auto& [name, bytes] : Picture(dir))
                inSaves.push_back(name);
            CHECK(inSaves == saveFolder);

            // The next start: nothing new, nothing changes (the log stays as written).
            const auto saves = Picture(dir);
            const auto drops = Picture(drop);
            CHECK(ImportDroppedSaves(dir, drop).empty());
            CHECK(Picture(dir) == saves && Picture(drop) == drops);

            // The same package put in again: its name is taken; it stays where it was put.
            WriteFile(drop / "ALEX", package);
            const auto drops2 = Picture(drop);
            const auto second = ImportDroppedSaves(dir, drop);
            CHECK(second.size() == 1 && second[0].outcome == Outcome::NameTaken);
            CHECK(Picture(dir) == saves && Picture(drop) == drops2);
            WriteLog(drop, second);
            const Bytes log2 = ReadFile(drop / "import-log.txt");
            CHECK(std::string(log2.begin(), log2.end()).find("ALEX: not imported (a save of that name is already here)") != std::string::npos);
            fs::remove(drop / "ALEX");

            // Files the game can't read: refused, each left where it was, each
            // with its reason in the log. (The save folder alone, as on macOS,
            // Windows and Linux, says nothing about such files: case 13.)
            WriteFile(drop / "junk.bin", Bytes{ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 });
            WriteFile(drop / "garbage.bin", Bytes(300, 0x5A));
            Bytes cut = package;
            cut.resize(0xD000);
            WriteFile(drop / "ALEX cut", cut);
            Bytes cutSave = alex;
            cutSave.resize(40000);
            WriteFile(drop / "cut.sav", cutSave);
            WriteFile(drop / "save.zip", zip);
            WriteFile(drop / "empty", Bytes{});
            const auto drops3 = Picture(drop);
            const auto refused = ImportDroppedSaves(dir, drop);
            CHECK(Picture(dir) == saves && Picture(drop) == drops3);
            CHECK(refused.size() == 6);
            auto said = [&](const char* file, Outcome o, const char* why) {
                const Report* r = Find(refused, file);
                CHECK(r && r->outcome == o && r->detail.find(why) != std::string::npos);
            };
            said("junk.bin", Outcome::NotThisGame, "10 bytes, too short");
            said("empty", Outcome::NotThisGame, "0 bytes, too short");
            said("garbage.bin", Outcome::NotThisGame, "doesn't start as a save");
            said("save.zip", Outcome::NotThisGame, "a zip file: unzip it");
            said("ALEX cut", Outcome::Unreadable, "");
            said("cut.sav", Outcome::Unreadable, "");
            WriteLog(drop, refused);
            const Bytes log3 = ReadFile(drop / "import-log.txt");
            const std::string text3(log3.begin(), log3.end());
            for (const Report& r : refused)
                CHECK(text3.find(ReportLine(r)) != std::string::npos);
            CHECK(text3.find("bug-report") == std::string::npos && text3.find(".iso") == std::string::npos && text3.find("default.xex") == std::string::npos
                && text3.find("Inbox") == std::string::npos && text3.find("shot.png") == std::string::npos);
        }

        // 11b. A drop folder's folders: only a save's own folder (<NAME>/<NAME>,
        // copied from another computer's save folder, its .xcontent beside it
        // or not) and Xenia's layout are taken; each moves to the drop folder's
        // imported/ once its save is in. A folder whose save can't come in
        // stays as it is, with its line; any other folder is not read.
        {
            const fs::path dir = Fresh(scratch, "apart-folders-saves");
            const fs::path drop = Fresh(scratch, "apart-folders-drop");
            WriteFile(dir / "ALEX" / "ALEX", alex);
            WriteFile(dir / "ALEX.xcontent", MakeXContent("ALEX", u""));
            Bytes header = MakeXContent("MF", u"MF");
            header.resize(328, 0);
            WriteFile(drop / "454107D9" / "00000001" / "MF" / "MF", MakeSave("MF"));
            WriteFile(drop / "454107D9" / "Headers" / "00000001" / "MF.header", header);
            WriteFile(drop / "RACER7" / "RACER7", MakeSave("RACER7"));
            WriteFile(drop / "RACER7.xcontent", MakeXContent("RACER7", u"Racer seven"));
            WriteFile(drop / "BOB" / "BOB", MakeSave("BOB"));
            WriteFile(drop / "ALEX" / "ALEX", other);  // that name is taken
            Bytes bad = MakeSave("BAD");
            bad[30000] ^= 0x80;
            WriteFile(drop / "BAD" / "BAD", bad);
            WriteFile(drop / "NOTES" / "NOTES", Bytes(500, 'n'));
            WriteFile(drop / "game" / "files" / "default.xex", Bytes{ 'X', 'E', 'X', '2' });
            const auto reports = ImportDroppedSaves(dir, drop);
            CHECK(reports.size() == 5);
            auto outcome = [&](const char* source) {
                const Report* r = Find(reports, source);
                return r ? r->outcome : Outcome::Failed;
            };
            CHECK(outcome("454107D9") == Outcome::Imported && outcome("RACER7") == Outcome::Imported && outcome("BOB") == Outcome::Imported);
            CHECK(outcome("ALEX") == Outcome::NameTaken && outcome("BAD") == Outcome::Unreadable && !Find(reports, "NOTES") && !Find(reports, "game"));
            CHECK(ReadFile(dir / "MF" / "MF") == MakeSave("MF") && ReadFile(dir / "BOB" / "BOB") == MakeSave("BOB"));
            CHECK(ReadFile(dir / "RACER7" / "RACER7") == MakeSave("RACER7") && ReadFile(dir / "RACER7.xcontent") == MakeXContent("RACER7", u"Racer seven"));
            CHECK(ReadFile(dir / "ALEX" / "ALEX") == alex && !fs::exists(dir / "imported"));
            CHECK(ReadFile(drop / "imported" / "454107D9" / "00000001" / "MF" / "MF") == MakeSave("MF") && !fs::exists(drop / "454107D9"));
            CHECK(ReadFile(drop / "imported" / "RACER7" / "RACER7") == MakeSave("RACER7") && !fs::exists(drop / "RACER7"));
            CHECK(ReadFile(drop / "imported" / "RACER7.xcontent") == MakeXContent("RACER7", u"Racer seven") && !fs::exists(drop / "RACER7.xcontent"));
            CHECK(ReadFile(drop / "imported" / "BOB" / "BOB") == MakeSave("BOB") && !fs::exists(drop / "BOB"));
            CHECK(ReadFile(drop / "ALEX" / "ALEX") == other && ReadFile(drop / "BAD" / "BAD") == bad && ReadFile(drop / "NOTES" / "NOTES") == Bytes(500, 'n'));
            CHECK(ReadFile(drop / "game" / "files" / "default.xex") == (Bytes{ 'X', 'E', 'X', '2' }));
            const Report* racer = Find(reports, "RACER7");
            CHECK(racer && racer->detail.find("the dropped folder is now imported/RACER7, its .xcontent imported/RACER7.xcontent") != std::string::npos);
            // The next start: the two that stayed say so again; nothing changes.
            const auto saves = Picture(dir);
            const auto drops = Picture(drop);
            const auto again = ImportDroppedSaves(dir, drop);
            CHECK(again.size() == 2 && Picture(dir) == saves && Picture(drop) == drops);
        }

        // 12. A save in the Xenia emulator's layout (GitHub #105's real save):
        // 454107D9/00000001/<NAME>/<NAME> with Headers/00000001/<NAME>.header
        // (the XCONTENT_DATA, then the title's fields). The save comes in with
        // the header's display name and the whole folder moves to imported/;
        // a header without its save (#105's had one) is no save.
        {
            const fs::path dir = Fresh(scratch, "xenia");
            Bytes header = MakeXContent("ALEX", u"Xenia name");
            header.resize(328, 0);
            header[0x13C] = 0x45, header[0x13D] = 0x41, header[0x13E] = 0x07, header[0x13F] = 0xD9;
            WriteFile(dir / "454107D9" / "00000001" / "ALEX" / "ALEX", alex);
            WriteFile(dir / "454107D9" / "Headers" / "00000001" / "ALEX.header", header);
            WriteFile(dir / "454107D9" / "Headers" / "00000001" / "NAME.header", MakeXContent("NAME", u""));
            WriteFile(dir / "454107D9" / ".DS_Store", Bytes{ 'M', 'C', '0', '2' });
            const auto reports = ImportDroppedSaves(dir);
            CHECK(reports.size() == 1 && reports[0].outcome == Outcome::Imported && reports[0].name == "ALEX");
            CHECK(ReadFile(dir / "ALEX" / "ALEX") == alex);
            const Bytes x = ReadFile(dir / "ALEX.xcontent");
            CHECK(x.size() == 308 && x == MakeXContent("ALEX", u"Xenia name"));
            CHECK(!fs::exists(dir / "454107D9") && ReadFile(dir / "imported" / "454107D9" / "00000001" / "ALEX" / "ALEX") == alex);
            CHECK(fs::exists(dir / "imported" / "454107D9" / "Headers" / "00000001" / "NAME.header"));
            WriteLog(dir, reports);
            const Bytes log = ReadFile(dir / "import-log.txt");
            const std::string text(log.begin(), log.end());
            CHECK(text.find("454107D9: imported as \"ALEX\"") != std::string::npos && text.find("(Xenia's layout)") != std::string::npos
                && text.find("the dropped folder is now imported/454107D9") != std::string::npos);
            const auto before = Picture(dir);
            CHECK(ImportDroppedSaves(dir).empty());
            CHECK(Picture(dir) == before);
        }
        // Its 00000001/ folder alone, in lower case letters too; two saves.
        {
            const fs::path dir = Fresh(scratch, "xenia-type-folder");
            WriteFile(dir / "00000001" / "RACER7" / "RACER7", MakeSave("RACER7"));
            WriteFile(dir / "00000001" / "ALEX" / "ALEX", alex);
            const auto reports = ImportDroppedSaves(dir);
            CHECK(reports.size() == 2 && reports[0].outcome == Outcome::Imported && reports[0].name == "ALEX"
                && reports[1].outcome == Outcome::Imported && reports[1].name == "RACER7");
            CHECK(ReadFile(dir / "RACER7" / "RACER7") == MakeSave("RACER7") && ReadFile(dir / "ALEX.xcontent") == MakeXContent("ALEX", u""));
            CHECK(!fs::exists(dir / "00000001") && fs::exists(dir / "imported" / "00000001" / "RACER7" / "RACER7"));

            const fs::path lower = Fresh(scratch, "xenia-lower");
            WriteFile(lower / "454107d9" / "00000001" / "ALEX" / "ALEX", alex);
            const auto low = ImportDroppedSaves(lower);
            CHECK(low.size() == 1 && low[0].outcome == Outcome::Imported && ReadFile(lower / "ALEX" / "ALEX") == alex);
        }
        // NOTHING changes when the name is taken: the folder stays, with its
        // line in import-log.txt. A folder of that layout with no save in it,
        // and a real save that happens to be named 00000001, are as before.
        {
            const fs::path dir = Fresh(scratch, "xenia-taken");
            WriteFile(dir / "ALEX" / "ALEX", alex);
            WriteFile(dir / "ALEX.xcontent", MakeXContent("ALEX", u""));
            WriteFile(dir / "454107D9" / "00000001" / "ALEX" / "ALEX", other);
            const auto before = Picture(dir);
            const auto reports = ImportDroppedSaves(dir);
            CHECK(reports.size() == 1 && reports[0].outcome == Outcome::NameTaken);
            CHECK(Picture(dir) == before);

            const fs::path empty = Fresh(scratch, "xenia-empty");
            WriteFile(empty / "454107D9" / "Headers" / "00000001" / "NAME.header", MakeXContent("NAME", u""));
            const auto nothing = Picture(empty);
            CHECK(ImportDroppedSaves(empty).empty() && Picture(empty) == nothing);

            const fs::path named = Fresh(scratch, "save-named-00000001");
            WriteFile(named / "00000001" / "00000001", MakeSave("00000001"));
            const auto adopted = ImportDroppedSaves(named);
            CHECK(adopted.size() == 1 && adopted[0].outcome == Outcome::Adopted && fs::exists(named / "00000001.xcontent"));
        }

        // 13. The save folder alone (macOS, Windows, Linux: no drop folder):
        // as before, a file that is not a save or a package gets no line; a
        // folder <NAME>/<NAME> that holds no save gets the line it always got
        // (AdoptFolder: case 5) and nothing changes; a dropped save still
        // moves to imported/ there.
        {
            const fs::path dir = Fresh(scratch, "save-folder-alone");
            Bytes zip(60, 0);
            memcpy(&zip[0], "PK\3\4", 4);
            WriteFile(dir / "junk.bin", Bytes{ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 });
            WriteFile(dir / "garbage.bin", Bytes(300, 0x5A));
            WriteFile(dir / "save.zip", zip);
            WriteFile(dir / "empty", Bytes{});
            WriteFile(dir / "speedbreaker-bug-report-v0.2.6-x.zip", Bytes{ 'C', 'O', 'N', ' ' });
            WriteFile(dir / "NOTES" / "NOTES", Bytes(500, 'n'));
            const auto before = Picture(dir);
            const auto reports = ImportDroppedSaves(dir);
            CHECK(Picture(dir) == before);
            CHECK(reports.size() == 1 && Utf8(reports[0].source.filename()) == "NOTES" && reports[0].outcome == Outcome::Unreadable);
            WriteFile(dir / "x.sav", alex);
            const auto in = ImportDroppedSaves(dir);
            const Report* x = Find(in, "x.sav");
            CHECK(in.size() == 2 && x && x->outcome == Outcome::Imported && ReadFile(dir / "imported" / "x.sav") == alex);
        }
    }

    int Dump(const char* path)
    {
        Bytes bytes;
        if (!ReadWhole(path, bytes))
        {
            fprintf(stderr, "can't read %s\n", path);
            return 1;
        }
        printf("%s: %zu bytes\n", path, bytes.size());
        if (bytes.size() >= 4 && IsStfsMagic(bytes.data()))
        {
            StfsPackage p;
            std::string why;
            if (!ParseStfs(bytes, p, why))
            {
                printf("package: %s\n", why.c_str());
                return 1;
            }
            printf("package %s header %#x data from %#llx shift %u content type %#x title %08X\n", p.magic, p.headerSize,
                (unsigned long long)p.dataBase, p.shift, p.contentType, p.titleId);
            printf("console id %02x%02x%02x%02x%02x profile id %02x%02x%02x%02x%02x%02x%02x%02x table: %u block(s) at %u, %u blocks in all, separation %u\n",
                p.consoleId[0], p.consoleId[1], p.consoleId[2], p.consoleId[3], p.consoleId[4], p.profileId[0], p.profileId[1], p.profileId[2],
                p.profileId[3], p.profileId[4], p.profileId[5], p.profileId[6], p.profileId[7], p.tableBlocks, p.tableStart, p.totalBlocks, p.blockSeparation);
            for (const StfsFile& f : StfsFiles(p, StfsChain::TableA))
            {
                // The file's bytes by each chain, summed (EA's CRC) to compare with tools/stfs.py's reads.
                printf("  %-28s %s%s start %u size %u parent %d crc", f.name.c_str(), f.directory ? "dir " : "", f.contiguous ? "contiguous" : "chained",
                    f.startBlock, f.size, f.parent);
                for (StfsChain chain : { StfsChain::Contiguous, StfsChain::TableA, StfsChain::TableB })
                {
                    Bytes out;
                    if (!f.directory && StfsRead(p, f.startBlock, (f.size + 0xFFF) >> 12, chain, out))
                    {
                        out.resize(f.size);
                        printf(" %08X", EaCrc(out.data(), out.size()));
                    }
                    else
                        printf(" -");
                }
                printf("\n");
            }
            std::string name;
            Bytes save;
            SaveCheck check;
            if (StfsFindSave(p, name, save, check, why))
                printf("save \"%s\": profile \"%s\", tag %08X\n", name.c_str(), check.alias.c_str(), check.tag);
            else
                printf("no Most Wanted save: %s\n", why.c_str());
            return 0;
        }
        const SaveCheck check = CheckSave(bytes);
        if (check.ok)
            printf("a whole Most Wanted save: profile \"%s\", tag %08X\n", check.alias.c_str(), check.tag);
        else
            printf("not a save this takes: %s\n", check.why.c_str());
        return 0;
    }
}

int main(int argc, char** argv)
{
    if (argc > 2 && strcmp(argv[1], "--dump") == 0)
        return Dump(argv[2]);
    const fs::path scratch = argc > 1 ? argv[1] : "save_import_scratch";
    // The scratch folder is emptied before and after: only ever one of this name.
    if (scratch.filename() != "save_import_scratch")
    {
        fprintf(stderr, "the scratch folder must be named save_import_scratch\n");
        return 2;
    }
    fs::remove_all(scratch);
    fs::create_directories(scratch);

    FileTests();
    PackageTests();
    XContentTests();
    FolderTests(scratch);

    fs::remove_all(scratch);
    if (failures)
    {
        fprintf(stderr, "save_import_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("save_import_test: all passed\n");
    return 0;
}
