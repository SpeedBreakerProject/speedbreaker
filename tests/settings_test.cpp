// Tests for settings::SetDeviceDefault (runtime/user/settings.cpp): a
// device's own default before Load (an iPhone's Frame Rate) and after it
// (a GPU's, known once its Vulkan device exists: Windows on an integrated
// GPU). From the repo root:
//   clang++ -std=c++20 -O2 -Iruntime -isystem tools/XenonRecomp/thirdparty/tomlplusplus/include
//     tests/settings_test.cpp runtime/user/settings.cpp runtime/platform/posix/file_io.cpp -o build/settings_test
//   build/settings_test <scratch folder>
// The scratch folder's settings.toml is written and read; nothing else.
#include <user/settings.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>  // std::istreambuf_iterator
#include <string>
#include <vector>

using settings::Id;

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

namespace
{
    void SetEnv(const char* name, const char* value)
    {
        if (!name)
            return;  // an option without a variable (v0.1.5's Key options): the C runtime's _putenv_s fails fast on null
#ifdef _WIN32
        _putenv_s(name, value ? value : "");  // "" removes it
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
}

int main(int argc, char** argv)
{
    std::filesystem::path dir = argc > 1 ? argv[1] : "settings_test_out";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / "settings.toml";

    // The options used here come from the file and these defaults only.
    for (const settings::Option& o : settings::Options())
        SetEnv(o.env, nullptr);
    SetEnv("NFSMW_TEXTURE_MIPS", "1");  // Mipmaps: an environment override

    // Before Load, as on an iPhone: what a file without the option gives.
    settings::SetDeviceDefault(Id::FrameRate, 30, "test: before Load");
    WriteFile(file,
        "[video]\n"
        "anti_aliasing = true\n"
        "[audio]\n"
        "master_volume = 50\n");
    CHECK(settings::Load(file), "the file loads");
    CHECK(settings::GetInt(Id::FrameRate) == 30, "frame rate %d", settings::GetInt(Id::FrameRate));
    CHECK(settings::IsDefault(Id::FrameRate), "30 is the default");
    CHECK(settings::GetInt(Id::MasterVolume) == 50, "the file's volume");
    CHECK(!settings::Dirty(), "nothing to save after a load");

    std::vector<Id> told;
    uint32_t listener = settings::Subscribe([&told](Id id) { told.push_back(id); });
    uint64_t generation = settings::Generation();

    // After Load, an option the file holds: its value stays.
    settings::SetDeviceDefault(Id::AntiAliasing, 0, "test: in the file");
    CHECK(settings::GetBool(Id::AntiAliasing), "the file's anti-aliasing stays on");
    CHECK(settings::GetStoredValue(Id::AntiAliasing) == 1, "stored on");
    CHECK(settings::DefaultValue(Id::AntiAliasing) == 0, "but the default is off");
    CHECK(!settings::IsDefault(Id::AntiAliasing), "on isn't the default now");

    // After Load, a live option the file doesn't hold: the new default now.
    settings::SetDeviceDefault(Id::Anisotropy, 2, "test: not in the file");
    CHECK(settings::GetInt(Id::Anisotropy) == 2, "anisotropy %d", settings::GetInt(Id::Anisotropy));
    CHECK(settings::GetStoredValue(Id::Anisotropy) == 2, "stored 2x");
    CHECK(settings::IsDefault(Id::Anisotropy), "2x is the default");
    CHECK(settings::Reset(Id::Anisotropy) == settings::SetResult::Unchanged, "a reset keeps 2x");
    CHECK(settings::Generation() > generation, "the generation moved");
    CHECK(told.size() == 1 && told[0] == Id::Anisotropy, "listeners told once, of anisotropy (%zu calls)", told.size());
    CHECK(!settings::Dirty(), "a device default isn't a change to save");

    // A Restart option keeps this run's value.
    settings::SetDeviceDefault(Id::WindowWidth, 1600, "test: restart option");
    CHECK(settings::GetInt(Id::WindowWidth) == 1280, "window width %d", settings::GetInt(Id::WindowWidth));
    CHECK(settings::GetStoredValue(Id::WindowWidth) == 1280, "stored 1280");
    CHECK(settings::DefaultValue(Id::WindowWidth) == 1600, "the default is 1600");

    // Changed since the start (the menu): that value stays.
    settings::Set(Id::Sharpening, 0.5);
    settings::SetDeviceDefault(Id::Sharpening, 0.1, "test: set since");
    CHECK(settings::GetFloat(Id::Sharpening) == 0.5f, "sharpening %.2f", settings::GetFloat(Id::Sharpening));

    // An environment override still wins for the run; the stored value moves.
    settings::SetDeviceDefault(Id::Mipmaps, 0, "test: environment");
    CHECK(settings::GetBool(Id::Mipmaps), "NFSMW_TEXTURE_MIPS=1 wins");
    CHECK(settings::GetStoredValue(Id::Mipmaps) == 0, "stored off");
    CHECK(settings::EnvOverride(Id::Mipmaps) != nullptr, "the override is reported");

    // After a save the file holds every option: a later default changes nothing.
    told.clear();
    CHECK(settings::Save(), "saved");
    settings::SetDeviceDefault(Id::Vibration, 0, "test: after a save");
    CHECK(settings::GetBool(Id::Vibration), "the saved vibration stays on");
    CHECK(told.empty(), "no listener calls");

    // The saved file gives the values back, the device's 2x among them.
    CHECK(settings::Load(file), "the saved file loads");
    CHECK(settings::GetInt(Id::Anisotropy) == 2, "anisotropy %d after a reload", settings::GetInt(Id::Anisotropy));
    CHECK(settings::GetBool(Id::AntiAliasing), "anti-aliasing on after a reload");
    CHECK(settings::GetFloat(Id::Sharpening) == 0.5f, "sharpening %.2f after a reload", settings::GetFloat(Id::Sharpening));
    {
        std::ifstream in(file);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(text.find("anisotropic_filtering = \"2x\"") != std::string::npos, "the file holds 2x");
        CHECK(text.find("mipmaps = false") != std::string::npos, "the file holds the stored mipmaps, not the override");
    }

    settings::Unsubscribe(listener);
    std::filesystem::remove_all(dir, ec);
    printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "all passed", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
