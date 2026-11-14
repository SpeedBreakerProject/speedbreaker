// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See mods.h.
#include "mods.h"

#include <download/unpack.h>
#include <update/json.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <format>
#include <fstream>
#include <iterator>
#include <mutex>

namespace mods
{
    namespace fs = std::filesystem;

    namespace
    {
        bool IsDigit(char c)
        {
            return c >= '0' && c <= '9';
        }

        // A positive decimal with no leading zeros (and no overflow), else 0.
        uint64_t PlainNumber(std::string_view s)
        {
            if (s.empty() || s.size() > 18 || s[0] == '0')
                return 0;
            uint64_t n = 0;
            for (char c : s)
            {
                if (!IsDigit(c))
                    return 0;
                n = n * 10 + uint64_t(c - '0');
            }
            return n;
        }

        // Written once by ApplyPendingAtBoot, before the game and menu start.
        std::vector<std::string> s_presentAtBoot;
        fs::path s_data;

        constexpr size_t kCount = std::size(kMods);

        struct Disk
        {
            bool files = false;
            uint64_t installed = 0, pending = 0;
        };

        // Written by the main and download threads, read by the menu.
        std::mutex s_mutex;
        Disk s_disk[kCount];
        uint64_t s_available[kCount]{};

        size_t IndexOf(const Mod* mod)
        {
            return size_t(mod - kMods);
        }

        // Copies `from` into `to` (a new folder), top level VERSION goes last.
        bool CopySet(const fs::path& from, const fs::path& to, std::error_code& ec)
        {
            fs::create_directories(to, ec);
            for (auto it = fs::recursive_directory_iterator(from, ec);
                 !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            {
                const fs::path rel = it->path().lexically_relative(from);
                const fs::path target = to / rel;
                if (rel == kVersionFile)
                    continue;
                if (it->is_directory(ec))
                    fs::create_directories(target, ec);
                else if (!ec)
                    fs::copy_file(it->path(), target,
                        fs::copy_options::overwrite_existing, ec);
                if (ec)
                    return false;
            }
            if (!ec)
                fs::copy_file(from / kVersionFile, to / kVersionFile,
                    fs::copy_options::overwrite_existing, ec);
            return !ec;
        }

        // Form of a texture file and its "<TEX>.dds" name; false if it's not one.
        bool TextureName(const fs::path& p, Form& form, fs::path& name)
        {
            form = HasExtension(p, ".SBLINK") ? Form::Link
                : HasExtension(p, ".ZST")     ? Form::Zst
                                              : Form::Plain;
            name = form == Form::Plain ? p.filename() : p.stem();
            return HasExtension(name, ".DDS");
        }

        // Target of the .sblink under `root`, or empty if it's bad: absolute,
        // points outside the mod, not a .dds or missing.
        fs::path ResolveLink(const fs::path& root, const fs::path& link)
        {
            std::ifstream in(link, std::ios::binary);
            std::string line;
            std::getline(in, line);
            while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
                line.pop_back();
            const fs::path target(std::u8string(line.begin(), line.end()));
            bool inside = !target.empty() && target.is_relative()
                && !target.has_root_name();
            for (const auto& part : target)
                inside = inside && part != "..";
            std::error_code ec;
            if (inside && HasExtension(target, ".DDS"))
                for (const auto& path :
                    { root / target, root / (target.string() + ".zst") })
                    if (fs::is_regular_file(path, ec))
                        return path;
            fprintf(stderr, "[mods] bad link %s\n", link.string().c_str());
            return {};
        }
    }

    // --- The manifest ---

    bool ValidKey(std::string_view key)
    {
        if (key.empty() || key.size() > 64)
            return false;
        return std::all_of(key.begin(), key.end(),
            [](char c)
            { return IsDigit(c) || (c >= 'a' && c <= 'z') || c == '_' || c == '-'; });
    }

    uint64_t TagVersion(std::string_view key, std::string_view tag)
    {
        if (!ValidKey(key) || tag.size() < key.size() + 3
            || tag.substr(0, key.size()) != key || tag.substr(key.size(), 2) != "-v")
            return 0;
        return PlainNumber(tag.substr(key.size() + 2));
    }

    bool ParseManifest(std::string_view json, std::vector<Entry>& out, std::string& error)
    {
        using update::json::Kind;
        using update::json::Value;
        out.clear();
        Value root;
        if (!update::json::Parse(json, root, error))
            return false;
        if (root.kind != Kind::Object)
            return error = "not an object", false;
        std::vector<Entry> entries;
        const Value* mods = root.Member("mods", error);
        if (!error.empty())
            return false;
        if (!mods || mods->kind != Kind::Object)
            return error = mods ? "\"mods\" of the wrong type" : "no \"mods\"", false;
        for (size_t m = 0; m < mods->keys.size(); m++)
        {
            const std::string& key = mods->keys[m];
            const Value& value = mods->values[m];
            if (!ValidKey(key))
                return error = std::format("a bad mod name \"{}\"", key), false;
            if (Find(entries, key))
                return error = std::format("a second \"{}\"", key), false;
            if (value.kind != Kind::Object)
                return error = std::format("\"{}\" of the wrong type", key), false;
            const Value* tag = value.Member("tag", error);
            const Value* file = error.empty() ? value.Member("file", error) : nullptr;
            if (!error.empty())
                return error = std::format("{}: {}", key, error), false;
            if (!tag || tag->kind != Kind::String)
                return error = std::format("{}: {} \"tag\"", key,
                                           tag ? "a non-string" : "no"),
                       false;
            if (!file || file->kind != Kind::String)
                return error = std::format("{}: {} \"file\"", key,
                                           file ? "a non-string" : "no"),
                       false;
            Entry e{ key, tag->text, file->text, TagVersion(key, tag->text) };
            if (!e.version)
                return error = std::format("{}: a bad tag \"{}\"", key, e.tag), false;
            if (!download::PlainFileName(e.file))
                return error = std::format("{}: a bad file name \"{}\"", key, e.file),
                       false;
            entries.push_back(std::move(e));
        }
        out = std::move(entries);
        return true;
    }

    const Entry* Find(const std::vector<Entry>& entries, std::string_view key)
    {
        for (const Entry& e : entries)
            if (e.key == key)
                return &e;
        return nullptr;
    }

    uint64_t ParseVersionFile(std::string_view text, std::string_view key)
    {
        auto space = [](char c)
        { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
        while (!text.empty() && space(text.front()))
            text.remove_prefix(1);
        while (!text.empty() && space(text.back()))
            text.remove_suffix(1);
        return TagVersion(key, text);
    }

    std::string VersionFileText(std::string_view tag)
    {
        return std::string(tag) + "\n";
    }

    // --- Sets on disk ---

    fs::path ModsFolder(const fs::path& data)
    {
        return data / "mods";
    }

    fs::path UpdateFolder(const fs::path& data)
    {
        return data / "mods_update";
    }

    uint64_t ReadVersion(const fs::path& file, std::string_view key)
    {
        std::ifstream in(file, std::ios::binary);
        char text[256];
        in.read(text, sizeof(text));
        return ParseVersionFile(std::string_view(text, size_t(in.gcount())), key);
    }

    uint64_t InstalledVersion(const fs::path& data, std::string_view key)
    {
        return ReadVersion(ModsFolder(data) / key / kVersionFile, key);
    }

    uint64_t PendingVersion(const fs::path& data, std::string_view key)
    {
        return ReadVersion(UpdateFolder(data) / key / kVersionFile, key);
    }

    bool HasTextures(const fs::path& dir)
    {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(dir, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        {
            Form form;
            fs::path name;
            if (TextureName(it->path(), form, name) && it->is_regular_file(ec))
                return true;
        }
        return false;
    }

    bool MoveSet(const fs::path& data, std::string_view key, std::string& error)
    {
        const fs::path from = UpdateFolder(data) / key, to = ModsFolder(data) / key;
        std::error_code ec;
        if (!fs::is_regular_file(from / kVersionFile, ec))
        {
            error = from.string() + " has no " + kVersionFile;
            return false;
        }
        fs::create_directories(ModsFolder(data), ec);
        fs::remove_all(to, ec);
        if (ec)
        {
            error = "can't remove " + to.string() + ": " + ec.message();
            return false;
        }
        fs::rename(from, to, ec);
        if (!ec)
            return true;
        // Another drive (mods/ links somewhere else): copy,then drop the original.
        std::error_code renameError = ec;
        if (CopySet(from, to, ec))
        {
            fs::remove_all(from, ec);
            return true;
        }
        error = "can't move " + from.string() + " to " + to.string() + ": "
            + renameError.message() + "; copying: " + ec.message();
        fs::remove_all(to, ec);
        return false;
    }

    Placed PlaceSet(const fs::path& data, std::string_view key, std::string& error)
    {
        if (HasTextures(ModsFolder(data) / key))
            return Placed::Pending;
        if (!MoveSet(data, key, error))
            return Placed::Failed;
        RemoveUpdateFolderIfEmpty(data);
        return Placed::Installed;
    }

    std::vector<std::string> ApplyPendingAtBoot(const fs::path& data)
    {
        std::vector<std::string> installedKeys;
        std::error_code ec;
        std::vector<fs::path> sets;
        for (const auto& entry : fs::directory_iterator(UpdateFolder(data), ec))
            if (entry.is_directory(ec) && !entry.is_symlink(ec))
                sets.push_back(entry.path());
        for (const fs::path& dir : sets)
        {
            const std::string key = dir.filename().string();
            const uint64_t pending = ValidKey(key) ? PendingVersion(data, key) : 0;
            const uint64_t installed = pending ? InstalledVersion(data, key) : 0;
            if (pending > installed)
            {
                std::string error;
                if (MoveSet(data, key, error))
                {
                    fprintf(stderr, "[mods] %s: v%llu installed (was %s)\n", key.c_str(),
                        (unsigned long long)pending,
                        installed ? ("v" + std::to_string(installed)).c_str()
                                  : "none or unversioned");
                    installedKeys.push_back(key);
                    continue;
                }
                fprintf(stderr, "[mods] %s: v%llu not installed: %s\n", key.c_str(),
                    (unsigned long long)pending, error.c_str());
            }
            else
                fprintf(stderr, "[mods] %s: deleted (%s)\n", dir.string().c_str(),
                    !ValidKey(key) ? "not a mod's name"
                    : pending      ? "not newer than the installed set"
                                   : "no VERSION");
            fs::remove_all(dir, ec);
        }
        RemoveUpdateFolderIfEmpty(data);

        s_presentAtBoot.clear();
        for (const auto& entry : fs::directory_iterator(ModsFolder(data), ec))
            if (entry.is_directory(ec) && HasTextures(entry.path()))
                s_presentAtBoot.push_back(entry.path().filename().string());
        return installedKeys;
    }

    bool PresentAtBoot(std::string_view key)
    {
        return std::ranges::find(s_presentAtBoot, key) != s_presentAtBoot.end();
    }

    void RemoveUpdateFolderIfEmpty(const fs::path& data)
    {
        std::error_code ec;
        const fs::path folder = UpdateFolder(data);
        if (fs::is_directory(folder, ec) && fs::is_empty(folder, ec) && !ec)
            fs::remove(folder, ec);
    }

    // --- Mod state ---

    const Mod* FindMod(std::string_view key)
    {
        for (const Mod& m : kMods)
            if (key == m.key)
                return &m;
        return nullptr;
    }

    const Mod* FindMod(settings::Id setting)
    {
        for (const Mod& m : kMods)
            if (m.setting == setting)
                return &m;
        return nullptr;
    }

    void StartSession(const fs::path& data)
    {
        if (!kAvailable)
            return;
        s_data = data;
        const std::vector<std::string> installed = ApplyPendingAtBoot(data);
        for (const Mod& m : kMods)
        {
            Refresh(data, m.key);
            const Disk& d = s_disk[IndexOf(&m)];
            const bool justInstalled =
                std::find(installed.begin(), installed.end(), m.key) != installed.end();
            if (d.files && !justInstalled)  // ApplyPendingAtBoot logged those
                fprintf(stderr, "[mods] %s: v%llu%s\n", m.key,
                    (unsigned long long)d.installed, d.installed ? "" : " (no VERSION)");
        }
    }

    void Refresh(const fs::path& data, std::string_view key)
    {
        const Mod* mod = FindMod(key);
        if (!mod)
            return;
        Disk d;
        d.files = HasTextures(ModsFolder(data) / key);
        d.installed = InstalledVersion(data, key);
        d.pending = PendingVersion(data, key);
        if (d.pending <= d.installed)
            d.pending = 0;  // purged at the next start
        {
            std::lock_guard lock(s_mutex);
            s_disk[IndexOf(mod)] = d;
        }
        // Files that weren't there on start, or a newer set is waiting.
        settings::SetModWaiting(mod->setting,
            (d.files && !PresentAtBoot(key)) || d.pending);
    }

    void SetManifest(const std::vector<Entry>& manifest)
    {
        std::lock_guard lock(s_mutex);
        for (size_t i = 0; i < kCount; i++)
        {
            const Entry* e = Find(manifest, kMods[i].key);
            s_available[i] = e ? e->version : 0;
        }
    }

    State GetState(std::string_view key)
    {
        const Mod* mod = FindMod(key);
        if (!mod)
            return {};
        std::lock_guard lock(s_mutex);
        const size_t i = IndexOf(mod);
        const Disk& d = s_disk[i];
        const uint64_t available = s_available[i];
        return { Classify(d.files, d.installed, d.pending, available), d.installed,
            d.pending, available };
    }

    // --- Texture files ---

    fs::path Folder(std::string_view key)
    {
        return ModsFolder(s_data) / key;
    }

    void ToUpper(std::string& s)
    {
        std::ranges::transform(s, s.begin(),
            [](unsigned char c) { return char(std::toupper(c)); });
    }

    bool HasExtension(const fs::path& p, std::string_view ext)
    {
        std::string e = p.extension().string();
        ToUpper(e);
        return e == ext;
    }

    void ForEachTexture(const fs::path& root, const fs::path& dir,
        const AddTextureFn& add)
    {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(dir, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec))
        {
            const fs::path& p = it->path();
            // Top level `_` folders (_overrides) aren't part of the base set.
            if (it.depth() == 0 && p.filename().string().starts_with('_')
                && it->is_directory(ec))
            {
                it.disable_recursion_pending();
                continue;
            }
            Form form;
            fs::path name;
            if (!TextureName(p, form, name) || !it->is_regular_file(ec))
                continue;
            TextureFile file{ form == Form::Link ? ResolveLink(root, p) : p, form };
            std::string texture = name.stem().string();
            ToUpper(texture);  // the way the game names it
            if (!file.path.empty())
                add(p, std::move(texture), std::move(file));
        }
    }
}
