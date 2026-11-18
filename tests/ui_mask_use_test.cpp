// Tests for video::ui_mask (runtime/video/ui_mask_use.h): when the presenter's
// UI pass (Sharper UI's UI-C) applies to a picture, the same rule as before
// (checked against the old expression over a grid of sizes), the presenter's
// word packed into one atomic, and the renderer's NFSMW_UI_MASK_LAZY skip
// condition (build the mask unless the presenter said "doesn't apply" for that
// exact size). presenter.cpp and renderer.cpp are read to check they apply it
// (the front-end draws keep their mask-writing pipelines; a skipped frame's
// draws write the 1x1 image; the switch turns it off).
// From the repo root:
//   clang++ -std=c++20 -O2 -Iruntime tests/ui_mask_use_test.cpp -o build/ui_mask_use_test
//   build/ui_mask_use_test runtime/gpu/renderer.cpp runtime/video/presenter.cpp
#include <video/ui_mask_use.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using namespace video;

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

// presenter.cpp's UiPassApplies before NFSMW_UI_MASK_LAZY, with the mask present.
static bool OldRule(bool on, bool bicubic, bool hdr, uint32_t w, uint32_t h, int32_t dw, int32_t dh)
{
    if (!on || !bicubic || hdr || !upscale::Enlarged(w, h, dw, dh))
        return false;
    return uint32_t(dw) % w != 0 || uint32_t(dh) % h != 0;
}

static void TestPassApplies()
{
    struct Case { uint32_t w, h; int32_t dw, dh; bool applies; const char* what; };
    for (const Case& c : {
             Case{ 2560, 1440, 2560, 1440, false, "the Mac's first window at Auto 2x2: shown 1:1" },
             Case{ 2560, 1440, 3840, 2160, true, "2x2 at 4K: x1.5" },
             Case{ 1280, 720, 2560, 1440, false, "1x at 1440p: a whole x2" },
             Case{ 1280, 720, 3840, 2160, false, "1x at 4K: a whole x3" },
             Case{ 2560, 1440, 3024, 1701, true, "2x2 in the MacBook's whole display, 16:9: x1.18" },
             Case{ 2560, 1664, 3024, 1964, true, "Vert+ 2x2 in the whole display: x1.18" },
             Case{ 2560, 1440, 1920, 1080, false, "shrunk" },
             Case{ 2560, 1440, 2560, 1600, true, "taller only, not by a whole factor" },
             Case{ 2560, 1440, 2560, 2880, false, "taller only, by exactly 2" },
             Case{ 2560, 1440, 2600, 1400, false, "wider but shorter: not enlarged" },
             Case{ 0, 1440, 3840, 2160, false, "no picture" },
             Case{ 2560, 1440, 0, 0, false, "nowhere on the screen" } })
    {
        CHECK(ui_mask::PassApplies(true, true, false, c.w, c.h, c.dw, c.dh) == c.applies, "%s: %ux%u -> %dx%d", c.what, c.w, c.h, c.dw, c.dh);
        CHECK(!ui_mask::PassApplies(false, true, false, c.w, c.h, c.dw, c.dh), "%s: the pass off", c.what);
        CHECK(!ui_mask::PassApplies(true, false, false, c.w, c.h, c.dw, c.dh), "%s: another scale pass", c.what);
        CHECK(!ui_mask::PassApplies(true, true, true, c.w, c.h, c.dw, c.dh), "%s: HDR", c.what);
    }
    // The same answer as the old rule everywhere (a grid of pictures and screens).
    int differ = 0, cases = 0;
    for (uint32_t w : { 1u, 640u, 1280u, 1600u, 1920u, 2560u, 2580u, 3200u, 3840u })
        for (uint32_t h : { 1u, 720u, 800u, 1080u, 1131u, 1440u, 1600u, 1664u, 2160u })
            for (int32_t dw : { 0, 1, 1280, 1800, 1920, 2560, 3024, 3456, 3840, 5120, 7680 })
                for (int32_t dh : { 0, 1, 720, 1080, 1131, 1440, 1701, 1964, 2160, 2880, 4320 })
                    for (int k = 0; k < 8; k++)
                    {
                        const bool on = k & 1, bicubic = k & 2, hdr = k & 4;
                        cases++;
                        if (ui_mask::PassApplies(on, bicubic, hdr, w, h, dw, dh) != OldRule(on, bicubic, hdr, w, h, dw, dh))
                            differ++;
                    }
    CHECK(differ == 0, "%d of %d cases differ from the old rule", differ, cases);
}

static void TestUseful()
{
    // Nothing said yet: build it.
    CHECK(ui_mask::Useful(0, 2560, 1440), "no word yet");
    // The presenter's word for that exact size decides.
    CHECK(!ui_mask::Useful(ui_mask::Pack(2560, 1440, false), 2560, 1440), "doesn't apply at this size: skipped");
    CHECK(ui_mask::Useful(ui_mask::Pack(2560, 1440, true), 2560, 1440), "applies at this size: built");
    // Another size (a resolution change in flight): build it, as before.
    CHECK(ui_mask::Useful(ui_mask::Pack(2560, 1440, false), 2560, 1664), "another height");
    CHECK(ui_mask::Useful(ui_mask::Pack(2560, 1440, false), 3840, 1440), "another width");
    CHECK(ui_mask::Useful(ui_mask::Pack(1280, 720, false), 2560, 1440), "the 1x picture's word is not the 2x one's");
    // The fields don't bleed into each other at any size the renderer makes.
    for (uint32_t w : { 1u, 1280u, 2560u, 7680u, 16384u, 65535u, 0x7FFFFFFFu })
        for (uint32_t h : { 1u, 720u, 1440u, 4320u, 16384u, 65535u, 0x7FFFFFFFu })
        {
            const uint64_t no = ui_mask::Pack(w, h, false), yes = ui_mask::Pack(w, h, true);
            CHECK((no >> 63) && (yes >> 63), "%ux%u: marked said", w, h);
            CHECK(!ui_mask::Useful(no, w, h) && ui_mask::Useful(yes, w, h), "%ux%u: its own word", w, h);
            CHECK(ui_mask::Useful(no, w ^ 1, h) && ui_mask::Useful(no, w, h ^ 1), "%ux%u: a neighbour size builds", w, h);
        }
}

static std::string Read(const char* path)
{
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static void TestSources(const char* rendererPath, const char* presenterPath)
{
    const std::string r = Read(rendererPath), p = Read(presenterPath);
    CHECK(!r.empty() && !p.empty(), "read %s and %s", rendererPath, presenterPath);
    if (r.empty() || p.empty())
        return;
    // The presenter: its UI pass by the shared rule, the mask still required; the word published per picture.
    CHECK(p.find("return mask && ui_mask::PassApplies(UiMaskOn(), pass == upscale::Pass::Bicubic, s_swapHdr, w, h, dw, dh);") != std::string::npos,
        "presenter.cpp: UiPassApplies = a mask and the shared rule");
    CHECK(p.find("s_uiMaskUse.store(ui_mask::Pack(w, h, applies)") != std::string::npos, "presenter.cpp: the word for each picture drawn");
    // (Whatever Sharper UI's setting: the renderer reads that itself, so a live switch to On has its mask at once.)
    CHECK(p.find("ui_mask::PassApplies(true, pass == upscale::Pass::Bicubic, s_swapHdr, w, h, dw, dh);") != std::string::npos,
        "presenter.cpp: the word without the setting");
    CHECK(p.find("ui_mask::Useful(s_uiMaskUse.load(") != std::string::npos, "presenter.cpp: UiMaskUseful reads it");
    // The renderer: the switch, the decision per front image, and the draws.
    CHECK(r.find("std::getenv(\"NFSMW_UI_MASK_LAZY\"); return !(v && v[0] == '0');") != std::string::npos,
        "renderer.cpp: NFSMW_UI_MASK_LAZY, on unless 0");
    CHECK(r.find(": UiMaskLazy();") != std::string::npos && r.find("const bool live = !lazyNext || video::UiMaskUseful(outW, outH);") != std::string::npos,
        "renderer.cpp: built unless lazy and the presenter said no");
    CHECK(r.find("std::getenv(\"NFSMW_UI_MASK_LAZY_AB\"); return v && v[0] == '1';") != std::string::npos,
        "renderer.cpp: the A/B diagnostic only when asked for");
    // The draws take the mask-writing variant whenever they did before (the images exist: `armed`), whether or not this
    // frame's mask is built: the same pipelines, so the same pictures.
    CHECK(r.find("if (UiMaskWanted() && s_uiMask.armed)") != std::string::npos, "renderer.cpp: the variant's mask bits as before");
    CHECK(r.find("uiMaskInfo.imageView = s_uiMask.dummy.view;") != std::string::npos, "renderer.cpp: a skipped frame writes the 1x1 image");
    CHECK(r.find("VK_FORMAT_R32_UINT, { 1, 1, 1 }") != std::string::npos, "renderer.cpp: the 1x1 image");
    // The resolve and its clear run only for a built mask.
    CHECK(r.find("if (s_uiMask.live && s_uiMask.written)") != std::string::npos, "renderer.cpp: no resolve or clear for a skipped frame");
}

int main(int argc, char** argv)
{
    TestPassApplies();
    TestUseful();
    if (argc >= 3)
        TestSources(argv[1], argv[2]);
    else
    {
        printf("  FAIL: give renderer.cpp and presenter.cpp\n");
        g_failures++;
    }
    if (g_failures)
    {
        printf("ui_mask_use_test: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("ui_mask_use_test: all passed\n");
    return 0;
}
