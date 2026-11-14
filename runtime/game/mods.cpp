// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See mods.h.
//
// Settings > Mods: game side of the mods (the hooks), one option per mod
// ([mods] table). Files on disk and downloads are in mods/mods.h.
#include <stdafx.h>
#include <game/fe.h>
#include <game/mods.h>
#include <gpu/texture_override.h>
#include <region/region.h>

#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>

#include <kernel/function.h>
#include <kernel/memory.h>
#include <mods/mods.h>
#include <user/settings.h>

// Texture mods: <user data>/mods/<mod>, each turned on by its setting; the
// files replace textures through the renderer (gpu/texture_override.h).
// File forms and folders: mods/README.md.
namespace
{
    // TextureInfo fields (big-endian).
    constexpr uint32_t kInfoName = 0x0C;       // char[24]
    constexpr uint32_t kInfoHash = 0x24;       // u32, bStringHash of the name
    constexpr uint32_t kInfoWidth = 0x44;      // u16
    constexpr uint32_t kInfoHeight = 0x46;     // u16
    constexpr uint32_t kInfoPack = 0x68;       // guest pointer to the TexturePack
    constexpr uint32_t kInfoImageData = 0x70;  // guest pointer
    constexpr uint32_t kPackPath = 0x0C;       // guest char* (Global\\InGameTextures.tpk)

    using mods::HasExtension;
    using mods::TextureFile;
    using mods::ToUpper;

    // Gamepad Prompts: folder name of the set; null for the original icons and
    // when the packs weren't there on start (a download waits for a restart).
    // Layout: <set>/<TEX>.dds.
    const char* GamepadPromptSet()
    {
        if (!mods::PresentAtBoot("button_packs"))
            return nullptr;
        using enum settings::GamepadPrompts;
        switch (settings::GamepadPrompts(settings::GetInt(settings::Id::GamepadPrompts)))
        {
        case Xbox360: return "xbox_360";
        case XboxOne: return "xbox_one";
        case Ps3: return "ps3";
        case Ps4: return "ps4";
        case Switch: return "switch";
        default: return nullptr;
        }
    }

    // Files of the set by texture name, as the game stores it (24 chars).
    using PromptIndex = std::unordered_map<std::string, TextureFile>;

    PromptIndex IndexPromptMod(const char* set)
    {
        PromptIndex index;
        const auto root = mods::Folder("button_packs"), dir = root / set;
        mods::ForEachTexture(root, dir,
            [&](const auto&, std::string name, TextureFile file)
            { mods::AddTexture(index, std::move(name), std::move(file)); });
        fprintf(stderr, "[mods] Gamepad prompts: %zu textures in %s\n", index.size(),
            dir.string().c_str());
        return index;
    }

    // Controller picture on the controls and loading screens
    // (LOADING_CONTROLLER_X360, D-pad = stick + 1): the Xbox One/PS3/PS4/Switch
    // sets have their own, <set>/LOADING_CONTROLLER_{STICK,DPAD}.
    constexpr uint32_t kX360Stick = 0xEC3C0443;

    // Set name for the picture; null for the Xbox 360 one.
    const char* ControllerPictureSet()
    {
        const char* set = GamepadPromptSet();
        return set && strcmp(set, "xbox_360") != 0 ? set : nullptr;
    }

    // File in the set for this texture: a 32x32 icon or the controller picture.
    const TextureFile* FindPromptTexture(uint32_t info, uint8_t* base, const char* set)
    {
        static const PromptIndex s_index = IndexPromptMod(set);
        const uint32_t hash = PPC_LOAD_U32(info + kInfoHash);
        std::string name;
        if (hash == kX360Stick || hash == kX360Stick + 1)
        {
            if (!ControllerPictureSet())
                return nullptr;
            name = hash == kX360Stick ? "LOADING_CONTROLLER_STICK"
                                      : "LOADING_CONTROLLER_DPAD";
        }
        else if (PPC_LOAD_U16(info + kInfoWidth) == 32
            && PPC_LOAD_U16(info + kInfoHeight) == 32)
        {
            const char* text =
                static_cast<const char*>(g_memory.Translate(info + kInfoName));
            name.assign(text, strnlen(text, 24));
            ToUpper(name);
        }
        auto it = name.empty() ? s_index.end() : s_index.find(name);
        return it != s_index.end() ? &it->second : nullptr;
    }

    // HD HUD textures (HD HUD Mod setting), if the files were there on start
    // (a set downloaded later waits for a restart, same for minimaps).
    // Layout: <disc dir>/<pack file>/<texture pack>/<TEX>.dds.
    const char* HdHudRoot()
    {
        if (!settings::GetBool(settings::Id::HdHudMod) || !mods::PresentAtBoot("hd_hud"))
            return nullptr;
        static const std::string s_root = mods::Folder("hd_hud").string();
        return s_root.c_str();
    }

    // bStringHash of the pack file stem in uppercase
    // ("INGAMETEXTURES"), i.e. the folder name.
    uint32_t PackFolderHash(uint32_t pack, uint8_t* base)
    {
        const uint32_t path = PPC_LOAD_U32(pack + kPackPath);
        if (!path)
            return 0;
        const char* text = static_cast<const char*>(g_memory.Translate(path));
        std::string stem(text, strnlen(text, 128));
        stem.erase(0, stem.find_last_of("\\/") + 1);
        ToUpper(stem);
        if (stem.size() > 4 && stem.ends_with(".TPK"))
            stem.resize(stem.size() - 4);
        return game::fe::Hash(stem);
    }

    // Keyed by (pack name hash, texture name hash; names in the game are uppercase).
    using HudIndex = std::unordered_map<uint64_t, TextureFile>;

    uint64_t HudKey(uint32_t pack, uint32_t texture)
    {
        return uint64_t(pack) << 32 | texture;
    }

    HudIndex IndexHdHud(const char* root)
    {
        HudIndex index;
        mods::ForEachTexture(root, root,
            [&](const std::filesystem::path& p, const std::string& name, TextureFile file)
        {
            std::string folder = p.parent_path().filename().string();
            ToUpper(folder);
            const uint32_t pack = game::fe::Hash(folder);
            mods::AddTexture(index, HudKey(pack, game::fe::Hash(name)), std::move(file));
        });
        fprintf(stderr, "[mods] HD HUD: %zu textures in %s\n", index.size(), root);
        return index;
    }

    // Minimap files of the mod (TRACKS/L2RA/MINI_MAP*.BIN) by uppercase name.
    using MinimapIndex = std::unordered_map<std::string, std::filesystem::path>;

    MinimapIndex IndexHudMinimaps(const char* root)
    {
        MinimapIndex files;
        std::error_code ec;
        const auto dir = std::filesystem::path(root) / "TRACKS" / "L2RA";
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
        {
            // X.BIN or X.BIN.zst; a plain .BIN wins.
            const bool zst = HasExtension(entry.path(), ".ZST");
            std::string stem = (zst ? entry.path().stem() : entry.path()).stem().string();
            ToUpper(stem);
            if (!stem.starts_with("MINI_MAP"))
                continue;
            if (zst)
                files.try_emplace(std::move(stem), entry.path());
            else
                files.insert_or_assign(std::move(stem), entry.path());
        }
        fprintf(stderr, "[mods] HD HUD: %zu minimap files\n", files.size());
        return files;
    }

    // Registers an override for the texture: from the
    // prompt set, else from the HD HUD, else (none) drops
    // the entry an earlier texture left at that address.
    void RegisterModTexture(uint32_t info, uint8_t* base)
    {
        const char* set = GamepadPromptSet();
        const char* root = HdHudRoot();
        const uint32_t data = PPC_LOAD_U32(info + kInfoImageData);
        if ((!set && !root) || !data)
            return;
        const uint32_t addr = data & 0x1FFFFFFF;
        const uint32_t w = PPC_LOAD_U16(info + kInfoWidth);
        const uint32_t h = PPC_LOAD_U16(info + kInfoHeight);
        const uint32_t pack = PPC_LOAD_U32(info + kInfoPack);
        const uint32_t hash = PPC_LOAD_U32(info + kInfoHash);
        const TextureFile* file = set ? FindPromptTexture(info, base, set) : nullptr;
        if (!file && root && pack)
        {
            static const HudIndex s_hud = IndexHdHud(root);
            auto it = s_hud.find(HudKey(PackFolderHash(pack, base), hash));
            file = it != s_hud.end() ? &it->second : nullptr;
        }
        namespace overrides = gpu::texture_override;
        bool registered = file && overrides::Register(addr, w, h, file->path, pack);
        if (!file && root)
            registered = overrides::RegisterFromMinimap(addr, w, h, hash);
        if (!registered)
            overrides::Unregister(addr);
    }
}

// Builds a texture header from its TextureInfo (r3).
SB_HOOK(sub_8243F1D8)
{
    RegisterModTexture(ctx.r3.u32, base);
    SB_ORIGINAL(sub_8243F1D8)(ctx, base);
}

// Unloads a texture pack (r3: its 0xB3310000 chunk), its overrides go too.
SB_HOOK(sub_8221F118)
{
    if (HdHudRoot() || GamepadPromptSet())
    {
        // Payload of the 0x33310001 info chunk holds the TexturePack at +0x70.
        const uint32_t chunk = ctx.r3.u32, end = chunk + 8 + PPC_LOAD_U32(chunk + 4);
        for (uint32_t c = chunk + 8; c < end; c += 8 + PPC_LOAD_U32(c + 4))
            if (PPC_LOAD_U32(c) == 0x33310001)
            {
                if (const uint32_t pack = PPC_LOAD_U32(c + 8 + 0x70))
                    gpu::texture_override::UnloadPack(pack);
                break;
            }
    }
    SB_ORIGINAL(sub_8221F118)(ctx, base);
}

// Names the minimap file to load (r3: the name, "MINI_MAP" or
// "MINI_MAP_<race>"): we swap in the mod version if theres one.
SB_HOOK(sub_822A6BF0)
{
    const uint32_t nameAddr = ctx.r3.u32;
    SB_ORIGINAL(sub_822A6BF0)(ctx, base);
    if (const char* root = HdHudRoot())
    {
        static const auto s_minimaps = IndexHudMinimaps(root);
        const char* text = static_cast<const char*>(g_memory.Translate(nameAddr));
        std::string name(text, strnlen(text, 32));
        ToUpper(name);
        auto it = s_minimaps.find(name);
        gpu::texture_override::SetMinimapFile(it != s_minimaps.end() ? it->second
            : std::filesystem::path());
    }
}

namespace mods
{
    constexpr uint32_t kFindObject = 0x8227FD58;  // FEng find by name hash
    constexpr uint32_t kHideObject = 0x8227FE08;  // FEng hide

    // Gamepad Prompts: controller pictures of the non-360 sets (FindPromptTexture)
    // hide the "Xbox Guide" text: finds CBUTTON_18 in screen package and hides it.
    void HideGuideText(PPCContext& ctx, uint8_t* base, uint32_t screen)
    {
        if (!ControllerPictureSet())
            return;
        PPCContext call = ctx;
        call.r3.u64 = PPC_LOAD_U32(screen + 16);
        call.r4.u64 = 0x0F274B86;
        g_memory.FindFunction(region::Addr(kFindObject))(call, base);
        if (call.r3.u32)
            g_memory.FindFunction(region::Addr(kHideObject))(call, base);
    }
}
