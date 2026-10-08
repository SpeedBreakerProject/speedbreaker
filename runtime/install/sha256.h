// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// SHA-256 (FIPS 180-4) for the installer: the manifest pins every disc file
// by SHA-256 so a dump can be checked against the hashes people already
// publish for this disc, and so the check doesn't depend on a
// non-cryptographic hash staying collision-free against damaged data.
// Streaming: Update() any number of times, then Finish().
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace install
{
    using Sha256Digest = std::array<uint8_t, 32>;

    class Sha256
    {
    public:
        Sha256() { Reset(); }

        void Reset();
        void Update(const void* data, size_t size);
        // Pads and returns the digest; Reset() before hashing anything else.
        Sha256Digest Finish();

    private:
        uint32_t state[8];
        uint64_t length;        // bytes hashed so far
        uint8_t buffer[64];     // a partial block
        size_t buffered;
    };

    Sha256Digest Sha256Of(const void* data, size_t size);

    // Lower-case hex, as sha256sum and hashlib print it.
    std::string ToHex(const Sha256Digest& digest);
    // Accepts upper or lower case; false unless it is exactly 64 hex digits.
    bool FromHex(std::string_view hex, Sha256Digest& digest);

    // Which block function this build uses ("armv8-sha2" or "portable"), for logs.
    const char* Sha256Implementation();
}
