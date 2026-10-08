// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
// Stand-ins for Unleashed Recompiled's xxHash-keyed maps, without the
// xxHash / unordered_dense dependencies.
#pragma once
#include <cstdint>
#include <string_view>
#include <unordered_map>

// 64-bit FNV-1a.
inline uint64_t StringHash(std::string_view str)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (unsigned char c : str)
    {
        h ^= c;
        h *= 0x100000001B3ull;
    }
    return h;
}

template<typename T>
using xxHashMap = std::unordered_map<uint64_t, T>;
