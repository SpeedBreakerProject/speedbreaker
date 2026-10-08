// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Guest path -> host path.
//   \Device\Cdrom0\..., game:\..., d:\...     disc (GetGamePath()), read-only
//   cache:\..., \Device\Harddisk0\Cache0\...  <user>/cache
//   hdd:\..., \Device\Harddisk0\Partition1\...<user>/hdd
//   <root>:\...                               a root registered through XAM
//                                             (XamContentCreateEx: saves, ...)
// Lookups are case-insensitive, component by component, because the disc's
// names are upper case and Linux file systems are case-sensitive.
#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace vfs
{
    struct Resolved
    {
        std::filesystem::path host;
        bool readOnly;   // the disc
    };

    // `mustExist` = false resolves the parent directory and keeps the last
    // component as given (for creating files).
    std::optional<Resolved> Resolve(std::string_view guestPath, bool mustExist);

    // Case-insensitive wildcard match (* and ?), as used by NtQueryDirectoryFile.
    bool WildcardMatch(std::string_view pattern, std::string_view name);
}
