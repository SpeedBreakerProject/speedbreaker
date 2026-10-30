// SpeedBreaker tests. GPL-3.0-or-later (see COPYING).
//
// HDR Output's GLSL: the composite fragment shader and the frame-average
// compute shader embedded in video/presenter.cpp. Extracts both and checks
// the maths that must match video/hdr_output.h, then writes them for the
// glslangValidator checks (hdr_shader_check_*).
//
// Arguments: runtime/video/presenter.cpp and the folder to write the shaders
// to.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace
{
    int s_failures = 0;

#define CHECK(cond, ...)                                              \
    do                                                                \
    {                                                                 \
        if (!(cond))                                                  \
        {                                                             \
            s_failures++;                                             \
            fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
            fprintf(stderr, __VA_ARGS__);                             \
            fprintf(stderr, "\n");                                    \
        }                                                             \
    } while (0)

    std::string Read(const std::filesystem::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        return text.str();
    }

    // The raw string after `const char* <name> = R"(`.
    std::string Extract(const std::string& source, const std::string& name, size_t* at = nullptr)
    {
        const std::string marker = "const char* " + name + " = R\"(";
        const size_t found = source.find(marker);
        if (found == std::string::npos)
            return {};
        if (at)
            *at = found;
        const size_t start = found + marker.size();
        const size_t end = source.find(")\";", start);
        return end == std::string::npos ? std::string() : source.substr(start, end - start);
    }

    bool Has(const std::string& text, const std::string& what)
    {
        return text.find(what) != std::string::npos;
    }
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: %s runtime/video/presenter.cpp out-dir\n", argv[0]);
        return 2;
    }
    const std::string source = Read(argv[1]);
    CHECK(!source.empty(), "can't read %s", argv[1]);

    const std::string composite = Extract(source, "kHdrCompositeFs");
    const std::string average = Extract(source, "kHdrAverageCs");
    CHECK(!composite.empty(), "kHdrCompositeFs not found");
    CHECK(!average.empty(), "kHdrAverageCs not found");

    // The composite keeps the maths of video/hdr_output.h: the luminance
    // expansion, the Rec.709 -> Rec.2020 conversion, and PQ encoding.
    CHECK(Has(composite, "srgbToLinear"), "composite decodes sRGB");
    CHECK(Has(composite, "expand"), "composite expands highlights");
    CHECK(Has(composite, "pow(l, 8.0)"), "composite uses the v^8 curve");
    CHECK(Has(composite, "rec709ToRec2020"), "composite converts to Rec.2020 before PQ");
    CHECK(Has(composite, "nitsToPq"), "composite encodes PQ");
    CHECK(Has(composite, "uiLinear"), "composite blends the UI over the scene");
    CHECK(Has(composite, "p.peak"), "composite reads the peak push constant");

    // The average is the same sRGB decode and Rec.709 luma as hdr_output.h.
    CHECK(Has(average, "srgbToLinear"), "average decodes sRGB");
    CHECK(Has(average, "0.2126"), "average uses Rec.709 luma");
    CHECK(Has(average, "outCells.cells"), "average writes the cell buffer");
    CHECK(Has(average, "textureSize"), "average covers the whole scene");

    const std::filesystem::path dir = argv[2];
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    CHECK(!ec, "can't make %s: %s", dir.string().c_str(), ec.message().c_str());
    {
        std::ofstream out(dir / "hdr_composite.frag", std::ios::binary);
        out << composite << '\n';
        CHECK(out.good(), "can't write hdr_composite.frag");
    }
    {
        std::ofstream out(dir / "hdr_average.comp", std::ios::binary);
        out << average << '\n';
        CHECK(out.good(), "can't write hdr_average.comp");
    }

    if (s_failures)
        fprintf(stderr, "%d failure(s)\n", s_failures);
    return s_failures ? 1 : 0;
}
