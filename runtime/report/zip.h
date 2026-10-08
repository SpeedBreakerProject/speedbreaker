// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Just enough of zip, deflate and PNG for a bug report, written here: zlib
// isn't linked on every platform (the macOS build links nothing that has
// it), and a bug report must open with the stock tools of Windows, macOS
// and Linux. tests/report_zip_test.cpp checks the output against
// Python's zlib and zipfile, unzip -t, and ditto.
#pragma once
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace report
{
    // CRC-32 (zip, PNG); continue a running one by passing it back.
    uint32_t Crc32(const void* data, size_t size, uint32_t crc = 0);
    // Adler-32 (the zlib stream inside a PNG).
    uint32_t Adler32(const void* data, size_t size);

    // Raw deflate (RFC 1951): LZ77 over a 32 KB window with hash chains,
    // dynamic Huffman blocks. Text shrinks to about a fifth.
    std::vector<uint8_t> Deflate(const void* data, size_t size);

    // An 8-bit RGB PNG (rows top first, 3 bytes a pixel), each row
    // filtered the way that leaves the smallest residuals.
    std::vector<uint8_t> EncodePng(const uint8_t* rgb, uint32_t width, uint32_t height);

    // A .zip built in memory: deflated entries (stored where deflate doesn't
    // help), UTF-8 names ("logs/x.log" makes the folder), Unix permissions
    // 0644, no ZIP64 (the whole archive must stay under 4 GB).
    class ZipWriter
    {
    public:
        // False (and nothing added) if the archive would pass 4 GB.
        bool Add(const std::string& name, const void* data, size_t size, time_t modified);
        // Appends the central directory; the archive is complete.
        const std::vector<uint8_t>& Finish();
        size_t Entries() const { return m_entries.size(); }

    private:
        struct Entry
        {
            std::string name;
            uint32_t crc, compressedSize, size, offset;
            uint16_t method, time, date;
        };
        std::vector<Entry> m_entries;
        std::vector<uint8_t> m_data;
        bool m_finished = false;
    };
}
