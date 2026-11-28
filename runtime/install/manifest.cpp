// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See manifest.h.
#include "manifest.h"
#include "sha256.h"

#include <cassert>
#include <fstream>
#include <iterator>
#include <vector>

// The German and UK discs' lists are always checked (static_asserts below),
// but each is a game manifest only in a build that links its code (SB_PAL_DE,
// SB_PAL_UK, runtime/CMakeLists.txt): without it, that disc is refused up
// front (ReleaseWithoutCode) rather than installed and then not run.
#ifndef SB_PAL_DE
#define SB_PAL_DE 0
#endif
#ifndef SB_PAL_UK
#define SB_PAL_UK 0
#endif

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
        namespace pal_de
        {
            constexpr ManifestFile s_files[] = {
#include "manifest_pal_de.inc"
            };
        }
        namespace pal_uk
        {
            constexpr ManifestFile s_files[] = {
#include "manifest_pal_uk.inc"
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
        namespace pal_de
        {
#include "manifest_pal_de.inc"
        }
        namespace pal_uk
        {
#include "manifest_pal_uk.inc"
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
        static_assert(std::size(pal_de::s_files) == pal_de::s_count, "manifest_pal_de.inc: file count does not match MANIFEST_INFO");
        static_assert(SumSizes(pal_de::s_files) == pal_de::s_bytes, "manifest_pal_de.inc: sizes do not add up to MANIFEST_INFO's total");
        static_assert(XexOf(pal_de::s_files), "manifest_pal_de.inc: no default.xex entry");
        static_assert(XexOf(pal_de::s_files)->sha256 != XexOf(usa::s_files)->sha256
            && XexOf(pal_de::s_files)->sha256 != XexOf(pal_es::s_files)->sha256, "two regions with one default.xex");
        static_assert(XexOf(pal_de::s_files)->size != XexOf(usa::s_files)->size
            && XexOf(pal_de::s_files)->size != XexOf(pal_es::s_files)->size, "two regions' default.xex of one size");
        static_assert(pal_de::s_version != usa::s_version && pal_de::s_version != pal_es::s_version,
            "two regions with one manifest version");
        // The UK disc's default.xex has the Spanish one's size (3,940,352
        // bytes): those two are told apart by hash (ManifestForXexFile).
        static_assert(std::size(pal_uk::s_files) == pal_uk::s_count, "manifest_pal_uk.inc: file count does not match MANIFEST_INFO");
        static_assert(SumSizes(pal_uk::s_files) == pal_uk::s_bytes, "manifest_pal_uk.inc: sizes do not add up to MANIFEST_INFO's total");
        static_assert(XexOf(pal_uk::s_files), "manifest_pal_uk.inc: no default.xex entry");
        static_assert(XexOf(pal_uk::s_files)->sha256 != XexOf(usa::s_files)->sha256
            && XexOf(pal_uk::s_files)->sha256 != XexOf(pal_es::s_files)->sha256
            && XexOf(pal_uk::s_files)->sha256 != XexOf(pal_de::s_files)->sha256, "two regions with one default.xex");
        static_assert(XexOf(pal_uk::s_files)->size != XexOf(usa::s_files)->size
            && XexOf(pal_uk::s_files)->size != XexOf(pal_de::s_files)->size, "a default.xex size only hashing tells apart");
        static_assert(pal_uk::s_version != usa::s_version && pal_uk::s_version != pal_es::s_version
            && pal_uk::s_version != pal_de::s_version, "two regions with one manifest version");

        constexpr Manifest s_usa{ usa::s_version, usa::s_titleId, usa::s_files, usa::s_bytes, "usa", "USA" };
        constexpr Manifest s_palEs{ pal_es::s_version, pal_es::s_titleId, pal_es::s_files, pal_es::s_bytes, "pal-es",
            "Europe (PAL, Spanish)" };
#if SB_PAL_DE
        constexpr Manifest s_palDe{ pal_de::s_version, pal_de::s_titleId, pal_de::s_files, pal_de::s_bytes, "pal-de",
            "Europe (PAL, German)" };
#endif
#if SB_PAL_UK
        constexpr Manifest s_palUk{ pal_uk::s_version, pal_uk::s_titleId, pal_uk::s_files, pal_uk::s_bytes, "pal-uk",
            "Europe (PAL, English)" };
#endif
        constexpr const Manifest* s_manifests[] = {
            &s_usa,
            &s_palEs,
#if SB_PAL_DE
            &s_palDe,
#endif
#if SB_PAL_UK
            &s_palUk,
#endif
        };

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

    std::string NeededDisc()
    {
        std::string names;
        for (size_t i = 0; i < std::size(s_manifests); i++)
        {
            std::string name(s_manifests[i]->regionName);
            if (name.starts_with("Europe "))
                name = "European " + name.substr(7);
            names += (i == 0 ? "the " : i + 1 == std::size(s_manifests) ? " or the " : ", the ") + name;
        }
        return "SpeedBreaker needs a copy of the Xbox 360 disc of Need for Speed: Most Wanted, " + names + " one.";
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

    const Manifest* ManifestForXexFile(const std::filesystem::path& xex)
    {
        std::error_code ec;
        const uint64_t size = std::filesystem::file_size(xex, ec);
        if (ec)
            return nullptr;
        size_t sameSize = 0;
        for (const Manifest* m : s_manifests)
            sameSize += m->Xex().size == size;
        if (sameSize <= 1)
            return ManifestForXexSize(size);
        // Two releases with this size (the Spanish and UK discs): the hash
        // says which. default.xex is ~4 MB.
        std::ifstream in(xex, std::ios::binary);
        std::vector<char> buffer(1 << 16);
        Sha256 sha;
        while (in.read(buffer.data(), std::streamsize(buffer.size())) || in.gcount() > 0)
            sha.Update(buffer.data(), size_t(in.gcount()));
        if (in.bad())
            return nullptr;
        // Neither one's hash (a damaged or hand-made copy): the first of the
        // size, as before the two shared it; the install check finds the rest.
        const Manifest* byHash = ManifestForXexHash(ToHex(sha.Finish()));
        return byHash && byHash->Xex().size == size ? byHash : ManifestForXexSize(size);
    }

    const Manifest* ManifestForVersion(std::string_view version)
    {
        for (const Manifest* m : s_manifests)
            if (m->version == version)
                return m;
        return nullptr;
    }

    ReleaseWithoutCodeInfo ReleaseWithoutCode(std::string_view sha256)
    {
#if !SB_PAL_DE
        if (sha256 == XexOf(pal_de::s_files)->sha256)
            return { "Europe (PAL, German)", "German (PAL)", "ppc-pal-de" };
#endif
#if !SB_PAL_UK
        if (sha256 == XexOf(pal_uk::s_files)->sha256)
            return { "Europe (PAL, English)", "UK (PAL, English)", "ppc-pal-uk" };
#endif
        (void)sha256;
        return {};
    }
}
