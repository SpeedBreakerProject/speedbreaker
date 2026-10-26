// Settings > Video > Internal Resolution's menu labels (video::ResolutionLabel,
// runtime/video/picture_fit.cpp): each entry names the size it renders for
// the picture on this screen, not a 16:9 screen's "720p". From the repo root:
//   clang++ -std=c++20 -O2 -Iruntime tests/resolution_label_test.cpp runtime/video/picture_fit.cpp -o build/resolution_label_test
//   build/resolution_label_test runtime/user/settings.cpp runtime/ui/settings_menu.cpp runtime/gpu/renderer.cpp
// The labels are the table's (read from settings.cpp, whose tokens, what
// settings.toml holds, are checked unchanged) at the scale the renderer
// takes for each entry (gpu::renderer::InternalResolutionScale is
// WantedRenderScale: video::NativeShape for a setting, video::AutoNativeScale
// for Auto, on the picture video::FrameRegion reports, FitPicture's):
//   - 16:9 (1920x1080): 1x (1280x720), 1.5x (1920x1080), 2x (2560x1440)...
//   - 16:10 with Fill Screen (a Steam Deck's 1280x800): 1x (1280x800),
//     1.5x (1920x1200), 2x (2560x1600); Auto (1280x800);
//   - 16:10 with Aspect Ratio 16:9: the 16:9 box's, 1x (1280x720);
//   - 21:9 (3440x1440, Hor+): 1x (1720x720), 2x (3440x1440).
// The sources are read to check the menu shows these labels (ValueText) and
// the renderer's scale for them is the one it renders with.
// Planted bugs this catches (each tried): the 16:9 box ignored with Aspect
// Ratio 16:9 (1280x800 there), the label's old "(720p)" kept beside the size,
// the menu's value column measured with FormatValue (the table's text).
#include <video/picture_fit.h>

#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using gpu::RenderScale;

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

static std::string Read(const char* path)
{
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// kResolutionEntries in settings.cpp: value -> { token, label }.
struct Entry
{
    std::string token, label;
};
static std::map<int, Entry> s_entries;

static void ReadEntries(const std::string& src)
{
    const size_t start = src.find("kResolutionEntries[] = {");
    CHECK(start != std::string::npos, "settings.cpp has no kResolutionEntries");
    if (start == std::string::npos)
        return;
    const size_t end = src.find("};", start);
    std::istringstream lines(src.substr(start, end - start));
    std::string line;
    while (std::getline(lines, line))
    {
        // { 2, "1x", "1x (720p)" },  or  { kResolutionAuto, "auto", "Auto" },
        const size_t open = line.find('{');
        const size_t q1 = line.find('"'), q2 = line.find('"', q1 + 1), q3 = line.find('"', q2 + 1), q4 = line.find('"', q3 + 1);
        if (open == std::string::npos || q4 == std::string::npos)
            continue;
        const std::string value = line.substr(open + 1, line.find(',', open) - open - 1);
        const int v = value.find("kResolutionAuto") != std::string::npos ? 0 : std::stoi(value);
        s_entries[v] = { line.substr(q1 + 1, q2 - q1 - 1), line.substr(q3 + 1, q4 - q3 - 1) };
    }
}

// The menu's text for entry `setting` on a screenW x screenH screen, Fill
// Screen (Aspect Ratio Auto) or 16:9, Auto within `budget`.
static std::string Label(int setting, uint32_t screenW, uint32_t screenH, bool fill, uint32_t budget)
{
    const video::PictureFit fit = video::FitPicture(screenW, screenH, 1280.0f, 720.0f, fill, fill);
    const uint32_t w = uint32_t(fit.width), h = uint32_t(fit.height);
    const RenderScale scale = setting > 0 ? video::NativeShape(gpu::AxisFromHalves(uint32_t(setting)), w, h) : video::AutoNativeScale(w, h, budget);
    return video::ResolutionLabel(s_entries[setting].label, scale);
}

static void Expect(const char* screen, uint32_t screenW, uint32_t screenH, bool fill, uint32_t budget, std::vector<std::pair<int, const char*>> want)
{
    printf("%s (%ux%u, %s, Auto budget %u):", screen, screenW, screenH, fill ? "Fill Screen" : "Aspect 16:9", budget);
    for (const auto& [setting, text] : want)
    {
        const std::string got = Label(setting, screenW, screenH, fill, budget);
        printf("  \"%s\"", got.c_str());
        CHECK(got == text, "%s, entry %d: \"%s\", want \"%s\"", screen, setting, got.c_str(), text);
    }
    printf("\n");
}

int main(int argc, char** argv)
{
    if (argc < 4)
    {
        printf("usage: resolution_label_test settings.cpp settings_menu.cpp renderer.cpp\n");
        return 2;
    }
    ReadEntries(Read(argv[1]));
    // The settings file's values are unchanged: only the labels moved.
    const std::map<int, const char*> tokens = { { 0, "auto" }, { 2, "1x" }, { 3, "1.5x" }, { 4, "2x" }, { 6, "3x" } };
    CHECK(s_entries.size() == tokens.size(), "%zu entries", s_entries.size());
    for (const auto& [v, token] : tokens)
        CHECK(s_entries.count(v) && s_entries[v].token == token, "entry %d's token \"%s\", want \"%s\"", v, s_entries[v].token.c_str(), token);

    // The name is the table's label up to " (".
    CHECK(video::ResolutionLabel("1x (720p)", RenderScale{ { 1, 1 }, { 10, 9 } }) == "1x (1280x800)", "the name");
    CHECK(video::ResolutionLabel("Auto", RenderScale{ { 1, 1 }, { 1, 1 } }) == "Auto (1280x720)", "a label without a size");

    // 16:9.
    Expect("16:9", 1920, 1080, true, 6,
        { { 2, "1x (1280x720)" }, { 3, "1.5x (1920x1080)" }, { 4, "2x (2560x1440)" }, { 6, "3x (3840x2160)" }, { 0, "Auto (2560x1440)" } });
    Expect("16:9 1440p", 2560, 1440, true, 6, { { 2, "1x (1280x720)" }, { 0, "Auto (2560x1440)" } });
    // 16:10 filled: a Steam Deck (an integrated GPU's budget, 4), a 16:10 Mac.
    Expect("16:10 Fill Screen (Steam Deck)", 1280, 800, true, 4,
        { { 2, "1x (1280x800)" }, { 3, "1.5x (1920x1200)" }, { 4, "2x (2560x1600)" }, { 6, "3x (3840x2400)" }, { 0, "Auto (1280x800)" } });
    Expect("16:10 Fill Screen", 1920, 1200, true, 6, { { 2, "1x (1280x800)" }, { 3, "1.5x (1920x1200)" } });
    // 16:10 with Aspect Ratio 16:9: the 16:9 box.
    Expect("16:10 Aspect 16:9 (Steam Deck)", 1280, 800, false, 4,
        { { 2, "1x (1280x720)" }, { 3, "1.5x (1920x1080)" }, { 4, "2x (2560x1440)" }, { 0, "Auto (1280x720)" } });
    // 21:9 (Hor+): its own columns.
    Expect("21:9", 3440, 1440, true, 6,
        { { 2, "1x (1720x720)" }, { 3, "1.5x (2580x1080)" }, { 4, "2x (3440x1440)" }, { 6, "3x (5160x2160)" }, { 0, "Auto (3440x1440)" } });
    // 21:9 with Aspect Ratio 16:9: its 16:9 box (2560x1440).
    Expect("21:9 Aspect 16:9", 3440, 1440, false, 6, { { 2, "1x (1280x720)" }, { 0, "Auto (2560x1440)" } });

    // The menu shows these (its value text and the column's width), and the
    // renderer's scale for a label is the one it renders with.
    const std::string menu = Read(argv[2]), renderer = Read(argv[3]);
    CHECK(menu.find("video::ResolutionLabel(text, gpu::renderer::InternalResolutionScale(") != std::string::npos, "the menu doesn't label it");
    CHECK(menu.find("CalcTextSize(ValueText(o, v)") != std::string::npos, "the value column isn't measured with the labels");
    CHECK(menu.find("std::string text = ValueText(o, stored);") != std::string::npos, "the row doesn't show the label");
    const size_t fn = renderer.find("gpu::RenderScale InternalResolutionScale(");
    CHECK(fn != std::string::npos && renderer.find("return WantedRenderScale(setting, regionW, regionH);", fn) < renderer.find("\n    }\n", fn),
        "InternalResolutionScale isn't WantedRenderScale");
    // WantedRenderScale is the test's composition: NativeShape for a
    // setting, NativeAuto over AutoRenderScale (AutoNativeScale) for Auto.
    CHECK(renderer.find("return s_nativeShape ? video::NativeShape(a, regionW, regionH)") != std::string::npos, "a setting's scale moved");
    CHECK(renderer.find("return s_nativeShape ? video::NativeAuto(sx, sy, regionW, regionH, s_scaleBudget)") != std::string::npos, "Auto's scale moved");

    printf(g_failures ? "%d FAILED\n" : "resolution labels OK\n", g_failures);
    return g_failures ? 1 : 0;
}
