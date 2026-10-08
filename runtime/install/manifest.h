// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The known-good disc: every file's disc-relative path, size and SHA-256,
// generated from a verified dump by scripts/gen_install_manifest.py into
// manifest.inc. The recompiled code only works with the one default.xex it was
// generated from, so that file's hash doubles as the supported-version check.
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

        // The default.xex entry (the one file every manifest must have).
        const ManifestFile& Xex() const;
        // Case-insensitive; nullptr if absent.
        const ManifestFile* Find(std::string_view path) const;
    };

    // The manifest compiled in from manifest.inc. Every API that checks files
    // takes a Manifest so tests can pass a small one instead.
    const Manifest& GameManifest();

    // ASCII case-insensitive equality: the disc's names are ASCII.
    bool PathEqualsIgnoreCase(std::string_view a, std::string_view b);
}
