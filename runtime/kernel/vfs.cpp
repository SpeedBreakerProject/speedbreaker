// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
#include <stdafx.h>
#include "vfs.h"
#include "xam.h"

#include <user/paths.h>

namespace vfs
{
    namespace
    {
        bool IEquals(std::string_view a, std::string_view b)
        {
            if (a.size() != b.size())
                return false;
            for (size_t i = 0; i < a.size(); i++)
                if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
                    return false;
            return true;
        }

        bool IStartsWith(std::string_view s, std::string_view prefix)
        {
            return s.size() >= prefix.size() && IEquals(s.substr(0, prefix.size()), prefix);
        }

        // Finds `name` in `dir`, exact match first, then case-insensitively.
        std::optional<std::filesystem::path> FindChild(const std::filesystem::path& dir, std::string_view name)
        {
            std::error_code ec;
            std::filesystem::path exact = dir / std::string(name);
            if (std::filesystem::exists(exact, ec))
                return exact;
            for (auto& entry : std::filesystem::directory_iterator(dir, ec))
            {
                std::string n = entry.path().filename().string();
                if (IEquals(n, name))
                    return entry.path();
            }
            return std::nullopt;
        }

        std::filesystem::path UserDir(const char* name)
        {
            std::filesystem::path p = GetUserPath() / name;
            std::error_code ec;
            std::filesystem::create_directories(p, ec);
            return p;
        }

        // Splits off the device/root prefix; returns the host root and the rest.
        std::optional<std::pair<Resolved, std::string_view>> Root(std::string_view path)
        {
            if (IStartsWith(path, "\\??\\"))
                path.remove_prefix(4);

            struct Device { std::string_view prefix; int kind; };
            static constexpr Device devices[] = {
                { "\\Device\\Cdrom0\\", 0 }, { "\\Device\\Cdrom0", 0 },
                { "\\Device\\Harddisk0\\Cache0\\", 1 }, { "\\Device\\Harddisk0\\Cache1\\", 1 },
                { "\\Device\\Harddisk0\\Partition1\\", 2 },
            };
            for (const Device& d : devices)
            {
                if (IStartsWith(path, d.prefix))
                {
                    std::string_view rest = path.substr(d.prefix.size());
                    if (d.kind == 0) return std::pair{ Resolved{ GetGamePath(), true }, rest };
                    if (d.kind == 1) return std::pair{ Resolved{ UserDir("cache"), false }, rest };
                    return std::pair{ Resolved{ UserDir("hdd"), false }, rest };
                }
            }

            size_t colon = path.find(':');
            if (colon == std::string_view::npos || colon == 0)
                return std::nullopt;
            std::string_view root = path.substr(0, colon);
            std::string_view rest = path.substr(colon + 1);
            while (!rest.empty() && (rest.front() == '\\' || rest.front() == '/'))
                rest.remove_prefix(1);

            if (IEquals(root, "game") || IEquals(root, "d"))
                return std::pair{ Resolved{ GetGamePath(), true }, rest };
            if (IEquals(root, "cache"))
                return std::pair{ Resolved{ UserDir("cache"), false }, rest };
            if (IEquals(root, "hdd"))
                return std::pair{ Resolved{ UserDir("hdd"), false }, rest };

            std::string_view registered = XamGetRootPath(root);
            if (!registered.empty())
                return std::pair{ Resolved{ std::filesystem::path(std::string(registered)), false }, rest };
            return std::nullopt;
        }
    }

    std::optional<Resolved> Resolve(std::string_view guestPath, bool mustExist)
    {
        auto root = Root(guestPath);
        if (!root)
        {
            fprintf(stderr, "[vfs] unknown device in \"%.*s\"\n", int(guestPath.size()), guestPath.data());
            return std::nullopt;
        }
        auto [resolved, rest] = *root;

        std::vector<std::string_view> parts;
        size_t start = 0;
        while (start <= rest.size())
        {
            size_t end = rest.find_first_of("\\/", start);
            if (end == std::string_view::npos)
                end = rest.size();
            std::string_view part = rest.substr(start, end - start);
            if (!part.empty() && part != ".")
            {
                if (part == "..")
                {
                    if (!parts.empty())
                        parts.pop_back();
                }
                else
                {
                    parts.push_back(part);
                }
            }
            start = end + 1;
        }

        std::filesystem::path current = resolved.host;
        for (size_t i = 0; i < parts.size(); i++)
        {
            bool last = i + 1 == parts.size();
            auto child = FindChild(current, parts[i]);
            if (child)
            {
                current = *child;
            }
            else if (last && !mustExist)
            {
                current /= std::string(parts[i]);
            }
            else
            {
                return std::nullopt;
            }
        }
        resolved.host = current;
        return resolved;
    }

    bool WildcardMatch(std::string_view pattern, std::string_view name)
    {
        // Iterative matcher with single-star backtracking.
        size_t p = 0, n = 0, star = std::string_view::npos, mark = 0;
        while (n < name.size())
        {
            if (p < pattern.size() && (pattern[p] == '?' ||
                std::tolower((unsigned char)pattern[p]) == std::tolower((unsigned char)name[n])))
            {
                p++; n++;
            }
            else if (p < pattern.size() && pattern[p] == '*')
            {
                star = p++; mark = n;
            }
            else if (star != std::string_view::npos)
            {
                p = star + 1; n = ++mark;
            }
            else
            {
                return false;
            }
        }
        while (p < pattern.size() && pattern[p] == '*')
            p++;
        return p == pattern.size();
    }
}
