// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// A strict JSON reader (RFC 8259, UTF-8 per RFC 3629): read as it goes
// (update/check.cpp picks GitHub's fields that way, no tree), or as a tree
// for small files (Parse). Errors say what was wrong and at which byte.
// Self-contained, like check.cpp.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace update::json
{
    enum class Kind : uint8_t {
        String, Number, True, False, Null, Object, Array
    };

    // Values inside more containers than this are refused.
    inline constexpr int kMaxDepth = 64;

    // The UTF-8 sequence at s[i] (no overlong forms, no surrogates, not
    // past U+10FFFF): its length and code point, or 0 if it isn't one.
    size_t DecodeUtf8(std::string_view s, size_t i, uint32_t& cp);
    void AppendUtf8(uint32_t cp, std::string& out);

    // Reads at s[i] and moves i past what it read. The first Fail sticks:
    // "<what> at byte <i>".
    struct Reader
    {
        std::string_view s;
        size_t i = 0;
        std::string error;

        Reader() = default;
        explicit Reader(std::string_view text) : s(text) {}

        bool Fail(std::string_view what);
        bool Peek(char c) const
        {
            return i < s.size() && s[i] == c;
        }
        void Space();

        // At the opening quote. `out` null: checked, not kept.
        bool String(std::string* out);
        // `out`, if any, gets the number as written.
        bool Number(std::string* out = nullptr);
        bool Literal(std::string_view word);
        // Any value, `depth` containers in. Objects and arrays are checked,
        // not kept; `kind` and `text` (a string or a number) may be null.
        bool Value(int depth, Kind* kind, std::string* text);

        // At the '{' of an object at `depth`: member(name) for each member,
        // at its value (after the ':'), which it reads. `keepNames` false:
        // names are checked, `name` stays empty.
        template <class F>
        bool Members(int depth, bool keepNames, F&& member);
        // At the '[' of an array at `depth`: item() for each item, which it
        // reads.
        template <class F>
        bool Items(int depth, F&& item);

    private:
        bool Hex4(uint32_t& v);
        bool Digits();
    };

    // --- A tree, for small files ---

    struct Value
    {
        Kind kind = Kind::Null;
        std::string text;               // a string, or a number as written
        std::vector<std::string> keys;  // object members, in order
        std::vector<Value> values;      // their values, or the array items

        // A member by name: null if it's missing,
        // and if it's there twice (`error` says so).
        const Value* Member(std::string_view name, std::string& error) const;
    };
    // Nothing but whitespace after the value. Else false and what was wrong.
    bool Parse(std::string_view text, Value& out, std::string& error);

    template <class F>
    bool Reader::Members(int depth, bool keepNames, F&& member)
    {
        if (depth > kMaxDepth)
            return Fail("nesting deeper than 64 levels");
        i++;
        Space();
        if (Peek('}'))
        {
            i++;
            return true;
        }
        std::string name;
        while (true)
        {
            Space();
            if (!Peek('"'))
                return Fail("a missing member name");
            name.clear();
            if (!String(keepNames ? &name : nullptr))
                return false;
            Space();
            if (!Peek(':'))
                return Fail("a missing ':'");
            i++;
            if (!member(name))
                return false;
            Space();
            if (Peek(','))
            {
                i++;
                continue;
            }
            if (Peek('}'))
            {
                i++;
                return true;
            }
            return Fail("a missing ',' or '}'");
        }
    }

    template <class F>
    bool Reader::Items(int depth, F&& item)
    {
        if (depth > kMaxDepth)
            return Fail("nesting deeper than 64 levels");
        i++;
        Space();
        if (Peek(']'))
        {
            i++;
            return true;
        }
        while (true)
        {
            Space();
            if (!item())
                return false;
            Space();
            if (Peek(','))
            {
                i++;
                continue;
            }
            if (Peek(']'))
            {
                i++;
                return true;
            }
            return Fail("a missing ',' or ']'");
        }
    }
}
