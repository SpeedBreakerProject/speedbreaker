// The bug report's own deflate, PNG and zip writers (runtime/report/zip.cpp)
// against independent readers. From the repo root:
//   clang++ -std=c++20 -O2 -Iruntime tests/report_zip_test.cpp runtime/report/zip.cpp -o build/report_zip_test
//   build/report_zip_test build/report_zip_out [extra files to deflate...]
//   python3 tests/report_zip_check.py build/report_zip_out
//   unzip -t build/report_zip_out/test.zip        (and on macOS: ditto -x -k build/report_zip_out/test.zip /tmp/x)
// The program checks CRC-32 and Adler-32 against published values and
// writes each case's input and output; the script inflates every stream with
// Python's zlib, opens the archive with zipfile and decodes the PNG by hand.
#include <report/zip.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void Write(const std::filesystem::path& path, const void* data, size_t size)
{
    std::ofstream out(path, std::ios::binary);
    out.write(static_cast<const char*>(data), std::streamsize(size));
}

static std::vector<uint8_t> Bytes(const std::string& s)
{
    return std::vector<uint8_t>(s.begin(), s.end());
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        printf("usage: %s <out dir> [files...]\n", argv[0]);
        return 2;
    }
    std::filesystem::path out = argv[1];
    std::filesystem::create_directories(out);

    // Published check values.
    CHECK(report::Crc32("123456789", 9) == 0xCBF43926u, "crc32 %08X", report::Crc32("123456789", 9));
    CHECK(report::Adler32("Wikipedia", 9) == 0x11E60398u, "adler32 %08X", report::Adler32("Wikipedia", 9));
    CHECK(report::Crc32("56789", 5, report::Crc32("1234", 4)) == 0xCBF43926u, "running crc32");

    std::mt19937 rng(12345);
    std::vector<std::vector<uint8_t>> cases;
    cases.push_back({});
    cases.push_back(Bytes("a"));
    cases.push_back(Bytes("aaa"));
    cases.push_back(Bytes("abcabcabcabcabcabcabcabcabcabc"));
    {
        // Log-like text: repeated structure, changing numbers.
        std::string text;
        for (int i = 0; i < 20000; i++)
            text += "[perf] " + std::to_string(55 + i % 7) + ".2 fps (worst frame " + std::to_string(16 + (i * 7919) % 13) +
                ".4 ms) | CP busy " + std::to_string(i % 97) + " ms/frame | draws " + std::to_string(i * 31 % 4100) + "\n";
        cases.push_back(Bytes(text));
    }
    {
        std::vector<uint8_t> random(300000);
        for (auto& b : random)
            b = uint8_t(rng());
        cases.push_back(random);
    }
    cases.push_back(std::vector<uint8_t>(2 << 20, 0));  // long runs: matches of 258 at distance 1
    {
        // Matches at the far end of the window (32768) and just past it.
        std::vector<uint8_t> block(32768);
        for (auto& b : block)
            b = uint8_t(rng());
        std::vector<uint8_t> v = block;
        v.insert(v.end(), block.begin(), block.end());
        v.push_back(1);
        v.insert(v.end(), block.begin(), block.begin() + 1000);
        cases.push_back(v);
    }
    {
        // Every byte value, runs of every length 1..300 (length codes, 16/17/18 runs in the code lengths).
        std::vector<uint8_t> v;
        for (int len = 1; len <= 300; len++)
            v.insert(v.end(), size_t(len), uint8_t(len * 37));
        for (int b = 0; b < 256; b++)
            v.push_back(uint8_t(b));
        cases.push_back(v);
    }
    {
        // Few symbols (a skewed alphabet; one literal plus end of block).
        std::vector<uint8_t> v(100000);
        for (auto& b : v)
            b = (rng() % 100) == 0 ? 'b' : 'a';
        cases.push_back(v);
    }
    for (int i = 2; i < argc; i++)
    {
        std::ifstream in(argv[i], std::ios::binary);
        cases.emplace_back(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    for (size_t i = 0; i < cases.size(); i++)
    {
        std::vector<uint8_t> d = report::Deflate(cases[i].data(), cases[i].size());
        CHECK(!d.empty(), "case %zu: empty output", i);
        Write(out / ("raw_" + std::to_string(i) + ".bin"), cases[i].data(), cases[i].size());
        Write(out / ("deflate_" + std::to_string(i) + ".bin"), d.data(), d.size());
        printf("case %zu: %zu -> %zu bytes\n", i, cases[i].size(), d.size());
    }

    // A PNG with smooth areas, edges and noise, and an odd size.
    {
        uint32_t w = 301, h = 203;
        std::vector<uint8_t> rgb(size_t(w) * h * 3);
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
            {
                uint8_t* p = &rgb[(size_t(y) * w + x) * 3];
                p[0] = uint8_t(x);
                p[1] = uint8_t(y * 2);
                p[2] = (x / 40 + y / 40) % 2 ? uint8_t(rng()) : uint8_t(200);
            }
        std::vector<uint8_t> png = report::EncodePng(rgb.data(), w, h);
        Write(out / "test.png", png.data(), png.size());
        Write(out / "test.rgb", rgb.data(), rgb.size());
        std::string size = std::to_string(w) + " " + std::to_string(h) + "\n";
        Write(out / "test.size", size.data(), size.size());
        printf("png: %u x %u -> %zu bytes\n", w, h, png.size());
    }

    // An archive: stored and deflated entries, folders, an empty file.
    {
        report::ZipWriter zip;
        std::filesystem::create_directories(out / "zipref" / "logs");
        struct Entry { std::string name; std::vector<uint8_t> data; };
        std::vector<Entry> entries = {
            { "README.txt", Bytes("A bug report.\n") },
            { "empty.txt", {} },
            { "logs/2026-09-28_14-03-22.log", cases[4] },
            { "logs/random.bin", cases[5] },
            { "screenshot.png", {} },
        };
        {
            std::ifstream in(out / "test.png", std::ios::binary);
            entries[4].data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        for (const Entry& e : entries)
        {
            CHECK(zip.Add(e.name, e.data.data(), e.data.size(), 1790000000), "add %s", e.name.c_str());
            Write(out / "zipref" / e.name, e.data.data(), e.data.size());
        }
        const std::vector<uint8_t>& archive = zip.Finish();
        Write(out / "test.zip", archive.data(), archive.size());
        printf("zip: %zu entries, %zu bytes\n", zip.Entries(), archive.size());
    }

    printf("%s (%d failures)\n", g_failures ? "FAILED" : "ok", g_failures);
    return g_failures ? 1 : 0;
}
