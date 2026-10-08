// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See zip.h.
//
// Formats: RFC 1950/1951 (zlib, deflate), PKWARE's APPNOTE.TXT (zip), the
// PNG specification. Self-contained (no runtime headers) so the test can
// build it alone.
#include "zip.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <queue>

namespace report
{
    namespace
    {
        const std::array<uint32_t, 256>& CrcTable()
        {
            static const std::array<uint32_t, 256> table = [] {
                std::array<uint32_t, 256> t{};
                for (uint32_t i = 0; i < 256; i++)
                {
                    uint32_t c = i;
                    for (int k = 0; k < 8; k++)
                        c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                    t[i] = c;
                }
                return t;
            }();
            return table;
        }

        // Lengths 3..258 as codes 257..285 plus extra bits; distances
        // 1..32768 as codes 0..29 plus extra bits (RFC 1951 3.2.5).
        constexpr uint16_t kLengthBase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99,
            115, 131, 163, 195, 227, 258 };
        constexpr uint8_t kLengthExtra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
        constexpr uint16_t kDistBase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537,
            2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
        constexpr uint8_t kDistExtra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12,
            13, 13 };
        // The order code-length code lengths are sent in (3.2.7).
        constexpr uint8_t kCodeLengthOrder[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

        constexpr int kWindow = 32768;
        constexpr int kHashBits = 15;
        constexpr int kMaxChain = 64;
        constexpr int kMinMatch = 3, kMaxMatch = 258;
        constexpr size_t kBlockTokens = 1 << 16;

        int LengthCode(int length)
        {
            int code = 0;
            while (code < 28 && kLengthBase[code + 1] <= length)
                code++;
            return code;
        }

        int DistanceCode(int distance)
        {
            int code = 0;
            while (code < 29 && kDistBase[code + 1] <= distance)
                code++;
            return code;
        }

        // Codes go out least significant bit first; Huffman codes are
        // stored bit-reversed so they come out most significant bit first.
        struct BitWriter
        {
            std::vector<uint8_t>& out;
            uint64_t bits = 0;
            int count = 0;

            void Put(uint32_t value, int n)
            {
                bits |= uint64_t(value) << count;
                count += n;
                while (count >= 8)
                {
                    out.push_back(uint8_t(bits));
                    bits >>= 8;
                    count -= 8;
                }
            }

            void Flush()
            {
                if (count > 0)
                    out.push_back(uint8_t(bits));
                bits = 0;
                count = 0;
            }
        };

        // Huffman code lengths of at most maxBits. Too deep a tree flattens
        // its frequencies (halved, kept non-zero) and tries again.
        void BuildLengths(const uint32_t* frequencies, int count, int maxBits, uint8_t* lengths)
        {
            std::vector<uint64_t> f(frequencies, frequencies + count);
            while (true)
            {
                std::fill(lengths, lengths + count, 0);
                using Item = std::pair<uint64_t, int>;  // weight, node (ties by node: deterministic)
                std::priority_queue<Item, std::vector<Item>, std::greater<Item>> heap;
                std::vector<int> parent(size_t(count) * 2, -1);
                for (int i = 0; i < count; i++)
                    if (f[i] > 0)
                        heap.push({ f[i], i });
                if (heap.empty())
                    return;
                if (heap.size() == 1)
                {
                    lengths[heap.top().second] = 1;
                    return;
                }
                int next = count;
                while (heap.size() > 1)
                {
                    Item a = heap.top();
                    heap.pop();
                    Item b = heap.top();
                    heap.pop();
                    parent[size_t(a.second)] = next;
                    parent[size_t(b.second)] = next;
                    heap.push({ a.first + b.first, next++ });
                }
                int deepest = 0;
                for (int i = 0; i < count; i++)
                {
                    if (f[i] == 0)
                        continue;
                    int depth = 0;
                    for (int n = i; parent[size_t(n)] >= 0; n = parent[size_t(n)])
                        depth++;
                    lengths[i] = uint8_t(std::min(depth, 255));
                    deepest = std::max(deepest, depth);
                }
                if (deepest <= maxBits)
                    return;
                for (uint64_t& v : f)
                    if (v > 0)
                        v = (v + 1) / 2;
            }
        }

        // Canonical codes (RFC 1951 3.2.2), bit-reversed for BitWriter.
        void CanonicalCodes(const uint8_t* lengths, int count, uint16_t* codes)
        {
            int perLength[16] = {};
            for (int i = 0; i < count; i++)
                if (lengths[i])
                    perLength[lengths[i]]++;
            uint16_t next[16] = {};
            uint16_t code = 0;
            for (int bits = 1; bits < 16; bits++)
            {
                code = uint16_t((code + perLength[bits - 1]) << 1);
                next[bits] = code;
            }
            for (int i = 0; i < count; i++)
            {
                codes[i] = 0;
                if (!lengths[i])
                    continue;
                uint16_t c = next[lengths[i]]++, reversed = 0;
                for (int b = 0; b < lengths[i]; b++)
                    reversed = uint16_t((reversed << 1) | ((c >> b) & 1));
                codes[i] = reversed;
            }
        }

        // Every alphabet gets two used symbols at least: a one-symbol code
        // is incomplete, which inflaters reject for most alphabets.
        void AtLeastTwo(uint32_t* frequencies, int count)
        {
            int used = 0;
            for (int i = 0; i < count; i++)
                used += frequencies[i] > 0;
            for (int i = 0; i < count && used < 2; i++)
                if (frequencies[i] == 0)
                {
                    frequencies[i] = 1;
                    used++;
                }
        }

        struct Token
        {
            uint16_t value;     // a literal byte, or a match length
            uint16_t distance;  // 0: a literal
        };

        void WriteBlock(BitWriter& w, const std::vector<Token>& tokens, bool final)
        {
            uint32_t litFreq[286] = {}, distFreq[30] = {};
            for (const Token& t : tokens)
            {
                if (t.distance == 0)
                    litFreq[t.value]++;
                else
                {
                    litFreq[257 + LengthCode(t.value)]++;
                    distFreq[DistanceCode(t.distance)]++;
                }
            }
            litFreq[256] = 1;  // end of block
            AtLeastTwo(litFreq, 286);
            AtLeastTwo(distFreq, 30);
            uint8_t litLen[286], distLen[30];
            uint16_t litCode[286], distCode[30];
            BuildLengths(litFreq, 286, 15, litLen);
            BuildLengths(distFreq, 30, 15, distLen);
            CanonicalCodes(litLen, 286, litCode);
            CanonicalCodes(distLen, 30, distCode);

            int hlit = 286, hdist = 30;
            while (hlit > 257 && litLen[hlit - 1] == 0)
                hlit--;
            while (hdist > 1 && distLen[hdist - 1] == 0)
                hdist--;
            // The code lengths, run-length coded (16: repeat the previous
            // 3-6 times, 17: 3-10 zeros, 18: 11-138 zeros).
            std::vector<uint8_t> all(litLen, litLen + hlit);
            all.insert(all.end(), distLen, distLen + hdist);
            struct Rle { uint8_t symbol, extra; };
            std::vector<Rle> rle;
            for (size_t i = 0; i < all.size();)
            {
                uint8_t v = all[i];
                size_t run = 1;
                while (i + run < all.size() && all[i + run] == v)
                    run++;
                size_t left = run;
                if (v == 0)
                {
                    while (left >= 11)
                    {
                        size_t n = std::min<size_t>(left, 138);
                        rle.push_back({ 18, uint8_t(n - 11) });
                        left -= n;
                    }
                    if (left >= 3)
                    {
                        rle.push_back({ 17, uint8_t(left - 3) });
                        left = 0;
                    }
                }
                else
                {
                    rle.push_back({ v, 0 });
                    left--;
                    while (left >= 3)
                    {
                        size_t n = std::min<size_t>(left, 6);
                        rle.push_back({ 16, uint8_t(n - 3) });
                        left -= n;
                    }
                }
                while (left > 0)
                {
                    rle.push_back({ v, 0 });
                    left--;
                }
                i += run;
            }
            uint32_t clFreq[19] = {};
            for (const Rle& r : rle)
                clFreq[r.symbol]++;
            AtLeastTwo(clFreq, 19);
            uint8_t clLen[19];
            uint16_t clCode[19];
            BuildLengths(clFreq, 19, 7, clLen);
            CanonicalCodes(clLen, 19, clCode);
            int hclen = 19;
            while (hclen > 4 && clLen[kCodeLengthOrder[hclen - 1]] == 0)
                hclen--;

            w.Put(final ? 1 : 0, 1);
            w.Put(2, 2);  // dynamic Huffman
            w.Put(uint32_t(hlit - 257), 5);
            w.Put(uint32_t(hdist - 1), 5);
            w.Put(uint32_t(hclen - 4), 4);
            for (int i = 0; i < hclen; i++)
                w.Put(clLen[kCodeLengthOrder[i]], 3);
            for (const Rle& r : rle)
            {
                w.Put(clCode[r.symbol], clLen[r.symbol]);
                if (r.symbol == 16)
                    w.Put(r.extra, 2);
                else if (r.symbol == 17)
                    w.Put(r.extra, 3);
                else if (r.symbol == 18)
                    w.Put(r.extra, 7);
            }
            for (const Token& t : tokens)
            {
                if (t.distance == 0)
                {
                    w.Put(litCode[t.value], litLen[t.value]);
                    continue;
                }
                int lc = LengthCode(t.value);
                w.Put(litCode[257 + lc], litLen[257 + lc]);
                if (kLengthExtra[lc])
                    w.Put(uint32_t(t.value - kLengthBase[lc]), kLengthExtra[lc]);
                int dc = DistanceCode(t.distance);
                w.Put(distCode[dc], distLen[dc]);
                if (kDistExtra[dc])
                    w.Put(uint32_t(t.distance - kDistBase[dc]), kDistExtra[dc]);
            }
            w.Put(litCode[256], litLen[256]);
        }

        void Put16(std::vector<uint8_t>& out, uint32_t v)
        {
            out.push_back(uint8_t(v));
            out.push_back(uint8_t(v >> 8));
        }

        void Put32(std::vector<uint8_t>& out, uint32_t v)
        {
            Put16(out, v & 0xFFFF);
            Put16(out, v >> 16);
        }

        void Put32BE(std::vector<uint8_t>& out, uint32_t v)
        {
            for (int shift = 24; shift >= 0; shift -= 8)
                out.push_back(uint8_t(v >> shift));
        }

        void PngChunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data)
        {
            Put32BE(out, uint32_t(data.size()));
            size_t start = out.size();
            out.insert(out.end(), type, type + 4);
            out.insert(out.end(), data.begin(), data.end());
            Put32BE(out, Crc32(out.data() + start, out.size() - start));
        }
    }

    uint32_t Crc32(const void* data, size_t size, uint32_t crc)
    {
        const auto& table = CrcTable();
        const uint8_t* p = static_cast<const uint8_t*>(data);
        crc = ~crc;
        for (size_t i = 0; i < size; i++)
            crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
        return ~crc;
    }

    uint32_t Adler32(const void* data, size_t size)
    {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        uint32_t a = 1, b = 0;
        while (size > 0)
        {
            size_t n = std::min<size_t>(size, 5552);  // the most before b can overflow
            for (size_t i = 0; i < n; i++)
            {
                a += p[i];
                b += a;
            }
            a %= 65521;
            b %= 65521;
            p += n;
            size -= n;
        }
        return (b << 16) | a;
    }

    std::vector<uint8_t> Deflate(const void* data, size_t size)
    {
        const uint8_t* in = static_cast<const uint8_t*>(data);
        std::vector<uint8_t> out;
        out.reserve(size / 3 + 64);
        BitWriter w{ out };
        std::vector<int32_t> head(size_t(1) << kHashBits, -1);
        std::vector<int32_t> prev(kWindow, -1);
        auto hash = [&](size_t p) {
            uint32_t v = uint32_t(in[p]) | uint32_t(in[p + 1]) << 8 | uint32_t(in[p + 2]) << 16;
            return (v * 2654435761u) >> (32 - kHashBits);
        };
        auto insert = [&](size_t p) {
            if (p + kMinMatch > size)
                return;
            uint32_t h = hash(p);
            prev[p & (kWindow - 1)] = head[h];
            head[h] = int32_t(p);
        };
        // Positions are int32: over 1 GB is refused (ZipWriter stores it),
        // far past anything a bug report holds.
        if (size > (size_t(1) << 30))
            return {};

        std::vector<Token> tokens;
        tokens.reserve(kBlockTokens);
        size_t pos = 0;
        while (pos < size)
        {
            int bestLength = 0, bestDistance = 0;
            if (pos + kMinMatch <= size)
            {
                int limit = int(std::min<size_t>(kMaxMatch, size - pos));
                int32_t candidate = head[hash(pos)];
                for (int chain = 0; candidate >= 0 && chain < kMaxChain; chain++)
                {
                    size_t c = size_t(candidate);
                    if (pos - c > size_t(kWindow))
                        break;
                    if (in[c + size_t(bestLength)] == in[pos + size_t(bestLength)])
                    {
                        int length = 0;
                        while (length < limit && in[c + size_t(length)] == in[pos + size_t(length)])
                            length++;
                        if (length > bestLength)
                        {
                            bestLength = length;
                            bestDistance = int(pos - c);
                            if (length == limit)
                                break;
                        }
                    }
                    candidate = prev[c & (kWindow - 1)];
                }
            }
            if (bestLength >= kMinMatch)
            {
                tokens.push_back({ uint16_t(bestLength), uint16_t(bestDistance) });
                for (int i = 0; i < bestLength; i++)
                    insert(pos + size_t(i));
                pos += size_t(bestLength);
            }
            else
            {
                tokens.push_back({ in[pos], 0 });
                insert(pos);
                pos++;
            }
            if (tokens.size() == kBlockTokens && pos < size)
            {
                WriteBlock(w, tokens, false);
                tokens.clear();
            }
        }
        WriteBlock(w, tokens, true);
        w.Flush();
        return out;
    }

    std::vector<uint8_t> EncodePng(const uint8_t* rgb, uint32_t width, uint32_t height)
    {
        // Filtered rows: each starts with its filter type (0 none, 1 sub,
        // 2 up, 3 average, 4 Paeth), picked by the smallest sum of residuals.
        const size_t stride = size_t(width) * 3;
        std::vector<uint8_t> raw;
        raw.reserve((stride + 1) * height);
        std::vector<uint8_t> zero(stride, 0), candidate(stride), best(stride);
        for (uint32_t y = 0; y < height; y++)
        {
            const uint8_t* row = rgb + size_t(y) * stride;
            const uint8_t* up = y ? row - stride : zero.data();
            uint64_t bestCost = UINT64_MAX;
            uint8_t bestType = 0;
            for (uint8_t type = 0; type < 5; type++)
            {
                uint64_t cost = 0;
                for (size_t i = 0; i < stride; i++)
                {
                    int a = i >= 3 ? row[i - 3] : 0, b = up[i], c = i >= 3 ? up[i - 3] : 0;
                    int predicted = 0;
                    switch (type)
                    {
                    case 1: predicted = a; break;
                    case 2: predicted = b; break;
                    case 3: predicted = (a + b) / 2; break;
                    case 4:
                    {
                        int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                        predicted = (pa <= pb && pa <= pc) ? a : pb <= pc ? b : c;
                        break;
                    }
                    default: break;
                    }
                    uint8_t v = uint8_t(row[i] - predicted);
                    candidate[i] = v;
                    cost += uint64_t(std::abs(int(int8_t(v))));
                }
                if (cost < bestCost)
                {
                    bestCost = cost;
                    bestType = type;
                    best.swap(candidate);
                }
            }
            raw.push_back(bestType);
            raw.insert(raw.end(), best.begin(), best.end());
        }

        std::vector<uint8_t> zlib = { 0x78, 0x9C };  // deflate, 32 KB window
        std::vector<uint8_t> deflated = Deflate(raw.data(), raw.size());
        zlib.insert(zlib.end(), deflated.begin(), deflated.end());
        Put32BE(zlib, Adler32(raw.data(), raw.size()));

        std::vector<uint8_t> png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
        std::vector<uint8_t> header;
        Put32BE(header, width);
        Put32BE(header, height);
        header.insert(header.end(), { 8, 2, 0, 0, 0 });  // 8-bit RGB, deflate, adaptive filters, not interlaced
        PngChunk(png, "IHDR", header);
        PngChunk(png, "IDAT", zlib);
        PngChunk(png, "IEND", {});
        return png;
    }

    bool ZipWriter::Add(const std::string& name, const void* data, size_t size, time_t modified)
    {
        if (m_finished || size >= 0xFFFFFFFFu)
            return false;
        std::vector<uint8_t> deflated = Deflate(data, size);
        bool stored = deflated.size() >= size || (size > 0 && deflated.empty());
        const uint8_t* payload = stored ? static_cast<const uint8_t*>(data) : deflated.data();
        size_t payloadSize = stored ? size : deflated.size();
        // Room for this entry's headers, and the central directory's.
        if (m_data.size() + payloadSize + 2 * (46 + name.size()) + 1024 >= 0xFFFFFFFFull)
            return false;

        struct tm local{};
        localtime_r(&modified, &local);
        int year = std::max(local.tm_year + 1900, 1980);
        Entry e;
        e.name = name;
        e.crc = Crc32(data, size);
        e.compressedSize = uint32_t(payloadSize);
        e.size = uint32_t(size);
        e.offset = uint32_t(m_data.size());
        e.method = stored ? 0 : 8;
        e.time = uint16_t(local.tm_hour << 11 | local.tm_min << 5 | local.tm_sec / 2);
        e.date = uint16_t((year - 1980) << 9 | (local.tm_mon + 1) << 5 | local.tm_mday);

        Put32(m_data, 0x04034B50);
        Put16(m_data, 20);      // version needed: 2.0 (deflate)
        Put16(m_data, 0x0800);  // names are UTF-8
        Put16(m_data, e.method);
        Put16(m_data, e.time);
        Put16(m_data, e.date);
        Put32(m_data, e.crc);
        Put32(m_data, e.compressedSize);
        Put32(m_data, e.size);
        Put16(m_data, uint32_t(name.size()));
        Put16(m_data, 0);
        m_data.insert(m_data.end(), name.begin(), name.end());
        m_data.insert(m_data.end(), payload, payload + payloadSize);
        m_entries.push_back(std::move(e));
        return true;
    }

    const std::vector<uint8_t>& ZipWriter::Finish()
    {
        if (m_finished)
            return m_data;
        m_finished = true;
        uint32_t directory = uint32_t(m_data.size());
        for (const Entry& e : m_entries)
        {
            Put32(m_data, 0x02014B50);
            Put16(m_data, 0x031E);  // made by: Unix (for the permissions), spec 3.0
            Put16(m_data, 20);
            Put16(m_data, 0x0800);
            Put16(m_data, e.method);
            Put16(m_data, e.time);
            Put16(m_data, e.date);
            Put32(m_data, e.crc);
            Put32(m_data, e.compressedSize);
            Put32(m_data, e.size);
            Put16(m_data, uint32_t(e.name.size()));
            Put16(m_data, 0);  // extra
            Put16(m_data, 0);  // comment
            Put16(m_data, 0);  // disk
            Put16(m_data, 0);  // internal attributes
            Put32(m_data, 0100644u << 16);  // external: a regular file, rw-r--r--
            Put32(m_data, e.offset);
            m_data.insert(m_data.end(), e.name.begin(), e.name.end());
        }
        uint32_t directorySize = uint32_t(m_data.size()) - directory;
        Put32(m_data, 0x06054B50);
        Put16(m_data, 0);
        Put16(m_data, 0);
        Put16(m_data, uint32_t(m_entries.size()));
        Put16(m_data, uint32_t(m_entries.size()));
        Put32(m_data, directorySize);
        Put32(m_data, directory);
        Put16(m_data, 0);
        return m_data;
    }
}
