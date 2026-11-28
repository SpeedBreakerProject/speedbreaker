// Tests for the installer's regions (install/manifest.h): the USA and the
// European (PAL, Spanish) manifests, told apart by default.xex's hash and
// size and by the version an install's marker records, and FindGameInstall
// finding either region's install (with which it is) in a scratch folder with
// a sandbox HOME; and a file that isn't any of those discs, listed by the
// scan with why and what the installer needs when it finds nothing to install
// (#108), and left out beside an image that installs. Built with SB_PAL_DE=1
// (region_manifest_test_pal_de), the German disc's manifest is a third
// region; without it, that disc is refused up front (ReleaseWithoutCode).
// From the repo root:
//   clang++ -std=c++20 -O2 -Iruntime tests/region_manifest_test.cpp runtime/install/{placement,installer,locate,manifest,result,disc_source,sha256}.cpp runtime/user/portable.cpp -o build/region_manifest_test
//   (on macOS add -framework CoreFoundation)
//   build/region_manifest_test [folder]
//   (add -DSB_PAL_DE=1 for the German region)
// No game files: the stand-in installs are a default.xex of the right size
// (all zeros) and a marker; the stand-in disc images are sparse files.
#include <install/installer.h>
#include <install/locate.h>
#include <install/placement.h>
#include <install/sha256.h>
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

#ifndef SB_PAL_DE
#define SB_PAL_DE 0
#endif
#ifndef SB_PAL_UK
#define SB_PAL_UK 0
#endif

static constexpr std::string_view kGermanXex = "23ece29d18724ea28971d52f936964d127148bd2359ff24795da7442b9069bf4";
static constexpr std::string_view kUkXex = "e2ea2ff11bf84cb30430d824d7bfe699a3acf3e7400d21b9a7c938e3d7a10474";

static void Manifests()
{
    auto all = GameManifests();
    CHECK(all.size() == 2u + SB_PAL_DE + SB_PAL_UK, "regions: %zu", all.size());
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
    CHECK(ReleaseWithoutCode(usa.Xex().sha256).regionName.empty() && ReleaseWithoutCode(pal->Xex().sha256).regionName.empty()
        && ReleaseWithoutCode("00").regionName.empty(), "ReleaseWithoutCode: none for a built-in or unknown hash");

    // The German disc: a region with its code linked, refused up front without it.
    const Manifest* de = ManifestForXexHash(kGermanXex);
    const ReleaseWithoutCodeInfo without = ReleaseWithoutCode(kGermanXex);
#if SB_PAL_DE
    CHECK(de && de == all[2] && de->region == "pal-de" && de->regionName == "Europe (PAL, German)", "the German manifest, last");
    CHECK(without.regionName.empty(), "with its code: not refused");
    if (de)
    {
        CHECK(de->titleId == usa.titleId && de->files.size() == 49, "its title and 49 files");
        CHECK(ManifestForXexSize(3944448) == de && ManifestForVersion(de->version) == de, "by size and version");
        size_t german = 0;
        for (const ManifestFile& f : de->files)
            german += f.path.ends_with("_german_pal.wmv");
        CHECK(german == 33, "German movies: %zu", german);
    }
#else
    CHECK(!de && !ManifestForXexSize(3944448), "without its code: no German manifest");
    CHECK(without.regionName == "Europe (PAL, German)" && without.discName == "German (PAL)" && without.codeFolder == "ppc-pal-de",
        "refused, naming the disc and its code");
#endif

    // The UK disc: English PAL, its default.xex the Spanish one's size.
    const Manifest* uk = ManifestForXexHash(kUkXex);
    const ReleaseWithoutCodeInfo ukWithout = ReleaseWithoutCode(kUkXex);
#if SB_PAL_UK
    CHECK(uk && uk == all.back() && uk->region == "pal-uk" && uk->regionName == "Europe (PAL, English)", "the UK manifest, last");
    CHECK(ukWithout.regionName.empty(), "with its code: not refused");
    if (uk)
    {
        CHECK(uk->titleId == usa.titleId && uk->files.size() == 52 && uk->totalBytes == 7169042728ull, "its title, 52 files");
        CHECK(uk->Xex().size == pal->Xex().size && ManifestForXexSize(3940352) == pal, "the Spanish size: by size, the Spanish one");
        CHECK(ManifestForVersion(uk->version) == uk && uk->version != pal->version, "by version");
        size_t english = 0;
        for (const ManifestFile& f : uk->files)
            english += f.path.ends_with("_english_pal.wmv");
        CHECK(english == 36, "English PAL movies: %zu", english);
    }
#else
    CHECK(!uk, "without its code: no UK manifest");
    CHECK(ukWithout.regionName == "Europe (PAL, English)" && ukWithout.discName == "UK (PAL, English)" && ukWithout.codeFolder == "ppc-pal-uk",
        "refused, naming the disc and its code");
#endif
}

// ManifestForXexFile: the Spanish and UK default.xex share a size, so a file
// of it is told apart by its hash; one that is neither's is the Spanish one,
// as by size before.
static void XexFile(const fs::path& root)
{
    const Manifest* pal = ManifestForVersion("2e42eb3950fcc436");
    if (!pal)
        return;
    fs::path f = root / "xexfile" / "default.xex";
    fs::create_directories(f.parent_path());
    {
        std::ofstream(f, std::ios::binary) << "x";
    }
    fs::resize_file(f, pal->Xex().size);
    CHECK(ManifestForXexFile(f) == pal, "a stand-in of the Spanish size: the Spanish manifest");
    fs::resize_file(f, GameManifest().Xex().size);
    CHECK(ManifestForXexFile(f) == &GameManifest(), "of the USA size: the USA one (not hashed)");
    CHECK(ManifestForXexFile(root / "missing.xex") == nullptr, "no file: none");
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

// The German disc's installs (a build with its code).
static void GermanInstalls(const fs::path& root)
{
#if SB_PAL_DE
    const Manifest& usa = GameManifest();
    const Manifest* de = ManifestForXexHash(kGermanXex);
    if (!de)
        return;
    RecordInstallPath(DefaultInstallPath());  // the earlier recorded install: gone
    fs::path user = DefaultInstallPath();
    StandIn(user, de->Xex().size, std::string(de->version).c_str());
    std::optional<GameInstall> found = FindGameInstall();
    CHECK(found && found->manifest == de && found->marker, "a German install: found, German");
    CHECK(InstalledRegion(user) == de, "InstalledRegion: German");
    CHECK(RegionFolder("/h/game", *de) == fs::path("/h/game PAL-DE"), "RegionFolder German: %s", RegionFolder("/h/game", *de).c_str());
    fs::path dir = root / "keep-de", game = dir / "game";
    StandIn(game, usa.Xex().size, "535fd83157066102");
    RegionKeep k = KeepOtherRegion(game, *de);
    CHECK(k.kept == &usa && k.gameDir == dir / "game PAL-DE", "German beside USA: %s", k.gameDir.c_str());
#else
    (void)root;
#endif
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

// A sparse stand-in of an Xbox 360 game's image: an XDVDFS volume from the
// file's first byte (its descriptor in sector 32, the root directory in
// sector 33) whose one file, in sector 34, is a default.xex holding `xex`.
static void ImageStandIn(const fs::path& file, const std::string& xex, uint64_t size)
{
    auto le32 = [](uint32_t v) { return std::string{ char(v), char(v >> 8), char(v >> 16), char(v >> 24) }; };
    const std::string name = "default.xex";
    const std::string descriptor = "MICROSOFT*XBOX*MEDIA" + le32(33) + le32(2048);
    // No entry to its left or right, the data's sector and size, a plain
    // file, the name; 0xFF to the sector's end.
    std::string directory = std::string(4, '\0') + le32(34) + le32(uint32_t(xex.size())) + char(0x80) + char(name.size()) + name;
    directory.resize(2048, '\xFF');
    fs::create_directories(file.parent_path());
    {
        std::ofstream f(file, std::ios::binary);
        f.seekp(32 * 2048);
        f << descriptor;
        f.seekp(33 * 2048);
        f << directory;
        f.seekp(34 * 2048);
        f << xex;
    }
    fs::resize_file(file, size);
}

// A file that isn't the disc (GitHub issue #108: a 3.11 GB image of another
// version of the game, which Rescan left out without a word): a scan that
// finds nothing to install lists it, not installable, and its reason, like a
// picked file's, ends with what the installer needs instead. Beside an image
// that installs it is left out. Sparse stand-ins: zeros, and a DVD-video
// image's ISO 9660 mark.
static void NotTheDisc(const fs::path& root)
{
    const std::string needed = NeededDisc();
    const char* expected = SB_PAL_DE && SB_PAL_UK
        ? "SpeedBreaker needs a copy of the Xbox 360 disc of Need for Speed: Most Wanted, the USA, the European "
          "(PAL, Spanish), the European (PAL, German) or the European (PAL, English) one."
        : SB_PAL_DE ? "SpeedBreaker needs a copy of the Xbox 360 disc of Need for Speed: Most Wanted, the USA, the European "
                      "(PAL, Spanish) or the European (PAL, German) one."
        : SB_PAL_UK ? "SpeedBreaker needs a copy of the Xbox 360 disc of Need for Speed: Most Wanted, the USA, the European "
                      "(PAL, Spanish) or the European (PAL, English) one."
                    : "SpeedBreaker needs a copy of the Xbox 360 disc of Need for Speed: Most Wanted, the USA or the European "
                      "(PAL, Spanish) one.";
    CHECK(needed == expected, "what it needs: %s", needed.c_str());

    const fs::path dir = root / "not-the-disc";
    const fs::path zeros = dir / "Need for Speed - Most Wanted (fake).iso";
    Write(zeros, "");
    fs::resize_file(zeros, 3110ull << 20);
    const fs::path dvd = dir / "video.iso";
    Write(dvd, std::string(0x8001, '\0') + "CD001");
    fs::resize_file(dvd, 300ull << 20);
    Write(dir / "small.iso", std::string(1000, '\0'));  // under minSize: left out, as before
    Write(dir / "notes.txt", std::string(2 << 20, 'x'));  // not an .iso: left out, as before

    const std::vector<DiscImageCandidate> found = FindDiscImages({ dir }, 1 << 20);
    CHECK(found.size() == 2, "listed: %zu", found.size());
    if (found.size() == 2)
    {
        const DiscImageCandidate& a = found[0];
        CHECK(a.path == zeros && a.size == (3110ull << 20) && a.status.error == Error::NotADiscImage && !a.xexMatches,
            "zeros: %s, %s", a.path.c_str(), ErrorName(a.status.error));
        CHECK(a.status.message == "Need for Speed - Most Wanted (fake).iso is not an Xbox 360 disc image. " + needed, "zeros: %s",
            a.status.message.c_str());
        const DiscImageCandidate& b = found[1];
        CHECK(b.path == dvd && b.status.error == Error::UnsupportedFormat, "video: %s, %s", b.path.c_str(), ErrorName(b.status.error));
        CHECK(b.status.message.starts_with("video.iso is a DVD image without an Xbox 360 game partition") &&
                b.status.message.ends_with(". Dump the whole disc. " + needed),
            "video: %s", b.status.message.c_str());
    }

    // The image of an Xbox 360 game that isn't this one (a default.xex, not
    // the manifest's) was listed before those were, and is: nothing here
    // installs, so all three show.
    const std::string xex = "stand-in default", otherXex = "another default.";
    const std::string hash = ToHex(Sha256Of(xex.data(), xex.size()));
    const ManifestFile files[] = { { "default.xex", xex.size(), hash } };
    const Manifest test{ "test-1", 0x454107D9, files, xex.size() };
    const fs::path other = dir / "another game.iso";
    ImageStandIn(other, otherXex, 2 << 20);
    const std::vector<DiscImageCandidate> none = FindDiscImages({ dir }, 1 << 20, nullptr, test);
    CHECK(none.size() == 3, "nothing to install: %zu listed", none.size());
    if (none.size() == 3)
    {
        CHECK(none[0].path == zeros && none[0].status.error == Error::NotADiscImage, "zeros: %s", ErrorName(none[0].status.error));
        CHECK(none[1].path == other && none[1].status.error == Error::WrongGame && !none[1].xexMatches && none[1].status.message.ends_with(needed),
            "another game: %s, %s", ErrorName(none[1].status.error), none[1].status.message.c_str());
        CHECK(none[2].path == dvd && none[2].status.error == Error::UnsupportedFormat, "video: %s", ErrorName(none[2].status.error));
    }

    // Beside an image that installs (the stand-in whose default.xex is the
    // test manifest's), the files that aren't an Xbox 360 game's image are
    // left out, as they were before the scan listed them; the other game's
    // image stays, under it.
    const fs::path disc = dir / "the disc.iso";
    ImageStandIn(disc, xex, 2 << 20);
    const std::vector<DiscImageCandidate> ready = FindDiscImages({ dir }, 1 << 20, nullptr, test);
    CHECK(ready.size() == 2, "one to install: %zu listed", ready.size());
    if (ready.size() == 2)
    {
        CHECK(ready[0].path == disc && ready[0].status.Ok() && ready[0].xexMatches && ready[0].size == (2 << 20), "the disc first: %s, %s",
            ready[0].path.c_str(), ErrorName(ready[0].status.error));
        CHECK(ready[1].path == other && ready[1].status.error == Error::WrongGame, "another game under it: %s, %s", ready[1].path.c_str(),
            ErrorName(ready[1].status.error));
    }
    for (const DiscImageCandidate& c : ready)
        CHECK(c.path != zeros && c.path != dvd, "left out beside the disc: %s", c.path.c_str());
    // With the game's own manifests the stand-in is one more game that isn't
    // this one: nothing installs, and all four show.
    const std::vector<DiscImageCandidate> all = FindDiscImages({ dir }, 1 << 20);
    CHECK(all.size() == 4 && all[0].path == zeros && all[1].path == other && all[2].path == disc && all[3].path == dvd,
        "the game's manifests: %zu listed", all.size());
    for (const DiscImageCandidate& c : all)
        CHECK(!c.status.Ok() && c.status.message.ends_with(needed), "%s: %s", c.path.c_str(), c.status.message.c_str());

    // Picked (Browse for image..., or a typed path): the same reason.
    std::unique_ptr<DiscSource> source;
    const Result picked = OpenDiscSource(zeros, source);
    CHECK(!source && picked.error == Error::NotADiscImage && picked.message.ends_with(needed), "picked: %s", picked.message.c_str());
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
    XexFile(root);
    Installs(root, argv[0]);
    KeepRegions(root);
    GermanInstalls(root);
    NotTheDisc(root);
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
