// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Files as the runtime uses them, where Windows differs from POSIX. Files
// are C runtime descriptors everywhere (read, write, close and fsync work on
// them as they are), but on Windows they are opened through CreateFileW:
// wide paths (std::filesystem::path::c_str() is wchar_t there), binary,
// and shared for reading, writing and deleting as POSIX files are. Offsets
// and sizes are 64-bit (MSVC's off_t and struct stat sizes are 32-bit, and
// a disc image is 7.8 GB). Failures set errno as POSIX would (Windows'
// errors mapped: ERROR_NOACCESS, a buffer the kernel can't write, is EFAULT
// as on Linux), so strerror(errno) explains them on every platform.
//
// On macOS, iOS and Linux each call is the POSIX call the runtime made
// before (posix/file_io.cpp), unchanged.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>

namespace platform
{
    // open(2): O_RDONLY/O_WRONLY/O_RDWR with O_CREAT, O_EXCL, O_TRUNC,
    // O_APPEND, O_CLOEXEC (and on Windows _O_SEQUENTIAL for a front-to-back
    // read). A descriptor, or -1 with errno.
    int OpenFile(const std::filesystem::path& path, int flags, int mode = 0644);

    // fopen(3) of a path (Windows: _wfopen, so any file name works).
    std::FILE* OpenStdioFile(const std::filesystem::path& path, const char* mode);

    // pread/pwrite at a 64-bit offset: the bytes moved (a read returns fewer
    // only at the end of the file, 0 there), or -1 with errno.
    int64_t ReadAt(int fd, void* buffer, size_t size, uint64_t offset);
    int64_t WriteAt(int fd, const void* buffer, size_t size, uint64_t offset);
    // ftruncate to a 64-bit size: 0, or -1 with errno.
    int Truncate(int fd, uint64_t size);

    // fstat / stat (links followed), with 64-bit sizes. `device` and `inode`
    // name the file for loop detection (Windows: the volume serial number
    // and the file index).
    struct FileInfo
    {
        uint64_t size = 0;
        bool directory = false;
        bool regular = false;
        int64_t modifiedSeconds = 0;  // since 1970
        int64_t modifiedNanoseconds = 0;
        uint64_t device = 0;
        uint64_t inode = 0;
    };
    int InfoOf(int fd, FileInfo& info);
    int InfoOf(const std::filesystem::path& path, FileInfo& info);

    // rename(2): `target` becomes `source` in one step (Windows: MoveFileExW,
    // replacing an existing file, written through). 0, or errno.
    int AtomicReplace(const std::filesystem::path& source, const std::filesystem::path& target);
    // unlink(2): 0, or -1 with errno.
    int RemoveFile(const std::filesystem::path& path);
    // fsync of a folder, so a rename in it survives a power cut (some file
    // systems refuse it: best effort). Windows: NTFS logs the rename itself,
    // so nothing.
    void SyncDirectory(const std::filesystem::path& dir);

    // Bytes free for this user on the drive holding `folder`; nullopt when
    // it can't be told.
    std::optional<uint64_t> FreeSpace(const std::filesystem::path& folder);

    // Whether a new file, or a new folder, can be made in `folder`: 0, or
    // errno. Ask for what the caller will really make. POSIX: access(W_OK |
    // X_OK), the one permission that covers both. Windows: one of them is
    // made and deleted again (under a name no other check uses at the same
    // time), since access() there ignores the folder's permissions (ACLs),
    // and there the two are separate rights: the root of an NTFS drive (C:\,
    // and a new data drive the same) lets an ordinary user add folders but
    // not files. (An elevated process, such as an SSH session, may do both.)
    enum class NewEntry
    {
        File,    // a bug report on the Desktop
        Folder,  // the installer's <dest>.partial beside its destination
    };
    int CheckWritableFolder(const std::filesystem::path& folder, NewEntry what);

    // An exclusive lock on a folder, held until UnlockFolder (or the process
    // ends), for one install at a time per folder. A lock (>= 0); -1 if it
    // can't be locked, with `busy` set when another process holds it.
    // POSIX: flock on the folder, for the whole system. Windows: a named
    // mutex for the folder's volume and file index, in the Global namespace
    // so that every logon session (an SSH session is session 0; each
    // desktop user has their own) sees the same one, and openable by every
    // signed-in user (no file is left in the folder).
    intptr_t LockFolder(const std::filesystem::path& folder, bool& busy);
    void UnlockFolder(intptr_t lock);
}
