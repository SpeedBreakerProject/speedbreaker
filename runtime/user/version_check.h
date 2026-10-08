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
    // emphasis and code marks dropped (the note draws plain text).
    struct NoteLine
    {
        enum class Kind : uint8_t { Text, Bullet, Heading, Gap } kind;
        std::string text;
    };

    inline std::string PlainText(std::string_view line)
    {
        std::string out;
        for (size_t i = 0; i < line.size(); i++)
        {
            if (line[i] == '`' || (line[i] == '*' && i + 1 < line.size() && line[i + 1] == '*'))
            {
                if (line[i] == '*')
                    i++;
                continue;
            }
            out += line[i];
        }
        return out;
    }

    inline std::vector<NoteLine> ParseNote(std::string_view entry)
    {
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
                next = { NoteLine::Kind::Bullet, PlainText(body.substr(2)) };
            else if (body.starts_with("#"))
                next = { NoteLine::Kind::Heading, PlainText(body.substr(body.find_first_not_of('#') == std::string_view::npos
                                                                               ? body.size()
                                                                               : body.find_first_not_of('#'))) };
            else if (!gap && !lines.empty() && (lines.back().kind == NoteLine::Kind::Bullet || lines.back().kind == NoteLine::Kind::Text))
            {
                // The next line of a bullet or a paragraph.
                lines.back().text += " " + PlainText(body);
                continue;
            }
            else
                next.text = PlainText(body);
            if (next.kind == NoteLine::Kind::Heading)
                next.text.erase(0, next.text.find_first_not_of(' '));
            if (gap)
                lines.push_back({ NoteLine::Kind::Gap, {} });
            gap = false;
            lines.push_back(std::move(next));
        }
        return lines;
    }
}
