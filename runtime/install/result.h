// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// What the install engine reports back: an error kind the UI can branch on
// (which screen or hint to show), plus a message that already says what went
// wrong and what to do, so a text-only frontend (the --install command) needs
// nothing else.
#pragma once
#include <cstdint>
#include <string>

namespace install
{
    enum class Error
    {
        None,
        NotFound,               // the path does not exist or cannot be opened
        NotADiscImage,          // neither an Xbox 360 disc image nor a folder holding default.xex
        UnsupportedFormat,      // an Xbox 360 package (Games on Demand/STFS) or a DVD-video-only image
        TruncatedImage,         // the image ends before its files do (incomplete copy, FAT32's 4 GB limit)
        CorruptImage,           // the image's directory tree is damaged
        WrongGame,              // default.xex belongs to another title
        UnsupportedVersion,     // Most Wanted, but not the default.xex this build was recompiled from
        MissingFile,            // a disc file the game needs is absent (bad dump)
        CorruptFile,            // a disc file has the wrong size or SHA-256 (bad dump)
        ReadError,              // I/O error reading the source
        DestinationNotWritable,
        DestinationNotEmpty,    // the destination holds files that are not a previous install
        NotEnoughSpace,
        WriteError,             // I/O error writing (disk filled up, drive removed)
        Cancelled,
    };

    struct Result
    {
        Error error = Error::None;
        std::string message;    // for the user: what happened and what to do about it
        std::string file;       // the file concerned, if any (disc-relative or host path)
        std::string foundHash;  // UnsupportedVersion/CorruptFile: the SHA-256 that was found

        bool Ok() const { return error == Error::None; }
    };

    // Stable identifier ("not_a_disc_image", ...) for logs and UI string tables.
    const char* ErrorName(Error error);

    // "7.12 GB", decimal units as file managers show them.
    std::string FormatSize(uint64_t bytes);
}
