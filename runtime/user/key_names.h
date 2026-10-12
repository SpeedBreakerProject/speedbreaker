// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The keys a Controls row can hold (Settings > Controls), named for
// settings.toml: "w", "left_shift", "keypad_1", or "none" for no key. The
// code is the key's USB HID usage, which is what SDL's scancodes are (A is
// 4), so a binding is the physical key whatever the layout; SDL itself
// stays out of this header and of settings.cpp, and tests/key_names_test.cpp
// checks every row against SDL's name for the code (hid/input.cpp pins a
// few at compile time). SDL's own names won't do for the file: some change
// once video starts (on a Mac Left Alt becomes "Left Option"), while the
// settings load before that, and some keys share one. The table is also
// the list of what can be bound: F1 (it opens the menu), Cmd / Windows and
// the media keys are left out on purpose. Header only.
#pragma once
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string_view>

namespace settings::keynames
{
    constexpr uint16_t kNone = 0;          // no key; "none" in the file
    constexpr uint16_t kUnknown = 0xFFFF;  // Code(): not a name in the table

    struct Key
    {
        uint16_t code;
        const char* token;
    };

    // Ascending by code. The tokens are SDL's scancode constants in lower
    // case with underscores: SDL_SCANCODE_LEFTBRACKET is "left_bracket",
    // SDL_SCANCODE_KP_1 "keypad_1", SDL_SCANCODE_NUMLOCKCLEAR "num_lock".
    constexpr Key kKeys[] = {
        { 4, "a" }, { 5, "b" }, { 6, "c" }, { 7, "d" }, { 8, "e" }, { 9, "f" }, { 10, "g" }, { 11, "h" }, { 12, "i" },
        { 13, "j" }, { 14, "k" }, { 15, "l" }, { 16, "m" }, { 17, "n" }, { 18, "o" }, { 19, "p" }, { 20, "q" }, { 21, "r" },
        { 22, "s" }, { 23, "t" }, { 24, "u" }, { 25, "v" }, { 26, "w" }, { 27, "x" }, { 28, "y" }, { 29, "z" },
        { 30, "1" }, { 31, "2" }, { 32, "3" }, { 33, "4" }, { 34, "5" }, { 35, "6" }, { 36, "7" }, { 37, "8" }, { 38, "9" }, { 39, "0" },
        { 40, "return" }, { 41, "escape" }, { 42, "backspace" }, { 43, "tab" }, { 44, "space" },
        { 45, "minus" }, { 46, "equals" }, { 47, "left_bracket" }, { 48, "right_bracket" }, { 49, "backslash" },
        { 50, "non_us_hash" }, { 51, "semicolon" }, { 52, "apostrophe" }, { 53, "grave" }, { 54, "comma" },
        { 55, "period" }, { 56, "slash" }, { 57, "caps_lock" },
        { 59, "f2" }, { 60, "f3" }, { 61, "f4" }, { 62, "f5" }, { 63, "f6" }, { 64, "f7" }, { 65, "f8" }, { 66, "f9" },
        { 67, "f10" }, { 68, "f11" }, { 69, "f12" },
        { 70, "print_screen" }, { 71, "scroll_lock" }, { 72, "pause" }, { 73, "insert" }, { 74, "home" },
        { 75, "page_up" }, { 76, "delete" }, { 77, "end" }, { 78, "page_down" },
        { 79, "right" }, { 80, "left" }, { 81, "down" }, { 82, "up" },
        { 83, "num_lock" }, { 84, "keypad_divide" }, { 85, "keypad_multiply" }, { 86, "keypad_minus" },
        { 87, "keypad_plus" }, { 88, "keypad_enter" },
        { 89, "keypad_1" }, { 90, "keypad_2" }, { 91, "keypad_3" }, { 92, "keypad_4" }, { 93, "keypad_5" },
        { 94, "keypad_6" }, { 95, "keypad_7" }, { 96, "keypad_8" }, { 97, "keypad_9" }, { 98, "keypad_0" },
        { 99, "keypad_period" }, { 100, "non_us_backslash" }, { 101, "application" }, { 103, "keypad_equals" },
        { 104, "f13" }, { 105, "f14" }, { 106, "f15" }, { 107, "f16" }, { 108, "f17" }, { 109, "f18" },
        { 110, "f19" }, { 111, "f20" }, { 112, "f21" }, { 113, "f22" }, { 114, "f23" }, { 115, "f24" },
        { 133, "keypad_comma" },
        { 135, "international_1" }, { 136, "international_2" }, { 137, "international_3" },
        { 138, "international_4" }, { 139, "international_5" }, { 140, "international_6" },
        { 224, "left_ctrl" }, { 225, "left_shift" }, { 226, "left_alt" },
        { 228, "right_ctrl" }, { 229, "right_shift" }, { 230, "right_alt" },
    };
    constexpr size_t kKeyCount = std::size(kKeys);

    namespace detail
    {
        constexpr char Lower(char c)
        {
            return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
        }

        // `text` in any case against a lower-case token.
        constexpr bool Same(std::string_view text, const char* token)
        {
            size_t i = 0;
            for (; i < text.size() && token[i]; i++)
                if (Lower(text[i]) != token[i])
                    return false;
            return i == text.size() && token[i] == '\0';
        }
    }

    // The file's name for a code: "none" for kNone; nullptr for a code that
    // isn't a key here.
    constexpr const char* Token(uint16_t code)
    {
        if (code == kNone)
            return "none";
        for (const Key& k : kKeys)
            if (k.code == code)
                return k.token;
        return nullptr;
    }

    // The code for a name in the file, in any case; kUnknown when it isn't one.
    constexpr uint16_t Code(std::string_view token)
    {
        if (detail::Same(token, "none"))
            return kNone;
        for (const Key& k : kKeys)
            if (detail::Same(token, k.token))
                return k.code;
        return kUnknown;
    }

    // A value a Key option can hold: no key, or a key of the table.
    constexpr bool Known(uint16_t code)
    {
        return code == kNone || Token(code) != nullptr;
    }
}
