// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Texture overrides (DDS files from texture mods), swapped in by the
// renderer: guest memory and the game TextureInfo are left as they are.
// The game thread registers an override for the guest address of a texture
// when its header gets built; the command processor finds it when loading
// that texture and checks the guest bytes are still the same it saw first
// (if a pack is unloaded and its memory reused, the entry is dropped).
#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace gpu::texture_override
{
    enum class Format : uint8_t { Rgba8, Dxt1, Dxt3, Dxt5 };
    // Level 0: DXT blocks of a DDS file as they are (no mips),
    // else R,G,B,A bytes (mips are made on the GPU). `generation`:
    // for a minimap file the MinimapGeneration when it was loaded,
    // 0 for a DDS file (kept until its texture pack unloads).
    struct Image
    {
        uint32_t width = 0, height = 0;
        Format format = Format::Rgba8;
        // Freed by the renderer once on the GPU (the only reader after decode).
        mutable std::vector<uint8_t> texels;
        // The DDS file (.zst: packed), read by Decode, once.
        std::filesystem::path file;
        bool packed = false;  // `file` is zstd
        mutable bool read = false;
        uint64_t id = 0;  // unique
        uint32_t generation = 0;

#ifndef SB_NO_MODS
        // Frees `texels` (`file` isn't read after it).
        void Release() const;
        // Renderer: `file` into `texels` (empty when it fails), on the first call.
        void Decode() const;
#else
        void Release() const {}
        void Decode() const {}
#endif
    };

#ifndef SB_NO_MODS

    // Game thread: the DDS at `path` replaces the w x h texture at guest
    // physical address `base` (read once per path, until the guest
    // TexturePack `pack` unloads). False if it can't be read.
    bool Register(uint32_t base, uint32_t width, uint32_t height,
        const std::filesystem::path& path, uint32_t pack);
    // Game thread: guest TexturePack `pack` unloads, so its overrides go
    // and images nothing else uses get freed (TakeFreed).
    void UnloadPack(uint32_t pack);
    // Renderer: the ids of the images freed since the last call.
    std::vector<uint64_t> TakeFreed();

    // Game thread: the PC minimap file (a MINI_MAP*.BIN: TPKs, as is or in
    // JDLZ blocks) RegisterFromMinimap reads from now on; empty: none.
    void SetMinimapFile(const std::filesystem::path& file);
    // Game thread: texture `nameHash` (bStringHash of the
    // name) from the current minimap file replaces the w x h
    // texture at `base`. False if there's no such texture.
    bool RegisterFromMinimap(uint32_t base, uint32_t width, uint32_t height,
        uint32_t nameHash);
    // Game thread: a texture without an override now loads at `base`.
    void Unregister(uint32_t base);
    // Goes up when the textures of a minimap file are dropped (another one
    // got loaded): the renderer then frees its images from older generations.
    uint32_t MinimapGeneration();

    // Command processor: the override for the w x h texture at `base`
    // whose guest bytes are `guest[0, size)`, or null.
    std::shared_ptr<const Image> Find(uint32_t base, uint32_t width, uint32_t height,
        const uint8_t* guest, uint32_t size);
#else
    // Not built with SB_NO_MODS (mods::kAvailable): what the renderer calls.
    inline std::vector<uint64_t> TakeFreed() { return {}; }
    inline uint32_t MinimapGeneration() { return 0; }
    inline std::shared_ptr<const Image> Find(uint32_t, uint32_t, uint32_t, const uint8_t*,
        uint32_t)
    {
        return nullptr;
    }
#endif
}
