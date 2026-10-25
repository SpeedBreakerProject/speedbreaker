// The platform layer's file calls (runtime/platform/file_io.h), on the
// host's own file system, in a folder given on the command line (made if
// missing; the files are removed afterwards). From the repo root:
//   clang++ -std=c++20 -O2 -pthread -Iruntime tests/file_io_test.cpp runtime/platform/posix/file_io.cpp -o build/file_io_test
//   build/file_io_test build/file_io_out
// (Windows: runtime/platform/win32/file_io.cpp; tests/CMakeLists.txt.)
// The disc image reads past 4 GB (MSVC's off_t is 32-bit), the guest file
// reads that must report a protected destination as EFAULT (imports_file's
// bounce buffer), the settings save that replaces its file every time, the
// installer's writability check and the install lock. A 5 GB sparse file
// stands in for a disc image: it uses no disk space. On Windows the
// permission checks also run as an ordinary user would (a restricted copy of
// this process's token): an SSH session is elevated, and an administrator
// passes every permission check.
#include <platform/file_io.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <platform/os.h>
#include <aclapi.h>
#include <sddl.h>
#include <winioctl.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

namespace fs = std::filesystem;

static std::string ReadAll(const fs::path& path)
{
    std::string text;
    if (FILE* f = platform::OpenStdioFile(path, "rb"))
    {
        char buffer[256];
        size_t n;
        while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0)
            text.append(buffer, n);
        fclose(f);
    }
    return text;
}

static bool WriteText(const fs::path& path, const char* text)
{
    FILE* f = platform::OpenStdioFile(path, "wb");
    if (!f)
        return false;
    bool ok = fwrite(text, 1, strlen(text), f) == strlen(text);
    return fclose(f) == 0 && ok;
}

// Marks a new, empty file sparse, so growing it costs no disk space
// (Windows only: NTFS files are not sparse unless asked).
static bool MakeSparse(int fd)
{
#ifdef _WIN32
    DWORD bytes = 0;
    return DeviceIoControl(reinterpret_cast<HANDLE>(_get_osfhandle(fd)), FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &bytes, nullptr) != 0;
#else
    (void)fd;
    return true;
#endif
}

// The installer's probes leave nothing behind.
static int Leftovers(const fs::path& dir)
{
    int count = 0;
    std::error_code ec;
    for (const fs::directory_entry& e : fs::directory_iterator(dir, ec))
        if (e.path().filename().string().starts_with(".speedbreaker-write-test"))
            count++;
    return count;
}

#ifdef _WIN32
// For a thread to impersonate (SetThreadToken): this process's token as an
// ordinary user's is, not elevated: the Administrators group deny-only,
// every privilege but traverse checking dropped, and new objects owned by
// the user rather than by Administrators. nullptr if it can't be made.
static HANDLE OrdinaryUserToken()
{
    HANDLE process = nullptr, restricted = nullptr, impersonation = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ADJUST_DEFAULT | TOKEN_ASSIGN_PRIMARY
            | TOKEN_IMPERSONATE, &process))
        return nullptr;
    BYTE admins[SECURITY_MAX_SID_SIZE];
    DWORD size = sizeof(admins);
    SID_AND_ATTRIBUTES disable{ admins, 0 };
    if (CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admins, &size)
        && CreateRestrictedToken(process, DISABLE_MAX_PRIVILEGE, 1, &disable, 0, nullptr, 0, nullptr, &restricted))
    {
        alignas(TOKEN_USER) BYTE user[256];
        DWORD n = 0;
        if (GetTokenInformation(restricted, TokenUser, user, sizeof(user), &n))
        {
            TOKEN_OWNER owner{ reinterpret_cast<TOKEN_USER*>(user)->User.Sid };
            SetTokenInformation(restricted, TokenOwner, &owner, sizeof(owner));
        }
        DuplicateToken(restricted, SecurityImpersonation, &impersonation);
        CloseHandle(restricted);
    }
    CloseHandle(process);
    return impersonation;
}

static bool IsAdministrator()
{
    BYTE admins[SECURITY_MAX_SID_SIZE];
    DWORD size = sizeof(admins);
    BOOL member = FALSE;
    return CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admins, &size)
        && CheckTokenMembership(nullptr, admins, &member) && member;
}

// Runs `f` on a thread of its own impersonating `token`.
template <typename F>
static bool RunAs(HANDLE token, F f)
{
    bool ran = false;
    std::thread t([&] {
        if (!SetThreadToken(nullptr, token))
            return;
        ran = true;
        f();
        RevertToSelf();
    });
    t.join();
    return ran;
}
#endif

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        printf("usage: %s <folder>\n", argv[0]);
        return 2;
    }
    const fs::path dir = argv[1];
    std::error_code ec;
    fs::create_directories(dir, ec);

    printf("1. create, exclusively\n");
    const fs::path file = dir / "file.bin";
    platform::RemoveFile(file);
    int fd = platform::OpenFile(file, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    CHECK(fd >= 0, "%s", strerror(errno));
    int again = platform::OpenFile(file, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    CHECK(again < 0 && errno == EEXIST, "a second exclusive create: %d, %s", again, strerror(errno));
    int missing = platform::OpenFile(dir / "missing.bin", O_RDONLY | O_CLOEXEC);
    CHECK(missing < 0 && errno == ENOENT, "a missing file: %d, %s", missing, strerror(errno));

    printf("2. positional reads and writes, sizes\n");
    CHECK(platform::WriteAt(fd, "hello", 5, 0) == 5, "%s", strerror(errno));
    char buffer[16] = {};
    CHECK(platform::ReadAt(fd, buffer, sizeof(buffer), 0) == 5 && memcmp(buffer, "hello", 5) == 0, "read back '%.5s'", buffer);
    CHECK(platform::ReadAt(fd, buffer, sizeof(buffer), 5) == 0, "a read at the end isn't 0");
    platform::FileInfo info, byPath;
    CHECK(platform::InfoOf(fd, info) == 0 && info.size == 5 && info.regular && !info.directory, "size %llu", (unsigned long long)info.size);
    CHECK(platform::InfoOf(file, byPath) == 0 && byPath.device == info.device && byPath.inode == info.inode,
        "the path and the descriptor aren't the same file");

    printf("3. past 4 GB (a 5 GB sparse file)\n");
    constexpr uint64_t kGiB = 1ull << 30;
    CHECK(platform::Truncate(fd, 0) == 0, "%s", strerror(errno));
    bool sparse = MakeSparse(fd);
    CHECK(sparse, "couldn't make the file sparse");
    if (sparse)
    {
        CHECK(platform::Truncate(fd, 5 * kGiB) == 0, "%s", strerror(errno));
        const uint64_t at = 4 * kGiB + kGiB / 2 + 3;
        CHECK(platform::WriteAt(fd, "past4GiB", 8, at) == 8, "%s", strerror(errno));
        memset(buffer, 0, sizeof(buffer));
        CHECK(platform::ReadAt(fd, buffer, 8, at) == 8 && memcmp(buffer, "past4GiB", 8) == 0, "read back '%.8s'", buffer);
        // The same bytes 4 GB lower must still be zero (a 32-bit offset would
        // have written there).
        memset(buffer, 1, sizeof(buffer));
        CHECK(platform::ReadAt(fd, buffer, 8, at - 4 * kGiB) == 8 && buffer[0] == 0 && buffer[7] == 0, "4 GB lower isn't zero");
        CHECK(platform::ReadAt(fd, buffer, 8, 5 * kGiB) == 0, "a read at 5 GB isn't the end");
        CHECK(platform::InfoOf(fd, info) == 0 && info.size == 5 * kGiB, "size %llu", (unsigned long long)info.size);
        CHECK(platform::Truncate(fd, 0) == 0, "%s", strerror(errno));
    }

    printf("4. a read into a read-only page is EFAULT, and reads nothing\n");
    CHECK(platform::WriteAt(fd, "data", 4, 0) == 4, "%s", strerror(errno));
#ifdef _WIN32
    void* page = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    DWORD oldProtect;
    VirtualProtect(page, 4096, PAGE_READONLY, &oldProtect);
#else
    void* page = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    mprotect(page, 4096, PROT_READ);
#endif
    errno = 0;
    int64_t got = platform::ReadAt(fd, page, 4, 0);
    CHECK(got < 0 && errno == EFAULT, "%lld, %s", (long long)got, strerror(errno));
    CHECK(static_cast<const char*>(page)[0] == 0, "the page was written");
    close(fd);

    printf("5. replace a file, twice (a settings save after the first)\n");
    const fs::path target = dir / "settings.toml", temp = dir / "settings.toml.tmp";
    CHECK(WriteText(target, "old") && WriteText(temp, "new 1"), "can't write the files");
    CHECK(platform::AtomicReplace(temp, target) == 0 && ReadAll(target) == "new 1" && !fs::exists(temp, ec), "first: '%s'", ReadAll(target).c_str());
    CHECK(WriteText(temp, "new 2"), "can't write the file");
    CHECK(platform::AtomicReplace(temp, target) == 0 && ReadAll(target) == "new 2" && !fs::exists(temp, ec), "second: '%s'", ReadAll(target).c_str());
    CHECK(platform::RemoveFile(target) == 0 && platform::RemoveFile(target) != 0 && errno == ENOENT, "removing: %s", strerror(errno));

    printf("6. a name that isn't ASCII\n");
    const fs::path unicode = dir / fs::path(std::u8string(u8"über-日本.txt"));
    CHECK(WriteText(unicode, "unicode") && ReadAll(unicode) == "unicode", "'%s'", ReadAll(unicode).c_str());
    platform::RemoveFile(unicode);

    printf("7. folders\n");
    CHECK(platform::InfoOf(dir, info) == 0 && info.directory && !info.regular, "the folder isn't one");
    int dirFd = platform::OpenFile(dir, O_WRONLY | O_CLOEXEC);
    CHECK(dirFd < 0 && errno == EISDIR, "writing a folder: %d, %s", dirFd, strerror(errno));
    int canFile = platform::CheckWritableFolder(dir, platform::NewEntry::File);
    int canFolder = platform::CheckWritableFolder(dir, platform::NewEntry::Folder);
    CHECK(canFile == 0 && canFolder == 0, "not writable: a file %s, a folder %s", strerror(canFile), strerror(canFolder));
    std::optional<uint64_t> space = platform::FreeSpace(dir);
    CHECK(space && *space > 0, "no free space figure");
    if (space)
        printf("   %.1f GB free\n", double(*space) / double(kGiB));

    printf("8. writability checks at the same time (the installer screen's and an install's) don't collide\n");
    {
        std::atomic<int> refused{ 0 };
        std::vector<std::thread> checkers;
        for (int t = 0; t < 4; t++)
            checkers.emplace_back([&] {
                for (int i = 0; i < 100; i++)
                    if (platform::CheckWritableFolder(dir, i % 2 ? platform::NewEntry::Folder : platform::NewEntry::File) != 0)
                        refused++;
            });
        for (std::thread& t : checkers)
            t.join();
        CHECK(refused == 0, "%d of 400 checks refused", refused.load());
        CHECK(Leftovers(dir) == 0, "%d test files or folders left behind", Leftovers(dir));
    }

    printf("9. the folder lock: a second holder is turned away until the first lets go\n");
    bool busy = true;
    intptr_t lock = platform::LockFolder(dir, busy);
    CHECK(lock >= 0 && !busy, "the first lock: %lld", (long long)lock);
#ifdef _WIN32
    {
        // In the Global namespace: an install over SSH (session 0) and one at
        // the desktop (session 1 or more) see the same lock.
        CHECK(platform::InfoOf(dir, info) == 0, "%s", strerror(errno));
        wchar_t name[96];
        swprintf(name, std::size(name), L"Global\\SpeedBreaker-folder-%08X-%016llX", uint32_t(info.device),
            (unsigned long long)info.inode);
        HANDLE global = OpenMutexW(SYNCHRONIZE, FALSE, name);
        CHECK(global != nullptr, "no lock in the Global namespace: error %lu", GetLastError());
        if (global)
            CloseHandle(global);
        // And an ordinary user's token is told it's busy, not let in.
        if (HANDLE token = OrdinaryUserToken())
        {
            bool userBusy = false;
            intptr_t userLock = -1;
            CHECK(RunAs(token, [&] { userLock = platform::LockFolder(dir, userBusy); platform::UnlockFolder(userLock); }),
                "can't impersonate: error %lu", GetLastError());
            CHECK(userLock < 0 && userBusy, "an ordinary user's lock: %lld, busy %d", (long long)userLock, userBusy);
            CloseHandle(token);
        }
    }
#endif
    std::atomic<int> state{ 0 };  // 1: refused as busy, 2: taken after the unlock
    std::thread other([&] {
        bool b = false;
        intptr_t second = platform::LockFolder(dir, b);
        if (second < 0 && b)
            state = 1;
        platform::UnlockFolder(second);
    });
    other.join();
    CHECK(state == 1, "the second lock wasn't refused as busy");
    platform::UnlockFolder(lock);
    std::thread later([&] {
        bool b = true;
        intptr_t third = platform::LockFolder(dir, b);
        if (third >= 0 && !b)
            state = 2;
        platform::UnlockFolder(third);
    });
    later.join();
    CHECK(state == 2, "the lock wasn't free after the unlock");

#ifdef _WIN32
    printf("10. a drive's root, as an ordinary user: new folders yes, new files no\n");
    {
        // A folder with C:\'s own ACL, as Windows sets it on every NTFS
        // drive's root: Authenticated Users may add folders (LC,
        // FILE_ADD_SUBDIRECTORY) but not files, and may change what is in
        // the folders they add (the inherit-only Modify).
        const fs::path root = dir / "drive-root";
        if (fs::exists(root, ec))  // left by a failed run: make it deletable again
            SetNamedSecurityInfoW(const_cast<wchar_t*>(root.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                nullptr, nullptr, nullptr);
        fs::remove_all(root, ec);
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;LC;;;AU)(A;OICIIO;SDGXGWGR;;;AU)(A;OICI;FA;;;SY)"
            L"(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)", SDDL_REVISION_1, &descriptor, nullptr);
        SECURITY_ATTRIBUTES security{ sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE };
        bool made = descriptor && CreateDirectoryW(root.c_str(), &security);
        CHECK(made, "can't make %s: error %lu", root.string().c_str(), GetLastError());
        LocalFree(descriptor);
        HANDLE token = OrdinaryUserToken();
        CHECK(token != nullptr, "no ordinary user's token: error %lu", GetLastError());
        if (made && token)
        {
            bool admin = true;
            int folderError = -1, fileError = -1;
            CHECK(RunAs(token, [&] {
                admin = IsAdministrator();
                folderError = platform::CheckWritableFolder(root, platform::NewEntry::Folder);
                fileError = platform::CheckWritableFolder(root, platform::NewEntry::File);
            }), "can't impersonate: error %lu", GetLastError());
            printf("   this process %s an administrator; the restricted token %s\n", IsAdministrator() ? "is" : "isn't",
                admin ? "still is (no test)" : "isn't");
            CHECK(!admin, "the restricted token is still an administrator's");
            CHECK(folderError == 0, "a new folder (what the installer makes there) was refused: %s", strerror(folderError));
            CHECK(fileError == EACCES, "a new file: %d (%s), but the ACL refuses one", fileError, strerror(fileError));
            CHECK(Leftovers(root) == 0 && fs::is_empty(root, ec), "the checks left something behind");
        }
        if (token)
            CloseHandle(token);
        // A NULL DACL (this process owns the folder, so it may set one): then
        // anyone may delete it.
        SetNamedSecurityInfoW(const_cast<wchar_t*>(root.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
            nullptr, nullptr, nullptr);
        fs::remove_all(root, ec);
        CHECK(!fs::exists(root, ec), "can't remove %s", root.string().c_str());
    }
#endif

    platform::RemoveFile(file);
    printf(g_failures ? "%d failure(s)\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
