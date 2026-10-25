// Tests for native rows and columns (GitHub #34): video::NativeShape and
// video::AutoNativeScale (runtime/video/picture_fit.cpp). From the repo root:
//   clang++ -std=c++20 -O2 -Iruntime tests/native_rows_test.cpp runtime/video/picture_fit.cpp -o build/native_rows_test
//   build/native_rows_test
// The report: on a Steam Deck (1280x800, Fill Screen, Internal Resolution
// 1x) the game rendered its 16:9 frame's 720 rows and the presenter
// stretched them to 800, so the picture was softer than at 16:9, whose 720
// rows it showed 1:1. Checked:
//   - a Vert+ picture renders the rows its shape has at the setting's
//     columns, a Hor+ one the columns at its rows: the Deck's 1280x800 at 1x
//     is 1280x800, shown 1:1; 1.5x on 1920x1200 and 2x on 2560x1600 too; a
//     MacBook's 3024x1898 within a pixel of its shape; ultrawides, 4:3, a
//     phone;
//   - every picture within 0.5% of 16:9 (16:9 screens, letterboxed boxes,
//     the 16:9 setting on any screen) keeps the setting's scale on both
//     axes, and Auto there is AutoRenderScale's, exactly as before;
//   - Auto takes the native shape only within its budget (the Deck's 1x10/9
//     at 4, an ultrawide's 2.6875x2 at 6), else keeps AutoRenderScale's.
// Planted bugs this catches (each tried): rows from the 720 rows instead of
// the columns (1x on the Deck 1x1), the 16:9 margin dropped (a 1366x768 box
// gets 1281/1280 columns), the budget unchecked (a MacBook's Auto 2x2.232).
#include <video/picture_fit.h>

#include <cmath>
#include <cstdio>
#include <numeric>

using gpu::AxisScale;
using gpu::RenderScale;
using video::AutoNativeScale;
using video::AutoRenderScale;
using video::FitPicture;
using video::NativeShape;
using video::PictureFit;

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const AxisScale kOne{ 1, 1 }, kHalf{ 3, 2 }, kTwo{ 2, 1 }, kThree{ 3, 1 };
static const AxisScale kSettings[] = { kOne, kHalf, kTwo, kThree };  // 1x, 1.5x, 2x, 3x

// The picture Fill Screen gives a W x H screen (the region FrameRegion
// reports), and the host frame the scale renders: its 1280x720 guest frame
// at the scale.
static PictureFit Picture(uint32_t w, uint32_t h, bool fill = true)
{
    return FitPicture(w, h, 1280.0f, 720.0f, fill, fill);
}

static void Native(uint32_t screenW, uint32_t screenH, AxisScale base, uint32_t wantW, uint32_t wantH)
{
    const PictureFit fit = Picture(screenW, screenH);
    const RenderScale s = NativeShape(base, uint32_t(fit.width), uint32_t(fit.height));
    const uint32_t hostW = s.x.Size(1280), hostH = s.y.Size(720);
    printf("%ux%u at %s: picture %dx%d, renders %s = %ux%u, shown at %.4f x %.4f\n", screenW, screenH, gpu::AxisName(base).c_str(), fit.width,
        fit.height, gpu::ScaleName(s).c_str(), hostW, hostH, double(fit.width) / hostW, double(fit.height) / hostH);
    CHECK(hostW == wantW && hostH == wantH, "%ux%u at %s: %ux%u, want %ux%u", screenW, screenH, gpu::AxisName(base).c_str(), hostW, hostH, wantW,
        wantH);
}

int main()
{
    printf("The report: a Steam Deck, Fill Screen, 1x\n");
    {
        const PictureFit fit = Picture(1280, 800);
        CHECK(fit.width == 1280 && fit.height == 800 && std::fabs(fit.scaleY - 0.9f) < 1e-6f, "Deck picture %dx%d, view y %.4f", fit.width, fit.height, fit.scaleY);
        const RenderScale s = NativeShape(kOne, 1280, 800);
        CHECK((s == RenderScale{ kOne, { 10, 9 } }), "Deck 1x: %s, want 1x10/9", gpu::ScaleName(s).c_str());
        // The presenter shows the 1280x800 frame on the 1280x800 picture:
        // 1:1, no resampling (before: 1280x720 at 1 x 1.111).
        CHECK(s.x.Size(1280) == uint32_t(fit.width) && s.y.Size(720) == uint32_t(fit.height), "Deck 1x frame %ux%u for a %dx%d picture",
            s.x.Size(1280), s.y.Size(720), fit.width, fit.height);
        // Its 16:9 setting: a 1280x720 box, 1x1 as before (the crisp case).
        const PictureFit box = Picture(1280, 800, false);
        CHECK(box.width == 1280 && box.height == 720, "Deck 16:9 box %dx%d", box.width, box.height);
        CHECK((NativeShape(kOne, 1280, 720) == RenderScale{ kOne, kOne }), "Deck 16:9 at 1x: %s", gpu::ScaleName(NativeShape(kOne, 1280, 720)).c_str());
    }
    Native(1280, 800, kHalf, 1920, 1200);   // 1.5x on the Deck
    Native(1280, 800, kTwo, 2560, 1600);
    Native(1920, 1200, kOne, 1280, 800);    // 16:10 1200p: 1x a uniform 1.5x enlargement
    Native(1920, 1200, kHalf, 1920, 1200);  // and 1:1 at 1.5x
    Native(2560, 1600, kOne, 1280, 800);
    Native(2560, 1600, kTwo, 2560, 1600);   // 1:1 at 2x
    Native(3024, 1898, kOne, 1280, 803);    // a MacBook Pro's fullscreen (below the notch)
    Native(3024, 1898, kTwo, 2560, 1607);
    Native(3024, 1890, kTwo, 2560, 1600);
    Native(3440, 1440, kOne, 1720, 720);    // ultrawide, Hor+: the columns
    Native(3440, 1440, kTwo, 3440, 1440);   // 1:1 at 2x (Auto gave 3x2: 3840 columns)
    Native(2560, 1080, kOne, 1707, 720);
    Native(2560, 1080, kHalf, 2560, 1080);
    Native(2732, 2048, kOne, 1280, 960);    // iPad Pro 12.9", 4:3
    Native(1280, 1024, kOne, 1280, 960);    // a 4:3 picture letterboxed on 5:4
    Native(2160, 1440, kOne, 1280, 853);    // 3:2
    Native(2796, 1290, kOne, 1561, 720);    // iPhone 15 Pro Max

    printf("Every Vert+ and Hor+ picture, every setting: the base axis as the setting, the other the picture's shape\n");
    {
        const uint32_t screens[][2] = { { 1280, 800 }, { 1920, 1200 }, { 2560, 1600 }, { 3024, 1898 }, { 3024, 1890 }, { 2880, 1800 },
            { 1440, 900 }, { 1680, 1050 }, { 2160, 1440 }, { 3000, 2000 }, { 2732, 2048 }, { 2048, 1536 }, { 1024, 768 }, { 3440, 1440 },
            { 2560, 1080 }, { 5120, 1440 }, { 3840, 1600 }, { 2796, 1290 }, { 2556, 1179 }, { 1280, 1024 }, { 720, 1280 } };
        for (const auto& sc : screens)
        {
            const PictureFit fit = Picture(sc[0], sc[1]);
            const bool tall = float(fit.width) / float(fit.height) < 16.0f / 9.0f;
            for (const AxisScale& base : kSettings)
            {
                const RenderScale s = NativeShape(base, uint32_t(fit.width), uint32_t(fit.height));
                const AxisScale& kept = tall ? s.x : s.y;
                const AxisScale& grown = tall ? s.y : s.x;
                CHECK(kept == base, "%ux%u at %s: kept axis %s", sc[0], sc[1], gpu::AxisName(base).c_str(), gpu::AxisName(kept).c_str());
                CHECK(grown.Value() > base.Value(), "%ux%u at %s: extended axis %s", sc[0], sc[1], gpu::AxisName(base).c_str(),
                    gpu::AxisName(grown).c_str());
                // The frame's shape is the picture's within half a host pixel
                // of the extended axis.
                const double hostW = s.x.Size(1280), hostH = s.y.Size(720);
                const double want = tall ? hostW * fit.height / fit.width : hostH * fit.width / fit.height;
                CHECK(std::fabs((tall ? hostH : hostW) - want) <= 0.5, "%ux%u at %s: %.0fx%.0f for a %dx%d picture", sc[0], sc[1],
                    gpu::AxisName(base).c_str(), hostW, hostH, fit.width, fit.height);
                // Reduced (a block or a texel axis), at most 3x's 6 (32:9 at 3x).
                CHECK(std::gcd(grown.num, grown.den) == 1 && grown.num <= 6 * grown.den, "%ux%u: %u/%u", sc[0], sc[1], grown.num, grown.den);
            }
        }
    }

    printf("16:9 pictures (within 0.5%%): the setting's scale both ways, Auto as AutoRenderScale, exactly as before\n");
    {
        // 16:9 screens, the boxes the 16:9 setting gives other screens, and
        // boxes rounded a pixel off 16:9.
        const uint32_t boxes[][2] = { { 1280, 720 }, { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 }, { 1365, 768 }, { 1366, 768 },
            { 1360, 768 }, { 3024, 1701 }, { 3024, 1700 }, { 2560, 1439 }, { 1279, 720 }, { 2732, 1536 }, { 3440, 1935 }, { 640, 360 } };
        for (const auto& b : boxes)
        {
            for (const AxisScale& base : kSettings)
            {
                const RenderScale s = NativeShape(base, b[0], b[1]);
                CHECK((s == RenderScale{ base, base }), "%ux%u at %s: %s", b[0], b[1], gpu::AxisName(base).c_str(), gpu::ScaleName(s).c_str());
            }
            for (uint32_t budget = 1; budget <= 9; budget++)
            {
                uint32_t sx = 0, sy = 0;
                AutoRenderScale(b[0], b[1], budget, sx, sy);
                const RenderScale s = AutoNativeScale(b[0], b[1], budget);
                CHECK(s == RenderScale::Whole(sx, sy), "%ux%u budget %u: Auto %s, before %ux%u", b[0], b[1], budget, gpu::ScaleName(s).c_str(), sx, sy);
            }
        }
        // The 16:9 setting on other screens letterboxes a 16:9 box: the same.
        const uint32_t screens[][2] = { { 1280, 800 }, { 3024, 1898 }, { 3440, 1440 }, { 2732, 2048 }, { 2796, 1290 } };
        for (const auto& sc : screens)
        {
            const PictureFit box = Picture(sc[0], sc[1], false);
            for (const AxisScale& base : kSettings)
                CHECK((NativeShape(base, uint32_t(box.width), uint32_t(box.height)) == RenderScale{ base, base }), "%ux%u 16:9 box %dx%d at %s",
                    sc[0], sc[1], box.width, box.height, gpu::AxisName(base).c_str());
        }
        // No picture yet: the setting's scale.
        CHECK((NativeShape(kTwo, 0, 0) == RenderScale{ kTwo, kTwo }), "no region");
    }

    printf("Auto: the native shape within the budget, else as before\n");
    {
        struct Case { uint32_t w, h, budget; RenderScale want; const char* what; };
        const Case cases[] = {
            { 1280, 800, 4, { kOne, { 10, 9 } }, "Steam Deck: 1280x800" },
            { 3440, 1440, 6, { { 43, 16 }, kTwo }, "ultrawide on a discrete GPU: 3440x1440 in place of 3x2" },
            { 3440, 1440, 4, RenderScale::Whole(2, 2), "ultrawide, integrated: 2.6875x2 is over 4, so 2x2" },
            { 2560, 1600, 6, { kTwo, { 20, 9 } }, "1600p, discrete: 2560x1600 in place of 2x2" },
            { 1920, 1200, 6, { kTwo, { 20, 9 } }, "1200p, discrete: 2560x1600" },
            { 1920, 1200, 4, RenderScale::Whole(2, 2), "1200p, integrated: 2x2.222 is over 4" },
            { 3024, 1898, 4, RenderScale::Whole(2, 2), "MacBook Pro fullscreen, M1 Pro: 2x2.232 is over 4" },
            { 3024, 1898, 9, RenderScale::Whole(3, 3), "MacBook Pro fullscreen, M3 Max: 3x3.347 is over 9" },
            { 2732, 2048, 1, RenderScale::Whole(1, 1), "iPad: budget 1" },
            { 2796, 1290, 1, RenderScale::Whole(1, 1), "iPhone: budget 1" },
            { 1280, 960, 4, { kOne, { 4, 3 } }, "a 4:3 picture on 1280x1024: 1280x960, 1:1, in place of 1x2's 1440 rows" },
        };
        for (const Case& c : cases)
        {
            const RenderScale s = AutoNativeScale(c.w, c.h, c.budget);
            printf("  %ux%u budget %u: %s (%s)\n", c.w, c.h, c.budget, gpu::ScaleName(s).c_str(), c.what);
            CHECK(s == c.want, "%s: %s, want %s", c.what, gpu::ScaleName(s).c_str(), gpu::ScaleName(c.want).c_str());
        }
    }

    if (g_failures)
    {
        printf("%d FAILED\n", g_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
