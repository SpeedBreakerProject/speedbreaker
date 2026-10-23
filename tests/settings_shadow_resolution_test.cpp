// Settings > Graphics > Shadow Resolution: independent scales, live, saved in
// [video], and overridden for one run by NFSMW_SHADOW_RESOLUTION.
#include <user/settings.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace
{
    int s_failures = 0;

    void SetEnv(const char* name, const char* value)
    {
        if (value)
            setenv(name, value, 1);
        else
            unsetenv(name);
    }

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
    } while (false)

    std::string Read(const std::filesystem::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        return text.str();
    }
}

int main(int argc, char** argv)
{
    using namespace settings;
    constexpr Id id = Id::ShadowResolution;
    SetEnv("NFSMW_SHADOW_RESOLUTION", nullptr);
    SetEnv("NFSMW_SHADOW_FILTERING", nullptr);
    const std::filesystem::path dir = argc > 1 ? argv[1] : "settings_shadow_resolution_scratch";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / "settings.toml";

    const Option& option = Info(id);
    CHECK(option.category == Category::Graphics && option.type == Type::Enum && option.apply == Apply::Live && !option.hidden,
        "available as a live Graphics menu option");
    CHECK(std::string(option.table) == "video" && std::string(option.key) == "shadow_resolution",
        "stored in [video].shadow_resolution");
    CHECK(std::string(option.env) == "NFSMW_SHADOW_RESOLUTION", "environment override name");
    CHECK(DefaultValue(id) == 2, "1x original resolution by default");
    CHECK(option.entries.size() == 5, "five fixed shadow scales");
    if (option.entries.size() == 5)
    {
        CHECK(option.entries[0].value == 2 && std::string(option.entries[0].token) == "1x" &&
            std::string(option.entries[0].label) == "1x (1600x1600)", "first: original 1x");
        CHECK(option.entries[1].value == 3 && std::string(option.entries[1].token) == "1.5x" &&
            std::string(option.entries[1].label) == "1.5x (2400x2400)", "second: 1.5x");
        CHECK(option.entries[2].value == 4 && std::string(option.entries[2].token) == "2x" &&
            std::string(option.entries[2].label) == "2x (3200x3200)", "third: 2x");
        CHECK(option.entries[3].value == 6 && std::string(option.entries[3].token) == "3x" &&
            std::string(option.entries[3].label) == "3x (4800x4800)", "fourth: 3x");
        CHECK(option.entries[4].value == 8 && std::string(option.entries[4].token) == "4x" &&
            std::string(option.entries[4].label) == "4x (6400x6400)", "fifth: 4x");
    }

    const Option& resolution = Info(Id::InternalResolution);
    CHECK(resolution.entries.size() == 6, "Auto plus five fixed resolution choices");
    CHECK(resolution.entries.size() >= 6 && resolution.entries[5].value == 8 &&
        std::string(resolution.entries[5].token) == "4x" && std::string(resolution.entries[5].label) == "4x (2880p)",
        "4x Internal Resolution is offered in the F1 Graphics menu");

    CHECK(Load(file), "load defaults: %s", LastError().c_str());
    CHECK(GetInt(id) == 2, "default is original 1x");
    std::ofstream(file, std::ios::binary) << "[video]\nhigh_resolution_shadows = true\n";
    CHECK(Load(file) && GetInt(id) == 2, "the previous boolean setting is not migrated");
    CHECK(Set(id, 8) == SetResult::Applied && GetInt(id) == 8, "menu selection applies live");
    CHECK(Save(file), "save: %s", LastError().c_str());
    CHECK(Read(file).find("shadow_resolution = \"4x\"") != std::string::npos, "saved as a TOML token");
    CHECK(Set(id, 2) == SetResult::Applied && Load(file) && GetInt(id) == 8, "saved setting loads");

    SetEnv("NFSMW_SHADOW_RESOLUTION", "1x");
    CHECK(Load(file) && GetInt(id) == 2, "environment overrides the saved value");
    CHECK(EnvOverride(id) != nullptr, "the menu can show the active override");
    CHECK(Save(file) && Read(file).find("shadow_resolution = \"4x\"") != std::string::npos,
        "saving preserves the configured value, not the override");
    SetEnv("NFSMW_SHADOW_RESOLUTION", nullptr);

    constexpr Id filteringId = Id::ShadowFiltering;
    const Option& filtering = Info(filteringId);
    CHECK(filtering.category == Category::Graphics && filtering.type == Type::Enum &&
        filtering.apply == Apply::Live && !filtering.hidden, "available as a live Graphics menu option");
    CHECK(std::string(filtering.table) == "video" && std::string(filtering.key) == "shadow_filtering",
        "stored in [video].shadow_filtering");
    CHECK(std::string(filtering.env) == "NFSMW_SHADOW_FILTERING", "environment override name");
    CHECK(DefaultValue(filteringId) == int32_t(ShadowFilteringMode::Original), "Original is the default");
    CHECK(filtering.entries.size() == 2, "Original and PCSS prototype choices");
    if (filtering.entries.size() == 2)
    {
        CHECK(filtering.entries[0].value == int32_t(ShadowFilteringMode::Original) &&
            std::string(filtering.entries[0].token) == "original", "first: Original filtering");
        CHECK(filtering.entries[1].value == int32_t(ShadowFilteringMode::PcssPrototype) &&
            std::string(filtering.entries[1].token) == "pcss", "second: PCSS prototype");
    }
    CHECK(Set(filteringId, int32_t(ShadowFilteringMode::PcssPrototype)) == SetResult::Applied, "PCSS applies live");
    CHECK(Save(file) && Read(file).find("shadow_filtering = \"pcss\"") != std::string::npos,
        "PCSS is saved as a TOML token");
    SetEnv("NFSMW_SHADOW_FILTERING", "original");
    CHECK(Load(file) && GetInt(filteringId) == int32_t(ShadowFilteringMode::Original),
        "Original environment override is loaded");
    CHECK(Save(file) && Read(file).find("shadow_filtering = \"pcss\"") != std::string::npos,
        "saving preserves the configured value, not the override");
    SetEnv("NFSMW_SHADOW_FILTERING", nullptr);

    std::filesystem::remove_all(dir);
    if (s_failures)
    {
        fprintf(stderr, "%d check(s) FAILED\n", s_failures);
        return 1;
    }
    puts("all checks passed");
    return 0;
}
