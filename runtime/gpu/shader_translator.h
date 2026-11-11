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
        // PS: sun shadow PCF (Shadow Edges: Smooth): the 9-tap groups rewritten,
        // their taps and the compares that read them.
        uint32_t shadowPcfGroups = 0;
        uint32_t shadowPcfTaps = 0;
        uint32_t shadowPcfCompares = 0;
    };

    struct TranslateOptions
    {
        // Settings > Graphics > Sharper UI's variant of a pixel shader
        // (gpu/sharp_ui.h): its 2D fetches through sharpTex2D. Off (the
        // default): the GLSL is exactly the plain translation's.
        bool sharpFetch2D = false;
        // Sun shadow PCF: -1 or 1 on (the default), 0 off (the plain
        // translation: Shadow Edges' Classic), 2 on in draws the renderer
        // flags (the gated form of a same-run A/B harness).
        int shadowPcf = -1;
        // Its early out (ShadowPcfEarlyOut()): -1 as the environment says, 0
        // off, 1 on.
        int shadowPcfEarly = -1;
    };

    // Sun shadow PCF (GitHub #90): the default translation; Settings >
    // Graphics > Shadow Edges' Classic is the plain one (TranslateOptions::
    // shadowPcf 0), a variant the renderer switches to live.
    // Most Wanted's sun shadow receivers take 9 bilinear taps of the shadow
    // map (0 and +-0.5 texel) and compare each filtered DEPTH with the
    // pixel's: 9 binary tests, the caster's edge still the map's texel
    // staircase. A pixel shader with that pattern (9 2D taps of one slot
    // around one coordinate, each read by an sge against a reference) gets,
    // instead, percentage-closer filtering: the 3x3 texels the taps cover
    // (4 textureGathers instead of 9 samples) are each compared with the
    // reference, and each tap's result is those 0/1 values weighted as its
    // bilinear sample weighted the depths: the same taps, smooth edges.
    // Both of Most Wanted's maps are k_24_8 (D32 when lent, R32F once
    // resolved); the gathers read either. Shaders without the pattern
    // translate exactly as before.
    // NFSMW_SHADOW_PCF_EARLY (on by default; 0 off): a compare whose 3x3
    // texels all test the same (the block all lit or all in shadow: most of
    // the screen) gives every tap that 0 or 1, the game's own result there,
    // and skips the weights; only blocks across an edge are weighted.
    bool ShadowPcfEarlyOut();

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
