// SpeedBreaker: minimal stand-ins for the Xenia base utilities used by the
// Xenos headers ported from Xenia (BSD license, see headers in this folder).
#pragma once
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#define static_assert_size(type, size) static_assert(sizeof(type) == (size), "bad size for " #type)
#define assert_true(x) assert(x)
#define assert_false(x) assert(!(x))
#define assert_zero(x) assert((x) == 0)
#define assert_not_zero(x) assert((x) != 0)
#define assert_always(...) assert(false)
#define assert_unhandled_case(x) assert(false && "unhandled case")
#define XE_FORCEINLINE inline __attribute__((always_inline))

namespace xe
{
    template<typename T, size_t N>
    constexpr size_t countof(T (&)[N]) { return N; }

    inline uint16_t byte_swap(uint16_t v) { return __builtin_bswap16(v); }
    inline uint32_t byte_swap(uint32_t v) { return __builtin_bswap32(v); }
    inline uint64_t byte_swap(uint64_t v) { return __builtin_bswap64(v); }

    // Hosts are little-endian (x86-64, arm64).
    template<typename T>
    using le = T;

    typedef uint32_t fourcc_t;
    constexpr fourcc_t make_fourcc(const char (&s)[5])
    {
        return (uint32_t(s[0]) << 24) | (uint32_t(s[1]) << 16) | (uint32_t(s[2]) << 8) | uint32_t(s[3]);
    }

    namespace memory
    {
        template<typename T, typename F>
        T Reinterpret(F from) { T to; std::memcpy(&to, &from, sizeof(T)); return to; }
    }
}
