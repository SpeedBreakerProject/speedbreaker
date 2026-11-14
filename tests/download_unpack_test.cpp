// Tests for download/unpack.h: sha256sum lines and stored zips
// (the central directory checks, then a real file opened and
// extracted in a scratch folder). From the repo root:
//   clang++ -std=c++20 -O2 -Iruntime tests/download_unpack_test.cpp
//     runtime/download/unpack.cpp runtime/update/json.cpp
//     -o build/download_unpack_test
//   build/download_unpack_test <scratch folder>
#include <download/unpack.h>

#include <cstdio>
#include <fstream>
#include <string>

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; \
    printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

namespace fs = std::filesystem;
using namespace download;

static void Names()
{
    CHECK(PlainFileName("hd_hud-v1.zip") && PlainFileName("A.b_c-1"), "plain");
    for (const char* bad : { "", ".zip", "a/b.zip", "../a", "a b", "a\\b", "\xc3\xa9" })
        CHECK(!PlainFileName(bad), "'%s'", bad);
    CHECK(!PlainFileName(std::string(129, 'a')) && PlainFileName(std::string(128, 'a')),
        "128 at most");
}

static void Sha256()
{
    const std::string hex =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    CHECK(ParseSha256Line(hex + "  a.zip\n", "a.zip") == hex, "sha256sum's line");
    CHECK(ParseSha256Line(hex + "  a.zip", "a.zip") == hex, "no line end");
    CHECK(ParseSha256Line(hex + " *a.zip\r\n", "a.zip") == hex, "binary mark, CRLF");
    std::string upper = hex;
    for (char& c : upper)
        c = char(toupper(c));
    CHECK(ParseSha256Line(upper + "  a.zip\n", "a.zip") == hex, "upper case hex");
    for (std::string bad : { hex + "  b.zip\n", hex + " a.zip\n", hex + "  a.zip\n\n",
             hex.substr(1) + "  a.zip\n", "g" + hex.substr(1) + "  a.zip\n",
             hex + "  a.zip extra\n", hex + "  \n", std::string() })
        CHECK(ParseSha256Line(bad, "a.zip").empty(), "'%s'", bad.c_str());
}

// --- A stored zip, built here ---

struct ZipFile
{
    std::string name, data;
    uint32_t external = 0;  // Unix mode << 16 when made == 3
    uint16_t method = 0, flags = 0, made = 0x031E;
};

static void Put16(std::string& s, uint32_t v)
{
    s += char(v & 0xFF);
    s += char(v >> 8 & 0xFF);
}

static void Put32(std::string& s, uint32_t v)
{
    Put16(s, v & 0xFFFF);
    Put16(s, v >> 16);
}

static std::string MakeZip(const std::vector<ZipFile>& files,
    const std::string& comment = "")
{
    std::string out, dir;
    for (const ZipFile& f : files)
    {
        const uint32_t local = uint32_t(out.size());
        const uint32_t size = uint32_t(f.data.size()), nameLen = uint32_t(f.name.size());
        Put32(out, 0x04034b50), Put16(out, 10), Put16(out, f.flags), Put16(out, f.method);
        Put32(out, 0), Put32(out, 0x12345678), Put32(out, size), Put32(out, size);
        Put16(out, nameLen), Put16(out, 0);
        out += f.name + f.data;
        Put32(dir, 0x02014b50), Put16(dir, f.made), Put16(dir, 10), Put16(dir, f.flags);
        Put16(dir, f.method), Put32(dir, 0), Put32(dir, 0x12345678), Put32(dir, size);
        Put32(dir, size), Put16(dir, nameLen), Put16(dir, 0), Put16(dir, 0);
        Put16(dir, 0), Put16(dir, 0), Put32(dir, f.external), Put32(dir, local);
        dir += f.name;
    }
    const uint32_t offset = uint32_t(out.size()), count = uint32_t(files.size());
    out += dir;
    Put32(out, 0x06054b50), Put16(out, 0), Put16(out, 0), Put16(out, count);
    Put16(out, count), Put32(out, uint32_t(dir.size())), Put32(out, offset);
    Put16(out, uint32_t(comment.size()));
    return out + comment;
}

// The whole check, the way the game runs it on a file.
static ZipError Check(const std::string& zip, const char* root, ZipContents& contents,
    std::string* detail = nullptr)
{
    const size_t tailBytes = std::min(zip.size(), kZipTailBytes);
    const std::string_view bytes(zip);
    ZipDirectory dir;
    ZipCheck c = FindZipDirectory(bytes.substr(zip.size() - tailBytes), zip.size(), dir);
    if (c.error == ZipError::None)
        c = ReadZipDirectory(bytes.substr(dir.offset, dir.size), dir, root, contents);
    if (detail)
        *detail = c.detail;
    return c.error;
}

static void Zips()
{
    ZipContents c;
    std::string detail;
    const std::string good = MakeZip({ { "hd_hud/", "" }, { "hd_hud/fonts/", "" },
        { "hd_hud/fonts/a.zst", "AAAA" }, { "hd_hud/b.dds", "BBBBBB" } });
    CHECK(Check(good, "hd_hud", c, &detail) == ZipError::None, "%s", detail.c_str());
    CHECK(c.entries.size() == 4 && c.files == 2 && c.unpackedSize == 10,
        "%zu entries", c.entries.size());
    if (c.entries.size() == 4)
    {
        CHECK(c.entries[0].directory && c.entries[0].path.empty(), "the key's folder");
        CHECK(c.entries[1].directory && c.entries[1].path == "fonts",
            "%s", c.entries[1].path.c_str());
        CHECK(!c.entries[2].directory && c.entries[2].path == "fonts/a.zst"
            && c.entries[2].size == 4, "a file");
        const ZipEntry& e = c.entries[2];
        auto header = [&](uint64_t at) {
            return std::string_view(good).substr(at, kZipLocalHeaderBytes);
        };
        const uint64_t data = LocalDataOffset(header(e.headerOffset), e, good.size());
        CHECK(data && good.substr(data, e.size) == "AAAA",
            "its data at %llu", (unsigned long long)data);
        CHECK(!LocalDataOffset(header(e.headerOffset), e, data + 3), "past the limit");
        CHECK(!LocalDataOffset(header(e.headerOffset + 1), e, good.size()),
            "not a header");
        CHECK(!LocalDataOffset(header(c.entries[3].headerOffset), e, good.size()),
            "another entry's");
    }
    const std::vector<ZipFile> one = { { "hd_hud/a.dds", "x" } };
    CHECK(Check(MakeZip(one), "hd_hud", c) == ZipError::None, "no folder entries");
    CHECK(Check(MakeZip(one, std::string(300, 'c')), "hd_hud", c) == ZipError::None,
        "a comment");
    const std::string endInComment = std::string("PK\x05\x06", 4) + std::string(30, 'c');
    CHECK(Check(MakeZip(one, endInComment), "hd_hud", c) == ZipError::None,
        "an end signature in the comment");
    CHECK(Check(MakeZip({ { "hd_hud/NOTICE", "n" }, { "hd_hud/LICENSE", "l" },
        { "hd_hud/_overrides/RUSSIAN/a.dds", "x" } }), "hd_hud", c) == ZipError::None,
        "NOTICE, LICENSE, _overrides");
    CHECK(Check(MakeZip({ { "hd_hud/\xc3\xa9.dds", "x" } }), "hd_hud", c)
            == ZipError::None,
        "UTF-8 names");

    struct Case
    {
        std::vector<ZipFile> files;
        const char* what;
    };
    const Case rejected[] = {
        { { { "other/a.dds", "x" } }, "outside the key" },
        { { { "hd_hud", "x" } }, "a file named as the key" },
        { { { "hd_hudx/a.dds", "x" } }, "a key's prefix" },
        { { { "a.dds", "x" } }, "at the root" },
        { { { "/hd_hud/a.dds", "x" } }, "absolute" },
        { { { "hd_hud/../a.dds", "x" } }, ".." },
        { { { "hd_hud/./a.dds", "x" } }, "." },
        { { { "hd_hud//a.dds", "x" } }, "empty part" },
        { { { "hd_hud/a\\b.dds", "x" } }, "backslash" },
        { { { "hd_hud/c:a.dds", "x" } }, "drive" },
        { { { "hd_hud/a\n.dds", "x" } }, "control character" },
        { { { "hd_hud/a\xff.dds", "x" } }, "not UTF-8" },
        { { { "hd_hud/a.", "x" } }, "trailing dot" },
        { { { "hd_hud/a.dds", "x", 0, 8 } }, "deflated" },
        { { { "hd_hud/a.dds", "x", 0, 0, 1 } }, "encrypted" },
        { { { "hd_hud/a.dds", "target", 0120777u << 16 } }, "a symlink" },
        { { { "hd_hud/a/", "x" } }, "a folder with data" },
        { { { "hd_hud/a.dds", "x" }, { "hd_hud/A.DDS", "y" } }, "duplicates" },
        { { { "hd_hud/a", "x" }, { "hd_hud/a/b.dds", "y" } }, "under a file" },
        { { { "hd_hud/a/b.dds", "y" }, { "hd_hud/a", "x" } },
          "under a file, either order" },
        { { { "hd_hud/", "" } }, "no files" },
        { {}, "empty" },
    };
    for (const Case& k : rejected)
        CHECK(Check(MakeZip(k.files), "hd_hud", c, &detail) == ZipError::Rejected,
            "%s: %s", k.what, detail.c_str());
    // A symlink mode from a non-Unix host means nothing.
    const std::vector<ZipFile> dos = {
        { "hd_hud/a.dds", "x", 0120777u << 16, 0, 0, 0x0014 },
    };
    CHECK(Check(MakeZip(dos), "hd_hud", c) == ZipError::None, "DOS attributes");

    CHECK(Check("", "hd_hud", c) == ZipError::Damaged, "empty file");
    CHECK(Check("not a zip at all, not even close to one", "hd_hud", c)
            == ZipError::Damaged,
        "text");
    CHECK(Check(good.substr(0, good.size() - 5), "hd_hud", c) == ZipError::Damaged,
        "cut short");
    CHECK(Check(good.substr(20), "hd_hud", c) != ZipError::None, "front cut off");
    std::string bent = good;
    bent[bent.size() - 22 + 16] ^= 0x01;  // directory offset
    CHECK(Check(bent, "hd_hud", c) == ZipError::Damaged, "a wrong directory offset");
    bent = good;
    bent[bent.size() - 22 + 10] += 1, bent[bent.size() - 22 + 8] += 1;  // one entry more
    CHECK(Check(bent, "hd_hud", c) == ZipError::Damaged, "a wrong count");
    bent = good;
    bent[bent.size() - 22 + 4] = 1;  // disk 1
    CHECK(Check(bent, "hd_hud", c) == ZipError::Rejected, "multi-disk");
    bent = good;
    for (int k = 0; k < 4; k++)
        bent[bent.size() - 22 + 16 + k] = char(0xFF);
    CHECK(Check(bent, "hd_hud", c) == ZipError::Rejected, "zip64 offset");
    std::string locator;
    Put32(locator, 0x07064b50);
    locator += std::string(16, '\0');
    bent = good;
    bent.insert(bent.size() - 22, locator);
    CHECK(Check(bent, "hd_hud", c) == ZipError::Rejected, "zip64 locator");
    CHECK(Check(good, "a/b", c) == ZipError::Rejected
        && Check(good, "..", c) == ZipError::Rejected, "a bad root");
    const std::vector<ZipFile> anywhere = {
        { "a.dds", "x" },
        { "b/", "" },
        { "b/c.dds", "y" },
    };
    CHECK(Check(MakeZip(anywhere), "", c) == ZipError::None && c.entries.size() == 3
        && c.entries[0].path == "a.dds" && c.entries[1].path == "b",
        "no root: entries anywhere");
    CHECK(Check(MakeZip({ { "/a.dds", "x" } }), "", c) == ZipError::Rejected,
        "no root, absolute");
}

// A real file: opened, extracted, a bad one refused.
static void Files(const fs::path& scratch)
{
    fs::create_directories(scratch);
    const fs::path file = scratch / "set.zip", target = scratch / "out";
    std::ofstream(file, std::ios::binary) << MakeZip({ { "hd_hud/", "" },
        { "hd_hud/fonts/a.zst", "AAAA" }, { "hd_hud/b.dds", "BBBBBB" } });
    ZipArchive zip;
    ZipCheck check = OpenZip(file, "hd_hud", zip);
    CHECK(check.error == ZipError::None && zip.contents.files == 2,
        "%s", check.detail.c_str());
    fs::create_directories(target);
    std::ofstream(target / "stale.txt") << "x";
    uint64_t last = 0;
    Extracted got = ExtractZip(zip, target, [] { return false; },
        [&](uint64_t done) { last = done; });
    auto read = [](const fs::path& p) {
        std::ifstream in(p, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    };
    CHECK(got.result == Extract::Ok && last == 2, "%s", got.detail.c_str());
    CHECK(read(target / "fonts" / "a.zst") == "AAAA"
        && read(target / "b.dds") == "BBBBBB", "contents");
    CHECK(!fs::exists(target / "stale.txt"), "target replaced");
    got = ExtractZip(zip, target, [] { return true; }, {});
    CHECK(got.result == Extract::Cancelled && !fs::exists(target),
        "cancelled: target removed");
    CHECK(OpenZip(file, "other", zip).error == ZipError::Rejected, "another root");
    CHECK(OpenZip(scratch / "missing.zip", "hd_hud", zip).error == ZipError::Damaged,
        "missing");
    std::error_code ec;
    fs::remove_all(scratch, ec);
}

int main(int argc, char** argv)
{
    Names();
    Sha256();
    Zips();
    Files(argc > 1 ? argv[1] : "download_unpack_out");
    printf("%s (%d failures)\n", g_failures ? "FAILED" : "passed", g_failures);
    return g_failures ? 1 : 0;
}
