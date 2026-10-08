// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Guest memory in the shaders: each that reads or writes it declares
// SharedMemory { uint g_mem[]; } at set 0, binding 0, follows it with one of
// these, and goes through memLoad(word) / memStore(word, value) (word = byte
// address >> 2, below 2^27). Normally that is g_mem itself: the whole 512 MB
// in one binding. Where a device binds less of a storage buffer than that
// (maxStorageBufferRange: 128 MB on Turnip, the Steam Frame's Adreno 750),
// the renderer compiles the shaders with SPLIT_MEMORY and binds the memory
// as four 128 MB parts, binding 0 and 13-15 (renderer.cpp, kSharedPart).
#pragma once

#define GLSL_SHARED_MEMORY_READ R"(
#ifdef SPLIT_MEMORY
layout(std430, set = 0, binding = 13) readonly buffer SharedMemory1 { uint g_mem1[]; };
layout(std430, set = 0, binding = 14) readonly buffer SharedMemory2 { uint g_mem2[]; };
layout(std430, set = 0, binding = 15) readonly buffer SharedMemory3 { uint g_mem3[]; };
uint memLoad(uint w)
{
    uint i = w & 0x1FFFFFFu, part = w >> 25;
    if (part == 0u) return g_mem[i];
    if (part == 1u) return g_mem1[i];
    if (part == 2u) return g_mem2[i];
    return g_mem3[i];
}
#else
#define memLoad(w) g_mem[w]
#endif
)"

#define GLSL_SHARED_MEMORY_WRITE R"(
#ifdef SPLIT_MEMORY
layout(std430, set = 0, binding = 13) buffer SharedMemory1 { uint g_mem1[]; };
layout(std430, set = 0, binding = 14) buffer SharedMemory2 { uint g_mem2[]; };
layout(std430, set = 0, binding = 15) buffer SharedMemory3 { uint g_mem3[]; };
void memStore(uint w, uint v)
{
    uint i = w & 0x1FFFFFFu, part = w >> 25;
    if (part == 0u) g_mem[i] = v;
    else if (part == 1u) g_mem1[i] = v;
    else if (part == 2u) g_mem2[i] = v;
    else g_mem3[i] = v;
}
#else
#define memStore(w, v) g_mem[w] = (v)
#endif
)"
