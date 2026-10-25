// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
// Windows' error codes as the errno values POSIX code expects (Windows only).
#pragma once
#include <platform/os.h>

namespace platform
{
    inline int ErrnoFromWin32(DWORD error)
    {
        switch (error)
        {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_DRIVE:
        case ERROR_BAD_NETPATH:
        case ERROR_BAD_NET_NAME:
        case ERROR_INVALID_NAME:
        case ERROR_BAD_PATHNAME:
        case ERROR_DIRECTORY:
            return ENOENT;
        case ERROR_FILE_EXISTS:
        case ERROR_ALREADY_EXISTS:
            return EEXIST;
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
        case ERROR_CURRENT_DIRECTORY:
            return EACCES;
        case ERROR_WRITE_PROTECT:
            return EROFS;
        case ERROR_DISK_FULL:
        case ERROR_HANDLE_DISK_FULL:
            return ENOSPC;
        case ERROR_NOACCESS:
        case ERROR_INVALID_USER_BUFFER:
            return EFAULT;
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY:
        case ERROR_COMMITMENT_LIMIT:
            return ENOMEM;
        case ERROR_TOO_MANY_OPEN_FILES:
            return EMFILE;
        case ERROR_INVALID_HANDLE:
            return EBADF;
        case ERROR_DIR_NOT_EMPTY:
            return ENOTEMPTY;
        case ERROR_NOT_SAME_DEVICE:
            return EXDEV;
        case ERROR_BROKEN_PIPE:
        case ERROR_NO_DATA:
            return EPIPE;
        case ERROR_INVALID_PARAMETER:
            return EINVAL;
        case ERROR_FILENAME_EXCED_RANGE:
            return ENAMETOOLONG;
        default:
            return EIO;
        }
    }
}
