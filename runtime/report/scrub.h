// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See report.h (ScrubHome).
//
// A folder's path replaced in text that leaves the machine (a bug report):
// the home folder as "~", or on Windows the user's profile as
// "%USERPROFILE%". Header only, for tests/scrub_test.cpp.
#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace report::detail
{
    struct FolderName
    {
        std::string path;         // no trailing separator
        std::string replacement;  // "~", "%USERPROFILE%"
    };

    namespace scrub
    {
        inline bool PathChar(char c)
        {
            return std::isalnum(uint8_t(c)) || c == '.' || c == '_' || c == '-';
        }

        // How many bytes of `text` from `at` spell `path`, or 0. Windows
        // paths: ASCII letters in either case, and a separator as \, / or \\
        // (a backslash escaped, as in TOML or JSON). Elsewhere byte for byte.
        inline size_t MatchAt(std::string_view text, size_t at, std::string_view path, bool windows)
        {
            if (!windows)
                return text.compare(at, path.size(), path) == 0 ? path.size() : 0;
            size_t t = at;
            for (char p : path)
            {
                if (t >= text.size())
                    return 0;
                if (p == '\\' || p == '/')
                {
                    if (text[t] == '/')
                        t++;
                    else if (text[t] == '\\')
                        t += (t + 1 < text.size() && text[t + 1] == '\\') ? 2 : 1;
                    else
                        return 0;
                }
                else if (std::tolower(uint8_t(text[t])) == std::tolower(uint8_t(p)))
                    t++;
                else
                    return 0;
            }
            return t - at;
        }
    }

    // Each folder's path in `text` as its replacement, where it is the whole
    // folder (/home/deck, not /home/deck2) and not the tail of a longer path
    // (/mnt/home/deck); the longest paths first. `windows`: see MatchAt.
    inline std::string ReplaceFolders(std::string text, std::vector<FolderName> folders, bool windows)
    {
        std::sort(folders.begin(), folders.end(), [](const FolderName& a, const FolderName& b) { return a.path.size() > b.path.size(); });
        for (const FolderName& folder : folders)
        {
            if (folder.path.empty())
                continue;
            std::string out;
            out.reserve(text.size());
            size_t from = 0;
            if (!windows)
            {
                for (size_t at; (at = text.find(folder.path, from)) != std::string::npos;)
                {
                    size_t end = at + folder.path.size();
                    bool whole = (end == text.size() || !scrub::PathChar(text[end])) && (at == 0 || !scrub::PathChar(text[at - 1]));
                    out.append(text, from, at - from);
                    out += whole ? folder.replacement : folder.path;
                    from = end;
                }
            }
            else
            {
                const char first = char(std::tolower(uint8_t(folder.path[0])));
                for (size_t at = 0; at < text.size();)
                {
                    size_t length = 0;
                    if (char(std::tolower(uint8_t(text[at]))) == first && (at == 0 || !scrub::PathChar(text[at - 1])))
                        length = scrub::MatchAt(text, at, folder.path, true);
                    size_t end = at + length;
                    if (length == 0 || (end < text.size() && scrub::PathChar(text[end])))
                    {
                        at++;
                        continue;
                    }
                    out.append(text, from, at - from);
                    out += folder.replacement;
                    from = at = end;
                }
            }
            out.append(text, from, std::string::npos);
            text.swap(out);
        }
        return text;
    }
}
