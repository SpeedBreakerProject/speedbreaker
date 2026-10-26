// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See manifest.h.
#include "manifest.h"

#include <cassert>
#include <iterator>

namespace install
{
    namespace
    {
        // One namespace per region: its files, then its MANIFEST_INFO.
#define MANIFEST_INFO(version, titleId, count, bytes)
#define MANIFEST_FILE(path, size, sha256) ManifestFile{ path, size, sha256 },
        namespace usa
        {
            constexpr ManifestFile s_files[] = {
#include "manifest.inc"
            };
        }
        namespace pal_es
        {
            constexpr ManifestFile s_files[] = {
#include "manifest_pal_es.inc"
            };
        }
#undef MANIFEST_INFO
#undef MANIFEST_FILE

#define MANIFEST_INFO(version, titleId, count, bytes) \
        constexpr std::string_view s_version = version; \
        constexpr uint32_t s_titleId = titleId; \
        constexpr size_t s_count = count; \
        constexpr uint64_t s_bytes = bytes;
#define MANIFEST_FILE(path, size, sha256)
        namespace usa
        {
#include "manifest.inc"
        }
        namespace pal_es
        {
#include "manifest_pal_es.inc"
        }
#undef MANIFEST_INFO
#undef MANIFEST_FILE

        template <size_t N>
        constexpr uint64_t SumSizes(const ManifestFile (&files)[N])
        {
            uint64_t sum = 0;
            for (const ManifestFile& f : files)
                sum += f.size;
            return sum;
        }

        template <size_t N>
        constexpr const ManifestFile* XexOf(const ManifestFile (&files)[N])
        {
            for (const ManifestFile& f : files)
                if (f.path == "default.xex")
                    return &f;
            return nullptr;
        }

        // A hand-edited or truncated .inc fails the build, not an install.
        static_assert(std::size(usa::s_files) == usa::s_count, "manifest.inc: file count does not match MANIFEST_INFO");
        static_assert(SumSizes(usa::s_files) == usa::s_bytes, "manifest.inc: sizes do not add up to MANIFEST_INFO's total");
        static_assert(XexOf(usa::s_files), "manifest.inc: no default.xex entry");
        static_assert(std::size(pal_es::s_files) == pal_es::s_count, "manifest_pal_es.inc: file count does not match MANIFEST_INFO");
        static_assert(SumSizes(pal_es::s_files) == pal_es::s_bytes, "manifest_pal_es.inc: sizes do not add up to MANIFEST_INFO's total");
        static_assert(XexOf(pal_es::s_files), "manifest_pal_es.inc: no default.xex entry");
        // The regions are told apart by default.xex (hash, or size where no
        // hashing is done) and by the version an install's marker records.
        static_assert(XexOf(usa::s_files)->sha256 != XexOf(pal_es::s_files)->sha256, "two regions with one default.xex");
        static_assert(XexOf(usa::s_files)->size != XexOf(pal_es::s_files)->size, "two regions' default.xex of one size");
        static_assert(usa::s_version != pal_es::s_version, "two regions with one manifest version");

        constexpr Manifest s_usa{ usa::s_version, usa::s_titleId, usa::s_files, usa::s_bytes, "usa", "USA" };
        constexpr Manifest s_palEs{ pal_es::s_version, pal_es::s_titleId, pal_es::s_files, pal_es::s_bytes, "pal-es",
            "Europe (PAL, Spanish)" };
        constexpr const Manifest* s_manifests[] = { &s_usa, &s_palEs };

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
        return s_usa;
    }

    std::span<const Manifest* const> GameManifests()
    {
        return s_manifests;
    }

    bool IsGameManifest(const Manifest& manifest)
    {
        for (const Manifest* m : s_manifests)
            if (m == &manifest)
                return true;
        return false;
    }

    const Manifest* ManifestForXexHash(std::string_view sha256)
    {
        for (const Manifest* m : s_manifests)
            if (m->Xex().sha256 == sha256)
                return m;
        return nullptr;
    }

    const Manifest* ManifestForXexSize(uint64_t size)
    {
        for (const Manifest* m : s_manifests)
            if (m->Xex().size == size)
                return m;
        return nullptr;
    }

    const Manifest* ManifestForVersion(std::string_view version)
    {
        for (const Manifest* m : s_manifests)
            if (m->version == version)
                return m;
        return nullptr;
    }
}
