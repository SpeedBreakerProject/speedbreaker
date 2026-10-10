// Tests for gpu/depth24_glsl.h: the resolve shader's 24-bit depth pack
// (formats 22 and 23, k_24_8 and k_24_8_FLOAT) and its C++ mirror. Until
// d24-fix the pack was `unorm(c.r, 16777215.0) << 8`, which wrapped depth
// 1.0 (the cleared far plane) to 0: every texel of the sun's shadow map
// without a caster read "nearest", and roads lost their sunlight. From the
// repo root:
//   clang++ -std=c++20 -O2 -Iruntime tests/depth24_pack_test.cpp -o build/depth24_pack_test
//   build/depth24_pack_test runtime/gpu/renderer.cpp
// PackDepth24 evaluates the shader's own tokens (SB_DEPTH24_PACK_EXPR), so
// what is checked here is the expression the GPU compiles. With
// renderer.cpp's path (ctest passes it) it also checks that the resolve
// shader, rebuilt from its literal and comments left out, defines the
// helper and packs formats 22 and 23 with it.
#include <gpu/depth24_glsl.h>
#include <gpu/shared_memory_glsl.h>

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <utility>
#include <sstream>
#include <string>

using gpu::Depth24Rounding;
using gpu::PackDepth24;
using gpu::UnpackDepth24;

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const Depth24Rounding kRoundings[] = { Depth24Rounding::Separate, Depth24Rounding::Fused };
static const char* Name(Depth24Rounding r) { return r == Depth24Rounding::Fused ? "fma" : "separate"; }

// The released pack (renderer.cpp up to v0.1.0): the same arithmetic, no
// clamp to 24 bits, shifted into the word.
static uint32_t ReleasedWord(float v, Depth24Rounding rounding)
{
    float c = std::fmin(std::fmax(v, 0.0f), 1.0f);
    float sum;
    if (rounding == Depth24Rounding::Fused)
        sum = std::fmaf(c, 16777215.0f, 0.5f);
    else
    {
        volatile float product = c * 16777215.0f;
        sum = product + 0.5f;
    }
    return uint32_t(sum) << 8;
}

static float FromBits(uint32_t bits)
{
    float v;
    memcpy(&v, &bits, 4);
    return v;
}

static void TestFarPlane()
{
    printf("far plane (1.0)\n");
    for (Depth24Rounding r : kRoundings)
    {
        CHECK(PackDepth24(1.0f, r) == 0xFFFFFFu, "%s: 1.0 packs to %06X", Name(r), PackDepth24(1.0f, r));
        CHECK(uint32_t(PackDepth24(1.0f, r) << 8) == 0xFFFFFF00u, "%s: word %08X", Name(r), uint32_t(PackDepth24(1.0f, r) << 8));
        CHECK(UnpackDepth24(PackDepth24(1.0f, r)) == 1.0f, "%s: reads back as %.9g", Name(r), UnpackDepth24(PackDepth24(1.0f, r)));
        // The bug this guards: the arithmetic without the clamp wraps 1.0 to
        // the word 0 (nearest) either way. If this ever stops holding, the
        // mirror no longer models the shader's arithmetic.
        CHECK(ReleasedWord(1.0f, r) == 0u, "%s: the unclamped pack gave %08X, not the wrap to 0", Name(r), ReleasedWord(1.0f, r));
    }
    CHECK(PackDepth24(1.0f) == 0xFFFFFFu, "default rounding: %06X", PackDepth24(1.0f));
}

// Every binary32 in [0.5, 1] (the top of the range, where 24 bits can
// overflow): at most 0xFFFFFF, never decreasing, within 1 LSB of v * (2^24 - 1).
static void TestTopHalf()
{
    printf("every binary32 in [0.5, 1]\n");
    const uint32_t first = 0x3F000000u, last = 0x3F800000u;  // 0.5, 1.0
    for (Depth24Rounding r : kRoundings)
    {
        uint64_t over = 0, decreasing = 0, off = 0, released = 0;
        uint32_t previous = 0, firstBad = 0;
        double worst = 0;
        for (uint32_t bits = first; bits <= last; bits++)
        {
            float v = FromBits(bits);
            uint32_t d = PackDepth24(v, r);
            double exact = double(v) * 16777215.0;  // exact: 24 x 24 significant bits
            double error = std::fabs(double(d) - exact);
            worst = std::max(worst, error);
            bool bad = false;
            if (d > 0xFFFFFFu)
                over++, bad = true;
            if (bits > first && d < previous)
                decreasing++, bad = true;
            if (error > 1.0)
                off++, bad = true;
            if (bad && !firstBad)
                firstBad = bits;
            released += (ReleasedWord(v, r) >> 8) != d;
            previous = d;
        }
        printf("  %s: %u values, worst error %.8f LSB; %llu differ from the released pack\n", Name(r), last - first + 1, worst,
            (unsigned long long)released);
        CHECK(over == 0 && decreasing == 0 && off == 0, "%s: %llu above 0xFFFFFF, %llu decreasing, %llu more than 1 LSB off (first at %.9g)",
            Name(r), (unsigned long long)over, (unsigned long long)decreasing, (unsigned long long)off, FromBits(firstBad));
        // Only 1.0 changed: the clamp touches nothing else.
        CHECK(released == 1, "%s: %llu values pack differently from the released arithmetic, expected 1 (1.0)", Name(r),
            (unsigned long long)released);
    }
}

static void TestZeroAndBelow()
{
    printf("0, negatives, and outside [0, 1]\n");
    const float inf = std::numeric_limits<float>::infinity();
    const float low[] = { 0.0f, -0.0f, -FLT_TRUE_MIN, -FLT_MIN, -1e-30f, -0.25f, -0.5f, -1.0f, -2.0f, -FLT_MAX, -inf,
        FLT_TRUE_MIN, FLT_MIN, 1e-30f, 1e-8f };
    const float high[] = { std::nextafter(1.0f, 2.0f), 1.5f, 2.0f, 1e30f, FLT_MAX, inf };
    for (Depth24Rounding r : kRoundings)
    {
        for (float v : low)
            CHECK(PackDepth24(v, r) == 0u, "%s: %.9g packs to %06X", Name(r), v, PackDepth24(v, r));
        for (float v : high)
            CHECK(PackDepth24(v, r) == 0xFFFFFFu, "%s: %.9g packs to %06X", Name(r), v, PackDepth24(v, r));
        CHECK(PackDepth24(std::numeric_limits<float>::quiet_NaN(), r) == 0u, "%s: NaN packs to %06X", Name(r),
            PackDepth24(std::numeric_limits<float>::quiet_NaN(), r));
    }
}

// Every 24-bit value, read as the untile reads it (float(d) / 16777215) and
// resolved again: within 1 LSB, and the ends exact.
static void TestRoundTrip()
{
    printf("every 24-bit value round-trips\n");
    for (Depth24Rounding r : kRoundings)
    {
        uint64_t offByOne = 0, worse = 0;
        uint32_t firstWorse = 0;
        for (uint32_t k = 0; k <= 0xFFFFFFu; k++)
        {
            uint32_t back = PackDepth24(UnpackDepth24(k), r);
            uint32_t diff = back > k ? back - k : k - back;
            offByOne += diff == 1;
            if (diff > 1 && !worse++)
                firstWorse = k;
        }
        printf("  %s: %llu of 16777216 off by one, %llu by more\n", Name(r), (unsigned long long)offByOne, (unsigned long long)worse);
        CHECK(worse == 0, "%s: %llu values more than 1 LSB off (first %06X -> %06X)", Name(r), (unsigned long long)worse, firstWorse,
            PackDepth24(UnpackDepth24(firstWorse), r));
        CHECK(PackDepth24(UnpackDepth24(0), r) == 0u, "%s: 0 came back as %06X", Name(r), PackDepth24(UnpackDepth24(0), r));
        CHECK(PackDepth24(UnpackDepth24(0xFFFFFFu), r) == 0xFFFFFFu, "%s: 0xFFFFFF came back as %06X", Name(r),
            PackDepth24(UnpackDepth24(0xFFFFFFu), r));
    }
}

// GLSL text without its comments (GLSL has no string literals): a line
// commented out, or an expression quoted in a comment, doesn't count.
static std::string StripGlslComments(const std::string& text)
{
    std::string out;
    for (size_t i = 0; i < text.size();)
    {
        if (text.compare(i, 2, "//") == 0)
            i = std::min(text.find('\n', i), text.size());
        else if (text.compare(i, 2, "/*") == 0)
        {
            size_t end = text.find("*/", i + 2);
            i = end == std::string::npos ? text.size() : end + 2;
            out += ' ';
        }
        else
            out += text[i++];
    }
    return out;
}

static uint32_t Count(const std::string& text, const std::string& what)
{
    uint32_t n = 0;
    for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1))
        n++;
    return n;
}

// Each line of `text` that is exactly `line` once surrounding blanks are
// trimmed.
static uint32_t CountLines(const std::string& text, const std::string& line)
{
    uint32_t n = 0;
    std::istringstream in(text);
    for (std::string l; std::getline(in, l);)
    {
        size_t a = l.find_first_not_of(" \t\r"), b = l.find_last_not_of(" \t\r");
        n += a != std::string::npos && l.compare(a, b - a + 1, line) == 0;
    }
    return n;
}

// The GLSL text: packDepth24 is defined once, outside comments, with the
// body PackDepth24 evaluates (SB_DEPTH24_PACK_EXPR's tokens, so the two
// can't differ; GLSL_DEPTH24_PACK_DEFINITION is that line).
static void TestGlslText()
{
    printf("GLSL text\n");
    const std::string definition = GLSL_DEPTH24_PACK_DEFINITION;
    printf("  %s\n", definition.c_str());
    CHECK(definition == "uint packDepth24(float v) { return " SB_DEPTH24_STRINGIFY(SB_DEPTH24_PACK_EXPR(v)) "; }",
        "the definition isn't SB_DEPTH24_PACK_EXPR's body: %s", definition.c_str());
    // The clamp, as text (the checks above test what it does).
    CHECK(definition.find("min(") != std::string::npos && definition.find("0xFFFFFFu") != std::string::npos,
        "no clamp to 0xFFFFFF in %s", definition.c_str());
    const std::string code = StripGlslComments(GLSL_DEPTH24_PACK);
    CHECK(CountLines(code, definition) == 1, "GLSL_DEPTH24_PACK doesn't define packDepth24 on a line of its own, outside comments:\n%s",
        GLSL_DEPTH24_PACK);
    CHECK(Count(code, "packDepth24") == 1, "%u packDepth24 outside comments in GLSL_DEPTH24_PACK, expected 1:\n%s",
        Count(code, "packDepth24"), GLSL_DEPTH24_PACK);
}

// The GLSL that `name`'s literal in renderer.cpp's source makes: its raw
// string pieces, and the macros between them replaced by their text. A
// macro this test doesn't include (another branch's splice) is left out,
// and named.
static bool ShaderText(const std::string& src, const char* name, std::string& glsl)
{
    std::string start = std::string("const char* ") + name + " =";
    size_t at = src.find(start);
    CHECK(at != std::string::npos, "no %s", name);
    if (at == std::string::npos)
        return false;
    at += start.size();
    const std::pair<const char*, const char*> splices[] = {
        { "GLSL_DEPTH24_PACK", GLSL_DEPTH24_PACK },
        { "GLSL_SHARED_MEMORY_WRITE", GLSL_SHARED_MEMORY_WRITE },
        { "GLSL_SHARED_MEMORY_READ", GLSL_SHARED_MEMORY_READ },
    };
    while (true)
    {
        at = src.find_first_not_of(" \t\r\n", at);
        if (at == std::string::npos)
            break;
        if (src[at] == ';')
            return true;
        if (src.compare(at, 3, "R\"(") == 0)
        {
            size_t end = src.find(")\"", at + 3);
            if (end == std::string::npos)
                break;
            glsl += src.substr(at + 3, end - at - 3);
            at = end + 2;
            continue;
        }
        size_t end = src.find_first_of(" \t\r\n;", at);
        std::string word = src.substr(at, end - at);
        bool known = false, macro = !word.empty() && word.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") == std::string::npos;
        for (auto& [splice, text] : splices)
            if (word == splice)
                glsl += text, known = true;
        if (!known && macro)
            printf("  (%s splices %s, which isn't checked here)\n", name, word.c_str());
        CHECK(known || macro, "%s: '%s' is neither a raw string nor a macro", name, word.c_str());
        if (!known && !macro)
            return false;
        at = end;
    }
    CHECK(false, "%s doesn't end", name);
    return false;
}

// renderer.cpp: the resolve shader takes the helper and packs 22 and 23
// with it (every variant compiles from this one text). The text is the
// shader's own, comments left out.
static void TestRendererSource(const char* path)
{
    printf("resolve shader in %s\n", path);
    std::ifstream file(path);
    CHECK(file.good(), "can't read %s", path);
    if (!file.good())
        return;
    std::stringstream ss;
    ss << file.rdbuf();
    std::string glsl;
    if (!ShaderText(ss.str(), "kResolveGlsl", glsl))
        return;
    const std::string code = StripGlslComments(glsl);
    CHECK(CountLines(code, GLSL_DEPTH24_PACK_DEFINITION) == 1, "kResolveGlsl doesn't define packDepth24 as GLSL_DEPTH24_PACK does");
    CHECK(Count(code, "packDepth24(float") == 1, "%u packDepth24 definitions in kResolveGlsl, expected 1", Count(code, "packDepth24(float"));
    // pack32's line for 22 and 23, and nothing else packing 24 bits.
    CHECK(Count(code, "case 22u: case 23u:") == 1, "%u case 22u/23u lines in kResolveGlsl, expected 1 (pack32)",
        Count(code, "case 22u: case 23u:"));
    CHECK(CountLines(code, "case 22u: case 23u: return packDepth24(c.r) << 8;") == 1, "formats 22/23 don't pack with packDepth24");
    size_t pack32 = code.find("uint pack32(vec4 c)"), line = code.find("case 22u: case 23u:");
    CHECK(pack32 != std::string::npos && line > pack32 && code.find("uint pack16(", pack32) > line,
        "the case 22u/23u line isn't in pack32");
    // (The untile's float(v >> 8) / 16777215.0 reads depth back; packs
    // either shift the result or pass the scale to unorm.)
    CHECK(code.find("16777215.0) <<") == std::string::npos && code.find("16777215.0)<<") == std::string::npos &&
        code.find(", 16777215.0)") == std::string::npos, "an unclamped 24-bit pack is left in kResolveGlsl");
}

int main(int argc, char** argv)
{
    TestFarPlane();
    TestTopHalf();
    TestZeroAndBelow();
    TestRoundTrip();
    TestGlslText();
    if (argc > 1)
        TestRendererSource(argv[1]);
    else
        printf("(no renderer.cpp path given: its resolve shader not checked)\n");
    printf(g_failures ? "%d FAILED\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
