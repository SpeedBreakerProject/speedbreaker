// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The known-good discs: every file's disc-relative path, size and SHA-256,
// generated from a verified dump by scripts/gen_install_manifest.py, one per
// supported release (region): manifest.inc (USA) and manifest_pal_es.inc
// (Europe, the PAL disc with Spanish speech and movies). Each region's code is
// recompiled from that one default.xex, so its hash doubles as the
// supported-version check and says which region a disc or install is (the
// title ID is the same in both).
//
// A compiled-in manifest (GameManifests()) passed to the installer's functions
// stands for all of them: they take the region the files themselves say
// (default.xex's hash or size, the install marker). A test's own manifest is
// taken as it is.
#pragma once
#include <cstdint>
#include <span>
#include <string_view>

namespace install
{
    struct ManifestFile
    {
        std::string_view path;      // '/'-separated, in the dump's case (Movies/, NFS/, default.xex)
        uint64_t size;
        std::string_view sha256;    // lower-case hex
    };

    struct Manifest
    {
        std::string_view version;   // changes whenever the entries do; recorded in the install marker
        uint32_t titleId;           // from default.xex's execution info (0x454107D9)
        std::span<const ManifestFile> files;
        uint64_t totalBytes;
        std::string_view region = {};      // "usa", "pal-es": recorded in the install marker
        std::string_view regionName = {};  // for the player: "USA", "Europe (PAL, Spanish)"

        // The default.xex entry (the one file every manifest must have).
        const ManifestFile& Xex() const;
        // Case-insensitive; nullptr if absent.
        const ManifestFile* Find(std::string_view path) const;
    };

    // The USA manifest (manifest.inc), the default of every API that checks
    // files; they all take a Manifest so tests can pass a small one instead.
    const Manifest& GameManifest();

    // Every compiled-in manifest, USA first. Their default.xex hashes, sizes
    // and versions all differ (static_asserts in manifest.cpp).
    std::span<const Manifest* const> GameManifests();
    bool IsGameManifest(const Manifest& manifest);  // one of GameManifests()

    // The compiled-in manifest with that default.xex SHA-256 (lower-case hex),
    // default.xex size, or version; nullptr if none.
    const Manifest* ManifestForXexHash(std::string_view sha256);
    const Manifest* ManifestForXexSize(uint64_t size);
    const Manifest* ManifestForVersion(std::string_view version);

    // What the installer's functions use for `requested`: itself, unless it
    // is a compiled-in manifest and `other` (also compiled in, or nullptr)
    // says the files are another region's.
    inline const Manifest& Pick(const Manifest& requested, const Manifest* other)
    {
        return other && IsGameManifest(requested) ? *other : requested;
    }

    // ASCII case-insensitive equality: the disc's names are ASCII.
    bool PathEqualsIgnoreCase(std::string_view a, std::string_view b);
}
