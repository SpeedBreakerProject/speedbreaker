// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Windows' own places for things (Windows only): the shell's known folders,
// the drives, and OneDrive's cloud-only files. (macOS and Linux build these
// from HOME, /Volumes and /run/media in user/paths.h and install/locate.cpp.)
#pragma once
#ifdef _WIN32
#include <filesystem>
#include <string>
#include <vector>

namespace platform
{
    enum class KnownFolder
    {
        LocalAppData,  // %LOCALAPPDATA%: per user, never roams
        Profile,       // %USERPROFILE%
        Downloads,
        Desktop,       // follows OneDrive's folder backup where that's on
        Documents,
    };
    // Where the shell keeps it (SHGetKnownFolderPath); empty if it can't say.
    std::filesystem::path GetKnownFolder(KnownFolder folder);

    struct Drive
    {
        std::filesystem::path root;  // "D:\"
        std::string label;           // the volume's name, or "D:"
        bool removable = false;      // a USB stick or an SD card
        bool readOnly = false;
        bool system = false;         // the drive Windows runs from
    };
    // The local drives with a file system: fixed and removable ones (no
    // optical drives, network shares or RAM disks).
    std::vector<Drive> Drives();

    // A file that is only in the cloud (OneDrive's Files On-Demand and the
    // like): opening it would start a download.
    bool IsCloudOnly(const std::filesystem::path& file);
}
#endif
