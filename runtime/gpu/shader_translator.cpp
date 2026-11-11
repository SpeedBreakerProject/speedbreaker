// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Xenos microcode -> GLSL. Instruction semantics follow Xenia's
// spirv_shader_translator{,_alu,_fetch}.cc and shader_translator.cc
// (BSD license); comments name the rule where it isn't obvious.
#include "shader_translator.h"
#include "shared_memory_glsl.h"
#include "sharp_ui.h"

#include "xenos/ucode.h"

#include <atomic>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <array>
#include <format>
#include <map>
#include <set>

// gpuSwap(v, endian): a guest word in the given Xenos endianness (0 none,
// 1 8in16, 2 8in32, 3 16in32) as a host uint. On Windows as selects: Intel's
// Windows Vulkan driver (UHD 620, 101.2135) compiled the early-return chain
// below wrongly in vertex shaders, returning the swapped word OR'd with the
// unswapped one for 8in32 (1.0f read back as 1.0039f, 0x3F80803F). Vertex
// positions and colours came out wrong: the front end's layers at z = 1 lay
// beyond the far plane and were clipped, so the title screen and every menu
// were black while movies played. The same function (for every endian value,
// 4 and up left as they are, as the chain does); the other platforms keep
// the chain for now (the same SPIR-V and caches as before).
//
// cubeDirection(c) below, a run of early returns on the cube face, is written
// as selects on Windows for the same reason, though it hasn't shown yet.
#ifdef _WIN32
#define GLSL_GPU_SWAP R"(uint gpuSwap(uint v, uint endian)
{
    uint s8 = ((v << 8) & 0xFF00FF00u) | ((v >> 8) & 0x00FF00FFu);
    v = (endian == 1u || endian == 2u) ? s8 : v;
    return (endian == 2u || endian == 3u) ? (v >> 16) | (v << 16) : v;
}
)"
#define GLSL_CUBE_FACE_DIRECTION R"(    vec3 d = vec3(-sc * 2.0, -tc * 2.0, -1.0);
    d = face == 4 ? vec3(sc * 2.0, -tc * 2.0, 1.0) : d;
    d = face == 3 ? vec3(sc * 2.0, -1.0, -tc * 2.0) : d;
    d = face == 2 ? vec3(sc * 2.0, 1.0, tc * 2.0) : d;
    d = face == 1 ? vec3(-1.0, -tc * 2.0, sc * 2.0) : d;
    return face == 0 ? vec3(1.0, -tc * 2.0, -sc * 2.0) : d;)"
#else
#define GLSL_CUBE_FACE_DIRECTION R"(    if (face == 0) return vec3(1.0, -tc * 2.0, -sc * 2.0);
    if (face == 1) return vec3(-1.0, -tc * 2.0, sc * 2.0);
    if (face == 2) return vec3(sc * 2.0, 1.0, tc * 2.0);
    if (face == 3) return vec3(sc * 2.0, -1.0, -tc * 2.0);
    if (face == 4) return vec3(sc * 2.0, -tc * 2.0, 1.0);
    return vec3(-sc * 2.0, -tc * 2.0, -1.0);)"
#define GLSL_GPU_SWAP R"(uint gpuSwap(uint v, uint endian)
{
    if (endian == 1u) return ((v << 8) & 0xFF00FF00u) | ((v >> 8) & 0x00FF00FFu);
    if (endian == 2u) return (v << 24) | ((v << 8) & 0x00FF0000u) | ((v >> 8) & 0x0000FF00u) | (v >> 24);
    if (endian == 3u) return (v >> 16) | (v << 16);
    return v;
}
)"
#endif

namespace gpu
{
    using namespace xe::gpu;
    using namespace xe::gpu::ucode;

    const char* ShaderCommonGlsl()
    {
        return R"(#version 460
#extension GL_EXT_samplerless_texture_functions : require

layout(std430, set = 0, binding = 0) readonly buffer SharedMemory { uint g_mem[]; };)" GLSL_SHARED_MEMORY_READ R"(
// The renderer's ring: small vertex ranges copied when the draw was recorded
// (the guest may reuse their memory before the GPU runs it), see vmem().
layout(std430, set = 0, binding = 2) readonly buffer RingMemory { uint g_ring[]; };

layout(std140, set = 0, binding = 1) uniform XenosConstants
{
    vec4 c[512];          // float constants (VS and PS share the file)
    uvec4 fetch[48];      // 32 fetch constants x 6 dwords
    uvec4 bools[2];       // 256 bool constants
    uvec4 loops[8];       // 32 loop constants
    uvec4 drawInfo[4];    // see gpu/renderer.cpp
} u;

layout(push_constant) uniform DrawConstants
{
    uint vsConstBase;     // in vec4s
    uint psConstBase;
    uint indexAddress;    // physical, 0 = not indexed
    uint indexInfo;       // bit 0: 32-bit indices; bits 1-2: endian; bit 3: primitive expansion
    uint indexOffset;     // VGT_INDX_OFFSET
    uint alphaTest;       // bits 0-2: func (7 = always), rest unused
    float alphaRef;
    uint flags;           // bit 0: apply posScale; bits 1-3: vertex W/XY/Z formats
    vec4 posScale;        // NDC = pos.xy * posScale.xy + posScale.zw * pos.w
    uvec4 vfRedirect[4];  // guest start, size, ring byte offset: vertex data snapshots
    uint constRing;       // ring byte offset of this draw's XenosConstants, see constRel()
} pc;

// Textures: set 1, one binding per fetch slot the shader uses (declared after
// translation): sampler2D t2d_N at binding N, sampler3D t3d_N at 32 + N,
// samplerCube tcube_N at 64 + N.

uint fetchDword(uint slot, uint dword)
{
    uint i = slot * 6u + dword;
    return u.fetch[i >> 2][i & 3u];
}

// A 2D texture's guest size (fetch word 2: width - 1, height - 1).
vec2 fetchSize2D(uint slot)
{
    uint d = fetchDword(slot, 2u);
    return vec2(float((d & 0x1FFFu) + 1u), float(((d >> 13) & 0x1FFFu) + 1u));
}

// A texture fetch constant's LOD bias (word 4 bits 12:21, 1/32 units).
float fetchLodBias(uint slot)
{
    return float(bitfieldExtract(int(fetchDword(slot, 4u)), 12, 10)) / 32.0;
}

)" GLSL_GPU_SWAP R"(
// Guest memory words are big-endian; the buffer holds them as host uints.
uint memWord(uint byteAddress)
{
    uint v = memLoad((byteAddress & 0x1FFFFFFFu) >> 2);
    return (v << 24) | ((v << 8) & 0x00FF0000u) | ((v >> 8) & 0x0000FF00u) | (v >> 24);
}

// A vertex data word: from a snapshot in the ring when the draw has one for it.
uint vmem(uint byteAddress)
{
    uint a = byteAddress & 0x1FFFFFFFu;
    for (int r = 0; r < 4; r++)
    {
        uint d = a - pc.vfRedirect[r].x;
        if (d < pc.vfRedirect[r].y)
            return g_ring[(pc.vfRedirect[r].z + d) >> 2];
    }
    return memLoad(a >> 2);
}

bool boolConst(uint i) { return ((u.bools[i >> 7][(i >> 5) & 3u] >> (i & 31u)) & 1u) != 0u; }

// Shader Model 3 multiplication: +-0 (or denormal) * anything = +0.
// No `precise` anywhere: MoltenVK (SPIRV-Cross) implements it with
// optnone helper calls per operation, making shaders ~20x slower. Positions
// stay identical across passes through `invariant gl_Position` instead.
vec4 xmul(vec4 a, vec4 b)
{
    vec4 r = a * b;
    bvec4 zero = notEqual(vec4(equal(abs(a), vec4(0.0))) + vec4(equal(abs(b), vec4(0.0))), vec4(0.0));
    return mix(r, vec4(0.0), zero);
}
float xmul(float a, float b)
{
    float r = a * b;
    return (abs(a) == 0.0 || abs(b) == 0.0) ? 0.0 : r;
}
)";
    }

    namespace
    {
        const char* kComp = "xyzw";

        struct Translator
        {
            ShaderStage stage;
            const uint32_t* ucode;
            size_t dwords;
            TranslateOptions options;
            TranslatedShader out;
            std::string body;
            int indent = 1;
            bool usesRelativeRegisters = false;
            uint32_t maxRegister = 0;

            // Previous vfetch_full, for vfetch_mini.
            VertexFetchInstruction lastFullFetch{};
            // r0 (a vertex shader's incoming vertex index) written so far.
            bool r0Written = false;

            // The instruction being emitted (its ucode address), for the
            // shadow PCF plan below.
            uint32_t curAddr = 0;

            // Sun shadow PCF (GitHub #90, gpu/shader_translator.h): the 9-tap
            // receivers' taps and the compares that read them, found before
            // emission (PlanShadowPcf).
            struct PcfGroup
            {
                uint32_t slot = 0;
                uint32_t firstAddr = 0;   // the group's first tap in program order: the gathers go there
            };
            struct PcfTap
            {
                uint32_t group = 0;
                int ox = 0, oy = 0;       // the tap's offset in half texels (-1, 0, 1)
                bool reconstruct = true;  // something else may read the tap's register: rebuild the filtered depth
            };
            std::vector<PcfGroup> pcfGroups;
            int pcfMode = 0;  // 1: PCF; 2: PCF only in draws the renderer flags (NFSMW_SHADOW_AB), else the game's samples
            bool pcfEarly = false;  // ShadowPcfEarlyOut(): uniform blocks skip the weights
            std::map<uint32_t, PcfTap> pcfTaps;                // by instruction address
            std::map<uint32_t, std::array<int, 4>> pcfCompares; // by instruction address: per component, the tap's address (-1: none)

            void Error(const std::string& message)
            {
                if (out.error.empty())
                    out.error = message;
            }

            void Line(const std::string& s)
            {
                body.append(size_t(indent) * 4, ' ');
                body += s;
                body += '\n';
            }

            void NoteRegister(uint32_t r)
            {
                maxRegister = std::max(maxRegister, r);
            }

            std::string Reg(uint32_t index, bool loopRelative)
            {
                NoteRegister(index);
                if (!loopRelative)
                    return std::format("r[{}]", index);
                usesRelativeRegisters = true;
                return std::format("r[clamp({} + aL, 0, R_COUNT - 1)]", index);
            }

            std::string ConstBase() const
            {
                return stage == ShaderStage::Vertex ? "pc.vsConstBase" : "pc.psConstBase";
            }

            // Float constant with Xenos out-of-range = 0 behaviour for
            // dynamically addressed reads.
            std::string Const(uint32_t index, int addressing /*0 abs, 1 a0, 2 aL*/)
            {
                if (addressing == 0 && index < 256)
                {
                    // Resolved to a packed slot (or kept) once all reads are known.
                    out.constUsed[index >> 6] |= 1ull << (index & 63);
                    return std::format("u.c[{} + @CK{}@]", ConstBase(), index);
                }
                if (addressing == 0)
                    return std::format("u.c[{} + {}u]", ConstBase(), index);
                out.constRelative = true;
                std::string rel = addressing == 1 ? "a0" : "aL";
                return std::format("constRel({}, {})", index, rel);
            }

            // ALU source operand i (1..3) as a vec4 before swizzling.
            std::string AluSourceStorage(const AluInstruction& op, uint32_t i, bool& isAbs)
            {
                uint32_t reg = op.src_reg(i);
                if (op.src_is_temp(i))
                {
                    isAbs = AluInstruction::is_src_temp_value_absolute(reg);
                    return Reg(AluInstruction::src_temp_reg(reg), AluInstruction::is_src_temp_relative(reg));
                }
                isAbs = op.abs_constants();
                int addressing = 0;
                if (op.src_const_is_addressed(i))
                    addressing = op.is_const_address_register_relative() ? 1 : 2;
                return Const(reg, addressing);
            }

            std::string ApplyModifiers(std::string v, bool isAbs, bool negate)
            {
                if (isAbs)
                    v = "abs(" + v + ")";
                if (negate)
                    v = "-(" + v + ")";
                return v;
            }

            // Full vec4 ALU operand (swizzled, modifiers applied).
            std::string AluVector(const AluInstruction& op, uint32_t i)
            {
                bool isAbs;
                std::string s = AluSourceStorage(op, i, isAbs);
                uint32_t sw = op.src_swizzle(i);
                std::string swz;
                for (uint32_t c = 0; c < 4; c++)
                    swz += kComp[AluInstruction::GetSwizzledComponentIndex(sw, c)];
                if (swz != "xyzw")
                    s += "." + swz;
                return ApplyModifiers(s, isAbs, op.src_negate(i));
            }

            // Scalar operand component: 'a' is the W slot, 'b' the X slot.
            std::string AluScalarComponent(const AluInstruction& op, uint32_t i, uint32_t slot)
            {
                bool isAbs;
                std::string s = AluSourceStorage(op, i, isAbs);
                s += std::string(".") + kComp[AluInstruction::GetSwizzledComponentIndex(op.src_swizzle(i), slot)];
                return ApplyModifiers(s, isAbs, op.src_negate(i));
            }

            // Operands of the scalar constant+temporary ops (mulsc, addsc, subsc).
            void ScalarConstTemp(const AluInstruction& op, std::string& a, std::string& b)
            {
                uint32_t sw = op.src_swizzle(3);
                int addressing = 0;
                if (op.src_const_is_addressed(3))
                    addressing = op.is_const_address_register_relative() ? 1 : 2;
                a = Const(op.src_reg(3), addressing) + "." + kComp[AluInstruction::GetSwizzledComponentIndex(sw, 3)];
                b = Reg(op.scalar_const_reg_op_src_temp_reg(), false) + "." +
                    kComp[AluInstruction::GetSwizzledComponentIndex(sw, 0)];
                if (op.abs_constants())
                {
                    a = "abs(" + a + ")";
                    b = "abs(" + b + ")";
                }
                if (op.src_negate(3))
                {
                    a = "-(" + a + ")";
                    b = "-(" + b + ")";
                }
            }

            std::string VectorOp(const AluInstruction& op)
            {
                auto s = [&](uint32_t i) { return AluVector(op, i); };
                switch (op.vector_opcode())
                {
                case AluVectorOpcode::kAdd: return "(" + s(1) + " + " + s(2) + ")";
                case AluVectorOpcode::kMul: return "xmul(" + s(1) + ", " + s(2) + ")";
                case AluVectorOpcode::kMad: return "(xmul(" + s(1) + ", " + s(2) + ") + " + s(3) + ")";
                case AluVectorOpcode::kMax: return "vmax(" + s(1) + ", " + s(2) + ")";
                case AluVectorOpcode::kMin: return "vmin(" + s(1) + ", " + s(2) + ")";
                case AluVectorOpcode::kMaxA:
                    return "maxa(" + s(1) + ", " + s(2) + ")";
                case AluVectorOpcode::kSeq: return "vec4(equal(" + s(1) + ", " + s(2) + "))";
                case AluVectorOpcode::kSgt: return "vec4(greaterThan(" + s(1) + ", " + s(2) + "))";
                case AluVectorOpcode::kSge: return "vec4(greaterThanEqual(" + s(1) + ", " + s(2) + "))";
                case AluVectorOpcode::kSne: return "vsne(" + s(1) + ", " + s(2) + ")";
                case AluVectorOpcode::kFrc: return "fract(" + s(1) + ")";
                case AluVectorOpcode::kTrunc: return "trunc(" + s(1) + ")";
                case AluVectorOpcode::kFloor: return "floor(" + s(1) + ")";
                case AluVectorOpcode::kCndEq: return "mix(" + s(3) + ", " + s(2) + ", vec4(equal(" + s(1) + ", vec4(0.0))))";
                case AluVectorOpcode::kCndGe: return "mix(" + s(3) + ", " + s(2) + ", vec4(greaterThanEqual(" + s(1) + ", vec4(0.0))))";
                case AluVectorOpcode::kCndGt: return "mix(" + s(3) + ", " + s(2) + ", vec4(greaterThan(" + s(1) + ", vec4(0.0))))";
                case AluVectorOpcode::kDp4: return "vec4(dp4(" + s(1) + ", " + s(2) + "))";
                case AluVectorOpcode::kDp3: return "vec4(dp3(" + s(1) + ", " + s(2) + "))";
                case AluVectorOpcode::kDp2Add: return "vec4(dp2add(" + s(1) + ", " + s(2) + ", " + s(3) + "))";
                case AluVectorOpcode::kCube: return "cubeop(" + s(1) + ")";
                case AluVectorOpcode::kMax4: return "vec4(max4(" + s(1) + "))";
                case AluVectorOpcode::kSetpEqPush: return "setpPush(" + s(1) + ", " + s(2) + ", 0)";
                case AluVectorOpcode::kSetpNePush: return "setpPush(" + s(1) + ", " + s(2) + ", 1)";
                case AluVectorOpcode::kSetpGtPush: return "setpPush(" + s(1) + ", " + s(2) + ", 2)";
                case AluVectorOpcode::kSetpGePush: return "setpPush(" + s(1) + ", " + s(2) + ", 3)";
                case AluVectorOpcode::kKillEq: out.usesKill = true; return "killv(any(equal(" + s(1) + ", " + s(2) + ")))";
                case AluVectorOpcode::kKillGt: out.usesKill = true; return "killv(any(greaterThan(" + s(1) + ", " + s(2) + ")))";
                case AluVectorOpcode::kKillGe: out.usesKill = true; return "killv(any(greaterThanEqual(" + s(1) + ", " + s(2) + ")))";
                case AluVectorOpcode::kKillNe: out.usesKill = true; return "killv(any(vsne(" + s(1) + ", " + s(2) + ") != vec4(0.0)))";
                case AluVectorOpcode::kDst: return "dstop(" + s(1) + ", " + s(2) + ")";
                }
                Error(std::format("unknown vector opcode {}", uint32_t(op.vector_opcode())));
                return "vec4(0.0)";
            }

            std::string ScalarOp(const AluInstruction& op)
            {
                auto a = [&]() { return AluScalarComponent(op, 3, 3); };
                auto b = [&]() { return AluScalarComponent(op, 3, 0); };
                std::string ca, cb;
                switch (op.scalar_opcode())
                {
                case AluScalarOpcode::kAdds: return "(" + a() + " + " + b() + ")";
                case AluScalarOpcode::kSubs: return "(" + a() + " - " + b() + ")";
                case AluScalarOpcode::kAddsPrev: return "(" + a() + " + ps)";
                case AluScalarOpcode::kSubsPrev: return "(" + a() + " - ps)";
                case AluScalarOpcode::kMuls: return "xmul(" + a() + ", " + b() + ")";
                case AluScalarOpcode::kMulsPrev: return "xmul(" + a() + ", ps)";
                case AluScalarOpcode::kMulsPrev2: return "mulsprev2(" + a() + ", " + b() + ")";
                case AluScalarOpcode::kMaxs: return "smax(" + a() + ", " + b() + ")";
                case AluScalarOpcode::kMins: return "smin(" + a() + ", " + b() + ")";
                case AluScalarOpcode::kMaxAs: return "maxas(" + a() + ", " + b() + ", true)";
                case AluScalarOpcode::kMaxAsf: return "maxas(" + a() + ", " + b() + ", false)";
                case AluScalarOpcode::kSeqs: return "((" + a() + " == 0.0) ? 1.0 : 0.0)";
                case AluScalarOpcode::kSgts: return "((" + a() + " > 0.0) ? 1.0 : 0.0)";
                case AluScalarOpcode::kSges: return "((" + a() + " >= 0.0) ? 1.0 : 0.0)";
                case AluScalarOpcode::kSnes: return "(!(" + a() + " == 0.0) ? 1.0 : 0.0)";
                case AluScalarOpcode::kFrcs: return "fract(" + a() + ")";
                case AluScalarOpcode::kTruncs: return "trunc(" + a() + ")";
                case AluScalarOpcode::kFloors: return "floor(" + a() + ")";
                case AluScalarOpcode::kExp: return "exp2(" + a() + ")";
                case AluScalarOpcode::kLog: return "log2(" + a() + ")";
                case AluScalarOpcode::kLogc: return "clampNegInf(log2(" + a() + "))";
                case AluScalarOpcode::kRcp: return "(1.0 / " + a() + ")";
                case AluScalarOpcode::kRcpc: return "clampInf(1.0 / " + a() + ")";
                case AluScalarOpcode::kRcpf: return "flushInf(1.0 / " + a() + ")";
                case AluScalarOpcode::kRsq: return "inversesqrt(" + a() + ")";
                case AluScalarOpcode::kRsqc: return "clampInf(inversesqrt(" + a() + "))";
                case AluScalarOpcode::kRsqf: return "flushInf(inversesqrt(" + a() + "))";
                case AluScalarOpcode::kSqrt: return "sqrt(" + a() + ")";
                case AluScalarOpcode::kSin: return "sin(" + a() + ")";
                case AluScalarOpcode::kCos: return "cos(" + a() + ")";
                case AluScalarOpcode::kSetpEq: return "setp(" + a() + " == 0.0)";
                case AluScalarOpcode::kSetpNe: return "setp(!(" + a() + " == 0.0))";
                case AluScalarOpcode::kSetpGt: return "setp(" + a() + " > 0.0)";
                case AluScalarOpcode::kSetpGe: return "setp(" + a() + " >= 0.0)";
                case AluScalarOpcode::kSetpInv: return "setpInv(" + a() + ")";
                case AluScalarOpcode::kSetpPop: return "setpPop(" + a() + ")";
                case AluScalarOpcode::kSetpClr: return "setpClr()";
                case AluScalarOpcode::kSetpRstr: return "setpRstr(" + a() + ")";
                case AluScalarOpcode::kKillsEq: out.usesKill = true; return "kills(" + a() + " == 0.0)";
                case AluScalarOpcode::kKillsGt: out.usesKill = true; return "kills(" + a() + " > 0.0)";
                case AluScalarOpcode::kKillsGe: out.usesKill = true; return "kills(" + a() + " >= 0.0)";
                case AluScalarOpcode::kKillsNe: out.usesKill = true; return "kills(!(" + a() + " == 0.0))";
                case AluScalarOpcode::kKillsOne: out.usesKill = true; return "kills(" + a() + " == 1.0)";
                case AluScalarOpcode::kMulsc0:
                case AluScalarOpcode::kMulsc1:
                    ScalarConstTemp(op, ca, cb);
                    return "xmul(" + ca + ", " + cb + ")";
                case AluScalarOpcode::kAddsc0:
                case AluScalarOpcode::kAddsc1:
                    ScalarConstTemp(op, ca, cb);
                    return "(" + ca + " + " + cb + ")";
                case AluScalarOpcode::kSubsc0:
                case AluScalarOpcode::kSubsc1:
                    ScalarConstTemp(op, ca, cb);
                    return "(" + ca + " - " + cb + ")";
                case AluScalarOpcode::kRetainPrev: return "ps";
                }
                Error(std::format("unknown scalar opcode {}", uint32_t(op.scalar_opcode())));
                return "0.0";
            }

            // Where a result goes.
            std::string ExportTarget(uint32_t dest)
            {
                if (stage == ShaderStage::Vertex)
                {
                    if (dest < 16)
                    {
                        out.interpolatorMask |= 1u << dest;
                        return std::format("o_interpT[{}]", dest);
                    }
                    if (dest == 62)
                        return "o_position";
                    if (dest == 63)
                        return "o_pointSize";
                }
                else
                {
                    if (dest < 4)
                    {
                        out.colorTargetMask |= 1u << dest;
                        return std::format("o_color[{}]", dest);
                    }
                    if (dest == 61)
                    {
                        out.writesDepth = true;
                        return "o_depth";
                    }
                }
                if (dest >= 32 && dest <= 37)
                {
                    out.usesMemExport = true;
                    return std::format("o_memexport[{}]", dest - 32);
                }
                Error(std::format("unsupported export register {}", dest));
                return "o_unused";
            }

            void Alu(const AluInstruction& op)
            {
                uint32_t vecMask = op.GetVectorOpResultWriteMask();
                uint32_t scaMask = op.GetScalarOpResultWriteMask();
                uint32_t zeroMask = op.GetConstant0WriteMask();
                uint32_t oneMask = op.GetConstant1WriteMask();
                const auto& vinfo = GetAluVectorOpcodeInfo(op.vector_opcode());
                const auto& sinfo = GetAluScalarOpcodeInfo(op.scalar_opcode());
                bool vecNeeded = vecMask || vinfo.changed_state;
                bool scaNeeded = scaMask || sinfo.changed_state ||
                    op.scalar_opcode() != AluScalarOpcode::kRetainPrev;
                if (!vecNeeded && !scaNeeded && !zeroMask && !oneMask)
                    return;

                if (op.is_predicated())
                {
                    Line(std::format("if (p0 == {}) {{", op.predicate_condition() ? "true" : "false"));
                    indent++;
                }
                // Both operations read the registers as they were before the
                // instruction: compute into temporaries, then store.
                Line("{");
                indent++;
                if (vecNeeded)
                    Line("vec4 vres = " + VectorOp(op) + ";");
                // Sun shadow PCF: a planned compare. The 3x3 texels of each
                // group it reads are tested against the reference once; each
                // component then weights those 0/1 values as its tap's
                // bilinear sample weights the depths: 0..1 instead of 0 or 1.
                if (auto cmp = pcfCompares.find(curAddr); cmp != pcfCompares.end())
                {
                    out.shadowPcfCompares++;
                    Line("vec4 sref = " + AluVector(op, 2) + ";");
                    // One reference for every component it writes (r.xxxx in every receiver): one test of the block.
                    uint32_t refComp = ~0u;
                    bool oneRef = true;
                    for (uint32_t c = 0; c < 4; c++)
                        if (cmp->second[c] >= 0)
                        {
                            uint32_t rc = AluInstruction::GetSwizzledComponentIndex(op.src_swizzle(2), c);
                            oneRef = oneRef && (refComp == ~0u || refComp == rc);
                            refComp = rc;
                        }
                    std::set<uint32_t> groups;
                    for (uint32_t c = 0; c < 4; c++)
                        if (cmp->second[c] >= 0)
                            groups.insert(pcfTaps.at(uint32_t(cmp->second[c])).group);
                    for (uint32_t g : groups)
                    {
                        Line(pcfMode == 2 ? std::format("if (spcfOn{}) {{", g) : std::string("{"));
                        indent++;
                        // The lane to read the reference from: the first one written (all equal with oneRef).
                        uint32_t lane = 0;
                        while (lane < 3 && cmp->second[lane] < 0)
                            lane++;
                        if (oneRef)
                            Line(std::format("vec3 sc0 = step(vec3(sref.{0}), spcfR{1}0), sc1 = step(vec3(sref.{0}), spcfR{1}1), sc2 = step(vec3(sref.{0}), spcfR{1}2);",
                                kComp[lane], g));
                        // The early out: all 9 tests the same, every tap is
                        // that 0 or 1 (each tap's weights sum to 1), as the
                        // game's own test gives there; the weights only where
                        // the block straddles an edge.
                        bool early = oneRef && pcfEarly;
                        if (early)
                        {
                            Line("float sn = dot(sc0 + sc1 + sc2, vec3(1.0));");
                            Line("if (sn == 0.0 || sn == 9.0)");
                            Line("{");
                            indent++;
                            Line("float sv = min(sn, 1.0);");
                            for (uint32_t c = 0; c < 4; c++)
                                if (cmp->second[c] >= 0 && pcfTaps.at(uint32_t(cmp->second[c])).group == g)
                                    Line(std::format("vres.{} = sv;", kComp[c]));
                            indent--;
                            Line("}");
                            Line("else");
                            Line("{");
                            indent++;
                        }
                        for (uint32_t c = 0; c < 4; c++)
                        {
                            if (cmp->second[c] < 0)
                                continue;
                            const PcfTap& tap = pcfTaps.at(uint32_t(cmp->second[c]));
                            if (tap.group != g)
                                continue;
                            std::string p = std::format("spcfA{} + vec2({}, {})", g, 0.5f * float(tap.ox), 0.5f * float(tap.oy));
                            if (oneRef)
                                Line(std::format("vres.{} = spcfW(sc0, sc1, sc2, {});", kComp[c], p));
                            else
                                Line(std::format("vres.{0} = spcfW(step(vec3(sref.{0}), spcfR{1}0), step(vec3(sref.{0}), spcfR{1}1), step(vec3(sref.{0}), spcfR{1}2), {2});",
                                    kComp[c], g, p));
                        }
                        if (early)
                        {
                            indent--;
                            Line("}");
                        }
                        indent--;
                        Line("}");
                    }
                }
                if (scaNeeded)
                {
                    Line("float sres = " + ScalarOp(op) + ";");
                    Line("ps = sres;");
                }

                auto clamp = [](std::string v, bool c) { return c ? "clamp(" + v + ", 0.0, 1.0)" : v; };
                if (op.is_export())
                {
                    std::string t = ExportTarget(op.vector_dest());
                    for (uint32_t c = 0; c < 4; c++)
                    {
                        std::string comp = std::string(".") + kComp[c];
                        if (vecMask & (1u << c))
                            Line(t + comp + " = " + clamp("vres" + comp, op.vector_clamp()) + ";");
                        else if (scaMask & (1u << c))
                            Line(t + comp + " = " + clamp(scaNeeded ? "sres" : "ps", op.scalar_clamp()) + ";");
                        else if (zeroMask & (1u << c))
                            Line(t + comp + " = 0.0;");
                        else if (oneMask & (1u << c))
                            Line(t + comp + " = 1.0;");
                    }
                }
                else
                {
                    if ((vecMask && (op.vector_dest() == 0 || op.is_vector_dest_relative())) ||
                        (scaMask && (op.scalar_dest() == 0 || op.is_scalar_dest_relative())))
                        r0Written = true;
                    if (vecMask)
                    {
                        std::string dst = Reg(op.vector_dest(), op.is_vector_dest_relative());
                        for (uint32_t c = 0; c < 4; c++)
                            if (vecMask & (1u << c))
                                Line(dst + "." + kComp[c] + " = " + clamp(std::string("vres.") + kComp[c], op.vector_clamp()) + ";");
                    }
                    if (scaMask)
                    {
                        std::string dst = Reg(op.scalar_dest(), op.is_scalar_dest_relative());
                        for (uint32_t c = 0; c < 4; c++)
                            if (scaMask & (1u << c))
                                Line(dst + "." + kComp[c] + " = " + clamp(scaNeeded ? "sres" : "ps", op.scalar_clamp()) + ";");
                    }
                }
                indent--;
                Line("}");
                if (op.is_predicated())
                {
                    indent--;
                    Line("}");
                }
            }

            // Destination swizzle for fetches: 3 bits per component.
            void StoreFetchResult(uint32_t dest, bool destRelative, uint32_t swizzle, const std::string& value)
            {
                if (dest == 0 || destRelative)
                    r0Written = true;
                std::string dst = Reg(dest, destRelative);
                for (uint32_t c = 0; c < 4; c++)
                {
                    uint32_t s = (swizzle >> (3 * c)) & 7;
                    if (s == 7)
                        continue;  // keep
                    std::string v = s < 4 ? value + "." + kComp[s] : (s == 4 ? "0.0" : "1.0");
                    Line(dst + "." + kComp[c] + " = " + v + ";");
                }
            }

            void VertexFetch(const VertexFetchInstruction& op)
            {
                const VertexFetchInstruction& full = op.is_mini_fetch() ? lastFullFetch : op;
                if (!op.is_mini_fetch())
                    lastFullFetch = op;
                uint32_t slot = full.fetch_constant_index();  // 0..95 (3 per 6-dword slot)
                out.vertexFetchMask |= 1u << (slot / 3);
                out.vertexFetchConstants[slot / 32] |= 1u << (slot % 32);
                // The vertices this fetch reads (see vertexStride).
                if (stage != ShaderStage::Vertex || full.src() != 0 || full.is_src_relative() || (full.src_swizzle() & 3) != 0 || r0Written ||
                    op.offset() < 0 || op.offset() > 251)
                    out.vertexIndexOther[slot / 32] |= 1u << (slot % 32);
                else
                {
                    out.vertexStride[slot] = uint8_t(std::max<uint32_t>(out.vertexStride[slot], full.stride()));
                    out.vertexExtent[slot] = uint8_t(std::max<int32_t>(out.vertexExtent[slot], op.offset() + 4));
                }

                if (op.is_predicated())
                {
                    Line(std::format("if (p0 == {}) {{", op.predicate_condition() ? "true" : "false"));
                    indent++;
                }
                Line("{");
                indent++;
                std::string index = Reg(full.src(), full.is_src_relative()) + "." + kComp[full.src_swizzle() & 3];
                Line(std::format("float vindex = {};", index));
                // Index to integer: floor, or round-to-nearest as floor(x + 0.5).
                Line(full.is_index_rounded() ? "int ivindex = int(floor(vindex + 0.5));" : "int ivindex = int(floor(vindex));");
                // Fetch constant: dword 0 bits 2-31 = address >> 2; dword 1 bits 0-1 = endian.
                uint32_t fetchSlot = slot / 3, sub = slot % 3;
                Line(std::format("uint vf0 = fetchDword({}u, {}u), vf1 = fetchDword({}u, {}u);",
                    fetchSlot, sub * 2, fetchSlot, sub * 2 + 1));
                Line(std::format("uint vbase = (vf0 & 0xFFFFFFFCu) + uint(ivindex * {} + ({})) * 4u;",
                    int(full.stride()), int(op.offset())));
                Line("uint vendian = vf1 & 3u;");

                xenos::VertexFormat format = op.data_format();
                bool isSigned = op.is_signed();
                bool normalized = op.is_normalized();
                auto word = [&](int i) { return std::format("gpuSwap(vmem(vbase + {}u), vendian)", i * 4); };
                std::string value;
                // Packed integer formats: widths/offsets per component.
                auto packed = [&](std::initializer_list<std::pair<int, int>> comps, int words) {
                    std::vector<std::pair<int, int>> v(comps);
                    Line("uint w0 = " + word(0) + ";");
                    if (words > 1)
                        Line("uint w1 = " + word(1) + ";");
                    std::string parts[4];
                    for (size_t i = 0; i < v.size(); i++)
                    {
                        auto [offset, width] = v[i];
                        int w = offset / 32;
                        int o = offset % 32;
                        std::string src = std::format("w{}", w);
                        std::string ext = isSigned
                            ? std::format("float(bitfieldExtract(int({}), {}, {}))", src, o, width)
                            : std::format("float(bitfieldExtract({}, {}, {}))", src, o, width);
                        if (normalized)
                        {
                            float scaleInv = isSigned ? float((1u << (width - 1)) - 1) : float((1u << width) - 1);
                            if (isSigned && op.signed_rf_mode() == xenos::SignedRepeatingFractionMode::kNoZero)
                                ext = std::format("(({} + 0.5) / {})", ext, scaleInv + 0.5f);
                            else if (isSigned)
                                ext = std::format("max({} / {}, -1.0)", ext, scaleInv);
                            else
                                ext = std::format("({} / {})", ext, scaleInv);
                        }
                        parts[i] = ext;
                    }
                    std::string vec = "vec4(";
                    for (int i = 0; i < 4; i++)
                        vec += (i < int(v.size()) ? parts[i] : std::string(i == 3 ? "1.0" : "0.0")) + (i < 3 ? ", " : ")");
                    return vec;
                };
                switch (format)
                {
                case xenos::VertexFormat::k_8_8_8_8: value = packed({ { 0, 8 }, { 8, 8 }, { 16, 8 }, { 24, 8 } }, 1); break;
                case xenos::VertexFormat::k_2_10_10_10: value = packed({ { 0, 10 }, { 10, 10 }, { 20, 10 }, { 30, 2 } }, 1); break;
                case xenos::VertexFormat::k_10_11_11: value = packed({ { 0, 11 }, { 11, 11 }, { 22, 10 } }, 1); break;
                case xenos::VertexFormat::k_11_11_10: value = packed({ { 0, 10 }, { 10, 11 }, { 21, 11 } }, 1); break;
                case xenos::VertexFormat::k_16_16: value = packed({ { 0, 16 }, { 16, 16 } }, 1); break;
                case xenos::VertexFormat::k_16_16_16_16: value = packed({ { 0, 16 }, { 16, 16 }, { 32, 16 }, { 48, 16 } }, 2); break;
                case xenos::VertexFormat::k_16_16_FLOAT:
                    Line("uint w0 = " + word(0) + ";");
                    value = "vec4(unpackHalf2x16(w0), 0.0, 1.0)";
                    break;
                case xenos::VertexFormat::k_16_16_16_16_FLOAT:
                    Line("uint w0 = " + word(0) + ", w1 = " + word(1) + ";");
                    value = "vec4(unpackHalf2x16(w0), unpackHalf2x16(w1))";
                    break;
                case xenos::VertexFormat::k_32:
                case xenos::VertexFormat::k_32_32:
                case xenos::VertexFormat::k_32_32_32_32:
                {
                    int n = format == xenos::VertexFormat::k_32 ? 1 : format == xenos::VertexFormat::k_32_32 ? 2 : 4;
                    std::string parts[4] = { "0.0", "0.0", "0.0", "1.0" };
                    for (int i = 0; i < n; i++)
                    {
                        std::string raw = word(i);
                        std::string f = isSigned ? "float(int(" + raw + "))" : "float(" + raw + ")";
                        if (normalized)
                            f = isSigned ? "max(" + f + " / 2147483647.0, -1.0)" : "(" + f + " / 4294967295.0)";
                        parts[i] = f;
                    }
                    value = "vec4(" + parts[0] + ", " + parts[1] + ", " + parts[2] + ", " + parts[3] + ")";
                    break;
                }
                case xenos::VertexFormat::k_32_FLOAT:
                    value = "vec4(uintBitsToFloat(" + word(0) + "), 0.0, 0.0, 1.0)";
                    break;
                case xenos::VertexFormat::k_32_32_FLOAT:
                    value = "vec4(uintBitsToFloat(" + word(0) + "), uintBitsToFloat(" + word(1) + "), 0.0, 1.0)";
                    break;
                case xenos::VertexFormat::k_32_32_32_FLOAT:
                    value = "vec4(uintBitsToFloat(" + word(0) + "), uintBitsToFloat(" + word(1) + "), uintBitsToFloat(" + word(2) + "), 1.0)";
                    break;
                case xenos::VertexFormat::k_32_32_32_32_FLOAT:
                    value = "vec4(uintBitsToFloat(" + word(0) + "), uintBitsToFloat(" + word(1) + "), uintBitsToFloat(" + word(2) +
                        "), uintBitsToFloat(" + word(3) + "))";
                    break;
                default:
                    Error(std::format("unsupported vertex format {}", uint32_t(format)));
                    value = "vec4(0.0)";
                }
                if (op.exp_adjust())
                    value = std::format("({} * {})", value, std::ldexp(1.0f, op.exp_adjust()));
                Line("vec4 vdata = " + value + ";");
                StoreFetchResult(op.dest(), op.is_dest_relative(), op.dest_swizzle(), "vdata");
                indent--;
                Line("}");
                if (op.is_predicated())
                {
                    indent--;
                    Line("}");
                }
            }

            void TextureFetch(const TextureFetchInstruction& op)
            {
                if (op.opcode() == FetchOpcode::kSetTextureLod)
                {
                    Line("texLod = " + Reg(op.src(), op.is_src_relative()) + "." + kComp[op.src_swizzle() & 3] + ";");
                    return;
                }
                if (op.opcode() != FetchOpcode::kTextureFetch)
                {
                    Error(std::format("texture fetch opcode {} not implemented yet", uint32_t(op.opcode())));
                    return;
                }
                uint32_t slot = op.fetch_constant_index();
                if (op.is_predicated())
                {
                    Line(std::format("if (p0 == {}) {{", op.predicate_condition() ? "true" : "false"));
                    indent++;
                }
                Line("{");
                indent++;
                std::string src = Reg(op.src(), op.is_src_relative());
                uint32_t sw = op.src_swizzle();
                auto coord = [&](int i) { return src + "." + kComp[(sw >> (2 * i)) & 3]; };
                // The LOD sources, as Xenia sums them: the fetch constant's
                // bias, the register LOD (setTexLOD: HLSL tex2Dbias/tex2Dlod)
                // and the instruction's bias. With a computed LOD (pixel
                // shaders) they bias the implicit LOD; otherwise they are the
                // LOD. (Using the register value as an absolute LOD pinned
                // ~70% of NFSMW's world draws to one mip level.)
                bool computedLod = op.use_computed_lod() && stage == ShaderStage::Pixel;
                std::string lod = std::format("fetchLodBias({}u)", slot);
                if (op.use_register_lod())
                    lod = "texLod + " + lod;
                if (op.lod_bias() != 0.0f)
                    lod += std::format(" + {}", op.lod_bias());
                auto sample = [&](const std::string& texture, const std::string& coords) {
                    return computedLod ? std::format("texture({}, {}, {})", texture, coords, lod)
                                       : std::format("textureLod({}, {}, {})", texture, coords, lod);
                };
                std::string value;
                switch (op.dimension())
                {
                case xenos::FetchOpDimension::k1D:
                case xenos::FetchOpDimension::k2D:
                {
                    out.texture2DMask |= 1u << slot;
                    std::string uv = op.dimension() == xenos::FetchOpDimension::k1D
                        ? "vec2(" + coord(0) + ", 0.5)" : "vec2(" + coord(0) + ", " + coord(1) + ")";
                    // Texel coordinates count guest texels: divide by the
                    // guest size (the fetch constant's), not the image's,
                    // which is larger at a scaled internal resolution. 1D
                    // textures keep the image size (a different field).
                    if (op.unnormalized_coordinates())
                        uv = op.dimension() == xenos::FetchOpDimension::k2D
                            ? std::format("({}) / fetchSize2D({}u)", uv, slot)
                            : std::format("({}) / vec2(textureSize(t2d_{}, 0))", uv, slot);
                    if (op.offset_x() != 0.0f || op.offset_y() != 0.0f)
                        uv = std::format("({} + vec2({}, {}) / vec2(textureSize(t2d_{}, 0)))", uv, op.offset_x(), op.offset_y(), slot);
                    // Sharper UI's variant (gpu/sharp_ui.h): a pixel
                    // shader's 2D fetch through sharpTex2D, which keeps
                    // this very fetch where it doesn't magnify.
                    if (options.sharpFetch2D && stage == ShaderStage::Pixel && op.dimension() == xenos::FetchOpDimension::k2D)
                    {
                        out.sharpFetches++;
                        value = std::format("sharpTex2D(t2d_{}, {}, {}u, {}, {})", slot, uv, slot, computedLod ? "true" : "false", lod);
                    }
                    else
                        value = sample(std::format("t2d_{}", slot), uv);
                    // Sun shadow PCF (PlanShadowPcf): a planned tap. Its
                    // group's first tap gathers the 3x3 texels all 9 taps
                    // cover (4 gathers); every tap then gets its filtered
                    // depth from them, or nothing when only its compare reads
                    // it. Under NFSMW_SHADOW_AB (pcfMode 2) only in draws the
                    // renderer flags and where the slot is a depth map, the
                    // game's own sample otherwise.
                    if (auto tap = pcfTaps.find(curAddr); tap != pcfTaps.end())
                    {
                        uint32_t g = tap->second.group;
                        if (pcfGroups[g].firstAddr == curAddr)
                        {
                            if (pcfMode == 2)
                            {
                                Line(std::format("spcfOn{} = shadowPcfOn({}u);", g, slot));
                                Line(std::format("if (spcfOn{}) {{", g));
                            }
                            else
                                Line("{");
                            indent++;
                            Line(std::format("vec2 sz = vec2(textureSize(t2d_{}, 0));", slot));
                            Line(std::format("vec2 s = vec2({}, {}) * sz;", coord(0), coord(1)));
                            Line("vec2 b = floor(s) - 1.0;");
                            Line(std::format("spcfA{} = s - b - 0.5;", g));
                            Line(std::format("vec4 g00 = textureGather(t2d_{}, (b + vec2(1.0, 1.0)) / sz);", slot));
                            Line(std::format("vec4 g10 = textureGather(t2d_{}, (b + vec2(3.0, 1.0)) / sz);", slot));
                            Line(std::format("vec4 g01 = textureGather(t2d_{}, (b + vec2(1.0, 3.0)) / sz);", slot));
                            Line(std::format("float g11 = textureGather(t2d_{}, (b + vec2(3.0, 3.0)) / sz).w;", slot));
                            Line(std::format("spcfR{}0 = vec3(g00.w, g00.z, g10.w);", g));
                            Line(std::format("spcfR{}1 = vec3(g00.x, g00.y, g10.x);", g));
                            Line(std::format("spcfR{}2 = vec3(g01.w, g01.z, g11);", g));
                            indent--;
                            Line("}");
                        }
                        out.shadowPcfTaps++;
                        std::string pcf = tap->second.reconstruct
                            ? std::format("vec4(spcfW(spcfR{0}0, spcfR{0}1, spcfR{0}2, spcfA{0} + vec2({1}, {2})))", g,
                                  0.5f * float(tap->second.ox), 0.5f * float(tap->second.oy))
                            : std::string("vec4(0.0)");
                        if (pcfMode == 2)
                        {
                            Line("vec4 tdata;");
                            Line(std::format("if (spcfOn{}) tdata = {};", g, pcf));
                            Line("else tdata = " + value + ";");
                        }
                        else
                            Line(std::format("vec4 tdata = {};{}", pcf, tap->second.reconstruct ? "" : "  // read only by its compare"));
                        StoreFetchResult(op.dest(), op.is_dest_relative(), op.dest_swizzle(), "tdata");
                        indent--;
                        Line("}");
                        if (op.is_predicated())
                        {
                            indent--;
                            Line("}");
                        }
                        return;
                    }
                    break;
                }
                case xenos::FetchOpDimension::k3DOrStacked:
                {
                    out.texture3DMask |= 1u << slot;
                    std::string uvw = "vec3(" + coord(0) + ", " + coord(1) + ", " + coord(2) + ")";
                    value = sample(std::format("t3d_{}", slot), uvw);
                    break;
                }
                case xenos::FetchOpDimension::kCube:
                {
                    out.textureCubeMask |= 1u << slot;
                    // Coordinates come from the cube instruction: (sc, tc, face)
                    // with sc/tc in [1, 2] after the usual /|2ma| + 1.5.
                    std::string dir = "cubeDirection(vec3(" + coord(0) + ", " + coord(1) + ", " + coord(2) + "))";
                    value = sample(std::format("tcube_{}", slot), dir);
                    break;
                }
                }
                Line("vec4 tdata = " + value + ";");
                StoreFetchResult(op.dest(), op.is_dest_relative(), op.dest_swizzle(), "tdata");
                indent--;
                Line("}");
                if (op.is_predicated())
                {
                    indent--;
                    Line("}");
                }
            }

            template<typename T>
            void Exec(const T& exec)
            {
                uint32_t seq = exec.sequence();
                for (uint32_t k = 0; k < exec.count(); k++, seq >>= 2)
                {
                    const uint32_t* op = ucode + size_t(exec.address() + k) * 3;
                    if (size_t(exec.address() + k) * 3 + 3 > dwords)
                    {
                        Error("instruction address outside the shader");
                        return;
                    }
                    curAddr = exec.address() + k;
                    if (seq & 1)
                    {
                        auto& f = *reinterpret_cast<const FetchInstruction*>(op);
                        if (f.opcode() == FetchOpcode::kVertexFetch)
                            VertexFetch(f.vertex_fetch());
                        else
                            TextureFetch(f.texture_fetch());
                    }
                    else
                    {
                        Alu(*reinterpret_cast<const AluInstruction*>(op));
                    }
                }
            }

            // Sun shadow PCF (GitHub #90). Most Wanted's sun shadow receivers
            // take 9 bilinear taps of the shadow map, at 0 and +-0.5 texel
            // around one coordinate, and compare each FILTERED depth with
            // z/w + bias (sge): 9 binary results, so a caster's edge is still
            // the map's texel staircase. Found here, before emission:
            //  - a group of 9 such taps: 2D fetches of one slot from one
            //    source register and swizzle (not written until the last
            //    tap), the 9 offsets once each, each into one register
            //    component from .x, under one condition;
            //  - the sge instructions whose first operand, on every
            //    component they write, is one of those taps (no modifiers),
            //    under the same condition; every tap must reach one before
            //    anything overwrites it.
            // From the group's first tap to its last compare the program must
            // run straight: exec clauses only, none ending the program before
            // the last, no jump, call or loop into or out of them, no
            // predicate change. A tap's register gets the filtered depth
            // rebuilt from the gathered texels unless the plan proves only its
            // compare reads it. Anything else is translated as before.
            struct PcfInstr
            {
                uint32_t addr = 0;
                uint32_t cf = 0;
                uint64_t guard = 0;  // the exec clause's condition and the instruction's predicate
                bool fetch = false;
            };

            void PlanShadowPcf(const std::vector<ControlFlowInstruction>& cf)
            {
                // Control flow: jump/call/loop targets (an entry from elsewhere)
                // and the clauses a planned range may cross.
                std::vector<bool> target(cf.size() + 1, false);
                auto mark = [&](size_t i) { if (i < target.size()) target[i] = true; };
                for (size_t i = 0; i < cf.size(); i++)
                {
                    const auto& c = cf[i];
                    switch (c.opcode())
                    {
                    case ControlFlowOpcode::kCondJmp: mark(c.cond_jmp.address()); break;
                    case ControlFlowOpcode::kCondCall: mark(c.cond_call.address()); mark(i + 1); break;
                    case ControlFlowOpcode::kLoopStart: mark(c.loop_start.address()); mark(i + 1); break;
                    case ControlFlowOpcode::kLoopEnd: mark(c.loop_end.address()); mark(i + 1); break;
                    default: break;
                    }
                }
                auto passable = [&](size_t i, bool last) {
                    switch (cf[i].opcode())
                    {
                    case ControlFlowOpcode::kNop:
                    case ControlFlowOpcode::kAlloc:
                    case ControlFlowOpcode::kExec:
                    case ControlFlowOpcode::kCondExec:
                    case ControlFlowOpcode::kCondExecPred:
                    case ControlFlowOpcode::kCondExecPredClean:
                        return true;
                    case ControlFlowOpcode::kExecEnd:
                    case ControlFlowOpcode::kCondExecEnd:
                    case ControlFlowOpcode::kCondExecPredEnd:
                    case ControlFlowOpcode::kCondExecPredCleanEnd:
                        return last;
                    default:
                        return false;
                    }
                };

                // Every instruction in program order.
                std::vector<PcfInstr> list;
                for (size_t i = 0; i < cf.size(); i++)
                {
                    const auto& c = cf[i];
                    if (!IsControlFlowOpcodeExec(c.opcode()))
                        continue;
                    uint64_t guard = 0;
                    uint32_t address = c.exec.address(), count = c.exec.count(), seq = c.exec.sequence();
                    switch (c.opcode())
                    {
                    case ControlFlowOpcode::kCondExec:
                    case ControlFlowOpcode::kCondExecEnd:
                    case ControlFlowOpcode::kCondExecPredClean:
                    case ControlFlowOpcode::kCondExecPredCleanEnd:
                        guard = 1 | (uint64_t(c.cond_exec.bool_address()) << 4) | (uint64_t(c.cond_exec.condition()) << 12);
                        break;
                    case ControlFlowOpcode::kCondExecPred:
                    case ControlFlowOpcode::kCondExecPredEnd:
                        guard = 2 | (uint64_t(c.cond_exec_pred.condition()) << 12);
                        break;
                    default:
                        break;
                    }
                    for (uint32_t k = 0; k < count; k++, seq >>= 2)
                    {
                        if (size_t(address + k) * 3 + 3 > dwords)
                            return;
                        PcfInstr in;
                        in.addr = address + k;
                        in.cf = uint32_t(i);
                        in.fetch = (seq & 1) != 0;
                        const uint32_t* w = ucode + size_t(in.addr) * 3;
                        bool pred = in.fetch ? reinterpret_cast<const FetchInstruction*>(w)->is_predicated()
                                             : reinterpret_cast<const AluInstruction*>(w)->is_predicated();
                        bool cond = in.fetch ? reinterpret_cast<const FetchInstruction*>(w)->predicate_condition()
                                             : reinterpret_cast<const AluInstruction*>(w)->predicate_condition();
                        in.guard = guard | (pred ? (1ull << 16) | (uint64_t(cond) << 17) : 0);
                        list.push_back(in);
                    }
                }

                // A tap candidate: (slot, source register, swizzle) and its
                // offset and destination component.
                struct Tap
                {
                    size_t idx;
                    uint32_t slot, src, swizzle, dreg, dcomp;
                    int ox, oy;
                };
                auto asTap = [&](size_t idx, Tap& t) {
                    const PcfInstr& in = list[idx];
                    if (!in.fetch)
                        return false;
                    const auto& f = *reinterpret_cast<const FetchInstruction*>(ucode + size_t(in.addr) * 3);
                    if (f.opcode() != FetchOpcode::kTextureFetch)
                        return false;
                    const auto op = f.texture_fetch();
                    if (op.dimension() != xenos::FetchOpDimension::k2D || op.unnormalized_coordinates() || op.is_dest_relative() ||
                        op.is_src_relative() || !op.use_computed_lod() || op.use_register_lod())
                        return false;
                    float ox = op.offset_x() * 2.0f, oy = op.offset_y() * 2.0f;
                    if ((ox != -1.0f && ox != 0.0f && ox != 1.0f) || (oy != -1.0f && oy != 0.0f && oy != 1.0f))
                        return false;
                    int comp = -1;
                    for (uint32_t c = 0; c < 4; c++)
                    {
                        uint32_t s = (op.dest_swizzle() >> (3 * c)) & 7;
                        if (s == 7)
                            continue;
                        if (s != 0 || comp >= 0)
                            return false;
                        comp = int(c);
                    }
                    if (comp < 0 || op.dest() == op.src())
                        return false;
                    t = { idx, op.fetch_constant_index(), op.src(), op.src_swizzle(), op.dest(), uint32_t(comp), int(ox), int(oy) };
                    return true;
                };
                // The registers an instruction may read (bit per register; all
                // ones with relative addressing) and the components it writes.
                struct Access
                {
                    std::vector<std::pair<uint32_t, uint32_t>> reads;  // (register, component mask)
                    bool readsAll = false;
                    std::vector<std::pair<uint32_t, uint32_t>> writes;  // (register, component mask)
                    bool writesAll = false;
                    bool predicate = false;  // changes p0
                };
                // skipSrc1: an sge's first operand, read by the compare itself.
                auto accessOf = [&](size_t idx, bool skipSrc1 = false) {
                    Access a;
                    const PcfInstr& in = list[idx];
                    const uint32_t* w = ucode + size_t(in.addr) * 3;
                    auto read = [&](uint32_t r, uint32_t mask = 0xF) { a.reads.push_back({ r, mask }); };
                    if (in.fetch)
                    {
                        const auto& f = *reinterpret_cast<const FetchInstruction*>(w);
                        uint32_t dest, swizzle;
                        bool destRel, srcRel;
                        if (f.opcode() == FetchOpcode::kVertexFetch)
                        {
                            const auto op = f.vertex_fetch();
                            dest = op.dest(); swizzle = op.dest_swizzle(); destRel = op.is_dest_relative(); srcRel = op.is_src_relative();
                            read(op.src());
                        }
                        else
                        {
                            const auto op = f.texture_fetch();
                            dest = op.dest(); swizzle = op.dest_swizzle(); destRel = op.is_dest_relative(); srcRel = op.is_src_relative();
                            read(op.src());
                            if (op.opcode() != FetchOpcode::kTextureFetch)
                                swizzle = 07777;  // setTexLOD and the like write no register
                        }
                        a.readsAll = srcRel;
                        uint32_t mask = 0;
                        for (uint32_t c = 0; c < 4; c++)
                            if (((swizzle >> (3 * c)) & 7) != 7)
                                mask |= 1u << c;
                        if (destRel)
                            a.writesAll = true;
                        else if (mask)
                            a.writes.push_back({ dest, mask });
                        return a;
                    }
                    const auto& op = *reinterpret_cast<const AluInstruction*>(w);
                    const auto& vinfo = GetAluVectorOpcodeInfo(op.vector_opcode());
                    const auto& sinfo = GetAluScalarOpcodeInfo(op.scalar_opcode());
                    bool constTemp = false;
                    switch (op.scalar_opcode())
                    {
                    case AluScalarOpcode::kMulsc0: case AluScalarOpcode::kMulsc1:
                    case AluScalarOpcode::kAddsc0: case AluScalarOpcode::kAddsc1:
                    case AluScalarOpcode::kSubsc0: case AluScalarOpcode::kSubsc1:
                        constTemp = true;
                        break;
                    default:
                        break;
                    }
                    // The operands used: the vector op's, and the scalar op's (src3, or a temporary of its own).
                    uint32_t used = vinfo.GetOperandCount();
                    if (sinfo.operand_count && !constTemp)
                        used = 3;
                    if (constTemp)
                        read(op.scalar_const_reg_op_src_temp_reg());
                    for (uint32_t i = skipSrc1 ? 2 : 1; i <= used; i++)
                        if (op.src_is_temp(i))
                        {
                            uint32_t reg = op.src_reg(i), mask = 0;
                            if (AluInstruction::is_src_temp_relative(reg))
                                a.readsAll = true;
                            for (uint32_t c = 0; c < 4; c++)
                                mask |= 1u << AluInstruction::GetSwizzledComponentIndex(op.src_swizzle(i), c);
                            read(AluInstruction::src_temp_reg(reg), mask);
                        }
                    a.predicate = ((vinfo.changed_state | sinfo.changed_state) & kAluOpChangedStatePredicate) != 0;
                    if (!op.is_export())
                    {
                        if (uint32_t m = op.GetVectorOpResultWriteMask())
                        {
                            if (op.is_vector_dest_relative())
                                a.writesAll = true;
                            else
                                a.writes.push_back({ op.vector_dest(), m });
                        }
                        if (uint32_t m = op.GetScalarOpResultWriteMask())
                        {
                            if (op.is_scalar_dest_relative())
                                a.writesAll = true;
                            else
                                a.writes.push_back({ op.scalar_dest(), m });
                        }
                    }
                    return a;
                };
                auto readsComp = [](const Access& a, uint32_t r, uint32_t c) {
                    if (a.readsAll)
                        return true;
                    for (auto& [reg, m] : a.reads)
                        if (reg == r && (m >> c & 1))
                            return true;
                    return false;
                };
                auto writesComp = [](const Access& a, uint32_t r, uint32_t c) {
                    if (a.writesAll)
                        return true;
                    for (auto& [reg, m] : a.writes)
                        if (reg == r && (m >> c & 1))
                            return true;
                    return false;
                };

                // Pass 1: groups of 9 taps.
                std::vector<std::vector<Tap>> groups;
                std::vector<Tap> open;
                for (size_t idx = 0; idx < list.size(); idx++)
                {
                    Tap t;
                    if (asTap(idx, t))
                    {
                        bool fits = !open.empty() && t.slot == open[0].slot && t.src == open[0].src && t.swizzle == open[0].swizzle &&
                            list[idx].guard == list[open[0].idx].guard;
                        for (const Tap& o : open)
                            fits = fits && (o.ox != t.ox || o.oy != t.oy);
                        if (!fits)
                            open.clear();
                        open.push_back(t);
                        if (open.size() == 9)
                        {
                            groups.push_back(open);
                            open.clear();
                        }
                        continue;
                    }
                    if (!open.empty())
                    {
                        Access a = accessOf(idx);
                        bool src = a.writesAll || a.predicate;
                        for (uint32_t c = 0; c < 4; c++)
                            src = src || writesComp(a, open[0].src, c);
                        if (src)
                            open.clear();
                    }
                }

                // Pass 2: each group's compares, and which taps need their depth.
                for (const auto& group : groups)
                {
                    const size_t first = group[0].idx;
                    const uint64_t guard = list[first].guard;
                    struct Live { size_t tap; bool consumed = false, rawRead = false; };
                    std::map<uint32_t, Live> live;   // register * 4 + component
                    std::map<size_t, bool> dead;      // tap idx -> its register provably read only by its compare
                    std::map<size_t, std::array<int, 4>> compares;
                    size_t lastCompare = 0;
                    bool ok = true, killsValid = true;
                    auto isGroupTap = [&](size_t idx, Tap& out) {
                        for (const Tap& t : group)
                            if (t.idx == idx) { out = t; return true; }
                        return false;
                    };
                    std::set<size_t> consumedTaps;
                    bool entered = false;  // past a clause control can also arrive at from elsewhere
                    size_t idx = first;
                    for (; idx < list.size() && ok; idx++)
                    {
                        // The scan follows the program while control can only go
                        // on to the next clause: it stops where control can leave
                        // (a jump, call, loop or return), and where the program
                        // ends every tap only its compare read is dead. Past a
                        // clause control can also arrive at from elsewhere (an
                        // entry), no new compare is taken (the range check below
                        // wants none there), but a write still ends a tap's life:
                        // the paths that matter are the ones through its compare.
                        if (idx > first && list[idx].cf != list[idx - 1].cf)
                        {
                            if (!passable(list[idx - 1].cf, false))
                            {
                                if (passable(list[idx - 1].cf, true))  // the program ended
                                    for (auto& [key, l] : live)
                                        if (consumedTaps.count(l.tap) && !l.rawRead)
                                            dead[l.tap] = true;
                                break;
                            }
                            bool straight = true;
                            for (size_t c = list[idx - 1].cf + 1; c <= list[idx].cf; c++)
                            {
                                entered = entered || target[c];
                                straight = straight && (c == list[idx].cf || passable(c, false));
                            }
                            if (!straight)
                                break;
                        }
                        Tap t;
                        if (isGroupTap(idx, t))
                        {
                            auto prev = live.find(t.dreg * 4 + t.dcomp);
                            if (prev != live.end() && !consumedTaps.count(prev->second.tap))
                                ok = false;  // a tap overwritten by another before any compare read it
                            live[t.dreg * 4 + t.dcomp] = Live{ idx };
                            continue;
                        }
                        // A compare reading taps: an sge whose first operand is a tap on every component it writes.
                        std::array<int, 4> mapped{ -1, -1, -1, -1 };
                        bool isCompare = false;
                        if (!list[idx].fetch && !entered)
                        {
                            const auto& op = *reinterpret_cast<const AluInstruction*>(ucode + size_t(list[idx].addr) * 3);
                            uint32_t mask = op.GetVectorOpResultWriteMask();
                            if (op.vector_opcode() == AluVectorOpcode::kSge && !op.is_export() && mask && !op.is_vector_dest_relative() &&
                                op.src_is_temp(1) && !AluInstruction::is_src_temp_relative(op.src_reg(1)) &&
                                !AluInstruction::is_src_temp_value_absolute(op.src_reg(1)) && !op.src_negate(1) && list[idx].guard == guard)
                            {
                                uint32_t reg = AluInstruction::src_temp_reg(op.src_reg(1));
                                isCompare = true;
                                for (uint32_t c = 0; c < 4 && isCompare; c++)
                                {
                                    if (!(mask >> c & 1))
                                        continue;
                                    uint32_t comp = AluInstruction::GetSwizzledComponentIndex(op.src_swizzle(1), c);
                                    auto it = live.find(reg * 4 + comp);
                                    if (it == live.end())
                                        isCompare = false;
                                    else
                                        mapped[c] = int(it->second.tap);
                                }
                            }
                        }
                        Access a = accessOf(idx, isCompare);
                        // Raw reads: a live tap's component read other than as a compare's tap.
                        for (auto& [key, l] : live)
                            if (readsComp(a, key / 4, key % 4))
                                l.rawRead = true;
                        if (isCompare)
                        {
                            for (uint32_t c = 0; c < 4; c++)
                                if (mapped[c] >= 0)
                                {
                                    for (auto& [key, l] : live)
                                        if (l.tap == size_t(mapped[c]))
                                            l.consumed = true;
                                    consumedTaps.insert(size_t(mapped[c]));
                                }
                            compares[idx] = mapped;
                            lastCompare = idx;
                        }
                        if (a.predicate)
                            killsValid = false;
                        // Writes end a tap's life.
                        for (auto it = live.begin(); it != live.end();)
                        {
                            if (!writesComp(a, it->first / 4, it->first % 4))
                            {
                                ++it;
                                continue;
                            }
                            if (!it->second.consumed && !consumedTaps.count(it->second.tap))
                                ok = false;  // overwritten before any compare read it
                            else if (!it->second.rawRead && (list[idx].guard == 0 || (killsValid && list[idx].guard == guard)))
                                dead[it->second.tap] = true;
                            it = live.erase(it);
                        }
                    }
                    // The scan ran off the program's last clause: the end.
                    if (ok && idx == list.size() && passable(list.back().cf, true) && !passable(list.back().cf, false))
                        for (auto& [key, l] : live)
                            if (consumedTaps.count(l.tap) && !l.rawRead)
                                dead[l.tap] = true;
                    if (!ok || consumedTaps.size() != 9 || compares.empty())
                        continue;
                    // The whole range runs together.
                    for (size_t k = first; k <= lastCompare && ok; k++)
                        if (accessOf(k).predicate)
                            ok = false;
                    for (size_t c = list[first].cf; c <= list[lastCompare].cf && ok; c++)
                        ok = passable(c, c == list[lastCompare].cf) && (c == list[first].cf || !target[c]);
                    if (!ok)
                        continue;
                    uint32_t g = uint32_t(pcfGroups.size());
                    pcfGroups.push_back({ group[0].slot, list[first].addr });
                    for (const Tap& t : group)
                        pcfTaps[list[t.idx].addr] = PcfTap{ g, t.ox, t.oy, !dead.count(t.idx) };
                    for (auto& [at, m] : compares)
                    {
                        std::array<int, 4> byAddr{ -1, -1, -1, -1 };
                        for (uint32_t c = 0; c < 4; c++)
                            if (m[c] >= 0)
                                byAddr[c] = int(list[size_t(m[c])].addr);
                        auto& dst = pcfCompares.try_emplace(list[at].addr, std::array<int, 4>{ -1, -1, -1, -1 }).first->second;
                        for (uint32_t c = 0; c < 4; c++)
                            if (byAddr[c] >= 0)
                                dst[c] = byAddr[c];
                    }
                }
            }

            void Translate()
            {
                // Control flow program bound: the first exec's instruction address.
                uint32_t bound = uint32_t(dwords / 3);
                std::vector<ControlFlowInstruction> cf;
                for (uint32_t i = 0; i < bound; i++)
                {
                    ControlFlowInstruction ab[2];
                    UnpackControlFlowInstructions(ucode + i * 3, ab);
                    for (auto& c : ab)
                        if (IsControlFlowOpcodeExec(c.opcode()))
                            bound = std::min(bound, c.exec.address());
                }
                for (uint32_t i = 0; i < bound; i++)
                {
                    ControlFlowInstruction ab[2];
                    UnpackControlFlowInstructions(ucode + i * 3, ab);
                    cf.push_back(ab[0]);
                    cf.push_back(ab[1]);
                }

                pcfMode = options.shadowPcf >= 0 ? options.shadowPcf : 1;
                pcfEarly = options.shadowPcfEarly >= 0 ? options.shadowPcfEarly > 0 : ShadowPcfEarlyOut();
                if (stage == ShaderStage::Pixel && pcfMode > 0)
                    PlanShadowPcf(cf);

                // Straight-line programs (no jumps, calls, loops) are emitted flat.
                bool flat = true;
                for (auto& c : cf)
                {
                    switch (c.opcode())
                    {
                    case ControlFlowOpcode::kLoopStart:
                    case ControlFlowOpcode::kLoopEnd:
                    case ControlFlowOpcode::kCondCall:
                    case ControlFlowOpcode::kReturn:
                    case ControlFlowOpcode::kCondJmp:
                        flat = false;
                        break;
                    default:
                        break;
                    }
                }

                if (flat)
                {
                    for (auto& c : cf)
                    {
                        if (!EmitFlatCf(c))
                            break;
                    }
                }
                else
                {
                    EmitStateMachine(cf);
                }
            }

            // Returns false once the program has ended.
            bool EmitFlatCf(const ControlFlowInstruction& c)
            {
                switch (c.opcode())
                {
                case ControlFlowOpcode::kNop:
                case ControlFlowOpcode::kAlloc:
                case ControlFlowOpcode::kMarkVsFetchDone:
                    return true;
                case ControlFlowOpcode::kExec:
                case ControlFlowOpcode::kExecEnd:
                    Exec(c.exec);
                    return c.opcode() != ControlFlowOpcode::kExecEnd;
                case ControlFlowOpcode::kCondExec:
                case ControlFlowOpcode::kCondExecEnd:
                case ControlFlowOpcode::kCondExecPredClean:
                case ControlFlowOpcode::kCondExecPredCleanEnd:
                    Line(std::format("if (boolConst({}u) == {}) {{", c.cond_exec.bool_address(),
                        c.cond_exec.condition() ? "true" : "false"));
                    indent++;
                    Exec(c.cond_exec);
                    indent--;
                    Line("}");
                    return c.opcode() != ControlFlowOpcode::kCondExecEnd &&
                        c.opcode() != ControlFlowOpcode::kCondExecPredCleanEnd;
                case ControlFlowOpcode::kCondExecPred:
                case ControlFlowOpcode::kCondExecPredEnd:
                    Line(std::format("if (p0 == {}) {{", c.cond_exec_pred.condition() ? "true" : "false"));
                    indent++;
                    Exec(c.cond_exec_pred);
                    indent--;
                    Line("}");
                    return c.opcode() != ControlFlowOpcode::kCondExecPredEnd;
                default:
                    Error(std::format("control flow opcode {} in a flat program", uint32_t(c.opcode())));
                    return false;
                }
            }

            // General control flow: a pc state machine over CF indices.
            void EmitStateMachine(const std::vector<ControlFlowInstruction>& cf)
            {
                Line("int cfpc = 0;");
                Line("int loopDepth = 0; int loopCounter[4]; int loopAl[4];");
                Line("int callDepth = 0; int callStack[4];");
                Line("while (cfpc >= 0) {");
                indent++;
                Line("switch (cfpc) {");
                for (size_t i = 0; i < cf.size(); i++)
                {
                    const auto& c = cf[i];
                    Line(std::format("case {}: {{", i));
                    indent++;
                    int next = int(i) + 1;
                    switch (c.opcode())
                    {
                    case ControlFlowOpcode::kNop:
                    case ControlFlowOpcode::kAlloc:
                    case ControlFlowOpcode::kMarkVsFetchDone:
                        break;
                    case ControlFlowOpcode::kExec:
                    case ControlFlowOpcode::kExecEnd:
                        Exec(c.exec);
                        if (c.opcode() == ControlFlowOpcode::kExecEnd)
                            next = -1;
                        break;
                    case ControlFlowOpcode::kCondExec:
                    case ControlFlowOpcode::kCondExecEnd:
                    case ControlFlowOpcode::kCondExecPredClean:
                    case ControlFlowOpcode::kCondExecPredCleanEnd:
                        Line(std::format("if (boolConst({}u) == {}) {{", c.cond_exec.bool_address(),
                            c.cond_exec.condition() ? "true" : "false"));
                        indent++;
                        Exec(c.cond_exec);
                        indent--;
                        Line("}");
                        if (c.opcode() == ControlFlowOpcode::kCondExecEnd || c.opcode() == ControlFlowOpcode::kCondExecPredCleanEnd)
                            next = -1;
                        break;
                    case ControlFlowOpcode::kCondExecPred:
                    case ControlFlowOpcode::kCondExecPredEnd:
                        Line(std::format("if (p0 == {}) {{", c.cond_exec_pred.condition() ? "true" : "false"));
                        indent++;
                        Exec(c.cond_exec_pred);
                        indent--;
                        Line("}");
                        if (c.opcode() == ControlFlowOpcode::kCondExecPredEnd)
                            next = -1;
                        break;
                    case ControlFlowOpcode::kLoopStart:
                    {
                        // Loop constant: count (8 bits), start (8), step (signed 8).
                        uint32_t lc = c.loop_start.loop_id();
                        Line(std::format("uint lcv = u.loops[{}u][{}u];", lc >> 2, lc & 3));
                        Line("loopCounter[loopDepth] = 0; loopAl[loopDepth] = aL;");
                        Line("aL = int(bitfieldExtract(lcv, 8, 8)); loopDepth++;");
                        Line(std::format("if (bitfieldExtract(lcv, 0, 8) == 0u) {{ loopDepth--; aL = loopAl[loopDepth]; cfpc = {}; break; }}",
                            c.loop_start.address()));
                        break;
                    }
                    case ControlFlowOpcode::kLoopEnd:
                    {
                        uint32_t lc = c.loop_end.loop_id();
                        Line(std::format("uint lcv = u.loops[{}u][{}u];", lc >> 2, lc & 3));
                        Line("int li = loopDepth - 1; loopCounter[li]++; aL += bitfieldExtract(int(lcv), 16, 8);");
                        std::string cond = std::format("uint(loopCounter[li]) < bitfieldExtract(lcv, 0, 8)");
                        if (c.loop_end.is_predicated_break())
                            cond += std::format(" && !(p0 == {})", c.loop_end.condition() ? "true" : "false");
                        Line(std::format("if ({}) {{ cfpc = {}; break; }}", cond, c.loop_end.address()));
                        Line("loopDepth--; aL = loopAl[loopDepth];");
                        break;
                    }
                    case ControlFlowOpcode::kCondCall:
                    {
                        std::string cond = c.cond_call.is_unconditional() ? "true"
                            : c.cond_call.is_predicated()
                                ? std::format("p0 == {}", c.cond_call.condition() ? "true" : "false")
                                : std::format("boolConst({}u) == {}", c.cond_call.bool_address(), c.cond_call.condition() ? "true" : "false");
                        Line(std::format("if ({}) {{ callStack[callDepth++] = {}; cfpc = {}; break; }}", cond, i + 1, c.cond_call.address()));
                        break;
                    }
                    case ControlFlowOpcode::kReturn:
                        Line("if (callDepth > 0) { cfpc = callStack[--callDepth]; break; }");
                        next = -1;
                        break;
                    case ControlFlowOpcode::kCondJmp:
                    {
                        std::string cond = c.cond_jmp.is_unconditional() ? "true"
                            : c.cond_jmp.is_predicated()
                                ? std::format("p0 == {}", c.cond_jmp.condition() ? "true" : "false")
                                : std::format("boolConst({}u) == {}", c.cond_jmp.bool_address(), c.cond_jmp.condition() ? "true" : "false");
                        Line(std::format("if ({}) {{ cfpc = {}; break; }}", cond, c.cond_jmp.address()));
                        break;
                    }
                    default:
                        Error(std::format("control flow opcode {} not implemented", uint32_t(c.opcode())));
                    }
                    Line(std::format("cfpc = {}; break;", next));
                    indent--;
                    Line("}");
                }
                Line("default: cfpc = -1; break;");
                Line("}");
                indent--;
                Line("}");
            }
        };

        // Helper functions every shader gets (after the common declarations).
        const char* kHelpers = R"(
vec4 vmax(vec4 a, vec4 b) { return mix(b, a, vec4(greaterThanEqual(a, b))); }
vec4 vmin(vec4 a, vec4 b) { return mix(b, a, vec4(lessThan(a, b))); }
float smax(float a, float b) { return a >= b ? a : b; }
float smin(float a, float b) { return a < b ? a : b; }
vec4 vsne(vec4 a, vec4 b) { return vec4(not(equal(a, b))); }
float dp4(vec4 a, vec4 b) { float r = xmul(a.x, b.x) + xmul(a.y, b.y) + xmul(a.z, b.z) + xmul(a.w, b.w); return r; }
float dp3(vec4 a, vec4 b) { float r = xmul(a.x, b.x) + xmul(a.y, b.y) + xmul(a.z, b.z); return r; }
float dp2add(vec4 a, vec4 b, vec4 c) { float r = xmul(a.x, b.x) + xmul(a.y, b.y) + c.x; return r; }
float max4(vec4 a) { return max(max(a.x, a.y), max(a.z, a.w)); }
float clampInf(float v) { return isinf(v) ? (v > 0.0 ? 3.402823466e+38 : -3.402823466e+38) : v; }
float clampNegInf(float v) { return (isinf(v) && v < 0.0) ? -3.402823466e+38 : v; }
float flushInf(float v) { return isinf(v) ? (v > 0.0 ? 0.0 : -0.0) : v; }
)";

        // Sun shadow PCF (Shadow Edges: Smooth). spcfW: the bilinear sample
        // at p (texels from the first texel's centre, 0 <= p < 2) of a 3x3
        // block given by rows (depths, or their 0/1 tests against the
        // reference: PCF), as separable weights. shadowPcfOn: NFSMW_SHADOW_AB's
        // gate (the renderer's flag, a depth-format slot).
        const char* kShadowPcfHelpers = R"(
float spcfW(vec3 r0, vec3 r1, vec3 r2, vec2 p)
{
    vec2 a = clamp(p, 0.0, 1.0), b = clamp(p - 1.0, 0.0, 1.0);
    vec3 wx = vec3(1.0 - a.x, a.x - b.x, b.x), wy = vec3(1.0 - a.y, a.y - b.y, b.y);
    return dot(wy, vec3(dot(wx, r0), dot(wx, r1), dot(wx, r2)));
}
)";
        const char* kShadowPcfGateGlsl = R"(
bool shadowPcfOn(uint slot)
{
    uint format = fetchDword(slot, 1u) & 0x3Fu;
    return (format == 22u || format == 23u) && (pc.flags & 16u) != 0u;  // k_24_8, k_24_8_FLOAT
}
)";

        const char* kStateHelpers = R"(
vec4 maxa(vec4 s0, vec4 s1)
{
    a0 = int(clamp(floor(s0.w + 0.5), -256.0, 255.0));
    return vmax(s0, s1);
}
float maxas(float a, float b, bool round)
{
    a0 = int(clamp(floor(round ? a + 0.5 : a), -256.0, 255.0));
    return smax(a, b);
}
vec4 setpPush(vec4 s0, vec4 s1, int op)
{
    bool cw = op == 0 ? s1.w == 0.0 : op == 1 ? !(s1.w == 0.0) : op == 2 ? s1.w > 0.0 : s1.w >= 0.0;
    p0 = s0.w == 0.0 && cw;
    bool cx = op == 0 ? s1.x == 0.0 : op == 1 ? !(s1.x == 0.0) : op == 2 ? s1.x > 0.0 : s1.x >= 0.0;
    return vec4(((s0.x == 0.0 && cx) ? -1.0 : s0.x) + 1.0);
}
float setp(bool c) { p0 = c; return c ? 0.0 : 1.0; }
float setpInv(float a) { p0 = a == 1.0; return p0 ? 0.0 : (a == 0.0 ? 1.0 : a); }
float setpPop(float a) { float m = a - 1.0; p0 = m <= 0.0; return p0 ? 0.0 : m; }
float setpClr() { p0 = false; return 3.402823466e+38; }
float setpRstr(float a) { p0 = a == 0.0; return p0 ? 0.0 : a; }
float mulsprev2(float a, float b)
{
    bool ok = ps != -3.402823466e+38 && !isinf(ps) && !isnan(ps) && !isinf(b) && !isnan(b) && b > 0.0;
    return ok ? xmul(a, ps) : -3.402823466e+38;
}
vec4 dstop(vec4 s0, vec4 s1) { return vec4(1.0, xmul(s0.y, s1.y), s0.z, s1.w); }
vec4 cubeop(vec4 s)
{
    // Operand is .z_xy (the y slot unused): x = s.z, y = s.w, z = s.x.
    float x = s.z, y = s.w, z = s.x;
    float tc, sc, ma, id;
    if (abs(z) >= abs(x) && abs(z) >= abs(y)) { tc = -y; sc = z < 0.0 ? -x : x; ma = z; id = z < 0.0 ? 5.0 : 4.0; }
    else if (abs(y) >= abs(x)) { tc = y < 0.0 ? -z : z; sc = x; ma = y; id = y < 0.0 ? 3.0 : 2.0; }
    else { tc = -y; sc = x < 0.0 ? z : -z; ma = x; id = x < 0.0 ? 1.0 : 0.0; }
    return vec4(tc, sc, 2.0 * ma, id);
}
vec3 cubeDirection(vec3 c)
{
    // tfetchCube takes (sc, tc, face): D3D's compiler swizzles the cube
    // instruction's (tc, sc) result with .yx before scaling it into [1, 2]
    // (NFSMW: mad r2.xy, r0.yx, 1/|2ma|, 1.5). Reading it as (tc, sc)
    // transposed every face (the rear-view mirror showed the world at 90
    // degrees). Back to a direction, inverting cubeop().
    float sc = c.x - 1.5, tc = c.y - 1.5;
    int face = int(c.z + 0.5);
)" GLSL_CUBE_FACE_DIRECTION R"(
}
)";
    }

    bool PackedConstantsEnabled()
    {
        static const bool enabled = [] { const char* v = std::getenv("NFSMW_CP_OPT"); return !v || (strtoul(v, nullptr, 0) & 8) != 0; }();
        return enabled;
    }

    bool ShadowPcfEarlyOut()
    {
        static const bool enabled = [] { const char* v = std::getenv("NFSMW_SHADOW_PCF_EARLY"); return !(v && v[0] == '0'); }();
        return enabled;
    }

    bool RelativeConstantsFromRing()
    {
        static const bool enabled = [] { const char* v = std::getenv("NFSMW_CONST_REL_RING"); return !(v && v[0] == '0'); }();
        return enabled;
    }

    static std::atomic<uint32_t> s_scalarRegistersDefault{ 0 };

    void SetScalarRegistersDefault(uint32_t stageMask)
    {
        s_scalarRegistersDefault.store(stageMask & 3u, std::memory_order_relaxed);
    }

    uint32_t ScalarRegisters()
    {
        static const int forced = [] {
            const char* v = std::getenv("NFSMW_SCALAR_REGS");
            return v && *v ? int(strtoul(v, nullptr, 0) & 3u) : -1;
        }();
        return forced >= 0 ? uint32_t(forced) : s_scalarRegistersDefault.load(std::memory_order_relaxed);
    }

    // r[N] -> rN (N constant) through the whole shader, with one variable per
    // register. Empty if anything still indexes r[] another way.
    static std::string ScalarizeRegisters(const std::string& g, uint32_t regCount)
    {
        std::string decl = "vec4 ", zero;
        for (uint32_t i = 0; i < regCount; i++)
        {
            decl += std::format("{}r{}", i ? ", " : "", i);
            zero += std::format("r{} = vec4(0.0); ", i);
        }
        decl += ";";
        std::string s = g;
        auto replaceAll = [&](const std::string& from, const std::string& to) {
            for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size()))
                s.replace(at, from.size(), to);
        };
        replaceAll("vec4 r[R_COUNT];", decl);
        replaceAll("for (int i = 0; i < R_COUNT; i++) r[i] = vec4(0.0);", zero);
        std::string out;
        out.reserve(s.size());
        auto ident = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
        for (size_t i = 0; i < s.size(); i++)
        {
            if (s[i] == 'r' && i + 1 < s.size() && s[i + 1] == '[' && (i == 0 || !ident(s[i - 1])))
            {
                size_t j = i + 2;
                while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j])))
                    j++;
                if (j == i + 2 || j >= s.size() || s[j] != ']')
                    return {};  // r[<expression>]: keep the array
                out += 'r';
                out.append(s, i + 2, j - i - 2);
                i = j;
                continue;
            }
            out += s[i];
        }
        return out;
    }

    TranslatedShader TranslateShader(ShaderStage stage, const uint32_t* ucode, size_t dwordCount)
    {
        return TranslateShader(stage, ucode, dwordCount, TranslateOptions{});
    }

    TranslatedShader TranslateShader(ShaderStage stage, const uint32_t* ucode, size_t dwordCount, const TranslateOptions& options)
    {
        Translator t;
        t.stage = stage;
        t.options = options;
        t.ucode = ucode;
        t.dwords = dwordCount;
        t.Translate();

        TranslatedShader result = std::move(t.out);
        uint32_t regCount = std::max<uint32_t>(t.maxRegister + 1, t.usesRelativeRegisters ? 64u : 1u);
        if (stage == ShaderStage::Pixel)
            regCount = std::max(regCount, 16u);  // interpolators land in r0..r15
        result.registerCount = regCount;

        std::string g = ShaderCommonGlsl();
        g += kHelpers;
        g += std::format("#define R_COUNT {}\n", regCount);
        for (uint32_t n = 0; n < 32; n++)
        {
            if (result.texture2DMask & (1u << n))
                g += std::format("layout(set = 1, binding = {}) uniform sampler2D t2d_{};\n", n, n);
            if (result.texture3DMask & (1u << n))
                g += std::format("layout(set = 1, binding = {}) uniform sampler3D t3d_{};\n", 32 + n, n);
            if (result.textureCubeMask & (1u << n))
                g += std::format("layout(set = 1, binding = {}) uniform samplerCube tcube_{};\n", 64 + n, n);
        }
        if (result.sharpFetches)
            g += gpu::sharp_ui::kSharpTex2DGlsl;
        if (options.sharpFetch2D && stage == ShaderStage::Pixel)
            g += gpu::sharp_ui::kUiMaskDeclGlsl;
        if (stage == ShaderStage::Vertex)
        {
            // Only the interpolators this shader writes are outputs: each
            // one costs vertex output bandwidth (a lot on tile-based GPUs).
            for (uint32_t i = 0; i < 16; i++)
                if (result.interpolatorMask & (1u << i))
                    g += std::format("layout(location = {0}) out vec4 o_i{0};\n", i);
            g += "vec4 o_interpT[16];\n";
            g += "out gl_PerVertex { invariant vec4 gl_Position; float gl_PointSize; };\n";
        }
        else
        {
            // Inputs exist only for interpolators the vertex shader writes:
            // the host defines IN0..IN15 per pipeline.
            for (uint32_t i = 0; i < 16; i++)
                g += std::format("#ifdef IN{0}\nlayout(location = {0}) in vec4 i_i{0};\n#endif\n", i);
            // Colour outputs exist only for bound attachments (MoltenVK rejects
            // outputs without one): the host defines OUT0..OUT3 per pipeline.
            for (int i = 0; i < 4; i++)
                g += std::format("#ifdef OUT{0}\nlayout(location = {0}) out vec4 o_c{0};\n#endif\n", i);
            g += "vec4 o_color[4];\n";
        }
        // Registers and machine state are globals so helpers can update them.
        g += "vec4 r[R_COUNT];\nbool p0 = false;\nint a0 = 0;\nint aL = 0;\nfloat ps = 0.0;\nfloat texLod = 0.0;\n";
        g += "vec4 o_memexport[5];\nvec4 o_unused;\n";
        if (stage == ShaderStage::Vertex)
            g += "vec4 o_position = vec4(0.0, 0.0, 0.0, 1.0);\nvec4 o_pointSize = vec4(0.0);\n";
        else
            g += "vec4 o_depth = vec4(0.0);\nbool killed = false;\n";
        g += kStateHelpers;
        result.shadowPcfGroups = uint32_t(t.pcfGroups.size());
        if (!t.pcfGroups.empty())
        {
            // Sun shadow PCF (Shadow Edges: Smooth): per group, the first
            // tap's position in the 3x3 block (texels; the block's origin is
            // texel floor(s) - 1), the block's depths row by row, and under
            // NFSMW_SHADOW_AB whether PCF runs in this draw.
            for (size_t i = 0; i < t.pcfGroups.size(); i++)
                g += std::format("{1}vec2 spcfA{0} = vec2(0.0);\nvec3 spcfR{0}0 = vec3(0.0), spcfR{0}1 = vec3(0.0), spcfR{0}2 = vec3(0.0);\n", i,
                    t.pcfMode == 2 ? std::format("bool spcfOn{} = false;\n", i) : std::string());
            g += kShadowPcfHelpers;
            if (t.pcfMode == 2)
                g += kShadowPcfGateGlsl;
        }
        // Relative reads come from the same constants through the ring's
        // storage buffer: AMD's Windows driver reads a dynamically indexed
        // u.c[] inside per-pixel predicated blocks as if a0 were 0, which
        // turned Most Wanted's far verge grass black (issue #67).
        // NFSMW_CONST_REL_RING=0 reads the uniform buffer as before.
        if (RelativeConstantsFromRing())
            g += std::format(R"(
vec4 constRel(int index, int rel)
{{
    int i = index + rel;
    if (i < 0 || i > 255)
        return vec4(0.0);
    uint w = (pc.constRing >> 2) + ({} + uint(i)) * 4u;
    return uintBitsToFloat(uvec4(g_ring[w], g_ring[w + 1u], g_ring[w + 2u], g_ring[w + 3u]));
}}
)", stage == ShaderStage::Vertex ? "pc.vsConstBase" : "pc.psConstBase");
        else
            g += std::format(R"(
vec4 constRel(int index, int rel)
{{
    int i = index + rel;
    return (i < 0 || i > 255) ? vec4(0.0) : u.c[{} + uint(i)];
}}
)", stage == ShaderStage::Vertex ? "pc.vsConstBase" : "pc.psConstBase");
        if (stage == ShaderStage::Pixel)
            g += "vec4 killv(bool c) { if (c) killed = true; return vec4(0.0); }\nfloat kills(bool c) { if (c) killed = true; return 0.0; }\n";
        else
            g += "vec4 killv(bool c) { return vec4(0.0); }\nfloat kills(bool c) { return 0.0; }\n";

        if (stage == ShaderStage::Vertex)
        {
            // The shader body runs as a function so main() can run it for
            // several source vertices (rectangle lists synthesise a 4th corner).
            g += R"(
uint fetchIndex(uint i)
{
    if (pc.indexAddress == 0u)
        return i;
    if ((pc.indexInfo & 1u) != 0u)
        return gpuSwap(memLoad(((pc.indexAddress + i * 4u) & 0x1FFFFFFFu) >> 2), (pc.indexInfo >> 1) & 3u);
    uint endian = (pc.indexInfo >> 1) & 3u;
    uint word = gpuSwap(memLoad(((pc.indexAddress + (i & ~1u) * 2u) & 0x1FFFFFFFu) >> 2), endian);
    // Memory is read as little-endian words and then swapped: with 8in16 or no
    // swap the first 16-bit index ends up in the low half, with 8in32/16in32 in
    // the high half.
    bool firstInHigh = endian == 2u || endian == 3u;
    bool high = ((i & 1u) != 0u) != firstInHigh;
    return high ? (word >> 16) : (word & 0xFFFFu);
}

void runVertex(uint logicalIndex)
{
    for (int i = 0; i < R_COUNT; i++) r[i] = vec4(0.0);
    for (int i = 0; i < 16; i++) o_interpT[i] = vec4(0.0);
    p0 = false; a0 = 0; aL = 0; ps = 0.0;
    o_position = vec4(0.0, 0.0, 0.0, 1.0);
    r[0].x = float(fetchIndex(logicalIndex) + pc.indexOffset);
)";
            g += t.body;
            g += R"(}

void main()
{
    uint vid = uint(gl_VertexIndex);
    uint mode = (pc.indexInfo >> 3) & 3u;   // 0 as is, 1 rectangle list, 2 quad list
    // runVertex() has ONE call site: compilers inline every call, and the
    // rectangle's synthesised corner used to add three more copies of the
    // whole shader (RADV took ~10x longer to compile every vertex shader).
    uint first = vid, count = 1u;
    if (mode == 1u)
    {
        // Rectangle: v0 v1 v2 given, v3 = v1 + v2 - v0. Triangles (0,1,2) (1,3,2).
        uint rect = vid / 6u, k = vid % 6u;
        uint corner = k == 0u ? 0u : k == 1u ? 1u : k == 2u ? 2u : k == 3u ? 1u : k == 4u ? 3u : 2u;
        first = rect * 3u + (corner < 3u ? corner : 0u);
        count = corner < 3u ? 1u : 3u;
    }
    else if (mode == 2u)
    {
        uint quad = vid / 6u, k = vid % 6u;
        first = quad * 4u + (k == 0u ? 0u : k == 1u ? 1u : k == 2u ? 2u : k == 3u ? 0u : k == 4u ? 2u : 3u);
    }
    vec4 pos = vec4(0.0), pos0 = vec4(0.0);
    vec4 it[16], it0[16];
    for (uint c = 0u; c < count; c++)
    {
        runVertex(first + c);
        if (count == 3u && c == 0u)
        {
            pos0 = o_position; it0 = o_interpT;
        }
        else if (c <= 1u)
        {
            pos = o_position; it = o_interpT;
        }
        else
        {
            // Same order as before: (v1 + v2) - v0.
            pos = pos + o_position - pos0;
            for (int i = 0; i < 16; i++) it[i] = it[i] + o_interpT[i] - it0[i];
        }
    }
@@VS_OUTPUTS@@    // PA_CL_VTE_CNTL formats (as Xenia): bit 1 W is not 1/W, bit 2 XY were
    // already divided by W, bit 3 Z was.
    if ((pc.flags & 2u) == 0u) pos.w = 1.0 / pos.w;
    if ((pc.flags & 4u) != 0u) pos.xy *= pos.w;
    if ((pc.flags & 8u) != 0u) pos.z *= pos.w;
    if ((pc.flags & 1u) != 0u)
    {
        // Guest viewport transform folded into the shader: guest clip space
        // -> render target pixels -> host NDC over the whole target.
        pos.xy = pos.xy * pc.posScale.xy + pc.posScale.zw * pos.w;
    }
    gl_Position = pos;
    gl_PointSize = 1.0;
}
)";
        }
        else
        {
            g += "\nvoid main()\n{\n";
            g += "    for (int i = 0; i < R_COUNT; i++) r[i] = vec4(0.0);\n";
            for (uint32_t i = 0; i < std::min(16u, regCount); i++)
                g += std::format("#ifdef IN{0}\n    r[{0}] = i_i{0};\n#endif\n", i);
            g += t.body;
            // discard costs hidden-surface removal on tile-based GPUs: only
            // shaders with kills, and pipelines with alpha test (ALPHA_TEST).
            if (result.usesKill)
                g += "    if (killed) discard;\n";
            g += R"(#ifdef ALPHA_TEST
    // Alpha test (RB_COLORCONTROL): func 0 never ... 7 always.
    uint af = pc.alphaTest & 7u;
    float a = o_color[0].a;
    bool pass = af == 7u || (af == 1u && a < pc.alphaRef) || (af == 2u && a == pc.alphaRef) ||
        (af == 3u && a <= pc.alphaRef) || (af == 4u && a > pc.alphaRef) || (af == 5u && a != pc.alphaRef) ||
        (af == 6u && a >= pc.alphaRef);
    if (!pass) discard;
#endif
@@UI_MASK@@#ifdef OUT0
    o_c0 = o_color[0];
#endif
#ifdef OUT1
    o_c1 = o_color[1];
#endif
#ifdef OUT2
    o_c2 = o_color[2];
#endif
#ifdef OUT3
    o_c3 = o_color[3];
#endif
}
)";
        }
        if (size_t at = g.find("@@UI_MASK@@"); at != std::string::npos)
            g.replace(at, strlen("@@UI_MASK@@"), options.sharpFetch2D ? gpu::sharp_ui::kUiMaskStoreGlsl : "");
        if (stage == ShaderStage::Vertex)
        {
            std::string outs;
            for (uint32_t i = 0; i < 16; i++)
                if (result.interpolatorMask & (1u << i))
                    outs += std::format("    o_i{0} = it[{0}];\n", i);
            size_t at = g.find("@@VS_OUTPUTS@@");
            g.replace(at, strlen("@@VS_OUTPUTS@@"), outs);
        }
        // Issue #67: one variable per register (see ScalarRegisters()).
        if (!t.usesRelativeRegisters && (ScalarRegisters() >> uint32_t(stage) & 1u))
            if (std::string s = ScalarizeRegisters(g, regCount); !s.empty())
                g = std::move(s);
        // Absolute constant reads: packed slot numbers (the count of used
        // constants below), unless the shader also addresses them relatively.
        result.constPacked = PackedConstantsEnabled() && !result.constRelative;
        {
            std::string resolved;
            resolved.reserve(g.size());
            size_t pos = 0;
            while (true)
            {
                size_t at = g.find("@CK", pos);
                if (at == std::string::npos)
                {
                    resolved.append(g, pos, std::string::npos);
                    break;
                }
                resolved.append(g, pos, at - pos);
                size_t end = g.find('@', at + 3);
                uint32_t index = uint32_t(std::stoul(g.substr(at + 3, end - at - 3)));
                uint32_t slot = index;
                if (result.constPacked)
                {
                    slot = 0;
                    for (uint32_t q = 0; q < index / 64; q++)
                        slot += uint32_t(__builtin_popcountll(result.constUsed[q]));
                    slot += uint32_t(__builtin_popcountll(result.constUsed[index / 64] & ((1ull << (index % 64)) - 1)));
                }
                resolved += std::to_string(slot) + "u";
                pos = end + 1;
            }
            g = std::move(resolved);
        }
        result.glsl = std::move(g);
        result.ok = result.error.empty();
        return result;
    }
}
