// SpeedBreaker tests. GPL-3.0-or-later (see COPYING).
//
// The picture's tone curve (video/display_tone.h: Settings > Display >
// Brightness) and the 360's display gamma ramp as the game programs it
// (video/display_gamma.h: the DC_LUT_* register protocol), header only. The
// optional arguments are runtime/video/presenter.cpp (its tone shader wraps
// the scale shaders it uses, which must each have exactly one main) and
// runtime/user/settings.cpp (the two options' defaults: Brightness 0, Xbox 360
// Display Gamma on since v0.1.4).
//
// Build by hand: c++ -std=c++20 -O2 -UNDEBUG -I runtime tests/display_tone_test.cpp
#include <video/display_tone.h>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>

using namespace video;
namespace dg = video::display_gamma;

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

    double Levels(double v) { return v * 255.0; }

    // ---- A model of Most Wanted's (and Carbon's) Direct3D ramp, from the
    // recompiled code (the constants are the XEX's):
    //   the game (sub_82458C20): ramp16[k] = int(pow(k/255, g) * 65535 + 0.5),
    //     g = 1.06 at its Brightness 0 .. 0.57 at 1 (0.815 at its default 0.5)
    //   Direct3D (sub_825A1DA8): table[i] = int(f(i/1023) * 1023 + 0.5) for
    //     the type VdGetCurrentDisplayGamma reports: 1 sRGB encode, 2 (and
    //     others) the BT.709 curve, 3 a power, each of the sRGB decode;
    //     ramp16' = table[ramp16 >> 6] << 6
    //   the DC LUT (sub_825A20A8): DC_LUT_30_COLOR = ramp16' >> 6 per channel.
    double SrgbDecode(double x) { return x <= 0.04045 ? x * 0.07739938050508499 : std::pow((x + 0.054999999701976776) * 0.9478673338890076, 2.4000000953674316); }
    double SrgbEncode(double x) { return x <= 0.0031308 ? x * 12.92 : std::pow(x, 0.4166666567325592) * 1.0549999475479126 - 0.054999999701976776; }
    double Bt709(double x) { return x < 0.018 ? x * 4.5 : std::pow(x, 0.44999998807907104) * 1.0989999771118164 - 0.0989999994635582; }

    std::vector<uint32_t> D3dRamp10(double g, int type)
    {
        std::vector<uint32_t> table(1024), out(256);
        for (int i = 0; i < 1024; i++)
        {
            double x = SrgbDecode(i * 0.0009775171056389809);
            double y = type == 1 ? SrgbEncode(x) : type == 3 ? std::pow(x, 1.0 / 2.22222233) : Bt709(x);
            table[i] = uint32_t(std::clamp(int(y * 1023.0 + 0.5), 0, 1023));
        }
        for (int k = 0; k < 256; k++)
        {
            uint32_t ramp16 = uint32_t(std::pow(k / 255.0, g) * 65535.0 + 0.5) & 0xFFFF;
            out[k] = ((table[ramp16 >> 6] << 6) & 0xFFFF) >> 6;
        }
        return out;
    }

    // The register writes sub_825A20A8 makes for a ramp (r = g = b), as the
    // command processor hands them on.
    std::vector<std::pair<uint32_t, uint32_t>> D3dWrites(const std::vector<uint32_t>& r10, const std::vector<uint32_t>* g10 = nullptr,
        const std::vector<uint32_t>* b10 = nullptr)
    {
        std::vector<std::pair<uint32_t, uint32_t>> w = { { dg::kRegRwMode, 0 }, { dg::kRegRwIndex, 0 }, { dg::kRegWriteEnMask, 7 } };
        for (uint32_t k = 0; k < 256; k++)
        {
            uint32_t r = r10[k], g = g10 ? (*g10)[k] : r10[k], b = b10 ? (*b10)[k] : r10[k];
            w.push_back({ dg::kRegColor30, (r << 20) | (g << 10) | b });
            w.push_back({ dg::kRegRwIndex, k + 1 });  // the COND_WRITE after each entry; 0x100 at the end
        }
        return w;
    }

    // Feeds writes; returns how many completed a table.
    int Feed(dg::Decoder& d, const std::vector<std::pair<uint32_t, uint32_t>>& writes)
    {
        int completed = 0;
        for (auto [reg, value] : writes)
            completed += d.Write(reg, value) ? 1 : 0;
        return completed;
    }

    void Neutral()
    {
        CHECK(tone::Neutral(0, false), "Brightness 0 with the ramp off must be neutral");
        CHECK(!tone::Neutral(1, false) && !tone::Neutral(-1, false), "any other Brightness is not");
        CHECK(!tone::Neutral(0, true), "the 360's ramp on is not neutral");
        CHECK(tone::BrightnessExponent(0) == 1.0, "exponent at 0: %g", tone::BrightnessExponent(0));
        tone::Lut lut = tone::BuildLut(0, nullptr);
        for (uint32_t k = 0; k < 256; k++)
            for (uint32_t c = 0; c < 3; c++)
                CHECK(lut.entry[k][c] == float(k / 255.0), "neutral table [%u][%u] = %.9f", k, c, lut.entry[k][c]);
        // ... and what the shader reads from it at a value and between two.
        for (uint32_t k = 0; k <= 255; k++)
            CHECK(std::abs(tone::Sample(lut, 0, k / 255.0) - k / 255.0) < 1e-7, "neutral sample at %u", k);
        CHECK(std::abs(tone::Sample(lut, 1, 0.5) - 0.5) < 1e-7, "neutral sample between entries");
    }

    // Cooler Colors (off by default): off must be the table as before, bit
    // for bit, at every Brightness step, with and without a ramp; on, black
    // and white stay put and every channel keeps rising (no clipped blue).
    void CoolerColors()
    {
        CHECK(tone::Neutral(0, false, false), "Cooler Colors off keeps the plain pipeline");
        CHECK(!tone::Neutral(0, false, true), "Cooler Colors on needs the tone pipeline");
        dg::Table ramp;
        for (uint32_t k = 0; k < 256; k++)
        {
            uint16_t v = uint16_t(std::lround(std::pow(k / 255.0, 0.86) * 1023.0));
            ramp.entry[k] = { v, v, v };
        }
        for (int32_t b = tone::kBrightnessMin; b <= tone::kBrightnessMax; b++)
            for (const dg::Table* r : std::initializer_list<const dg::Table*>{ nullptr, &ramp })
            {
                tone::Lut off = tone::BuildLut(b, r, false), on = tone::BuildLut(b, r, true);
                for (uint32_t k = 0; k < 256; k++)
                    for (uint32_t c = 0; c < 3; c++)
                    {
                        double v = r ? r->entry[k][c] / 1023.0 : k / 255.0;
                        CHECK(off.entry[k][c] == float(tone::ApplyBrightness(v, b)), "off = before, step %d [%u][%u]", b, k, c);
                        if (k > 0)
                            CHECK(on.entry[k][c] > on.entry[k - 1][c], "on rises, step %d [%u][%u]", b, k, c);
                    }
                for (uint32_t c = 0; c < 3; c++)
                    CHECK(on.entry[0][c] == 0.0f && on.entry[255][c] == 1.0f, "on keeps black and white, step %d channel %u", b, c);
            }
    }

    void BrightnessSteps()
    {
        double prevMid = -1;
        for (int32_t s = tone::kBrightnessMin; s <= tone::kBrightnessMax; s++)
        {
            tone::Lut lut = tone::BuildLut(s, nullptr);
            CHECK(lut.entry[0][0] == 0.0f && lut.entry[255][0] == 1.0f, "step %d: black and white stay put (%g, %g)", s,
                lut.entry[0][0], lut.entry[255][0]);
            for (uint32_t k = 1; k < 256; k++)
                CHECK(lut.entry[k][0] > lut.entry[k - 1][0], "step %d: not increasing at %u", s, k);
            for (uint32_t k = 0; k < 256; k++)
                CHECK(lut.entry[k][0] == lut.entry[k][1] && lut.entry[k][1] == lut.entry[k][2], "step %d: channels differ at %u", s, k);
            double mid = Levels(lut.entry[128][0]);
            if (s > 0)
                for (uint32_t k = 1; k < 255; k++)
                    CHECK(lut.entry[k][0] > k / 255.0f, "step %d: %u not brighter", s, k);
            if (s < 0)
                for (uint32_t k = 1; k < 255; k++)
                    CHECK(lut.entry[k][0] < k / 255.0f, "step %d: %u not darker", s, k);
            if (prevMid >= 0)
            {
                // Gentle: mid-grey moves 4..7 levels of 255 a step; no value
                // moves more than 9 in one step.
                CHECK(mid - prevMid >= 4.0 && mid - prevMid <= 7.0, "step %d: mid-grey moved %.2f levels", s, mid - prevMid);
                tone::Lut before = tone::BuildLut(s - 1, nullptr);
                for (uint32_t k = 0; k < 256; k++)
                    CHECK(Levels(lut.entry[k][0] - before.entry[k][0]) <= 9.0, "step %d: value %u moved %.2f levels", s, k,
                        Levels(lut.entry[k][0] - before.entry[k][0]));
            }
            prevMid = mid;
        }
        // The ends reach about as far as the games' own brightness range
        // does from its default (their exponent 0.57 .. 1.06 around 0.815):
        // mid-grey 157.5 .. 104.0 levels.
        double brightest = Levels(tone::ApplyBrightness(128 / 255.0, tone::kBrightnessMax));
        double darkest = Levels(tone::ApplyBrightness(128 / 255.0, tone::kBrightnessMin));
        double gameBright = Levels(std::pow(128 / 255.0, 0.57 / 0.815)), gameDark = Levels(std::pow(128 / 255.0, 1.06 / 0.815));
        CHECK(std::abs(brightest - gameBright) <= 4.0, "+5 mid-grey %.1f, the game's brightest %.1f", brightest, gameBright);
        CHECK(std::abs(darkest - gameDark) <= 8.0, "-5 mid-grey %.1f, the game's darkest %.1f", darkest, gameDark);
        // Out of range is clamped (a hand-edited settings.toml).
        CHECK(tone::BrightnessExponent(9) == tone::BrightnessExponent(5) && tone::BrightnessExponent(-9) == tone::BrightnessExponent(-5),
            "steps clamp to -5..+5");
        printf("brightness: mid-grey (128) by step:");
        for (int32_t s = tone::kBrightnessMin; s <= tone::kBrightnessMax; s++)
            printf(" %+d:%.1f", s, Levels(tone::ApplyBrightness(128 / 255.0, s)));
        printf("; 16 -> %.1f at +5, %.1f at -5\n", Levels(tone::ApplyBrightness(16 / 255.0, 5)), Levels(tone::ApplyBrightness(16 / 255.0, -5)));
    }

    void HalfTexels()
    {
        CHECK(tone::ToHalf(0.0f) == 0 && tone::ToHalf(1.0f) == 0x3C00 && tone::ToHalf(0.5f) == 0x3800, "exact halves");
        CHECK(tone::FromHalf(0x3C00) == 1.0f && tone::FromHalf(0x3555) > 0.333f && tone::FromHalf(0x3555) < 0.3334f, "decode");
        uint16_t prev = 0;
        for (uint32_t i = 1; i <= 100000; i++)
        {
            float x = i / 100000.0f;
            uint16_t h = tone::ToHalf(x);
            CHECK(h >= prev, "not monotonic at %g", x);
            prev = h;
            CHECK(std::abs(tone::FromHalf(h) - x) <= x * (1.0f / 2048.0f) + 1e-7f, "half of %g is %g", x, tone::FromHalf(h));
        }
        // Subnormals (shadows under 2^-14) still round to nearest.
        CHECK(std::abs(tone::FromHalf(tone::ToHalf(3e-5f)) - 3e-5f) <= std::ldexp(1.0f, -25), "subnormal");
        // Every neutral entry within an eighth of a level.
        auto texels = tone::ToHalfTexels(tone::BuildLut(0, nullptr));
        for (uint32_t k = 0; k < 256; k++)
            CHECK(std::abs(tone::FromHalf(texels[k * 4]) - k / 255.0f) * 255.0f < 0.125f && texels[k * 4 + 3] == 0x3C00, "texel %u", k);
    }

    void D3dModelMatchesTheDump()
    {
        // Carbon's ramp as logged from the command processor at the game's
        // default brightness (fidelity study, 2026-10-12): 8 -> 5.484,
        // 16 -> 12.214, 128 -> 133.9, 192 -> 196.4 (8-bit levels of 10-bit
        // entries). The model gives the same entries at g = 0.815, type 2.
        auto ramp = D3dRamp10(0.815, 2);
        CHECK(std::abs(ramp[8] * 255.0 / 1023.0 - 5.484) < 0.01, "8 -> %.3f", ramp[8] * 255.0 / 1023.0);
        CHECK(std::abs(ramp[16] * 255.0 / 1023.0 - 12.214) < 0.01, "16 -> %.3f", ramp[16] * 255.0 / 1023.0);
        CHECK(std::abs(ramp[128] * 255.0 / 1023.0 - 133.9) < 0.05, "128 -> %.3f", ramp[128] * 255.0 / 1023.0);
        CHECK(std::abs(ramp[192] * 255.0 / 1023.0 - 196.4) < 0.05, "192 -> %.3f", ramp[192] * 255.0 / 1023.0);
        // With an sRGB display (type 1) Direct3D passes the game's own ramp
        // through: with the game's ramp at 1.0 that is the identity.
        auto srgb = D3dRamp10(1.0, 1);
        dg::Table identity = dg::Table::Identity();
        for (uint32_t k = 0; k < 256; k++)
            CHECK(std::abs(int(srgb[k]) - int(identity.entry[k][0])) <= 1, "type 1, identity ramp: %u -> %u", k, srgb[k]);
    }

    void DecoderFollowsDirect3d()
    {
        CHECK(!dg::IsLutRegister(0x1920) && dg::IsLutRegister(0x1921) && dg::IsLutRegister(0x1925) && dg::IsLutRegister(0x1930) &&
                  !dg::IsLutRegister(0x1931) && !dg::IsLutRegister(0),
            "the LUT register range");
        auto ramp = D3dRamp10(0.815, 2);
        dg::Decoder d;
        CHECK(d.table() == dg::Table::Identity(), "a new decoder holds the identity");
        // Completed by the last entry's write, once.
        auto writes = D3dWrites(ramp);
        int completed = 0, at = -1;
        for (size_t i = 0; i < writes.size(); i++)
            if (d.Write(writes[i].first, writes[i].second))
            {
                completed++;
                at = int(i);
            }
        CHECK(completed == 1 && at == int(writes.size()) - 2, "completed %d times, at write %d of %zu", completed, at, writes.size());
        for (uint32_t k = 0; k < 256; k++)
            CHECK(d.table().entry[k][0] == ramp[k] && d.table().entry[k][1] == ramp[k] && d.table().entry[k][2] == ramp[k], "entry %u", k);
        CHECK(d.TableMode() && d.PwlWrites() == 0 && d.Pending() == 0, "table mode, nothing pending");
        // The same ramp again completes again (the runtime drops repeats).
        CHECK(Feed(d, writes) == 1, "a second sweep completes");

        // Channels land where DC_LUT_30_COLOR puts them (red 20:29, blue 0:9).
        std::vector<uint32_t> r(256), g(256), b(256);
        for (uint32_t k = 0; k < 256; k++)
        {
            r[k] = k * 4;
            g[k] = 1023 - k * 4;
            b[k] = k;
        }
        CHECK(Feed(d, D3dWrites(r, &g, &b)) == 1, "per-channel sweep");
        CHECK(d.table().entry[10][0] == 40 && d.table().entry[10][1] == 983 && d.table().entry[10][2] == 10, "channel order: %u %u %u",
            d.table().entry[10][0], d.table().entry[10][1], d.table().entry[10][2]);

        // DC_LUT_30_COLOR moves the index on by itself (a writer that sets
        // RW_INDEX once, then 256 colors).
        dg::Decoder autoIndex;
        std::vector<std::pair<uint32_t, uint32_t>> colorsOnly = { { dg::kRegRwIndex, 0 } };
        for (uint32_t k = 0; k < 256; k++)
            colorsOnly.push_back({ dg::kRegColor30, (r[k] << 20) | (g[k] << 10) | b[k] });
        CHECK(Feed(autoIndex, colorsOnly) == 1 && autoIndex.table().entry[255][0] == r[255] && autoIndex.table().entry[1][2] == b[1],
            "auto-increment");

        // Half a sweep is not a table.
        dg::Decoder half;
        auto partial = D3dWrites(ramp);
        partial.resize(3 + 2 * 128);
        CHECK(Feed(half, partial) == 0 && half.Pending() == 128, "half a sweep: pending %zu", half.Pending());

        // The write-enable mask: red only.
        dg::Decoder masked;
        auto redOnly = D3dWrites(ramp);
        redOnly[2].second = 4;
        CHECK(Feed(masked, redOnly) == 1, "red-only sweep completes");
        CHECK(masked.table().entry[128][0] == ramp[128] && masked.table().entry[128][1] == dg::Table::Identity().entry[128][1],
            "red only: green kept");
        // A mask of 0 writes nothing (and completes nothing).
        dg::Decoder none;
        auto noMask = D3dWrites(ramp);
        noMask[2].second = 0;
        CHECK(Feed(none, noMask) == 0 && none.table() == dg::Table::Identity(), "mask 0");

        // DC_LUT_SEQ_COLOR: red, green, blue a write, bits 6:15.
        dg::Decoder seq;
        int seqCompleted = seq.Write(dg::kRegRwIndex, 0) ? 1 : 0;
        for (uint32_t k = 0; k < 256; k++)
            for (uint32_t c = 0; c < 3; c++)
                seqCompleted += seq.Write(dg::kRegSeqColor, (c == 0 ? r[k] : c == 1 ? g[k] : b[k]) << 6) ? 1 : 0;
        CHECK(seqCompleted == 1 && seq.table().entry[200][0] == r[200] && seq.table().entry[200][1] == g[200] &&
                  seq.table().entry[200][2] == b[200],
            "sequential writes: %d completions", seqCompleted);
        // An index write restarts at red.
        seq.Write(dg::kRegRwIndex, 5);
        seq.Write(dg::kRegSeqColor, 100 << 6);  // red of 5
        seq.Write(dg::kRegRwIndex, 7);
        seq.Write(dg::kRegSeqColor, 300 << 6);  // red of 7, not green
        CHECK(seq.table().entry[5][0] == 100 && seq.table().entry[7][0] == 300 && seq.table().entry[7][1] == g[7], "index restarts at red");

        // The PWL ramp: counted, not applied; in PWL mode DC_LUT_30_COLOR
        // writes don't touch the table.
        dg::Decoder pwl;
        pwl.Write(dg::kRegLutaControl, 3);
        pwl.Write(dg::kRegRwMode, 1);
        pwl.Write(dg::kRegPwlData, 0x02000000);
        pwl.Write(dg::kRegColor30, 0x3FFFFFFF);
        CHECK(!pwl.TableMode() && pwl.PwlWrites() == 1 && pwl.table() == dg::Table::Identity(), "PWL");
    }

    void RampThroughTheTable()
    {
        auto ramp10 = D3dRamp10(0.815, 2);
        dg::Table ramp;
        for (uint32_t k = 0; k < 256; k++)
            ramp.entry[k] = { uint16_t(ramp10[k]), uint16_t(ramp10[k]), uint16_t(ramp10[k]) };
        tone::Lut lut = tone::BuildLut(0, &ramp);
        for (uint32_t k = 0; k < 256; k++)
            CHECK(lut.entry[k][0] == float(ramp10[k] / 1023.0), "the ramp alone, entry %u", k);
        // The 360 ramp darkens the shadows and brightens the mid-tones.
        CHECK(Levels(lut.entry[8][0]) < 7.0 && Levels(lut.entry[128][0]) > 132.0, "8 -> %.1f, 128 -> %.1f", Levels(lut.entry[8][0]),
            Levels(lut.entry[128][0]));
        // Brightness after it.
        tone::Lut both = tone::BuildLut(2, &ramp);
        for (uint32_t k = 0; k < 256; k++)
            CHECK(std::abs(both.entry[k][0] - std::pow(ramp10[k] / 1023.0, tone::BrightnessExponent(2))) < 1e-6, "ramp then +2, %u", k);
    }

    // presenter.cpp's scale shaders: the tone variant renames their one main.
    void PresenterShaders(const char* path)
    {
        std::ifstream in(path);
        std::stringstream ss;
        ss << in.rdbuf();
        std::string src = ss.str();
        CHECK(!src.empty(), "can't read %s", path);
        for (const char* name : { "kScaleFs", "kScaleFastFs" })
        {
            std::string open = std::string("const char* ") + name + " = R\"(";
            size_t a = src.find(open);
            CHECK(a != std::string::npos, "%s not found", name);
            if (a == std::string::npos)
                continue;
            size_t b = src.find(")\";", a);
            std::string glsl = src.substr(a, b - a);
            size_t first = glsl.find("void main()");
            CHECK(first != std::string::npos && glsl.find("void main()", first + 1) == std::string::npos, "%s: one main", name);
            CHECK(glsl.find("scaleMain") == std::string::npos && glsl.find("toneTable") == std::string::npos,
                "%s: the plain shader stays as it was", name);
        }
        // The plain scale pipeline is still made from those shaders, and the
        // tone pipeline only from ToneVariant.
        CHECK(src.find("s_post.fs = PostModule(s_fastScale ? kScaleFastFs : kScaleFs, 1);") != std::string::npos, "the plain scale shader");
        CHECK(src.find("ToneVariant(s_fastScale ? kScaleFastFs : kScaleFs)") != std::string::npos, "the tone variant's source");
        CHECK(src.find("tone ? s_tone.pipeline : s_post.scale") != std::string::npos, "the scale pass picks the plain pipeline when neutral");
    }

    // settings.cpp's option table: Xbox 360 Display Gamma is on by default
    // (v0.1.4: the picture a real 360 sends to a TV; a settings.toml without
    // the key, as every earlier version wrote it, gets it on), Brightness 0.
    void SettingsDefaults(const char* path)
    {
        std::ifstream in(path);
        std::stringstream ss;
        ss << in.rdbuf();
        std::string src = ss.str();
        CHECK(!src.empty(), "can't read %s", path);
        auto entry = [&](const char* key) {
            // From the key to the end of its option ("},").
            size_t a = src.find(std::string(".key = \"") + key + "\"");
            if (a == std::string::npos)
                return std::string();
            size_t b = src.find("},", a);
            return src.substr(a, b == std::string::npos ? std::string::npos : b - a);
        };
        std::string gamma = entry("xbox_display_gamma"), brightness = entry("brightness"), cooler = entry("cooler_colors");
        CHECK(cooler.find(".defaultValue = 0 ") != std::string::npos, "Cooler Colors defaults to off: %s", cooler.c_str());
        CHECK(gamma.find(".defaultValue = 1 ") != std::string::npos, "Xbox 360 Display Gamma defaults to on: %s", gamma.c_str());
        CHECK(brightness.find(".defaultValue = 0,") != std::string::npos, "Brightness defaults to 0: %s", brightness.c_str());
        CHECK(gamma.find("Off: the picture as before") == std::string::npos, "the help text no longer calls Off the default");
    }
}

int main(int argc, char** argv)
{
    Neutral();
    CoolerColors();
    BrightnessSteps();
    HalfTexels();
    D3dModelMatchesTheDump();
    DecoderFollowsDirect3d();
    RampThroughTheTable();
    if (argc > 1)
        PresenterShaders(argv[1]);
    else
        printf("(presenter.cpp not given: its shader checks skipped)\n");
    if (argc > 2)
        SettingsDefaults(argv[2]);
    else
        printf("(settings.cpp not given: the defaults' checks skipped)\n");
    if (s_failures)
    {
        fprintf(stderr, "%d failure(s)\n", s_failures);
        return 1;
    }
    printf("display_tone_test: all passed\n");
    return 0;
}
