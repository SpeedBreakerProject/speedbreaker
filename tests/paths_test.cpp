// Where the per-user folders go (runtime/user/paths.h), from the environment
// alone: nothing is created or read. From the repo root:
//   clang++ -std=c++20 -O2 -Iruntime tests/paths_test.cpp -o build/paths_test
//   build/paths_test
// (Windows: with runtime/platform/win32/folders.cpp, shell32 and ole32;
// tests/CMakeLists.txt.) macOS and Linux: the HOME-based folders, and the
// XDG_* overrides where they apply (Linux; not macOS). Windows: %LOCALAPPDATA%
// \SpeedBreaker and its Cache folder whatever HOME says (Git Bash and MSYS2
// set it), and the XDG_* overrides for test sandboxes.
#include <user/paths.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#ifdef _WIN32
#include <platform/folders.h>
#endif

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void Set(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value ? value : "");  // "" removes it
#else
    if (value)
        setenv(name, value, 1);
    else
        unsetenv(name);
#endif
}

static std::string Show(const std::filesystem::path& p)
{
    return p.string();
}

int main()
{
    namespace fs = std::filesystem;
#ifdef _WIN32
    const fs::path home = "C:\\home-from-git-bash";
    const fs::path data = "C:\\sandbox\\data", cache = "C:\\sandbox\\cache";
#else
    const fs::path home = "/tmp/paths-test-home";
    const fs::path data = "/tmp/paths-test-data", cache = "/tmp/paths-test-cache";
#endif

    printf("1. no XDG_* overrides, HOME=%s\n", home.string().c_str());
    Set("HOME", home.string().c_str());
    Set("XDG_DATA_HOME", nullptr);
    Set("XDG_CACHE_HOME", nullptr);
    printf("   data:  %s\n   cache: %s\n", Show(GetUserPath()).c_str(), Show(GetCachePath()).c_str());
#ifdef _WIN32
    fs::path local = platform::GetKnownFolder(platform::KnownFolder::LocalAppData);
    CHECK(!local.empty() && local.is_absolute(), "LocalAppData: %s", local.string().c_str());
    if (const char* env = std::getenv("LOCALAPPDATA"))
        CHECK(fs::path(env) == local, "the shell's LocalAppData %s isn't %%LOCALAPPDATA%% %s", local.string().c_str(), env);
    CHECK(GetUserPath() == local / "SpeedBreaker", "data %s", Show(GetUserPath()).c_str());
    CHECK(GetCachePath() == local / "SpeedBreaker" / "Cache", "cache %s", Show(GetCachePath()).c_str());
    CHECK(GetUserPath().string().find("home-from-git-bash") == std::string::npos, "HOME was used");
#elif defined(__APPLE__)
    CHECK(GetUserPath() == home / "Library/Application Support" / "SpeedBreaker", "data %s", Show(GetUserPath()).c_str());
    CHECK(GetCachePath() == home / "Library/Caches" / "SpeedBreaker", "cache %s", Show(GetCachePath()).c_str());
#else
    CHECK(GetUserPath() == home / ".local/share" / "speedbreaker", "data %s", Show(GetUserPath()).c_str());
    CHECK(GetCachePath() == home / ".cache" / "speedbreaker", "cache %s", Show(GetCachePath()).c_str());
#endif

    printf("2. XDG_DATA_HOME=%s XDG_CACHE_HOME=%s\n", data.string().c_str(), cache.string().c_str());
    Set("XDG_DATA_HOME", data.string().c_str());
    Set("XDG_CACHE_HOME", cache.string().c_str());
    printf("   data:  %s\n   cache: %s\n", Show(GetUserPath()).c_str(), Show(GetCachePath()).c_str());
#if defined(__APPLE__)
    // macOS ignores them (only iOS takes them); the HOME-based folders stay.
    CHECK(GetUserPath() == home / "Library/Application Support" / "SpeedBreaker", "data %s", Show(GetUserPath()).c_str());
    CHECK(GetCachePath() == home / "Library/Caches" / "SpeedBreaker", "cache %s", Show(GetCachePath()).c_str());
#else
    CHECK(GetUserPath() == data / kUserFolderName, "data %s", Show(GetUserPath()).c_str());
    CHECK(GetCachePath() == cache / kUserFolderName, "cache %s", Show(GetCachePath()).c_str());
#endif

#ifdef _WIN32
    printf("3. XDG_DATA_HOME only: the cache goes inside it (a sandbox keeps everything)\n");
    Set("XDG_CACHE_HOME", nullptr);
    printf("   cache: %s\n", Show(GetCachePath()).c_str());
    CHECK(GetCachePath() == data / "SpeedBreaker" / "Cache", "cache %s", Show(GetCachePath()).c_str());
#endif

    printf(g_failures ? "%d failure(s)\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
