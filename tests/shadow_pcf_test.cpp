// SpeedBreaker tests. GPL-3.0-or-later (see COPYING).
//
// Sun shadow PCF (GitHub #90, gpu/shader_translator.h; Settings > Graphics > Shadow Edges: Smooth):
//   - the CPU model of the GLSL's block maths: the 4 gathers' texels laid out
//     as the 3x3 block, spcfW() of the depths at every tap's position equal
//     to the hardware's bilinear sample there, spcfW() of the per-texel tests
//     equal to their bilinear weights (PCF), a ramp (not a step) across an
//     edge; where the 9 tests agree (the early out) every tap's PCF equals
//     that 0 or 1 and so does the game's own test;
//   - the translator on Most Wanted's own pixel shaders (game/shaders, the
//     dumped microcode; the part is skipped, exit 77, without them): exactly
//     the 13 sun-shadow receivers are rewritten (9a81af49: both of its 9-tap
//     groups), each group with 9 taps, 4 gathers instead of 9 samples, its
//     compares, each block test with its early out (and none with
//     NFSMW_SHADOW_PCF_EARLY's off form, otherwise the same); NFSMW_SHADOW_AB's
//     form (gated) keeps the game's samples behind the flag; every other shader's GLSL is byte for byte the plain
//     translation's, and so is every shader's with the option off;
//   - mutations of a receiver's microcode that break the pattern (a tap's
//     offset, a tap's slot, the compare's opcode, its operand's negation)
//     leave it untouched;
//   - each rewritten receiver's GLSL (all three forms) written to a folder (with
//     the defines the renderer adds), compiled after this by glslangValidator
//     where it is installed;
// Arguments: the shader folder (game/shaders), and a folder the GLSL is written to.
//
// Build by hand (from the repo root):
//   c++ -std=c++20 -O2 -UNDEBUG -I runtime tests/shadow_pcf_test.cpp runtime/gpu/shader_translator.cpp runtime/gpu/xenos/ucode.cpp -o build/shadow_pcf_test
//   build/shadow_pcf_test game/shaders build/shadow_pcf_glsl
#include <gpu/shader_translator.h>
#include <gpu/xenos/ucode.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace
{
    int s_failures = 0;

#define CHECK(cond, ...)                                                    \
    do                                                                      \
    {                                                                       \
        if (!(cond))                                                        \
        {                                                                   \
            s_failures++;                                                   \
            fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
            fprintf(stderr, __VA_ARGS__);                                   \
            fprintf(stderr, "\n");                                          \
        }                                                                   \
    } while (0)

    using namespace xe::gpu;
    using namespace xe::gpu::ucode;

    // ---- CPU model of the GLSL (shader_translator.cpp kShadowPcfHelpers and the gather block).
    struct V3 { float x, y, z; };
    float Mix(float a, float b, float t) { return a + (b - a) * t; }
    float Clamp01(float v) { return std::fmin(std::fmax(v, 0.0f), 1.0f); }
    // spcfW: separable weights (1 - a, a - b, b) per axis, a = clamp(p), b = clamp(p - 1).
    float Tent(V3 r0, V3 r1, V3 r2, float px, float py)
    {
        float ax = Clamp01(px), bx = Clamp01(px - 1.0f), ay = Clamp01(py), by = Clamp01(py - 1.0f);
        float wx[3] = { 1.0f - ax, ax - bx, bx }, wy[3] = { 1.0f - ay, ay - by, by };
        auto row = [&](V3 r) { return wx[0] * r.x + wx[1] * r.y + wx[2] * r.z; };
        return wy[0] * row(r0) + wy[1] * row(r1) + wy[2] * row(r2);
    }

    // A W x H map with clamp-to-border 1.0 (the world receivers' sampler).
    struct Map
    {
        int w, h;
        std::vector<float> d;
        float At(int x, int y) const { return (x < 0 || y < 0 || x >= w || y >= h) ? 1.0f : d[size_t(y) * w + x]; }
        // Vulkan's textureGather at uv: (i0,j1), (i1,j1), (i1,j0), (i0,j0).
        void Gather(float u, float v, float out[4]) const
        {
            float x = u * w - 0.5f, y = v * h - 0.5f;
            int i0 = int(std::floor(x)), j0 = int(std::floor(y));
            out[0] = At(i0, j0 + 1); out[1] = At(i0 + 1, j0 + 1); out[2] = At(i0 + 1, j0); out[3] = At(i0, j0);
        }
        float Bilinear(float u, float v, const std::function<float(float)>& f) const
        {
            float x = u * w - 0.5f, y = v * h - 0.5f;
            int i0 = int(std::floor(x)), j0 = int(std::floor(y));
            float fx = x - float(i0), fy = y - float(j0);
            return Mix(Mix(f(At(i0, j0)), f(At(i0 + 1, j0)), fx), Mix(f(At(i0, j0 + 1)), f(At(i0 + 1, j0 + 1)), fx), fy);
        }
    };

    // The emitted block: s = uv * size, b = floor(s) - 1, A = s - b - 0.5, 4 gathers -> rows.
    struct Block { V3 r0, r1, r2; float ax, ay; };
    Block Gathered(const Map& m, float u, float v)
    {
        float sx = u * m.w, sy = v * m.h, bx = std::floor(sx) - 1.0f, by = std::floor(sy) - 1.0f;
        float g00[4], g10[4], g01[4], g11[4];
        m.Gather((bx + 1.0f) / m.w, (by + 1.0f) / m.h, g00);
        m.Gather((bx + 3.0f) / m.w, (by + 1.0f) / m.h, g10);
        m.Gather((bx + 1.0f) / m.w, (by + 3.0f) / m.h, g01);
        m.Gather((bx + 3.0f) / m.w, (by + 3.0f) / m.h, g11);
        Block b;
        b.r0 = { g00[3], g00[2], g10[3] };
        b.r1 = { g00[0], g00[1], g10[0] };
        b.r2 = { g01[3], g01[2], g11[3] };
        b.ax = sx - bx - 0.5f;
        b.ay = sy - by - 0.5f;
        return b;
    }

    void TestModel()
    {
        Map m{ 16, 16, {} };
        uint32_t seed = 12345;
        auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return float(seed >> 8) / float(1u << 24); };
        for (int i = 0; i < 256; i++)
            m.d.push_back(rnd());
        int checked = 0, uniform = 0;
        for (int k = 0; k < 4000; k++)
        {
            float u = rnd() * 1.2f - 0.1f, v = rnd() * 1.2f - 0.1f, ref = rnd();
            Block b = Gathered(m, u, v);
            for (int oy = -1; oy <= 1; oy++)
                for (int ox = -1; ox <= 1; ox++)
                {
                    float px = b.ax + 0.5f * ox, py = b.ay + 0.5f * oy;
                    CHECK(px >= 0.0f && px < 2.0f && py >= 0.0f && py < 2.0f, "tap outside the block: %f %f", px, py);
                    float tu = u + 0.5f * ox / m.w, tv = v + 0.5f * oy / m.h;
                    float depth = Tent(b.r0, b.r1, b.r2, px, py);
                    float hw = m.Bilinear(tu, tv, [](float d) { return d; });
                    CHECK(std::fabs(depth - hw) < 1e-4f, "depth %f vs bilinear %f at (%f, %f) tap (%d, %d)", depth, hw, u, v, ox, oy);
                    auto lit = [&](float d) { return d >= ref ? 1.0f : 0.0f; };
                    V3 c0{ lit(b.r0.x), lit(b.r0.y), lit(b.r0.z) }, c1{ lit(b.r1.x), lit(b.r1.y), lit(b.r1.z) },
                        c2{ lit(b.r2.x), lit(b.r2.y), lit(b.r2.z) };
                    float pcf = Tent(c0, c1, c2, px, py), want = m.Bilinear(tu, tv, lit);
                    CHECK(std::fabs(pcf - want) < 1e-4f, "pcf %f vs %f", pcf, want);
                    // The early out: the 9 tests agree -> min(n, 1) for every tap, = PCF and = the game's test of
                    // its filtered depth.
                    float n = c0.x + c0.y + c0.z + c1.x + c1.y + c1.z + c2.x + c2.y + c2.z;
                    if (n == 0.0f || n == 9.0f)
                    {
                        float early = std::fmin(n, 1.0f);
                        CHECK(std::fabs(pcf - early) < 1e-6f && lit(hw) == early, "early out %f: PCF %f, the game's test %f", early, pcf, lit(hw));
                        uniform++;
                    }
                    checked++;
                }
        }
        CHECK(checked == 36000, "%d", checked);
        CHECK(uniform > 3600 && uniform < 32400, "%d of the taps' blocks uniform", uniform);

        // An edge: depth 0 (caster) left of x = 8, 1 right; receiver at 0.5. The game's 9 tests on filtered depth
        // give a step of 3 levels across one texel at most; PCF ramps over ~2 texels.
        Map e{ 16, 16, {} };
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++)
                e.d.push_back(x < 8 ? 0.0f : 1.0f);
        float prevPcf = -1.0f, maxGameJump = 0.0f, maxPcfJump = 0.0f, prevGame = -1.0f;
        std::set<int> gameLevels, pcfLevels;
        for (int s = 0; s <= 64; s++)
        {
            float u = (6.0f + 4.0f * s / 64.0f) / 16.0f, v = 0.5f;
            Block b = Gathered(e, u, v);
            float game = 0.0f, pcf = 0.0f;
            for (int oy = -1; oy <= 1; oy++)
                for (int ox = -1; ox <= 1; ox++)
                {
                    float tu = u + 0.5f * ox / 16.0f, tv = v + 0.5f * oy / 16.0f;
                    game += e.Bilinear(tu, tv, [](float d) { return d; }) >= 0.5f ? 1.0f : 0.0f;
                    auto lit = [](float d) { return d >= 0.5f ? 1.0f : 0.0f; };
                    V3 c0{ lit(b.r0.x), lit(b.r0.y), lit(b.r0.z) }, c1{ lit(b.r1.x), lit(b.r1.y), lit(b.r1.z) },
                        c2{ lit(b.r2.x), lit(b.r2.y), lit(b.r2.z) };
                    pcf += Tent(c0, c1, c2, b.ax + 0.5f * ox, b.ay + 0.5f * oy);
                }
            CHECK(pcf >= prevPcf - 1e-5f, "PCF not monotonic across the edge at step %d", s);
            if (s > 0)
            {
                maxGameJump = std::fmax(maxGameJump, game - prevGame);
                maxPcfJump = std::fmax(maxPcfJump, pcf - prevPcf);
            }
            prevPcf = pcf;
            prevGame = game;
            gameLevels.insert(int(std::lround(game)));
            pcfLevels.insert(int(std::lround(pcf * 8.0f)));
        }
        CHECK(gameLevels.size() == 4, "the game's tests across an edge: %zu levels (0, 3, 6, 9)", gameLevels.size());
        CHECK(pcfLevels.size() > 24, "PCF across an edge: %zu levels of 1/8", pcfLevels.size());
        // 4 texels in 64 steps: the game jumps by 3 of 9 at once, PCF by well under one.
        CHECK(maxGameJump == 3.0f && maxPcfJump < 0.75f, "jumps across an edge: game %.2f, PCF %.2f", maxGameJump, maxPcfJump);
    }

    // ---- The translator on the game's shaders.
    std::vector<uint32_t> Load(const std::filesystem::path& p)
    {
        std::ifstream f(p, std::ios::binary);
        std::vector<uint32_t> w;
        uint32_t x;
        while (f.read(reinterpret_cast<char*>(&x), 4))
            w.push_back(__builtin_bswap32(x));  // dumped as the guest's big-endian words
        return w;
    }

    gpu::TranslatedShader Translate(const std::vector<uint32_t>& w, int pcf, int early = 1)
    {
        gpu::TranslateOptions o;
        o.shadowPcf = pcf;
        o.shadowPcfEarly = early;
        return gpu::TranslateShader(gpu::ShaderStage::Pixel, w.data(), w.size(), o);
    }

    size_t Count(const std::string& s, const std::string& what)
    {
        size_t n = 0;
        for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + 1))
            n++;
        return n;
    }

    // The instructions in program order: (address, is fetch).
    std::vector<std::pair<uint32_t, bool>> Instructions(const std::vector<uint32_t>& w)
    {
        uint32_t bound = uint32_t(w.size() / 3);
        for (uint32_t i = 0; i < bound; i++)
        {
            ControlFlowInstruction ab[2];
            UnpackControlFlowInstructions(w.data() + i * 3, ab);
            for (auto& c : ab)
                if (IsControlFlowOpcodeExec(c.opcode()))
                    bound = std::min(bound, c.exec.address());
        }
        std::vector<std::pair<uint32_t, bool>> list;
        for (uint32_t i = 0; i < bound; i++)
        {
            ControlFlowInstruction ab[2];
            UnpackControlFlowInstructions(w.data() + i * 3, ab);
            for (auto& c : ab)
                if (IsControlFlowOpcodeExec(c.opcode()))
                {
                    uint32_t seq = c.exec.sequence();
                    for (uint32_t k = 0; k < c.exec.count(); k++, seq >>= 2)
                        list.push_back({ c.exec.address() + k, (seq & 1) != 0 });
                }
        }
        return list;
    }

    // Flips the one bit of instruction `addr` that turns `before` into a different `after(word)` reading
    // (found by trying each bit, so no field layout is assumed here). False if none does.
    bool Mutate(std::vector<uint32_t>& w, uint32_t addr, const std::function<bool(const uint32_t*)>& wanted)
    {
        for (uint32_t bit = 0; bit < 96; bit++)
        {
            std::vector<uint32_t> m = w;
            m[size_t(addr) * 3 + bit / 32] ^= 1u << (bit % 32);
            if (wanted(m.data() + size_t(addr) * 3))
            {
                w = std::move(m);
                return true;
            }
        }
        return false;
    }

    int TestShaders(const std::filesystem::path& dir, const std::filesystem::path& outDir)
    {
        if (!std::filesystem::is_directory(dir))
        {
            fprintf(stderr, "no shader folder %s: the game's shaders part skipped\n", dir.string().c_str());
            return 77;
        }
        // The receivers (issues/90/shadow-quality/STEP1.md): 9 taps each, 9a81af49 two groups of 9.
        const std::map<std::string, uint32_t> receivers = {
            { "42220e24e11f5220", 1 }, { "44c366e23ae5d695", 1 }, { "48553c3b04eb9b2f", 1 }, { "672e3ab309ec00b0", 1 },
            { "6c105e8dfb4c5220", 1 }, { "781d5cda69b100b2", 1 }, { "793a572b6da508d4", 1 }, { "90a9b04dbce54afb", 1 },
            { "9a81af49c72455cc", 2 }, { "a0910baf65176645", 1 }, { "d3f6eaa9311f5220", 1 }, { "e53b1f5c8a7bdaa0", 1 },
            { "eb525efa9f5f9bd9", 1 },
        };
        std::filesystem::create_directories(outDir);
        size_t pixelShaders = 0, rewritten = 0;
        for (const auto& entry : std::filesystem::directory_iterator(dir))
        {
            std::string name = entry.path().filename().string();
            if (name.rfind("ps_", 0) != 0 || entry.path().extension() != ".bin")
                continue;
            pixelShaders++;
            std::string hash = name.substr(3, 16);
            auto w = Load(entry.path());
            auto off = Translate(w, 0), on = Translate(w, 1), full = Translate(w, 1, 0), gated = Translate(w, 2),
                 plain = gpu::TranslateShader(gpu::ShaderStage::Pixel, w.data(), w.size(), {});
            CHECK(off.ok && on.ok && gated.ok, "%s: %s / %s / %s", hash.c_str(), off.error.c_str(), on.error.c_str(), gated.error.c_str());
            CHECK(off.shadowPcfGroups == 0 && off.glsl.find("spcf") == std::string::npos, "%s: rewritten with the option off", hash.c_str());
            auto it = receivers.find(hash);
            if (it == receivers.end())
            {
                CHECK(on.shadowPcfGroups == 0 && on.glsl == off.glsl && gated.glsl == off.glsl, "%s: not a receiver, but its GLSL changed (%u groups)",
                    hash.c_str(), on.shadowPcfGroups);
                continue;
            }
            rewritten++;
            uint32_t groups = it->second;
            CHECK(on.shadowPcfGroups == groups, "%s: %u groups, want %u", hash.c_str(), on.shadowPcfGroups, groups);
            CHECK(on.shadowPcfTaps == 9 * groups, "%s: %u taps", hash.c_str(), on.shadowPcfTaps);
            CHECK(on.shadowPcfCompares >= groups && on.shadowPcfCompares <= 9 * groups, "%s: %u compares", hash.c_str(), on.shadowPcfCompares);
            CHECK(Count(on.glsl, "textureGather(") == 4 * groups, "%s: %zu gathers", hash.c_str(), Count(on.glsl, "textureGather("));
            size_t pcfs = Count(on.glsl, "= spcfW(sc0, sc1, sc2, ") + Count(on.glsl, "= spcfW(step(");
            CHECK(pcfs == 9 * groups, "%s: %zu PCF compares", hash.c_str(), pcfs);
            // The early out: one per block test (every receiver tests against one reference), each giving the same
            // components as its weights; the off form has none and is otherwise the same.
            size_t tests = Count(on.glsl, "vec3 sc0 = step("), earlies = Count(on.glsl, "if (sn == 0.0 || sn == 9.0)");
            CHECK(tests > 0 && earlies == tests && Count(on.glsl, "= sv;") == pcfs, "%s: %zu block tests, %zu early outs, %zu early taps",
                hash.c_str(), tests, earlies, Count(on.glsl, "= sv;"));
            CHECK(full.glsl.find("sn ==") == std::string::npos && Count(full.glsl, "vec3 sc0 = step(") == tests &&
                    Count(full.glsl, "= spcfW(sc0, sc1, sc2, ") + Count(full.glsl, "= spcfW(step(") == pcfs &&
                    full.shadowPcfCompares == on.shadowPcfCompares && Count(full.glsl, "textureGather(") == 4 * groups,
                "%s: the early out's off form", hash.c_str());
            CHECK(Count(on.glsl, "shadowPcfOn(") == 0 && on.glsl.find("pc.flags & 16u") == std::string::npos, "%s: gated", hash.c_str());
            // The taps' 9 samples are gone (the gathers replace them); nothing else changed.
            CHECK(Count(off.glsl, "texture(t2d_") == Count(on.glsl, "texture(t2d_") + 9 * groups, "%s: samples %zu -> %zu", hash.c_str(),
                Count(off.glsl, "texture(t2d_"), Count(on.glsl, "texture(t2d_"));
            // NFSMW_SHADOW_AB's form: the gate per group, the game's own samples behind it.
            CHECK(gated.shadowPcfGroups == groups && Count(gated.glsl, "textureGather(") == 4 * groups, "%s: gated groups", hash.c_str());
            CHECK(Count(gated.glsl, "shadowPcfOn(") == groups + 1, "%s: the gate per group", hash.c_str());
            CHECK(Count(gated.glsl, "else tdata = texture(t2d_") == 9 * groups, "%s: fallback samples %zu", hash.c_str(),
                Count(gated.glsl, "else tdata = texture(t2d_"));
            CHECK(Count(off.glsl, "texture(t2d_") == Count(gated.glsl, "texture(t2d_"), "%s: gated: other fetches changed", hash.c_str());
            CHECK(off.texture2DMask == on.texture2DMask && off.constUsed[0] == on.constUsed[0] && off.constUsed[1] == on.constUsed[1] &&
                    off.colorTargetMask == on.colorTargetMask && off.registerCount == on.registerCount,
                "%s: interface changed", hash.c_str());
            CHECK(plain.glsl == (gpu::ShadowPcfEarlyOut() ? on.glsl : full.glsl),
                "%s: the default translation is not the PCF one", hash.c_str());
            // For glslangValidator: the defines the renderer's pipeline adds (one colour output, every input).
            std::string defines = "#define OUT0\n";
            for (int i = 0; i < 16; i++)
                defines += "#define IN" + std::to_string(i) + "\n";
            for (auto [form, glsl] : { std::pair{ "", &on.glsl }, std::pair{ "_full", &full.glsl }, std::pair{ "_ab", &gated.glsl } })
            {
                std::string src = *glsl;
                size_t eol = src.find('\n');
                src.insert(eol + 1, defines);
                std::ofstream(outDir / ("ps_" + hash + form + ".frag")) << src;
            }
        }
        CHECK(pixelShaders >= receivers.size(), "only %zu pixel shaders in %s", pixelShaders, dir.string().c_str());
        CHECK(rewritten == receivers.size(), "%zu of %zu receivers found", rewritten, receivers.size());

        // Mutations of 793a572b (taps at 27-35, compares in the next clause) that break the pattern.
        auto base = Load(dir / "ps_793a572b6da508d4.bin");
        CHECK(Translate(base, 1).shadowPcfGroups == 1, "793a572b: the unmutated shader");
        std::vector<uint32_t> taps, compares;
        for (auto [addr, fetch] : Instructions(base))
        {
            const uint32_t* ins = base.data() + size_t(addr) * 3;
            if (fetch)
            {
                const auto& f = *reinterpret_cast<const FetchInstruction*>(ins);
                if (f.opcode() == FetchOpcode::kTextureFetch && f.texture_fetch().fetch_constant_index() == 4)
                    taps.push_back(addr);
            }
            else if (reinterpret_cast<const AluInstruction*>(ins)->vector_opcode() == AluVectorOpcode::kSge)
                compares.push_back(addr);
        }
        CHECK(taps.size() == 9, "793a572b: %zu taps of slot 4", taps.size());
        CHECK(compares.size() == 3, "793a572b: %zu sge", compares.size());
        auto expectUntouched = [&](const std::vector<uint32_t>& w, const char* what) {
            auto on = Translate(w, 1), off = Translate(w, 0);
            CHECK(on.ok, "%s: %s", what, on.error.c_str());
            CHECK(on.shadowPcfGroups == 0 && on.glsl == off.glsl, "%s: still rewritten", what);
        };
        if (taps.size() == 9 && compares.size() == 3)
        {
            auto tex = [](const uint32_t* p) { return reinterpret_cast<const FetchInstruction*>(p)->texture_fetch(); };
            auto alu = [](const uint32_t* p) { return reinterpret_cast<const AluInstruction*>(p); };
            {
                auto m = base;
                float ox = tex(m.data() + taps[4] * 3).offset_x();
                CHECK(Mutate(m, taps[4], [&](const uint32_t* p) {
                    auto t = tex(p);
                    return t.opcode() == FetchOpcode::kTextureFetch && t.offset_x() != ox && t.offset_x() != 0.5f && t.offset_x() != -0.5f &&
                        t.offset_x() != 0.0f && t.offset_y() == tex(base.data() + taps[4] * 3).offset_y() && t.fetch_constant_index() == 4;
                }), "no offset bit found");
                expectUntouched(m, "a tap's offset off the grid");
            }
            {
                auto m = base;
                CHECK(Mutate(m, taps[2], [&](const uint32_t* p) {
                    auto t = tex(p);
                    return t.opcode() == FetchOpcode::kTextureFetch && t.fetch_constant_index() != 4 &&
                        t.offset_x() == tex(base.data() + taps[2] * 3).offset_x();
                }), "no slot bit found");
                expectUntouched(m, "a tap from another slot");
            }
            {
                auto m = base;
                CHECK(Mutate(m, compares[1], [&](const uint32_t* p) {
                    auto v = alu(p)->vector_opcode();
                    return v != AluVectorOpcode::kSge && GetAluVectorOpcodeInfo(v).changed_state == 0 &&
                        alu(p)->GetVectorOpResultWriteMask() == alu(base.data() + compares[1] * 3)->GetVectorOpResultWriteMask();
                }), "no other opcode found");
                expectUntouched(m, "a compare that isn't sge");
            }
            {
                auto m = base;
                CHECK(Mutate(m, compares[0], [&](const uint32_t* p) {
                    return alu(p)->vector_opcode() == AluVectorOpcode::kSge && alu(p)->src_negate(1);
                }), "no negate bit found");
                expectUntouched(m, "a negated tap operand");
            }
        }
        return 0;
    }

}

int main(int argc, char** argv)
{
    TestModel();
    int shaders = 0;
    if (argc >= 3)
        shaders = TestShaders(argv[1], argv[2]);
    if (s_failures)
    {
        fprintf(stderr, "%d failure(s)\n", s_failures);
        return 1;
    }
    if (shaders == 77)
        return 77;
    printf("shadow_pcf_test: OK\n");
    return 0;
}
