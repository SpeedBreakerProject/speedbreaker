// Tests for video::FitPicture, PlacePicture and AutoRenderScale
// (runtime/video/picture_fit.cpp). From the repo root:
//   clang++ -std=c++20 -O2 -Iruntime tests/picture_fit_test.cpp runtime/video/picture_fit.cpp -o build/picture_fit_test
//   build/picture_fit_test
#include <video/picture_fit.h>

#include <cmath>
#include <cstdio>

using video::AutoRenderScale;
using video::FitPicture;
using video::PictureFit;
using video::PlacePicture;

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

static bool Near(float a, float b, float tolerance = 1e-4f)
{
    return std::fabs(a - b) <= tolerance;
}

// Fill Screen on a W x H screen (the hooks present), the 1280x720 frame.
static void Fill(uint32_t w, uint32_t h, float sx, float sy, int32_t pw, int32_t ph)
{
    PictureFit f = FitPicture(w, h, 1280.0f, 720.0f, true, true);
    printf("%ux%u: scale x %.4f y %.4f, picture %dx%d\n", w, h, f.scaleX, f.scaleY, f.width, f.height);
    CHECK(Near(f.scaleX, sx) && Near(f.scaleY, sy), "%ux%u: scale x %.4f y %.4f, want %.4f %.4f", w, h, f.scaleX, f.scaleY, sx, sy);
    CHECK(f.width == pw && f.height == ph, "%ux%u: picture %dx%d, want %dx%d", w, h, f.width, f.height, pw, ph);
    CHECK(f.scaleX == 1.0f || f.scaleY == 1.0f, "%ux%u: both scales below 1", w, h);
    // A redraw of that frame on the same screen keeps its picture.
    PictureFit again = PlacePicture(w, h, 1280.0f, 720.0f, f.scaleX, f.scaleY);
    CHECK(again.width == f.width && again.height == f.height, "%ux%u: redraw %dx%d, new frame %dx%d", w, h, again.width, again.height,
        f.width, f.height);
}

// The letterbox from before Vert+, exactly.
static void Letterbox(uint32_t w, uint32_t h, float frameW, float frameH, const PictureFit& f, const char* what)
{
    float scale = std::fmin(float(w) / frameW, float(h) / frameH);
    int32_t pw = int32_t(frameW * scale), ph = int32_t(frameH * scale);
    CHECK(f.scaleX == 1.0f && f.scaleY == 1.0f, "%s %ux%u: scale x %.4f y %.4f", what, w, h, f.scaleX, f.scaleY);
    CHECK(f.width == pw && f.height == ph, "%s %ux%u: picture %dx%d, want %dx%d", what, w, h, f.width, f.height, pw, ph);
}

static void Scale(uint32_t regionW, uint32_t regionH, uint32_t budget, uint32_t wantX, uint32_t wantY)
{
    uint32_t sx = 0, sy = 0;
    AutoRenderScale(regionW, regionH, budget, sx, sy);
    printf("render scale for %ux%u, budget %u: %ux%u\n", regionW, regionH, budget, sx, sy);
    CHECK(sx == wantX && sy == wantY, "%ux%u budget %u: %ux%u, want %ux%u", regionW, regionH, budget, sx, sy, wantX, wantY);
}

int main()
{
    printf("Fill Screen:\n");
    Fill(1280, 720, 1.0f, 1.0f, 1280, 720);
    Fill(1366, 768, 1.0f, 1.0f, 1365, 768);         // within 1% of 16:9: letterboxed as before
    Fill(2560, 1080, 0.75f, 1.0f, 2560, 1080);      // Hor+
    Fill(3440, 1440, 0.7442f, 1.0f, 3440, 1440);
    Fill(2796, 1290, 0.8202f, 1.0f, 2796, 1290);    // iPhone 15 Pro Max
    Fill(1280, 800, 1.0f, 1.0f, 1280, 720);         // Steam Deck: 16:10 keeps its bars (kTallMinStretch)
    Fill(3024, 1890, 1.0f, 1.0f, 3024, 1701);       // 16:10 Mac fullscreen, likewise
    Fill(2160, 1440, 1.0f, 0.84375f, 2160, 1440);   // 3:2: Vert+
    Fill(2732, 2048, 1.0f, 0.7504f, 2732, 2048);    // iPad Pro 12.9"
    Fill(1024, 768, 1.0f, 0.75f, 1024, 768);
    Fill(1280, 1024, 1.0f, 0.75f, 1280, 960);       // narrower than 4:3: a 4:3 picture, letterboxed
    Fill(720, 1280, 1.0f, 0.75f, 720, 540);         // portrait

    printf("16:9 setting (or no hooks at all), and no Vert+ hooks:\n");
    const uint32_t screens[][2] = { { 1280, 720 }, { 1366, 768 }, { 2560, 1080 }, { 3440, 1440 }, { 1280, 800 }, { 2732, 2048 },
        { 1024, 768 }, { 1280, 1024 }, { 720, 1280 }, { 1920, 1080 }, { 3840, 2160 } };
    for (const auto& s : screens)
    {
        Letterbox(s[0], s[1], 1280.0f, 720.0f, FitPicture(s[0], s[1], 1280.0f, 720.0f, false, false), "16:9");
        Letterbox(s[0], s[1], 3840.0f, 1440.0f, FitPicture(s[0], s[1], 3840.0f, 1440.0f, false, false), "16:9 (3x2 image)");
        // Without the Vert+ hooks (an older ppc/): Hor+ only.
        PictureFit wideOnly = FitPicture(s[0], s[1], 1280.0f, 720.0f, true, false);
        CHECK(wideOnly.scaleY == 1.0f, "%ux%u without the Vert+ hooks: scale y %.4f", s[0], s[1], wideOnly.scaleY);
        if (float(s[0]) / float(s[1]) <= 16.0f / 9.0f * 1.01f)
            Letterbox(s[0], s[1], 1280.0f, 720.0f, wideOnly, "no Vert+ hooks");
    }

    printf("Redraws:\n");
    {
        // A frame rendered for 4:3, drawn again after a resize to 16:10:
        // its 4:3 shape, pillarboxed, until the next new frame decides.
        PictureFit f = PlacePicture(1280, 800, 1280.0f, 720.0f, 1.0f, 0.75f);
        CHECK(f.width == 1067 && f.height == 800, "4:3 frame on 1280x800: %dx%d", f.width, f.height);
        // A 16:9 frame (1, 1) is letterboxed whatever the screen.
        Letterbox(1024, 768, 1280.0f, 720.0f, PlacePicture(1024, 768, 1280.0f, 720.0f, 1.0f, 1.0f), "redraw 1, 1");
    }

    printf("Auto internal resolution:\n");
    Scale(1280, 720, 6, 1, 1);
    Scale(1280, 800, 4, 1, 1);    // Steam Deck, Vert+: not 1x2
    Scale(1280, 720, 4, 1, 1);    // Steam Deck, 16:9 setting
    Scale(1024, 768, 4, 1, 1);
    Scale(2732, 2048, 1, 1, 1);   // iPad: budget 1
    Scale(3024, 1890, 4, 2, 2);   // 16:10 Mac fullscreen, Vert+: 3x3 cut to 2x2
    Scale(3024, 1701, 4, 2, 2);   // its 16:9 box
    Scale(2560, 1440, 6, 2, 2);   // 1440p
    Scale(3440, 1440, 6, 3, 2);   // ultrawide filled
    Scale(3840, 2160, 9, 3, 3);
    Scale(1365, 768, 4, 2, 2);    // a letterboxed 16:9 box, a pixel narrower than 16:9: as before
    Scale(1280, 960, 4, 1, 2);    // a 4:3 picture letterboxed on 1280x1024
    Scale(2560, 1600, 6, 2, 2);   // 16:10 on a discrete GPU: as its 16:9 box, not 2x3

    if (g_failures)
    {
        printf("%d FAILED\n", g_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
