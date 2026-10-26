// Tests for the installer's regions (install/manifest.h): the USA and the
// European (PAL, Spanish) manifests, told apart by default.xex's hash and
// size and by the version an install's marker records, and FindGameInstall
// finding either region's install (with which it is) in a scratch folder with
// a sandbox HOME. From the repo root:
//   clang++ -std=c++20 -O2 -Iruntime tests/region_manifest_test.cpp runtime/install/{placement,installer,locate,manifest,result,disc_source,sha256}.cpp runtime/user/portable.cpp -o build/region_manifest_test
//   (on macOS add -framework CoreFoundation)
//   build/region_manifest_test [folder]
// No game files: the stand-in installs are a default.xex of the right size
// (all zeros) and a marker.
#include <install/installer.h>
#include <install/locate.h>
#include <install/placement.h>
#include <user/paths.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

namespace fs = std::filesystem;
using namespace install;

static void Manifests()
{
    auto all = GameManifests();
    CHECK(all.size() == 2, "two regions: %zu", all.size());
    const Manifest& usa = GameManifest();
    CHECK(all[0] == &usa, "USA first (the default)");
    const Manifest* pal = ManifestForVersion("2e42eb3950fcc436");
    CHECK(pal && pal == all[1], "the PAL manifest by its version");
    if (!pal)
        return;
    CHECK(usa.region == "usa" && pal->region == "pal-es", "region ids: %.*s %.*s", int(usa.region.size()), usa.region.data(),
        int(pal->region.size()), pal->region.data());
    CHECK(usa.version == "535fd83157066102", "the USA manifest is v0.1.9's (installs stay valid)");
    CHECK(usa.titleId == pal->titleId && usa.titleId == 0x454107D9, "one title ID for both");
    CHECK(usa.Xex().sha256 == "aebdf3c14d1e4e4ed2c130d94bc1a0e97bd6d5bf39bdfdef33c3909c9cc4ba4c", "USA default.xex");
    CHECK(pal->Xex().sha256 == "aad15fc218d034de3131f746e0b763dd8049a40dd676efc87667fcaf4fc72694", "PAL default.xex");
    CHECK(ManifestForXexHash(usa.Xex().sha256) == &usa && ManifestForXexHash(pal->Xex().sha256) == pal, "by hash");
    CHECK(ManifestForXexHash("00") == nullptr, "an unknown hash: none");
    CHECK(ManifestForXexSize(3936256) == &usa && ManifestForXexSize(3940352) == pal, "by size");
    CHECK(ManifestForXexSize(16) == nullptr, "an unknown size: none");
    CHECK(usa.files.size() == 49 && pal->files.size() == 49, "49 files each");
    CHECK(pal->totalBytes == 7006136115ull && usa.totalBytes == 7123373273ull, "sizes");
    // The PAL disc's movies are Spanish, PAL; none of the USA's names.
    size_t spanish = 0, english = 0;
    for (const ManifestFile& f : pal->files)
    {
        spanish += f.path.ends_with("_spanish_pal.wmv");
        english += f.path.find("english") != std::string_view::npos;
    }
    CHECK(spanish == 33 && english == 0, "PAL movies: %zu Spanish, %zu English", spanish, english);
    CHECK(pal->Find("NFS/ZZDATA9.BIN") && usa.Find("nfs/zzdata9.bin"), "case-insensitive lookups");
    CHECK(!pal->Find("Movies/ealogo_english_ntsc.wmv") && usa.Find("Movies/ealogo_english_ntsc.wmv"), "region-only files");

    // Pick: a compiled-in manifest takes the region the files say; a test's
    // own manifest is kept.
    static const ManifestFile files[] = { { "default.xex", 16, "00" } };
    Manifest test{ "test-1", 0x454107D9, files, 16 };
    CHECK(&Pick(usa, pal) == pal && &Pick(*pal, &usa) == &usa && &Pick(usa, nullptr) == &usa, "Pick, compiled-in");
    CHECK(&Pick(test, pal) == &test, "Pick, a test manifest");
    CHECK(IsGameManifest(usa) && IsGameManifest(*pal) && !IsGameManifest(test), "IsGameManifest");
}

static void Write(const fs::path& file, const std::string& text)
{
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

// A stand-in install: default.xex of the region's size (zeros), and the
// marker for `version` if given.
static void StandIn(const fs::path& dir, uint64_t xexSize, const char* version)
{
    fs::create_directories(dir);
    {
        std::ofstream f(dir / "default.xex", std::ios::binary);
    }
    fs::resize_file(dir / "default.xex", xexSize);
    if (version)
        Write(dir / kInstallMarker, std::string("manifest_version = \"") + version + "\"\n");
}

static void Installs(const fs::path& root, const char* argv0)
{
    const Manifest& usa = GameManifest();
    const Manifest* pal = ManifestForVersion("2e42eb3950fcc436");
    if (!pal)
        return;
    fs::path home = root / "home";
    fs::create_directories(home);
    setenv("HOME", home.c_str(), 1);
    unsetenv("XDG_DATA_HOME");
    unsetenv("NFSMW_GAME_DIR");
    SetProgramPath(argv0);
    chdir(root.c_str());  // no ./game/files here

    CHECK(!FindGameInstall(), "nothing installed");

    // The installer's own folder: by its marker.
    fs::path user = DefaultInstallPath();
    StandIn(user, pal->Xex().size, "2e42eb3950fcc436");
    std::optional<GameInstall> found = FindGameInstall();
    CHECK(found && found->manifest == pal && found->marker, "a PAL install: found, PAL (%s)",
        found && found->manifest ? std::string(found->manifest->region).c_str() : "-");
    CHECK(IsUsableInstall(user, true), "usable (any region)");
    CHECK(IsUsableInstall(user, true, usa), "the USA manifest stands for every compiled-in region");
    // A PAL default.xex with the USA marker: not current.
    Write(user / kInstallMarker, "manifest_version = \"535fd83157066102\"\n");
    CHECK(!FindGameInstall(), "PAL files, USA marker: not current (reinstall)");
    // USA.
    StandIn(user, usa.Xex().size, "535fd83157066102");
    found = FindGameInstall();
    CHECK(found && found->manifest == &usa && found->marker, "a USA install: found, USA");
    // An older manifest's marker: not current.
    Write(user / kInstallMarker, "manifest_version = \"0123456789abcdef\"\n");
    CHECK(!FindGameInstall(), "an older marker: not current");

    // Set up by hand (NFSMW_GAME_DIR): no marker, the region by size.
    fs::path byHand = root / "by-hand";
    StandIn(byHand, pal->Xex().size, nullptr);
    setenv("NFSMW_GAME_DIR", byHand.c_str(), 1);
    found = FindGameInstall();
    CHECK(found && found->manifest == pal && !found->marker && found->origin == InstallOrigin::Environment,
        "NFSMW_GAME_DIR, PAL by size");
    fs::resize_file(byHand / "default.xex", 1234);
    CHECK(!FindGameInstall(), "another size: neither region (ignored, logged)");
    unsetenv("NFSMW_GAME_DIR");

    // The recorded install (another drive), PAL.
    fs::path drive = root / "drive/game";
    StandIn(drive, pal->Xex().size, "2e42eb3950fcc436");
    CHECK(RecordInstallPath(drive), "recorded");
    found = FindGameInstall();
    CHECK(found && found->manifest == pal && found->path == fs::absolute(drive).lexically_normal(), "recorded PAL install");
    CHECK(CheckRecordedInstall().state == RecordedState::Usable, "CheckRecordedInstall: usable (PAL)");
    // An explicit test manifest stays exact.
    static const ManifestFile files[] = { { "default.xex", 16, "00" } };
    Manifest test{ "test-1", 0x454107D9, files, 16 };
    CHECK(!FindGameInstall(test), "a test manifest finds no real region's install");
}

// The PAL-safety rule (install/placement.h's KeepOtherRegion): a disc of one
// region into a folder holding the installer's install of the other goes
// beside it, never over it; a same-region install is replaced as before.
static void KeepRegions(const fs::path& root)
{
    const Manifest& usa = GameManifest();
    const Manifest* pal = ManifestForVersion("2e42eb3950fcc436");
    if (!pal)
        return;
    // The folder's name with the region's id.
    CHECK(RegionFolder("/h/game", *pal) == fs::path("/h/game PAL-ES"), "RegionFolder PAL: %s", RegionFolder("/h/game", *pal).c_str());
    CHECK(RegionFolder("/h/game/", usa) == fs::path("/h/game USA"), "RegionFolder USA, trailing separator: %s",
        RegionFolder("/h/game/", usa).c_str());
    CHECK(RegionFolder("/d/speedbreaker/game", *pal) == fs::path("/d/speedbreaker/game PAL-ES"), "a drive's folder");

    // What the disk says: the installer's marker beside a region's default.xex.
    fs::path dir = root / "keep";
    fs::path game = dir / "game", palDir = dir / "game PAL-ES";
    CHECK(!InstalledRegion(game), "nothing there: none");
    StandIn(game, usa.Xex().size, nullptr);
    CHECK(!InstalledRegion(game), "no marker (extracted by hand): none");
    StandIn(game, usa.Xex().size, "0123456789abcdef");
    CHECK(InstalledRegion(game) == &usa, "an older USA marker: still the USA install (it would be lost)");
    StandIn(game, usa.Xex().size, "535fd83157066102");
    CHECK(InstalledRegion(game) == &usa, "a USA install");
    fs::resize_file(game / "default.xex", 1234);
    CHECK(!InstalledRegion(game), "an unknown default.xex: none");
    fs::resize_file(game / "default.xex", usa.Xex().size);

    // A USA install there: the PAL disc goes beside it, the USA disc replaces it.
    RegionKeep k = KeepOtherRegion(game, *pal);
    CHECK(k.kept == &usa && k.gameDir == palDir && k.keptDir == game, "PAL beside USA: %s", k.gameDir.c_str());
    k = KeepOtherRegion(game, usa);
    CHECK(!k.kept && k.gameDir == game, "USA over USA: replaced as today");
    // The PAL folder already holding a PAL install (installed beside before):
    // that one is replaced.
    StandIn(palDir, pal->Xex().size, "2e42eb3950fcc436");
    k = KeepOtherRegion(game, *pal);
    CHECK(k.kept == &usa && k.gameDir == palDir, "PAL again: its own folder, replaced");
    CHECK(InstalledRegion(palDir) == pal, "the PAL install");
    // And the other way round: a PAL install in the default folder.
    StandIn(game, pal->Xex().size, "2e42eb3950fcc436");
    k = KeepOtherRegion(game, usa);
    CHECK(k.kept == pal && k.gameDir == dir / "game USA", "USA beside PAL: %s", k.gameDir.c_str());
    CHECK(!KeepOtherRegion(game, *pal).kept, "PAL over PAL: replaced");
    // An empty or missing folder: as asked.
    CHECK(!KeepOtherRegion(dir / "new", *pal).kept && KeepOtherRegion(dir / "new", *pal).gameDir == dir / "new", "a new folder");
    // A test's own manifest: as asked, whatever is there.
    static const ManifestFile files[] = { { "default.xex", 16, "00" } };
    Manifest test{ "test-1", 0x454107D9, files, 16 };
    CHECK(!KeepOtherRegion(game, test).kept, "a test manifest: no region rule");
    // The beside folder holding the other region too: the next number.
    k = KeepOtherRegion("/h/game", *pal, [&](const fs::path& p) -> const Manifest* {
        return p == "/h/game" || p == "/h/game PAL-ES" ? &usa : nullptr;
    });
    CHECK(k.kept == &usa && k.gameDir == fs::path("/h/game PAL-ES 2"), "past another USA install: %s", k.gameDir.c_str());

    // The line the screen and the command line show.
    k = KeepOtherRegion("/h/game", *pal, [&](const fs::path& p) -> const Manifest* { return p == "/h/game" ? &usa : nullptr; });
    const std::string note = RegionKeepNote(k, *pal, "~/game", "~/game PAL-ES");
    CHECK(note == "~/game holds the USA install. This Europe (PAL, Spanish) disc goes in its own folder, ~/game PAL-ES, so that "
                  "install stays.", "note: %s", note.c_str());
    CHECK(RegionKeepNote(KeepOtherRegion("/h/game", usa, [&](const fs::path&) -> const Manifest* { return &usa; }), usa, "a", "b").empty(),
        "no note for a same-region install");

    // CheckDestination of the folder beside: a fresh install, nothing replaced;
    // of the folder asked for: the replace KeepOtherRegion prevents.
    DestinationInfo info;
    fs::remove_all(palDir);
    CheckDestination(palDir, &info, *pal);
    CHECK(!info.replacing, "beside: replaces nothing");
    CheckDestination(game, &info, usa);
    CHECK(info.replacing, "the PAL folder asked for by a USA disc would be replaced");
}

int main(int argc, char** argv)
{
    Manifests();
    fs::path base = argc > 1 ? fs::path(argv[1]) : fs::current_path();
    fs::create_directories(base);
    std::string tmpl = (base / "region-manifest-XXXXXX").string();
    if (!mkdtemp(tmpl.data()))
    {
        printf("FAIL: cannot make a scratch folder in %s\n", base.c_str());
        return 1;
    }
    fs::path root = fs::absolute(tmpl);
    Installs(root, argv[0]);
    KeepRegions(root);
    chdir(base.c_str());
    std::error_code ec;
    fs::remove_all(root, ec);
    if (g_failures)
    {
        printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("region_manifest_test: all checks passed\n");
    return 0;
}
