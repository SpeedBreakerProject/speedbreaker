// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See platform/file_io.h.
// macOS, iOS and Linux: the POSIX calls the runtime always made.
#include <platform/file_io.h>

#include <cerrno>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace platform
{
    namespace
    {
        void Fill(const struct stat& st, FileInfo& info)
        {
            info.size = uint64_t(st.st_size);
            info.directory = S_ISDIR(st.st_mode);
            info.regular = S_ISREG(st.st_mode);
#ifdef __APPLE__
            info.modifiedSeconds = st.st_mtimespec.tv_sec;
            info.modifiedNanoseconds = st.st_mtimespec.tv_nsec;
#else
            info.modifiedSeconds = st.st_mtim.tv_sec;
            info.modifiedNanoseconds = st.st_mtim.tv_nsec;
#endif
            info.device = uint64_t(st.st_dev);
            info.inode = uint64_t(st.st_ino);
        }
    }

    int OpenFile(const std::filesystem::path& path, int flags, int mode)
    {
        return open(path.c_str(), flags, mode);
    }

    std::FILE* OpenStdioFile(const std::filesystem::path& path, const char* mode)
    {
        return fopen(path.c_str(), mode);
    }

    int64_t ReadAt(int fd, void* buffer, size_t size, uint64_t offset)
    {
        static_assert(sizeof(off_t) == 8, "large file offsets are required");
        return pread(fd, buffer, size, off_t(offset));
    }

    int64_t WriteAt(int fd, const void* buffer, size_t size, uint64_t offset)
    {
        return pwrite(fd, buffer, size, off_t(offset));
    }

    int Truncate(int fd, uint64_t size)
    {
        return ftruncate(fd, off_t(size));
    }

    int InfoOf(int fd, FileInfo& info)
    {
        struct stat st;
        if (fstat(fd, &st) != 0)
            return -1;
        Fill(st, info);
        return 0;
    }

    int InfoOf(const std::filesystem::path& path, FileInfo& info)
    {
        struct stat st;
        if (stat(path.c_str(), &st) != 0)
            return -1;
        Fill(st, info);
        return 0;
    }

    int AtomicReplace(const std::filesystem::path& source, const std::filesystem::path& target)
    {
        return rename(source.c_str(), target.c_str()) == 0 ? 0 : errno;
    }

    int RemoveFile(const std::filesystem::path& path)
    {
        return unlink(path.c_str());
    }

    void SyncDirectory(const std::filesystem::path& dir)
    {
        int fd = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd >= 0)
        {
            fsync(fd);
            close(fd);
        }
    }

    std::optional<uint64_t> FreeSpace(const std::filesystem::path& folder)
    {
        struct statvfs vfs;
        if (statvfs(folder.c_str(), &vfs) != 0)
            return std::nullopt;
        return uint64_t(vfs.f_bavail) * vfs.f_frsize;
    }

    int CheckWritableFolder(const std::filesystem::path& folder, NewEntry)
    {
        return access(folder.c_str(), W_OK | X_OK) == 0 ? 0 : errno;
    }

    intptr_t LockFolder(const std::filesystem::path& folder, bool& busy)
    {
        busy = false;
        int fd = open(folder.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) != 0)
        {
            busy = errno == EWOULDBLOCK;
            close(fd);
            fd = -1;
        }
        return fd;
    }

    void UnlockFolder(intptr_t lock)
    {
        if (lock >= 0)
            close(int(lock));
    }
}
