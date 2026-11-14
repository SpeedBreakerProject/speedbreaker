// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See texture_override.h.
#include <stdafx.h>
#include "texture_override.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <thirdparty/zstd/zstd.h>

namespace gpu::texture_override
{
    namespace
    {
        using ImageMap = std::unordered_map<uint32_t, std::shared_ptr<const Image>>;

        struct Entry
        {
            uint32_t width, height;
            std::shared_ptr<const Image> image;
            uint64_t contentHash = 0;  // guest bytes when first loaded (0: not yet)
            uint32_t pack = 0;         // guest TexturePack (0: from a minimap file)
        };
        std::mutex s_mutex;
        std::unordered_map<uint32_t, Entry> s_entries;  // by guest address
        // DDS files by path (null: unreadable).
        std::unordered_map<std::string, std::shared_ptr<const Image>> s_images;
        std::atomic<uint64_t> s_nextId{ 1 };
        std::vector<uint64_t> s_freed;  // image ids, for TakeFreed
        // Current minimap file: path, if it's loaded, and textures by name hash.
        std::filesystem::path s_minimap;
        bool s_minimapLoaded = false;
        ImageMap s_minimapImages;
        std::atomic<uint32_t> s_minimapGeneration{ 1 };

        uint32_t Le32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }

        size_t BlockBytes(uint32_t width, uint32_t height, Format format)
        {
            const size_t blocks = size_t((width + 3) / 4) * ((height + 3) / 4);
            return blocks * (format == Format::Dxt1 ? 8 : 16);
        }

        // Unpacked size of a zstd frame; false if unknown or over 1 GB.
        bool ZstdSize(const std::vector<uint8_t>& packed, size_t& size)
        {
            const unsigned long long n =
                ZSTD_getFrameContentSize(packed.data(), packed.size());
            if (n == ZSTD_CONTENTSIZE_UNKNOWN || n == ZSTD_CONTENTSIZE_ERROR
                || n > (1ull << 30))
                return false;
            size = size_t(n);
            return true;
        }

        // A whole zstd frame; empty if it fails.
        std::vector<uint8_t> Unzstd(const std::vector<uint8_t>& packed)
        {
            size_t size;
            if (!ZstdSize(packed, size))
                return {};
            std::vector<uint8_t> out(size);
            if (ZSTD_isError(ZSTD_decompress(out.data(), out.size(), packed.data(),
                    packed.size())))
                return {};
            return out;
        }

        struct DdsHeader
        {
            uint32_t width, height;
            Format format;      // Rgba8: palettized or 32-bit
            bool palettized;    // 256 B,G,R,A entries, then one byte per texel
            uint32_t masks[4];  // 32-bit: R,G,B,A (0: none)
        };

        // Header `d` (128 bytes) of a DDS file of `size` bytes; false unless
        // it's DXT1/3/5, 8-bit palettized or 32-bit with a complete level 0.
        bool ReadDdsHeader(const uint8_t* d, size_t size, DdsHeader& dds)
        {
            if (size < 128 || memcmp(d, "DDS ", 4) != 0)
                return false;
            dds.height = Le32(d + 12);
            dds.width = Le32(d + 16);
            if (!dds.width || !dds.height || dds.width > 8192 || dds.height > 8192)
                return false;
            const uint32_t flags = Le32(d + 80), bits = Le32(d + 88);
            const size_t avail = size - 128, texels = size_t(dds.width) * dds.height;
            dds.palettized = false;
            if (flags & 4)
            {
                const char* fourCc = reinterpret_cast<const char*>(d + 84);
                dds.format = !memcmp(fourCc, "DXT1", 4) ? Format::Dxt1
                    : !memcmp(fourCc, "DXT3", 4) ? Format::Dxt3
                    : !memcmp(fourCc, "DXT5", 4) ? Format::Dxt5 : Format::Rgba8;
                return dds.format != Format::Rgba8
                    && avail >= BlockBytes(dds.width, dds.height, dds.format);
            }
            dds.format = Format::Rgba8;
            dds.palettized = (flags & 0x20) && bits == 8;
            for (int c = 0; c < 4; c++)
                dds.masks[c] = c < 3 || (flags & 1) ? Le32(d + 92 + 4 * c) : 0;
            if (dds.palettized)
                return avail >= 1024 + texels;
            return bits == 32 && avail >= texels * 4;
        }

        void Color565(uint16_t c, uint8_t* out)
        {
            const uint32_t r = c >> 11, g = (c >> 5) & 63, b = c & 31;
            out[0] = uint8_t(r << 3 | r >> 2);
            out[1] = uint8_t(g << 2 | g >> 4);
            out[2] = uint8_t(b << 3 | b >> 2);
            out[3] = 255;
        }

        // A BC1 colour block into 16 RGBA texels; `fourColor`: the BC2/BC3 kind.
        void DecodeColor(const uint8_t* block, bool fourColor, uint8_t (*out)[4])
        {
            const uint16_t c0 = uint16_t(block[0] | block[1] << 8);
            const uint16_t c1 = uint16_t(block[2] | block[3] << 8);
            uint8_t pal[4][4];
            Color565(c0, pal[0]);
            Color565(c1, pal[1]);
            for (int i = 0; i < 3; i++)
            {
                if (fourColor || c0 > c1)
                {
                    pal[2][i] = uint8_t((2 * pal[0][i] + pal[1][i]) / 3);
                    pal[3][i] = uint8_t((pal[0][i] + 2 * pal[1][i]) / 3);
                }
                else
                {
                    pal[2][i] = uint8_t((pal[0][i] + pal[1][i]) / 2);
                    pal[3][i] = 0;
                }
            }
            pal[2][3] = 255;
            pal[3][3] = fourColor || c0 > c1 ? 255 : 0;
            const uint32_t bits = Le32(block + 4);
            for (int t = 0; t < 16; t++)
                memcpy(out[t], pal[(bits >> (2 * t)) & 3], 4);
        }

        void DecodeAlpha3(const uint8_t* block, uint8_t (*out)[4])
        {
            for (int t = 0; t < 16; t++)
            {
                const uint32_t a = (block[t / 2] >> (4 * (t & 1))) & 15;
                out[t][3] = uint8_t(a * 17);
            }
        }

        void DecodeAlpha5(const uint8_t* block, uint8_t (*out)[4])
        {
            const uint32_t a0 = block[0], a1 = block[1];
            uint8_t pal[8] = { uint8_t(a0), uint8_t(a1) };
            for (uint32_t i = 1; i < 7; i++)
                pal[i + 1] = a0 > a1 ? uint8_t(((7 - i) * a0 + i * a1) / 7)
                    : i < 5          ? uint8_t(((5 - i) * a0 + i * a1) / 5)
                                     : uint8_t(i == 5 ? 0 : 255);
            uint64_t bits = 0;
            for (int i = 0; i < 6; i++)
                bits |= uint64_t(block[2 + i]) << (8 * i);
            for (int t = 0; t < 16; t++)
                out[t][3] = pal[(bits >> (3 * t)) & 7];
        }

        // Level 0 of a DDS file that ReadDdsHeader accepts, as R,G,B,A texels;
        // `keepBlocks`: DXT stays as is. Null for any other file.
        std::shared_ptr<Image> DecodeDds(const std::vector<uint8_t>& file,
            bool keepBlocks)
        {
            DdsHeader dds;
            if (!ReadDdsHeader(file.data(), file.size(), dds))
                return nullptr;
            const uint32_t w = dds.width, h = dds.height;
            auto image = std::make_shared<Image>();
            image->width = w;
            image->height = h;
            const uint8_t* src = file.data() + 128;
            if (dds.format != Format::Rgba8 && keepBlocks)
            {
                image->format = dds.format;
                image->texels.assign(src, src + BlockBytes(w, h, dds.format));
                return image;
            }
            image->texels.resize(size_t(w) * h * 4);
            uint8_t* px = image->texels.data();
            if (dds.format != Format::Rgba8)
            {
                const bool dxt1 = dds.format == Format::Dxt1;
                uint8_t block[16][4];
                for (uint32_t by = 0; by < (h + 3) / 4; by++)
                    for (uint32_t bx = 0; bx < (w + 3) / 4; bx++, src += dxt1 ? 8 : 16)
                    {
                        DecodeColor(src + (dxt1 ? 0 : 8), !dxt1, block);
                        if (dds.format == Format::Dxt3)
                            DecodeAlpha3(src, block);
                        else if (dds.format == Format::Dxt5)
                            DecodeAlpha5(src, block);
                        for (uint32_t t = 0; t < 16; t++)
                        {
                            const uint32_t x = bx * 4 + (t & 3), y = by * 4 + t / 4;
                            if (x < w && y < h)
                                memcpy(px + (size_t(y) * w + x) * 4, block[t], 4);
                        }
                    }
            }
            else if (dds.palettized)
                for (size_t i = 0; i < size_t(w) * h; i++)
                {
                    const uint8_t* e = src + size_t(src[1024 + i]) * 4;
                    uint8_t* o = px + i * 4;
                    o[0] = e[2], o[1] = e[1], o[2] = e[0], o[3] = e[3];
                }
            else
                // To R,G,B,A whatever the masks are (no alpha: opaque).
                for (size_t i = 0; i < size_t(w) * h; i++)
                {
                    const uint32_t v = Le32(src + i * 4);
                    for (int c = 0; c < 4; c++)
                    {
                        const uint32_t m = dds.masks[c];
                        px[i * 4 + c] = m ? uint8_t((v & m) >> std::countr_zero(m)) : 255;
                    }
                }
            return image;
        }

        // A .zst file, any case.
        bool IsZst(const std::filesystem::path& path)
        {
            std::string e = path.extension().string();
            return e.size() == 4 && e[0] == '.' && (e[1] | 0x20) == 'z'
                && (e[2] | 0x20) == 's' && (e[3] | 0x20) == 't';
        }

        std::vector<uint8_t> ReadFile(const std::filesystem::path& path)
        {
            std::ifstream in(path, std::ios::binary);
            return { std::istreambuf_iterator<char>(in), {} };
        }

        // Bytes of the file, a .zst one unpacked (empty if that fails).
        std::vector<uint8_t> ReadUnpacked(const std::filesystem::path& path)
        {
            std::vector<uint8_t> file = ReadFile(path);
            return IsZst(path) ? Unzstd(file) : file;
        }

        // Header of a DDS file; the texels get read on the first draw (Decode).
        std::shared_ptr<Image> LoadDds(const std::filesystem::path& path)
        {
            std::ifstream in(path, std::ios::binary);
            const bool packed = IsZst(path);
            std::vector<uint8_t> head(128);
            size_t size;
            if (packed)
            {
                // the frame header gives the size, the first blocks the DDS header
                std::vector<uint8_t> chunk(ZSTD_DStreamInSize());
                in.read(reinterpret_cast<char*>(chunk.data()),
                    std::streamsize(chunk.size()));
                chunk.resize(size_t(in.gcount()));
                if (!ZstdSize(chunk, size))
                    return nullptr;
                ZSTD_DCtx* dctx = ZSTD_createDCtx();
                if (!dctx)
                    return nullptr;
                ZSTD_outBuffer dst{ head.data(), head.size(), 0 };
                while (dst.pos < dst.size && !chunk.empty())
                {
                    ZSTD_inBuffer src{ chunk.data(), chunk.size(), 0 };
                    bool failed = false;
                    while (dst.pos < dst.size && src.pos < src.size && !failed)
                        failed = ZSTD_isError(ZSTD_decompressStream(dctx, &dst, &src));
                    if (failed || dst.pos == dst.size)
                        break;
                    chunk.resize(chunk.capacity());
                    in.read(reinterpret_cast<char*>(chunk.data()),
                        std::streamsize(chunk.size()));
                    chunk.resize(size_t(in.gcount()));
                }
                ZSTD_freeDCtx(dctx);
                head.resize(dst.pos);
            }
            else
            {
                in.read(reinterpret_cast<char*>(head.data()),
                    std::streamsize(head.size()));
                head.resize(size_t(in.gcount()));
                std::error_code ec;
                size = size_t(std::filesystem::file_size(path, ec));
                if (ec)
                    return nullptr;
            }
            DdsHeader dds;
            if (head.size() < 128 || !ReadDdsHeader(head.data(), size, dds))
                return nullptr;
            auto image = std::make_shared<Image>();
            image->width = dds.width;
            image->height = dds.height;
            image->format = dds.format;
            image->packed = packed;
            image->file = path;
            return image;
        }

        // JDLZ (EA LZ77 flavour): `in` starts at the "JDLZ" header.
        bool Jdlz(const uint8_t* in, size_t avail, std::vector<uint8_t>& out)
        {
            if (avail < 16 || memcmp(in, "JDLZ", 4) != 0)
                return false;
            const uint32_t usize = Le32(in + 8), csize = Le32(in + 12);
            // a minimap BIN unpacks to a few MB; a corrupt header could ask for 4 GB
            if (csize > avail || usize > (64u << 20))
                return false;
            const size_t start = out.size();
            out.resize(start + usize);
            uint8_t* o = out.data() + start;
            size_t i = 16, n = 0;
            uint32_t f1 = 1, f2 = 1;
            while (i < csize && n < usize)
            {
                if (f1 == 1)
                    f1 = in[i++] | 0x100u;
                if (f2 == 1 && i < csize)
                    f2 = in[i++] | 0x100u;
                if (i >= csize)
                    break;
                if (f1 & 1)
                {
                    if (i + 2 > csize)
                        return false;
                    uint32_t length, distance;
                    if (f2 & 1)
                    {
                        length = (in[i + 1] | ((in[i] & 0xF0u) << 4)) + 3;
                        distance = (in[i] & 0x0Fu) + 1;
                    }
                    else
                    {
                        distance = (in[i + 1] | ((in[i] & 0xE0u) << 3)) + 17;
                        length = (in[i] & 0x1Fu) + 3;
                    }
                    i += 2;
                    if (distance > n)
                        return false;
                    for (uint32_t k = 0; k < length && n < usize; k++, n++)
                        o[n] = o[n - distance];
                    f2 >>= 1;
                }
                else
                    o[n++] = in[i++];
                f1 >>= 1;
            }
            return n == usize;
        }

        // Textures of a minimap file by name hash: TPKs (0xB3300000: entries
        // 0x33310004, D3D formats 0x33310005, data 0x33320002 from its first
        // 128-byte boundary), either as is or in 0x3A100 chunks with one JDLZ
        // block each (4-byte aligned; HUFF blocks skipped) that join into them.
        ImageMap LoadMinimapFile(const std::filesystem::path& path, uint32_t generation)
        {
            ImageMap images;
            std::vector<uint8_t> file = ReadUnpacked(path);
            std::vector<uint8_t> d;
            if (file.size() >= 4 && Le32(file.data()) == 0xB3300000)
                d.swap(file);  // (file now empty: no JDLZ pass)
            // (Padding between the chunks, of no fixed alignment.)
            for (size_t p = 0; p + 16 <= file.size(); p += 4)
            {
                const uint32_t id = Le32(&file[p]), size = Le32(&file[p + 4]);
                if (id != 0x3A100 || memcmp(&file[p + 8], "JDLZ", 4) != 0
                    || p + 8 + size > file.size())
                    continue;
                if (!Jdlz(&file[p + 8], size, d))
                    break;
                p += 8 + size - 4;
            }
            struct Chunk { uint32_t id, size; size_t data; };
            auto chunks = [&](size_t p, size_t end, auto&& visit) {
                while (p + 8 <= end)
                {
                    const Chunk c{ Le32(&d[p]), Le32(&d[p + 4]), p + 8 };
                    if (c.data + c.size > end)
                        return;
                    visit(c);
                    p = c.data + c.size;
                }
            };
            chunks(0, d.size(), [&](const Chunk& tpk) {
                if (tpk.id != 0xB3300000)
                    return;
                std::vector<size_t> entries, formats;
                size_t data = 0;
                chunks(tpk.data, tpk.data + tpk.size, [&](const Chunk& c) {
                    chunks(c.data, c.data + c.size, [&](const Chunk& k) {
                        if (k.id == 0x33310004)
                            for (size_t e = 0; e + 0x7C <= k.size; e += 0x7C)
                                entries.push_back(k.data + e);
                        else if (k.id == 0x33310005)
                            for (size_t e = 0; e + 0x20 <= k.size; e += 0x20)
                                formats.push_back(k.data + e);
                        else if (k.id == 0x33320002)
                            data = (k.data + 0x7F) & ~size_t(0x7F);
                    });
                });
                for (size_t n = 0; n < entries.size() && n < formats.size() && data; n++)
                {
                    const uint8_t* e = &d[entries[n]];
                    const uint32_t nameHash = Le32(e + 0x24), offset = Le32(e + 0x30);
                    const uint32_t size = Le32(e + 0x38);
                    const uint32_t format = Le32(&d[formats[n]] + 0x14);
                    const uint32_t w = uint32_t(e[0x44] | e[0x45] << 8);
                    const uint32_t h = uint32_t(e[0x46] | e[0x47] << 8);
                    if (data + offset + size > d.size() || !w || !h)
                        continue;
                    // As a DDS file: header, then the texels.
                    std::vector<uint8_t> dds(128 + size_t(size));
                    memcpy(dds.data(), "DDS ", 4);
                    auto put = [&](size_t o, uint32_t v) { memcpy(&dds[o], &v, 4); };
                    put(12, h);
                    put(16, w);
                    if (format == 21 || format == 22)  // D3DFMT_A8R8G8B8, X8R8G8B8
                    {
                        put(80, format == 21 ? 0x41 : 0x40);
                        put(88, 32);
                        put(92, 0x00FF0000);
                        put(96, 0x0000FF00);
                        put(100, 0x000000FF);
                        put(104, 0xFF000000);
                    }
                    else
                    {
                        put(80, 4);
                        put(84, format);  // a FourCC (DXT1/3/5)
                    }
                    memcpy(dds.data() + 128, &d[data + offset], size);
                    // Decoded, so the radar keeps its mips.
                    if (auto image = DecodeDds(dds, false))
                    {
                        image->id = s_nextId++;
                        image->generation = generation;
                        images[nameHash] = std::move(image);
                    }
                }
            });
            return images;
        }

        // FNV-1a of the first 64 KB; never 0.
        uint64_t HashBytes(const uint8_t* p, uint32_t size)
        {
            uint64_t h = 0xCBF29CE484222325ull;
            for (uint32_t i = 0, n = std::min<uint32_t>(size, 64 << 10); i < n; i++)
                h = (h ^ p[i]) * 0x100000001B3ull;
            return h | 1;
        }
    }

    void Image::Release() const
    {
        std::vector<uint8_t>().swap(texels);
        read = true;
    }

    void Image::Decode() const
    {
        if (read || file.empty())
            return;
        read = true;
        const std::vector<uint8_t> dds = packed ? ReadUnpacked(file) : ReadFile(file);
        auto decoded = DecodeDds(dds, true);
        if (!decoded || decoded->width != width || decoded->height != height
            || decoded->format != format)
        {
            fprintf(stderr, "[mods] Texture override: can't unpack a %ux%u texture\n",
                width, height);
            return;
        }
        texels = std::move(decoded->texels);
    }

    bool Register(uint32_t base, uint32_t width, uint32_t height,
        const std::filesystem::path& path, uint32_t pack)
    {
        const std::string key = path.string();
        std::shared_ptr<const Image> image;
        bool known;
        {
            std::lock_guard lock(s_mutex);
            auto it = s_images.find(key);
            known = it != s_images.end();
            if (known)
                image = it->second;
        }
        if (!known)
        {
            auto loaded = LoadDds(path);
            if (loaded)
                loaded->id = s_nextId++;
            else
                fprintf(stderr, "[mods] Texture override: can't read %s\n", key.c_str());
            image = std::move(loaded);
            std::lock_guard lock(s_mutex);
            s_images[key] = image;
        }
        if (!image)
            return false;
        std::lock_guard lock(s_mutex);
        s_entries[base] = { width, height, std::move(image), 0, pack };
        return true;
    }

    void UnloadPack(uint32_t pack)
    {
        std::lock_guard lock(s_mutex);
        std::erase_if(s_entries, [&](const auto& e) { return e.second.pack == pack; });
        // Every DDS image no entry uses anymore: re-read if its pack loads again.
        std::unordered_set<const Image*> used;
        for (const auto& [base, e] : s_entries)
            used.insert(e.image.get());
        std::erase_if(s_images, [&](const auto& e)
        {
            if (!e.second || used.contains(e.second.get()))
                return false;
            s_freed.push_back(e.second->id);
            return true;
        });
    }

    std::vector<uint64_t> TakeFreed()
    {
        std::lock_guard lock(s_mutex);
        return std::exchange(s_freed, {});
    }

    void SetMinimapFile(const std::filesystem::path& file)
    {
        std::lock_guard lock(s_mutex);
        // Minimap textures load again, the old adresses are stale.
        std::erase_if(s_entries,
            [](const auto& e) { return e.second.image->generation != 0; });
        if (file == s_minimap)
            return;  // same file, the images stay
        s_minimap = file;
        s_minimapLoaded = false;
        if (s_minimapImages.empty())
            return;
        s_minimapImages.clear();
        s_minimapGeneration++;
    }

    bool RegisterFromMinimap(uint32_t base, uint32_t width, uint32_t height,
        uint32_t nameHash)
    {
        std::unique_lock lock(s_mutex);
        if (s_minimap.empty())
            return false;
        if (!s_minimapLoaded)
        {
            s_minimapLoaded = true;
            const std::filesystem::path file = s_minimap;
            const uint32_t generation = s_minimapGeneration;
            lock.unlock();
            auto images = LoadMinimapFile(file, generation);
            lock.lock();
            if (file != s_minimap)
                return false;
            if (images.empty())
                fprintf(stderr, "[mods] Texture override: can't read %s\n",
                    file.string().c_str());
            s_minimapImages = std::move(images);
        }
        auto it = s_minimapImages.find(nameHash);
        if (it == s_minimapImages.end())
            return false;
        s_entries[base] = { width, height, it->second };
        return true;
    }

    void Unregister(uint32_t base)
    {
        std::lock_guard lock(s_mutex);
        s_entries.erase(base);
    }

    uint32_t MinimapGeneration()
    {
        return s_minimapGeneration;
    }

    std::shared_ptr<const Image> Find(uint32_t base, uint32_t width, uint32_t height,
        const uint8_t* guest, uint32_t size)
    {
        {
            std::lock_guard lock(s_mutex);
            if (!s_entries.contains(base))
                return nullptr;
        }
        // up to 64 KB: hashed outside the lock, the game thread registers meanwhile
        const uint64_t hash = HashBytes(guest, size);
        std::lock_guard lock(s_mutex);
        auto it = s_entries.find(base);
        if (it == s_entries.end())
            return nullptr;
        Entry& e = it->second;
        if (e.width != width || e.height != height
            || (e.contentHash && e.contentHash != hash))
        {
            // Unregister/UnloadPack shouldn't leave stale
            // entries: a size change here means we missed one; a
            // contents change means the game rewrote the texture.
            fprintf(stderr, "[mods] Texture override: dropped %08X (%s changed)\n", base,
                e.width != width || e.height != height ? "size" : "contents");
            s_entries.erase(it);
            return nullptr;
        }
        e.contentHash = hash;
        return e.image;
    }
}
