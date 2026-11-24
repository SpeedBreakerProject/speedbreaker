// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// What the version that ran last says about this launch, for the session
// log's [version] line (a bug report then shows when a problem began) and
// the one-time "What's new" note. The last version is kept in the player's
// settings.toml ([app] last_version, settings::LastVersion) and recorded at
// each launch. Header only, for tests/version_check_test.cpp.
//
// Offline only: nothing is checked against anything but the settings file.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace version
{
    // "0.1.0" (or "v0.1.0"): its three numbers. False for anything else.
    inline bool Parse(std::string_view text, std::array<uint32_t, 3>& out)
    {
        if (!text.empty() && (text.front() == 'v' || text.front() == 'V'))
            text.remove_prefix(1);
        size_t part = 0;
        out = {};
        bool digit = false;
        for (char c : text)
        {
            if (c >= '0' && c <= '9')
            {
                if (out[part] > 99999)
                    return false;
                out[part] = out[part] * 10 + uint32_t(c - '0');
                digit = true;
            }
            else if (c == '.' && digit && part < 2)
            {
                part++;
                digit = false;
            }
            else
                return false;
        }
        return digit && part == 2;
    }

    // -1, 0 or 1. Versions that don't parse compare as text, after any that do.
    inline int Compare(std::string_view a, std::string_view b)
    {
        std::array<uint32_t, 3> x, y;
        bool px = Parse(a, x), py = Parse(b, y);
        if (px && py)
            return x < y ? -1 : x > y ? 1 : 0;
        if (px != py)
            return px ? -1 : 1;
        return a < b ? -1 : a > b ? 1 : 0;
    }

    enum class Change : uint8_t
    {
        FirstRun,         // no settings.toml yet: a new player
        Unversioned,      // a settings.toml from a build before versions were recorded
        Same,             // the version that ran last
        Updated,          // a newer version than the last
        Older,            // an older version than the last (a player went back)
    };

    // `last`: the version settings.toml recorded ("" for none); `hadSettings`:
    // there was a settings.toml at all.
    inline Change Classify(std::string_view last, bool hadSettings, std::string_view current)
    {
        if (last.empty())
            return hadSettings ? Change::Unversioned : Change::FirstRun;
        int c = Compare(current, last);
        return c == 0 ? Change::Same : c > 0 ? Change::Updated : Change::Older;
    }

    // The session log's line, after "[version] ".
    inline std::string Describe(Change change, std::string_view last, std::string_view current)
    {
        std::string v = "v" + std::string(current), was = "v" + std::string(last);
        switch (change)
        {
        case Change::FirstRun: return "first run (" + v + ")";
        case Change::Unversioned: return "updated to " + v + " from a build before versions were recorded";
        case Change::Same: return v + ", the same as the last run";
        case Change::Updated: return "updated from " + was + " to " + v;
        case Change::Older: return "went back from " + was + " to " + v;
        }
        return v;
    }

    // The note shows on the first launch of a newer version (and once after
    // a build from before versions), never on a new player's first run, a
    // relaunch or a step back; and only with an entry to show.
    inline bool ShowWhatsNew(Change change, std::string_view entry)
    {
        if (entry.find_first_not_of(" \t\r\n") == std::string_view::npos)
            return false;
        return change == Change::Updated || change == Change::Unversioned;
    }

    // A CHANGELOG.md entry as the note lays it out, line by line: Markdown's
    // emphasis and code marks dropped (the note draws plain text), and a
    // no-break space (kNoBreakSpace) wherever the line must not break.
    struct NoteLine
    {
        enum class Kind : uint8_t { Text, Bullet, Heading, Gap } kind;
        std::string text;
    };

    // U+00A0 in UTF-8. WrapNote never breaks a line there, and draws it as a
    // plain space.
    constexpr std::string_view kNoBreakSpace = "\xC2\xA0";

    // Markdown's marks dropped. A `code` span (a path, a file's name, a
    // setting) keeps its spaces as no-break ones: it stays on one line.
    inline std::string PlainText(std::string_view line)
    {
        std::string out;
        bool code = false;
        for (size_t i = 0; i < line.size(); i++)
        {
            if (line[i] == '`' || (line[i] == '*' && i + 1 < line.size() && line[i + 1] == '*'))
            {
                if (line[i] == '*')
                    i++;
                else
                    code = !code;
                continue;
            }
            if (code && line[i] == ' ')
                out += kNoBreakSpace;
            else
                out += line[i];
        }
        return out;
    }

    // The units a number keeps on its line ("1 ms", "15 %", "190 MB").
    inline bool IsUnit(std::string_view word)
    {
        while (!word.empty() && std::string_view(".,;:!?)\"").find(word.back()) != std::string_view::npos)
            word.remove_suffix(1);
        for (std::string_view unit : { "%", "ms", "s", "fps", "Hz", "kHz", "MHz", "GHz", "B", "KB", "kB", "MB", "GB", "TB", "KiB",
                 "MiB", "GiB", "W", "mW", "nits", "px", "dB", "second", "seconds", "minute", "minutes", "hour", "hours", "day", "days" })
            if (word == unit)
                return true;
        return false;
    }

    // "12", "1.5", "7,800": digits, with '.' or ',' between them (after an
    // opening parenthesis or quote mark, if any).
    inline bool IsNumber(std::string_view word)
    {
        while (!word.empty() && (word.front() == '(' || word.front() == '"'))
            word.remove_prefix(1);
        if (word.empty() || word.front() < '0' || word.front() > '9')
            return false;
        for (size_t i = 0; i < word.size(); i++)
        {
            const char c = word[i];
            if (c >= '0' && c <= '9')
                continue;
            if ((c == '.' || c == ',') && i + 1 < word.size() && word[i + 1] >= '0' && word[i + 1] <= '9')
                continue;
            return false;
        }
        return true;
    }

    // "#87", "#101,", "(#81)": an issue's number.
    inline bool IsIssueNumber(std::string_view word)
    {
        while (!word.empty() && (word.front() == '(' || word.front() == '"'))
            word.remove_prefix(1);
        return word.size() >= 2 && word.front() == '#' && word[1] >= '0' && word[1] <= '9';
    }

    // The spaces a line keeps (a no-break space instead):
    // - between a number and its unit ("about 1 ms");
    // - through a range that has a unit ("9 to 21 %", "0.5 to 1.5 ms": v0.2.6's
    //   What's new ended lines at "9" and at "4", with "to 21 %" below);
    // - between a word and an issue's number ("issue #87");
    // - between a word and a parenthesis that closes in the word it opens
    //   ("Auto (3600x1600)", "hot ("serious")"). One that goes on is a place
    //   to break like any other: holding "GPU (reported with ..." ended the
    //   line before it a word early.
    inline std::string KeepTogether(std::string_view text)
    {
        // The text's spaces: plain ones, and those a `code` span holds already.
        struct Space
        {
            size_t at;
            bool plain;
        };
        std::vector<Space> spaces;
        for (size_t i = 0; i < text.size(); i++)
        {
            if (text[i] == ' ')
                spaces.push_back({ i, true });
            else if (text.substr(i, 2) == kNoBreakSpace)
                spaces.push_back({ i++, false });
        }
        auto after = [&](size_t n) { return spaces[n].at + (spaces[n].plain ? 1 : kNoBreakSpace.size()); };
        // Word n lies before space n; the last one, after the last space.
        // Empty between two spaces in a row, and past the text's end.
        auto word = [&](size_t n) {
            if (n > spaces.size())
                return std::string_view();
            const size_t start = n == 0 ? 0 : after(n - 1);
            return text.substr(start, (n == spaces.size() ? text.size() : spaces[n].at) - start);
        };

        std::vector<bool> held(spaces.size(), false);
        for (size_t n = 0; n < spaces.size(); n++)
        {
            const std::string_view left = word(n), right = word(n + 1);
            if (left.empty() || right.empty())
                continue;
            const bool unit = IsNumber(left) && IsUnit(right);
            const bool paren = right.front() == '(' && right.find(')') != std::string_view::npos;
            if (unit || paren || IsIssueNumber(right))
                held[n] = true;
            if (IsNumber(left) && right == "to" && IsNumber(word(n + 2)) && IsUnit(word(n + 3)))
                held[n] = held[n + 1] = true;  // the unit's own space: by the rule above
        }

        std::string out;
        size_t from = 0;
        for (size_t n = 0; n < spaces.size(); n++)
        {
            out += text.substr(from, spaces[n].at - from);
            out += spaces[n].plain && !held[n] ? std::string_view(" ") : kNoBreakSpace;
            from = after(n);
        }
        out += text.substr(from);
        return out;
    }

    // Markdown's heading mark: 1 to 6 '#'s, then a space or the line's end. The
    // length of the mark, or 0 when the line isn't a heading ("#90, reported by
    // ..." in a wrapped paragraph is text).
    inline size_t HeadingMark(std::string_view body)
    {
        size_t hashes = body.find_first_not_of('#');
        if (hashes == std::string_view::npos)
            hashes = body.size();
        if (hashes == 0 || hashes > 6)
            return 0;
        return hashes == body.size() || body[hashes] == ' ' || body[hashes] == '\t' ? hashes : 0;
    }

    inline std::vector<NoteLine> ParseNote(std::string_view entry)
    {
        // Each line's Markdown first (a bullet's or a paragraph's lines
        // joined, so a `code` span may go on past a line's end), then its text.
        std::vector<NoteLine> lines;
        bool gap = false;
        while (!entry.empty())
        {
            size_t end = entry.find('\n');
            std::string_view line = entry.substr(0, end);
            entry = end == std::string_view::npos ? std::string_view() : entry.substr(end + 1);
            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);
            size_t indent = line.find_first_not_of(' ');
            if (indent == std::string_view::npos)
            {
                gap = !lines.empty();
                continue;
            }
            std::string_view body = line.substr(indent);
            NoteLine next{ NoteLine::Kind::Text, {} };
            if (body.starts_with("- ") || body.starts_with("* "))
                next = { NoteLine::Kind::Bullet, std::string(body.substr(2)) };
            else if (size_t mark = HeadingMark(body))
                next = { NoteLine::Kind::Heading, std::string(body.substr(mark)) };
            else if (!gap && !lines.empty() && (lines.back().kind == NoteLine::Kind::Bullet || lines.back().kind == NoteLine::Kind::Text))
            {
                // The next line of a bullet or a paragraph.
                lines.back().text += " " + std::string(body);
                continue;
            }
            else
                next.text = std::string(body);
            if (gap)
                lines.push_back({ NoteLine::Kind::Gap, {} });
            gap = false;
            lines.push_back(std::move(next));
        }
        for (NoteLine& line : lines)
        {
            if (line.kind == NoteLine::Kind::Gap)
                continue;
            line.text = PlainText(line.text);
            if (line.kind == NoteLine::Kind::Heading)
                line.text.erase(0, line.text.find_first_not_of(" \t"));
            line.text = KeepTogether(line.text);
        }
        return lines;
    }

    // A note line laid out `width` wide, as it is drawn: '\n' where each line
    // ends, no-break spaces as plain ones. It breaks only at a space, so a
    // path stays whole ("~/.local/share/...": ImGui's own wrapping also
    // breaks after a '.' or a quote mark before a letter), and never at a
    // no-break space. A word wider than the line alone breaks at its
    // no-break spaces, then wherever it must; nothing is dropped but the
    // spaces where lines end. `measure(text)`: the width text is drawn.
    template <typename Measure>
    std::string WrapNote(std::string_view text, float width, const Measure& measure)
    {
        std::string shown;
        std::vector<size_t> breaks, held;  // in `shown`: the spaces it may break at, and the no-break ones
        for (size_t i = 0; i < text.size(); i++)
        {
            if (text.substr(i, 2) == kNoBreakSpace)
            {
                held.push_back(shown.size());
                shown += ' ';
                i++;
                continue;
            }
            if (text[i] == ' ')
                breaks.push_back(shown.size());
            shown += text[i];
        }
        const std::string_view all = shown;
        std::string out;
        size_t start = 0;
        auto fits = [&](size_t end) { return measure(all.substr(start, end - start)) <= width; };
        // The last point in `points` after `start` and before `limit` whose
        // line fits (the first space of a run: no line ends in a space).
        auto lastFit = [&](const std::vector<size_t>& points, size_t limit) {
            size_t best = std::string_view::npos;
            for (size_t p : points)
            {
                if (p <= start || all[p - 1] == ' ')
                    continue;
                if (p >= limit || !fits(p))
                    break;
                best = p;
            }
            return best;
        };
        while (start < all.size())
        {
            if (fits(all.size()))
            {
                out += all.substr(start);
                break;
            }
            size_t at = lastFit(breaks, all.size());
            if (at == std::string_view::npos)
            {
                // The first word alone is wider than the line.
                size_t wordEnd = all.size();
                for (size_t p : breaks)
                    if (p > start)
                    {
                        wordEnd = p;
                        break;
                    }
                at = lastFit(held, wordEnd);
            }
            if (at == std::string_view::npos)
            {
                // Not even that: as many whole characters as fit, at least one.
                size_t cut = start;
                do
                {
                    cut++;
                    while (cut < all.size() && (static_cast<unsigned char>(all[cut]) & 0xC0) == 0x80)
                        cut++;
                } while (cut < all.size() && fits(cut));
                if (cut > start + 1 && !fits(cut))
                {
                    size_t back = cut - 1;
                    while (back > start && (static_cast<unsigned char>(all[back]) & 0xC0) == 0x80)
                        back--;
                    if (back > start)
                        cut = back;
                }
                out += all.substr(start, cut - start);
                if (cut < all.size())
                    out += '\n';
                start = cut;
                continue;
            }
            out += all.substr(start, at - start);
            out += '\n';
            start = at + 1;
            while (start < all.size() && all[start] == ' ')
                start++;
        }
        return out;
    }
}
