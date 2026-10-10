// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// "Choose a folder..." in the installer: where in a folder the player picked
// the game goes, and whether it may go there. GitHub issue #1 asked for an
// install outside ~/.local/share; the --install command's --dest already
// takes any folder as it is, and this is the installer screen's way to do it.
//
// The rules (PlaceInPicked):
//   - A new or empty folder (hidden files such as .DS_Store don't count) is
//     used as it is, and so is one that holds a previous SpeedBreaker install
//     (it is replaced, keeping DLC and the player's own files, as Install
//     always does).
//   - Any other folder gets a new folder inside it, kPickedSubfolder, which
//     must be new, empty or a previous install (and not a link: it would take
//     the game somewhere else). So do three folders that are empty but can't
//     be used as they are: the home folder itself, a folder whose parent
//     SpeedBreaker can't write (Install copies into a sibling
//     "<folder>.partial" first), and on macOS a folder holding only hidden
//     files that macOS won't let be moved (Install moves such a folder aside;
//     fresh ~/Music, ~/Pictures and ~/Desktop deny that to everyone). The top
//     of a drive (Install renames its copy into place, and a drive's top
//     can't be renamed) gets the drive's own install folder instead,
//     <drive>/speedbreaker/game (SpeedBreaker/game on macOS): the one the To
//     row's entry for that drive uses, so the two never make two copies.
//   - Never into the system's root folder, the home folder itself,
//     SpeedBreaker's own folder (the SteamOS bundle or the Mac app: an update
//     or an uninstall replaces it, and on the Mac the app's signature would
//     break) or anything inside it, or a folder inside an install (the
//     default one or the one recorded last time).
//   - Never into an existing folder SpeedBreaker can't write: Install
//     replaces it or moves it aside, which macOS refuses for a folder its
//     owner can't write, after the whole copy.
//   - macOS: never into iCloud Drive (Desktop and Documents too, when the Mac
//     keeps them there) or another cloud folder (~/Library/CloudStorage):
//     macOS would upload the 7 GB and may remove the local copies to save
//     space, and the game reads its files as it plays.
//   - Linux: never into a folder kept in memory (tmpfs, such as /tmp on
//     SteamOS): it is emptied when the system restarts.
//   - Drives formatted for Windows are fine: the game only reads its files.
//     FAT32 can't hold a file of 4 GB or more (no game file is that big;
//     the check is the manifest's), NTFS is read-only on macOS, and SteamOS
//     doesn't mount such drives in Game Mode by itself (its automount takes
//     ext4 only), which the player is told.
//   - Free space and permission to write are CheckDestination's (Install's
//     own checks), on the folder the game would go to. The rules look at the
//     picked folder with its links resolved, and refuse a link where the
//     subfolder goes, so they see the folder Install will really use.
// The command line's --dest is exact (no subfolder), but gets the same
// refusals (CheckExactFolder).
//
// The deciding is pure (PlaceInPicked, PlaceExact: facts in, a choice out,
// tested in tests/install_folder_test.cpp); ProbeFolder is the only part that
// looks at the disk, and CheckPickedFolder / CheckExactFolder put the two
// together with CheckDestination.
//
// Windows: the disk reads are std::filesystem's where it has one, so this
// builds there; what it lacks (drive tops of mounted folders, writability,
// the file system's name) is POSIX-only and assumed fine. The screen hides
// "Choose a folder..." there until a port does those properly (the windows
// branch's platform/ layer: CheckWritableFolder, OneDrive as a cloud folder,
// drive roots other than the system's, SpeedBreaker.exe in the hints).
#pragma once
#include "installer.h"
#include "result.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace install
{
    // The folder made inside a picked folder that isn't empty. Not
    // "SpeedBreaker": on SteamOS that's the bundle's own folder in ~/Games,
    // the folder players are most likely to pick.
    inline constexpr const char* kPickedSubfolder = "SpeedBreaker Game";

    enum class FolderState
    {
        Missing,        // nothing there
        Empty,          // a folder holding nothing, or only hidden files
        Install,        // a folder holding Install's marker (a previous install)
        Other,          // a folder holding other files
        NotAFolder,     // a file (or a link to one)
        Unreadable,     // permission denied (on macOS, maybe privacy settings); `error` says
    };

    // What is at the folder the game would go in inside a picked one
    // (SubfolderName): ProbeFolder fills it in.
    struct SubfolderFacts
    {
        FolderState state = FolderState::Missing;
        int error = 0;                  // errno of the failed look, if any
        bool writable = true;           // it can be written (when it is there)
        bool link = false;              // it, or a folder on the way to it, is a link (to `target`)
        bool dangling = false;          // that link leads nowhere (`target`: where it points, as far as it says)
        std::filesystem::path target;
    };

    // What is on disk at a folder (ProbeFolder fills it in).
    struct FolderFacts
    {
        std::filesystem::path path;     // absolute, links resolved
        FolderState state = FolderState::Missing;
        int error = 0;                  // errno of the failed look, if any
        bool hidden = false;            // Empty, but holding hidden files: Install moves it aside rather than replace it
        bool writable = true;           // the folder itself can be written (when it is there)
        bool movable = true;            // macOS: no "deny delete" in its ACL, no immutable flag (it can be moved aside)
        bool driveTop = false;          // the top of a mounted drive (or the system's root folder)
        bool parentWritable = true;     // the folder above it can be written (Install's .partial goes there)
        std::string fileSystem;         // "apfs", "ext4", "exfat", "vfat", "msdos", "ntfs", "fuseblk", "tmpfs"...; "" unknown.
                                        // A missing folder: the nearest folder above it that is there.
        bool cloud = false;             // macOS: in iCloud Drive (the system says so; ~/Library paths are the rules')
        SubfolderFacts subfolder;       // path / SubfolderName(driveTop)
    };

    // What the rules need to know besides the folder itself.
    struct PlacementContext
    {
        std::filesystem::path home;                     // the home folder (empty: unknown)
        std::filesystem::path bundle;                   // SpeedBreaker's own folder (empty: unknown)
        std::vector<std::filesystem::path> installs;    // installs nothing may go inside: the default and the recorded one
        uint64_t largestFile = 0;                       // the manifest's biggest file
        bool macOS = false;
        bool steamOS = false;
    };

    // Why the game goes in a new folder inside the picked one.
    enum class SubfolderReason { None, NotEmpty, DriveTop, Home, ParentReadOnly, KeptInPlace };

    // The folder made inside a picked one: kPickedSubfolder, or on the top
    // of a drive the drive's own install folder (InstallDestinations').
    std::filesystem::path SubfolderName(bool driveTop);

    struct FolderChoice
    {
        std::filesystem::path picked;   // the folder the player picked (links resolved)
        std::filesystem::path gameDir;  // where the game goes: `picked`, or SubfolderName() in it
        SubfolderReason subfolder = SubfolderReason::None;
        std::vector<std::string> notes; // things to know that don't stop the install
        Result result;                  // Ok, or why not (DestinationNotWritable, DestinationNotEmpty, NotFound, NotEnoughSpace)
        DestinationInfo info;           // CheckDestination's for gameDir, when it got that far
    };

    // Pure: where in the picked folder the game goes, and the refusals and
    // notes above. No free-space or permission check (CheckPickedFolder adds
    // CheckDestination's).
    FolderChoice PlaceInPicked(const FolderFacts& picked, const PlacementContext& context);

    // Pure: the same refusals for an exact game folder (the one recorded
    // last time, offered again, or the command line's --dest).
    // `parentExists`: the folder above it is there; if not, it's on a drive
    // that isn't (NotFound), and nothing should be created where the drive
    // mounts. (The command line passes true: it makes missing folders.)
    FolderChoice PlaceExact(const FolderFacts& gameDir, bool parentExists, const PlacementContext& context);

    // One line under the folder's path: why the game goes in a new folder
    // inside the picked one ("" for SubfolderReason::None).
    std::string SubfolderExplanation(const FolderChoice& choice);

    // Reads the disk: what is at `path` (not following it further than
    // links). Cheap: a few stat calls, two folder listings that stop at the
    // first visible entry, and on Linux one read of /proc/self/mountinfo.
    FolderFacts ProbeFolder(const std::filesystem::path& path);

    // The running game's context: $HOME, BundleFolder(), the default and
    // recorded installs, the manifest's biggest file, the platform.
    PlacementContext CurrentPlacementContext(const Manifest& manifest = GameManifest());

    // ProbeFolder + PlaceInPicked + CheckDestination on the chosen game
    // folder: everything the installer screen shows for a picked folder.
    FolderChoice CheckPickedFolder(const std::filesystem::path& picked, const Manifest& manifest = GameManifest());

    // The same for an exact game folder (no subfolder): the one recorded last
    // time (RecordedInstallPath; `mayCreateParents` false: a missing drive is
    // NotFound), or the command line's --dest (true: Install makes the
    // folders above it).
    FolderChoice CheckExactFolder(const std::filesystem::path& gameDir, bool mayCreateParents,
        const Manifest& manifest = GameManifest());

    // main() calls this first with argv[0], so BundleFolder() can say which
    // folder the game runs from (the SteamOS launcher passes the program's
    // absolute path as argv[0]; the bundled C library's loader would be
    // /proc/self/exe).
    void SetProgramPath(const char* argv0);

    // SpeedBreaker's own folder: the Mac app (SpeedBreaker.app), the SteamOS
    // bundle (the folder with speedbreaker.sh; also when the program is in
    // its lib/), or the program's folder (a development build). Empty if
    // unknown.
    std::filesystem::path BundleFolder();

    // Is `path` `base` or inside it? (Lexical: both absolute and normalized.)
    bool IsAtOrInside(const std::filesystem::path& path, const std::filesystem::path& base);

    // A folder as the installer records and compares it: absolute, links
    // resolved as far as it exists, no trailing separator. SameFolder: the
    // two are the same folder that way (`--dest dir/` and `dir`, or a link
    // and where it leads).
    std::filesystem::path ResolvedFolder(const std::filesystem::path& path);
    bool SameFolder(const std::filesystem::path& a, const std::filesystem::path& b);

    // A drive formatted for Windows (FAT, exFAT, NTFS, or ntfs-3g's fuseblk).
    bool IsWindowsFileSystem(std::string_view fileSystem);

    // A file system kept in memory and emptied at a restart (tmpfs, ramfs).
    bool IsMemoryFileSystem(std::string_view fileSystem);

    // Pure: the one-line hint when "Choose a folder..." can't show a folder
    // picker: in SteamOS Game Mode (nothing of the desktop is on screen, so
    // it isn't tried), or where no dialog could open (on Linux,
    // `noPickerApp`: SDL found neither a file chooser portal nor zenity;
    // otherwise the picker failed some other way, logged; on macOS, any
    // failure). The command line installs into any folder, so it says how:
    // `program` is what to run (the bundle's speedbreaker.sh, the Mac app's
    // binary), `image` the image already chosen.
    std::string FolderPickerHint(bool gameMode, bool macOS, bool noPickerApp, const std::string& program,
        const std::string& image);

    // Why the install recorded last time (RecordedInstallPath) wasn't used.
    enum class RecordedState
    {
        None,           // nothing recorded
        Usable,         // it's there and current
        Missing,        // the folder isn't there (a drive not connected, or deleted)
        NotAllowed,     // it can't be read (on macOS, privacy settings)
        NotCurrent,     // the folder is there, but not a current install (files missing, an older version)
    };

    struct RecordedInstall
    {
        std::filesystem::path path;
        RecordedState state = RecordedState::None;
        int error = 0;
    };

    // Looks at the recorded install (a few stat calls).
    RecordedInstall CheckRecordedInstall(const Manifest& manifest = GameManifest());

    // Pure: the installer's notice for a recorded install that wasn't used,
    // with the path as the player reads it (`shown`, ~ for home): what
    // happened and what to do. "" for None and Usable.
    std::string DescribeRecordedInstall(const RecordedInstall& recorded, const std::string& shown, bool macOS);
}
