// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See manifest.h.
#include "manifest.h"

#include <cassert>
#include <iterator>

namespace install
{
    namespace
    {
#define MANIFEST_INFO(version, titleId, count, bytes)
#define MANIFEST_FILE(path, size, sha256) ManifestFile{ path, size, sha256 },
        constexpr ManifestFile s_files[] = {
#include "manifest.inc"
        };
#undef MANIFEST_INFO
#undef MANIFEST_FILE

#define MANIFEST_INFO(version, titleId, count, bytes) \
        constexpr std::string_view s_version = version; \
        constexpr uint32_t s_titleId = titleId; \
        constexpr size_t s_count = count; \
        constexpr uint64_t s_bytes = bytes;
#define MANIFEST_FILE(path, size, sha256)
#include "manifest.inc"
#undef MANIFEST_INFO
#undef MANIFEST_FILE

        constexpr uint64_t SumSizes()
        {
            uint64_t sum = 0;
            for (const ManifestFile& f : s_files)
                sum += f.size;
            return sum;
        }

        constexpr bool HasXex()
        {
            for (const ManifestFile& f : s_files)
                if (f.path == "default.xex")
                    return true;
            return false;
        }

        // A hand-edited or truncated manifest.inc fails the build, not an install.
        static_assert(std::size(s_files) == s_count, "manifest.inc: file count does not match MANIFEST_INFO");
        static_assert(SumSizes() == s_bytes, "manifest.inc: sizes do not add up to MANIFEST_INFO's total");
        static_assert(HasXex(), "manifest.inc: no default.xex entry");

        char Lower(char c)
        {
            return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
        }
    }

    bool PathEqualsIgnoreCase(std::string_view a, std::string_view b)
    {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); i++)
            if (Lower(a[i]) != Lower(b[i]))
                return false;
        return true;
    }

    const ManifestFile& Manifest::Xex() const
    {
        const ManifestFile* xex = Find("default.xex");
        assert(xex && "a manifest must list default.xex");
        return *xex;
    }

    const ManifestFile* Manifest::Find(std::string_view path) const
    {
        for (const ManifestFile& f : files)
            if (PathEqualsIgnoreCase(f.path, path))
                return &f;
        return nullptr;
    }

    const Manifest& GameManifest()
    {
        static const Manifest manifest{ s_version, s_titleId, s_files, s_bytes };
        return manifest;
    }
}
