// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Player settings: settings.toml in GetUserPath(), edited by the in-game
// menu and read by the systems that follow them.
//
//   - One table (Options()) describes every option: its TOML key, type,
//     default and range, menu category, label and description, the
//     environment variable that overrides it, and whether a change applies
//     live or after a restart. The menu iterates it.
//   - Reads are lock-free from any thread (an acquire and a relaxed load,
//     inlined), so per-frame and per-draw code reads settings where it uses
//     them instead of caching them in statics. Writes (the menu, Load) are
//     serialised. Nothing may read a setting from a static initializer:
//     that runs before main() calls Load(), so it would miss the file (it
//     gets the environment and defaults, with a warning in the log).
//   - An environment variable (NFSMW_AA=0, ...) wins over the file for that
//     run and is never written back; EnvOverride() tells the menu.
//   - A Restart option keeps its launch value for the whole run. Its
//     consumers read it once (the window's size), so a change mid-run would
//     reach only part of them. The menu edits the stored value and shows
//     RestartPending(). Every option the menu shows is Live: Internal
//     Resolution and Mipmaps rebuild what depends on them between two frames
//     (gpu/renderer's ApplyLiveSettings).
//   - Save() writes a temporary file, syncs it and renames it over
//     settings.toml: a crash or a full disk leaves the previous file whole.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace settings
{
    // Every option, grouped by menu category in menu order.
    enum class Id : uint8_t
    {
        // Display, Graphics (the [video] table)
        Fullscreen,
        VSync,
        FrameRate,
        PresentScale,  // shown on iOS only
        AntiAliasing,
        Scaling,
        AspectRatio,
        Sharpening,
        Anisotropy,
        Mipmaps,
        InternalResolution,
        WindowWidth,   // hidden: the window's size, remembered between runs
        WindowHeight,  // hidden
        // Audio
        MasterVolume,
        Mute,
        MuteInBackground,
        // Input
        StickDeadzone,
        TriggerDeadzone,
        Vibration,
        // Advanced
        AsyncShaders,
        PerformanceOverlay,
        Count
    };
    constexpr size_t kCount = size_t(Id::Count);

    enum class Type : uint8_t { Bool, Int, Float, Enum };
    enum class Category : uint8_t { Display, Graphics, Audio, Input, Advanced, Count };
    enum class Apply : uint8_t { Live, Restart };

    // Values of the enum options (GetInt returns them).
    enum class ScalingMode : int32_t { Bicubic = 0, Linear = 1 };
    // Frame Rate: 60, 30, or Auto (video/frame_rate.h: 60, or 30 while the
    // device is hot or can't hold 60).
    constexpr int32_t kFrameRateAuto = 0;
    // Internal resolution: 1-3 renders at that multiple of 720p; Auto picks
    // it from the screen area the picture covers (see renderer).
    constexpr int32_t kResolutionAuto = 0;
    // Auto: a screen of another shape is filled (video/picture_fit), the HUD
    // kept in a centred 16:9 block: a wider one with a wider view (Hor+), a
    // narrower one (16:10: the Steam Deck, 1280x800 Macs; 3:2, 4:3) with a
    // taller view (Vert+, down to 4:3). Only a screen within 1% of 16:9 keeps
    // the 16:9 frame (video::kTallMinStretch). Widescreen: 16:9 with bars.
    enum class AspectMode : int32_t { Auto = 0, Widescreen = 1 };
    // Anisotropy: 1 (off), 2, 4, 8, 16, or one of these.
    constexpr int32_t kAnisotropyAuto = 0;   // 16x on a discrete GPU or an M-series iPad, 4x on other integrated ones
    constexpr int32_t kAnisotropyGame = -1;  // each texture's own setting, as on the 360

    struct EnumEntry
    {
        int32_t value;
        const char* token;  // in settings.toml and the environment variable
        const char* label;  // in the menu
    };

    struct Option
    {
        Id id;
        Category category;
        Type type;
        Apply apply;
        bool hidden = false;      // saved, but not shown in the menu
        const char* table;        // settings.toml table, e.g. "video"
        const char* key;          // key in that table, e.g. "anti_aliasing"
        const char* label;        // menu label
        const char* description;  // one-line menu help
        const char* env;          // the environment variable that overrides it
        int envField = 0;         // 0: the whole variable; n: its n-th 'x'-separated field (NFSMW_WINDOW=WxH)
        double defaultValue;      // Bool: 0 or 1; Enum: an entry's value
        // Int and Float: the range and the menu's step. Enum: the range a
        // number in the file or the environment is clamped to before it
        // takes the entry at or below it (NFSMW_ANISOTROPY=12 is 8x); 0, 0:
        // tokens only.
        double min = 0, max = 0, step = 0;
        const char* unit = nullptr;              // FormatValue suffix; "%" on a Float shows value x 100
        std::span<const EnumEntry> entries = {};  // Enum only
    };

    enum class SetResult : uint8_t
    {
        Applied,     // in effect now
        Deferred,    // stored; takes effect at the next launch (a Restart option)
        Overridden,  // stored; this run keeps the environment variable's value
        Unchanged,   // it already had that value
        Invalid,     // not a value of this option (NaN, not an enum value)
    };

    // The option table, in Id order.
    std::span<const Option> Options();
    const Option& Info(Id id);
    const Option* Find(std::string_view table, std::string_view key);
    const char* CategoryName(Category category);

    namespace detail
    {
        // 0: untouched, 1: environment and defaults applied (read before
        // Load), 2: loaded.
        extern std::atomic<uint8_t> g_state;
        extern std::atomic<double> g_effective[kCount];
        extern std::atomic<uint64_t> g_generation;
        void InitBeforeLoad(Id first);
    }

    // The value in effect for this run: the environment's if it overrides
    // the option, else the stored one (a Restart option: as at launch).
    // Any thread; lock-free.
    inline double GetValue(Id id)
    {
        if (detail::g_state.load(std::memory_order_acquire) == 0) [[unlikely]]
            detail::InitBeforeLoad(id);
        return detail::g_effective[size_t(id)].load(std::memory_order_relaxed);
    }
    inline bool GetBool(Id id) { return GetValue(id) != 0.0; }
    inline int32_t GetInt(Id id) { return int32_t(GetValue(id)); }  // Int, Enum (the entry's value), Bool (0/1)
    inline float GetFloat(Id id) { return float(GetValue(id)); }

    // Bumped by every change of any option's stored or effective value
    // (Set, Reset, Load). A consumer that caches settings compares it once
    // a frame and re-reads when it moved.
    inline uint64_t Generation() { return detail::g_generation.load(std::memory_order_acquire); }

    // What the menu edits and Save() writes. Differs from GetValue() while
    // the environment overrides the option or a Restart option waits for a
    // restart.
    double GetStoredValue(Id id);
    bool IsDefault(Id id);  // stored value == default

    // Main thread, before Load(): `value` is the option's default on this
    // device instead of the table's (Frame Rate's Auto on an iPhone): what a
    // settings.toml without the option gives, and what Reset and IsDefault
    // use. Logged, with `why`. A settings.toml that a save wrote holds every
    // option, so it keeps the value it has.
    void SetDeviceDefault(Id id, double value, std::string_view why);
    // The option's default on this device: the table's, or SetDeviceDefault's.
    double DefaultValue(Id id);
    // The environment variable's name while it overrides this option for
    // this run; nullptr otherwise.
    const char* EnvOverride(Id id);
    // A Restart option whose stored value differs from the running one.
    bool RestartPending(Id id);
    // RestartPending for any option the menu shows. Hidden ones don't count:
    // the presenter stores the window's size on every resize, which isn't a
    // reason to ask the player to restart.
    bool AnyRestartPending();
    // Some stored value differs from what the last Load read or Save wrote
    // (changing an option and back again isn't dirty).
    bool Dirty();

    // Store a value (normalised: Bool to 0/1, Int rounded and clamped, Float
    // clamped and rounded to 1e-4, Enum an entry's value) and apply it if it
    // can be. Any thread. Listeners run on the calling thread afterwards.
    SetResult Set(Id id, double value);
    SetResult Reset(Id id);  // to the default
    void ResetAll();         // every option, hidden ones included

    // The value a left/right press (delta -1/+1) moves to from `value`:
    // Bool toggles, Enum takes the next or previous entry, Int and Float
    // move by step on the step grid, clamped. From a value between grid
    // points (a hand edit: 58%) a press goes to the next point that way (60%
    // or 55%). Any input gives a value of the option (NaN: the default). It
    // doesn't Set anything.
    double StepValue(Id id, double value, int delta);
    // For the menu: "On", "Bicubic", "85%", "8%", "0.30", "1280 px". Numbers
    // are shown clamped to the option's range; NaN or a non-entry is "?".
    std::string FormatValue(Id id, double value);

    // Called with an option's id after its effective value changes, on the
    // thread that changed it (the menu's, or Load's), outside the settings
    // lock (a listener may read or Set). Keep it short; hand thread-bound
    // work (SDL window calls) to the owning thread, or have that thread poll
    // Generation() instead. Unsubscribe doesn't wait for a call already
    // under way on another thread.
    using Listener = std::function<void(Id)>;
    uint32_t Subscribe(Listener listener);
    void Unsubscribe(uint32_t handle);

    // GetUserPath() / "settings.toml".
    std::filesystem::path DefaultPath();
    // Reads the file (missing: defaults), then the environment. Unknown keys
    // and unusable values are skipped with a log line; out-of-range values
    // are clamped. A file that doesn't parse is copied to settings.toml.bad
    // and every option starts at its default. Something that isn't a regular
    // file (a FIFO would block the launch) or is over 1 MiB (the real one is
    // ~3 KB) is refused the same way, without the copy. Restart options take
    // their running values from the first Load; a later Load (a reload)
    // replaces unsaved changes. Returns false only when the file exists but
    // couldn't be read or parsed (LastError() says why).
    bool Load();
    bool Load(const std::filesystem::path& path);
    // Writes every option's stored value (never an environment override) to
    // the path last loaded, or DefaultPath(). Creates the directory. Through
    // a symlinked settings.toml it replaces the file the link points at.
    // Atomic, also against another instance of the game saving at the same
    // moment (each process writes its own temporary file). Takes a few
    // milliseconds (a full flush to disk): on menu close or exit, not per
    // frame.
    bool Save();
    bool Save(const std::filesystem::path& path);

    // Why the last Load() or Save() failed, for the menu to show the player:
    // "" after one that succeeded (a missing file is a success). Values
    // skipped one by one aren't failures; they're only in the log.
    std::string LastError();

    // The game's own record in settings.toml, beside the options: [app]
    // last_version, the SpeedBreaker version that ran last
    // (user/version_check.h). "" when the file has none (a new player, or a
    // build from before versions were recorded).
    std::string LastVersion();
    // The last Load() found a settings.toml (whether or not it loaded).
    bool FileFound();
    // Main thread, after Load(): records `version` as the one that ran last,
    // in memory and in the file. Only the [app] table is rewritten (atomic,
    // like Save): the options stay as the file has them, so a player's file
    // that holds none keeps following the defaults. A file that didn't load
    // is left alone (false, logged), as is one already recording `version`.
    // Every Save() writes it too.
    bool RecordVersion(std::string_view version);
}
