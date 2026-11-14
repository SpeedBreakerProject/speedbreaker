// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Texture mods in <data>/mods/<key>, downloadable from the asset repository.
// mods.cpp: manifest, sets on disk, mod state, texture files.
// download.cpp: the download and the update check. Details: README.md.
#pragma once
#include <user/settings.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace mods
{
    // Off with SB_NO_MODS (macOS and iOS for now, see README.md).
#ifdef SB_NO_MODS
    inline constexpr bool kAvailable = false;
#else
    inline constexpr bool kAvailable = true;
#endif

    // --- The asset repository ---

#define SB_MODS_REPO "feelbadinc/speedbreaker-mod-assets"
    inline constexpr const char kManifestUrl[] =
        "https://raw.githubusercontent.com/" SB_MODS_REPO "/master/manifest.json";
    // The manifest is only a few hundred bytes.
    inline constexpr size_t kMaxManifestBytes = 64 << 10;
    // Version file of the installed set, written last.
    inline constexpr const char kVersionFile[] = "VERSION";

    struct Entry
    {
        std::string key;    // "hd_hud": the folder in mods/
        std::string tag;    // "hd_hud-v1"
        std::string file;   // "hd_hud-v1.zip"
        uint64_t version = 0;  // N from the tag
    };
    // Strict JSON, format in README.md. Entries stay in manifest order.
    // Returns false and what was wrong otherwise.
    bool ParseManifest(std::string_view json, std::vector<Entry>& out,
        std::string& error);
    const Entry* Find(const std::vector<Entry>& entries, std::string_view key);

    // A folder name the game accepts as a mod key.
    bool ValidKey(std::string_view key);
    // "<key>-v<N>", N is a number from 1 with no leading zeros: N, else 0.
    uint64_t TagVersion(std::string_view key, std::string_view tag);

    // Text of a VERSION file: the tag, with whitespace around it trimmed. Gives
    // its N for `key`, or 0 (missing, empty or a tag of some other key).
    uint64_t ParseVersionFile(std::string_view text, std::string_view key);
    std::string VersionFileText(std::string_view tag);  // "hd_hud-v1\n"

    // The manifest N is offered as an update when it's newer than both the
    // installed set and the one waiting to be swapped in (0 = none).
    inline bool UpdateOffered(uint64_t manifest, uint64_t installed, uint64_t pending)
    {
        return manifest > installed && manifest > pending;
    }

    // Mod state, the way the Settings rows show it.
    enum class Status : uint8_t
    {
        Missing,          // no textures in mods/<key> and nothing waiting
        Installed,        // installed (version 0: no VERSION), nothing newer
        Pending,          // a newer set waits in mods_update/<key> for next start
        UpdateAvailable,  // the manifest has one newer than both
    };
    // `files`: mods/<key> has textures; `pending`: N of the waiting set (0: none).
    inline Status Classify(bool files, uint64_t installed, uint64_t pending,
        uint64_t manifest)
    {
        if (!files && !pending)
            return Status::Missing;
        if (UpdateOffered(manifest, installed, pending))
            return Status::UpdateAvailable;
        return pending > installed ? Status::Pending : Status::Installed;
    }

    // --- Sets on disk ---

    // <data>/mods and <data>/mods_update.
    std::filesystem::path ModsFolder(const std::filesystem::path& data);
    std::filesystem::path UpdateFolder(const std::filesystem::path& data);

    // N from a VERSION file for `key` (0: missing, unreadable or another key).
    uint64_t ReadVersion(const std::filesystem::path& file, std::string_view key);
    uint64_t InstalledVersion(const std::filesystem::path& data, std::string_view key);
    uint64_t PendingVersion(const std::filesystem::path& data, std::string_view key);

    // Any texture file under `dir` (.dds, .dds.zst,
    // .dds.sblink, same as ForEachTexture takes).
    bool HasTextures(const std::filesystem::path& dir);

    // Moves mods_update/<key> over mods/<key>, VERSION last.
    // Returns false and why.
    bool MoveSet(const std::filesystem::path& data, std::string_view key,
        std::string& error);

    enum class Placed : uint8_t { Installed, Pending, Failed };
    // An unpacked set (VERSION already written): moved in now if mods/<key>
    // has no textures, otherwise left for the next start.
    Placed PlaceSet(const std::filesystem::path& data, std::string_view key,
        std::string& error);

    // On start, before mods are indexed: moves newer waiting sets in, drops
    // the rest, notes which mods have textures. Returns the keys moved in.
    std::vector<std::string> ApplyPendingAtBoot(const std::filesystem::path& data);
    // mods/<key> had textures when ApplyPendingAtBoot ran.
    bool PresentAtBoot(std::string_view key);

    // Removes mods_update/ if there's nothing left in it.
    void RemoveUpdateFolderIfEmpty(const std::filesystem::path& data);

    // --- Mod state ---

    struct Mod
    {
        const char* key;        // the folder in mods/
        settings::Id setting;   // its row in Settings
    };
    inline constexpr Mod kMods[] = {
        { "hd_hud", settings::Id::HdHudMod },
        { "button_packs", settings::Id::GamepadPrompts },
    };
    const Mod* FindMod(std::string_view key);
    const Mod* FindMod(settings::Id setting);

    struct State
    {
        Status status = Status::Missing;
        uint64_t installed = 0;  // N of mods/<key> (0: none, or no VERSION)
        uint64_t pending = 0;    // N of the waiting set (0: none)
        uint64_t available = 0;  // N in the manifest (0: not read or not listed)
    };

    // On start, before mods are indexed: ApplyPendingAtBoot, then the state of
    // each mod. `data` is the folder used for the rest of the session.
    void StartSession(const std::filesystem::path& data);
    // Re-reads mods/<key> and mods_update/<key> (after a download).
    void Refresh(const std::filesystem::path& data, std::string_view key);
    // Versions from the manifest; mods it doesn't list get 0.
    void SetManifest(const std::vector<Entry>& manifest);
    State GetState(std::string_view key);

    // --- Texture files (.dds, .dds.zst, .dds.sblink; README.md) ---

    // mods/<key> as set by StartSession.
    std::filesystem::path Folder(std::string_view key);

    void ToUpper(std::string& s);
    // Extension of `p` is `ext` (in uppercase: ".DDS"), any case.
    bool HasExtension(const std::filesystem::path& p, std::string_view ext);

    // Rank among the forms of one texture: plain beats .zst, .zst beats a link.
    enum class Form : uint8_t { Link, Zst, Plain };
    struct TextureFile
    {
        std::filesystem::path path;  // for a link, its target
        Form form;
    };

    // Calls add(file found, TEXTURE NAME, TextureFile)
    // for every texture file under `dir` (skips its top level `_` folders);
    // links resolve under `root` (bad ones get logged and skipped).
    using AddTextureFn =
        std::function<void(const std::filesystem::path&, std::string, TextureFile)>;
    void ForEachTexture(const std::filesystem::path& root,
        const std::filesystem::path& dir, const AddTextureFn& add);

    // Into an index by key: the better form wins.
    template <typename Map, typename Key>
    void AddTexture(Map& index, Key key, TextureFile file)
    {
        auto [it, added] = index.try_emplace(key, file);
        if (!added && file.form > it->second.form)
            it->second = std::move(file);
    }

    // --- Downloads (download.cpp) ---

    // Settings > Mods, row of the mod: opens its download dialog.
    void OpenDownloadDialog(std::string_view key);

    // --- Update checks (download.cpp) ---

    // A downloadable mod that has a newer version (in manifest order).
    struct Notice
    {
        std::string key;   // "hd_hud"
        std::string tag;   // "hd_hud-v2"
        // "HD HUD Mod v2 is available: open Settings > Mods to update."
        std::string text;
    };
    // Runs on the update check thread: reads the manifest (SetManifest) and
    // returns mods that have a newer version (null: couldn't read it).
    std::shared_ptr<const std::vector<Notice>> CheckManifest(
        const std::string& userAgent);
    // Mod tags we already announced (<data>/updates/notified_mods.txt).
    std::vector<std::string> NotifiedTags();
    // Marks `tag` as announced and replaces the older one for that mod.
    void RecordNotified(std::string_view key, const std::string& tag);
}
