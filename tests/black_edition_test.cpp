// Tests for Black Edition (GitHub #100): game::black_edition's write (header
// part: the flag set only when on, nothing touched when off) and its setting
// (user/settings: Settings > Game > Black Edition, [game] black_edition in
// settings.toml, NFSMW_BLACK_EDITION; off by default, a Restart option).
// Links user/settings.cpp (toml++ from XenonRecomp's vendored copy); no SDL.
// From the repo root:
//   clang++ -std=c++20 -O2 -Iruntime -Itools/XenonRecomp/thirdparty/tomlplusplus/include \
//     tests/black_edition_test.cpp runtime/user/settings.cpp runtime/platform/posix/file_io.cpp \
//     -o build/black_edition_test && build/black_edition_test
#include <game/black_edition.h>
#include <user/settings.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <process.h>  // _getpid
#else
#include <unistd.h>  // mkdtemp
#endif

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

namespace
{
    using namespace game::black_edition;
    using settings::Id;

    void SetEnv(const char* name, const char* value)
    {
        if (!name)
            return;
#ifdef _WIN32
        _putenv_s(name, value ? value : "");
#else
        if (value)
            setenv(name, value, 1);
        else
            unsetenv(name);
#endif
    }

    void WriteFile(const std::filesystem::path& path, const std::string& text)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << text;
    }

    std::string ReadAll(const std::filesystem::path& path)
    {
        std::ifstream f(path);
        std::stringstream s;
        s << f.rdbuf();
        return s.str();
    }

    // The flag's word and its neighbours (0x82A2CE00-0x82A2CE0F), as a disc
    // has them (all 0) or with something planted around it.
    struct Row
    {
        uint8_t b[16];
        uint8_t* flag() { return b + (kFlag - 0x82A2CE00); }
    };

    void TestAddress()
    {
        // tuhana's Xenia patch: be32 0x82A2CE04 = 0x00000100, i.e. the byte at
        // +2 is 1 and its three neighbours 0.
        const uint32_t word = 0x82A2CE04, value = 0x00000100;
        const uint8_t bytes[4] = { uint8_t(value >> 24), uint8_t(value >> 16), uint8_t(value >> 8), uint8_t(value) };
        int set = -1;
        for (int i = 0; i < 4; i++)
            if (bytes[i])
            {
                CHECK(set < 0, "the patch sets one byte only");
                set = i;
            }
        CHECK(word + uint32_t(set) == kFlag, "the patch's byte %08X is kFlag %08X", word + uint32_t(set), kFlag);
    }

    void TestApply()
    {
        // On: the flag becomes 1, nothing else changes.
        Row r{};
        CHECK(Apply(r.flag(), true), "on writes");
        for (int i = 0; i < 16; i++)
            CHECK(r.b[i] == (r.b + i == r.flag() ? 1 : 0), "byte %d after on: %u", i, r.b[i]);
        // Off: nothing written, whatever is there (the disc's 0, or anything).
        Row z{};
        CHECK(!Apply(z.flag(), false), "off writes nothing");
        for (int i = 0; i < 16; i++)
            CHECK(z.b[i] == 0, "byte %d after off: %u", i, z.b[i]);
        Row p;
        memset(p.b, 0xA5, sizeof(p.b));
        CHECK(!Apply(p.flag(), false), "off writes nothing");
        for (int i = 0; i < 16; i++)
            CHECK(p.b[i] == 0xA5, "planted byte %d after off: %u", i, p.b[i]);
        // On twice: still 1.
        Row t{};
        Apply(t.flag(), true);
        Apply(t.flag(), true);
        CHECK(*t.flag() == 1, "on twice: %u", *t.flag());
    }

    void TestRow()
    {
        const settings::Option& o = settings::Info(Id::BlackEdition);
        CHECK(o.id == Id::BlackEdition, "the row's id");
        CHECK(std::string_view(o.table) == "game" && std::string_view(o.key) == "black_edition", "[game] black_edition: [%s] %s",
            o.table, o.key);
        CHECK(std::string_view(o.label) == "Black Edition", "label %s", o.label);
        CHECK(o.category == settings::Category::Game, "Settings > Game");
        CHECK(o.type == settings::Type::Bool, "On/Off");
        CHECK(o.apply == settings::Apply::Restart, "at the next launch");
        CHECK(!o.hidden, "shown on every device");
        CHECK(o.defaultValue == 0 && settings::DefaultValue(Id::BlackEdition) == 0, "off by default");
        CHECK(o.env && std::string_view(o.env) == "NFSMW_BLACK_EDITION", "NFSMW_BLACK_EDITION");
        CHECK(o.description && std::string_view(o.description).find("Black Edition Challenge") != std::string_view::npos,
            "the description names what it adds");
        // [game] keeps its keys together, Language first (Save writes a table once).
        CHECK(size_t(Id::BlackEdition) == size_t(Id::GameLanguage) + 1, "after Language");
        // Catch Up was added after it, so it holds the last row.
        CHECK(size_t(Id::CatchUp) == size_t(Id::BlackEdition) + 1 && size_t(Id::CatchUp) + 1 == settings::kCount,
            "after Black Edition, the last row");
    }

    void TestFile(const std::filesystem::path& file)
    {
        for (const settings::Option& o : settings::Options())
            SetEnv(o.env, nullptr);
        // The launch: the file's value, read once (settings::Load before the
        // image; game/black_edition.cpp reads it there).
        WriteFile(file, "[game]\nlanguage = \"english\"\nblack_edition = true\n");
        CHECK(settings::Load(file), "loads: %s", settings::LastError().c_str());
        CHECK(settings::GetBool(Id::BlackEdition), "black_edition = true: on");
        CHECK(settings::GetStoredValue(Id::BlackEdition) == 1 && !settings::IsDefault(Id::BlackEdition), "stored on");
        CHECK(settings::EnvOverride(Id::BlackEdition) == nullptr, "no override");
        // The menu turns it off: stored, and this run keeps on.
        CHECK(settings::Set(Id::BlackEdition, 0) == settings::SetResult::Deferred, "a change waits for the next launch");
        CHECK(settings::GetBool(Id::BlackEdition), "this run stays on");
        CHECK(settings::GetStoredValue(Id::BlackEdition) == 0 && settings::RestartPending(Id::BlackEdition), "off is stored, pending");
        CHECK(settings::Save(), "saved");
        CHECK(ReadAll(file).find("black_edition = false") != std::string::npos, "the file holds black_edition = false");
        // Read again in the same run: the stored value follows the file, the
        // running one doesn't.
        CHECK(settings::Load(file), "reloads");
        CHECK(settings::GetBool(Id::BlackEdition) && settings::GetStoredValue(Id::BlackEdition) == 0, "on this run, off stored");
        WriteFile(file, "[video]\nvsync = true\n");
        CHECK(settings::Load(file), "a file without it loads");
        CHECK(settings::GetStoredValue(Id::BlackEdition) == 0 && settings::IsDefault(Id::BlackEdition), "missing: the default, off");
        // Words and numbers, as every Bool option reads them.
        WriteFile(file, "[game]\nblack_edition = 1\n");
        CHECK(settings::Load(file) && settings::GetStoredValue(Id::BlackEdition) == 1, "black_edition = 1: on");
        // NFSMW_BLACK_EDITION: the override is reported (it can't change a
        // run that has started either).
        SetEnv("NFSMW_BLACK_EDITION", "0");
        CHECK(settings::Load(file), "reloads with the variable");
        const char* env = settings::EnvOverride(Id::BlackEdition);
        CHECK(env && std::string_view(env) == "NFSMW_BLACK_EDITION", "the override is reported");
        CHECK(settings::GetStoredValue(Id::BlackEdition) == 1, "the file's value stays stored");
        SetEnv("NFSMW_BLACK_EDITION", nullptr);
    }
}

int main()
{
    TestAddress();
    TestApply();
    TestRow();

#ifdef _WIN32
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / ("speedbreaker-be-" + std::to_string(_getpid()));
    std::error_code scratchError;
    std::filesystem::remove_all(dir, scratchError);
    if (!std::filesystem::create_directory(dir, scratchError))
    {
        printf("can't make a scratch folder: %s\n", scratchError.message().c_str());
        return 2;
    }
#else
    std::string pattern = (std::filesystem::temp_directory_path() / "speedbreaker-be-XXXXXX").string();
    if (!mkdtemp(pattern.data()))
    {
        printf("can't make a scratch folder: %s\n", strerror(errno));
        return 2;
    }
    const std::filesystem::path dir = pattern;
#endif
    TestFile(dir / "settings.toml");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    if (g_failures)
    {
        printf("black_edition_test: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("black_edition_test: all passed\n");
    return 0;
}
