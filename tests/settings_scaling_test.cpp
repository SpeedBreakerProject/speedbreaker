// SpeedBreaker tests. GPL-3.0-or-later (see COPYING).
//
// Settings > Graphics > Upscaling's values in settings.toml (user/settings.cpp,
// linked as the game links it): "fsr1" (FSR 1, v0.1.5) read, saved and read
// back, next to "bicubic" and "linear"; the menu's order (Bicubic, Bilinear,
// FSR 1); NFSMW_SCALING=fsr1 for one run, never saved; a token this version
// doesn't know (a later version's) falls back to the default, Bicubic; and
// Sharpening's default, 0.3, unchanged.
// The argument is a scratch folder (settings.toml is written there).
//
// Build by hand (from the repo root):
//   c++ -std=c++20 -O2 -UNDEBUG -I runtime tests/settings_scaling_test.cpp runtime/user/settings.cpp -o build/settings_scaling_test
//   build/settings_scaling_test build/settings_scaling_scratch
#include <user/settings.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#ifdef _WIN32
// The MSVC C runtime has _putenv_s, not setenv and unsetenv ("" removes).
static int setenv(const char* name, const char* value, int) { return _putenv_s(name, value); }
static int unsetenv(const char* name) { return _putenv_s(name, ""); }
#endif

namespace
{
    int s_failures = 0;

#define CHECK(cond, ...)                                                    \
    do                                                                      \
    {                                                                       \
        if (!(cond))                                                        \
        {                                                                   \
            s_failures++;                                                   \
            fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
            fprintf(stderr, __VA_ARGS__);                                   \
            fprintf(stderr, "\n");                                          \
        }                                                                   \
    } while (0)

    using settings::Id;
    using settings::ScalingMode;

    std::string Read(const std::filesystem::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        return text.str();
    }

    void Write(const std::filesystem::path& path, const std::string& text)
    {
        std::ofstream(path, std::ios::binary) << text;
    }

    int32_t Scaling() { return settings::GetInt(Id::Scaling); }
}

int main(int argc, char** argv)
{
    unsetenv("NFSMW_SCALING");
    unsetenv("NFSMW_SHARPEN");
    const std::filesystem::path dir = argc > 1 ? argv[1] : "settings_scaling_scratch";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / "settings.toml";

    // The option: three entries in menu order, Bicubic the default.
    const settings::Option& o = settings::Info(Id::Scaling);
    CHECK(o.entries.size() == 3, "%zu entries", o.entries.size());
    if (o.entries.size() == 3)
    {
        CHECK(o.entries[0].value == int32_t(ScalingMode::Bicubic) && std::string(o.entries[0].token) == "bicubic", "first: bicubic");
        CHECK(o.entries[1].value == int32_t(ScalingMode::Linear) && std::string(o.entries[1].token) == "linear", "second: linear");
        CHECK(o.entries[2].value == int32_t(ScalingMode::Fsr1) && std::string(o.entries[2].token) == "fsr1", "third: fsr1");
    }
    CHECK(settings::DefaultValue(Id::Scaling) == double(ScalingMode::Bicubic), "default bicubic");
    CHECK(settings::FormatValue(Id::Scaling, double(ScalingMode::Fsr1)) == "FSR 1", "label %s", settings::FormatValue(Id::Scaling, 2).c_str());
    CHECK(settings::StepValue(Id::Scaling, double(ScalingMode::Linear), 1) == double(ScalingMode::Fsr1), "Bilinear, right: FSR 1");
    CHECK(settings::StepValue(Id::Scaling, double(ScalingMode::Fsr1), -1) == double(ScalingMode::Linear), "FSR 1, left: Bilinear");

    // No file: the defaults.
    CHECK(settings::Load(file), "load without a file: %s", settings::LastError().c_str());
    CHECK(Scaling() == int32_t(ScalingMode::Bicubic), "no file: %d", Scaling());
    CHECK(std::fabs(settings::GetFloat(Id::Sharpening) - 0.3f) < 1e-6f, "Sharpening's default 0.3: %.3f", settings::GetFloat(Id::Sharpening));

    // Chosen in the menu, saved, read back.
    CHECK(settings::Set(Id::Scaling, double(ScalingMode::Fsr1)) == settings::SetResult::Applied, "set FSR 1");
    CHECK(Scaling() == int32_t(ScalingMode::Fsr1), "in effect at once: %d", Scaling());
    CHECK(settings::Save(file), "save: %s", settings::LastError().c_str());
    const std::string saved = Read(file);
    CHECK(saved.find("scaling = \"fsr1\"") != std::string::npos, "saved as a token:\n%s", saved.c_str());
    CHECK(settings::Set(Id::Scaling, double(ScalingMode::Linear)) == settings::SetResult::Applied, "set bilinear");
    CHECK(settings::Load(file) && Scaling() == int32_t(ScalingMode::Fsr1), "read back: %d", Scaling());

    // Each token by hand.
    for (auto [token, value] : { std::pair{ "bicubic", ScalingMode::Bicubic }, std::pair{ "linear", ScalingMode::Linear }, std::pair{ "fsr1", ScalingMode::Fsr1 } })
    {
        Write(file, std::string("[video]\nscaling = \"") + token + "\"\n");
        CHECK(settings::Load(file) && Scaling() == int32_t(value), "%s: %d", token, Scaling());
    }
    // A later version's value: the default, and the file isn't rewritten by loading it.
    Write(file, "[video]\nscaling = \"fsr3\"\n");
    settings::Load(file);
    CHECK(Scaling() == int32_t(ScalingMode::Bicubic), "unknown token: %d", Scaling());
    CHECK(Read(file).find("fsr3") != std::string::npos, "the file is left as it was");

    // NFSMW_SCALING=fsr1 for this run: in effect, never saved.
    Write(file, "[video]\nscaling = \"linear\"\n");
    setenv("NFSMW_SCALING", "fsr1", 1);
    CHECK(settings::Load(file), "load with the environment");
    CHECK(Scaling() == int32_t(ScalingMode::Fsr1), "environment: %d", Scaling());
    CHECK(settings::EnvOverride(Id::Scaling) != nullptr, "the menu shows the override");
    CHECK(settings::GetStoredValue(Id::Scaling) == double(ScalingMode::Linear), "stored: %g", settings::GetStoredValue(Id::Scaling));
    CHECK(settings::Save(file) && Read(file).find("scaling = \"linear\"") != std::string::npos, "saved: the file's value:\n%s", Read(file).c_str());
    unsetenv("NFSMW_SCALING");

    std::filesystem::remove_all(dir);
    if (s_failures)
    {
        printf("%d check(s) FAILED\n", s_failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
