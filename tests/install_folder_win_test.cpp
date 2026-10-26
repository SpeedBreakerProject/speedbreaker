// Windows: "Choose a folder..." in the installer (install/placement.h) on a
// real disk (install_folder_test is POSIX only: mkdtemp, chmod, links). In a
// new folder made inside the given one (only that is deleted afterwards):
//   - a new, an empty and a non-empty folder (SpeedBreaker Game inside it);
//   - the system drive's root, C:\: its top, so <root>\SpeedBreaker\game, not
//     "the system's root folder" (nothing is made there: probing only);
//   - a folder in OneDrive (OneDrive= pointed at a scratch folder) refused;
//   - the system's own folders refused as not writable (C:\Windows, which
//     CheckWritableFolder probes by making a folder: denied without admin);
//   - the program's own folder refused.
// From the build: tests\install_folder_win_test.exe <folder>
#include <install/installer.h>
#include <install/manifest.h>
#include <install/placement.h>
#include <user/paths.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

namespace fs = std::filesystem;
using namespace install;

static bool Has(const std::string& text, const char* part)
{
    return text.find(part) != std::string::npos;
}

static void Show(const char* what, const FolderChoice& c)
{
    printf("[%s] picked %s -> %s: %s%s\n", what, c.picked.string().c_str(), c.gameDir.string().c_str(),
        c.result.Ok() ? "ok" : "refused: ", c.result.message.c_str());
}

int main(int argc, char** argv)
{
    std::error_code ec;
    SetProgramPath(argv[0]);
    fs::path base = argc > 1 ? fs::path(argv[1]) : fs::current_path();
    fs::create_directories(base, ec);
    fs::path root = fs::weakly_canonical(base / ("install_folder_win_test." + std::to_string(std::rand())), ec);
    fs::remove_all(root, ec);
    if (!fs::create_directories(root, ec))
    {
        printf("can't make a folder in %s: %s\n", base.string().c_str(), ec.message().c_str());
        return 2;
    }
    // Free space and the manifest's sizes are CheckDestination's, as in a
    // real install: a manifest of one small file keeps the space check from
    // deciding these.
    static const ManifestFile files[] = { { "default.xex", 16, "00" } };
    const Manifest manifest{ "test-1", 0x454107D9, files, 16 };

    // New, empty, not empty.
    FolderChoice c = CheckPickedFolder(root / "new", manifest);
    Show("new", c);
    CHECK(c.result.error == Error::NotFound, "a folder that isn't there: %s", c.result.message.c_str());
    fs::create_directories(root / "empty", ec);
    c = CheckPickedFolder(root / "empty", manifest);
    Show("empty", c);
    CHECK(c.result.Ok() && c.gameDir == fs::weakly_canonical(root / "empty") && c.subfolder == SubfolderReason::None,
        "empty: as is: %s", c.result.message.c_str());
    fs::create_directories(root / "full", ec);
    std::ofstream(root / "full" / "notes.txt") << "mine";
    c = CheckPickedFolder(root / "full", manifest);
    Show("full", c);
    CHECK(c.result.Ok() && c.gameDir.filename() == kPickedSubfolder && c.subfolder == SubfolderReason::NotEmpty,
        "not empty: the subfolder: %s", c.gameDir.string().c_str());

    // The system drive's root: its top. Probing only (nothing made there).
    const fs::path drive = fs::path(std::getenv("SystemDrive") ? std::getenv("SystemDrive") : "C:") / "\\";
    FolderFacts facts = ProbeFolder(drive);
    CHECK(facts.driveTop && facts.state == FolderState::Other, "C:\\ is a drive's top: driveTop %d state %d", facts.driveTop,
        int(facts.state));
    c = PlaceInPicked(facts, CurrentPlacementContext(manifest));
    Show("drive root", c);
    CHECK(c.result.Ok() && c.gameDir == facts.path / kUserFolderName / "game" && c.subfolder == SubfolderReason::DriveTop,
        "C:\\ -> C:\\SpeedBreaker\\game: %s %s", c.gameDir.string().c_str(), c.result.message.c_str());
    CHECK(Has(SubfolderExplanation(c), "top of the drive"), "%s", SubfolderExplanation(c).c_str());

    // OneDrive: a scratch folder stands in for it.
    fs::create_directories(root / "od" / "Games", ec);
    _putenv_s("OneDrive", (root / "od").string().c_str());
    c = CheckPickedFolder(root / "od" / "Games", manifest);
    Show("onedrive", c);
    CHECK(c.result.error == Error::DestinationNotWritable && Has(c.result.message, "is in OneDrive"), "OneDrive: %s",
        c.result.message.c_str());
    _putenv_s("OneDrive", "");
    c = CheckPickedFolder(root / "od" / "Games", manifest);
    CHECK(c.result.Ok(), "the same folder, OneDrive elsewhere: %s", c.result.message.c_str());

    // A folder this user can't write (CheckWritableFolder makes and removes
    // a folder to tell): C:\Windows, or nothing to test as an administrator.
    const fs::path windows = std::getenv("SystemRoot") ? fs::path(std::getenv("SystemRoot")) : fs::path("C:\\Windows");
    facts = ProbeFolder(windows);
    printf("[windows folder] %s: writable %d, parent writable %d\n", windows.string().c_str(), facts.writable, facts.parentWritable);
    if (!facts.writable)
    {
        c = CheckPickedFolder(windows, manifest);
        Show("windows folder", c);
        CHECK(!c.result.Ok(), "C:\\Windows refused");
    }
    CHECK(ProbeFolder(root / "empty").writable, "our own folder is writable");

    // The program's own folder.
    c = CheckPickedFolder(BundleFolder(), manifest);
    Show("own folder", c);
    CHECK(!c.result.Ok() && Has(c.result.message, "SpeedBreaker's own folder"), "the program's folder: %s",
        c.result.message.c_str());

    fs::remove_all(root, ec);
    printf("%s (%d failures)\n", g_failures ? "FAILED" : "passed", g_failures);
    return g_failures ? 1 : 0;
}
