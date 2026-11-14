// Tests for mods/mods.h: manifest, mod tags and VERSION files; then in a
// scratch folder the downloaded mod sets on disk (moving a set in, the swap
// and clean-up on start, which mods had textures), the state of each mod and
// the restart its Settings row asks for. From the repo root:
//   clang++ -std=c++20 -O2 -Iruntime
//     -isystem tools/XenonRecomp/thirdparty/tomlplusplus/include
//     tests/mods_test.cpp runtime/mods/mods.cpp runtime/download/unpack.cpp
//     runtime/update/json.cpp runtime/user/settings.cpp
//     runtime/platform/posix/file_io.cpp -o build/mods_test
//   build/mods_test <scratch folder>
// (Windows: win32/file_io.cpp and folders.cpp, shell32 and ole32 instead;
// tests/CMakeLists.txt.)
#include <download/download.h>  // GitHubReleaseUrl
#include <mods/mods.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; \
    printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

namespace fs = std::filesystem;
using namespace mods;
using settings::Id;

static void Manifest()
{
    std::vector<Entry> e;
    std::string error;
    const char* good = R"({"mods":{"hd_hud":{"tag":"hd_hud-v1","file":"hd_hud-v1.zip"},)"
                       R"("button_packs":{"tag":"button_packs-v12",)"
                       R"("file":"button_packs-v12.zip"}}})";
    CHECK(ParseManifest(good, e, error) && e.size() == 2, "%s", error.c_str());
    CHECK(e.size() == 2 && e[0].key == "hd_hud" && e[0].tag == "hd_hud-v1"
        && e[0].file == "hd_hud-v1.zip" && e[0].version == 1, "first");
    CHECK(e.size() == 2 && e[1].key == "button_packs" && e[1].version == 12,
        "second, in manifest order");
    CHECK(Find(e, "button_packs") == &e[1] && !Find(e, "nope"), "Find");

    const char* extra =
        " {\n \"x\": [1, 2.5e3, null, true, false, {\"a\":\"\\u00e9\\ud83d\\ude00\"}],\n"
        " \"mods\": {}\n} \n";
    CHECK(ParseManifest(extra, e, error) && e.empty(),
        "unknown members anywhere are ignored: %s", error.c_str());
    const char* extraMembers =
        R"({"mods":{"hd_hud":{"tag":"hd_hud-v3","file":"a.zip",)"
        R"("size":5,"note":{"x":[]}}}})";
    CHECK(ParseManifest(extraMembers, e, error) && e.size() == 1 && e[0].version == 3,
        "unknown entry members: %s", error.c_str());

    const char* bad[] = {
        "",
        "[]",
        "{}",
        R"({"mods":[]})",
        R"({"mods":{}} x)",
        R"({"mods":{},"mods":{}})",
        R"({"mods":{"hd_hud":{"tag":"hd_hud-v1","file":"a.zip"},)"
        R"("hd_hud":{"tag":"hd_hud-v2","file":"b.zip"}}})",
        R"({"mods":{"hd_hud":{"tag":"hd_hud-v1","tag":"hd_hud-v2","file":"a.zip"}}})",
        R"({"mods":{"hd_hud":{"file":"a.zip"}}})",
        R"({"mods":{"hd_hud":{"tag":"hd_hud-v1"}}})",
        R"({"mods":{"hd_hud":{"tag":1,"file":"a.zip"}}})",
        R"({"mods":{"hd_hud":{"tag":"hd_hud-v1","file":null}}})",
        R"({"mods":{"hd_hud":"hd_hud-v1"}})",
        R"({"mods":{"hd_hud":{"tag":"other-v1","file":"a.zip"}}})",
        R"({"mods":{"hd_hud":{"tag":"hd_hud-v0","file":"a.zip"}}})",
        R"({"mods":{"hd_hud":{"tag":"hd_hud-v01","file":"a.zip"}}})",
        R"({"mods":{"hd_hud":{"tag":"hd_hud-v1.1","file":"a.zip"}}})",
        R"({"mods":{"hd_hud":{"tag":"hd_hud-v1","file":"../a.zip"}}})",
        R"({"mods":{"hd_hud":{"tag":"hd_hud-v1","file":"a/b.zip"}}})",
        R"({"mods":{"hd_hud":{"tag":"hd_hud-v1","file":".zip"}}})",
        R"({"mods":{"hd_hud":{"tag":"hd_hud-v1","file":""}}})",
        R"({"mods":{"HD":{"tag":"HD-v1","file":"a.zip"}}})",
        R"({"mods":{"../x":{"tag":"../x-v1","file":"a.zip"}}})",
        R"({"mods":{"":{"tag":"-v1","file":"a.zip"}}})",
        R"({"mods":{"a":{"tag":"a-v1","file":"a.zip"},}})",
        "{\"mods\":{\"a\":{\"tag\":\"a-v1\",\"file\":\"a\xff.zip\"}}}",
        R"({"mods":{"a":{"tag":"a-v1","file":"a.zip"}})",
        R"({"x":"\ud800","mods":{}})",
        R"({"x":01,"mods":{}})",
    };
    for (const char* json : bad)
    {
        error.clear();
        CHECK(!ParseManifest(json, e, error) && !error.empty() && e.empty(),
            "'%s' isn't a manifest", json);
    }
    std::string deep = std::string(70, '[') + std::string(70, ']');
    CHECK(!ParseManifest(R"({"mods":{},"x":)" + deep + "}", e, error),
        "64 levels at most");
}

static void Tags()
{
    CHECK(TagVersion("hd_hud", "hd_hud-v1") == 1, "v1");
    CHECK(TagVersion("hd_hud", "hd_hud-v123") == 123, "v123");
    for (const char* bad : { "hd_hud-v0", "hd_hud-v01", "hd_hud-v", "hd_hud-1",
             "hd_hud-v1 ", "hd_hud-v1a", "hd_hud_v1", "hd-v1", "hd_hud-v-1",
             "hd_hud-v1234567890123456789", "" })
        CHECK(TagVersion("hd_hud", bad) == 0, "'%s'", bad);
    CHECK(TagVersion("a-b", "a-b-v2") == 2, "keys with hyphens");
    CHECK(ValidKey("hd_hud") && ValidKey("button-packs2") && !ValidKey("")
        && !ValidKey("Hd") && !ValidKey("a/b") && !ValidKey("a.b")
        && !ValidKey(std::string(65, 'a')), "keys");
    CHECK(download::GitHubReleaseUrl(SB_MODS_REPO, "hd_hud-v1", "hd_hud-v1.zip")
              == "https://github.com/" SB_MODS_REPO
                 "/releases/download/hd_hud-v1/hd_hud-v1.zip",
        "release URL");

    CHECK(UpdateOffered(2, 1, 0) && UpdateOffered(1, 0, 0), "newer than installed");
    CHECK(!UpdateOffered(2, 1, 2) && !UpdateOffered(1, 1, 0) && !UpdateOffered(1, 2, 0)
        && !UpdateOffered(0, 0, 0), "not newer");
    CHECK(UpdateOffered(3, 1, 2), "newer than the waiting set too");
    CHECK(Classify(false, 0, 0, 1) == Status::Missing
        && Classify(false, 0, 0, 0) == Status::Missing, "missing");
    CHECK(Classify(true, 0, 0, 0) == Status::Installed
        && Classify(true, 2, 0, 2) == Status::Installed, "installed");
    CHECK(Classify(true, 0, 0, 1) == Status::UpdateAvailable,
        "unversioned files: any release is an update");
    CHECK(Classify(true, 1, 2, 2) == Status::Pending
        && Classify(false, 0, 1, 1) == Status::Pending, "waiting");
    CHECK(Classify(true, 1, 2, 3) == Status::UpdateAvailable,
        "newer than the waiting set");
}

static void VersionFile()
{
    CHECK(ParseVersionFile("hd_hud-v2\n", "hd_hud") == 2, "VERSION");
    CHECK(ParseVersionFile("  hd_hud-v7\r\n\n", "hd_hud") == 7, "trimmed");
    CHECK(ParseVersionFile("", "hd_hud") == 0
        && ParseVersionFile("garbage", "hd_hud") == 0
        && ParseVersionFile("other-v3", "hd_hud") == 0, "none");
    CHECK(ParseVersionFile(VersionFileText("hd_hud-v4"), "hd_hud") == 4
        && VersionFileText("x-v1") == "x-v1\n", "round trip");
}

static void Write(const fs::path& p, const std::string& text)
{
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

static std::string Read(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

static std::string Tag(const std::string& key, int version)
{
    return VersionFileText(key + "-v" + std::to_string(version));
}

// mods_update/<key>/ with one texture and VERSION (0: none).
static void Unpacked(const fs::path& data, const std::string& key, int version,
    const std::string& text)
{
    Write(UpdateFolder(data) / key / "PACK" / "A.dds.zst", text);
    if (version)
        Write(UpdateFolder(data) / key / kVersionFile, Tag(key, version));
}

static void Installed(const fs::path& data, const std::string& key, int version,
    const std::string& text)
{
    Write(ModsFolder(data) / key / "PACK" / "A.dds.zst", text);
    Write(ModsFolder(data) / key / "OLD_ONLY.dds", "old");
    if (version)
        Write(ModsFolder(data) / key / kVersionFile, Tag(key, version));
}

static void SetsOnDisk(const fs::path& data)
{
    std::string error;

    // Versions and textures.
    CHECK(InstalledVersion(data, "hd_hud") == 0 && PendingVersion(data, "hd_hud") == 0,
        "nothing");
    Installed(data, "hd_hud", 2, "v2");
    CHECK(InstalledVersion(data, "hd_hud") == 2, "installed v2");
    Write(ModsFolder(data) / "x" / kVersionFile, "hd_hud-v3\n");
    CHECK(ReadVersion(ModsFolder(data) / "x" / kVersionFile, "button_packs") == 0,
        "tag of another key");
    CHECK(HasTextures(ModsFolder(data) / "hd_hud"), "textures");
    Write(ModsFolder(data) / "empty" / "readme.txt", "x");
    Write(ModsFolder(data) / "empty" / "a.dds.txt", "x");
    Write(ModsFolder(data) / "empty" / "NOTICE", "x");
    Write(ModsFolder(data) / "empty" / "LICENSE", "x");
    CHECK(!HasTextures(ModsFolder(data) / "empty") && !HasTextures(data / "missing"),
        "no textures");
    Write(ModsFolder(data) / "upper" / "deep" / "B.DDS.SBLINK", "x");
    CHECK(HasTextures(ModsFolder(data) / "upper"), "any case, any depth");

    // The walk skips NOTICE/LICENSE and top level `_` folders.
    const fs::path set = ModsFolder(data) / "set";
    Write(set / "NOTICE", "x");
    Write(set / "LICENSE", "x");
    Write(set / "PACK" / "A.dds", "x");
    Write(set / "_overrides" / "RUSSIAN" / "PACK" / "A.dds", "x");
    Write(set / "PACK" / "_sub" / "B.dds", "x");
    std::vector<std::string> found;
    ForEachTexture(set, set, [&](const fs::path&, std::string name, TextureFile)
        { found.push_back(std::move(name)); });
    std::ranges::sort(found);
    const std::vector<std::string> expected{ "A", "B" };
    CHECK(found == expected, "%zu textures", found.size());
    fs::remove_all(data);

    // Fresh install: moved in right away, mods_update/ gone with it.
    Unpacked(data, "hd_hud", 1, "new");
    CHECK(PlaceSet(data, "hd_hud", error) == Placed::Installed, "%s", error.c_str());
    CHECK(InstalledVersion(data, "hd_hud") == 1
        && Read(ModsFolder(data) / "hd_hud" / "PACK" / "A.dds.zst") == "new", "moved");
    CHECK(!fs::exists(UpdateFolder(data)), "mods_update/ removed");

    // A folder without textures is replaced too.
    fs::remove_all(data);
    Write(ModsFolder(data) / "hd_hud" / "notes.txt", "x");
    Unpacked(data, "hd_hud", 1, "new");
    CHECK(PlaceSet(data, "hd_hud", error) == Placed::Installed, "%s", error.c_str());
    CHECK(!fs::exists(ModsFolder(data) / "hd_hud" / "notes.txt"), "old folder replaced");

    // Update: waits, and download files next to it stay.
    fs::remove_all(data);
    Installed(data, "hd_hud", 1, "v1");
    Unpacked(data, "hd_hud", 2, "v2");
    Write(UpdateFolder(data) / "button_packs-v1.zip.part", "part");
    CHECK(PlaceSet(data, "hd_hud", error) == Placed::Pending, "pending");
    CHECK(PendingVersion(data, "hd_hud") == 2 && InstalledVersion(data, "hd_hud") == 1,
        "both there");
    // No VERSION: refused.
    Unpacked(data, "button_packs", 0, "x");
    CHECK(!MoveSet(data, "button_packs", error) && !error.empty(), "no VERSION");

    // Start: the newer set moves in (old files gone), the unversioned set,
    // junk and links get deleted, root files stay.
    Write(UpdateFolder(data) / "Bad Key" / "x.dds", "x");
    ApplyPendingAtBoot(data);
    CHECK(InstalledVersion(data, "hd_hud") == 2
        && Read(ModsFolder(data) / "hd_hud" / "PACK" / "A.dds.zst") == "v2", "swapped");
    CHECK(!fs::exists(ModsFolder(data) / "hd_hud" / "OLD_ONLY.dds"),
        "old set files gone");
    CHECK(!fs::exists(UpdateFolder(data) / "hd_hud")
        && !fs::exists(UpdateFolder(data) / "button_packs")
        && !fs::exists(UpdateFolder(data) / "Bad Key"), "subfolders purged");
    CHECK(Read(UpdateFolder(data) / "button_packs-v1.zip.part") == "part",
        "download kept");
    CHECK(PresentAtBoot("hd_hud") && !PresentAtBoot("button_packs"), "present at boot");

    // Not newer: deleted, installed set untouched.
    Unpacked(data, "hd_hud", 2, "same");
    ApplyPendingAtBoot(data);
    CHECK(Read(ModsFolder(data) / "hd_hud" / "PACK" / "A.dds.zst") == "v2"
        && !fs::exists(UpdateFolder(data) / "hd_hud"), "same version");
    // Unversioned installed set (version 0): any vN replaces it.
    fs::remove(ModsFolder(data) / "hd_hud" / kVersionFile);
    Unpacked(data, "hd_hud", 1, "v1 again");
    ApplyPendingAtBoot(data);
    CHECK(InstalledVersion(data, "hd_hud") == 1, "over version 0");

    // Crash in the middle of a swap (installed set removed,
    // the waiting one whole): finished on next start.
    Unpacked(data, "hd_hud", 3, "v3");
    fs::remove_all(ModsFolder(data) / "hd_hud");
    ApplyPendingAtBoot(data);
    CHECK(InstalledVersion(data, "hd_hud") == 3, "finished");

    // Empty mods_update/ goes; a folder with only a download stays.
    fs::remove(UpdateFolder(data) / "button_packs-v1.zip.part");
    ApplyPendingAtBoot(data);
    CHECK(!fs::exists(UpdateFolder(data)), "empty mods_update/ removed");
}

// No environment override for this run (settings::EnvOverride).
static void Unset(const char* name)
{
#ifdef _WIN32
    _putenv_s(name, "");  // "" removes it
#else
    unsetenv(name);
#endif
}

static void ModStates(const fs::path& data)
{
    for (const settings::Option& o : settings::Options())
        if (o.env)
            Unset(o.env);

    // HD HUD on but not downloaded, original icons; a v1 button pack set is
    // installed with v2 waiting (moved in on start).
    Write(data / "settings.toml", "[mods]\nhd_hud_mod = true\n");
    CHECK(settings::Load(data / "settings.toml"), "settings load");
    Write(data / "mods/button_packs/ps4/A.dds", "1");
    Write(data / "mods/button_packs/VERSION", "button_packs-v1\n");
    Write(data / "mods_update/button_packs/ps4/A.dds", "2");
    Write(data / "mods_update/button_packs/VERSION", "button_packs-v2\n");
    StartSession(data);

    State hud = GetState("hd_hud"), packs = GetState("button_packs");
    CHECK(hud.status == Status::Missing, "HD HUD missing");
    CHECK(packs.status == Status::Installed && packs.installed == 2 && !packs.pending,
        "packs v2 installed at the start");
    CHECK(!settings::RestartPending(Id::HdHudMod)
        && !settings::RestartPending(Id::GamepadPrompts), "nothing to restart for");

    // The manifest: HD HUD v1, packs v3.
    std::vector<Entry> manifest;
    std::string error;
    CHECK(ParseManifest(R"({"mods":{"hd_hud":{"tag":"hd_hud-v1","file":"h.zip"},)"
                        R"("button_packs":{"tag":"button_packs-v3","file":"b.zip"}}})",
              manifest, error),
        "%s", error.c_str());
    SetManifest(manifest);
    hud = GetState("hd_hud"), packs = GetState("button_packs");
    CHECK(hud.status == Status::Missing && hud.available == 1,
        "HD HUD still missing, v1 offered");
    CHECK(packs.status == Status::UpdateAvailable && packs.available == 3,
        "packs v3 offered");

    // HD HUD downloaded mid-session (in place):
    // on, but not active untill we restart.
    Write(data / "mods/hd_hud/a.dds.zst", "x");
    Write(data / "mods/hd_hud/VERSION", "hd_hud-v1\n");
    Refresh(data, "hd_hud");
    CHECK(GetState("hd_hud").status == Status::Installed, "HD HUD installed");
    CHECK(settings::RestartPending(Id::HdHudMod), "a restart turns it on");

    // Packs v3 waiting: original icons need no restart, until a pack is picked.
    Write(data / "mods_update/button_packs/ps4/A.dds", "3");
    Write(data / "mods_update/button_packs/VERSION", "button_packs-v3\n");
    Refresh(data, "button_packs");
    packs = GetState("button_packs");
    CHECK(packs.status == Status::Pending && packs.pending == 3, "packs v3 waiting");
    CHECK(!settings::RestartPending(Id::GamepadPrompts),
        "original icons: nothing to restart for");
    settings::Set(Id::GamepadPrompts, 4);
    CHECK(settings::RestartPending(Id::GamepadPrompts), "a pack picked");

    // A set still unpacking (no VERSION yet) isn't waiting.
    fs::remove(data / "mods_update/button_packs/VERSION");
    Refresh(data, "button_packs");
    CHECK(GetState("button_packs").status == Status::UpdateAvailable,
        "unpacking: v3 still offered");

    // The next start: the HD HUD is active; a v2 waiting asks for a restart.
    CHECK(settings::Load(data / "settings.toml"), "settings reload");
    StartSession(data);
    CHECK(PresentAtBoot("hd_hud") && !settings::RestartPending(Id::HdHudMod),
        "HD HUD active");
    Write(data / "mods_update/hd_hud/a.dds.zst", "y");
    Write(data / "mods_update/hd_hud/VERSION", "hd_hud-v2\n");
    Refresh(data, "hd_hud");
    CHECK(GetState("hd_hud").status == Status::Pending, "HD HUD v2 waiting");
    CHECK(settings::RestartPending(Id::HdHudMod), "a restart swaps it in");

    CHECK(FindMod("hd_hud")->setting == Id::HdHudMod && !FindMod("nope"), "lookup");
}

int main(int argc, char** argv)
{
    const fs::path scratch = argc > 1 ? argv[1] : "mods_out";
    Manifest();
    Tags();
    VersionFile();
    std::error_code ec;
    fs::remove_all(scratch, ec);
    SetsOnDisk(scratch / "disk");
    ModStates(scratch / "state");
    fs::remove_all(scratch, ec);
    printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "all passed", g_failures,
        g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
