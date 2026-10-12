// Tests for user/key_names.h: the keys a Controls row can hold and their
// names in settings.toml. Links SDL3 to check every code against SDL's name
// for that scancode (no SDL_Init: the names are SDL's built-in ones, before
// a platform renames any). From the repo root, with the deps in build/deps:
//   clang++ -std=c++20 -O2 -Iruntime -Ibuild/deps/include tests/key_names_test.cpp -Lbuild/deps/lib -lSDL3 \
//     -Wl,-rpath,build/deps/lib -o build/key_names_test && build/key_names_test
#include <user/key_names.h>

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_scancode.h>

#include <cctype>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <string_view>

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

using namespace settings::keynames;

int main()
{
    CHECK(kKeyCount == 123, "%zu keys", kKeyCount);

    // Tokens: lower-case letters, digits and underscores, each once. Codes:
    // scancodes, ascending, each once. And both ways round.
    std::set<std::string_view> tokens;
    uint16_t last = 0;
    for (const Key& k : kKeys)
    {
        bool wellFormed = k.token && k.token[0];
        for (const char* c = k.token; wellFormed && *c; c++)
            wellFormed = (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_';
        CHECK(wellFormed, "%s: a token is [a-z0-9_]+", k.token ? k.token : "(null)");
        CHECK(tokens.insert(k.token).second, "%s: once", k.token);
        CHECK(k.code > last, "%s (%d): codes ascend", k.token, k.code);
        last = k.code;
        CHECK(k.code != kNone && k.code != kUnknown && k.code < SDL_SCANCODE_COUNT, "%s: a scancode", k.token);
        CHECK(Token(k.code) == k.token, "%s: Token(code)", k.token);
        CHECK(Code(k.token) == k.code, "%s: Code(token)", k.token);
        CHECK(Known(k.code), "%s: Known", k.token);
    }
    CHECK(!tokens.count("none"), "\"none\" is not a row of the table");

    // none, any case, and what isn't a name.
    CHECK(std::string_view(Token(kNone)) == "none" && Code("none") == kNone && Code("NONE") == kNone && Known(kNone), "none is 0");
    CHECK(Code("Left_Shift") == SDL_SCANCODE_LSHIFT && Code("W") == SDL_SCANCODE_W && Code("w") == SDL_SCANCODE_W, "any case");
    CHECK(Code("") == kUnknown && Code("Left Shift") == kUnknown && Code("left_shif") == kUnknown &&
        Code("left_shiftx") == kUnknown && Code("none ") == kUnknown, "not a token");
    CHECK(Token(1) == nullptr && Token(SDL_SCANCODE_LGUI) == nullptr && Token(kUnknown) == nullptr && Token(512) == nullptr, "not a key");
    CHECK(!Known(1) && !Known(SDL_SCANCODE_F1) && !Known(SDL_SCANCODE_LGUI) && !Known(SDL_SCANCODE_RGUI) && !Known(kUnknown),
        "F1, Cmd / Windows and nameless codes are out");
    CHECK(Code("f1") == kUnknown && Code("left_gui") == kUnknown && Code("right_gui") == kUnknown, "...by name too");
    CHECK(Code("escape") == SDL_SCANCODE_ESCAPE && Known(SDL_SCANCODE_ESCAPE), "Esc is a key (Back's default)");

    // SDL agrees with every row: its name for the scancode, lowered with
    // spaces as underscores, is the token, bar the keys SDL names by their
    // symbol or runs together.
    const std::map<uint16_t, std::string_view> exceptions = {
        { SDL_SCANCODE_MINUS, "minus" }, { SDL_SCANCODE_EQUALS, "equals" }, { SDL_SCANCODE_LEFTBRACKET, "left_bracket" },
        { SDL_SCANCODE_RIGHTBRACKET, "right_bracket" }, { SDL_SCANCODE_BACKSLASH, "backslash" }, { SDL_SCANCODE_NONUSHASH, "non_us_hash" },
        { SDL_SCANCODE_SEMICOLON, "semicolon" }, { SDL_SCANCODE_APOSTROPHE, "apostrophe" }, { SDL_SCANCODE_GRAVE, "grave" },
        { SDL_SCANCODE_COMMA, "comma" }, { SDL_SCANCODE_PERIOD, "period" }, { SDL_SCANCODE_SLASH, "slash" },
        { SDL_SCANCODE_CAPSLOCK, "caps_lock" }, { SDL_SCANCODE_PRINTSCREEN, "print_screen" }, { SDL_SCANCODE_SCROLLLOCK, "scroll_lock" },
        { SDL_SCANCODE_PAGEUP, "page_up" }, { SDL_SCANCODE_PAGEDOWN, "page_down" }, { SDL_SCANCODE_NUMLOCKCLEAR, "num_lock" },
        { SDL_SCANCODE_KP_DIVIDE, "keypad_divide" }, { SDL_SCANCODE_KP_MULTIPLY, "keypad_multiply" }, { SDL_SCANCODE_KP_MINUS, "keypad_minus" },
        { SDL_SCANCODE_KP_PLUS, "keypad_plus" }, { SDL_SCANCODE_KP_PERIOD, "keypad_period" }, { SDL_SCANCODE_KP_EQUALS, "keypad_equals" },
        { SDL_SCANCODE_KP_COMMA, "keypad_comma" }, { SDL_SCANCODE_NONUSBACKSLASH, "non_us_backslash" },
    };
    for (const Key& k : kKeys)
    {
        std::string expected;
        if (auto e = exceptions.find(k.code); e != exceptions.end())
            expected = e->second;
        else
            for (const char* c = SDL_GetScancodeName(SDL_Scancode(k.code)); *c; c++)
                expected += *c == ' ' ? '_' : char(std::tolower(uint8_t(*c)));
        CHECK(expected == k.token, "%d: SDL calls it \"%s\", the table \"%s\"", k.code, SDL_GetScancodeName(SDL_Scancode(k.code)), k.token);
    }

    // Usable at compile time (the option table's defaults are checked when it's built).
    static_assert(Code("w") == 26 && Token(26)[0] == 'w' && Known(0) && !Known(227) && Code("None") == kNone, "constexpr");

    printf("%s (%d failures)\n", g_failures ? "FAILED" : "passed", g_failures);
    return g_failures ? 1 : 0;
}
