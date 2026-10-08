// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// iOS: the Files app's side of installing (ios_files.mm). SDL has no file
// dialog on iOS, so Browse presents the system's document picker, which opens
// the disc image where it is: 7 GB, never copied into the app. A file outside
// the app's own folder can be read only while the app holds the access the
// picker granted for it (its security scope), so a pick comes with that
// grant, held until ReleaseFileAccess.
//
// PickDiscImage, its answer and IsPickerShown: main thread. The rest: any
// thread.
#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

struct SDL_Window;

namespace platform::ios
{
    // A held grant to read a picked file; 0: none (none was needed).
    using FileAccess = uint64_t;

    struct PickedFile
    {
        std::string path;
        FileAccess access = 0;  // give it back once nothing reads the file
        // Set when the pick isn't on the device yet: a file a cloud service
        // (iCloud Drive, or another app's in the Files app) hasn't downloaded,
        // or a folder with such files in it. Reading it now would fail, or
        // wait for the whole download with nothing to say why, so it isn't
        // to be read: this says what to do instead (its download is asked
        // for), and `access` is 0, the grant already given back.
        std::string notHere;
    };

    // Presents the document picker over `window`: for a disc image or, with
    // `folder`, a folder (an extracted disc). `done` runs on the main thread
    // with the pick, or nullopt if the player cancelled. A picker still open
    // is closed first and never answers. False: it couldn't be shown.
    bool PickDiscImage(SDL_Window* window, bool folder, std::function<void(std::optional<PickedFile>)> done);

    // A picker is on screen, or on its way on or off it. UIKit won't show
    // another until it's gone, and a controller still reaches the screen
    // behind it, so nothing there should act meanwhile.
    bool IsPickerShown();

    // Stops reading a picked file under its grant. 0, or one already given
    // back: nothing.
    void ReleaseFileAccess(FileAccess access);

    // Keeps `dir` and everything in it out of iCloud and computer backups.
    // False (logged) if it couldn't be marked.
    bool ExcludeFromBackup(const std::filesystem::path& dir);

    // Free space on the volume holding `path` for something the player asked
    // for: what statvfs reports plus what iOS would purge (caches, offloaded
    // data) to make room. nullopt if it can't be told.
    std::optional<uint64_t> AvailableCapacityForImportantUsage(const std::filesystem::path& path);
}
