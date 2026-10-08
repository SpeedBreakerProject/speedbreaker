// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// First-run install: copy the user's disc (an image or an extracted folder)
// into a game directory, checking every file against the manifest's size and
// SHA-256 as it is copied (one read of the source), so a bad dump is caught
// at install time rather than as a crash an hour into the game.
//
// The copy goes to a sibling "<destination>.partial" directory and is renamed
// into place only once every file has matched and the install marker is
// written, so the destination is never half-installed: a cancelled, failed or
// interrupted install leaves the previous state (nothing, or the old install).
//
// Everything here blocks; a UI runs it on a worker thread. Typical flow:
//   OpenDiscSource(path) -> CheckSource() (fast: default.xex + file list;
//   show the result) -> CheckDestination() (instant; the confirm page) ->
//   Install() (minutes; progress + cancel).
#pragma once
#include "disc_source.h"
#include "manifest.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace install
{
    struct Progress
    {
        enum class Phase
        {
            Checking,       // default.xex and the file list
            Copying,        // Install: copying and hashing
            Verifying,      // Verify: hashing only
            Finalizing,     // writing the marker, moving the install into place
        };
        Phase phase = Phase::Checking;
        uint64_t bytesDone = 0, bytesTotal = 0;
        size_t filesDone = 0, filesTotal = 0;
        std::string_view currentFile;   // disc-relative path, manifest spelling
    };

    // Called on the installing thread, after every chunk (a few MB), so keep
    // it cheap: store the numbers and let the UI thread draw them.
    using ProgressCallback = std::function<void(const Progress&)>;

    // Is this the supported game and version? Hashes default.xex (4 MB) and
    // reads its title ID: WrongGame (another title, or an original Xbox disc),
    // UnsupportedVersion (Most Wanted, another default.xex; foundHash says
    // which), NotADiscImage (no default.xex), ReadError. `identity`, if given,
    // receives what was found.
    Result CheckGameVersion(const DiscSource& source, const Manifest& manifest = GameManifest(),
        XexIdentity* identity = nullptr);

    // Everything short of hashing the data: CheckGameVersion, then every
    // manifest file present with the right size (MissingFile/CorruptFile name
    // the first bad file and count the rest), and not cut off (TruncatedImage).
    // Files the manifest doesn't list ($SystemUpdate/, ...) are ignored.
    Result CheckSource(const DiscSource& source, const Manifest& manifest = GameManifest());

    // Copies the manifest's files from `source` into `destination`, which must
    // not exist, be empty (hidden files such as .DS_Store aside), or hold a
    // previous install. A previous install is replaced only after the new one
    // is complete, and only its game files go: everything else in it (dlc/,
    // a disc image, the player's own files, even beside the game's in NFS/)
    // is moved into the new install. Runs CheckDestination's checks first,
    // then holds a lock on the destination's parent folder, so a second
    // Install into the same place fails (DestinationNotWritable) instead of
    // colliding. Stops between chunks when `cancel` becomes true (Cancelled;
    // the partial copy is deleted).
    Result Install(const DiscSource& source, const std::filesystem::path& destination,
        const ProgressCallback& progress = {}, const std::atomic<bool>* cancel = nullptr,
        const Manifest& manifest = GameManifest());

    struct DestinationInfo
    {
        std::filesystem::path path;         // the game folder: absolute, links resolved
        uint64_t required = 0;              // free space Install needs (the files plus a margin)
        uint64_t available = UINT64_MAX;    // free space there now; UINT64_MAX if it can't be told
        bool replacing = false;             // a previous install is there (replaced when the new one is done)
    };

    // Whether Install could use `destination`, without changing anything on
    // disk, so a confirm page can show the problem (or the space needed and
    // free) before the Install button is pressed: DestinationNotEmpty (other
    // files there, a file, or a leftover .partial/.previous that isn't the
    // installer's), DestinationNotWritable (no permission, a file in the path,
    // the top of a drive, a link to something missing), NotEnoughSpace.
    // `info`, if given, is filled as far as the checks got. Install repeats
    // these checks itself.
    Result CheckDestination(const std::filesystem::path& destination, DestinationInfo* info = nullptr,
        const Manifest& manifest = GameManifest());

    // Finishes a reinstall that was interrupted (a crash or power cut) while
    // the old install sat at "<destination>.previous": puts it back if nothing
    // took its place, or retires it (carrying the player's files over) if the
    // new install made it. Install and FindGameInstall call it; one stat when
    // there is nothing to do, and a no-op while an Install is running there.
    void RecoverInterruptedInstall(const std::filesystem::path& destination, const Manifest& manifest = GameManifest());

    // Hashes every manifest file in `source` without copying: checks an image
    // before installing, or an installed folder ("verify game files").
    Result Verify(const DiscSource& source, const ProgressCallback& progress = {},
        const std::atomic<bool>* cancel = nullptr, const Manifest& manifest = GameManifest());

    // Where Install puts the game by default: GetUserPath()/game.
    std::filesystem::path DefaultInstallPath();

    // The marker Install writes into the game directory last: it records the
    // manifest version the files were checked against. A dot file, so file
    // managers hide it; the guest would still see it in a "*" listing of the
    // disc root (NtQueryDirectoryFile doesn't filter), which is harmless.
    inline constexpr const char* kInstallMarker = ".nfsmw-install";

    // The manifest version recorded in `dir`'s marker, if there is one.
    std::optional<std::string> ReadInstallMarker(const std::filesystem::path& dir);
}
