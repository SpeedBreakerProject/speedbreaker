// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See check.h.
//
// Self-contained (no runtime headers but user/version_check.h) so the test
// can build it alone. Formats: Semantic Versioning 2.0.0, RFC 8259 (JSON),
// RFC 3629 (UTF-8), RFC 9112 (HTTP/1.1 messages, as curl prints them),
// GitHub's REST API for releases.
#include "check.h"

#include <algorithm>
#include <cstring>
#include <format>

namespace update
{
    namespace
    {
        bool IsDigit(char c)
        {
            return c >= '0' && c <= '9';
        }

        bool IsAlpha(char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        }

        bool IsAlnum(char c)
        {
            return IsDigit(c) || IsAlpha(c);
        }

        char Lower(char c)
        {
            return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
        }

        bool EqualNoCase(std::string_view a, std::string_view b)
        {
            if (a.size() != b.size())
                return false;
            for (size_t i = 0; i < a.size(); i++)
                if (Lower(a[i]) != Lower(b[i]))
                    return false;
            return true;
        }

        bool ContainsNoCase(std::string_view text, std::string_view word)
        {
            if (word.empty() || text.size() < word.size())
                return word.empty();
            for (size_t i = 0; i + word.size() <= text.size(); i++)
                if (EqualNoCase(text.substr(i, word.size()), word))
                    return true;
            return false;
        }

        std::string_view TrimLeft(std::string_view s)
        {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
                s.remove_prefix(1);
            return s;
        }

        std::string_view Trim(std::string_view s)
        {
            s = TrimLeft(s);
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
                s.remove_suffix(1);
            return s;
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

        // The UTF-8 sequence at s[i] (RFC 3629: no overlong forms, no
        // surrogates, nothing past U+10FFFF): its length and code point, or
        // 0 for a byte that starts no valid sequence.
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

        // Text for one line on screen: valid UTF-8 only, no control
        // characters (C0, DEL, C1) or bidirectional overrides, white space
        // (tabs, line breaks, no-break spaces) as one space, none at either end.
        std::string Sanitize(std::string_view s)
        {
            std::string out;
            out.reserve(s.size());
            for (size_t i = 0; i < s.size();)
            {
                uint32_t cp = 0;
                size_t n = DecodeUtf8(s, i, cp);
                if (n == 0)
                {
                    i++;  // a stray byte
                    continue;
                }
                i += n;
                if (cp == ' ' || (cp >= '\t' && cp <= '\r') || cp == 0xA0)
                {
                    if (!out.empty() && out.back() != ' ')
                        out += ' ';
                    continue;
                }
                const bool control = cp < 0x20 || (cp >= 0x7F && cp <= 0x9F);
                const bool bidi = (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2066 && cp <= 0x2069) || cp == 0x200E || cp == 0x200F;
                if (!control && !bidi)
                    out.append(s.substr(i - n, n));
            }
            while (!out.empty() && out.back() == ' ')
                out.pop_back();
            return out;
        }

        // The longest prefix of `s` of at most `max` bytes that ends on a
        // character boundary.
        size_t Utf8Prefix(std::string_view s, size_t max)
        {
            if (s.size() <= max)
                return s.size();
            size_t end = max;
            while (end > 0 && (uint8_t(s[end]) & 0xC0) == 0x80)
                end--;
            return end;
        }

        // A line cut to `max` bytes: at the last space if there is one in its
        // last third, with "..." after.
        std::string Cut(std::string_view s, size_t max)
        {
            if (s.size() <= max)
                return std::string(s);
            size_t keep = max > 3 ? max - 3 : 0;
            size_t end = Utf8Prefix(s, keep);
            if (size_t space = s.substr(0, end).rfind(' '); space != std::string_view::npos && space > end * 2 / 3)
                end = space;
            std::string out(Trim(s.substr(0, end)));
            out += "...";
            return out;
        }

        // --- SemVer ---

        bool Number(std::string_view s, uint64_t& out)
        {
            if (s.empty() || s.size() > 15 || (s.size() > 1 && s[0] == '0'))
                return false;
            uint64_t v = 0;
            for (char c : s)
            {
                if (!IsDigit(c))
                    return false;
                v = v * 10 + uint64_t(c - '0');
            }
            out = v;
            return true;
        }

        bool AllDigits(std::string_view s)
        {
            return !s.empty() && std::all_of(s.begin(), s.end(), IsDigit);
        }

        // Dot-separated identifiers of [0-9A-Za-z-], none empty.
        bool Identifiers(std::string_view s, std::vector<std::string>* out, bool numericNoLeadingZero)
        {
            while (true)
            {
                size_t dot = s.find('.');
                std::string_view part = s.substr(0, dot);
                if (part.empty() || !std::all_of(part.begin(), part.end(), [](char c) { return IsAlnum(c) || c == '-'; }))
                    return false;
                if (numericNoLeadingZero && AllDigits(part) && part.size() > 1 && part[0] == '0')
                    return false;
                if (out)
                    out->emplace_back(part);
                if (dot == std::string_view::npos)
                    return true;
                s.remove_prefix(dot + 1);
            }
        }

        // --- JSON (RFC 8259) ---

        enum class Kind : uint8_t { String, Number, True, False, Null, Object, Array };

        enum Field : int { kTag, kName, kUrl, kBody, kMessage, kDraft, kPrerelease, kFields };
        constexpr std::string_view kFieldNames[kFields] = { "tag_name", "name", "html_url", "body", "message", "draft", "prerelease" };
        constexpr int kMaxDepth = 64;

        struct JsonReader
        {
            std::string_view s;
            size_t i = 0;
            std::string error;

            bool Fail(std::string_view what)
            {
                if (error.empty())
                    error = std::format("{} at byte {}", what, i);
                return false;
            }

            bool Peek(char c) const
            {
                return i < s.size() && s[i] == c;
            }

            void Space()
            {
                while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
                    i++;
            }

            bool Hex4(uint32_t& v)
            {
                if (s.size() - i < 4)
                    return Fail("a short \\u escape");
                v = 0;
                for (int k = 0; k < 4; k++)
                {
                    char c = s[i];
                    uint32_t d = IsDigit(c) ? uint32_t(c - '0') : (Lower(c) >= 'a' && Lower(c) <= 'f') ? uint32_t(Lower(c) - 'a' + 10) : 99;
                    if (d == 99)
                        return Fail("a bad \\u escape");
                    v = v * 16 + d;
                    i++;
                }
                return true;
            }

            // At the opening quote. `out` null: checked, not kept.
            bool String(std::string* out)
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
                    if (c == '\\')
                    {
                        if (++i >= s.size())
                            return Fail("an unterminated string");
                        char e = s[i++], literal = 0;
                        switch (e)
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
                        continue;
                    }
                    uint32_t cp = 0;
                    size_t n = DecodeUtf8(s, i, cp);
                    if (n == 0)
                        return Fail("invalid UTF-8");
                    if (out)
                        out->append(s.substr(i, n));
                    i += n;
                }
            }

            bool Digits()
            {
                if (i >= s.size() || !IsDigit(s[i]))
                    return Fail("a bad number");
                while (i < s.size() && IsDigit(s[i]))
                    i++;
                return true;
            }

            bool Number()
            {
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
                return true;
            }

            bool Literal(std::string_view word)
            {
                if (s.substr(i, word.size()) != word)
                    return Fail("an unknown word");
                i += word.size();
                return true;
            }

            bool Value(int depth, Kind* kind, std::string* text)
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
                    k = Kind::Object, ok = Object(depth + 1, nullptr);
                else if (c == '[')
                    k = Kind::Array, ok = Array(depth + 1);
                else if (c == 't')
                    k = Kind::True, ok = Literal("true");
                else if (c == 'f')
                    k = Kind::False, ok = Literal("false");
                else if (c == 'n')
                    k = Kind::Null, ok = Literal("null");
                else if (c == '-' || IsDigit(c))
                    k = Kind::Number, ok = Number();
                else
                    return Fail(c >= 0x20 && c < 0x7F ? std::format("an unexpected '{}'", c) : std::format("an unexpected byte 0x{:02x}", uint8_t(c)));
                if (ok && kind)
                    *kind = k;
                return ok;
            }

            static bool Assign(Release& r, int field, Kind kind, std::string&& text)
            {
                auto string = [&](std::string& to, bool nullable) {
                    if (kind == Kind::String)
                        to = std::move(text);
                    return kind == Kind::String || (nullable && kind == Kind::Null);
                };
                auto boolean = [&](bool& to) {
                    to = kind == Kind::True;
                    return kind == Kind::True || kind == Kind::False;
                };
                switch (field)
                {
                case kTag: return r.hasTag = string(r.tag, false);
                case kName: return string(r.name, true);
                case kUrl: return string(r.url, true);
                case kBody: return string(r.body, true);
                case kMessage: return string(r.message, true);
                case kDraft: return boolean(r.draft);
                case kPrerelease: return boolean(r.prerelease);
                }
                return false;
            }

            // At the '{'. `top`: the document's own object, whose members are read.
            bool Object(int depth, Release* top)
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
                uint32_t seen = 0;
                while (true)
                {
                    Space();
                    if (!Peek('"'))
                        return Fail("a missing member name");
                    std::string key;
                    if (!String(top ? &key : nullptr))
                        return false;
                    Space();
                    if (!Peek(':'))
                        return Fail("a missing ':'");
                    i++;
                    int field = -1;
                    for (int f = 0; top && f < kFields; f++)
                        if (key == kFieldNames[f])
                            field = f;
                    if (field >= 0)
                    {
                        if (seen & (1u << field))
                            return Fail(std::format("a second \"{}\"", key));
                        seen |= 1u << field;
                        Space();
                        const size_t at = i;
                        Kind kind = Kind::Null;
                        std::string text;
                        if (!Value(depth, &kind, &text))
                            return false;
                        if (!Assign(*top, field, kind, std::move(text)))
                        {
                            i = at;
                            return Fail(std::format("\"{}\" of the wrong type", key));
                        }
                    }
                    else if (!Value(depth, nullptr, nullptr))
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

            bool Array(int depth)
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
                    if (!Value(depth, nullptr, nullptr))
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
        };

        // --- Markdown to plain lines ---

        bool IsAsciiPunct(char c)
        {
            return (c >= '!' && c <= '/') || (c >= ':' && c <= '@') || (c >= '[' && c <= '`') || (c >= '{' && c <= '~');
        }

        // A letter or digit for emphasis rules (non-ASCII counts as one).
        bool IsWordByte(char c)
        {
            return IsAlnum(c) || uint8_t(c) >= 0x80;
        }

        // How far ahead a mark looks for its other half (a link's ']' and
        // ')', code's closing backticks, a tag's '>'): far enough for any
        // real one, and a line of a thousand unclosed '[' stays linear.
        constexpr size_t kLookAhead = 2048;

        // The `close` that ends the `open` at s[at], with nesting and
        // backslash escapes; npos if none within kLookAhead.
        size_t Match(std::string_view s, size_t at, char open, char close)
        {
            int depth = 0;
            for (size_t k = at; k < s.size() && k - at < kLookAhead; k++)
            {
                if (s[k] == '\\')
                    k++;
                else if (s[k] == open)
                    depth++;
                else if (s[k] == close && --depth == 0)
                    return k;
            }
            return std::string_view::npos;
        }

        // "&amp;", "&#169;", "&#xA9;": what it stands for, and its length.
        bool Entity(std::string_view s, size_t at, std::string& out, size_t& length)
        {
            size_t semi = s.substr(at, 12).find(';');
            if (semi == std::string_view::npos || semi == 1)
                return false;
            semi += at;
            std::string_view name = s.substr(at + 1, semi - at - 1);
            static constexpr std::pair<std::string_view, std::string_view> kNamed[] = {
                { "amp", "&" }, { "lt", "<" }, { "gt", ">" }, { "quot", "\"" }, { "apos", "'" }, { "nbsp", " " },
                { "mdash", "\xE2\x80\x94" }, { "ndash", "\xE2\x80\x93" }, { "hellip", "..." }, { "copy", "\xC2\xA9" },
            };
            for (auto& [n, text] : kNamed)
                if (name == n)
                {
                    out = text;
                    length = semi - at + 1;
                    return true;
                }
            if (name[0] != '#' || name.size() < 2)
                return false;
            const bool hex = name[1] == 'x' || name[1] == 'X';
            std::string_view digits = name.substr(hex ? 2 : 1);
            if (digits.empty() || digits.size() > 6)
                return false;
            uint32_t cp = 0;
            for (char c : digits)
            {
                uint32_t d = IsDigit(c) ? uint32_t(c - '0') : (hex && Lower(c) >= 'a' && Lower(c) <= 'f') ? uint32_t(Lower(c) - 'a' + 10) : 99;
                if (d == 99)
                    return false;
                cp = cp * (hex ? 16 : 10) + d;
            }
            if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
                return false;
            out.clear();
            AppendUtf8(cp, out);
            length = semi - at + 1;
            return true;
        }

        // Inline Markdown to plain text (control characters are dropped later).
        std::string Inline(std::string_view s, int depth = 0)
        {
            std::string out;
            out.reserve(s.size());
            for (size_t i = 0; i < s.size();)
            {
                const char c = s[i];
                // \* is a literal *.
                if (c == '\\' && i + 1 < s.size() && IsAsciiPunct(s[i + 1]))
                {
                    out += s[i + 1];
                    i += 2;
                    continue;
                }
                // `code`: its text as it is.
                if (c == '`')
                {
                    size_t run = 0;
                    while (i + run < s.size() && s[i + run] == '`')
                        run++;
                    size_t close = std::string_view::npos;
                    for (size_t k = i + run; (k = s.find('`', k)) != std::string_view::npos && k - i < kLookAhead;)
                    {
                        size_t n = 0;
                        while (k + n < s.size() && s[k + n] == '`')
                            n++;
                        if (n == run)
                        {
                            close = k;
                            break;
                        }
                        k += n;
                    }
                    if (close == std::string_view::npos)
                    {
                        out.append(run, '`');
                        i += run;
                        continue;
                    }
                    std::string_view code = s.substr(i + run, close - i - run);
                    if (code.size() >= 2 && code.front() == ' ' && code.back() == ' ' && Trim(code).size())
                        code = code.substr(1, code.size() - 2);
                    out += code;
                    i = close + run;
                    continue;
                }
                // [text](url), [text][ref], ![alt](url): the text.
                if (c == '[' || (c == '!' && i + 1 < s.size() && s[i + 1] == '['))
                {
                    const size_t open = c == '!' ? i + 1 : i;
                    const size_t close = depth < 4 ? Match(s, open, '[', ']') : std::string_view::npos;
                    if (close != std::string_view::npos && close + 1 < s.size() && (s[close + 1] == '(' || s[close + 1] == '['))
                    {
                        size_t end = s[close + 1] == '(' ? Match(s, close + 1, '(', ')') : Match(s, close + 1, '[', ']');
                        if (end != std::string_view::npos)
                        {
                            out += Inline(s.substr(open + 1, close - open - 1), depth + 1);
                            i = end + 1;
                            continue;
                        }
                    }
                    out += c;
                    i++;
                    continue;
                }
                // <https://...>: the address; an HTML tag: nothing (a line break: a space).
                if (c == '<')
                {
                    size_t close = s.substr(i, kLookAhead).find('>');
                    if (close != std::string_view::npos)
                    {
                        close += i;
                        std::string_view inner = s.substr(i + 1, close - i - 1);
                        const bool spaced = inner.find_first_of(" \t") != std::string_view::npos;
                        if ((inner.starts_with("https://") || inner.starts_with("http://")) && !spaced)
                        {
                            out += inner;
                            i = close + 1;
                            continue;
                        }
                        size_t k = inner.starts_with('/') ? 1 : 0;
                        if (k < inner.size() && IsAlpha(inner[k]))
                        {
                            size_t nameEnd = k;
                            while (nameEnd < inner.size() && (IsAlnum(inner[nameEnd]) || inner[nameEnd] == '-'))
                                nameEnd++;
                            if (nameEnd == inner.size() || inner[nameEnd] == ' ' || inner[nameEnd] == '/' || inner[nameEnd] == '\t')
                            {
                                if (EqualNoCase(inner.substr(k, nameEnd - k), "br"))
                                    out += ' ';
                                i = close + 1;
                                continue;
                            }
                        }
                    }
                    out += c;
                    i++;
                    continue;
                }
                if (c == '&')
                {
                    std::string text;
                    size_t length = 0;
                    if (Entity(s, i, text, length))
                    {
                        out += text;
                        i += length;
                        continue;
                    }
                    out += c;
                    i++;
                    continue;
                }
                // Emphasis and strikethrough marks: dropped where they can open
                // or close (not between spaces, as in "2 * 3", nor inside a
                // word, as in snake_case or ~/Games).
                if (c == '*' || c == '_' || c == '~')
                {
                    size_t run = 0;
                    while (i + run < s.size() && s[i + run] == c)
                        run++;
                    const char before = i > 0 ? s[i - 1] : ' ', after = i + run < s.size() ? s[i + run] : ' ';
                    const bool spaceBefore = before == ' ' || before == '\t', spaceAfter = after == ' ' || after == '\t';
                    bool mark = !(spaceBefore && spaceAfter) && !(IsWordByte(before) && IsWordByte(after));
                    if (c == '~')
                        mark = mark && run == 2;
                    if (!mark)
                        out.append(run, c);
                    i += run;
                    continue;
                }
                out += c;
                i++;
            }
            return out;
        }

        bool IsRule(std::string_view t)
        {
            // ---, ***, ___ (spaces allowed between), and === (a setext underline).
            char c = t.empty() ? 0 : t[0];
            if (c != '-' && c != '*' && c != '_' && c != '=')
                return false;
            size_t n = 0;
            for (char x : t)
                if (x == c)
                    n++;
                else if (x != ' ' && x != '\t')
                    return false;
            return n >= 3 || (c == '=' && n >= 1);
        }

        bool IsTableRule(std::string_view t)
        {
            // |---|:--:|
            bool bar = false, dash = false;
            for (char x : t)
                if (x == '|')
                    bar = true;
                else if (x == '-')
                    dash = true;
                else if (x != ':' && x != ' ' && x != '\t')
                    return false;
            return bar && dash;
        }

        // "<!-- ... -->" removed (to the end, if it doesn't end), CR LF and CR as LF.
        std::string Normalize(std::string_view s)
        {
            std::string out;
            out.reserve(s.size());
            for (size_t i = 0; i < s.size();)
            {
                if (s.substr(i, 4) == "<!--")
                {
                    size_t end = s.find("-->", i + 4);
                    if (end == std::string_view::npos)
                        break;
                    i = end + 3;
                    continue;
                }
                if (s[i] == '\r')
                {
                    out += '\n';
                    i += (i + 1 < s.size() && s[i + 1] == '\n') ? 2 : 1;
                    continue;
                }
                out += s[i++];
            }
            return out;
        }

        struct NotesBuilder
        {
            Notes notes;
            size_t maxChars, maxLines, used = 0;
            bool gap = false;      // a blank line (or a rule) before the next one
            bool joinable = false; // a line without a mark of its own continues the last one

            bool Full() const
            {
                return notes.truncated;
            }

            void Add(version::NoteLine::Kind kind, std::string_view raw)
            {
                if (Full())
                    return;
                std::string text = Sanitize(raw);
                if (text.empty())
                    return;
                const bool gapFirst = gap && !notes.lines.empty();
                gap = false;
                if (notes.lines.size() + (gapFirst ? 2 : 1) > maxLines)
                {
                    notes.truncated = true;
                    return;
                }
                if (gapFirst)
                    notes.lines.push_back({ version::NoteLine::Kind::Gap, {} });
                if (used + text.size() > maxChars)
                {
                    text = Cut(text, maxChars > used ? maxChars - used : 0);
                    notes.truncated = true;
                }
                used += text.size();
                notes.lines.push_back({ kind, std::move(text) });
            }

            void Join(std::string_view raw)
            {
                if (Full())
                    return;
                std::string text = Sanitize(raw);
                if (text.empty())
                    return;
                std::string& last = notes.lines.back().text;
                if (used + 1 + text.size() > maxChars)
                {
                    text = Cut(text, maxChars > used + 1 ? maxChars - used - 1 : 0);
                    notes.truncated = true;
                }
                used += 1 + text.size();
                last += ' ';
                last += text;
            }
        };
    }

    // --- SemVer ---

    bool ParseSemver(std::string_view text, Semver& out)
    {
        if (!text.empty() && (text.front() == 'v' || text.front() == 'V'))
            text.remove_prefix(1);
        Semver v;
        if (size_t plus = text.find('+'); plus != std::string_view::npos)
        {
            if (!Identifiers(text.substr(plus + 1), nullptr, false))
                return false;
            v.build = text.substr(plus + 1);
            text = text.substr(0, plus);
        }
        if (size_t dash = text.find('-'); dash != std::string_view::npos)
        {
            if (!Identifiers(text.substr(dash + 1), &v.pre, true))
                return false;
            text = text.substr(0, dash);
        }
        uint64_t* parts[3] = { &v.major, &v.minor, &v.patch };
        for (int k = 0; k < 3; k++)
        {
            size_t dot = text.find('.');
            if ((k < 2) != (dot != std::string_view::npos))
                return false;
            if (!Number(text.substr(0, dot), *parts[k]))
                return false;
            text = k < 2 ? text.substr(dot + 1) : std::string_view();
        }
        out = std::move(v);
        return true;
    }

    int CompareSemver(const Semver& a, const Semver& b)
    {
        auto cmp = [](auto x, auto y) { return x < y ? -1 : x > y ? 1 : 0; };
        if (int c = cmp(a.major, b.major); c)
            return c;
        if (int c = cmp(a.minor, b.minor); c)
            return c;
        if (int c = cmp(a.patch, b.patch); c)
            return c;
        // A release outranks its pre-releases.
        if (a.pre.empty() || b.pre.empty())
            return cmp(a.pre.empty(), b.pre.empty());
        for (size_t k = 0; k < std::min(a.pre.size(), b.pre.size()); k++)
        {
            const std::string &x = a.pre[k], &y = b.pre[k];
            const bool xn = AllDigits(x), yn = AllDigits(y);
            int c;
            if (xn && yn)
                c = x.size() != y.size() ? cmp(x.size(), y.size()) : cmp(x.compare(y), 0);  // no leading zeros: longer is larger
            else if (xn != yn)
                c = xn ? -1 : 1;  // numbers before words
            else
                c = cmp(x.compare(y), 0);
            if (c)
                return c;
        }
        return cmp(a.pre.size(), b.pre.size());
    }

    std::string FormatSemver(const Semver& v)
    {
        std::string text = std::format("{}.{}.{}", v.major, v.minor, v.patch);
        for (size_t k = 0; k < v.pre.size(); k++)
            text += (k ? "." : "-") + v.pre[k];
        return text;
    }

    // --- JSON ---

    bool ParseRelease(std::string_view json, Release& out, std::string& error)
    {
        JsonReader reader;
        reader.s = json;
        Release release;
        reader.Space();
        if (reader.i >= json.size())
            reader.error = "an empty answer";
        else if (!reader.Peek('{'))
            reader.Fail("not a JSON object");
        else if (reader.Object(1, &release))
        {
            reader.Space();
            if (reader.i != json.size())
                reader.Fail("more after the JSON");
        }
        if (!reader.error.empty())
        {
            error = std::move(reader.error);
            return false;
        }
        out = std::move(release);
        return true;
    }

    // --- HTTP ---

    bool ParseHttpResponse(std::string_view raw, HttpResponse& out)
    {
        HttpResponse response;
        size_t pos = 0;
        bool any = false;
        while (raw.substr(pos).starts_with("HTTP/"))
        {
            size_t lf = raw.find("\n\n", pos), crlf = raw.find("\r\n\r\n", pos);
            size_t blockEnd, next;
            if (crlf != std::string_view::npos && (lf == std::string_view::npos || crlf < lf))
                blockEnd = crlf, next = crlf + 4;
            else if (lf != std::string_view::npos)
                blockEnd = lf, next = lf + 2;
            else
                return false;
            std::string_view block = raw.substr(pos, blockEnd - pos);
            // "HTTP/1.1 200 OK", "HTTP/2 403"
            size_t eol = block.find('\n');
            std::string_view status = block.substr(0, eol);
            if (!status.empty() && status.back() == '\r')
                status.remove_suffix(1);
            size_t space = status.find(' ');
            if (space == std::string_view::npos || status.size() < space + 4 || !AllDigits(status.substr(space + 1, 3)) ||
                (status.size() > space + 4 && status[space + 4] != ' '))
                return false;
            response.status = std::stoi(std::string(status.substr(space + 1, 3)));
            response.headers.clear();
            for (std::string_view rest = eol == std::string_view::npos ? std::string_view() : block.substr(eol + 1); !rest.empty();)
            {
                size_t end = rest.find('\n');
                std::string_view line = rest.substr(0, end);
                rest = end == std::string_view::npos ? std::string_view() : rest.substr(end + 1);
                if (!line.empty() && line.back() == '\r')
                    line.remove_suffix(1);
                if (line.empty())
                    continue;
                size_t colon = line.find(':');
                if (colon == std::string_view::npos || colon == 0)
                    return false;
                std::string name(Trim(line.substr(0, colon)));
                std::transform(name.begin(), name.end(), name.begin(), Lower);
                response.headers.emplace_back(std::move(name), std::string(Trim(line.substr(colon + 1))));
            }
            pos = next;
            any = true;
        }
        if (!any)
            return false;
        response.body = raw.substr(pos);
        out = std::move(response);
        return true;
    }

    std::string_view Header(const HttpResponse& response, std::string_view name)
    {
        for (const auto& [key, value] : response.headers)
            if (EqualNoCase(key, name))
                return value;
        return {};
    }

    // --- Release notes ---

    Notes CleanNotes(std::string_view markdown, size_t maxChars, size_t maxLines)
    {
        using K = version::NoteLine::Kind;
        NotesBuilder b;
        b.maxChars = maxChars;
        b.maxLines = maxLines;
        // GitHub allows 125,000 characters; the dialog shows far fewer.
        const std::string text = Normalize(markdown.substr(0, Utf8Prefix(markdown, 64 * 1024)));
        bool fence = false;
        char fenceChar = 0;
        size_t fenceRun = 0;
        for (std::string_view rest = text; !rest.empty() && !b.Full();)
        {
            size_t end = rest.find('\n');
            std::string_view raw = rest.substr(0, end);
            rest = end == std::string_view::npos ? std::string_view() : rest.substr(end + 1);
            size_t indent = 0;
            for (char c : raw)
                if (c == ' ')
                    indent++;
                else if (c == '\t')
                    indent += 4;
                else
                    break;
            std::string_view t = Trim(raw);

            // ``` blocks: their lines as they are.
            if (fence)
            {
                size_t run = 0;
                while (run < t.size() && t[run] == fenceChar)
                    run++;
                if (run >= fenceRun && Trim(t.substr(run)).empty())
                    fence = false;
                else
                    b.Add(K::Text, raw);
                b.joinable = false;
                continue;
            }
            if (t.empty())
            {
                b.gap = true;
                b.joinable = false;
                continue;
            }
            if (indent < 4 && (t.starts_with("```") || t.starts_with("~~~")))
            {
                fence = true;
                fenceChar = t[0];
                fenceRun = 0;
                while (fenceRun < t.size() && t[fenceRun] == fenceChar)
                    fenceRun++;
                b.joinable = false;
                continue;
            }
            // > quotes: their text.
            while (t.starts_with('>'))
                t = TrimLeft(t.substr(1));
            if (t.empty())
            {
                b.gap = true;
                b.joinable = false;
                continue;
            }
            // A rule: a gap; under a paragraph's line, a setext heading.
            if (IsRule(t))
            {
                if ((t[0] == '=' || t[0] == '-') && b.joinable && !b.gap && !b.notes.lines.empty() && b.notes.lines.back().kind == K::Text)
                    b.notes.lines.back().kind = K::Heading;
                else
                    b.gap = true;
                b.joinable = false;
                continue;
            }
            // # Heading
            if (t[0] == '#')
            {
                size_t n = 0;
                while (n < t.size() && t[n] == '#')
                    n++;
                if (n <= 6 && (n == t.size() || t[n] == ' ' || t[n] == '\t'))
                {
                    std::string_view h = Trim(t.substr(n));
                    while (!h.empty() && h.back() == '#')
                        h.remove_suffix(1);
                    b.Add(K::Heading, Inline(Trim(h)));
                    b.joinable = false;
                    continue;
                }
            }
            // [ref]: https://... (a link's target, not text)
            if (t[0] == '[')
                if (size_t close = Match(t, 0, '[', ']'); close != std::string_view::npos && close > 1 && t.substr(close + 1).starts_with(':'))
                    continue;
            if (IsTableRule(t))
                continue;
            // - bullet, * bullet, + bullet (and - [x] tasks)
            if ((t[0] == '-' || t[0] == '*' || t[0] == '+') && t.size() > 1 && (t[1] == ' ' || t[1] == '\t'))
            {
                std::string_view item = TrimLeft(t.substr(2));
                if (item.starts_with("[ ] ") || item.starts_with("[x] ") || item.starts_with("[X] "))
                    item.remove_prefix(4);
                b.Add(K::Bullet, Inline(item));
                b.joinable = true;
                continue;
            }
            // 1. step (the number stays)
            {
                size_t d = 0;
                while (d < t.size() && d < 9 && IsDigit(t[d]))
                    d++;
                if (d > 0 && d + 1 < t.size() && (t[d] == '.' || t[d] == ')') && (t[d + 1] == ' ' || t[d + 1] == '\t'))
                {
                    b.Add(K::Text, std::string(t.substr(0, d)) + ". " + Inline(Trim(t.substr(d + 1))));
                    b.joinable = true;
                    continue;
                }
            }
            // | a | b |: the cells, between bars.
            if (t[0] == '|')
            {
                std::string row;
                for (std::string_view cells = t.substr(1); !cells.empty();)
                {
                    size_t bar = 0;
                    while (bar < cells.size() && cells[bar] != '|')
                        bar += cells[bar] == '\\' ? 2 : 1;
                    std::string cell = Sanitize(Inline(Trim(cells.substr(0, std::min(bar, cells.size())))));
                    if (!cell.empty())
                        row += (row.empty() ? "" : " | ") + cell;
                    cells = bar < cells.size() ? cells.substr(bar + 1) : std::string_view();
                }
                b.Add(K::Text, row);
                b.joinable = false;
                continue;
            }
            // Paragraph text (and HTML lines, which may leave nothing).
            std::string line = Inline(t);
            if (Sanitize(line).empty())
                continue;
            if (b.joinable && !b.gap && !b.notes.lines.empty())
                b.Join(line);
            else
            {
                b.Add(K::Text, line);
                b.joinable = true;
            }
        }
        return std::move(b.notes);
    }

    std::string CleanLine(std::string_view text, size_t maxChars)
    {
        std::string flat = Normalize(text.substr(0, Utf8Prefix(text, 4096)));
        std::replace(flat.begin(), flat.end(), '\n', ' ');
        return Cut(Sanitize(Inline(Trim(flat))), maxChars);
    }

    // --- The outcome ---

    std::string SafePageUrl(std::string_view url)
    {
        constexpr std::string_view kAllowed = "-._~/%+:?=&#@!$'()*,;";
        const std::string_view prefix = kPagePrefix;
        if (url.size() <= prefix.size() || url.size() > 300 || !url.starts_with(prefix))
            return kReleasesPage;
        for (char c : url)
            if (!IsAlnum(c) && kAllowed.find(c) == std::string_view::npos)
                return kReleasesPage;
        if (url.find("/../") != std::string_view::npos || url.find("/./") != std::string_view::npos || url.ends_with("/.."))
            return kReleasesPage;
        return std::string(url);
    }

    Result Evaluate(const Fetched& fetched, std::string_view current, int64_t now)
    {
        Result r;
        Semver running;
        const bool runningOk = ParseSemver(current, running);
        r.current = runningOk ? FormatSemver(running) : CleanLine(current, 40);
        r.url = kReleasesPage;
        auto fail = [&r](Outcome outcome, std::string message, std::string log) {
            r.outcome = outcome;
            r.headline = "Couldn't check for updates";
            r.message = std::move(message);
            r.log = "check failed: " + log;
            return r;
        };
        const std::string detail = fetched.detail.empty() ? std::string() : " (" + CleanLine(fetched.detail, 200) + ")";
        switch (fetched.transport)
        {
        case Transport::Ok:
            break;
        case Transport::Offline:
            return fail(Outcome::Offline, "No connection to GitHub. Check that this device is online.", "offline" + detail);
        case Transport::Timeout:
            return fail(Outcome::Timeout, std::format("GitHub didn't answer within {} seconds. Try again later.", int(kTimeoutSeconds)),
                "timed out" + detail);
        case Transport::Secure:
            return fail(Outcome::Offline, "Couldn't connect to GitHub securely. Check the date and time.",
                "secure connection failed" + detail);
        case Transport::TooLarge:
            return fail(Outcome::Unreadable, "GitHub's answer was too large to read. Try again later.", "answer too large" + detail);
        case Transport::Garbled:
            return fail(Outcome::Unreadable, "GitHub's answer couldn't be read. Try again later.", "unreadable answer" + detail);
        case Transport::Unavailable:
            return fail(Outcome::Unavailable,
                fetched.detail.empty() ? "Checking for updates isn't available here yet." : CleanLine(fetched.detail, 200), "not available" + detail);
        case Transport::Failed:
            return fail(Outcome::Offline, std::format("The check couldn't be made{}. Try again later.", detail), "failed" + detail);
        }

        const HttpResponse& response = fetched.response;
        // GitHub allows 60 unauthenticated requests an hour per address:
        // past that, 403 (or 429) with x-ratelimit-remaining: 0 and the
        // reset time (Unix seconds) in x-ratelimit-reset.
        if (response.status == 403 || response.status == 429)
        {
            if (response.status == 429 || Header(response, "x-ratelimit-remaining") == "0" || ContainsNoCase(response.body, "rate limit"))
            {
                int64_t minutes = -1;
                auto seconds = [](std::string_view text, int64_t& out) {
                    uint64_t v = 0;
                    if (!Number(text, v))
                        return false;
                    out = int64_t(v);
                    return true;
                };
                int64_t reset = 0, retry = 0;
                if (seconds(Header(response, "x-ratelimit-reset"), reset) && reset > now)
                    minutes = (reset - now + 59) / 60;
                else if (seconds(Header(response, "retry-after"), retry) && retry > 0)
                    minutes = (retry + 59) / 60;
                minutes = std::min<int64_t>(minutes, 24 * 60);
                std::string when = minutes > 0 ? std::format("in {} minute{}", minutes, minutes == 1 ? "" : "s") : "later";
                return fail(Outcome::RateLimited,
                    std::format("GitHub's limit of 60 checks an hour is used up. Try again {}.", when),
                    std::format("rate-limited (HTTP {}, try again {})", response.status, when));
            }
        }
        if (response.status != 200)
        {
            Release answer;
            std::string ignored;
            ParseRelease(response.body, answer, ignored);
            std::string said = answer.message.empty() ? "" : ": " + CleanLine(answer.message, 160);
            return fail(Outcome::ServerError,
                response.status == 404 ? "GitHub found no release of SpeedBreaker (HTTP 404)."
                                       : std::format("GitHub answered with an error (HTTP {}). Try again later.", response.status),
                std::format("HTTP {}{}", response.status, said));
        }

        Release release;
        std::string error;
        constexpr const char* kUnreadable = "GitHub's answer couldn't be read. Try again later.";
        if (!ParseRelease(response.body, release, error))
            return fail(Outcome::Unreadable, kUnreadable, "unreadable answer: " + error);
        if (!release.hasTag)
            return fail(Outcome::Unreadable, kUnreadable, "unreadable answer: no tag_name");
        Semver latest;
        if (!ParseSemver(release.tag, latest))
            return fail(Outcome::Unreadable, kUnreadable, "unreadable answer: the tag \"" + CleanLine(release.tag, 40) + "\" isn't a version");
        if (!runningOk)
            return fail(Outcome::Unreadable, std::format("This build's version ({}) can't be compared with releases.", r.current),
                "this build's version \"" + r.current + "\" isn't a version");

        // Drafts and pre-releases are never offered (the "latest" release is
        // neither, but the answer is checked rather than trusted).
        const bool ignored = release.draft || release.prerelease || !latest.pre.empty();
        const int newer = CompareSemver(latest, running);
        if (ignored || newer <= 0)
        {
            r.outcome = Outcome::UpToDate;
            r.latest = ignored ? "" : FormatSemver(latest);
            r.headline = std::format("You're up to date (v{})", r.current);
            r.message = !ignored && newer < 0 ? std::format("The newest release is v{}; this build is newer.", r.latest)
                                               : "This is the newest release of SpeedBreaker.";
            r.log = ignored ? std::format("up to date: running v{}; the latest, {}, is a draft or pre-release (ignored)", r.current,
                                  CleanLine(release.tag, 40))
                            : std::format("up to date: running v{}, the latest release is v{}", r.current, r.latest);
            return r;
        }
        r.outcome = Outcome::Available;
        r.latest = FormatSemver(latest);
        r.url = SafePageUrl(release.url);
        r.name = CleanLine(release.name, 120);
        r.notes = CleanNotes(release.body);
        r.headline = std::format("v{} is available", r.latest);
        r.message = std::format("You have v{}.", r.current);
        r.log = std::format("v{} is available (running v{}): {}", r.latest, r.current, r.url);
        return r;
    }
}
