// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See mods.h.
//
// Mod download job and our part of the update check (README.md).
#include "mods.h"

#include <download/download.h>
#include <download/unpack.h>
#include <install/result.h>  // FormatSize
#include <report/report.h>
#include <ui/ui.h>
#include <update/fetch.h>
#include <update/release_names.h>  // SB_APP_NAME
#include <user/paths.h>

#include <algorithm>
#include <cstdio>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <system_error>

namespace mods
{
    namespace
    {
        namespace fs = std::filesystem;

        // Removes download parts and hash sidecars in mods_update/ that no
        // manifest entry names anymore (older releases).
        void DropStrangers(const fs::path& folder, const std::vector<Entry>& manifest)
        {
            std::error_code ec;
            std::vector<fs::path> strangers;
            for (const auto& entry : fs::directory_iterator(folder, ec))
            {
                if (!entry.is_regular_file(ec))
                    continue;
                std::string name = entry.path().filename().string();
                if (name.ends_with(".part"))
                    name.resize(name.size() - 5);
                else if (name.ends_with(".sha256"))
                    name.resize(name.size() - 7);
                else
                    continue;
                bool known = false;
                for (const Entry& e : manifest)
                    known |= e.file == name;
                if (!known)
                    strangers.push_back(entry.path());
            }
            for (const fs::path& path : strangers)
            {
                fprintf(stderr, "[mods] deleted %s (not in the manifest)\n",
                    path.string().c_str());
                std::filesystem::remove(path, ec);
            }
        }

        bool Resolve(const fs::path& data, Entry& entry, const download::Context& ctx,
            download::Target& target, download::Failure& f)
        {
            std::string body, error;
            if (!ctx.Get(kManifestUrl, kMaxManifestBytes, body, f, "manifest", false))
                return false;
            std::vector<Entry> manifest;
            if (!ParseManifest(body, manifest, error))
            {
                f = { download::Error::NotAvailable, "manifest: " + error };
                return false;
            }
            SetManifest(manifest);
            const Entry* found = Find(manifest, entry.key);
            if (!found)
            {
                f = { download::Error::NotAvailable, "not in the manifest" };
                return false;
            }
            entry = *found;
            DropStrangers(UpdateFolder(data), manifest);
            target.url = download::GitHubReleaseUrl(SB_MODS_REPO, entry.tag, entry.file);
            target.name = entry.file;
            target.version = std::format("v{}", entry.version);
            target.spaceFactor = 2;  // the zip plus an unpacked copy
            return true;
        }

        bool Finish(const fs::path& data, const Entry& entry,
            const download::Context& ctx, const fs::path& file, std::string& log,
            download::Failure& f)
        {
            using download::Error;
            download::ZipArchive zip;
            const download::ZipCheck check = download::OpenZip(file, entry.key, zip);
            if (check.error != download::ZipError::None)
            {
                const bool rejected = check.error == download::ZipError::Rejected;
                f = { rejected ? Error::Rejected : Error::Damaged, check.detail };
                if (f.error == Error::Rejected)
                    f.message = "The download isn't a " SB_APP_NAME " mod.";
                return false;
            }
            const uint64_t size = zip.contents.unpackedSize, files = zip.contents.files;
            const std::optional<uint64_t> free = download::FreeSpace(UpdateFolder(data));
            if (free && *free < size)
            {
                f = download::NoSpace(size, *free, "unpacking");
                return false;
            }

            const fs::path target = UpdateFolder(data) / entry.key;
            ctx.Progress(0, files);
            const download::Extracted got = download::ExtractZip(zip, target,
                [&] { return ctx.Cancelled(); },
                [&](uint64_t done) { ctx.Progress(done, files); });
            switch (got.result)
            {
            case download::Extract::Ok: break;
            case download::Extract::Cancelled:
                f = { Error::Cancelled, got.detail };
                return false;
            case download::Extract::Damaged:
                f = { Error::Damaged, got.detail };
                return false;
            case download::Extract::CantWrite:
                f = download::CantWrite(got.detail, "unpacking");
                return false;
            }
            {
                const fs::path version = target / kVersionFile;
                std::ofstream out(version, std::ios::binary | std::ios::trunc);
                out << VersionFileText(entry.tag);
                if (!out.flush())
                {
                    std::error_code ec;
                    fs::remove_all(target, ec);
                    f = download::CantWrite(version, "VERSION");
                    return false;
                }
            }
            const std::string unpacked =
                std::format("{} files, {}", files, install::FormatSize(size));

            // The set is complete. If were exiting there's no time to move it
            // in (copying across drives can be slow), next start does that.
            if (ctx.Exiting())
            {
                log = std::format("unpacked ({}); waits for the next start (exiting)",
                    unpacked);
                return true;
            }
            std::string error;
            const Placed placed = PlaceSet(data, entry.key, error);
            Refresh(data, entry.key);
            if (placed == Placed::Failed)
            {
                f = download::CantWrite(ModsFolder(data) / entry.key, error);
                return false;
            }
            const char* what =
                placed == Placed::Installed ? "installed" : "waits for the next start";
            log = std::format("{} ({})", what, unpacked);
            return true;
        }

        download::Task MakeTask(const std::string& key)
        {
            const fs::path data = GetUserPath();
            auto entry = std::make_shared<Entry>();
            entry->key = key;
            download::Task task;
            task.label = key;
            task.folder = UpdateFolder(data);
            task.userAgent = std::format("SpeedBreaker/{}", report::Version());
            task.resolve = [data, entry](const download::Context& ctx,
                               download::Target& target, download::Failure& f) {
                return Resolve(data, *entry, ctx, target, f);
            };
            task.finish = [data, entry](const download::Context& ctx,
                              const fs::path& file, std::string& log,
                              download::Failure& f) {
                return Finish(data, *entry, ctx, file, log, f);
            };
            return task;
        }

        // Mod tags the startup toast already announced, one per line
        // (<data>/updates/notified_mods.txt).
        std::filesystem::path NotifiedModsPath()
        {
            return GetUserPath() / "updates" / "notified_mods.txt";
        }
    }

    std::vector<std::string> NotifiedTags()
    {
        std::vector<std::string> tags;
        std::ifstream file(NotifiedModsPath(), std::ios::binary);
        for (std::string line; std::getline(file, line);)
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (!line.empty())
                tags.push_back(std::move(line));
        }
        return tags;
    }

    void RecordNotified(std::string_view key, const std::string& tag)
    {
        std::vector<std::string> tags = NotifiedTags();
        if (std::find(tags.begin(), tags.end(), tag) != tags.end())
            return;
        // Drop the older tag of this mod.
        std::erase_if(tags,
            [&](const std::string& t) { return TagVersion(key, t) != 0; });
        tags.push_back(tag);
        std::error_code ec;
        const std::filesystem::path path = NotifiedModsPath();
        std::filesystem::create_directories(path.parent_path(), ec);
        const std::filesystem::path temp = path.parent_path() / "notified_mods.txt.new";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            for (const std::string& t : tags)
                out << t << "\n";
            if (!out.flush())
            {
                fprintf(stderr,
                    "[mods] couldn't note %s as announced (%s): "
                    "it may be announced again\n",
                    tag.c_str(), temp.string().c_str());
                return;
            }
        }
        std::filesystem::rename(temp, path, ec);
        if (ec)
            fprintf(stderr,
                "[mods] couldn't note %s as announced (%s): it may be announced again\n",
                tag.c_str(), ec.message().c_str());
    }

    std::shared_ptr<const std::vector<Notice>> CheckManifest(const std::string& userAgent)
    {
        const update::Fetched answer = update::Fetch({ .url = kManifestUrl,
            .userAgent = userAgent, .headers = {}, .maxBytes = kMaxManifestBytes });
        if (answer.transport != update::Transport::Ok || answer.response.status != 200)
        {
            fprintf(stderr,
                "[mods] couldn't read the manifest (transport %d, HTTP %d%s%s)\n",
                int(answer.transport), answer.response.status,
                answer.detail.empty() ? "" : ", ", answer.detail.c_str());
            return nullptr;
        }
        std::vector<Entry> manifest;
        std::string error;
        if (!ParseManifest(answer.response.body, manifest, error))
        {
            fprintf(stderr, "[mods] bad manifest (%s)\n", error.c_str());
            return nullptr;
        }
        SetManifest(manifest);
        auto notices = std::make_shared<std::vector<Notice>>();
        std::string log;
        for (const Entry& entry : manifest)
        {
            const Mod* mod = FindMod(entry.key);
            if (!mod)
                continue;  // a mod this version doesn't know
            const State state = GetState(entry.key);
            const std::string waiting =
                state.pending ? std::format(", v{} waiting", state.pending)
                              : std::string();
            log += std::format("{}{} v{} (have v{}{})", log.empty() ? "" : ", ",
                entry.key, entry.version, state.installed, waiting);
            if (state.status == Status::UpdateAvailable)
                notices->push_back({ entry.key, entry.tag,
                    std::format("{} v{} is available: open Settings > Mods to update.",
                        settings::Info(mod->setting).label, entry.version) });
        }
        fprintf(stderr, "[mods] manifest: %s; %zu update%s\n",
            log.empty() ? "none listed" : log.c_str(), notices->size(),
            notices->size() == 1 ? "" : "s");
        return notices;
    }

    void OpenDownloadDialog(std::string_view key)
    {
        const Mod* mod = FindMod(key);
        if (!mod)
            return;
        const settings::Id id = mod->setting;
        const settings::Option& o = settings::Info(id);
        const State state = GetState(key);
        const bool update = state.status == Status::UpdateAvailable;
        const uint64_t have = std::max(state.installed, state.pending);
        const bool choice = o.type == settings::Type::Enum;

        ui::DownloadDialog d;
        d.title = std::format("{} {}", update ? "Update" : "Download", o.label);
        d.name = o.label;
        if (update && have)
            d.have = std::format("You have v{}.", have);
        d.start = [task = MakeTask(std::string(key))] { download::Start(task); };
        if (choice)
        {
            // A choice of packs: tell where to pick one.
            d.note = std::format(
                "Choose your pack in Mods > {}.\nRestart " SB_APP_NAME
                " to apply changes.",
                o.label);
        }
        else
        {
            d.question = [id, label = std::string(o.label)] {
                if (settings::GetStoredValue(id) != 0.0)
                    return std::string();
                return std::format("Turn on {}?", label);
            };
            d.yes = "Turn on";
            d.confirm = [id] {
                const settings::Option& o = settings::Info(id);
                const double from = settings::GetStoredValue(id);
                const settings::SetResult result = settings::Set(id, 1.0);
                const std::string to =
                    settings::FormatValue(id, settings::GetStoredValue(id));
                fprintf(stderr, "[settings] menu: %s.%s %s -> %s (download dialog%s)\n",
                    o.table, o.key, settings::FormatValue(id, from).c_str(), to.c_str(),
                    result == settings::SetResult::Overridden
                        ? "; the environment keeps this run's value" : "");
            };
            d.restart = [id] { return settings::RestartPending(id); };
        }
        fprintf(stderr, "[mods] %s: %s dialog\n", mod->key,
            update ? "update" : "download");
        ui::OpenDownloadDialog(std::move(d));
    }
}
