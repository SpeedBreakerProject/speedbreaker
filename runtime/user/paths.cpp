// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See paths.h.
#include <stdafx.h>
#include "paths.h"

namespace
{
    namespace fs = std::filesystem;

    // base/nfsmw-recomp -> base/<current name>, by rename (the same folder,
    // so the move is instant and all or nothing: a player's saves, settings
    // and installed game come along). Where that can't happen, `name` goes
    // back to the old name and the old folder is used as it is.
    std::string MigrateFolder(const fs::path& base, const char*& name)
    {
        fs::path from = base / kOldUserFolderName, to = base / kUserFolderName;
        std::error_code ec;
        fs::file_status old = fs::symlink_status(from, ec);
        if (ec || !fs::exists(old))
            return {};
        if (fs::is_symlink(old) && fs::read_symlink(from, ec) == fs::path(kUserFolderName))
            return {};  // (a link this left behind)
        if (fs::exists(fs::symlink_status(to, ec)))
            return std::format("[paths] {} and {} both exist: using {}, {} left as it is\n",
                from.string(), to.string(), to.string(), from.string());
        fs::rename(from, to, ec);
        if (ec)
        {
            name = kOldUserFolderName;
            return std::format("[paths] couldn't rename {} to {} ({}): using it as it is\n",
                from.string(), to.string(), ec.message());
        }
        std::error_code linkEc;
        fs::create_directory_symlink(kUserFolderName, from, linkEc);
        return std::format("[paths] renamed {} to {} (the project is SpeedBreaker now){}\n", from.string(), to.string(),
            linkEc ? std::format("; no link at the old name ({})", linkEc.message()) : ", the old name now a link to it");
    }
}

std::string MigrateUserFolders()
{
    return MigrateFolder(UserDataBase(), DataFolderName()) + MigrateFolder(CacheBase(), CacheFolderName());
}
