// Tests for install/placement.h: where "Choose a folder..." puts the game in a
// picked folder, what it refuses, the notes it adds, and what the installer
// says about a recorded install that isn't usable. First the pure rules
// (PlaceInPicked, PlaceExact, DescribeRecordedInstall) on planted facts, then
// the same through the disk (ProbeFolder, CheckPickedFolder, FindGameInstall)
// in a scratch folder with a sandbox HOME. From the repo root:
//   clang++ -std=c++20 -O2 -Iruntime tests/install_folder_test.cpp runtime/install/{placement,installer,locate,manifest,result,disc_source,sha256}.cpp -o build/install_folder_test
//   (on macOS add -framework CoreFoundation)
//   build/install_folder_test [folder]
// The disk checks run in a new folder made inside [folder] (default: the
// current folder; not /tmp, which is in memory on SteamOS and refused), and
// only that one is deleted afterwards.
// INSTALL_FOLDER_TEST_ICLOUD=<a folder in iCloud Drive> (macOS) also checks
// that ProbeFolder sees it as such.
#include <install/installer.h>
#include <install/locate.h>
#include <install/placement.h>
#include <user/paths.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

namespace fs = std::filesystem;
using namespace install;

static bool Has(const std::string& text, const char* part)
{
    return text.find(part) != std::string::npos;
}

static FolderFacts Facts(const char* path, FolderState state, FolderState subfolder = FolderState::Missing)
{
    FolderFacts f;
    f.path = path;
    f.state = state;
    f.subfolder.state = subfolder;
    f.fileSystem = "ext4";
    return f;
}

static PlacementContext Context(bool macOS = false)
{
    PlacementContext c;
    c.home = macOS ? "/Users/p" : "/home/p";
    c.bundle = macOS ? "/Applications/SpeedBreaker.app" : "/home/p/Games/SpeedBreaker";
    c.installs = { macOS ? "/Users/p/Library/Application Support/SpeedBreaker/game" : "/home/p/.local/share/speedbreaker/game",
        "/run/media/p/SD/speedbreaker/game" };
    c.largestFile = 653262848;  // the real manifest's NFS/ZZDATA7.BIN
    c.macOS = macOS;
    return c;
}

static void PureRules()
{
    const PlacementContext onLinux = Context(false), mac = Context(true);
    const std::string sub = std::string("/") + kPickedSubfolder;

    // An empty or new folder is used as it is; any other gets the subfolder.
    FolderChoice c = PlaceInPicked(Facts("/home/p/Games2", FolderState::Empty), onLinux);
    CHECK(c.result.Ok() && c.gameDir == "/home/p/Games2" && c.subfolder == SubfolderReason::None, "empty: as is: %s %s",
        c.gameDir.c_str(), c.result.message.c_str());
    CHECK(SubfolderExplanation(c).empty(), "nothing to explain");
    c = PlaceInPicked(Facts("/home/p/Games", FolderState::Other), onLinux);
    CHECK(c.result.Ok() && c.gameDir == "/home/p/Games" + sub && c.subfolder == SubfolderReason::NotEmpty,
        "not empty: subfolder: %s", c.gameDir.c_str());
    CHECK(Has(SubfolderExplanation(c), "Games already holds other files"), "%s", SubfolderExplanation(c).c_str());
    CHECK(c.picked == "/home/p/Games", "picked kept: %s", c.picked.c_str());
    c = PlaceInPicked(Facts("/home/p/Games/", FolderState::Other), onLinux);
    CHECK(c.gameDir == "/home/p/Games" + sub, "a trailing slash: %s", c.gameDir.c_str());
    // The subfolder must be new, empty or a previous install.
    for (FolderState ok : { FolderState::Missing, FolderState::Empty, FolderState::Install })
    {
        c = PlaceInPicked(Facts("/home/p/Games", FolderState::Other, ok), onLinux);
        CHECK(c.result.Ok(), "subfolder state %d is fine: %s", int(ok), c.result.message.c_str());
    }
    c = PlaceInPicked(Facts("/home/p/Games", FolderState::Other, FolderState::Other), onLinux);
    CHECK(c.result.error == Error::DestinationNotEmpty && Has(c.result.message, kPickedSubfolder), "subfolder with other files: %s",
        c.result.message.c_str());
    c = PlaceInPicked(Facts("/home/p/Games", FolderState::Other, FolderState::NotAFolder), onLinux);
    CHECK(c.result.error == Error::DestinationNotEmpty && Has(c.result.message, "is a file"), "a file named like the subfolder");
    c = PlaceInPicked(Facts("/home/p/Games", FolderState::Other, FolderState::Unreadable), onLinux);
    CHECK(c.result.error == Error::DestinationNotWritable, "an unreadable subfolder");
    // A previous install is replaced where it is.
    c = PlaceInPicked(Facts("/home/p/old", FolderState::Install), onLinux);
    CHECK(c.result.Ok() && c.gameDir == "/home/p/old" && c.subfolder == SubfolderReason::None, "previous install: as is");

    // Empty, but not usable as it is. The top of a drive gets the drive's
    // own install folder, the one its entry on the To row uses (InstallDestinations).
    const fs::path drive = fs::path(kUserFolderName) / "game";
    CHECK(SubfolderName(true) == drive && SubfolderName(false) == kPickedSubfolder, "subfolder names: %s, %s",
        SubfolderName(true).c_str(), SubfolderName(false).c_str());
    FolderFacts f = Facts("/run/media/p/USB", FolderState::Empty);
    f.driveTop = true;
    c = PlaceInPicked(f, onLinux);
    CHECK(c.result.Ok() && c.gameDir == "/run/media/p/USB" / drive && c.subfolder == SubfolderReason::DriveTop, "drive top: %s",
        c.gameDir.c_str());
    CHECK(Has(SubfolderExplanation(c), "top of the drive USB"), "%s", SubfolderExplanation(c).c_str());
    f.state = FolderState::Install;  // even holding an install: a drive's top can't be renamed
    CHECK(PlaceInPicked(f, onLinux).gameDir == "/run/media/p/USB" / drive, "drive top with an install");
    f.state = FolderState::Other;
    f.subfolder.state = FolderState::Other;
    c = PlaceInPicked(f, onLinux);
    CHECK(c.result.error == Error::DestinationNotEmpty && Has(c.result.message, drive.c_str()), "the drive's folder taken: %s",
        c.result.message.c_str());
    f.subfolder.state = FolderState::Install;
    CHECK(PlaceInPicked(f, onLinux).result.Ok(), "the drive's folder holding an install: replaced");
    f = Facts("/home/p", FolderState::Empty);
    c = PlaceInPicked(f, onLinux);
    CHECK(c.result.Ok() && c.gameDir == "/home/p" + sub && c.subfolder == SubfolderReason::Home, "home, even empty: %s",
        c.gameDir.c_str());
    c = PlaceInPicked(Facts("/home/p", FolderState::Other), onLinux);
    CHECK(c.gameDir == "/home/p" + sub && c.subfolder == SubfolderReason::Home, "home: subfolder");
    f = Facts("/mnt/data/game", FolderState::Empty);
    f.parentWritable = false;
    c = PlaceInPicked(f, onLinux);
    CHECK(c.result.Ok() && c.gameDir == "/mnt/data/game" + sub && c.subfolder == SubfolderReason::ParentReadOnly,
        "parent read-only: subfolder: %s", c.gameDir.c_str());
    CHECK(Has(SubfolderExplanation(c), "can't write next to game"), "%s", SubfolderExplanation(c).c_str());
    // Hidden files only, and macOS won't let it be moved aside ("deny
    // delete" in its ACL: fresh ~/Music, ~/Pictures): Install would fail
    // after the copy, so a subfolder. Truly empty, it is replaced (that works).
    f = Facts("/Users/p/Music", FolderState::Empty);
    f.hidden = true;
    f.movable = false;
    c = PlaceInPicked(f, Context(true));
    CHECK(c.result.Ok() && c.gameDir == "/Users/p/Music" + sub && c.subfolder == SubfolderReason::KeptInPlace,
        "hidden files, can't be moved: subfolder: %s %s", c.gameDir.c_str(), c.result.message.c_str());
    CHECK(Has(SubfolderExplanation(c), "macOS keeps Music in place"), "%s", SubfolderExplanation(c).c_str());
    f.movable = true;
    CHECK(PlaceInPicked(f, Context(true)).gameDir == "/Users/p/Music", "hidden files, movable: as is");
    f.hidden = false;
    f.movable = false;
    CHECK(PlaceInPicked(f, Context(true)).gameDir == "/Users/p/Music", "truly empty, deny delete: as is (replaced)");
    f.hidden = true;
    c = PlaceExact(f, true, Context(true));
    CHECK(c.result.error == Error::DestinationNotWritable && Has(c.result.message, "keeps /Users/p/Music in place"),
        "exact (--dest), hidden files, can't be moved: %s", c.result.message.c_str());

    // A folder that is there but can't be written: Install replaces it or
    // moves it aside, which macOS refuses after the whole copy.
    f = Facts("/home/p/ro", FolderState::Empty);
    f.writable = false;
    c = PlaceInPicked(f, onLinux);
    CHECK(c.result.error == Error::DestinationNotWritable && Has(c.result.message, "can't write to /home/p/ro."),
        "empty, not writable: %s", c.result.message.c_str());
    f.state = FolderState::Install;
    CHECK(PlaceInPicked(f, onLinux).result.error == Error::DestinationNotWritable, "a previous install, not writable");
    CHECK(PlaceExact(f, true, onLinux).result.error == Error::DestinationNotWritable, "exact, not writable");
    f.state = FolderState::Other;  // the subfolder is made in it: CheckDestination's to refuse
    CHECK(PlaceInPicked(f, onLinux).result.Ok(), "not empty, not writable: placement leaves it to CheckDestination");
    f.writable = true;
    f.subfolder.state = FolderState::Empty;
    f.subfolder.writable = false;
    c = PlaceInPicked(f, onLinux);
    CHECK(c.result.error == Error::DestinationNotWritable && Has(c.result.message, kPickedSubfolder), "the subfolder, not "
        "writable: %s", c.result.message.c_str());
    f.subfolder.state = FolderState::Missing;
    CHECK(PlaceInPicked(f, onLinux).result.Ok(), "a missing subfolder's writability doesn't count");

    // A link where the subfolder goes would take the game elsewhere.
    f = Facts("/home/p/Games", FolderState::Other, FolderState::Empty);
    f.subfolder.link = true;
    f.subfolder.target = "/home/p/Games/SpeedBreaker/inside";
    c = PlaceInPicked(f, onLinux);
    CHECK(c.result.error == Error::DestinationNotEmpty && Has(c.result.message, "is a link to /home/p/Games/SpeedBreaker/inside"),
        "a link as the subfolder: %s", c.result.message.c_str());
    // One that leads nowhere says where it points, not that it's a link to itself.
    f = Facts("/home/p/Games", FolderState::Other, FolderState::Missing);
    f.subfolder.link = true;
    f.subfolder.dangling = true;
    f.subfolder.target = "/home/p/gone";
    c = PlaceInPicked(f, onLinux);
    CHECK(c.result.error == Error::DestinationNotEmpty && Has(c.result.message, "is a link to /home/p/gone, which isn't there")
        && !Has(c.result.message, "Choose that folder itself"), "a link leading nowhere: %s", c.result.message.c_str());
    f.subfolder.target.clear();
    c = PlaceInPicked(f, onLinux);
    CHECK(c.result.error == Error::DestinationNotEmpty && Has(c.result.message, "a link that leads nowhere"),
        "a link leading nowhere, target unknown: %s", c.result.message.c_str());
    // One to a file: not "choose that folder".
    f = Facts("/home/p/Games", FolderState::Other, FolderState::NotAFolder);
    f.subfolder.link = true;
    f.subfolder.target = "/home/p/notes.txt";
    c = PlaceInPicked(f, onLinux);
    CHECK(c.result.error == Error::DestinationNotEmpty && Has(c.result.message, "is a link to a file, /home/p/notes.txt")
        && !Has(c.result.message, "Choose that folder itself"), "a link to a file: %s", c.result.message.c_str());

    // What the picker can return but can't be used.
    c = PlaceInPicked(Facts("/home/p/gone", FolderState::Missing), onLinux);
    CHECK(c.result.error == Error::NotFound, "missing");
    c = PlaceInPicked(Facts("/home/p/file.txt", FolderState::NotAFolder), onLinux);
    CHECK(c.result.error == Error::DestinationNotEmpty, "a file");
    f = Facts("/Users/p/Documents/x", FolderState::Unreadable);
    f.error = EPERM;
    c = PlaceInPicked(f, mac);
    CHECK(c.result.error == Error::DestinationNotWritable && Has(c.result.message, "Privacy & Security"), "macOS EPERM: %s",
        c.result.message.c_str());
    f.error = EACCES;
    c = PlaceInPicked(f, onLinux);
    CHECK(c.result.error == Error::DestinationNotWritable && Has(c.result.message, "can't read"), "EACCES: %s", c.result.message.c_str());

    // Never: the system's root, SpeedBreaker's own folder or inside it, inside an install.
    f = Facts("/", FolderState::Other);
    f.driveTop = true;
    c = PlaceInPicked(f, onLinux);
    CHECK(c.result.error == Error::DestinationNotWritable && Has(c.result.message, "root folder"), "/: %s", c.result.message.c_str());
    c = PlaceInPicked(Facts("/home/p/Games/SpeedBreaker", FolderState::Other), onLinux);
    CHECK(c.result.error == Error::DestinationNotWritable && Has(c.result.message, "/home/p/Games/SpeedBreaker is SpeedBreaker's own folder")
        && !Has(c.result.message, kPickedSubfolder), "the bundle (named as picked, not its subfolder): %s", c.result.message.c_str());
    c = PlaceInPicked(Facts("/home/p/Games/SpeedBreaker/lib/x", FolderState::Empty), onLinux);
    CHECK(c.result.error == Error::DestinationNotWritable && Has(c.result.message, "inside SpeedBreaker's own folder, /home/p/Games/SpeedBreaker."),
        "inside the bundle: %s", c.result.message.c_str());
    c = PlaceInPicked(Facts("/home/p/Games/SpeedBreakerX", FolderState::Empty), onLinux);
    CHECK(c.result.Ok(), "a sibling whose name starts like the bundle's: %s", c.result.message.c_str());
    c = PlaceInPicked(Facts("/Applications/SpeedBreaker.app/Contents", FolderState::Other), mac);
    CHECK(c.result.error == Error::DestinationNotWritable && Has(c.result.message, "inside the SpeedBreaker app"), "the Mac app: %s",
        c.result.message.c_str());
    c = PlaceInPicked(Facts("/home/p/.local/share/speedbreaker/game/NFS", FolderState::Other), onLinux);
    CHECK(c.result.error == Error::DestinationNotWritable && Has(c.result.message, "/game/NFS is inside the game installed in")
        && !Has(c.result.message, kPickedSubfolder), "inside the default install: %s", c.result.message.c_str());
    c = PlaceInPicked(Facts("/run/media/p/SD/speedbreaker/game/Movies", FolderState::Empty), onLinux);
    CHECK(c.result.error == Error::DestinationNotWritable, "inside the recorded install");
    // The install folder itself, holding other files (the game would go in a
    // new folder inside it): not "inside" itself.
    c = PlaceInPicked(Facts("/home/p/.local/share/speedbreaker/game", FolderState::Other), onLinux);
    CHECK(c.result.error == Error::DestinationNotWritable
        && Has(c.result.message, "/home/p/.local/share/speedbreaker/game is the game's own install folder")
        && !Has(c.result.message, "inside the game installed"), "the install folder itself, not an install: %s",
        c.result.message.c_str());
    c = PlaceInPicked(Facts("/home/p/.local/share/speedbreaker/game", FolderState::Install), onLinux);
    CHECK(c.result.Ok() && c.gameDir == "/home/p/.local/share/speedbreaker/game", "the install itself: replaced: %s", c.result.message.c_str());

    // Drives formatted for Windows: fine, with the right word about them.
    f = Facts("/run/media/p/CARD", FolderState::Other);
    f.fileSystem = "vfat";
    c = PlaceInPicked(f, onLinux);
    CHECK(c.result.Ok() && c.notes.size() == 1 && Has(c.notes[0], "FAT32") && Has(c.notes[0], "only reads"), "FAT32: %s",
        c.notes.empty() ? "no note" : c.notes[0].c_str());
    PlacementContext big = onLinux;
    big.largestFile = 5000000000ull;
    c = PlaceInPicked(f, big);
    CHECK(c.result.error == Error::DestinationNotWritable && Has(c.result.message, "4 GB"), "FAT32 and a 5 GB file: %s",
        c.result.message.c_str());
    big.largestFile = (1ull << 32) - 1;
    CHECK(PlaceInPicked(f, big).result.Ok(), "FAT32 holds 4 GiB - 1 byte");
    big.largestFile = 1ull << 32;
    CHECK(PlaceInPicked(f, big).result.error == Error::DestinationNotWritable, "but not 4 GiB");
    PlacementContext steamOS = onLinux;
    steamOS.steamOS = true;
    f.fileSystem = "exfat";
    c = PlaceInPicked(f, steamOS);
    CHECK(c.result.Ok() && c.notes.size() == 1 && Has(c.notes[0], "exFAT") && Has(c.notes[0], "Game Mode"), "exFAT on SteamOS: %s",
        c.notes.empty() ? "no note" : c.notes[0].c_str());
    f.fileSystem = "fuseblk";
    c = PlaceInPicked(f, onLinux);
    CHECK(c.result.Ok() && c.notes.size() == 1 && Has(c.notes[0], "NTFS"), "ntfs-3g");
    f.fileSystem = "ext4";
    CHECK(PlaceInPicked(f, steamOS).notes.empty(), "ext4: nothing to say");
    // In memory: gone at the next restart.
    f = Facts("/tmp/games", FolderState::Empty);
    f.fileSystem = "tmpfs";
    c = PlaceInPicked(f, steamOS);
    CHECK(c.result.error == Error::DestinationNotWritable && Has(c.result.message, "/tmp/games is kept in memory (tmpfs)")
        && Has(c.result.message, "SteamOS restarts"), "tmpfs: %s", c.result.message.c_str());
    f.state = FolderState::Missing;
    CHECK(PlaceExact(f, true, onLinux).result.error == Error::DestinationNotWritable, "tmpfs, exact (--dest, made on it)");
    CHECK(IsMemoryFileSystem("ramfs") && !IsMemoryFileSystem("ext4") && !IsMemoryFileSystem(""), "memory file systems");
    // --dest into a folder not there yet gets the notes of the drive it would be made on.
    f.fileSystem = "exfat";
    c = PlaceExact(f, true, steamOS);
    CHECK(c.result.Ok() && c.notes.size() == 1 && Has(c.notes[0], "exFAT"), "--dest new on exFAT: a note (%zu)", c.notes.size());
    f = Facts("/Volumes/WIN", FolderState::Other);
    f.driveTop = true;
    f.fileSystem = "ntfs";
    c = PlaceInPicked(f, mac);
    CHECK(c.result.error == Error::DestinationNotWritable && Has(c.result.message, "macOS can only read"), "NTFS on macOS: %s",
        c.result.message.c_str());
    CHECK(IsWindowsFileSystem("msdos") && IsWindowsFileSystem("ntfs3") && !IsWindowsFileSystem("apfs") && !IsWindowsFileSystem("btrfs"),
        "Windows formats");

    // macOS asks before an app uses Desktop, Documents, Downloads or another drive.
    f = Facts("/Users/p/Documents/Games", FolderState::Other);
    f.fileSystem = "apfs";
    c = PlaceInPicked(f, mac);
    CHECK(c.result.Ok() && c.notes.size() == 1 && Has(c.notes[0], "Allow"), "Documents: a privacy note");
    f.path = "/Users/p/Games";
    CHECK(PlaceInPicked(f, mac).notes.empty(), "~/Games: no note");
    f.path = "/Volumes/USB";
    f.driveTop = true;
    f.fileSystem = "exfat";
    c = PlaceInPicked(f, mac);
    CHECK(c.result.Ok() && c.gameDir == "/Volumes/USB" / drive && c.notes.size() == 2, "an exFAT drive on the Mac: %s, %zu notes",
        c.gameDir.c_str(), c.notes.size());
    f = Facts("/home/p/Documents/Games", FolderState::Other);
    CHECK(PlaceInPicked(f, onLinux).notes.empty(), "no privacy note on Linux");

    // macOS: never into iCloud Drive or another cloud folder (the system's
    // word, or ~/Library's cloud folders); elsewhere there's no such thing.
    f = Facts("/Users/p/Documents/Games", FolderState::Other);
    f.fileSystem = "apfs";
    f.cloud = true;
    c = PlaceInPicked(f, mac);
    CHECK(c.result.error == Error::DestinationNotWritable && Has(c.result.message, "/Users/p/Documents/Games is kept in iCloud Drive")
        && !Has(c.result.message, kPickedSubfolder), "iCloud (Documents kept there): %s", c.result.message.c_str());
    CHECK(PlaceExact(f, true, mac).result.error == Error::DestinationNotWritable, "iCloud, exact (--dest)");
    f.cloud = false;
    for (const char* path : { "/Users/p/Library/Mobile Documents/com~apple~CloudDocs/Games", "/Users/p/Library/CloudStorage/Dropbox/Games" })
    {
        f.path = path;
        c = PlaceInPicked(f, mac);
        CHECK(c.result.error == Error::DestinationNotWritable && Has(c.result.message, "cloud"), "%s: %s", path, c.result.message.c_str());
    }
    f.path = "/Users/p/Library/Mobile Documents/com~apple~CloudDocs/Games";
    f.cloud = true;
    CHECK(PlaceInPicked(f, onLinux).result.Ok(), "no cloud rule off macOS");

    // The recorded folder, offered again as it is.
    FolderFacts g = Facts("/run/media/p/SD2/games/sb", FolderState::Missing);
    c = PlaceExact(g, false, onLinux);
    CHECK(c.result.error == Error::NotFound && Has(c.result.message, "isn't there now"), "a drive that isn't connected: %s",
        c.result.message.c_str());
    c = PlaceExact(g, true, onLinux);
    CHECK(c.result.Ok() && c.gameDir == g.path, "missing, but its folder is there: made again");
    c = PlaceExact(Facts("/home/p", FolderState::Other), true, onLinux);
    CHECK(c.result.error == Error::DestinationNotWritable && Has(c.result.message, "home folder"), "home: %s", c.result.message.c_str());
    c = PlaceExact(Facts("/home/p/Games/SpeedBreaker/game", FolderState::Missing), true, onLinux);
    CHECK(c.result.error == Error::DestinationNotWritable, "inside the bundle");
    g = Facts("/run/media/p/USB", FolderState::Install);
    g.driveTop = true;
    CHECK(PlaceExact(g, true, onLinux).result.error == Error::DestinationNotWritable, "a whole drive");

    // IsAtOrInside.
    CHECK(IsAtOrInside("/a/b", "/a") && IsAtOrInside("/a", "/a") && IsAtOrInside("/a/", "/a") && IsAtOrInside("/a/b/../c", "/a"),
        "inside");
    CHECK(!IsAtOrInside("/ab", "/a") && !IsAtOrInside("/", "/a") && !IsAtOrInside("/a/../b", "/a") && !IsAtOrInside("", "/a"),
        "outside");

    // The hint when the folder picker can't be shown: one line, with the command.
    std::string hint = FolderPickerHint(true, false, true, "~/Games/SpeedBreaker/speedbreaker.sh", "~/Downloads/nfsmw.iso");
    CHECK(Has(hint, "Game Mode") && Has(hint, "Desktop Mode") &&
        Has(hint, "~/Games/SpeedBreaker/speedbreaker.sh --install ~/Downloads/nfsmw.iso --dest <folder>") && !Has(hint, "\n"),
        "Game Mode: %s", hint.c_str());
    hint = FolderPickerHint(false, false, true, "speedbreaker.sh", "<image>");
    CHECK(Has(hint, "portal or zenity") && !Has(hint, "Game Mode") && Has(hint, "--dest <folder>"), "no dialog: %s", hint.c_str());
    // A picker that failed some other way (zenity refusing an option, say)
    // doesn't blame a missing one.
    hint = FolderPickerHint(false, false, false, "speedbreaker.sh", "<image>");
    CHECK(!Has(hint, "zenity") && Has(hint, "couldn't open") && Has(hint, "--dest <folder>"), "dialog failed: %s", hint.c_str());
    hint = FolderPickerHint(true, true, true, "/Applications/SpeedBreaker.app/Contents/MacOS/SpeedBreaker", "~/Games/nfsmw.iso");
    CHECK(Has(hint, "Terminal") && !Has(hint, "Game Mode") && Has(hint, "SpeedBreaker.app/Contents/MacOS/SpeedBreaker --install"),
        "macOS: %s", hint.c_str());

    // What the installer says about a recorded install it passed over.
    RecordedInstall r;
    r.path = "/run/media/p/SD/x";
    CHECK(DescribeRecordedInstall(r, "~/x", false).empty(), "none");
    r.state = RecordedState::Usable;
    CHECK(DescribeRecordedInstall(r, "~/x", false).empty(), "usable");
    r.state = RecordedState::Missing;
    std::string text = DescribeRecordedInstall(r, "/run/media/p/SD/x", false);
    CHECK(Has(text, "/run/media/p/SD/x") && Has(text, "isn't there now") && Has(text, "connect it"), "missing: %s", text.c_str());
    r.state = RecordedState::NotAllowed;
    r.error = EPERM;
    CHECK(Has(DescribeRecordedInstall(r, "~/Documents/g", true), "Privacy & Security"), "macOS privacy");
    CHECK(Has(DescribeRecordedInstall(r, "~/g", false), "permissions"), "Linux permissions");
    r.state = RecordedState::NotCurrent;
    CHECK(Has(DescribeRecordedInstall(r, "~/g", false), "incomplete or from an older version"), "not current");
}

// --- Through the disk -------------------------------------------------------

static void Write(const fs::path& file, const std::string& text)
{
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

static void DiskRules(const fs::path& root, const char* argv0)
{
    // A sandbox HOME, so the default install and the record are in the scratch folder.
    fs::path home = root / "home";
    fs::create_directories(home);
    setenv("HOME", home.c_str(), 1);
    unsetenv("XDG_DATA_HOME");
    unsetenv("NFSMW_GAME_DIR");
    SetProgramPath(argv0);
    chdir(root.c_str());  // no ./game/files here for FindGameInstall to find first

    // A small manifest: one default.xex of 16 bytes.
    static const ManifestFile files[] = { { "default.xex", 16, "00" } };
    Manifest manifest{ "test-1", 0x454107D9, files, 16 };
    Manifest huge = manifest;
    huge.totalBytes = 1ull << 62;
    auto marker = [&](const fs::path& dir) {
        Write(dir / "default.xex", std::string(16, 'x'));
        Write(dir / kInstallMarker, "manifest_version = \"test-1\"\n");
    };
    const std::string sub = kPickedSubfolder;

    // The file system is named (mountinfo on Linux, statfs on macOS); the
    // context is this machine's (printed: it differs from host to host).
    FolderFacts probed = ProbeFolder(root);
    CHECK(probed.state == FolderState::Other && !probed.fileSystem.empty(), "the scratch folder (holding home/): state %d, "
        "file system '%s'", int(probed.state), probed.fileSystem.c_str());
    CHECK(!probed.cloud, "the scratch folder isn't in iCloud Drive");
    PlacementContext context = CurrentPlacementContext(manifest);
    printf("[context] file system %s, steamOS %d, macOS %d, bundle %s, home %s\n", probed.fileSystem.c_str(), context.steamOS,
        context.macOS, context.bundle.c_str(), context.home.c_str());
    // A folder known to be in iCloud Drive (macOS, by hand: it reads only the
    // folder's own metadata), and one known not to be.
    if (const char* cloud = std::getenv("INSTALL_FOLDER_TEST_ICLOUD"); cloud && *cloud)
    {
        FolderFacts f = ProbeFolder(cloud);
        CHECK(f.cloud, "%s is in iCloud Drive", cloud);
        FolderFacts g = ProbeFolder(fs::path(cloud) / "no-such-folder/below");
        CHECK(g.cloud, "and a folder that isn't there yet below it");
        printf("[context] %s: in iCloud Drive %d (a missing folder below it: %d)\n", cloud, f.cloud, g.cloud);
    }

    fs::create_directories(root / "empty");
    FolderChoice c = CheckPickedFolder(root / "empty", manifest);
    CHECK(c.result.Ok() && c.gameDir == fs::canonical(root / "empty") && c.info.required > 16, "empty: as is: %s %s",
        c.gameDir.c_str(), c.result.message.c_str());
    Write(root / "hidden/.DS_Store", "x");
    c = CheckPickedFolder(root / "hidden", manifest);
    CHECK(c.result.Ok() && c.subfolder == SubfolderReason::None, "only hidden files: as is");
    Write(root / "full/notes.txt", "x");
    c = CheckPickedFolder(root / "full", manifest);
    CHECK(c.result.Ok() && c.gameDir == fs::canonical(root / "full") / sub, "not empty: subfolder: %s", c.gameDir.c_str());
    Write(root / "full2/notes.txt", "x");
    Write(root / "full2" / sub / "other.txt", "x");
    c = CheckPickedFolder(root / "full2", manifest);
    CHECK(c.result.error == Error::DestinationNotEmpty, "subfolder not empty: %s", c.result.message.c_str());
    Write(root / "full3/notes.txt", "x");
    marker(root / "full3" / sub);
    c = CheckPickedFolder(root / "full3", manifest);
    CHECK(c.result.Ok() && c.info.replacing, "subfolder holding a previous install: replaced");
    marker(root / "prev");
    c = CheckPickedFolder(root / "prev", manifest);
    CHECK(c.result.Ok() && c.gameDir == fs::canonical(root / "prev") && c.info.replacing, "a previous install: replaced in place");
    c = CheckPickedFolder(root / "nothing-here", manifest);
    CHECK(c.result.error == Error::NotFound, "missing");
    Write(root / "file", "x");
    c = CheckPickedFolder(root / "file", manifest);
    CHECK(c.result.error == Error::DestinationNotEmpty, "a file");
    fs::create_directories(root / "empty2");
    fs::create_directory_symlink(root / "empty2", root / "link");
    c = CheckPickedFolder(root / "link", manifest);
    CHECK(c.result.Ok() && c.gameDir == fs::canonical(root / "empty2"), "a link: resolved: %s", c.gameDir.c_str());
    c = CheckPickedFolder(root / "empty", huge);
    CHECK(c.result.error == Error::NotEnoughSpace, "free space: %s", c.result.message.c_str());

    // A link named like the subfolder (here into a folder the rules would
    // refuse): the rules checked the link's path, and Install followed it.
    Write(root / "linked/notes.txt", "x");
    fs::create_directories(home / "Library/Mobile Documents/elsewhere");
    fs::create_directory_symlink(home / "Library/Mobile Documents/elsewhere", root / "linked" / sub);
    c = CheckPickedFolder(root / "linked", manifest);
    CHECK(c.result.error == Error::DestinationNotEmpty && Has(c.result.message, "is a link to"), "a link as the subfolder: %s",
        c.result.message.c_str());
    fs::remove(root / "linked" / sub);
    fs::create_directories(root / "linked" / sub);
    CHECK(CheckPickedFolder(root / "linked", manifest).result.Ok(), "the same, a real folder: fine");
    // A link named like the subfolder that leads nowhere (absolute, and
    // relative to its folder): named by where it points.
    Write(root / "dangle/notes.txt", "x");
    fs::create_directory_symlink(root / "nowhere", root / "dangle" / sub);
    FolderFacts df = ProbeFolder(root / "dangle");
    CHECK(df.subfolder.link && df.subfolder.dangling && df.subfolder.target == root / "nowhere", "a link leading nowhere: "
        "probed: link %d dangling %d target %s", df.subfolder.link, df.subfolder.dangling, df.subfolder.target.c_str());
    c = CheckPickedFolder(root / "dangle", manifest);
    CHECK(c.result.error == Error::DestinationNotEmpty && Has(c.result.message, ((root / "nowhere").string() + ", which isn't there").c_str()),
        "a link leading nowhere: %s", c.result.message.c_str());
    fs::remove(root / "dangle" / sub);
    fs::create_directory_symlink("../nowhere2", root / "dangle" / sub);
    df = ProbeFolder(root / "dangle");
    CHECK(df.subfolder.dangling && df.subfolder.target == root / "nowhere2", "a relative link leading nowhere: %s",
        df.subfolder.target.c_str());
    fs::remove(root / "dangle" / sub);
    fs::create_directory_symlink(root / "empty2", root / "dangle" / sub);
    df = ProbeFolder(root / "dangle");
    CHECK(df.subfolder.link && !df.subfolder.dangling && df.subfolder.target == fs::canonical(root / "empty2"),
        "a link that leads somewhere isn't dangling: %s", df.subfolder.target.c_str());
    fs::remove(root / "dangle" / sub);

    // --dest into a folder not there yet: the file system it would be made
    // on (so its notes and rules apply).
    FolderFacts fresh = ProbeFolder(root / "not/yet");
    CHECK(fresh.state == FolderState::Missing && fresh.fileSystem == probed.fileSystem, "a folder not there yet: file system "
        "'%s' (its drive's: '%s')", fresh.fileSystem.c_str(), probed.fileSystem.c_str());
#ifdef __linux__
    // In memory (tmpfs): refused, and nothing made there.
    if (ProbeFolder("/dev/shm").fileSystem == "tmpfs")
    {
        fs::path shm = fs::path("/dev/shm") / ("install_folder_test." + std::to_string(getpid())) / "game";
        c = CheckExactFolder(shm, true, manifest);
        CHECK(c.result.error == Error::DestinationNotWritable && Has(c.result.message, "kept in memory"), "--dest on tmpfs: %s",
            c.result.message.c_str());
        CHECK(!fs::exists(shm.parent_path()), "nothing made on tmpfs");
    }
    else
        printf("[context] /dev/shm isn't tmpfs here: the tmpfs check is the pure one only\n");
#endif

    // Permissions (meaningless as root).
    if (geteuid() != 0)
    {
        Write(root / "ro/notes.txt", "x");
        chmod((root / "ro").c_str(), 0555);
        c = CheckPickedFolder(root / "ro", manifest);
        CHECK(c.result.error == Error::DestinationNotWritable, "can't write the picked folder: %s", c.result.message.c_str());
        chmod((root / "ro").c_str(), 0755);
        fs::create_directories(root / "rop/e");
        chmod((root / "rop").c_str(), 0555);
        c = CheckPickedFolder(root / "rop/e", manifest);
        CHECK(c.result.Ok() && c.subfolder == SubfolderReason::ParentReadOnly && c.gameDir == fs::canonical(root / "rop/e") / sub,
            "empty, parent read-only: subfolder: %s %s", c.gameDir.c_str(), c.result.message.c_str());
        chmod((root / "rop").c_str(), 0755);
        fs::create_directories(root / "nr");
        chmod((root / "nr").c_str(), 0);
        c = CheckPickedFolder(root / "nr", manifest);
        CHECK(c.result.error == Error::DestinationNotWritable, "unreadable: %s", c.result.message.c_str());
        chmod((root / "nr").c_str(), 0755);
        // Empty, but not writable: refused before the copy (macOS refuses
        // to replace it at the end), picked or exact.
        fs::create_directories(root / "ro-empty");
        chmod((root / "ro-empty").c_str(), 0555);
        c = CheckPickedFolder(root / "ro-empty", manifest);
        CHECK(c.result.error == Error::DestinationNotWritable && Has(c.result.message, "can't write to"), "empty, read-only: %s",
            c.result.message.c_str());
        c = CheckExactFolder(root / "ro-empty", true, manifest);
        CHECK(c.result.error == Error::DestinationNotWritable, "--dest empty, read-only: %s", c.result.message.c_str());
        chmod((root / "ro-empty").c_str(), 0755);
    }
#if defined(__APPLE__)
    // Hidden files only, with macOS's "deny delete" for everyone (as fresh
    // ~/Music and ~/Pictures have): Install can't move it aside, so a subfolder.
    Write(root / "acl/.localized", "");
    if (system(("chmod +a 'group:everyone deny delete' '" + (root / "acl").string() + "'").c_str()) == 0)
    {
        c = CheckPickedFolder(root / "acl", manifest);
        CHECK(c.result.Ok() && c.subfolder == SubfolderReason::KeptInPlace && c.gameDir == root / "acl" / sub,
            "hidden files, deny delete: subfolder: %s %s", c.gameDir.c_str(), c.result.message.c_str());
        system(("chmod -N '" + (root / "acl").string() + "'").c_str());
    }
    else
        CHECK(false, "couldn't set an ACL on %s", (root / "acl").c_str());
    c = CheckPickedFolder(root / "acl", manifest);
    CHECK(c.result.Ok() && c.subfolder == SubfolderReason::None, "hidden files, no ACL: as is");
#endif

    // Inside the default install, or the recorded one; SpeedBreaker's own folder.
    Write(DefaultInstallPath() / "NFS/x.bin", "x");
    c = CheckPickedFolder(DefaultInstallPath() / "NFS", manifest);
    CHECK(c.result.error == Error::DestinationNotWritable && Has(c.result.message, "inside the game installed"),
        "inside the default install: %s", c.result.message.c_str());
    CHECK(c.gameDir == fs::canonical(DefaultInstallPath() / "NFS") / sub, "(its subfolder)");
    marker(root / "rec");
    CHECK(RecordInstallPath(root / "rec"), "record");
    CHECK(RecordedInstallPath() == fs::absolute(root / "rec").lexically_normal(), "recorded: %s",
        RecordedInstallPath() ? RecordedInstallPath()->c_str() : "nothing");
    // The same folder however it is spelled: `--dest dir/` (tab completion),
    // or through a link. The record is the folder itself.
    CHECK(RecordInstallPath((root / "rec").string() + "/") && RecordedInstallPath() == root / "rec", "recorded with a "
        "trailing slash: %s", RecordedInstallPath() ? RecordedInstallPath()->c_str() : "nothing");
    fs::create_directory_symlink(root / "rec", root / "rec-link");
    CHECK(SameFolder(root / "rec-link", root / "rec") && SameFolder(root / "rec/", root / "rec") && !SameFolder(root / "rec", root / "prev"),
        "SameFolder");
    CHECK(RecordInstallPath(root / "rec-link") && RecordedInstallPath() == root / "rec", "recorded through a link: %s",
        RecordedInstallPath() ? RecordedInstallPath()->c_str() : "nothing");
    fs::remove(root / "rec-link");
    CHECK(RecordInstallPath(DefaultInstallPath().string() + "/") && !RecordedInstallPath(), "the default, with a trailing "
        "slash: no record");
    // A record v0.1.0 wrote for `--dest dir/` reads as the folder.
    Write(DefaultInstallPath().parent_path() / "game-location", (root / "rec").string() + "/\n");
    CHECK(RecordedInstallPath() == root / "rec", "an old record with a trailing slash: %s",
        RecordedInstallPath() ? RecordedInstallPath()->c_str() : "nothing");
    CHECK(RecordInstallPath(root / "rec") && RecordedInstallPath() == root / "rec", "recorded again");
    fs::create_directories(root / "rec/Movies");
    c = CheckPickedFolder(root / "rec/Movies", manifest);
    CHECK(c.result.error == Error::DestinationNotWritable, "inside the recorded install: %s", c.result.message.c_str());
    fs::path bundle = BundleFolder();
    CHECK(!bundle.empty() && fs::exists(bundle), "the test's own folder: %s", bundle.c_str());
    c = CheckPickedFolder(bundle, manifest);
    CHECK(c.result.error == Error::DestinationNotWritable && Has(c.result.message, "SpeedBreaker's own folder"),
        "the program's folder: %s", c.result.message.c_str());

    // The command line's --dest: exactly that folder (missing folders above it
    // are made), with the same refusals.
    c = CheckExactFolder(bundle / "game", true, manifest);
    CHECK(c.result.error == Error::DestinationNotWritable && Has(c.result.message, "inside SpeedBreaker's own folder"),
        "--dest in the program's folder: %s", c.result.message.c_str());
    c = CheckExactFolder(DefaultInstallPath() / "NFS/new", true, manifest);
    CHECK(c.result.error == Error::DestinationNotWritable && Has(c.result.message, "inside the game installed"),
        "--dest inside the default install: %s", c.result.message.c_str());
    c = CheckExactFolder(home, true, manifest);
    CHECK(c.result.error == Error::DestinationNotWritable && Has(c.result.message, "home folder"), "--dest home: %s",
        c.result.message.c_str());
    c = CheckExactFolder(root / "made/on/demand", true, manifest);
    CHECK(c.result.Ok() && c.subfolder == SubfolderReason::None && c.gameDir == root / "made/on/demand", "--dest new: %s %s",
        c.gameDir.c_str(), c.result.message.c_str());
    c = CheckExactFolder(root / "made/on/demand", false, manifest);
    CHECK(c.result.error == Error::NotFound, "the same, recorded (its drive gone): %s", c.result.message.c_str());
    c = CheckExactFolder(root / "full", true, manifest);
    CHECK(c.result.error == Error::DestinationNotEmpty, "--dest not empty: no subfolder, refused: %s", c.result.message.c_str());

    // The recorded install from the start: usable, then gone, then back.
    CHECK(CheckRecordedInstall(manifest).state == RecordedState::Usable, "recorded: usable");
    std::optional<GameInstall> found = FindGameInstall(manifest);
    CHECK(found && found->path == fs::absolute(root / "rec").lexically_normal() && found->marker, "found where recorded: %s",
        found ? found->path.c_str() : "nothing");
    fs::rename(root / "rec", root / "rec-unplugged");
    CHECK(CheckRecordedInstall(manifest).state == RecordedState::Missing, "recorded: missing");
    CHECK(!FindGameInstall(manifest), "missing: not found (the installer runs, with the notice)");
    CHECK(RecordedInstallPath().has_value(), "the record is kept for when the drive is back");
    c = CheckExactFolder(root / "rec", false, manifest);
    CHECK(c.result.Ok() && c.gameDir == fs::absolute(root / "rec").lexically_normal(), "offered again: its folder is there");
    c = CheckExactFolder(root / "unplugged-drive/speedbreaker/game", false, manifest);
    CHECK(c.result.error == Error::NotFound, "offered again on a drive that isn't there: %s", c.result.message.c_str());
    CHECK(!fs::exists(root / "unplugged-drive"), "nothing was created where the drive mounts");
    fs::rename(root / "rec-unplugged", root / "rec");
    found = FindGameInstall(manifest);
    CHECK(found && found->path == fs::absolute(root / "rec").lexically_normal(), "back: found again");
    fs::remove(root / "rec" / kInstallMarker);
    CHECK(CheckRecordedInstall(manifest).state == RecordedState::NotCurrent, "recorded: not current");
}

int main(int argc, char** argv)
{
    PureRules();

    // The disk checks' own new folder, made inside the one given (which is
    // never emptied or deleted: a mistyped argument must lose nothing).
    std::error_code ec;
    fs::path base = argc > 1 ? fs::path(argv[1]) : fs::current_path();
    fs::create_directories(base, ec);
    base = fs::weakly_canonical(fs::absolute(base), ec);
    // The program's own folder is refused (that's a rule under test), so the
    // scratch folder can't be in it.
    SetProgramPath(argv[0]);
    if (IsAtOrInside(base, BundleFolder()))
    {
        printf("the folder %s is inside the test's own folder %s: give one outside it\n", base.c_str(), BundleFolder().c_str());
        return 2;
    }
    std::string pattern = (base / "install_folder_test.XXXXXX").string();
    if (!mkdtemp(pattern.data()))
    {
        printf("can't make a folder in %s: %s\n", base.c_str(), strerror(errno));
        return 2;
    }
    fs::path root = fs::canonical(pattern);
    DiskRules(root, argv[0]);
    // Leave nothing read-only behind, then clean up (only the folder made here).
    for (const char* dir : { "ro", "rop", "nr", "ro-empty" })
        chmod((root / dir).c_str(), 0755);
    fs::remove_all(root, ec);

    printf("%s (%d failures)\n", g_failures ? "FAILED" : "passed", g_failures);
    return g_failures ? 1 : 0;
}
