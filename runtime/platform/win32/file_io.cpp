// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See platform/file_io.h.
#include <platform/file_io.h>
#include <platform/win32/errors.h>
#include <platform/os.h>

#include <sddl.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <format>
#include <iterator>
#include <string>

namespace platform
{
    namespace
    {
        HANDLE HandleOf(int fd)
        {
            return reinterpret_cast<HANDLE>(_get_osfhandle(fd));
        }

        int64_t ToUnixNs(const FILETIME& ft)
        {
            constexpr int64_t kEpochDelta = 116444736000000000;  // 1601 to 1970 in 100 ns
            return ((int64_t(ft.dwHighDateTime) << 32 | ft.dwLowDateTime) - kEpochDelta) * 100;
        }

        void Fill(const BY_HANDLE_FILE_INFORMATION& b, DWORD fileType, FileInfo& info)
        {
            info.directory = (b.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            info.regular = !info.directory && fileType == FILE_TYPE_DISK;
            info.size = info.directory ? 0 : uint64_t(b.nFileSizeHigh) << 32 | b.nFileSizeLow;
            int64_t ns = ToUnixNs(b.ftLastWriteTime);
            info.modifiedSeconds = ns / 1000000000;
            info.modifiedNanoseconds = ns % 1000000000;
            info.device = b.dwVolumeSerialNumber;
            info.inode = uint64_t(b.nFileIndexHigh) << 32 | b.nFileIndexLow;
        }

        int Fail(DWORD error)
        {
            errno = ErrnoFromWin32(error);
            return -1;
        }
    }

    int OpenFile(const std::filesystem::path& path, int flags, int mode)
    {
        int accessMode = flags & (_O_RDONLY | _O_WRONLY | _O_RDWR);
        DWORD access = accessMode == _O_WRONLY ? GENERIC_WRITE : accessMode == _O_RDWR ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ;
        DWORD disposition = OPEN_EXISTING;
        if (flags & _O_CREAT)
            disposition = (flags & _O_EXCL) ? CREATE_NEW : (flags & _O_TRUNC) ? CREATE_ALWAYS : OPEN_ALWAYS;
        else if (flags & _O_TRUNC)
            disposition = TRUNCATE_EXISTING;
        DWORD attributes = FILE_ATTRIBUTE_NORMAL;
        if ((flags & _O_CREAT) && !(mode & 0200))
            attributes = FILE_ATTRIBUTE_READONLY;
        if (flags & _O_SEQUENTIAL)
            attributes |= FILE_FLAG_SEQUENTIAL_SCAN;
        // A folder: what open(2) says about one opened for writing (for
        // reading, POSIX opens it; nothing here reads folders that way).
        DWORD existing = GetFileAttributesW(path.c_str());
        if (existing != INVALID_FILE_ATTRIBUTES && (existing & FILE_ATTRIBUTE_DIRECTORY))
        {
            errno = (flags & _O_CREAT) && (flags & _O_EXCL) ? EEXIST : EISDIR;
            return -1;
        }
        SECURITY_ATTRIBUTES security{ sizeof(SECURITY_ATTRIBUTES), nullptr, (flags & _O_NOINHERIT) ? FALSE : TRUE };
        HANDLE h = CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &security,
            disposition, attributes, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return Fail(GetLastError());
        int fd = _open_osfhandle(reinterpret_cast<intptr_t>(h), accessMode | (flags & _O_APPEND) | (flags & _O_NOINHERIT) | _O_BINARY);
        if (fd < 0)
        {
            CloseHandle(h);
            errno = EMFILE;
            return -1;
        }
        return fd;
    }

    std::FILE* OpenStdioFile(const std::filesystem::path& path, const char* mode)
    {
        wchar_t wideMode[16];
        size_t n = 0;
        for (; mode[n] && n + 1 < std::size(wideMode); n++)
            wideMode[n] = wchar_t(mode[n]);
        wideMode[n] = 0;
        return _wfopen(path.c_str(), wideMode);
    }

    int64_t ReadAt(int fd, void* buffer, size_t size, uint64_t offset)
    {
        HANDLE h = HandleOf(fd);
        if (h == INVALID_HANDLE_VALUE)
        {
            errno = EBADF;
            return -1;
        }
        OVERLAPPED at{};
        at.Offset = DWORD(offset);
        at.OffsetHigh = DWORD(offset >> 32);
        DWORD got = 0;
        if (!ReadFile(h, buffer, DWORD(std::min<size_t>(size, 1u << 30)), &got, &at))
        {
            DWORD error = GetLastError();
            return error == ERROR_HANDLE_EOF ? 0 : Fail(error);
        }
        return int64_t(got);
    }

    int64_t WriteAt(int fd, const void* buffer, size_t size, uint64_t offset)
    {
        HANDLE h = HandleOf(fd);
        if (h == INVALID_HANDLE_VALUE)
        {
            errno = EBADF;
            return -1;
        }
        OVERLAPPED at{};
        at.Offset = DWORD(offset);
        at.OffsetHigh = DWORD(offset >> 32);
        DWORD put = 0;
        if (!WriteFile(h, buffer, DWORD(std::min<size_t>(size, 1u << 30)), &put, &at))
            return Fail(GetLastError());
        return int64_t(put);
    }

    int Truncate(int fd, uint64_t size)
    {
        HANDLE h = HandleOf(fd);
        FILE_END_OF_FILE_INFO end{};
        end.EndOfFile.QuadPart = LONGLONG(size);
        if (h == INVALID_HANDLE_VALUE || !SetFileInformationByHandle(h, FileEndOfFileInfo, &end, sizeof(end)))
            return Fail(h == INVALID_HANDLE_VALUE ? ERROR_INVALID_HANDLE : GetLastError());
        return 0;
    }

    int InfoOf(int fd, FileInfo& info)
    {
        HANDLE h = HandleOf(fd);
        BY_HANDLE_FILE_INFORMATION b;
        if (h == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(h, &b))
            return Fail(h == INVALID_HANDLE_VALUE ? ERROR_INVALID_HANDLE : GetLastError());
        Fill(b, GetFileType(h), info);
        return 0;
    }

    int InfoOf(const std::filesystem::path& path, FileInfo& info)
    {
        // FILE_FLAG_BACKUP_SEMANTICS opens folders too; links (reparse
        // points) are followed, as stat(2) follows symlinks.
        HANDLE h = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return Fail(GetLastError());
        BY_HANDLE_FILE_INFORMATION b;
        bool ok = GetFileInformationByHandle(h, &b);
        DWORD error = GetLastError();
        if (ok)
            Fill(b, GetFileType(h), info);
        CloseHandle(h);
        return ok ? 0 : Fail(error);
    }

    int AtomicReplace(const std::filesystem::path& source, const std::filesystem::path& target)
    {
        if (MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return 0;
        return ErrnoFromWin32(GetLastError());
    }

    int RemoveFile(const std::filesystem::path& path)
    {
        return DeleteFileW(path.c_str()) ? 0 : Fail(GetLastError());
    }

    void SyncDirectory(const std::filesystem::path&)
    {
    }

    std::optional<uint64_t> FreeSpace(const std::filesystem::path& folder)
    {
        ULARGE_INTEGER available;
        if (!GetDiskFreeSpaceExW(folder.c_str(), &available, nullptr, nullptr))
            return std::nullopt;
        return uint64_t(available.QuadPart);
    }

    int CheckWritableFolder(const std::filesystem::path& folder, NewEntry what)
    {
        // This process, this thread and a count: the installer screen's
        // check and a running install may look at one folder at once.
        static std::atomic<uint32_t> s_count{ 0 };
        DWORD error = ERROR_ALREADY_EXISTS;
        // Another name only if this one is taken (left by a process that
        // ended between making a folder and deleting it).
        for (int attempt = 0; attempt < 4 && (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS); attempt++)
        {
            std::filesystem::path probe = folder / std::format(L".speedbreaker-write-test-{}-{}-{}", GetCurrentProcessId(),
                GetCurrentThreadId(), s_count.fetch_add(1, std::memory_order_relaxed));
            if (what == NewEntry::File)
            {
                HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                    FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
                if (h != INVALID_HANDLE_VALUE)
                {
                    CloseHandle(h);
                    return 0;
                }
            }
            else if (CreateDirectoryW(probe.c_str(), nullptr))
            {
                // Deleted at once. A scanner (Defender, the search indexer)
                // looking at the new folder can hold the deletion up for a
                // moment: wait for it a little.
                for (int i = 0; !RemoveDirectoryW(probe.c_str()); i++)
                {
                    if (i == 10)
                    {
                        fprintf(stderr, "[install] could not delete the empty test folder %s: error %lu\n",
                            probe.string().c_str(), GetLastError());
                        break;
                    }
                    Sleep(20);
                }
                return 0;
            }
            error = GetLastError();
        }
        return ErrnoFromWin32(error);
    }

    intptr_t LockFolder(const std::filesystem::path& folder, bool& busy)
    {
        busy = false;
        FileInfo info;
        if (InfoOf(folder, info) != 0 || !info.directory)
            return -1;
        // Global\: the whole machine, not this logon session (Local\ would
        // let an install over SSH and one at the desktop, or two users'
        // installs into one shared folder, run at once). Mutexes need no
        // privilege there (file mappings would). Every signed-in user may
        // wait on it and release it; nobody else may change or delete it.
        std::wstring name = std::format(L"Global\\SpeedBreaker-folder-{:08X}-{:016X}", uint32_t(info.device), info.inode);
        constexpr DWORD kAccess = SYNCHRONIZE | MUTEX_MODIFY_STATE;
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;0x001F0001;;;SY)(A;;0x001F0001;;;BA)(A;;0x001F0001;;;OW)"
            L"(A;;0x00100001;;;AU)", SDDL_REVISION_1, &descriptor, nullptr);
        SECURITY_ATTRIBUTES security{ sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE };
        HANDLE mutex = CreateMutexExW(descriptor ? &security : nullptr, name.c_str(), 0, kAccess);
        DWORD error = GetLastError();
        LocalFree(descriptor);
        if (!mutex)
        {
            // There, but not ours to open: it exists only while an install
            // holds it (or is checking it), so another install is running.
            busy = error == ERROR_ACCESS_DENIED;
            return -1;
        }
        DWORD wait = WaitForSingleObject(mutex, 0);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED)  // abandoned: its holder ended without letting go
            return reinterpret_cast<intptr_t>(mutex);
        busy = wait == WAIT_TIMEOUT;
        CloseHandle(mutex);
        return -1;
    }

    void UnlockFolder(intptr_t lock)
    {
        if (lock < 0)
            return;
        ReleaseMutex(reinterpret_cast<HANDLE>(lock));
        CloseHandle(reinterpret_cast<HANDLE>(lock));
    }
}
