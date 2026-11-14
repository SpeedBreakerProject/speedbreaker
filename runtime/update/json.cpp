// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See json.h.
#include "json.h"

#include <format>
#include <utility>

namespace update::json
{
    namespace
    {
        bool IsDigit(char c)
        {
            return c >= '0' && c <= '9';
        }

        int HexDigit(char c)
        {
            if (IsDigit(c))
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            return -1;
        }

        // A value of the tree, with the reader's depth rules.
        bool Build(Reader& r, int depth, Value& out)
        {
            if (depth > kMaxDepth)
                return r.Fail("nesting deeper than 64 levels");
            r.Space();
            if (r.Peek('{'))
            {
                out.kind = Kind::Object;
                return r.Members(depth + 1, true, [&](std::string& name) {
                    out.keys.push_back(std::move(name));
                    return Build(r, depth + 1, out.values.emplace_back());
                });
            }
            if (r.Peek('['))
            {
                out.kind = Kind::Array;
                return r.Items(depth + 1, [&] {
                    return Build(r, depth + 1, out.values.emplace_back());
                });
            }
            return r.Value(depth, &out.kind, &out.text);
        }
    }

    size_t DecodeUtf8(std::string_view s, size_t i, uint32_t& cp)
    {
        const uint8_t b0 = uint8_t(s[i]);
        if (b0 < 0x80)
        {
            cp = b0;
            return 1;
        }
        size_t n;
        uint8_t lo = 0x80, hi = 0xBF;
        if (b0 >= 0xC2 && b0 <= 0xDF)
            n = 2;
        else if (b0 == 0xE0)
            n = 3, lo = 0xA0;
        else if ((b0 >= 0xE1 && b0 <= 0xEC) || b0 == 0xEE || b0 == 0xEF)
            n = 3;
        else if (b0 == 0xED)
            n = 3, hi = 0x9F;
        else if (b0 == 0xF0)
            n = 4, lo = 0x90;
        else if (b0 >= 0xF1 && b0 <= 0xF3)
            n = 4;
        else if (b0 == 0xF4)
            n = 4, hi = 0x8F;
        else
            return 0;
        if (s.size() - i < n)
            return 0;
        cp = b0 & (n == 2 ? 0x1F : n == 3 ? 0x0F : 0x07);
        for (size_t k = 1; k < n; k++)
        {
            const uint8_t b = uint8_t(s[i + k]);
            if (b < (k == 1 ? lo : 0x80) || b > (k == 1 ? hi : 0xBF))
                return 0;
            cp = (cp << 6) | (b & 0x3F);
        }
        return n;
    }

    void AppendUtf8(uint32_t cp, std::string& out)
    {
        if (cp < 0x80)
            out += char(cp);
        else if (cp < 0x800)
        {
            out += char(0xC0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000)
        {
            out += char(0xE0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
        else
        {
            out += char(0xF0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
    }

    // --- Reader ---

    bool Reader::Fail(std::string_view what)
    {
        if (error.empty())
            error = std::format("{} at byte {}", what, i);
        return false;
    }

    void Reader::Space()
    {
        while (i < s.size()
            && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
            i++;
    }

    bool Reader::Hex4(uint32_t& v)
    {
        if (s.size() - i < 4)
            return Fail("a short \\u escape");
        v = 0;
        for (int k = 0; k < 4; k++, i++)
        {
            const int d = HexDigit(s[i]);
            if (d < 0)
                return Fail("a bad \\u escape");
            v = v * 16 + uint32_t(d);
        }
        return true;
    }

    bool Reader::String(std::string* out)
    {
        i++;
        while (true)
        {
            if (i >= s.size())
                return Fail("an unterminated string");
            const uint8_t c = uint8_t(s[i]);
            if (c == '"')
            {
                i++;
                return true;
            }
            if (c < 0x20)
                return Fail("a control character in a string");
            if (c != '\\')
            {
                uint32_t cp = 0;
                const size_t n = DecodeUtf8(s, i, cp);
                if (n == 0)
                    return Fail("invalid UTF-8");
                if (out)
                    out->append(s.substr(i, n));
                i += n;
                continue;
            }
            if (++i >= s.size())
                return Fail("an unterminated string");
            char literal = 0;
            switch (s[i++])
            {
            case '"': literal = '"'; break;
            case '\\': literal = '\\'; break;
            case '/': literal = '/'; break;
            case 'b': literal = '\b'; break;
            case 'f': literal = '\f'; break;
            case 'n': literal = '\n'; break;
            case 'r': literal = '\r'; break;
            case 't': literal = '\t'; break;
            case 'u':
            {
                uint32_t cp = 0;
                if (!Hex4(cp))
                    return false;
                if (cp >= 0xD800 && cp <= 0xDBFF)
                {
                    // A high surrogate: a low one must follow.
                    uint32_t low = 0;
                    if (s.size() - i < 2 || s[i] != '\\' || s[i + 1] != 'u')
                        return Fail("an unpaired surrogate");
                    i += 2;
                    if (!Hex4(low))
                        return false;
                    if (low < 0xDC00 || low > 0xDFFF)
                        return Fail("an unpaired surrogate");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                }
                else if (cp >= 0xDC00 && cp <= 0xDFFF)
                    return Fail("an unpaired surrogate");
                if (out)
                    AppendUtf8(cp, *out);
                continue;
            }
            default:
                i--;
                return Fail("a bad escape");
            }
            if (out)
                *out += literal;
        }
    }

    bool Reader::Digits()
    {
        if (i >= s.size() || !IsDigit(s[i]))
            return Fail("a bad number");
        while (i < s.size() && IsDigit(s[i]))
            i++;
        return true;
    }

    bool Reader::Number(std::string* out)
    {
        const size_t start = i;
        if (Peek('-'))
            i++;
        if (Peek('0'))
            i++;
        else if (!Digits())
            return false;
        if (Peek('.'))
        {
            i++;
            if (!Digits())
                return false;
        }
        if (Peek('e') || Peek('E'))
        {
            i++;
            if (Peek('+') || Peek('-'))
                i++;
            if (!Digits())
                return false;
        }
        if (out)
            *out = s.substr(start, i - start);
        return true;
    }

    bool Reader::Literal(std::string_view word)
    {
        if (s.substr(i, word.size()) != word)
            return Fail("an unknown word");
        i += word.size();
        return true;
    }

    bool Reader::Value(int depth, Kind* kind, std::string* text)
    {
        if (depth > kMaxDepth)
            return Fail("nesting deeper than 64 levels");
        Space();
        if (i >= s.size())
            return Fail("a missing value");
        const char c = s[i];
        Kind k;
        bool ok;
        if (c == '"')
            k = Kind::String, ok = String(text);
        else if (c == '{')
            k = Kind::Object, ok = Members(depth + 1, false, [&](std::string&) {
                return Value(depth + 1, nullptr, nullptr);
            });
        else if (c == '[')
            k = Kind::Array, ok = Items(depth + 1, [&] {
                return Value(depth + 1, nullptr, nullptr);
            });
        else if (c == 't')
            k = Kind::True, ok = Literal("true");
        else if (c == 'f')
            k = Kind::False, ok = Literal("false");
        else if (c == 'n')
            k = Kind::Null, ok = Literal("null");
        else if (c == '-' || IsDigit(c))
            k = Kind::Number, ok = Number(text);
        else if (c >= 0x20 && c < 0x7F)
            return Fail(std::format("an unexpected '{}'", c));
        else
            return Fail(std::format("an unexpected byte 0x{:02x}", uint8_t(c)));
        if (ok && kind)
            *kind = k;
        return ok;
    }

    // --- Tree ---

    const Value* Value::Member(std::string_view name, std::string& error) const
    {
        const Value* found = nullptr;
        for (size_t m = 0; m < keys.size(); m++)
            if (keys[m] == name)
            {
                if (found)
                {
                    error = std::format("a second \"{}\"", name);
                    return nullptr;
                }
                found = &values[m];
            }
        return found;
    }

    bool Parse(std::string_view text, Value& out, std::string& error)
    {
        out = {};
        error.clear();
        Reader r{ text };
        if (Build(r, 0, out))
        {
            r.Space();
            if (r.i != text.size())
                r.Fail("more after the JSON");
        }
        if (!r.error.empty())
        {
            error = std::move(r.error);
            return false;
        }
        return true;
    }
}
