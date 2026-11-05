// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Xenos shader microcode -> GLSL 4.60 (compiled to SPIR-V at runtime).
//
// Semantics follow Xenia's SPIR-V translator (BSD): Shader Model 3 rules for
// multiplication (+-0 * anything = +0), max/min NaN behaviour, predicates,
// kills, address/loop registers, vertex fetch formats and normalisation.
// Instruction encodings come from Xenia's ucode.h (gpu/xenos/).
//
// Resource model (shared by every shader):
//   set 0, binding 0  SharedMemory: guest physical memory as uint[] (words
//                     hold big-endian data; fetches swap per fetch constant)
//   set 0, binding 1  XenosConstants: float constants c[512], fetch constants,
//                     bool and loop constants, draw parameters
//   set 1, binding 0/1/2  sampler2D / sampler3D / samplerCube arrays [32],
//                     indexed by texture fetch constant slot
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace gpu
{
    enum class ShaderStage : uint32_t { Vertex = 0, Pixel = 1 };

    struct TranslatedShader
    {
        bool ok = false;
        std::string error;        // first translation error, if any
        std::string glsl;

        uint32_t registerCount = 0;
        uint32_t interpolatorMask = 0;   // VS: written; PS: read
        uint32_t colorTargetMask = 0;    // PS: oC# written
        bool writesDepth = false;
        bool usesKill = false;
        bool usesMemExport = false;
        uint32_t vertexFetchMask = 0;    // fetch constant slots used by vfetch (x3 dwords... slot = const index)
        uint32_t vertexFetchConstants[3] = {};  // the vertex fetch constants (0..95) vfetch reads
        // Per vertex fetch constant (0..95), for a vertex shader: the largest
        // dword stride its vfetches use, and the furthest dword past a
        // vertex's start they read (offset + 4); vertexIndexOther marks the
        // constants some vfetch of which doesn't index by the vertex index as
        // it comes in (r0.x, before anything writes r0), a negative offset, or
        // a pixel shader's. The others read only the draw's vertices' records:
        // the renderer copies or guards just those (HFR M3: the dynamic
        // buffers' other slots stay writable).
        uint8_t vertexStride[96] = {};
        uint8_t vertexExtent[96] = {};
        uint32_t vertexIndexOther[3] = {};
        uint32_t texture2DMask = 0;      // texture fetch slots used, by dimension
        uint32_t texture3DMask = 0;
        uint32_t textureCubeMask = 0;
        // Float constants the shader reads (by absolute index, 0..255 of its
        // stage). Packed: those reads use slots 0..N-1 in index order, so the
        // host uploads only the N used constants (not with relative reads).
        uint64_t constUsed[4] = {};
        bool constRelative = false;
        bool constPacked = false;
        uint32_t sharpFetches = 0;       // PS, TranslateOptions::sharpFetch2D: 2D fetches through sharpTex2D
    };

    struct TranslateOptions
    {
        // Settings > Graphics > Sharper UI's variant of a pixel shader
        // (gpu/sharp_ui.h): its 2D fetches through sharpTex2D. Off (the
        // default): the GLSL is exactly the plain translation's.
        bool sharpFetch2D = false;
    };

    // NFSMW_CP_OPT bit 8 (on by default): translate shaders with packed constants.
    bool PackedConstantsEnabled();

    // NFSMW_CONST_REL_RING (on by default): relative constant reads (constRel)
    // come from the ring's storage buffer rather than the uniform buffer.
    bool RelativeConstantsFromRing();

    // Registers as separate variables (r0, r1, ...) instead of the array r[]
    // in shaders without relative register addressing: bit 0 vertex, bit 1
    // pixel shaders. AMD's Windows driver (RX 6000) picks the wrong lighting
    // row in Most Wanted's far verge shader with the array (issue #67). The
    // renderer sets the default from the driver (pixel shaders, 2, on AMD's
    // proprietary driver: the half the fix needs; off elsewhere);
    // NFSMW_SCALAR_REGS=0..3 overrides it.
    void SetScalarRegistersDefault(uint32_t stageMask);
    uint32_t ScalarRegisters();

    // `ucode` is in host byte order (swap the guest's big-endian words first).
    TranslatedShader TranslateShader(ShaderStage stage, const uint32_t* ucode, size_t dwordCount);
    TranslatedShader TranslateShader(ShaderStage stage, const uint32_t* ucode, size_t dwordCount, const TranslateOptions& options);

    // The GLSL declarations every translated shader starts with (also used by
    // the host to lay out its buffers).
    const char* ShaderCommonGlsl();
}
