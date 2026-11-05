// SpeedBreaker tests. GPL-3.0-or-later (see COPYING).
//
// Settings > Graphics > Sharper UI (gpu/sharp_ui.h) and Movie Upscaling
// (video/upscale.h):
//   - which front-end draws take the reconstruction, by their blend
//     (premultiplied, straight, or left to the hardware), and the variant's
//     key bits;
//   - sharpTex2D's rule (magnified, linearly filtered, addressable, base
//     level) and its CPU model: a flat texture and every texel centre come
//     back exactly, an edge never overshoots the texels it lies between, a
//     glyph's transparent surround can't darken it, a transparent sample is
//     the hardware's, and an edge gets steeper than bilinear's;
//   - the GLSL: the same weights as the model, the hardware's fetch first;
//     written to a fragment shader with stand-ins for the translator's
//     declarations, both variants (compiled after this by glslangValidator
//     where it is installed);
//   - the wiring: gpu/shader_translator.cpp uses sharpTex2D only for the
//     variant's pixel-shader 2D fetches; gpu/renderer.cpp keys it per draw
//     (a scaled front-end draw with the setting on, the plain pipeline
//     while the variant compiles) and keeps the plain GLSL;
//   - Movie Upscaling: Sharper only while a movie plays at 1x and is
//     enlarged (EASU, or Lanczos without FSR 1), every other frame the
//     Upscaling setting's pass; the presenter's use of it and the scale
//     shaders' Lanczos mode.
// Arguments: runtime (the sources read), and a folder the GLSL is written to.
//
// Build by hand (from the repo root):
//   c++ -std=c++20 -O2 -UNDEBUG -I runtime tests/sharp_ui_test.cpp -o build/sharp_ui_test
//   build/sharp_ui_test runtime build/sharp_ui_glsl
#include <gpu/sharp_ui.h>
#include <video/fsr1.h>
#include <video/upscale.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
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

    namespace ui = gpu::sharp_ui;
    using ui::Rgba;

    std::string Read(const std::filesystem::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        return text.str();
    }

    // A small texture, clamped to its edges as the front end's atlases are.
    struct Image
    {
        int w, h;
        std::vector<Rgba> px;
        Rgba operator()(int x, int y) const
        {
            x = std::clamp(x, 0, w - 1);
            y = std::clamp(y, 0, h - 1);
            return px[size_t(y) * w + x];
        }
    };

    Rgba Bilinear(const Image& img, float px, float py)
    {
        const float cx = std::floor(px - 0.5f), cy = std::floor(py - 0.5f);
        const float fx = px - 0.5f - cx, fy = py - 0.5f - cy;
        const Rgba a = img(int(cx), int(cy)), b = img(int(cx) + 1, int(cy)), c = img(int(cx), int(cy) + 1), d = img(int(cx) + 1, int(cy) + 1);
        auto mix = [](float p, float q, float t) { return p + (q - p) * t; };
        return { mix(mix(a.r, b.r, fx), mix(c.r, d.r, fx), fy), mix(mix(a.g, b.g, fx), mix(c.g, d.g, fx), fy),
            mix(mix(a.b, b.b, fx), mix(c.b, d.b, fx), fy), mix(mix(a.a, b.a, fx), mix(c.a, d.a, fx), fy) };
    }

    bool Near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }
}

int main(int argc, char** argv)
{
    const std::filesystem::path rt = argc > 1 ? argv[1] : "runtime";
    const std::filesystem::path out = argc > 2 ? argv[2] : "sharp_ui_glsl";

    // Blends: SRC_ALPHA (the front end's 07060706, and additive 06/01) is
    // premultiplied; ONE or blending off straight; anything else left alone.
    CHECK(ui::ModeForBlend(0x07060706) == ui::Mode::Premultiplied, "07060706");
    CHECK(ui::ModeForBlend(0x00010106 & 0x1FFF1FFF) == ui::Mode::Premultiplied, "src alpha, dst one");
    CHECK(ui::ModeForBlend(~0u) == ui::Mode::Straight, "blending off");
    CHECK(ui::ModeForBlend(0x00010101) == ui::Mode::Straight, "one, one");
    CHECK(ui::ModeForBlend(0x00000008) == ui::Mode::Off, "dst colour (a multiply): hardware");
    CHECK(ui::ModeForBlend(0x0000070A) == ui::Mode::Off, "dst alpha: hardware");
    CHECK(ui::VariantBits(ui::Mode::Off) == 0, "Off: the plain variant's key");
    CHECK(ui::VariantBits(ui::Mode::Luma) != ui::VariantBits(ui::Mode::Straight) && (ui::VariantBits(ui::Mode::Luma) & 0x1FFFFF) == 0,
        "the movie's Y-only variant has a key of its own");
    CHECK(std::string(ui::kSharpTex2DGlsl).find("#ifdef SHARP_SLOT0_ONLY\n    if (slot != 0u)\n        return hw;") != std::string::npos,
        "Y-only: other slots return the hardware's sample");
    CHECK(ui::VariantBits(ui::Mode::Premultiplied) != ui::VariantBits(ui::Mode::Straight) &&
              (ui::VariantBits(ui::Mode::Premultiplied) & 0x1FFFFF) == 0 && (ui::VariantBits(ui::Mode::Straight) & 0x1FFFFF) == 0,
        "the variant bits are apart from each other and from outputs/inputs/alpha test (bits 0-20)");

    // The rule: magnified (rho < 1, lambda <= 0), linear, addressable, base level.
    CHECK(ui::Reconstructs(0.5f, true, 0.0f, 2, 2, 1, 0, 1), "2x magnified, clamp to edge");
    CHECK(ui::Reconstructs(0.5f, true, 0.0f, 0, 0, 1, 0, 1), "repeat");
    CHECK(!ui::Reconstructs(1.0f, true, 0.0f, 2, 2, 1, 0, 1), "1:1 is not magnified");
    CHECK(!ui::Reconstructs(1.5f, true, 0.0f, 2, 2, 1, 0, 1), "minified");
    CHECK(!ui::Reconstructs(0.5f, true, 1.5f, 2, 2, 1, 0, 1), "a bias that minifies");
    CHECK(!ui::Reconstructs(0.5f, false, 0.5f, 2, 2, 1, 0, 1), "an explicit LOD above 0");
    CHECK(ui::Reconstructs(0.5f, false, 0.0f, 2, 2, 1, 0, 1), "explicit LOD 0, magnified");
    CHECK(!ui::Reconstructs(0.5f, true, 0.0f, 2, 2, 0, 0, 1), "point filtered");
    CHECK(!ui::Reconstructs(0.5f, true, 0.0f, 1, 2, 1, 0, 1), "mirrored");
    CHECK(!ui::Reconstructs(0.5f, true, 0.0f, 2, 6, 1, 0, 1), "clamped to the border");
    CHECK(!ui::Reconstructs(0.5f, true, 0.0f, 2, 2, 1, 1, 4), "a mip floor above 0");
    CHECK(ui::Reconstructs(0.5f, true, 0.0f, 2, 2, 1, 1, 1), "a mip floor on a texture without mips");

    // Catmull-Rom's weights sum to 1; at f = 0 they are the second tap's alone.
    for (float f : { 0.0f, 0.25f, 0.5f, 0.9f })
    {
        const auto w = ui::CatmullRom(f);
        CHECK(Near(w[0] + w[1] + w[2] + w[3], 1.0f), "weights at %.2f sum to %.6f", f, w[0] + w[1] + w[2] + w[3]);
    }
    {
        const auto w = ui::CatmullRom(0.0f);
        CHECK(w[0] == 0.0f && w[1] == 1.0f && w[2] == 0.0f && w[3] == 0.0f, "f = 0: the texel itself");
    }

    // A flat texture: the same colour everywhere, either mode.
    {
        Image img{ 8, 8, std::vector<Rgba>(64, Rgba{ 0.3f, 0.6f, 0.9f, 0.7f }) };
        for (float x = 1.0f; x < 7.0f; x += 0.37f)
            for (bool pm : { false, true })
            {
                const Rgba r = ui::Reconstruct(img, x, 3.3f, pm);
                CHECK(r.a == -1.0f, "flat at %.2f (%s): the hardware's sample is kept (%f)", x, pm ? "premultiplied" : "straight", r.a);
            }
    }

    // Texel centres come back exactly (an interpolating filter: no blur, no shift).
    Image noise{ 8, 8, {} };
    for (int i = 0; i < 64; i++)
        noise.px.push_back({ float((i * 37) % 64) / 63.0f, float((i * 11) % 64) / 63.0f, float((i * 53) % 64) / 63.0f, 1.0f });
    for (int y = 1; y < 7; y++)
        for (int x = 1; x < 7; x++)
        {
            const Rgba r = ui::Reconstruct(noise, x + 0.5f, y + 0.5f, true);
            const Rgba t = noise(x, y);
            CHECK(Near(r.r, t.r) && Near(r.g, t.g) && Near(r.b, t.b), "texel %d,%d", x, y);
        }

    // Everywhere: inside the 2x2 texels around the sample (clamped: no
    // ringing or overshoot, no colour that isn't there).
    for (float y = 1.0f; y < 7.0f; y += 0.29f)
        for (float x = 1.0f; x < 7.0f; x += 0.31f)
        {
            const Rgba r = ui::Reconstruct(noise, x, y, false);
            if (r.a == -1.0f)
                continue;  // a flat 2x2: the hardware's
            const int cx = int(std::floor(x - 0.5f)), cy = int(std::floor(y - 0.5f));
            float lo = 1e9f, hi = -1e9f;
            for (int j = 0; j < 2; j++)
                for (int i = 0; i < 2; i++)
                {
                    lo = std::min(lo, noise(cx + i, cy + j).r);
                    hi = std::max(hi, noise(cx + i, cy + j).r);
                }
            CHECK(r.r >= lo - 1e-6f && r.r <= hi + 1e-6f, "%.2f,%.2f: %f outside %f..%f", x, y, r.r, lo, hi);
        }

    // A hard edge (black | white, opaque): steeper than bilinear's ramp, and
    // never past either side.
    {
        Image edge{ 8, 4, {} };
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 8; x++)
                edge.px.push_back(x < 4 ? Rgba{ 0, 0, 0, 1 } : Rgba{ 1, 1, 1, 1 });
        float sharpGap = 0.0f, linearGap = 0.0f;
        for (float x = 3.5f; x <= 4.5f; x += 0.125f)
        {
            Rgba r = ui::Reconstruct(edge, x, 2.0f, true);
            if (r.a == -1.0f)
                r = Bilinear(edge, x, 2.0f);  // flat: the hardware's
            CHECK(r.r >= 0.0f && r.r <= 1.0f, "edge at %.3f: %f", x, r.r);
            sharpGap += std::fabs(r.r - 0.5f);
            linearGap += std::fabs(Bilinear(edge, x, 2.0f).r - 0.5f);
        }
        CHECK(sharpGap > linearGap * 1.05f, "steeper than bilinear: %f vs %f", sharpGap, linearGap);
    }

    // A white glyph on transparent texels that hold black (as atlases do):
    // premultiplied, its colour stays white right to its edge; straight
    // bilinear darkens it there (the fringe premultiplying avoids).
    {
        Image glyph{ 8, 4, {} };
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 8; x++)
                glyph.px.push_back(x < 4 ? Rgba{ 0, 0, 0, 0 } : Rgba{ 1, 1, 1, 1 });
        const Rgba r = ui::Reconstruct(glyph, 4.1f, 2.0f, true);
        CHECK(r.a > 0.0f && Near(r.r, 1.0f) && Near(r.g, 1.0f) && Near(r.b, 1.0f), "glyph edge: %f %f %f a %f", r.r, r.g, r.b, r.a);
        CHECK(Bilinear(glyph, 4.1f, 2.0f).r < 0.99f, "(straight bilinear darkens it: %f)", Bilinear(glyph, 4.1f, 2.0f).r);
        const Rgba clear = ui::Reconstruct(glyph, 1.5f, 2.0f, true);
        CHECK(clear.a < 0.0f, "transparent: the hardware's sample (sentinel %f)", clear.a);
    }

    // The GLSL: the model's weights and rule, the hardware's fetch first.
    const std::string glsl = ui::kSharpTex2DGlsl;
    for (const char* piece : { "w[0] = f * (-0.5 + f * (1.0 - 0.5 * f));", "w[1] = 1.0 + f * f * (-2.5 + 1.5 * f);",
             "w[2] = f * (0.5 + f * (2.0 - 1.5 * f));", "w[3] = f * f * (-0.5 + 0.5 * f);", "vec4 r = clamp(cr, min(min(p[0], p[1]), min(p[2], p[3])), max(max(p[0], p[1]), max(p[2], p[3])));",
             "rho < 1.0 && lambda <= 0.0", "((d3 >> 19) & 3u) != 0u", "#ifndef SHARP_STRAIGHT", "r.a < 1.0 / 1024.0" })
        CHECK(glsl.find(piece) != std::string::npos, "sharpTex2D lacks: %s", piece);
    CHECK(glsl.find("vec4 hw = computedLod ? texture(t2, uv, lod) : textureLod(t2, uv, lod);") < glsl.find("if (!("),
        "the hardware's sample is taken before any branch");

    // The GLSL compiled as a fragment shader (both variants): with the
    // translator's fetch-constant reader stood in.
    std::filesystem::create_directories(out);
    for (bool straight : { false, true })
    {
        std::ofstream f(out / (straight ? "sharp_straight.frag" : "sharp_premultiplied.frag"), std::ios::binary);
        f << "#version 460\n" << (straight ? "#define SHARP_STRAIGHT\n" : "")
          << "layout(set = 0, binding = 0) uniform U { uvec4 fetch[48]; } u;\n"
             "uint fetchDword(uint slot, uint dword) { uint i = slot * 6u + dword; return u.fetch[i >> 2][i & 3u]; }\n"
             "layout(set = 1, binding = 0) uniform sampler2D t2d_0;\n"
          << glsl
          << "layout(location = 0) in vec4 i_i0;\nlayout(location = 0) out vec4 o;\n"
             "void main() { o = sharpTex2D(t2d_0, i_i0.xy, 0u, true, 0.0) + sharpTex2D(t2d_0, i_i0.zw, 0u, false, 0.0); }\n";
    }

    // The translator: sharpTex2D only for the option's pixel-shader 2D
    // fetches, its helper only when used.
    const std::string tr = Read(rt / "gpu/shader_translator.cpp");
    CHECK(tr.find("if (options.sharpFetch2D && stage == ShaderStage::Pixel && op.dimension() == xenos::FetchOpDimension::k2D)") != std::string::npos,
        "translator: sharpTex2D only for the variant's PS 2D fetches");
    CHECK(tr.find("if (result.sharpFetches)\n            g += gpu::sharp_ui::kSharpTex2DGlsl;") != std::string::npos,
        "translator: the helper only in the variant");
    CHECK(tr.find("return TranslateShader(stage, ucode, dwordCount, TranslateOptions{});") != std::string::npos,
        "translator: the plain call is the default options'");

    // The renderer: per draw, scaled targets only, the setting read where used.
    const std::string r = Read(rt / "gpu/renderer.cpp");
    const size_t candAt = r.find("const bool sharpCandidate = s_drawScaled && color[0] && s_ps->info.texture2DMask &&");
    CHECK(candAt != std::string::npos, "renderer: the candidate test (scaled target, colour, 2D textures)");
    CHECK(r.find("const bool frontEnd = sharpCandidate && IsFrontEndDraw();") != std::string::npos, "renderer: front-end draws");
    CHECK(r.find("const gpu::sharp_ui::Mode sharpMode = frontEnd ? gpu::sharp_ui::ModeForBlend(key.blend[0]) : gpu::sharp_ui::Mode::Off;") !=
            std::string::npos, "renderer: the front end's mode by its blend");
    CHECK(r.find("const gpu::sharp_ui::Mode movieMode = sharpCandidate && !frontEnd && IsMovieDraw()\n            ? gpu::sharp_ui::Mode::Luma") != std::string::npos,
        "renderer: the movie quad, scaled targets only");
    CHECK(r.find("if (movieMode != gpu::sharp_ui::Mode::Off && SharpMovieOn())") != std::string::npos, "renderer: Movie Upscaling decides");
    CHECK(r.find("return settings::GetInt(settings::Id::MovieUpscaling) == int32_t(settings::MovieUpscalingMode::Sharper);") != std::string::npos,
        "renderer: reads Movie Upscaling live");
    {
        const size_t m = r.find("bool IsMovieDraw()");
        const std::string body = m == std::string::npos ? std::string() : r.substr(m, r.find("\n        }\n", m) - m);
        CHECK(body.find("game::LastMovieFrameNs()") != std::string::npos && body.find("250'000'000") != std::string::npos &&
                  body.find("(Reg<reg::PA_CL_VTE_CNTL>().value & 0x3F) == 0") != std::string::npos,
            "IsMovieDraw: a fresh movie and a clip-space draw");
    }
    CHECK(r.find("if (sharpMode != gpu::sharp_ui::Mode::Off && SharpUiOn())\n        {\n            key.pad = uint32_t(sharpMode);") != std::string::npos,
        "renderer: the setting (or the A/B) decides, per draw");
    CHECK(r.find("return settings::GetBool(settings::Id::SharpUi);") != std::string::npos, "renderer: reads Sharper UI live");
    CHECK(r.find("if (pipelinePending && key.pad)") != std::string::npos, "renderer: the plain pipeline while the variant compiles");
    CHECK(r.find("gpu::sharp_ui::Mode(key.pad & 3)") != std::string::npos, "renderer: BuildPipeline passes the mode");
    CHECK(r.find("| gpu::sharp_ui::VariantBits(sharp) |\n                ((uiMask & 3u) << 24);") != std::string::npos, "renderer: the variant key holds the mode");
    CHECK(r.find("const std::string* glsl = &ps.info.glsl;") != std::string::npos, "renderer: the plain GLSL is the default");
    // The front-end test is the edge-bar rules' (c0 the hook's scale, no
    // rotation or projection, ePolys at fetch 95, a vertex shader reading them).
    const size_t fe = r.find("bool IsFrontEndDraw()");
    CHECK(fe != std::string::npos, "renderer: IsFrontEndDraw");
    if (fe != std::string::npos)
    {
        const std::string body = r.substr(fe, r.find("\n        }\n", fe) - fe);
        for (const char* piece : { "game::FrontEndScaleX()", "c[12] == 0.0f && c[13] == 0.0f", "c[14] == 0.0f && c[15] == 1.0f",
                 "(vf[0] & 3) == 3", "vertexFetchConstants[95 / 32]", "(vi.constUsed[0] & 0xFull) == 0xFull" })
            CHECK(body.find(piece) != std::string::npos, "IsFrontEndDraw lacks %s", piece);
        CHECK(body.find("AspectScale") == std::string::npos, "IsFrontEndDraw: every screen shape (no aspect gate)");
    }

    // Movie Upscaling.
    namespace up = video::upscale;
    const int32_t bic = int32_t(up::Mode::Bicubic), fsr = int32_t(up::Mode::Fsr1), lin = int32_t(up::Mode::Linear);
    const int32_t sharper = int32_t(up::MovieMode::Sharper), original = int32_t(up::MovieMode::Original);
    CHECK(up::ChooseWithMovie(bic, sharper, true, true, true, 1280, 720, 3840, 2160) == up::Pass::Fsr1, "a movie at 4K: EASU");
    CHECK(up::ChooseWithMovie(bic, sharper, true, true, false, 1280, 720, 2796, 1290) == up::Pass::Lanczos, "no FSR 1 (iPhone): Lanczos");
    CHECK(up::ChooseWithMovie(bic, original, true, true, true, 1280, 720, 3840, 2160) == up::Pass::Bicubic, "Original: Upscaling's");
    CHECK(up::ChooseWithMovie(lin, original, true, true, true, 1280, 720, 3840, 2160) == up::Pass::Linear, "Original: Upscaling's (bilinear)");
    CHECK(up::ChooseWithMovie(fsr, original, true, true, true, 1280, 720, 3840, 2160) == up::Pass::Fsr1, "Original: Upscaling's (FSR 1)");
    CHECK(up::ChooseWithMovie(bic, sharper, false, true, true, 1280, 720, 3840, 2160) == up::Pass::Bicubic, "no movie: Upscaling's");
    CHECK(up::ChooseWithMovie(bic, sharper, true, false, true, 2560, 1440, 3840, 2160) == up::Pass::Bicubic, "a scaled frame: Upscaling's");
    CHECK(up::ChooseWithMovie(bic, sharper, true, true, true, 1280, 720, 1280, 720) == up::Pass::Bicubic, "not enlarged: Upscaling's");
    CHECK(up::ChooseWithMovie(bic, sharper, true, true, true, 1280, 720, 1152, 648) == up::Pass::Bicubic, "shrunk: Upscaling's");
    CHECK(up::ChooseWithMovie(bic, 7, true, true, true, 1280, 720, 3840, 2160) == up::Pass::Bicubic, "an unknown mode: Original");
    CHECK(up::MoviePlaying(1000, 1000 + up::kMovieFreshNs) && !up::MoviePlaying(1000, 1001 + up::kMovieFreshNs), "fresh for kMovieFreshNs");
    CHECK(!up::MoviePlaying(0, 5) && !up::MoviePlaying(1000, 999), "no movie yet; a clock that went back");
    CHECK(up::AtOneX(1280, 720, 1280, 720) && up::AtOneX(1280, 720, 0, 0) && !up::AtOneX(2560, 1440, 1280, 720), "1x: the frame's own size");
    CHECK(up::kMovieRcas == 0.0f, "EASU alone for movies: RCAS at %.2f", up::kMovieRcas);
    CHECK(std::string(up::PassName(up::Pass::Lanczos)) == "Lanczos", "Lanczos named");

    const std::string pr = Read(rt / "video/presenter.cpp");
    CHECK(pr.find("upscale::ChooseWithMovie(settings::GetInt(settings::Id::Scaling), movieMode, movie,") != std::string::npos,
        "presenter: the frame's pass with Movie Upscaling");
    CHECK(pr.find("upscale::MoviePlaying(game::LastMovieFrameNs(), NowNs())") != std::string::npos, "presenter: the movie clock");
    CHECK(pr.find("sharpen = upscale::kMovieRcas;") != std::string::npos, "presenter: the movie's RCAS strength");
    CHECK(pr.find("pass == upscale::Pass::Lanczos ? 2u : 0u") != std::string::npos, "presenter: the Lanczos mode pushed");
    size_t lanczosModes = 0;
    for (size_t at = pr.find("if (p.bicubic == 2u)"); at != std::string::npos; at = pr.find("if (p.bicubic == 2u)", at + 1))
        lanczosModes++;
    CHECK(lanczosModes == 2, "both scale shaders have the Lanczos mode (%zu)", lanczosModes);

    // UI-C: the mask's kinds; only the variant writes it; the presenter's
    // pass discards where it is 0, and applies only to a non-integer
    // bicubic enlargement.
    CHECK(ui::UiMaskKind(0x07060706, false) == 1 && ui::UiMaskKind(0x00010101, false) == 2 && ui::UiMaskKind(~0u, false) == 3,
        "mask kinds: alpha, brightest channel, whole");
    CHECK(ui::UiMaskKind(0x07060706, true) == 0, "no mask from draws over small textures only (the white fill)");
    CHECK(ui::UiMaskKind(0x00000008, false) == 0, "no mask from blends Sharper UI leaves alone");
    CHECK(std::string(ui::kUiMaskStoreGlsl).find("imageAtomicMax(sb_uiMask, sbAt, sbValue);") != std::string::npos, "the atomic max");
    CHECK(tr.find("g.replace(at, strlen(\"@@UI_MASK@@\"), options.sharpFetch2D ? gpu::sharp_ui::kUiMaskStoreGlsl : \"\");") != std::string::npos,
        "translator: the mask write only in the variant");
    CHECK(r.find("key.pad |= gpu::sharp_ui::UiMaskKind(key.blend[0], small) << 2;") != std::string::npos, "renderer: the mask kind in the key");
    CHECK(r.find("if (scaled && !b && settings::GetBool(settings::Id::SharpUi) && UiMaskWanted())") != std::string::npos,
        "renderer: a mask for scaled real pictures with Sharper UI on");
    CHECK(pr.find("return uint32_t(dw) % w != 0 || uint32_t(dh) % h != 0;") != std::string::npos, "presenter: integer enlargements untouched");
    CHECK(pr.find("pass != upscale::Pass::Bicubic || s_swapHdr") != std::string::npos, "presenter: bicubic only, no HDR (bands followed)");
    {
        const std::string g = video::fsr1::UiEasuGlsl();
        CHECK(g.find("if (w <= 0.0)\n        discard;") != std::string::npos, "UI pass: outside the mask it draws nothing");
        CHECK(g.find("color = vec4(c, w);") != std::string::npos, "UI pass: EASU at the mask's weight");
        CHECK(g.find("return texelFetch(src, clamp(t, spanLo, spanHi), 0).rgb;") != std::string::npos &&
                  g.find("p.bandOn == 0u ? textureGather(src, q, 0) : quadIn(q, 0)") != std::string::npos,
            "UI pass: under Hor+/Vert+ its taps stay in the pixel's band span (no seam at the band's edge)");
        std::ofstream(out / "ui_easu.frag", std::ios::binary) << g;
    }

    if (s_failures)
    {
        printf("%d check(s) FAILED\n", s_failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
