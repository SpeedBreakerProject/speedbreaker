// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// File system exports. Semantics, info classes and struct layouts follow
// Xenia's xboxkrnl_io.cc / xboxkrnl_io_info.cc / info/file.h / info/volume.h.
// All I/O completes synchronously (as in Xenia); for files opened for
// asynchronous I/O the call still reports STATUS_PENDING after filling in
// the status block and signalling the event.
#include <stdafx.h>
#include <cerrno>
#include <kernel/write_watch.h>
#include <report/report.h>
#include "dispatcher.h"
#include "function.h"
#include "vfs.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
    constexpr uint32_t STATUS_NO_MORE_FILES = 0x80000006;
    constexpr uint32_t STATUS_BUFFER_OVERFLOW_ = 0x80000005;
    constexpr uint32_t STATUS_INVALID_INFO_CLASS = 0xC0000003;
    constexpr uint32_t STATUS_INFO_LENGTH_MISMATCH = 0xC0000004;
    constexpr uint32_t STATUS_OBJECT_NAME_COLLISION = 0xC0000035;
    constexpr uint32_t STATUS_FILE_IS_A_DIRECTORY = 0xC00000BA;
    constexpr uint32_t STATUS_NOT_A_DIRECTORY = 0xC0000103;

    // Creation dispositions.
    enum : uint32_t { FILE_SUPERSEDE, FILE_OPEN, FILE_CREATE, FILE_OPEN_IF, FILE_OVERWRITE, FILE_OVERWRITE_IF };
    // IO_STATUS_BLOCK.Information after a create.
    enum : uint32_t { FILE_SUPERSEDED, FILE_OPENED, FILE_CREATED, FILE_OVERWRITTEN, FILE_EXISTS, FILE_DOES_NOT_EXIST };
    // Create options.
    constexpr uint32_t FILE_DIRECTORY_FILE = 0x01;
    constexpr uint32_t FILE_SYNCHRONOUS_IO_ALERT = 0x10;
    constexpr uint32_t FILE_SYNCHRONOUS_IO_NONALERT = 0x20;
    constexpr uint32_t FILE_NON_DIRECTORY_FILE = 0x40;
    // Access.
    constexpr uint32_t GENERIC_WRITE_ = 0x40000000;
    constexpr uint32_t FILE_WRITE_DATA = 0x0002;
    constexpr uint32_t FILE_APPEND_DATA = 0x0004;
    // Attributes.
    constexpr uint32_t ATTR_READONLY = 0x01;
    constexpr uint32_t ATTR_DIRECTORY = 0x10;
    constexpr uint32_t ATTR_NORMAL = 0x80;

    constexpr int64_t FILETIME_EPOCH_DIFFERENCE = 116444736000000000LL;

    struct HostAttributes
    {
        uint64_t size = 0;
        uint64_t writeTime = 0;
        uint32_t attributes = 0;
    };

    std::optional<HostAttributes> Stat(const std::filesystem::path& path, bool readOnly)
    {
        struct stat st;
        if (stat(path.c_str(), &st) != 0)
            return std::nullopt;
        HostAttributes a;
        bool dir = S_ISDIR(st.st_mode);
        a.size = dir ? 0 : uint64_t(st.st_size);
#ifdef __APPLE__
        int64_t sec = st.st_mtimespec.tv_sec, nsec = st.st_mtimespec.tv_nsec;
#else
        int64_t sec = st.st_mtim.tv_sec, nsec = st.st_mtim.tv_nsec;
#endif
        a.writeTime = uint64_t(sec * 10000000 + nsec / 100 + FILETIME_EPOCH_DIFFERENCE);
        a.attributes = dir ? ATTR_DIRECTORY : ATTR_NORMAL;
        if (readOnly)
            a.attributes |= ATTR_READONLY;
        return a;
    }

    uint64_t AllocationSize(uint64_t size)
    {
        return (size + 0xFFF) & ~uint64_t(0xFFF);
    }
}

struct X_FILE_NETWORK_OPEN_INFORMATION
{
    be<uint64_t> creationTime;
    be<uint64_t> lastAccessTime;
    be<uint64_t> lastWriteTime;
    be<uint64_t> changeTime;
    be<uint64_t> allocationSize;
    be<uint64_t> endOfFile;
    be<uint32_t> attributes;
    be<uint32_t> pad;
};
static_assert(sizeof(X_FILE_NETWORK_OPEN_INFORMATION) == 56);

struct X_FILE_DIRECTORY_INFORMATION
{
    be<uint32_t> nextEntryOffset;
    be<uint32_t> fileIndex;
    be<uint64_t> creationTime;
    be<uint64_t> lastAccessTime;
    be<uint64_t> lastWriteTime;
    be<uint64_t> changeTime;
    be<uint64_t> endOfFile;
    be<uint64_t> allocationSize;
    be<uint32_t> attributes;
    be<uint32_t> fileNameLength;
    char fileName[1];
};
static_assert(offsetof(X_FILE_DIRECTORY_INFORMATION, fileName) == 0x40);

// Signaled at all times: every operation has completed by the time it
// returns, so waiting on the file handle never blocks.
struct FileObject final : Waitable
{
    int fd = -1;
    std::filesystem::path path;
    std::string guestPath;
    bool readOnly = false;
    bool isDirectory = false;
    bool synchronous = true;
    uint64_t position = 0;
    // Directory enumeration.
    std::vector<std::filesystem::path> entries;
    size_t nextEntry = 0;
    std::string pattern;

    ~FileObject() override
    {
        if (fd >= 0)
            close(fd);
    }

    bool IsSignaledLocked() const override { return true; }

    void FillNetworkOpenInformation(X_FILE_NETWORK_OPEN_INFORMATION* info) const
    {
        auto a = Stat(path, readOnly).value_or(HostAttributes{});
        info->creationTime = a.writeTime;
        info->lastAccessTime = a.writeTime;
        info->lastWriteTime = a.writeTime;
        info->changeTime = a.writeTime;
        info->allocationSize = AllocationSize(a.size);
        info->endOfFile = a.size;
        info->attributes = a.attributes;
        info->pad = 0;
    }
};

namespace
{
    FileObject* FileFromHandle(uint32_t handle)
    {
        if (handle == GUEST_INVALID_HANDLE_VALUE || !IsKernelObject(handle))
            return nullptr;
        return dynamic_cast<FileObject*>(GetKernelObject(handle));
    }

    std::string GuestString(const XANSI_STRING* s)
    {
        if (s == nullptr || s->Buffer.get() == nullptr)
            return {};
        return std::string(s->Buffer.get(), uint16_t(s->Length));
    }

    std::optional<std::string> ObjectPath(const XOBJECT_ATTRIBUTES* attributes)
    {
        std::string name = GuestString(attributes->Name.get());
        uint32_t root = attributes->RootDirectory;
        if (root != 0 && root != 0xFFFFFFFD)  // 0xFFFFFFFD = ObDosDevices
        {
            FileObject* dir = FileFromHandle(root);
            if (dir == nullptr)
                return std::nullopt;
            return dir->guestPath + "\\" + name;
        }
        return name;
    }

    void Complete(XIO_STATUS_BLOCK* iosb, uint32_t status, uint32_t information)
    {
        if (iosb)
        {
            iosb->Status = status;
            iosb->Information = information;
        }
    }

    void SignalEvent(uint32_t eventHandle)
    {
        if (eventHandle == 0 || !IsKernelObject(eventHandle))
            return;
        if (auto* e = dynamic_cast<EventObject*>(GetKernelObject(eventHandle)))
            e->Set();
    }

    void CheckNoApc(uint32_t apcRoutine, const char* who)
    {
        if (apcRoutine & ~1u)
        {
            fprintf(stderr, "[file] %s with an APC completion routine (%08X) is not supported yet\n", who, apcRoutine);
            assert(false && "I/O APC routines are not implemented");
        }
    }
}

uint32_t NtCreateFile(be<uint32_t>* handleOut, uint32_t desiredAccess, XOBJECT_ATTRIBUTES* attributes,
    XIO_STATUS_BLOCK* iosb, be<uint64_t>* allocationSize, uint32_t fileAttributes, uint32_t shareAccess,
    uint32_t disposition, uint32_t createOptions)
{
    if (attributes == nullptr || handleOut == nullptr)
        return STATUS_INVALID_PARAMETER;
    *handleOut = GUEST_INVALID_HANDLE_VALUE;

    auto guestPath = ObjectPath(attributes);
    if (!guestPath)
    {
        Complete(iosb, STATUS_INVALID_HANDLE, 0);
        return STATUS_INVALID_HANDLE;
    }

    bool wantsWrite = (desiredAccess & (GENERIC_WRITE_ | FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0;
    bool mayCreate = disposition != FILE_OPEN && disposition != FILE_OVERWRITE;
    auto resolved = vfs::Resolve(*guestPath, !mayCreate);
    if (!resolved)
    {
        fprintf(stderr, "[file] open \"%s\": not found\n", guestPath->c_str());
        Complete(iosb, STATUS_OBJECT_NAME_NOT_FOUND, FILE_DOES_NOT_EXIST);
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    std::error_code ec;
    bool exists = std::filesystem::exists(resolved->host, ec);
    bool isDir = exists && std::filesystem::is_directory(resolved->host, ec);
    bool wantDir = (createOptions & FILE_DIRECTORY_FILE) != 0;

    auto fail = [&](uint32_t status, uint32_t info) {
        fprintf(stderr, "[file] open \"%s\" failed: %08X\n", guestPath->c_str(), status);
        Complete(iosb, status, info);
        return status;
    };

    if (exists && disposition == FILE_CREATE)
        return fail(STATUS_OBJECT_NAME_COLLISION, FILE_EXISTS);
    if (!exists && (disposition == FILE_OPEN || disposition == FILE_OVERWRITE))
        return fail(STATUS_OBJECT_NAME_NOT_FOUND, FILE_DOES_NOT_EXIST);
    if (isDir && (createOptions & FILE_NON_DIRECTORY_FILE))
        return fail(STATUS_FILE_IS_A_DIRECTORY, 0);
    if (exists && !isDir && wantDir)
        return fail(STATUS_NOT_A_DIRECTORY, 0);
    if (resolved->readOnly && (wantsWrite || !exists))
        return fail(STATUS_ACCESS_DENIED, 0);

    uint32_t action = FILE_OPENED;
    auto* file = CreateKernelObject<FileObject>();
    file->path = resolved->host;
    file->guestPath = *guestPath;
    file->readOnly = resolved->readOnly;
    file->synchronous = (createOptions & (FILE_SYNCHRONOUS_IO_ALERT | FILE_SYNCHRONOUS_IO_NONALERT)) != 0;

    if (wantDir || isDir)
    {
        if (!exists)
        {
            std::filesystem::create_directories(resolved->host, ec);
            action = FILE_CREATED;
        }
        file->isDirectory = true;
    }
    else
    {
        int flags = wantsWrite ? O_RDWR : O_RDONLY;
        if (!exists)
        {
            flags |= O_CREAT;
            action = FILE_CREATED;
        }
        else if (disposition == FILE_SUPERSEDE || disposition == FILE_OVERWRITE || disposition == FILE_OVERWRITE_IF)
        {
            flags |= O_TRUNC | O_RDWR;
            action = disposition == FILE_SUPERSEDE ? FILE_SUPERSEDED : FILE_OVERWRITTEN;
        }
        file->fd = open(resolved->host.c_str(), flags, 0644);
        if (file->fd < 0)
        {
            DestroyKernelObject(file);
            return fail(STATUS_ACCESS_DENIED, 0);
        }
    }

    *handleOut = GetKernelHandle(file);
    fprintf(stderr, "[file] open \"%s\" -> %s%s\n", guestPath->c_str(), resolved->host.c_str(),
        action == FILE_CREATED ? " (created)" : "");
    Complete(iosb, STATUS_SUCCESS, action);
    return STATUS_SUCCESS;
}

uint32_t NtOpenFile(be<uint32_t>* handleOut, uint32_t desiredAccess, XOBJECT_ATTRIBUTES* attributes,
    XIO_STATUS_BLOCK* iosb, uint32_t openOptions)
{
    return NtCreateFile(handleOut, desiredAccess, attributes, iosb, nullptr, 0, 0, FILE_OPEN, openOptions);
}

uint32_t NtReadFile(uint32_t fileHandle, uint32_t eventHandle, uint32_t apcRoutine, uint32_t apcContext,
    XIO_STATUS_BLOCK* iosb, void* buffer, uint32_t length, be<int64_t>* byteOffset)
{
    FileObject* file = FileFromHandle(fileHandle);
    if (file == nullptr || file->isDirectory)
    {
        Complete(iosb, STATUS_INVALID_HANDLE, 0);
        return STATUS_INVALID_HANDLE;
    }
    CheckNoApc(apcRoutine, "NtReadFile");

    // A byte offset of -2 (FILE_USE_FILE_POINTER_POSITION) or none: current position.
    int64_t offset = byteOffset ? int64_t(*byteOffset) : -1;
    uint64_t pos = offset >= 0 ? uint64_t(offset) : file->position;
    writewatch::BeforeHostWrite(buffer, length);  // the kernel can't fault on watched pages
    // Another thread can protect a page of the buffer again between
    // BeforeHostWrite and the read (a texture's write watch, the GPU guard):
    // the kernel then stops short at that page, or fails with EFAULT, instead
    // of faulting. Any shortfall before end of file is read again, through a
    // bounce buffer if the kernel refuses the guest memory, and copied with
    // ordinary stores, which fault and are handled like any CPU write. (The
    // game treats a failed read as fatal: its error callback is null, a crash
    // at lr 82621950.)
    uint8_t* dst = static_cast<uint8_t*>(buffer);
    ssize_t n = 0;
    while (uint32_t(n) < length)
    {
        ssize_t got = pread(file->fd, dst + n, length - uint32_t(n), off_t(pos + uint64_t(n)));
        if (got < 0 && errno == EFAULT)
        {
            static thread_local std::vector<uint8_t> bounce;
            bounce.resize(length - uint32_t(n));
            do
                got = pread(file->fd, bounce.data(), bounce.size(), off_t(pos + uint64_t(n)));
            while (got < 0 && errno == EINTR);
            if (got > 0)
                memcpy(dst + n, bounce.data(), size_t(got));
        }
        if (got < 0 && errno == EINTR)
            continue;
        if (got < 0)
        {
            fprintf(stderr, "[file] read of \"%s\" failed: %s (offset %llu, %u bytes)\n", file->guestPath.c_str(),
                strerror(errno), (unsigned long long)(pos + uint64_t(n)), length - uint32_t(n));
            Complete(iosb, STATUS_UNSUCCESSFUL, 0);
            return STATUS_UNSUCCESSFUL;
        }
        if (got == 0)
            break;  // end of file
        n += got;
    }
    file->position = pos + uint64_t(n);
    report::g_fileBytesRead.fetch_add(uint64_t(n), std::memory_order_relaxed);  // the hang watchdog's "loading"

    uint32_t status = (n == 0 && length != 0) ? STATUS_END_OF_FILE : STATUS_SUCCESS;
    Complete(iosb, status, uint32_t(n));
    SignalEvent(eventHandle);
    if (status == STATUS_SUCCESS && !file->synchronous)
        return STATUS_PENDING;
    return status;
}

uint32_t NtWriteFile(uint32_t fileHandle, uint32_t eventHandle, uint32_t apcRoutine, uint32_t apcContext,
    XIO_STATUS_BLOCK* iosb, const void* buffer, uint32_t length, be<int64_t>* byteOffset)
{
    FileObject* file = FileFromHandle(fileHandle);
    if (file == nullptr || file->isDirectory)
    {
        Complete(iosb, STATUS_INVALID_HANDLE, 0);
        return STATUS_INVALID_HANDLE;
    }
    if (file->readOnly)
    {
        Complete(iosb, STATUS_ACCESS_DENIED, 0);
        return STATUS_ACCESS_DENIED;
    }
    CheckNoApc(apcRoutine, "NtWriteFile");

    int64_t offset = byteOffset ? int64_t(*byteOffset) : -1;
    uint64_t pos = offset >= 0 ? uint64_t(offset) : file->position;
    ssize_t n = pwrite(file->fd, buffer, length, off_t(pos));
    if (n < 0)
    {
        Complete(iosb, STATUS_UNSUCCESSFUL, 0);
        return STATUS_UNSUCCESSFUL;
    }
    file->position = pos + uint64_t(n);
    Complete(iosb, STATUS_SUCCESS, uint32_t(n));
    SignalEvent(eventHandle);
    return file->synchronous ? STATUS_SUCCESS : STATUS_PENDING;
}

uint32_t NtFlushBuffersFile(uint32_t fileHandle, XIO_STATUS_BLOCK* iosb)
{
    if (FileObject* file = FileFromHandle(fileHandle); file && file->fd >= 0 && !file->readOnly)
        fsync(file->fd);
    Complete(iosb, STATUS_SUCCESS, 0);
    return STATUS_SUCCESS;
}

// Info classes (X_FILE_INFORMATION_CLASS).
enum : uint32_t
{
    XFileBasicInformation = 4,
    XFileStandardInformation = 5,
    XFileInternalInformation = 6,
    XFileDispositionInformation = 13,
    XFilePositionInformation = 14,
    XFileModeInformation = 16,
    XFileAlignmentInformation = 17,
    XFileAllocationInformation = 19,
    XFileEndOfFileInformation = 20,
    XFileSectorInformation = 26,
    XFileIoPriorityInformation = 32,
    XFileNetworkOpenInformation = 34,
};

uint32_t NtQueryInformationFile(uint32_t fileHandle, XIO_STATUS_BLOCK* iosb, uint8_t* info, uint32_t length,
    uint32_t infoClass)
{
    FileObject* file = FileFromHandle(fileHandle);
    if (file == nullptr)
        return STATUS_INVALID_HANDLE;

    uint32_t written = 0;
    switch (infoClass)
    {
    case XFileInternalInformation:
    {
        if (length < 8) return STATUS_INFO_LENGTH_MISMATCH;
        *reinterpret_cast<be<uint64_t>*>(info) = std::hash<std::string>{}(file->path.string());
        written = 8;
        break;
    }
    case XFilePositionInformation:
    {
        if (length < 8) return STATUS_INFO_LENGTH_MISMATCH;
        *reinterpret_cast<be<uint64_t>*>(info) = file->position;
        written = 8;
        break;
    }
    case XFileNetworkOpenInformation:
    {
        if (length < sizeof(X_FILE_NETWORK_OPEN_INFORMATION)) return STATUS_INFO_LENGTH_MISMATCH;
        file->FillNetworkOpenInformation(reinterpret_cast<X_FILE_NETWORK_OPEN_INFORMATION*>(info));
        written = sizeof(X_FILE_NETWORK_OPEN_INFORMATION);
        break;
    }
    case XFileStandardInformation:
    {
        // AllocationSize, EndOfFile (8 each), NumberOfLinks (4), DeletePending, Directory.
        if (length < 22) return STATUS_INFO_LENGTH_MISMATCH;
        memset(info, 0, 24 <= length ? 24 : length);
        auto a = Stat(file->path, file->readOnly).value_or(HostAttributes{});
        reinterpret_cast<be<uint64_t>*>(info)[0] = AllocationSize(a.size);
        reinterpret_cast<be<uint64_t>*>(info)[1] = a.size;
        *reinterpret_cast<be<uint32_t>*>(info + 16) = 1;
        info[20] = 0;
        info[21] = file->isDirectory ? 1 : 0;
        written = 24;
        break;
    }
    case XFileAlignmentInformation:
    case XFileModeInformation:
    {
        if (length < 4) return STATUS_INFO_LENGTH_MISMATCH;
        *reinterpret_cast<be<uint32_t>*>(info) = 0;
        written = 4;
        break;
    }
    default:
        fprintf(stderr, "[file] NtQueryInformationFile: info class %u not supported yet\n", infoClass);
        assert(false && "unsupported NtQueryInformationFile class");
        return STATUS_INVALID_INFO_CLASS;
    }
    Complete(iosb, STATUS_SUCCESS, written);
    return STATUS_SUCCESS;
}

uint32_t NtSetInformationFile(uint32_t fileHandle, XIO_STATUS_BLOCK* iosb, uint8_t* info, uint32_t length,
    uint32_t infoClass)
{
    FileObject* file = FileFromHandle(fileHandle);
    if (file == nullptr)
        return STATUS_INVALID_HANDLE;

    uint32_t status = STATUS_SUCCESS, written = 0;
    switch (infoClass)
    {
    case XFilePositionInformation:
        if (length < 8) return STATUS_INFO_LENGTH_MISMATCH;
        file->position = *reinterpret_cast<be<uint64_t>*>(info);
        written = 8;
        break;
    case XFileEndOfFileInformation:
        if (length < 8) return STATUS_INFO_LENGTH_MISMATCH;
        if (file->readOnly || file->fd < 0)
            status = STATUS_ACCESS_DENIED;
        else if (ftruncate(file->fd, off_t(uint64_t(*reinterpret_cast<be<uint64_t>*>(info)))) != 0)
            status = STATUS_UNSUCCESSFUL;
        written = 8;
        break;
    case XFileAllocationInformation:
        if (length < 8) return STATUS_INFO_LENGTH_MISMATCH;
        written = 8;  // preallocation hint: ignored (as Xenia)
        break;
    case XFileDispositionInformation:
        if (length < 1) return STATUS_INFO_LENGTH_MISMATCH;
        if (info[0] != 0)
        {
            // Delete-on-close. Xenia ignores this; log it so we notice.
            fprintf(stderr, "[file] delete-on-close requested for \"%s\" (ignored)\n", file->guestPath.c_str());
        }
        break;
    default:
        fprintf(stderr, "[file] NtSetInformationFile: info class %u not supported yet\n", infoClass);
        assert(false && "unsupported NtSetInformationFile class");
        return STATUS_INVALID_INFO_CLASS;
    }
    Complete(iosb, status, written);
    return status;
}

uint32_t NtQueryFullAttributesFile(XOBJECT_ATTRIBUTES* attributes, X_FILE_NETWORK_OPEN_INFORMATION* info)
{
    auto guestPath = ObjectPath(attributes);
    if (!guestPath)
        return STATUS_INVALID_HANDLE;
    auto resolved = vfs::Resolve(*guestPath, true);
    if (!resolved)
        return STATUS_NO_SUCH_FILE;
    auto a = Stat(resolved->host, resolved->readOnly);
    if (!a)
        return STATUS_NO_SUCH_FILE;
    info->creationTime = a->writeTime;
    info->lastAccessTime = a->writeTime;
    info->lastWriteTime = a->writeTime;
    info->changeTime = a->writeTime;
    info->allocationSize = AllocationSize(a->size);
    info->endOfFile = a->size;
    info->attributes = a->attributes;
    info->pad = 0;
    return STATUS_SUCCESS;
}

uint32_t NtQueryDirectoryFile(uint32_t fileHandle, uint32_t eventHandle, uint32_t apcRoutine, uint32_t apcContext,
    XIO_STATUS_BLOCK* iosb, X_FILE_DIRECTORY_INFORMATION* info, uint32_t length, XANSI_STRING* fileName,
    uint32_t restartScan)
{
    if (length < 72)
        return STATUS_INFO_LENGTH_MISMATCH;
    FileObject* dir = FileFromHandle(fileHandle);
    if (dir == nullptr || !dir->isDirectory)
    {
        Complete(iosb, STATUS_NO_SUCH_FILE, 0);
        return STATUS_NO_SUCH_FILE;
    }
    CheckNoApc(apcRoutine, "NtQueryDirectoryFile");

    std::string pattern = GuestString(fileName);
    // A new pattern restarts the scan (Xenia always restarts with a name).
    if (!pattern.empty() || restartScan || dir->entries.empty())
    {
        if (!pattern.empty())
            dir->pattern = pattern;
        if (dir->pattern.empty())
            dir->pattern = "*";
        dir->entries.clear();
        std::error_code ec;
        for (auto& e : std::filesystem::directory_iterator(dir->path, ec))
            if (vfs::WildcardMatch(dir->pattern, e.path().filename().string()))
                dir->entries.push_back(e.path());
        std::sort(dir->entries.begin(), dir->entries.end());
        dir->nextEntry = 0;
    }

    if (dir->nextEntry >= dir->entries.size())
    {
        uint32_t status = dir->entries.empty() && !pattern.empty() ? STATUS_NO_SUCH_FILE : STATUS_NO_MORE_FILES;
        Complete(iosb, status, 0);
        return status;
    }

    const auto& entry = dir->entries[dir->nextEntry];
    std::string name = entry.filename().string();
    if (offsetof(X_FILE_DIRECTORY_INFORMATION, fileName) + name.size() > length)
    {
        Complete(iosb, STATUS_BUFFER_OVERFLOW_, 0);
        return STATUS_BUFFER_OVERFLOW_;
    }
    dir->nextEntry++;

    auto a = Stat(entry, dir->readOnly).value_or(HostAttributes{});
    info->nextEntryOffset = 0;
    info->fileIndex = uint32_t(dir->nextEntry);
    info->creationTime = a.writeTime;
    info->lastAccessTime = a.writeTime;
    info->lastWriteTime = a.writeTime;
    info->changeTime = a.writeTime;
    info->endOfFile = a.size;
    info->allocationSize = AllocationSize(a.size);
    info->attributes = a.attributes;
    info->fileNameLength = uint32_t(name.size());
    memcpy(info->fileName, name.data(), name.size());

    Complete(iosb, STATUS_SUCCESS, length);
    SignalEvent(eventHandle);
    return STATUS_SUCCESS;
}

// Volume info classes.
enum : uint32_t { XFileFsVolumeInformation = 1, XFileFsSizeInformation = 3, XFileFsAttributeInformation = 5 };

uint32_t NtQueryVolumeInformationFile(uint32_t fileHandle, XIO_STATUS_BLOCK* iosb, uint8_t* info, uint32_t length,
    uint32_t infoClass)
{
    FileObject* file = FileFromHandle(fileHandle);
    if (file == nullptr)
        return STATUS_INVALID_HANDLE;
    memset(info, 0, length);

    uint32_t status = STATUS_SUCCESS, written = 0;
    switch (infoClass)
    {
    case XFileFsVolumeInformation:
        // creation time (8), serial (4), label length (4), supports objects (1).
        if (length < 24) return STATUS_INFO_LENGTH_MISMATCH;
        written = 17;
        break;
    case XFileFsSizeInformation:
    {
        if (length < 24) return STATUS_INFO_LENGTH_MISMATCH;
        // Report a 4 GB volume, mostly free, 16 KB clusters of 512-byte sectors
        // (Xenia asserts bytes_per_sector == 0x200 for XCTD userland code).
        auto* p = reinterpret_cast<be<uint64_t>*>(info);
        p[0] = 0x40000;   // total allocation units
        p[1] = 0x38000;   // available
        reinterpret_cast<be<uint32_t>*>(info)[4] = 32;     // sectors per unit
        reinterpret_cast<be<uint32_t>*>(info)[5] = 0x200;  // bytes per sector
        written = 24;
        break;
    }
    case XFileFsAttributeInformation:
    {
        if (length < 12) return STATUS_INFO_LENGTH_MISMATCH;
        static constexpr char name[] = "FATX";
        auto* p = reinterpret_cast<be<uint32_t>*>(info);
        p[0] = 0;       // attributes
        p[1] = 255;     // max component name length
        p[2] = 4;
        if (length >= 12 + 4)
        {
            memcpy(info + 12, name, 4);
            written = 16;
        }
        else
        {
            status = STATUS_BUFFER_OVERFLOW_;
            written = 12;
        }
        break;
    }
    default:
        fprintf(stderr, "[file] NtQueryVolumeInformationFile: info class %u not supported yet\n", infoClass);
        assert(false && "unsupported NtQueryVolumeInformationFile class");
        return STATUS_INVALID_INFO_CLASS;
    }
    Complete(iosb, status, written);
    return status;
}

GUEST_FUNCTION_HOOK(__imp__NtCreateFile, NtCreateFile);
GUEST_FUNCTION_HOOK(__imp__NtOpenFile, NtOpenFile);
GUEST_FUNCTION_HOOK(__imp__NtReadFile, NtReadFile);
GUEST_FUNCTION_HOOK(__imp__NtWriteFile, NtWriteFile);
GUEST_FUNCTION_HOOK(__imp__NtFlushBuffersFile, NtFlushBuffersFile);
GUEST_FUNCTION_HOOK(__imp__NtQueryInformationFile, NtQueryInformationFile);
GUEST_FUNCTION_HOOK(__imp__NtSetInformationFile, NtSetInformationFile);
GUEST_FUNCTION_HOOK(__imp__NtQueryFullAttributesFile, NtQueryFullAttributesFile);
GUEST_FUNCTION_HOOK(__imp__NtQueryDirectoryFile, NtQueryDirectoryFile);
GUEST_FUNCTION_HOOK(__imp__NtQueryVolumeInformationFile, NtQueryVolumeInformationFile);
