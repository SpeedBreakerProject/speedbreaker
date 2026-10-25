// Tests for the Key options (Settings > Controls, user/settings.h): the
// table, the [keys] names in settings.toml, and what the menu's Set accepts.
// Links user/settings.cpp (and toml++ from XenonRecomp's vendored copy); no
// SDL. From the repo root:
//   clang++ -std=c++20 -O2 -Iruntime -Itools/XenonRecomp/thirdparty/tomlplusplus/include \
//     tests/settings_keys_test.cpp runtime/user/settings.cpp -o build/settings_keys_test && build/settings_keys_test
#include <user/key_names.h>
#include <user/settings.h>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
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

static std::string ReadAll(const std::filesystem::path& file)
{
    std::ifstream f(file);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

int main()
{
    using namespace settings;
    using keynames::Code;

    // The table: 48 Key options, every one in [keys], on the Controls page,
    // live, shown, with no environment variable and a default the key table
    // knows (none included).
    int keys = 0;
    std::set<std::string> names;
    for (const Option& o : Options())
    {
        if (o.type != Type::Key)
        {
            CHECK(std::string_view(o.table) != "keys", "%s.%s: only Key options live in [keys]", o.table, o.key);
            CHECK(o.env != nullptr, "%s.%s: every other option has an environment variable", o.table, o.key);
            continue;
        }
        keys++;
        CHECK(std::string_view(o.table) == "keys", "%s.%s: a Key option lives in [keys]", o.table, o.key);
        CHECK(o.category == Category::Controls, "%s: on the Controls page", o.key);
        CHECK(o.apply == Apply::Live && !o.hidden, "%s: live and shown", o.key);
        CHECK(o.env == nullptr, "%s: no environment variable", o.key);
        CHECK(names.insert(o.key).second, "%s: one row per key", o.key);
        CHECK(o.defaultValue >= 0 && o.defaultValue == std::floor(o.defaultValue) && keynames::Known(uint16_t(o.defaultValue)),
            "%s: the default is a key, or none", o.key);
        CHECK(o.min == 0 && o.max == 0 && o.step == 0 && o.entries.empty(), "%s: no range", o.key);
    }
    CHECK(keys == 48, "%d Key options", keys);
    CHECK(std::string_view(CategoryName(Category::Controls)) == "Controls", "the category's name");

    // The defaults: v0.1.4's keys (the arrows steer as well as A and D, and
    // are the D-pad; O/L/K/; are the right stick), plus the left stick
    // click, which had none. In page order, in pairs: the second key is the first's id +
    // 1, named <key>_2, with the row's label.
    struct Default { Id id; const char* key; const char* label; const char* first; const char* second; };
    const Default defaults[] = {
        { Id::KeyRightTrigger, "right_trigger", "Right Trigger", "w", "none" },
        { Id::KeyLeftTrigger, "left_trigger", "Left Trigger", "s", "none" },
        { Id::KeyLeftStickLeft, "left_stick_left", "Left Stick Left", "a", "left" },
        { Id::KeyLeftStickRight, "left_stick_right", "Left Stick Right", "d", "right" },
        { Id::KeyLeftStickUp, "left_stick_up", "Left Stick Up", "up", "none" },
        { Id::KeyLeftStickDown, "left_stick_down", "Left Stick Down", "down", "none" },
        { Id::KeyA, "a", "A", "space", "none" },
        { Id::KeyB, "b", "B", "backspace", "none" },
        { Id::KeyX, "x", "X", "left_shift", "none" },
        { Id::KeyY, "y", "Y", "left_ctrl", "none" },
        { Id::KeyLB, "lb", "LB", "q", "none" },
        { Id::KeyRB, "rb", "RB", "e", "none" },
        { Id::KeyDpadUp, "dpad_up", "D-pad Up", "up", "none" },
        { Id::KeyDpadDown, "dpad_down", "D-pad Down", "down", "none" },
        { Id::KeyDpadLeft, "dpad_left", "D-pad Left", "left", "none" },
        { Id::KeyDpadRight, "dpad_right", "D-pad Right", "right", "none" },
        { Id::KeyStart, "start", "Start", "return", "none" },
        { Id::KeyBack, "back", "Back", "escape", "none" },
        { Id::KeyRightStickUp, "right_stick_up", "Right Stick Up", "o", "none" },
        { Id::KeyRightStickDown, "right_stick_down", "Right Stick Down", "l", "none" },
        { Id::KeyRightStickLeft, "right_stick_left", "Right Stick Left", "k", "none" },
        { Id::KeyRightStickRight, "right_stick_right", "Right Stick Right", "semicolon", "none" },
        { Id::KeyLeftStickClick, "left_stick_click", "Left Stick Click", "z", "none" },
        { Id::KeyRightStickClick, "right_stick_click", "Right Stick Click", "c", "none" },
    };
    // The keys run from the end of the Wheel options (Input's own rows, then
    // the wheel's, come before them) to the start of Advanced, unbroken.
    size_t previous = size_t(Id::WheelSettingsVersion);
    for (const Default& d : defaults)
    {
        const Option& o = Info(d.id);
        const Option& o2 = Info(Id(size_t(d.id) + 1));
        CHECK(std::string_view(o.key) == d.key, "%s is %s", o.key, d.key);
        CHECK(std::string_view(o2.key) == std::string(d.key) + "_2", "%s: the second key is %s_2", o2.key, d.key);
        CHECK(o2.type == Type::Key, "%s: the second key is a Key option", o2.key);
        CHECK(std::string_view(o.label) == d.label && std::string_view(o2.label) == d.label, "%s: both keys carry the row's label", d.key);
        CHECK(o.defaultValue == double(Code(d.first)), "%s defaults to %s, not %s", o.key, d.first, FormatValue(o.id, o.defaultValue).c_str());
        CHECK(o2.defaultValue == double(Code(d.second)), "%s defaults to %s, not %s", o2.key, d.second, FormatValue(o2.id, o2.defaultValue).c_str());
        CHECK(size_t(d.id) == previous + 1, "%s follows the row before it", o.key);
        previous = size_t(d.id) + 1;
    }
    CHECK(size_t(Id::AsyncShaders) == previous + 1, "Advanced follows the keys");
    CHECK(EnvOverride(Id::KeyA) == nullptr, "nothing overrides a key");

    // A scratch folder of our own.
#ifdef _WIN32
    // Windows has no mkdtemp: a folder named after this process, made new.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / ("speedbreaker-keys-" + std::to_string(_getpid()));
    std::error_code scratchError;
    std::filesystem::remove_all(dir, scratchError);
    if (!std::filesystem::create_directory(dir, scratchError))
    {
        printf("can't make a scratch folder: %s\n", scratchError.message().c_str());
        return 2;
    }
#else
    std::string pattern = (std::filesystem::temp_directory_path() / "speedbreaker-keys-XXXXXX").string();
    if (!mkdtemp(pattern.data()))
    {
        printf("can't make a scratch folder: %s\n", strerror(errno));
        return 2;
    }
    const std::filesystem::path dir = pattern;
#endif
    const std::filesystem::path file = dir / "settings.toml";

    // The file: names in, in any case; "none" empties a key; anything else
    // keeps the default, SDL's spelling from the first build included.
    {
        std::ofstream f(file);
        f << "[keys]\n"
             "right_trigger = \"up\"\n"
             "left_trigger = \"Keypad_1\"\n"
             "left_stick_left_2 = \"none\"\n"
             "right_stick_left = \"K\"\n"
             "right_stick_up = \"LEFT_SHIFT\"\n"
             "a = \"Left Shift\"\n"
             "b = 42\n"
             "x = true\n"
             "y = \"\"\n"
             "lb = \"f1\"\n"
             "rb = \"left_gui\"\n";
    }
    CHECK(Load(file), "loads: %s", LastError().c_str());
    CHECK(GetInt(Id::KeyRightTrigger) == Code("up"), "\"up\": %d", GetInt(Id::KeyRightTrigger));
    CHECK(GetInt(Id::KeyLeftTrigger) == Code("keypad_1"), "a token in another case: %d", GetInt(Id::KeyLeftTrigger));
    CHECK(GetInt(Id::KeyLeftStickLeft2) == keynames::kNone, "\"none\" is no key: %d", GetInt(Id::KeyLeftStickLeft2));
    CHECK(GetInt(Id::KeyRightStickLeft) == Code("k"), "\"K\" is k: %d", GetInt(Id::KeyRightStickLeft));
    CHECK(GetInt(Id::KeyRightStickUp) == Code("left_shift"), "\"LEFT_SHIFT\" is left_shift: %d", GetInt(Id::KeyRightStickUp));
    CHECK(GetInt(Id::KeyA) == Code("space"), "SDL's spelling keeps the default");
    CHECK(GetInt(Id::KeyB) == Code("backspace"), "a number keeps the default");
    CHECK(GetInt(Id::KeyX) == Code("left_shift"), "a boolean keeps the default");
    CHECK(GetInt(Id::KeyY) == Code("left_ctrl"), "an empty name keeps the default");
    CHECK(GetInt(Id::KeyLB) == Code("q") && GetInt(Id::KeyRB) == Code("e"), "F1 and Cmd / Windows can't be bound from the file");
    CHECK(GetInt(Id::KeyStart) == Code("return") && GetInt(Id::KeyLeftStickRight2) == Code("right"), "a missing key keeps the default");
    CHECK(IsDefault(Id::KeyLB) && !IsDefault(Id::KeyRightTrigger) && !IsDefault(Id::KeyLeftStickLeft2), "IsDefault follows the file");

    // Shown in the log and the bug report by name.
    CHECK(FormatValue(Id::KeyRightTrigger, GetValue(Id::KeyRightTrigger)) == "up", "FormatValue: %s",
        FormatValue(Id::KeyRightTrigger, GetValue(Id::KeyRightTrigger)).c_str());
    CHECK(FormatValue(Id::KeyX, Code("left_shift")) == "left_shift" && FormatValue(Id::KeyX, 0) == "none", "a token, or none");
    CHECK(FormatValue(Id::KeyA, 1) == "?" && FormatValue(Id::KeyA, 227) == "?" && FormatValue(Id::KeyA, 1e9) == "?" &&
        FormatValue(Id::KeyA, NAN) == "?" && FormatValue(Id::KeyA, -1) == "?", "not a key: ?");

    // Set: a key of the table, or none; anything else is Invalid.
    CHECK(Set(Id::KeyA, Code("return")) == SetResult::Applied, "a key");
    CHECK(Set(Id::KeyA, Code("return")) == SetResult::Unchanged, "the same key again");
    CHECK(Set(Id::KeyA, keynames::kNone) == SetResult::Applied && GetInt(Id::KeyA) == 0, "none");
    CHECK(Set(Id::KeyA2, Code("return")) == SetResult::Applied && Set(Id::KeyA, Code("return")) == SetResult::Applied,
        "the same key in both cells of a row is allowed");
    CHECK(Set(Id::KeyA2, keynames::kNone) == SetResult::Applied, "and so is none in both");
    CHECK(Set(Id::KeyA, 1) == SetResult::Invalid, "1 is no key");
    CHECK(Set(Id::KeyA, 58) == SetResult::Invalid, "F1 is no key");
    CHECK(Set(Id::KeyA, 227) == SetResult::Invalid, "Left GUI is no key");
    CHECK(Set(Id::KeyA, keynames::kUnknown) == SetResult::Invalid && Set(Id::KeyA, 1e9) == SetResult::Invalid &&
        Set(Id::KeyA, -1) == SetResult::Invalid, "out of the table");
    CHECK(Set(Id::KeyA, double(Code("return")) + 0.5) == SetResult::Invalid, "not a whole number");
    CHECK(Set(Id::KeyA, NAN) == SetResult::Invalid, "NaN");
    CHECK(GetInt(Id::KeyA) == Code("return"), "invalid sets change nothing: %d", GetInt(Id::KeyA));
    CHECK(StepValue(Id::KeyA, Code("return"), 1) == double(Code("return")) && StepValue(Id::KeyA, Code("return"), -1) == double(Code("return")),
        "Left and Right do nothing to a key");
    CHECK(!IsDefault(Id::KeyA) && Reset(Id::KeyA) == SetResult::Applied && IsDefault(Id::KeyA) && GetInt(Id::KeyA) == Code("space"), "Reset");

    // Save: every key by name, the comment, no environment variable; and
    // what it wrote loads back.
    CHECK(Save(file), "saves: %s", LastError().c_str());
    std::string text = ReadAll(file);
    size_t at = text.find("\n[keys]\n"), advanced = text.find("\n[advanced]\n");
    CHECK(at != std::string::npos && advanced != std::string::npos && at < advanced, "[keys] before [advanced]");
    std::string section = at == std::string::npos ? "" : text.substr(at, advanced == std::string::npos ? std::string::npos : advanced - at);
    CHECK(section.find("right_trigger = \"up\"\n") != std::string::npos, "saved by name");
    CHECK(section.find("left_trigger = \"keypad_1\"\n") != std::string::npos, "the table's case, not the file's");
    CHECK(section.find("left_stick_left_2 = \"none\"\n") != std::string::npos, "an emptied key");
    CHECK(section.find("right_stick_left = \"k\"\n") != std::string::npos, "lower case");
    CHECK(section.find("right_stick_right = \"semicolon\"\n") != std::string::npos, "a symbol key by its name");
    CHECK(section.find("right_stick_up = \"left_shift\"\n") != std::string::npos, "a modifier");
    CHECK(section.find("left_stick_left = \"a\"\n") != std::string::npos && section.find("dpad_left_2 = \"none\"\n") != std::string::npos,
        "defaults are written too, both keys");
    CHECK(section.find("# A key name such as \"w\"") != std::string::npos, "the comment says what goes here");
    CHECK(section.find("NFSMW_") == std::string::npos, "no environment variable on a key");
    CHECK(text.find("(NFSMW_VIBRATION)") != std::string::npos, "the other options keep theirs");
    CHECK(Load(file), "loads what it saved: %s", LastError().c_str());
    CHECK(GetInt(Id::KeyRightTrigger) == Code("up") && GetInt(Id::KeyLeftTrigger) == Code("keypad_1") &&
        GetInt(Id::KeyRightStickUp) == Code("left_shift") && GetInt(Id::KeyLeftStickLeft2) == 0, "round trip");
    CHECK(!Dirty(), "nothing changed since the save");

    // The keys SDL names by a symbol are words here: nothing to escape.
    CHECK(Set(Id::KeyA, Code("backslash")) == SetResult::Applied && Set(Id::KeyB, Code("apostrophe")) == SetResult::Applied,
        "the backslash and apostrophe keys are keys");
    CHECK(Save(file), "saves them: %s", LastError().c_str());
    text = ReadAll(file);
    CHECK(text.find("\na = \"backslash\"\n") != std::string::npos && text.find("\nb = \"apostrophe\"\n") != std::string::npos, "by name");
    CHECK(Load(file), "and the file still parses: %s", LastError().c_str());
    CHECK(GetInt(Id::KeyA) == Code("backslash") && GetInt(Id::KeyB) == Code("apostrophe"), "round trip: %d %d", GetInt(Id::KeyA), GetInt(Id::KeyB));
    CHECK(GetInt(Id::KeyRightTrigger) == Code("up") && GetInt(Id::MasterVolume) == 100, "and the rest of the file survived");
    std::filesystem::remove_all(dir);

    printf("%s (%d failures)\n", g_failures ? "FAILED" : "passed", g_failures);
    return g_failures ? 1 : 0;
}
