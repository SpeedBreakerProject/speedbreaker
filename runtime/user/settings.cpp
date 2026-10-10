// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See settings.h.
#include "settings.h"
#include <user/paths.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <TargetConditionals.h>  // TARGET_OS_IOS: __APPLE__ alone means macOS or iOS
#endif

// toml++ (header-only), as vendored by XenonRecomp. The fallback path keeps
// this file building before runtime/CMakeLists.txt adds the include
// directory (the runtime globs every .cpp under runtime/).
// Its v3.4 literal operators warn under clang 20+ unless it's a system header.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-warning-option"
#pragma clang diagnostic ignored "-Wdeprecated-literal-operator"
#if __has_include(<toml++/toml.hpp>)
#include <toml++/toml.hpp>
#else
#include "../../tools/XenonRecomp/thirdparty/tomlplusplus/include/toml++/toml.hpp"
#endif
#pragma clang diagnostic pop

namespace settings
{
    namespace
    {
        constexpr EnumEntry kScalingEntries[] = {
            { int32_t(ScalingMode::Bicubic), "bicubic", "Bicubic" },
            { int32_t(ScalingMode::Linear), "linear", "Bilinear" },
        };

        // Frame Rate: the game's frames per second (60, or 30 on a GPU that
        // can't hold 60: one guest vblank every other refresh, two world
        // steps a frame, so the game keeps its speed). Auto switches between
        // the two (video/frame_rate.h). First, as the other Autos are.
        constexpr EnumEntry kFrameRateEntries[] = {
            { kFrameRateAuto, "auto", "Auto" },
            { 60, "60", "60 fps" },
            { 30, "30", "30 fps" },
        };

        // Screen Resolution (iOS): the drawables at this percentage of the
        // screen's pixels each way (video/presenter.cpp's present scale).
        constexpr EnumEntry kPresentScaleEntries[] = {
            { 100, "100", "Full" },
            { 75, "75", "75%" },
        };

        // An iOS app always has the whole screen (video::FullscreenLocked):
        // the Fullscreen row would change nothing there.
#if defined(__APPLE__) && TARGET_OS_IOS
        constexpr bool kFullscreenHidden = true;
#else
        constexpr bool kFullscreenHidden = false;
#endif
        // Screen Resolution scales an iOS view (Core Animation scales it up
        // to the screen); elsewhere the window's size is the player's.
        constexpr bool kPresentScaleHidden = !kFullscreenHidden;

        constexpr EnumEntry kAspectEntries[] = {
            { int32_t(AspectMode::Auto), "auto", "Fill Screen" },
            { int32_t(AspectMode::Widescreen), "16:9", "16:9" },
        };

        constexpr EnumEntry kResolutionEntries[] = {
            { kResolutionAuto, "auto", "Auto" },
            { 1, "1x", "1x (720p)" },
            { 2, "2x", "2x (1440p)" },
            { 3, "3x", "3x (2160p)" },
        };

        constexpr EnumEntry kAnisotropyEntries[] = {
            { kAnisotropyAuto, "auto", "Auto" },
            { 1, "off", "Off" },
            { 2, "2x", "2x" },
            { 4, "4x", "4x" },
            { 8, "8x", "8x" },
            { 16, "16x", "16x" },
            { kAnisotropyGame, "game", "Original" },
        };

        // Defaults are what the runtime did before settings existed, so an
        // empty settings.toml changes nothing.
        constexpr Option kOptions[] = {
            // Video
            { .id = Id::Fullscreen, .category = Category::Display, .type = Type::Bool, .apply = Apply::Live, .hidden = kFullscreenHidden,
              .table = "video", .key = "fullscreen", .label = "Fullscreen",
              .description = "Fill the screen with a borderless window. Off: a resizable window.",
              .env = "NFSMW_FULLSCREEN", .defaultValue = 0 },
            { .id = Id::VSync, .category = Category::Display, .type = Type::Bool, .apply = Apply::Live,
              .table = "video", .key = "vsync", .label = "V-Sync",
              .description = "Show each frame on a display refresh. Off can tear, but frames appear sooner.",
              .env = "NFSMW_VSYNC", .defaultValue = 1 },
            { .id = Id::FrameRate, .category = Category::Display, .type = Type::Enum, .apply = Apply::Live,
              .table = "video", .key = "frame_rate", .label = "Frame Rate",
              .description = "Auto: 60, or 30 while the device is hot or can't hold 60. A steady 30 feels smoother than a shaky 60 (the Steam Frame's headset). The game keeps its speed.",
              .env = "NFSMW_FRAME_RATE", .defaultValue = 60, .entries = kFrameRateEntries },
            { .id = Id::PresentScale, .category = Category::Display, .type = Type::Enum, .apply = Apply::Live, .hidden = kPresentScaleHidden,
              .table = "video", .key = "present_scale", .label = "Screen Resolution",
              .description = "75%: the picture and these menus at 3/4 of the screen's resolution each way, scaled up by iOS: a little softer, a little less GPU work.",
              .env = "NFSMW_SCREEN_RESOLUTION", .defaultValue = 100, .entries = kPresentScaleEntries },
            { .id = Id::AntiAliasing, .category = Category::Graphics, .type = Type::Bool, .apply = Apply::Live,
              .table = "video", .key = "anti_aliasing", .label = "Anti-Aliasing",
              .description = "Smooth jagged edges (the 360 used 2x MSAA; this is an FXAA-style pass).",
              .env = "NFSMW_AA", .defaultValue = 1 },
            { .id = Id::Scaling, .category = Category::Graphics, .type = Type::Enum, .apply = Apply::Live,
              .table = "video", .key = "scaling", .label = "Upscaling",
              .description = "How the 720p image is scaled to the window. Bicubic is sharper, bilinear softer.",
              .env = "NFSMW_SCALING", .defaultValue = double(ScalingMode::Bicubic), .entries = kScalingEntries },
            { .id = Id::AspectRatio, .category = Category::Display, .type = Type::Enum, .apply = Apply::Live,
              .table = "video", .key = "aspect_ratio", .label = "Aspect Ratio",
              .description = "Fill other screen shapes: a wider view on an ultrawide, a taller one on a Steam Deck or a 4:3 iPad, the HUD at 16:9. Or 16:9 with bars.",
              .env = "NFSMW_ASPECT", .defaultValue = double(AspectMode::Auto), .entries = kAspectEntries },
            { .id = Id::Sharpening, .category = Category::Graphics, .type = Type::Float, .apply = Apply::Live,
              .table = "video", .key = "sharpening", .label = "Sharpening",
              .description = "Extra sharpness for bicubic upscaling. 0 turns it off.",
              .env = "NFSMW_SHARPEN", .defaultValue = 0.3, .min = 0, .max = 1, .step = 0.05 },
            { .id = Id::Anisotropy, .category = Category::Graphics, .type = Type::Enum, .apply = Apply::Live,
              .table = "video", .key = "anisotropic_filtering", .label = "Anisotropic Filtering",
              .description = "Keeps roads and walls sharp at a distance. Auto: 16x on a dedicated GPU or an M-series iPad, 4x on other integrated ones.",
              .env = "NFSMW_ANISOTROPY", .defaultValue = kAnisotropyAuto, .min = 1, .max = 16, .entries = kAnisotropyEntries },
            { .id = Id::Mipmaps, .category = Category::Graphics, .type = Type::Bool, .apply = Apply::Live,
              .table = "video", .key = "mipmaps", .label = "Mipmaps",
              .description = "Use the game's smaller textures in the distance, so they don't shimmer.",
              .env = "NFSMW_TEXTURE_MIPS", .defaultValue = 1 },
            { .id = Id::InternalResolution, .category = Category::Graphics, .type = Type::Enum, .apply = Apply::Live,
              .table = "video", .key = "internal_resolution", .label = "Internal Resolution",
              .description = "Render the 3D scene above the 360's 720p. Auto: from the screen (1440p: 2x, or 3x2 filling an ultrawide).",
              .env = "NFSMW_RESOLUTION", .defaultValue = kResolutionAuto, .entries = kResolutionEntries },
            { .id = Id::WindowWidth, .category = Category::Display, .type = Type::Int, .apply = Apply::Restart, .hidden = true,
              .table = "video", .key = "window_width", .label = "Window Width",
              .description = "The window's width when the game starts.",
              .env = "NFSMW_WINDOW", .envField = 1, .defaultValue = 1280, .min = 320, .max = 16384, .step = 1, .unit = "px" },
            { .id = Id::WindowHeight, .category = Category::Display, .type = Type::Int, .apply = Apply::Restart, .hidden = true,
              .table = "video", .key = "window_height", .label = "Window Height",
              .description = "The window's height when the game starts.",
              .env = "NFSMW_WINDOW", .envField = 2, .defaultValue = 720, .min = 200, .max = 16384, .step = 1, .unit = "px" },
            // Audio
            { .id = Id::MasterVolume, .category = Category::Audio, .type = Type::Int, .apply = Apply::Live,
              .table = "audio", .key = "master_volume", .label = "Master Volume",
              .description = "The volume of all game audio.",
              .env = "NFSMW_VOLUME", .defaultValue = 100, .min = 0, .max = 100, .step = 5, .unit = "%" },
            { .id = Id::Mute, .category = Category::Audio, .type = Type::Bool, .apply = Apply::Live,
              .table = "audio", .key = "mute", .label = "Mute",
              .description = "Silence all game audio.",
              .env = "NFSMW_MUTE", .defaultValue = 0 },
            { .id = Id::MuteInBackground, .category = Category::Audio, .type = Type::Bool, .apply = Apply::Live,
              .table = "audio", .key = "mute_in_background", .label = "Mute in Background",
              .description = "Silence the game while its window isn't focused.",
              .env = "NFSMW_MUTE_UNFOCUSED", .defaultValue = 0 },
            // Input
            { .id = Id::StickDeadzone, .category = Category::Input, .type = Type::Float, .apply = Apply::Live,
              .table = "input", .key = "stick_deadzone", .label = "Stick Deadzone",
              .description = "How far a stick moves before the game sees it. Raise it if the car steers by itself.",
              .env = "NFSMW_DEADZONE", .defaultValue = 0.08, .min = 0, .max = 0.9, .step = 0.01, .unit = "%" },
            { .id = Id::TriggerDeadzone, .category = Category::Input, .type = Type::Float, .apply = Apply::Live,
              .table = "input", .key = "trigger_deadzone", .label = "Trigger Deadzone",
              .description = "How far a trigger is pressed before it counts.",
              .env = "NFSMW_TRIGGER_DEADZONE", .defaultValue = 0.02, .min = 0, .max = 0.9, .step = 0.01, .unit = "%" },
            { .id = Id::Vibration, .category = Category::Input, .type = Type::Bool, .apply = Apply::Live,
              .table = "input", .key = "vibration", .label = "Vibration",
              .description = "Controller rumble.",
              .env = "NFSMW_VIBRATION", .defaultValue = 1 },
            // Advanced
            { .id = Id::AsyncShaders, .category = Category::Advanced, .type = Type::Bool, .apply = Apply::Live,
              .table = "advanced", .key = "async_shaders", .label = "Asynchronous Shaders",
              .description = "Skip an object for a frame or two while its shader compiles, instead of stuttering.",
              .env = "NFSMW_ASYNC_PIPELINES", .defaultValue = 1 },
            { .id = Id::PerformanceOverlay, .category = Category::Advanced, .type = Type::Bool, .apply = Apply::Live,
              .table = "advanced", .key = "performance_overlay", .label = "Performance Overlay",
              .description = "Show the frame rate and frame time in a corner of the screen.",
              .env = "NFSMW_PERF_OVERLAY", .defaultValue = 0 },
        };
        static_assert(std::size(kOptions) == kCount, "one row per settings::Id");

        consteval bool TableIsConsistent()
        {
            for (size_t i = 0; i < kCount; i++)
            {
                const Option& o = kOptions[i];
                if (size_t(o.id) != i)
                    return false;  // rows in Id order
                // A table's keys are contiguous, so Save writes each table once.
                if (i > 0 && std::string_view(kOptions[i - 1].table) != o.table)
                    for (size_t j = 0; j < i; j++)
                        if (std::string_view(kOptions[j].table) == o.table)
                            return false;
                bool numeric = o.type == Type::Int || o.type == Type::Float;
                if (numeric && !(o.min < o.max && o.step > 0 && o.defaultValue >= o.min && o.defaultValue <= o.max))
                    return false;
                if (o.type == Type::Bool && o.defaultValue != 0 && o.defaultValue != 1)
                    return false;
                if (o.type == Type::Enum)
                {
                    bool found = false;
                    for (const EnumEntry& e : o.entries)
                        found |= double(e.value) == o.defaultValue;
                    if (!found)
                        return false;
                }
                if (o.env == nullptr || o.label == nullptr || o.description == nullptr)
                    return false;
            }
            return true;
        }
        static_assert(TableIsConsistent());

        // Writers (Set, Load, the first early read) hold s_mutex; the values
        // themselves are atomics, so readers never lock.
        std::mutex s_mutex;
        std::atomic<double> s_stored[kCount]{};    // what settings.toml holds / the menu set
        std::atomic<double> s_envValue[kCount]{};  // this run's environment override
        std::atomic<bool> s_envSet[kCount]{};
        double s_baseline[kCount]{};               // stored values as last loaded or saved (Dirty)
        std::atomic<double> s_deviceDefault[kCount]{};  // SetDeviceDefault's, where s_hasDeviceDefault
        std::atomic<bool> s_hasDeviceDefault[kCount]{};
        bool s_frozen = false;                     // Restart options hold their launch values (first Load)
        std::mutex s_saveMutex;                    // whole saves, so the last snapshot is the last file

        // Option i's default on this device (DefaultValue).
        double Default(size_t i)
        {
            return s_hasDeviceDefault[i].load(std::memory_order_acquire) ? s_deviceDefault[i].load(std::memory_order_relaxed)
                                                                          : kOptions[i].defaultValue;
        }

        std::string& LastErrorText()
        {
            static std::string text;  // under s_mutex; LastError()
            return text;
        }

        std::filesystem::path& LoadedPath()
        {
            static std::filesystem::path path;  // under s_mutex
            return path;
        }

        // The [app] table: the game's own records, not options (the menu
        // never shows them). last_version: the version that ran last
        // (user/version_check.h). Under s_mutex.
        constexpr const char* kAppTable = "app";
        constexpr const char* kLastVersionKey = "last_version";
        std::string& LastVersionText()
        {
            static std::string text;
            return text;
        }
        bool s_fileFound = false;  // the last Load found a settings.toml
        bool s_fileLoaded = false; // ...and read and parsed it

        std::string AppTable(const std::string& lastVersion)
        {
            // Only what a version looks like (a hand edit could hold a quote
            // or a newline, which would break the file).
            if (lastVersion.empty() || lastVersion.size() > 64 ||
                !std::all_of(lastVersion.begin(), lastVersion.end(), [](char c) { return std::isalnum(uint8_t(c)) || c == '.' || c == '-' || c == '+'; }))
                return "";
            return std::format("\n[{}]\n# The SpeedBreaker version that ran last (the game keeps this up to date).\n{} = \"{}\"\n",
                kAppTable, kLastVersionKey, lastVersion);
        }

        // `text` (a settings.toml) without its [app] table: from a line that
        // is [app] to the next table's line (or the end).
        std::string WithoutAppTable(std::string_view text)
        {
            std::string out;
            bool inApp = false;
            while (!text.empty())
            {
                size_t end = text.find('\n');
                std::string_view line = text.substr(0, end == std::string_view::npos ? text.size() : end + 1);
                text.remove_prefix(line.size());
                std::string_view trimmed = line;
                while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t'))
                    trimmed.remove_prefix(1);
                if (trimmed.starts_with("["))
                {
                    size_t close = trimmed.find(']');
                    std::string_view name = close == std::string_view::npos ? std::string_view() : trimmed.substr(1, close - 1);
                    while (!name.empty() && name.front() == ' ')
                        name.remove_prefix(1);
                    while (!name.empty() && name.back() == ' ')
                        name.remove_suffix(1);
                    inApp = name == kAppTable;
                }
                if (!inApp)
                    out += line;
            }
            while (out.size() >= 2 && out.ends_with("\n\n"))
                out.pop_back();
            return out;
        }

        std::mutex s_listenerMutex;
        std::vector<std::pair<uint32_t, std::shared_ptr<const Listener>>> s_listeners;
        uint32_t s_nextHandle = 1;

        std::string_view Trim(std::string_view s)
        {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\n' || s.front() == '\r'))
                s.remove_prefix(1);
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r'))
                s.remove_suffix(1);
            return s;
        }

        bool EqualsIgnoreCase(std::string_view a, std::string_view b)
        {
            return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                return std::tolower(uint8_t(x)) == std::tolower(uint8_t(y));
            });
        }

        // strtod over the whole text; no NaN or infinity.
        std::optional<double> ParseNumber(std::string_view text)
        {
            std::string s(Trim(text));
            if (s.empty())
                return {};
            char* end = nullptr;
            errno = 0;
            double v = std::strtod(s.c_str(), &end);
            if (end != s.c_str() + s.size() || errno == ERANGE || !std::isfinite(v))
                return {};
            return v;
        }

        std::optional<double> ParseBool(std::string_view text)
        {
            text = Trim(text);
            for (const char* t : { "true", "on", "yes" })
                if (EqualsIgnoreCase(text, t))
                    return 1.0;
            for (const char* f : { "false", "off", "no" })
                if (EqualsIgnoreCase(text, f))
                    return 0.0;
            // Numbers as the old getenv checks read them: 0 off, anything else on.
            if (auto n = ParseNumber(text))
                return *n != 0.0 ? 1.0 : 0.0;
            return {};
        }

        const EnumEntry* FindEntry(const Option& o, double value)
        {
            for (const EnumEntry& e : o.entries)
                if (double(e.value) == value)
                    return &e;
            return nullptr;
        }

        const EnumEntry* FindToken(const Option& o, std::string_view token)
        {
            token = Trim(token);
            for (const EnumEntry& e : o.entries)
                if (EqualsIgnoreCase(token, e.token))
                    return &e;
            return nullptr;
        }

        // A number for a numeric enum (anisotropy): clamped to its range,
        // then the entry at or below it, as NFSMW_ANISOTROPY=<1..16> read.
        std::optional<double> EnumFromNumber(const Option& o, double n)
        {
            if (!(o.min < o.max))
                return {};  // tokens only
            n = std::clamp(std::round(n), o.min, o.max);
            const EnumEntry* best = nullptr;
            for (const EnumEntry& e : o.entries)
                if (e.value >= o.min && e.value <= n && (!best || e.value > best->value))
                    best = &e;
            if (!best)
                return {};
            return double(best->value);
        }

        // A value this option can hold: Bool 0/1, Int rounded and clamped,
        // Float clamped and rounded to 1e-4 (a menu slider's float steps
        // then save as 0.35, not 0.3499999940395355), Enum an entry's value.
        // Adding 0.0 turns -0.0 (a file's "-0.0", NFSMW_DEADZONE=-0) into 0,
        // which would otherwise be saved as "-0.0" and shown as "-0%".
        std::optional<double> Normalize(const Option& o, double v)
        {
            if (std::isnan(v))
                return {};
            switch (o.type)
            {
            case Type::Bool:
                return v != 0.0 ? 1.0 : 0.0;
            case Type::Int:
                return std::clamp(std::round(v), o.min, o.max) + 0.0;
            case Type::Float:
                return std::clamp(std::round(std::clamp(v, o.min, o.max) * 1e4) / 1e4, o.min, o.max) + 0.0;
            case Type::Enum:
                if (const EnumEntry* e = FindEntry(o, v))
                    return double(e->value);
                return {};
            }
            return {};
        }

        // What the option accepts, for log lines and the file's comments.
        std::string RangeText(const Option& o)
        {
            switch (o.type)
            {
            case Type::Bool:
                return "true or false";
            case Type::Int:
                return std::format("a whole number from {} to {}", o.min, o.max);
            case Type::Float:
                return std::format("a number from {} to {}", o.min, o.max);
            case Type::Enum:
            {
                std::string text = "one of ";
                for (size_t e = 0; e < o.entries.size(); e++)
                    text += std::format("{}\"{}\"", e ? ", " : "", o.entries[e].token);
                if (o.min < o.max)
                    text += std::format(", or a number from {} to {}", o.min, o.max);
                return text;
            }
            }
            return "";
        }

        // Text (the environment, or a string in the file) as a value of the
        // option's kind, not yet clamped: Bool 0/1, Int and Float the
        // number, Enum an entry's value ("16x", "linear", or a number for a
        // numeric enum).
        std::optional<double> RawFromText(const Option& o, std::string_view text)
        {
            switch (o.type)
            {
            case Type::Bool:
                return ParseBool(text);
            case Type::Int:
            case Type::Float:
                return ParseNumber(text);
            case Type::Enum:
                if (const EnumEntry* e = FindToken(o, text))
                    return double(e->value);
                if (auto n = ParseNumber(text))
                    return EnumFromNumber(o, *n);
                return {};
            }
            return {};
        }

        // A TOML value, the same way: strings through RawFromText ("on",
        // "16x"), numbers and booleans as they are.
        std::optional<double> RawFromToml(const Option& o, const toml::node& node)
        {
            if (auto s = node.as_string())
                return RawFromText(o, s->get());
            if (auto b = node.as_boolean())
                return o.type == Type::Bool ? std::optional<double>(b->get() ? 1.0 : 0.0) : std::nullopt;
            std::optional<double> n;
            if (auto i = node.as_integer())
                n = double(i->get());
            else if (auto f = node.as_floating_point(); f && std::isfinite(f->get()))
                n = f->get();
            if (!n)
                return {};
            switch (o.type)
            {
            case Type::Bool:
                return *n != 0.0 ? 1.0 : 0.0;
            case Type::Int:
            case Type::Float:
                return n;
            case Type::Enum:
                return EnumFromNumber(o, *n);
            }
            return {};
        }

        // A raw value normalised (clamped, rounded), with a log line naming
        // `source` when it couldn't be used or had to be changed.
        std::optional<double> Accept(const Option& o, std::optional<double> raw, const std::string& source, bool log)
        {
            if (!raw)
            {
                if (log)
                    fprintf(stderr, "[settings] %s: not %s; ignored\n", source.c_str(), RangeText(o).c_str());
                return {};
            }
            std::optional<double> v = Normalize(o, *raw);
            if (log && v && (o.type == Type::Int || o.type == Type::Float) && std::abs(*v - *raw) > 1e-4)
            {
                if (*raw < o.min || *raw > o.max)
                    fprintf(stderr, "[settings] %s: out of range (%s to %s); using %s\n", source.c_str(),
                        std::format("{}", o.min).c_str(), std::format("{}", o.max).c_str(), std::format("{}", *v).c_str());
                else
                    fprintf(stderr, "[settings] %s: rounded to %s\n", source.c_str(), std::format("{}", *v).c_str());
            }
            return v;
        }

        std::string NodeText(const toml::node& node)
        {
            if (auto b = node.as_boolean())
                return b->get() ? "true" : "false";
            if (auto i = node.as_integer())
                return std::format("{}", i->get());
            if (auto f = node.as_floating_point())
                return std::format("{}", f->get());
            if (auto s = node.as_string())
                return std::format("\"{}\"", s->get());
            if (node.is_table())
                return "(a table)";
            if (node.is_array())
                return "(an array)";
            return "(a date or time)";
        }

        // The n-th (from 1) 'x'-separated field: NFSMW_WINDOW=3440x1440.
        std::optional<std::string_view> EnvField(std::string_view value, int field)
        {
            if (field <= 0)
                return value;
            for (int n = 1; ; n++)
            {
                size_t x = value.find_first_of("xX");
                if (n == field)
                    return value.substr(0, x);
                if (x == std::string_view::npos)
                    return {};
                value.remove_prefix(x + 1);
            }
        }

        // Reads option i's environment variable into s_envValue/s_envSet.
        // Under s_mutex. The runtime never calls setenv, so getenv is safe.
        void ReadEnvLocked(size_t i, bool log)
        {
            const Option& o = kOptions[i];
            s_envSet[i].store(false, std::memory_order_relaxed);
            const char* v = std::getenv(o.env);
            if (!v)
                return;
            std::optional<std::string_view> field = EnvField(v, o.envField);
            if (!field)
                return;
            std::string source = std::format("{}={} ({}.{})", o.env, v, o.table, o.key);
            // Quiet before Load(): Load() reads it again and logs.
            std::optional<double> value = Accept(o, RawFromText(o, *field), source, log);
            if (!value)
                return;
            s_envValue[i].store(*value, std::memory_order_relaxed);
            s_envSet[i].store(true, std::memory_order_relaxed);
            if (log)
                fprintf(stderr, "[settings] %s overrides %s.%s for this run\n", o.env, o.table, o.key);
        }

        // Publishes option i's effective value. Under s_mutex. Returns
        // whether it changed.
        bool RecomputeLocked(size_t i)
        {
            if (kOptions[i].apply == Apply::Restart && s_frozen)
                return false;  // keeps its launch value until the next run
            double v = s_envSet[i].load(std::memory_order_relaxed) ? s_envValue[i].load(std::memory_order_relaxed)
                                                                    : s_stored[i].load(std::memory_order_relaxed);
            return detail::g_effective[i].exchange(v, std::memory_order_relaxed) != v;
        }

        // Defaults plus the environment, for anything that runs before
        // Load(). Under s_mutex.
        void EnsureInitLocked(const char* reason)
        {
            if (detail::g_state.load(std::memory_order_relaxed) != 0)
                return;
            fprintf(stderr, "[settings] %s before settings::Load(): the environment and defaults only\n", reason);
            for (size_t i = 0; i < kCount; i++)
            {
                s_stored[i].store(Default(i), std::memory_order_relaxed);
                s_baseline[i] = Default(i);
                ReadEnvLocked(i, false);
                RecomputeLocked(i);
            }
            detail::g_generation.fetch_add(1, std::memory_order_release);
            detail::g_state.store(1, std::memory_order_release);
        }

        void EnsureInit(const char* reason)
        {
            if (detail::g_state.load(std::memory_order_acquire) != 0)
                return;
            std::lock_guard lock(s_mutex);
            EnsureInitLocked(reason);
        }

        void Notify(std::span<const Id> ids)
        {
            if (ids.empty())
                return;
            // Copied so a listener can Subscribe/Unsubscribe without deadlock.
            std::vector<std::shared_ptr<const Listener>> listeners;
            {
                std::lock_guard lock(s_listenerMutex);
                for (auto& [handle, listener] : s_listeners)
                    listeners.push_back(listener);
            }
            for (Id id : ids)
                for (auto& listener : listeners)
                    (*listener)(id);
        }

        // settings.toml is ~3 KB; anything this big isn't one.
        constexpr size_t kMaxFileSize = 1 << 20;

        enum class ReadStatus { Ok, Missing, Failed };

        // Reads a settings file into `out`; on Failed, `error` says why.
        // Only a regular file: opening a FIFO blocks until a writer comes
        // (the game would hang at launch), and /dev/zero never ends. O_NONBLOCK
        // makes the open itself return at once; it changes nothing for reads
        // of a regular file.
        ReadStatus ReadFile(const std::filesystem::path& path, std::string& out, std::string& error)
        {
            int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
            if (fd < 0)
            {
                if (errno == ENOENT)
                    return ReadStatus::Missing;
                error = strerror(errno);
                return ReadStatus::Failed;
            }
            struct stat st;
            if (fstat(fd, &st) != 0)
                error = strerror(errno);
            else if (S_ISDIR(st.st_mode))
                error = "it's a folder";
            else if (!S_ISREG(st.st_mode))
                error = "it isn't a regular file";
            else if (uint64_t(st.st_size) > kMaxFileSize)
                error = std::format("it's {} KB, too big for a settings file", uint64_t(st.st_size) / 1024);
            if (!error.empty())
            {
                close(fd);
                return ReadStatus::Failed;
            }
            char buffer[4096];
            for (;;)
            {
                ssize_t n = read(fd, buffer, sizeof(buffer));
                if (n < 0 && errno == EINTR)
                    continue;
                if (n < 0)
                {
                    error = strerror(errno);
                    break;
                }
                if (n == 0)
                    break;
                out.append(buffer, size_t(n));
                if (out.size() > kMaxFileSize)  // grew after the fstat
                {
                    error = "it's too big for a settings file";
                    break;
                }
            }
            close(fd);
            return error.empty() ? ReadStatus::Ok : ReadStatus::Failed;
        }

        std::string TomlValue(const Option& o, double v)
        {
            switch (o.type)
            {
            case Type::Bool:
                return v != 0.0 ? "true" : "false";
            case Type::Int:
                return std::format("{}", int64_t(v));
            case Type::Float:
            {
                std::string s = std::format("{}", v);  // shortest round trip
                if (s.find_first_of(".e") == std::string::npos)
                    s += ".0";  // a TOML float, not an integer
                return s;
            }
            case Type::Enum:
                if (const EnumEntry* e = FindEntry(o, v))
                    return std::format("\"{}\"", e->token);
                if (const EnumEntry* e = FindEntry(o, Default(size_t(o.id))))
                    return std::format("\"{}\"", e->token);
                return std::format("\"{}\"", FindEntry(o, o.defaultValue)->token);
            }
            return "";
        }

        std::string Serialize(const std::array<double, kCount>& values)
        {
            std::string out =
                "# SpeedBreaker settings, written by the game's settings menu. Hand edits\n"
                "# are fine while the game is closed; unknown keys are dropped on the next\n"
                "# save. The environment variable after each option overrides it for one\n"
                "# run without changing this file.\n";
            const char* table = "";
            for (size_t i = 0; i < kCount; i++)
            {
                const Option& o = kOptions[i];
                if (std::string_view(o.table) != table)
                {
                    table = o.table;
                    out += std::format("\n[{}]\n", table);
                }
                else
                    out += "\n";
                std::string range = RangeText(o);
                range[0] = char(std::toupper(uint8_t(range[0])));
                out += std::format("# {}: {}\n# {}. ({}{})\n", o.label, o.description, range, o.env,
                    o.apply == Apply::Restart ? "; applies at the next launch" : "");
                out += std::format("{} = {}\n", o.key, TomlValue(o, values[i]));
            }
            std::string lastVersion;
            {
                std::lock_guard lock(s_mutex);
                lastVersion = LastVersionText();
            }
            return out + AppTable(lastVersion);
        }

        // Where a save of `path` goes: through a symlinked settings.toml
        // (dotfiles kept elsewhere) to the file it points at, since a rename
        // over the link would replace it with a plain file. A relative link
        // is relative to its own directory, as the kernel reads it; a
        // dangling one still names where the file belongs.
        std::filesystem::path SaveTarget(const std::filesystem::path& path)
        {
            std::filesystem::path p = path;
            for (int hops = 0; hops < 8; hops++)  // a loop of links: the rename replaces the last one
            {
                std::error_code ec;
                if (!std::filesystem::is_symlink(std::filesystem::symlink_status(p, ec)))
                    break;
                std::filesystem::path link = std::filesystem::read_symlink(p, ec);
                if (ec || link.empty())
                    break;
                p = link.is_absolute() ? link : p.parent_path() / link;
            }
            return p;
        }

        // Removes what crashed saves left next to `target`: settings.toml.tmp
        // (the name before per-process ones) and any settings.toml.<pid>.tmp
        // a minute old. A save takes milliseconds, so a younger one may be
        // another instance's save in progress.
        void RemoveStaleTemps(const std::filesystem::path& target)
        {
            std::string prefix = target.filename().string() + ".";
            std::filesystem::path dir = target.parent_path().empty() ? std::filesystem::path(".") : target.parent_path();
            auto now = std::filesystem::file_time_type::clock::now();
            std::error_code ec;
            for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
            {
                std::string file = it->path().filename().string();
                if (file != prefix + "tmp")
                {
                    if (file.size() <= prefix.size() + 4 || !file.starts_with(prefix) || !file.ends_with(".tmp"))
                        continue;
                    std::string_view pid = std::string_view(file).substr(prefix.size(), file.size() - prefix.size() - 4);
                    if (!std::all_of(pid.begin(), pid.end(), [](char c) { return c >= '0' && c <= '9'; }))
                        continue;
                    std::error_code timeError;
                    auto written = std::filesystem::last_write_time(it->path(), timeError);
                    if (timeError || now - written < std::chrono::minutes(1))
                        continue;
                }
                std::error_code removeError;
                if (std::filesystem::remove(it->path(), removeError))
                    fprintf(stderr, "[settings] removed %s, left by a save that didn't finish\n", file.c_str());
            }
        }

        // Writes `path` so that it's always either the old file or the whole
        // new one: a temporary file in the same directory (rename is atomic
        // only within one file system), flushed to disk before the rename
        // (else a crash can leave the renamed file empty), then the
        // directory, so the rename itself survives a power cut. The
        // temporary file is this process's own (settings.toml.<pid>.tmp,
        // created exclusively): with one shared name, two instances of the
        // game saving at once write into the same file, and one renames the
        // other's half-written text into place. Returns "" or why it failed.
        std::string WriteAtomically(const std::filesystem::path& path, std::string_view text)
        {
            std::filesystem::path target = SaveTarget(path);
            std::error_code ec;
            std::filesystem::path dir = target.parent_path();
            if (!dir.empty())
            {
                std::filesystem::create_directories(dir, ec);
                if (ec)
                    return std::format("can't create {}: {}", dir.string(), ec.message());
            }
            RemoveStaleTemps(target);
            std::filesystem::path temp = target;
            temp += std::format(".{}.tmp", long(getpid()));
            int fd = open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
            if (fd < 0 && errno == EEXIST)
            {
                // Saves within a process are serialised, so this was left by
                // a crashed process that had the same pid.
                unlink(temp.c_str());
                fd = open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
            }
            if (fd < 0)
                return std::format("can't write {}: {}", target.string(), strerror(errno));
            int error = 0;
            for (size_t done = 0; !error && done < text.size();)
            {
                ssize_t n = write(fd, text.data() + done, text.size() - done);
                if (n > 0)
                    done += size_t(n);
                else if (n < 0 && errno == EINTR)
                    continue;
                else
                    error = n == 0 ? EIO : errno;
            }
#ifdef __APPLE__
            // fsync on macOS doesn't flush the drive's cache; F_FULLFSYNC does
            // (not every file system supports it).
            if (!error && fcntl(fd, F_FULLFSYNC) != 0 && fsync(fd) != 0)
                error = errno;
#else
            if (!error && fsync(fd) != 0)
                error = errno;
#endif
            if (close(fd) != 0 && !error)
                error = errno;
            if (error)
            {
                unlink(temp.c_str());
                return std::format("can't write {}: {}", target.string(), strerror(error));
            }
            if (rename(temp.c_str(), target.c_str()) != 0)
            {
                error = errno;
                unlink(temp.c_str());
                return std::format("can't replace {}: {}", target.string(), strerror(error));
            }
            if (int dirFd = open(dir.empty() ? "." : dir.c_str(), O_RDONLY | O_CLOEXEC); dirFd >= 0)
            {
                fsync(dirFd);  // best effort: some file systems refuse it on directories
                close(dirFd);
            }
            return "";
        }

        int Decimals(double step)
        {
            int d = 0;
            for (double s = step; d < 6 && std::abs(s - std::round(s)) > 1e-9; s *= 10)
                d++;
            return d;
        }
    }

    namespace detail
    {
        constinit std::atomic<uint8_t> g_state{ 0 };
        constinit std::atomic<double> g_effective[kCount]{};
        constinit std::atomic<uint64_t> g_generation{ 0 };
        static_assert(std::atomic<double>::is_always_lock_free);

        void InitBeforeLoad(Id first)
        {
            std::lock_guard lock(s_mutex);
            if (g_state.load(std::memory_order_relaxed) != 0)
                return;
            const Option& o = kOptions[size_t(first)];
            EnsureInitLocked(std::format("{}.{} was read", o.table, o.key).c_str());
        }
    }

    std::span<const Option> Options()
    {
        return kOptions;
    }

    const Option& Info(Id id)
    {
        return kOptions[size_t(id)];
    }

    const Option* Find(std::string_view table, std::string_view key)
    {
        for (const Option& o : kOptions)
            if (table == o.table && key == o.key)
                return &o;
        return nullptr;
    }

    const char* CategoryName(Category category)
    {
        switch (category)
        {
        case Category::Display: return "Display";
        case Category::Graphics: return "Graphics";
        case Category::Audio: return "Audio";
        case Category::Input: return "Input";
        case Category::Advanced: return "Advanced";
        default: return "";
        }
    }

    double GetStoredValue(Id id)
    {
        EnsureInit("a stored value was read");
        return s_stored[size_t(id)].load(std::memory_order_relaxed);
    }

    bool IsDefault(Id id)
    {
        return GetStoredValue(id) == Default(size_t(id));
    }

    void SetDeviceDefault(Id id, double value, std::string_view why)
    {
        size_t i = size_t(id);
        const Option& o = kOptions[i];
        std::optional<double> v = Normalize(o, value);
        if (!v)
            return;
        {
            std::lock_guard lock(s_mutex);
            s_deviceDefault[i].store(*v, std::memory_order_relaxed);
            s_hasDeviceDefault[i].store(true, std::memory_order_release);
        }
        fprintf(stderr, "[settings] %s.%s defaults to %s on this device (%s)%s\n", o.table, o.key, TomlValue(o, *v).c_str(),
            std::string(why).c_str(), detail::g_state.load(std::memory_order_acquire) == 2 ? "; too late for this run's stored value" : "");
    }

    double DefaultValue(Id id)
    {
        return Default(size_t(id));
    }

    const char* EnvOverride(Id id)
    {
        EnsureInit("an override was checked");
        return s_envSet[size_t(id)].load(std::memory_order_relaxed) ? kOptions[size_t(id)].env : nullptr;
    }

    bool RestartPending(Id id)
    {
        size_t i = size_t(id);
        EnsureInit("a restart was checked");
        return kOptions[i].apply == Apply::Restart && !s_envSet[i].load(std::memory_order_relaxed) &&
            s_stored[i].load(std::memory_order_relaxed) != detail::g_effective[i].load(std::memory_order_relaxed);
    }

    bool AnyRestartPending()
    {
        for (const Option& o : kOptions)
            if (!o.hidden && RestartPending(o.id))
                return true;
        return false;
    }

    bool Dirty()
    {
        std::lock_guard lock(s_mutex);
        EnsureInitLocked("settings were checked for changes");
        for (size_t i = 0; i < kCount; i++)
            if (s_stored[i].load(std::memory_order_relaxed) != s_baseline[i])
                return true;
        return false;
    }

    SetResult Set(Id id, double value)
    {
        size_t i = size_t(id);
        const Option& o = kOptions[i];
        std::optional<double> v = Normalize(o, value);
        if (!v)
            return SetResult::Invalid;
        bool changed;
        SetResult result;
        {
            std::lock_guard lock(s_mutex);
            EnsureInitLocked("a setting was set");
            if (s_stored[i].load(std::memory_order_relaxed) == *v)
                return SetResult::Unchanged;
            s_stored[i].store(*v, std::memory_order_relaxed);
            changed = RecomputeLocked(i);
            if (s_envSet[i].load(std::memory_order_relaxed))
                result = SetResult::Overridden;
            else
                result = detail::g_effective[i].load(std::memory_order_relaxed) == *v ? SetResult::Applied : SetResult::Deferred;
            detail::g_generation.fetch_add(1, std::memory_order_release);
        }
        if (changed)
            Notify({ &id, 1 });
        return result;
    }

    SetResult Reset(Id id)
    {
        return Set(id, Default(size_t(id)));
    }

    void ResetAll()
    {
        for (const Option& o : kOptions)
            Reset(o.id);
    }

    double StepValue(Id id, double value, int delta)
    {
        const Option& o = kOptions[size_t(id)];
        switch (o.type)
        {
        case Type::Bool:
            return delta == 0 ? (value != 0.0 ? 1.0 : 0.0) : value != 0.0 ? 0.0 : 1.0;
        case Type::Enum:
        {
            ptrdiff_t index = 0;
            for (size_t e = 0; e < o.entries.size(); e++)
                if (double(o.entries[e].value) == value)
                    index = ptrdiff_t(e);
            index = std::clamp<ptrdiff_t>(index + delta, 0, ptrdiff_t(o.entries.size()) - 1);
            return double(o.entries[size_t(index)].value);
        }
        case Type::Int:
        case Type::Float:
        {
            double v = std::isnan(value) ? Default(size_t(id)) : std::clamp(value, o.min, o.max);
            // Its place on the step grid, fractional between points. Within
            // float error of a point (0.3 / 0.05 is 5.999...) it's on it.
            double position = (v - o.min) / o.step;
            if (double nearest = std::round(position); std::abs(position - nearest) < 1e-6)
                position = nearest;
            // Between points, the first press goes only to the next point
            // that way: 58 goes to 60 or 55, not 65 or 50.
            double index = delta > 0 ? std::floor(position) + delta : delta < 0 ? std::ceil(position) + delta : position;
            return *Normalize(o, o.min + index * o.step);  // not NaN: v and step are finite
        }
        }
        return value;
    }

    std::string FormatValue(Id id, double value)
    {
        const Option& o = kOptions[size_t(id)];
        std::string_view unit = o.unit ? o.unit : "";
        switch (o.type)
        {
        case Type::Bool:
            return value != 0.0 ? "On" : "Off";
        case Type::Enum:
            if (const EnumEntry* e = FindEntry(o, value))
                return e->label;
            return "?";
        case Type::Int:
        case Type::Float:
            break;
        }
        // Clamped first: a value out of range would overflow the integer
        // cast (undefined behaviour), and -0.0 would show as "-0".
        if (std::isnan(value))
            return "?";
        value = std::clamp(value, o.min, o.max) + 0.0;
        if (o.type == Type::Int)
        {
            int64_t n = std::llround(value);
            if (unit == "%")
                return std::format("{}%", n);
            return unit.empty() ? std::format("{}", n) : std::format("{} {}", n, unit);
        }
        if (unit == "%")
            return std::format("{:.{}f}%", value * 100, Decimals(o.step * 100));
        return unit.empty() ? std::format("{:.{}f}", value, Decimals(o.step))
                            : std::format("{:.{}f} {}", value, Decimals(o.step), unit);
    }

    uint32_t Subscribe(Listener listener)
    {
        std::lock_guard lock(s_listenerMutex);
        uint32_t handle = s_nextHandle++;
        s_listeners.emplace_back(handle, std::make_shared<const Listener>(std::move(listener)));
        return handle;
    }

    void Unsubscribe(uint32_t handle)
    {
        std::lock_guard lock(s_listenerMutex);
        std::erase_if(s_listeners, [handle](const auto& l) { return l.first == handle; });
    }

    std::filesystem::path DefaultPath()
    {
        return GetUserPath() / "settings.toml";
    }

    bool Load()
    {
        return Load(DefaultPath());
    }

    bool Load(const std::filesystem::path& path)
    {
        // Read and parse outside the lock: readers never wait, and writers
        // wait only for the values to be published.
        std::array<std::optional<double>, kCount> fileValues;
        std::string text, readError, lastError, lastVersion;
        size_t fromFile = 0;
        ReadStatus status = ReadFile(path, text, readError);
        if (status == ReadStatus::Missing)
            fprintf(stderr, "[settings] no %s yet: defaults\n", path.c_str());
        else if (status == ReadStatus::Failed)
        {
            fprintf(stderr, "[settings] can't read %s: %s; defaults\n", path.c_str(), readError.c_str());
            lastError = std::format("Couldn't read {}: {}. The settings are at their defaults.", path.string(), readError);
        }
        else
        {
            toml::table root;
            std::string error;
            toml::source_position where{};
#if TOML_EXCEPTIONS
            try
            {
                root = toml::parse(text, path.string());
            }
            catch (const toml::parse_error& e)
            {
                error = e.description();
                where = e.source().begin;
            }
#else
            toml::parse_result result = toml::parse(text, path.string());
            if (result)
                root = std::move(result).table();
            else
            {
                error = result.error().description();
                where = result.error().source().begin;
            }
#endif
            if (!error.empty())
            {
                // Keep the player's file: the next Save would overwrite it.
                std::filesystem::path bad = path;
                bad += ".bad";
                std::error_code ec;
                std::filesystem::copy_file(path, bad, std::filesystem::copy_options::overwrite_existing, ec);
                fprintf(stderr, "[settings] %s doesn't parse: %s (line %u, column %u); defaults (kept as %s)\n", path.c_str(),
                    error.c_str(), unsigned(where.line), unsigned(where.column), ec ? "nothing: the copy failed" : bad.c_str());
                lastError = std::format("{} has a mistake at line {}, column {} ({}). The settings are at their defaults; {}.",
                    path.string(), where.line, where.column, error,
                    ec ? "saving them will replace the file" : std::format("the file was kept as {}", bad.string()));
            }
            for (auto&& [tableKey, tableNode] : root)
            {
                const toml::table* table = tableNode.as_table();
                if (!table)
                {
                    fprintf(stderr, "[settings] %s: %s outside a table; ignored\n", path.filename().c_str(),
                        std::string(tableKey.str()).c_str());
                    continue;
                }
                for (auto&& [key, node] : *table)
                {
                    std::string name = std::format("{}.{}", tableKey.str(), key.str());
                    if (tableKey.str() == kAppTable && key.str() == kLastVersionKey)
                    {
                        if (auto v = node.as_string())
                            lastVersion = v->get();
                        else
                            fprintf(stderr, "[settings] %s: %s = %s isn't a version; ignored\n", path.filename().c_str(), name.c_str(),
                                NodeText(node).c_str());
                        continue;
                    }
                    const Option* o = Find(tableKey.str(), key.str());
                    if (!o)
                    {
                        fprintf(stderr, "[settings] %s: unknown key %s; ignored\n", path.filename().c_str(), name.c_str());
                        continue;
                    }
                    std::string source = std::format("{}: {} = {}", path.filename().string(), name, NodeText(node));
                    if (auto v = Accept(*o, RawFromToml(*o, node), source, true))
                    {
                        fileValues[size_t(o->id)] = v;
                        fromFile++;
                    }
                }
            }
            if (error.empty())
                fprintf(stderr, "[settings] %s: %zu value%s from the file\n", path.c_str(), fromFile, fromFile == 1 ? "" : "s");
        }

        std::vector<Id> changed;
        {
            std::lock_guard lock(s_mutex);
            for (size_t i = 0; i < kCount; i++)
            {
                double stored = fileValues[i].value_or(Default(i));
                s_stored[i].store(stored, std::memory_order_relaxed);
                s_baseline[i] = stored;
                ReadEnvLocked(i, true);
                if (RecomputeLocked(i))
                    changed.push_back(Id(i));
            }
            s_frozen = true;
            LoadedPath() = path;
            LastErrorText() = lastError;
            LastVersionText() = lastVersion;
            s_fileFound = status != ReadStatus::Missing;
            s_fileLoaded = status == ReadStatus::Ok && lastError.empty();
            detail::g_generation.fetch_add(1, std::memory_order_release);
            detail::g_state.store(2, std::memory_order_release);
        }
        Notify(changed);
        return lastError.empty();
    }

    bool Save()
    {
        std::filesystem::path path;
        {
            std::lock_guard lock(s_mutex);
            path = LoadedPath();
        }
        return Save(path.empty() ? DefaultPath() : path);
    }

    bool Save(const std::filesystem::path& path)
    {
        std::lock_guard saveLock(s_saveMutex);
        std::array<double, kCount> values;
        {
            std::lock_guard lock(s_mutex);
            EnsureInitLocked("settings were saved");
            for (size_t i = 0; i < kCount; i++)
                values[i] = s_stored[i].load(std::memory_order_relaxed);
        }
        std::string error = WriteAtomically(path, Serialize(values));
        {
            std::lock_guard lock(s_mutex);
            if (!error.empty())
                LastErrorText() = std::format("Couldn't save the settings: {}.", error);
            else
            {
                LastErrorText().clear();
                std::copy(values.begin(), values.end(), s_baseline);
                if (LoadedPath().empty())
                    LoadedPath() = path;
            }
        }
        if (!error.empty())
        {
            fprintf(stderr, "[settings] %s\n", error.c_str());
            return false;
        }
        fprintf(stderr, "[settings] saved %s\n", path.c_str());
        return true;
    }

    std::string LastError()
    {
        std::lock_guard lock(s_mutex);
        return LastErrorText();
    }

    std::string LastVersion()
    {
        std::lock_guard lock(s_mutex);
        return LastVersionText();
    }

    bool FileFound()
    {
        std::lock_guard lock(s_mutex);
        return s_fileFound;
    }

    bool RecordVersion(std::string_view version)
    {
        std::lock_guard saveLock(s_saveMutex);
        std::filesystem::path path;
        {
            std::lock_guard lock(s_mutex);
            if (LastVersionText() == version)
                return true;
            // A file that didn't load is the player's to fix (it was kept
            // as .bad): rewriting it now would lose it before they can.
            if (s_fileFound && !s_fileLoaded)
            {
                fprintf(stderr, "[settings] the version isn't recorded: settings.toml didn't load\n");
                return false;
            }
            path = LoadedPath().empty() ? DefaultPath() : LoadedPath();
        }
        // Only the [app] table changes: the options stay as the file has
        // them, so one the player never set keeps following its default (a
        // full Save writes every option's value).
        std::string text, error;
        ReadStatus status = ReadFile(path, text, error);
        if (status == ReadStatus::Failed)
        {
            fprintf(stderr, "[settings] the version isn't recorded: can't read %s: %s\n", path.c_str(), error.c_str());
            return false;
        }
        if (status == ReadStatus::Missing)
            text = "# SpeedBreaker settings. The settings menu writes every option here when one\n"
                   "# changes; until then each one keeps its default.\n";
        std::string recorded(version);
        if (std::string failed = WriteAtomically(path, WithoutAppTable(text) + AppTable(recorded)); !failed.empty())
        {
            fprintf(stderr, "[settings] the version isn't recorded: %s\n", failed.c_str());
            return false;
        }
        std::lock_guard lock(s_mutex);
        LastVersionText() = recorded;
        s_fileFound = s_fileLoaded = true;
        if (LoadedPath().empty())
            LoadedPath() = path;
        return true;
    }
}
