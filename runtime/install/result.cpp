// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See result.h.
#include "result.h"

#include <cstdint>
#include <cstdio>

namespace install
{
    const char* ErrorName(Error error)
    {
        switch (error)
        {
        case Error::None: return "none";
        case Error::NotFound: return "not_found";
        case Error::NotADiscImage: return "not_a_disc_image";
        case Error::UnsupportedFormat: return "unsupported_format";
        case Error::TruncatedImage: return "truncated_image";
        case Error::CorruptImage: return "corrupt_image";
        case Error::WrongGame: return "wrong_game";
        case Error::UnsupportedVersion: return "unsupported_version";
        case Error::MissingFile: return "missing_file";
        case Error::CorruptFile: return "corrupt_file";
        case Error::ReadError: return "read_error";
        case Error::DestinationNotWritable: return "destination_not_writable";
        case Error::DestinationNotEmpty: return "destination_not_empty";
        case Error::NotEnoughSpace: return "not_enough_space";
        case Error::WriteError: return "write_error";
        case Error::Cancelled: return "cancelled";
        }
        return "unknown";
    }

    std::string FormatSize(uint64_t bytes)
    {
        // Decimal units, as file managers on both platforms show them.
        char text[32];
        if (bytes >= 1000000000ull)
            snprintf(text, sizeof(text), "%.2f GB", double(bytes) / 1e9);
        else if (bytes >= 1000000ull)
            snprintf(text, sizeof(text), "%.1f MB", double(bytes) / 1e6);
        else if (bytes >= 1000ull)
            snprintf(text, sizeof(text), "%.1f KB", double(bytes) / 1e3);
        else
            snprintf(text, sizeof(text), "%llu bytes", (unsigned long long)bytes);
        return text;
    }
}
