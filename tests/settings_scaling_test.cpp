// SpeedBreaker tests. GPL-3.0-or-later (see COPYING).
//
// Settings > Graphics > Upscaling, Shadow Resolution, Sharper UI and Movie
// Upscaling's values in settings.toml (user/settings.cpp, linked as the game links it): tokens read,
// saved and read back; environment overrides for one run, never saved; and
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

    int32_t ShadowResolution() { return settings::GetInt(Id::ShadowResolution); }

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
    unsetenv("NFSMW_SHADOW_RESOLUTION");
    unsetenv("NFSMW_SHARP_UI");
    unsetenv("NFSMW_MOVIE_UPSCALE");
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

    const settings::Option& shadows = settings::Info(Id::ShadowResolution);
    CHECK(shadows.entries.size() == 4, "%zu shadow-resolution entries", shadows.entries.size());
    if (shadows.entries.size() == 4)
    {
        CHECK(shadows.entries[0].value == settings::kShadowResolutionAuto && std::string(shadows.entries[0].token) == "auto",
            "first shadow option: Auto");
        CHECK(shadows.entries[1].value == 2 && std::string(shadows.entries[1].token) == "1x", "second shadow option: 1x");
        CHECK(shadows.entries[2].value == 3 && std::string(shadows.entries[2].token) == "1.5x", "third shadow option: 1.5x");
        CHECK(shadows.entries[3].value == 4 && std::string(shadows.entries[3].token) == "2x", "fourth shadow option: 2x");
    }
    CHECK(std::string(shadows.table) == "video" && std::string(shadows.key) == "shadow_resolution", "stored in [video].shadow_resolution");
    CHECK(settings::kShadowResolutionDefault == settings::kShadowResolutionAuto &&
            settings::DefaultValue(Id::ShadowResolution) == settings::kShadowResolutionAuto,
        "shadow-resolution default is Auto (GitHub #90)");
    CHECK(settings::FormatValue(Id::ShadowResolution, 3) == "1.5x (2400x2400)", "1.5x label: %s",
        settings::FormatValue(Id::ShadowResolution, 3).c_str());
    CHECK(settings::FormatValue(Id::ShadowResolution, settings::kShadowResolutionAuto) == "Auto", "Auto label: %s",
        settings::FormatValue(Id::ShadowResolution, settings::kShadowResolutionAuto).c_str());
    CHECK(settings::StepValue(Id::ShadowResolution, 2, 1) == 3 && settings::StepValue(Id::ShadowResolution, 4, 1) == 4,
        "shadow-resolution steps stop at 2x");
    CHECK(settings::StepValue(Id::ShadowResolution, settings::kShadowResolutionAuto, 1) == 2 &&
            settings::StepValue(Id::ShadowResolution, 2, -1) == settings::kShadowResolutionAuto &&
            settings::StepValue(Id::ShadowResolution, settings::kShadowResolutionAuto, -1) == settings::kShadowResolutionAuto,
        "Auto is left of 1x, and the left end");

    // No file: the defaults.
    CHECK(settings::Load(file), "load without a file: %s", settings::LastError().c_str());
    CHECK(Scaling() == int32_t(ScalingMode::Bicubic), "no file: %d", Scaling());
    CHECK(ShadowResolution() == settings::kShadowResolutionDefault, "no file: shadow resolution %d", ShadowResolution());
    CHECK(std::fabs(settings::GetFloat(Id::Sharpening) - 0.3f) < 1e-6f, "Sharpening's default 0.3: %.3f", settings::GetFloat(Id::Sharpening));

    // Chosen in the menu, saved, read back.
    CHECK(settings::Set(Id::Scaling, double(ScalingMode::Fsr1)) == settings::SetResult::Applied, "set FSR 1");
    CHECK(settings::Set(Id::ShadowResolution, 3) == settings::SetResult::Applied, "set shadow resolution 1.5x");
    CHECK(ShadowResolution() == 3, "shadow resolution takes effect live: %d", ShadowResolution());
    CHECK(Scaling() == int32_t(ScalingMode::Fsr1), "in effect at once: %d", Scaling());
    CHECK(settings::Save(file), "save: %s", settings::LastError().c_str());
    const std::string saved = Read(file);
    CHECK(saved.find("scaling = \"fsr1\"") != std::string::npos, "saved as a token:\n%s", saved.c_str());
    CHECK(saved.find("shadow_resolution = \"1.5x\"") != std::string::npos, "shadow resolution saved as a token:\n%s", saved.c_str());
    CHECK(settings::Set(Id::Scaling, double(ScalingMode::Linear)) == settings::SetResult::Applied, "set bilinear");
    CHECK(settings::Set(Id::ShadowResolution, 2) == settings::SetResult::Applied, "set shadow resolution 1x");
    CHECK(settings::Load(file) && Scaling() == int32_t(ScalingMode::Fsr1), "read back: %d", Scaling());
    CHECK(ShadowResolution() == 3, "shadow resolution read back: %d", ShadowResolution());
    CHECK(settings::Set(Id::ShadowResolution, settings::kShadowResolutionAuto) == settings::SetResult::Applied, "set shadow resolution Auto");
    CHECK(settings::Save(file) && Read(file).find("shadow_resolution = \"auto\"") != std::string::npos, "Auto saved as \"auto\":\n%s",
        Read(file).c_str());

    // From an earlier version (GitHub #90): loading keeps a file's value, as
    // for every option (a save writes every option, so the old default can't
    // be told from a choice); one without the key (no option ever saved, or
    // from before v0.2.1) takes Auto. Then MigrateShadowResolution, once: a
    // 1x from before 0.2.4 becomes Auto, saved at once with the hidden mark.
    using settings::kShadowResolutionAuto;
    auto Migrate = [&](const std::string& text) {
        Write(file, text);
        CHECK(settings::Load(file), "load: %s", settings::LastError().c_str());
        return settings::MigrateShadowResolution();
    };
    for (auto [token, value] : { std::pair{ "1x", 2 }, std::pair{ "1.5x", 3 }, std::pair{ "2x", 4 } })
    {
        Write(file, std::string("[video]\nshadow_resolution = \"") + token + "\"\n\n[app]\nlast_version = \"0.2.3\"\n");
        CHECK(settings::Load(file) && ShadowResolution() == value, "a v0.2.3 file's %s, as loaded: %d", token, ShadowResolution());
    }
    // v0.2.3's 1x: Auto, marked, in the file at once, the version untouched.
    CHECK(Migrate("[video]\nshadow_resolution = \"1x\"\n\n[app]\nlast_version = \"0.2.3\"\n"), "v0.2.3's 1x migrates");
    CHECK(ShadowResolution() == kShadowResolutionAuto && settings::GetInt(Id::ShadowResolutionVersion) == 1, "now Auto, marked: %d", ShadowResolution());
    {
        const std::string text = Read(file);
        CHECK(text.find("shadow_resolution = \"auto\"") != std::string::npos && text.find("shadow_resolution_version = 1") != std::string::npos &&
                text.find("last_version = \"0.2.3\"") != std::string::npos,
            "saved at once (Auto, the mark, the old version until the launch records its own):\n%s", text.c_str());
        CHECK(!settings::Dirty(), "nothing left to save");
    }
    // Once: the same file again does nothing, and a 1x chosen since (still
    // under a 0.2.3 last_version, as in a build not yet at 0.2.4) stays 1x.
    CHECK(settings::Load(file) && !settings::MigrateShadowResolution() && ShadowResolution() == kShadowResolutionAuto, "again: nothing");
    CHECK(settings::Set(Id::ShadowResolution, 2) == settings::SetResult::Applied && settings::Save(file), "1x chosen after the migration");
    CHECK(settings::Load(file) && !settings::MigrateShadowResolution() && ShadowResolution() == 2, "the marked file's 1x stays: %d",
        ShadowResolution());
    // Older versions too (v0.2.1 brought the option, a hand edit an older one).
    CHECK(Migrate("[video]\nshadow_resolution = \"1x\"\n\n[app]\nlast_version = \"0.2.1\"\n") && ShadowResolution() == kShadowResolutionAuto,
        "v0.2.1's 1x migrates");
    // Left as they are: 1.5x and 2x; 0.2.4 and later; no version; Auto; no key; no file.
    CHECK(!Migrate("[video]\nshadow_resolution = \"1.5x\"\n\n[app]\nlast_version = \"0.2.3\"\n") && ShadowResolution() == 3, "1.5x kept");
    CHECK(!Migrate("[video]\nshadow_resolution = \"2x\"\n\n[app]\nlast_version = \"0.2.3\"\n") && ShadowResolution() == 4, "2x kept");
    CHECK(!Migrate("[video]\nshadow_resolution = \"1x\"\n\n[app]\nlast_version = \"0.2.4\"\n") && ShadowResolution() == 2, "0.2.4's 1x kept");
    CHECK(!Migrate("[video]\nshadow_resolution = \"1x\"\n\n[app]\nlast_version = \"0.3.0\"\n") && ShadowResolution() == 2, "0.3.0's 1x kept");
    CHECK(!Migrate("[video]\nshadow_resolution = \"1x\"\n") && ShadowResolution() == 2, "no version: 1x kept");
    CHECK(!Migrate("[video]\nshadow_resolution = \"1x\"\nshadow_resolution_version = 1\n\n[app]\nlast_version = \"0.2.3\"\n") &&
            ShadowResolution() == 2,
        "marked: 1x kept");
    CHECK(!Migrate("[video]\nshadow_resolution = \"auto\"\n\n[app]\nlast_version = \"0.2.3\"\n") && ShadowResolution() == kShadowResolutionAuto,
        "Auto: nothing to do");
    CHECK(!Migrate("[video]\ninternal_resolution = \"auto\"\n\n[app]\nlast_version = \"0.2.3\"\n") && ShadowResolution() == kShadowResolutionAuto,
        "a file without the key: Auto, nothing to do");
    CHECK(!Migrate("[app]\nlast_version = \"0.2.3\"\n") && ShadowResolution() == kShadowResolutionAuto, "a file with only the version: Auto");
    std::filesystem::remove(file);
    CHECK(settings::Load(file) && !settings::MigrateShadowResolution() && ShadowResolution() == kShadowResolutionAuto, "no file: Auto");
    // NFSMW_SHADOW_RESOLUTION=2x for the run: the stored 1x migrates, the run keeps 2x.
    setenv("NFSMW_SHADOW_RESOLUTION", "2x", 1);
    CHECK(Migrate("[video]\nshadow_resolution = \"1x\"\n\n[app]\nlast_version = \"0.2.3\"\n") && ShadowResolution() == 4 &&
            settings::GetStoredValue(Id::ShadowResolution) == kShadowResolutionAuto && Read(file).find("shadow_resolution = \"auto\"") != std::string::npos,
        "under the environment: stored Auto, in effect 2x");
    unsetenv("NFSMW_SHADOW_RESOLUTION");

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

    // NFSMW_SHADOW_RESOLUTION=2x for this run: in effect, never saved.
    Write(file, "[video]\nshadow_resolution = \"1.5x\"\n");
    setenv("NFSMW_SHADOW_RESOLUTION", "2x", 1);
    CHECK(settings::Load(file), "load shadow resolution with the environment");
    CHECK(ShadowResolution() == 4, "shadow-resolution environment: %d", ShadowResolution());
    CHECK(settings::EnvOverride(Id::ShadowResolution) != nullptr, "the menu shows the shadow-resolution override");
    CHECK(settings::GetStoredValue(Id::ShadowResolution) == 3, "stored shadow resolution: %g", settings::GetStoredValue(Id::ShadowResolution));
    CHECK(settings::Save(file) && Read(file).find("shadow_resolution = \"1.5x\"") != std::string::npos,
        "environment override does not replace the file value:\n%s", Read(file).c_str());
    setenv("NFSMW_SHADOW_RESOLUTION", "auto", 1);
    CHECK(settings::Load(file) && ShadowResolution() == settings::kShadowResolutionAuto, "NFSMW_SHADOW_RESOLUTION=auto: %d", ShadowResolution());
    unsetenv("NFSMW_SHADOW_RESOLUTION");

    // Sharper UI (gpu/sharp_ui.h): on by default, live, [video].sharper_ui,
    // NFSMW_SHARP_UI=0 for one run, never saved.
    {
        const settings::Option& ui = settings::Info(Id::SharpUi);
        CHECK(ui.category == settings::Category::Graphics && ui.type == settings::Type::Bool && ui.apply == settings::Apply::Live && !ui.hidden,
            "Sharper UI: a live Graphics switch");
        CHECK(std::string(ui.table) == "video" && std::string(ui.key) == "sharper_ui" && std::string(ui.env) == "NFSMW_SHARP_UI",
            "Sharper UI: [video].sharper_ui, NFSMW_SHARP_UI");
        CHECK(std::string(ui.label) == "Sharper UI", "label %s", ui.label);
        Write(file, "");
        CHECK(settings::Load(file) && settings::GetBool(Id::SharpUi), "Sharper UI on by default");
        CHECK(settings::Set(Id::SharpUi, 0) == settings::SetResult::Applied && !settings::GetBool(Id::SharpUi), "Sharper UI off, live");
        CHECK(settings::Save(file) && Read(file).find("sharper_ui = false") != std::string::npos, "saved off:\n%s", Read(file).c_str());
        CHECK(settings::Load(file) && !settings::GetBool(Id::SharpUi), "read back off");
        setenv("NFSMW_SHARP_UI", "1", 1);
        CHECK(settings::Load(file) && settings::GetBool(Id::SharpUi) && settings::EnvOverride(Id::SharpUi), "NFSMW_SHARP_UI=1 wins for the run");
        CHECK(settings::Save(file) && Read(file).find("sharper_ui = false") != std::string::npos, "the override isn't saved:\n%s", Read(file).c_str());
        setenv("NFSMW_SHARP_UI", "0", 1);
        Write(file, "");
        CHECK(settings::Load(file) && !settings::GetBool(Id::SharpUi), "NFSMW_SHARP_UI=0 turns it off");
        unsetenv("NFSMW_SHARP_UI");
    }

    // Movie Upscaling (video/upscale.h): Original, Sharper (the default);
    // [video].movie_upscaling, NFSMW_MOVIE_UPSCALE=original|sharper.
    {
        const settings::Option& m = settings::Info(Id::MovieUpscaling);
        CHECK(m.category == settings::Category::Graphics && m.type == settings::Type::Enum && m.apply == settings::Apply::Live && !m.hidden,
            "Movie Upscaling: a live Graphics choice");
        CHECK(std::string(m.key) == "movie_upscaling" && std::string(m.env) == "NFSMW_MOVIE_UPSCALE", "[video].movie_upscaling, NFSMW_MOVIE_UPSCALE");
        CHECK(m.entries.size() == 2, "%zu entries", m.entries.size());
        if (m.entries.size() == 2)
        {
            CHECK(m.entries[0].value == int32_t(settings::MovieUpscalingMode::Original) && std::string(m.entries[0].token) == "original",
                "first: original");
            CHECK(m.entries[1].value == int32_t(settings::MovieUpscalingMode::Sharper) && std::string(m.entries[1].token) == "sharper",
                "second: sharper");
        }
        Write(file, "");
        CHECK(settings::Load(file) && settings::GetInt(Id::MovieUpscaling) == int32_t(settings::MovieUpscalingMode::Sharper), "Sharper by default");
        Write(file, "[video]\nmovie_upscaling = \"original\"\n");
        CHECK(settings::Load(file) && settings::GetInt(Id::MovieUpscaling) == int32_t(settings::MovieUpscalingMode::Original), "original read");
        setenv("NFSMW_MOVIE_UPSCALE", "sharper", 1);
        CHECK(settings::Load(file) && settings::GetInt(Id::MovieUpscaling) == int32_t(settings::MovieUpscalingMode::Sharper),
            "NFSMW_MOVIE_UPSCALE=sharper wins for the run");
        unsetenv("NFSMW_MOVIE_UPSCALE");
    }

    std::filesystem::remove_all(dir);
    if (s_failures)
    {
        printf("%d check(s) FAILED\n", s_failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
