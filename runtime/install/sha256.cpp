// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See sha256.h.
#include "sha256.h"

#include <algorithm>
#include <cstring>

// ARMv8's SHA-256 instructions hash about six times faster than the portable
// rounds (1.3 GB/s against 0.2 GB/s on an M1 Pro), which matters when the
// installer hashes 7 GB; every Apple Silicon Mac has them.
// NFSMW_SHA256_PORTABLE forces the portable path for testing.
#if defined(__aarch64__) && defined(__ARM_FEATURE_SHA2) && !defined(NFSMW_SHA256_PORTABLE)
#define NFSMW_SHA256_ARM 1
#include <arm_neon.h>
#endif

namespace install
{
    namespace
    {
        alignas(16) constexpr uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
        };

        constexpr uint32_t H0[8] = {
            0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
        };

#if NFSMW_SHA256_ARM
        // Four rounds per step: vsha256h/h2 update the two state halves, and
        // su0/su1 extend the message schedule four words at a time, so msg[i % 4]
        // always holds W[4i .. 4i+3] when step i uses it.
        void Compress(uint32_t state[8], const uint8_t* data, size_t blocks)
        {
            uint32x4_t abcd = vld1q_u32(&state[0]);
            uint32x4_t efgh = vld1q_u32(&state[4]);
            while (blocks--)
            {
                uint32x4_t msg[4];
                for (int i = 0; i < 4; i++)
                    msg[i] = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(data + 16 * i)));

                uint32x4_t a = abcd, e = efgh;
                for (int i = 0; i < 16; i++)
                {
                    uint32x4_t wk = vaddq_u32(msg[i & 3], vld1q_u32(&K[4 * i]));
                    if (i < 12)
                        msg[i & 3] = vsha256su1q_u32(vsha256su0q_u32(msg[i & 3], msg[(i + 1) & 3]), msg[(i + 2) & 3], msg[(i + 3) & 3]);
                    uint32x4_t previous = a;
                    a = vsha256hq_u32(a, e, wk);
                    e = vsha256h2q_u32(e, previous, wk);
                }
                abcd = vaddq_u32(abcd, a);
                efgh = vaddq_u32(efgh, e);
                data += 64;
            }
            vst1q_u32(&state[0], abcd);
            vst1q_u32(&state[4], efgh);
        }
#else
        inline uint32_t Rotr(uint32_t x, int n)
        {
            return (x >> n) | (x << (32 - n));
        }

        void Compress(uint32_t state[8], const uint8_t* data, size_t blocks)
        {
            while (blocks--)
            {
                uint32_t w[64];
                for (int i = 0; i < 16; i++)
                    w[i] = uint32_t(data[4 * i]) << 24 | uint32_t(data[4 * i + 1]) << 16 | uint32_t(data[4 * i + 2]) << 8 | data[4 * i + 3];
                for (int i = 16; i < 64; i++)
                {
                    uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
                    uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
                    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
                }

                uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
                uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
                for (int i = 0; i < 64; i++)
                {
                    uint32_t s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
                    uint32_t ch = (e & f) ^ (~e & g);
                    uint32_t t1 = h + s1 + ch + K[i] + w[i];
                    uint32_t s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
                    uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
                    uint32_t t2 = s0 + maj;
                    h = g; g = f; f = e; e = d + t1;
                    d = c; c = b; b = a; a = t1 + t2;
                }
                state[0] += a; state[1] += b; state[2] += c; state[3] += d;
                state[4] += e; state[5] += f; state[6] += g; state[7] += h;
                data += 64;
            }
        }
#endif
    }

    void Sha256::Reset()
    {
        memcpy(state, H0, sizeof(state));
        length = 0;
        buffered = 0;
    }

    void Sha256::Update(const void* data, size_t size)
    {
        auto p = static_cast<const uint8_t*>(data);
        length += size;
        if (buffered)
        {
            size_t take = std::min(size, sizeof(buffer) - buffered);
            memcpy(buffer + buffered, p, take);
            buffered += take;
            p += take;
            size -= take;
            if (buffered < sizeof(buffer))
                return;
            Compress(state, buffer, 1);
            buffered = 0;
        }
        if (size_t blocks = size / 64)
        {
            Compress(state, p, blocks);
            p += blocks * 64;
            size -= blocks * 64;
        }
        memcpy(buffer, p, size);
        buffered = size;
    }

    Sha256Digest Sha256::Finish()
    {
        // 0x80, zeros to 56 mod 64, then the message length in bits (big-endian).
        uint64_t bits = length * 8;
        uint8_t pad[72] = { 0x80 };
        size_t padSize = (buffered < 56 ? 56 : 120) - buffered;
        for (int i = 0; i < 8; i++)
            pad[padSize + i] = uint8_t(bits >> (56 - 8 * i));
        Update(pad, padSize + 8);

        Sha256Digest digest;
        for (int i = 0; i < 8; i++)
            for (int j = 0; j < 4; j++)
                digest[4 * i + j] = uint8_t(state[i] >> (24 - 8 * j));
        return digest;
    }

    Sha256Digest Sha256Of(const void* data, size_t size)
    {
        Sha256 sha;
        sha.Update(data, size);
        return sha.Finish();
    }

    std::string ToHex(const Sha256Digest& digest)
    {
        static constexpr char digits[] = "0123456789abcdef";
        std::string hex(64, '0');
        for (size_t i = 0; i < digest.size(); i++)
        {
            hex[2 * i] = digits[digest[i] >> 4];
            hex[2 * i + 1] = digits[digest[i] & 15];
        }
        return hex;
    }

    bool FromHex(std::string_view hex, Sha256Digest& digest)
    {
        if (hex.size() != 64)
            return false;
        auto nibble = [](char c) -> int
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        for (size_t i = 0; i < digest.size(); i++)
        {
            int hi = nibble(hex[2 * i]), lo = nibble(hex[2 * i + 1]);
            if (hi < 0 || lo < 0)
                return false;
            digest[i] = uint8_t(hi << 4 | lo);
        }
        return true;
    }

    const char* Sha256Implementation()
    {
#if NFSMW_SHA256_ARM
        return "armv8-sha2";
#else
        return "portable";
#endif
    }
}
