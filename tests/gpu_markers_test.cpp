// Tests for gpu::markers (runtime/gpu/gpu_markers.h): GPU markers' numbers
// (wrapping past 0), which word the GPU writes for a step, the newest number
// among the GPU's words judged against the CPU's newest, the checkpoints'
// encoding, the record of steps and draws, and the report that names the
// step the GPU had finished and the one it had begun when it stopped (GitHub
// #57, #99, #101). renderer.cpp is read to check it uses them: a step begun
// before every pass, dispatch, copy and clear it records outside a pass, the
// done marker after each full, compute and narrow barrier, and a device loss
// (VK_ERROR_DEVICE_LOST from the fence, an event, the wait or a submit)
// reported instead of waited on.
// From the repo root:
//   clang++ -std=c++20 -O2 -Iruntime tests/gpu_markers_test.cpp -o build/gpu_markers_test
//   build/gpu_markers_test runtime/gpu/renderer.cpp
#include <gpu/gpu_markers.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace gpu::markers;

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void TestNumbers()
{
    CHECK(NextId(0) == 1, "%u", NextId(0));
    CHECK(NextId(1) == 2, "%u", NextId(1));
    CHECK(NextId(0xFFFFFFFEu) == 0xFFFFFFFFu, "%u", NextId(0xFFFFFFFEu));
    CHECK(NextId(0xFFFFFFFFu) == 1, "past the top it skips 0: %u", NextId(0xFFFFFFFFu));
    // Behind: 0 is never written; a number "ahead" of the CPU is an old lap.
    CHECK(Behind(100, 100) == 0, "%u", Behind(100, 100));
    CHECK(Behind(100, 90) == 10, "%u", Behind(100, 90));
    CHECK(Behind(100, 0) == UINT32_MAX, "0 is never written");
    CHECK(Behind(100, 101) == UINT32_MAX, "ahead of the CPU: stale");
    // (Across the wrap the difference counts 0 too, which no step has: it orders steps, it isn't a count.)
    CHECK(Behind(5, 0xFFFFFFF0u) == 21, "across the wrap: %u", Behind(5, 0xFFFFFFF0u));
    // The begun slots: back-to-back steps never share a word, and every
    // slot is inside the buffer, after the done words.
    for (uint32_t id = 1; id < 1000; id++)
    {
        CHECK(BegunWord(id) >= kBegunBase && BegunWord(id) < kWords, "step %u: word %u", id, BegunWord(id));
        CHECK(BegunWord(id) != BegunWord(id + 1), "steps %u and %u share a word", id, id + 1);
        CHECK(BegunWord(id) != kDoneWord && BegunWord(id) != kPresentDoneWord, "step %u", id);
    }
    CHECK(kWords * 4 == 512, "the buffer: %u bytes", kWords * 4);
}

static void TestNewest()
{
    uint32_t w[kBegunSlots] = {};
    CHECK(Newest(w, kBegunSlots, 50) == 0, "nothing written");
    for (uint32_t id = 1; id <= 70; id++)
        w[id % kBegunSlots] = id;
    CHECK(Newest(w, kBegunSlots, 70) == 70, "%u", Newest(w, kBegunSlots, 70));
    // The CPU has recorded further; the GPU is at 70.
    CHECK(Newest(w, kBegunSlots, 400) == 70, "%u", Newest(w, kBegunSlots, 400));
    // Across the wrap: the newest is 3, after 0xFFFFFFFF.
    uint32_t v[kBegunSlots] = {};
    uint32_t id = 0xFFFFFFF0u;
    for (int k = 0; k < 20; k++, id = NextId(id))
        v[id % kBegunSlots] = id;
    uint32_t newest = 0xFFFFFFF0u;
    for (int k = 0; k < 19; k++)
        newest = NextId(newest);
    CHECK(Newest(v, kBegunSlots, newest) == newest, "%u, want %u", Newest(v, kBegunSlots, newest), newest);
    CHECK(newest == 4, "%u", newest);

    // Read: the done word and the newest begun; stale words read as 0.
    uint32_t buffer[kWords] = {};
    buffer[kDoneWord] = 40;
    buffer[kPresentDoneWord] = 38;
    for (uint32_t s = 30; s <= 42; s++)
        buffer[BegunWord(s)] = s;
    GpuState g = Read(buffer, 60);
    CHECK(g.done == 40 && g.begun == 42 && g.presentDone == 38 && g.latest == 60, "%u %u %u %u", g.done, g.begun, g.presentDone, g.latest);
    buffer[kDoneWord] = 61;  // "ahead": not this session's
    g = Read(buffer, 60);
    CHECK(g.done == 0, "%u", g.done);
}

static void TestCheckpoints()
{
    CHECK(CheckpointStep(Checkpoint(1234, 0)) == 1234 && CheckpointDraw(Checkpoint(1234, 0)) == 0, "step only");
    CHECK(CheckpointStep(Checkpoint(1234, 77)) == 1234 && CheckpointDraw(Checkpoint(1234, 77)) == 77, "a draw");
    CHECK(CheckpointStep(Checkpoint(0xFFFFFFFFu, 0xFFFFF)) == 0xFFFFFFFFu && CheckpointDraw(Checkpoint(0xFFFFFFFFu, 0xFFFFF)) == 0xFFFFF,
        "the largest");
    // A pointer holds it (vkCmdSetCheckpointNV's marker).
    CHECK(sizeof(void*) < 8 || uint64_t(uintptr_t(reinterpret_cast<void*>(uintptr_t(Checkpoint(4000000000u, 5))))) == Checkpoint(4000000000u, 5),
        "round trip through a pointer");
}

static Step MakeStep(uint32_t id, Kind kind, const char* text, uint64_t submission)
{
    Step s;
    s.id = id;
    s.kind = kind;
    s.submission = submission;
    s.frame = 7;
    s.at = 4.5;
    snprintf(s.text, sizeof(s.text), "%s", text);
    return s;
}

static bool Has(const std::vector<std::string>& lines, const std::string& part)
{
    for (const std::string& l : lines)
        if (l.find(part) != std::string::npos)
            return true;
    return false;
}

static void TestReport()
{
    static StepRing ring;
    for (uint32_t id = 1; id <= 30; id++)
        ring.Put(id) = MakeStep(id, id % 3 ? Kind::Pass : Kind::Resolve, ("step text " + std::to_string(id)).c_str(), 100 + id / 10);
    // The record keeps the last kSteps; an older number is gone.
    CHECK(ring.Find(30, &Step::id) && ring.Find(30, &Step::id)->id == 30, "found");
    CHECK(!ring.Find(31, &Step::id), "not recorded");
    CHECK(!ring.Find(0, &Step::id), "0 is nothing");
    ring.Put(30 + kSteps) = MakeStep(30 + kSteps, Kind::Copy, "later", 9);
    CHECK(!ring.Find(30, &Step::id), "overwritten");
    ring.Put(30) = MakeStep(30, Kind::Pass, "step text 30", 103);

    // A pass's draws in its line.
    Step* p = ring.Find(20, &Step::id);
    p->draws = 3;
    p->firstVs = 0x1f555aba5feafbb5ull, p->firstPs = 0xece5410eb3b5981dull;
    p->lastVs = 0xaull, p->lastPs = 0xbull;
    std::string d = Describe(*p);
    CHECK(d.find("3 draws") != std::string::npos && d.find("vs_1f555aba5feafbb5") != std::string::npos && d.find("ps_000000000000000b") != std::string::npos,
        "%s", d.c_str());

    // The GPU finished 20 and began 22; the CPU recorded up to 30.
    GpuState g{ 20, 22, 0, 30 };
    std::vector<std::string> r = Report(ring, g, "[gpu] ");
    CHECK(!r.empty() && r[0].find("finished about step 20 (pass)") != std::string::npos && r[0].find("begun about step 22 (pass)") != std::string::npos &&
        r[0].find("up to step 30") != std::string::npos, "%s", r.empty() ? "" : r[0].c_str());
    CHECK(Has(r, "step 16:") && !Has(r, "step 15:"), "four before the finished one");
    CHECK(Has(r, "step 20: pass") && Has(r, "<- finished (the newest finished)"), "the finished one marked");
    CHECK(Has(r, "step 21: resolve") && Has(r, "<- begun, not finished"), "between: begun, not finished");
    CHECK(Has(r, "step 22: pass") && Has(r, "<- begun (the newest begun), not finished"), "the newest begun marked");
    CHECK(Has(r, "step 25:") && !Has(r, "step 26:"), "three past the begun one");
    CHECK(Has(r, "<- not begun"), "after: not begun");
    for (const std::string& l : r)
        CHECK(l.rfind("[gpu] ", 0) == 0, "prefix: %s", l.c_str());

    // Nothing finished yet (the first step of the session hangs).
    g = { 0, 1, 0, 30 };
    r = Report(ring, g, "");
    CHECK(r[0].find("finished about step none") != std::string::npos && r[0].find("begun about step 1 (") != std::string::npos, "%s", r[0].c_str());
    CHECK(Has(r, "step 1:") && Has(r, "<- begun (the newest begun), not finished"), "the first step");
    // Finished and begun the same: the GPU is between steps.
    g = { 24, 24, 23, 30 };
    r = Report(ring, g, "");
    CHECK(Has(r, "step 24:") && Has(r, "<- finished (the newest finished)") && !Has(r, "begun (the newest"), "between steps");
    CHECK(r[0].find("presenter's last finished frame step 23") != std::string::npos, "%s", r[0].c_str());
    // The presenter's frames (no submission) go by the presenter's own done word.
    ring.Put(26) = MakeStep(26, Kind::Present, "the presenter's frame", 0);
    ring.Put(27) = MakeStep(27, Kind::Present, "the presenter's frame", 0);
    g = { 25, 27, 27, 30 };
    r = Report(ring, g, "");
    bool presenterFinished = false, presenterBegun = false;
    for (const std::string& l : r)
    {
        presenterFinished |= l.find("step 27: present") != std::string::npos && l.find("<- finished (the presenter's newest finished)") != std::string::npos;
        presenterBegun |= l.find("step 27: present") != std::string::npos && l.find("begun") != std::string::npos;
    }
    CHECK(presenterFinished && !presenterBegun, "the presenter's frame 27 is finished by its own word");
    g = { 25, 27, 26, 30 };
    r = Report(ring, g, "");
    CHECK(Has(r, "step 26: present") && Has(r, "<- finished (the presenter's newest finished)") && Has(r, "<- begun (the newest begun), not finished"),
        "frame 26 finished, 27 begun");
    // No marker at all.
    g = { 0, 0, 0, 30 };
    r = Report(ring, g, "");
    CHECK(r.size() == 1 && r[0].find("step none and begun about step none") != std::string::npos, "%s", r[0].c_str());
    // A step not (or no longer) in the record.
    g = { 31, 31, 0, 40 };
    r = Report(ring, g, "");
    CHECK(r[0].find("31 (no longer in the record)") != std::string::npos, "%s", r[0].c_str());
}

static void TestCheckpointLines()
{
    static StepRing steps;
    static DrawRing draws;
    steps.Put(500) = MakeStep(500, Kind::Pass, "area 1280x720@0,0", 9);
    steps.Find(500, &Step::id)->draws = 12;
    Draw& dr = draws.Put(77);
    dr = { 77, 500, 9, 0x1111ull, 0x2222ull, 6, 4 };
    std::string a = DescribeCheckpoint("top of pipe", Checkpoint(500, 9), steps, draws);
    CHECK(a.find("top of pipe: step 500 (pass), draw 9 of 12") != std::string::npos && a.find("vs_0000000000001111") != std::string::npos &&
        a.find("6 vertices") != std::string::npos, "%s", a.c_str());
    std::string b = DescribeCheckpoint("bottom of pipe", Checkpoint(500, 0), steps, draws);
    CHECK(b.find("step 500 (pass), its start") != std::string::npos, "%s", b.c_str());
    std::string c = DescribeCheckpoint("stage 0x1", Checkpoint(9, 0), steps, draws);
    CHECK(c.find("no longer in the record") != std::string::npos, "%s", c.c_str());
}

// renderer.cpp uses them.
static void TestRenderer(const char* path)
{
    std::ifstream in(path);
    if (!in)
    {
        printf("  (renderer.cpp not given: its checks skipped)\n");
        return;
    }
    std::vector<std::string> lines;
    for (std::string l; std::getline(in, l);)
        lines.push_back(l);
    // Each GPU command recorded outside a pass has a step begun within the
    // lines before it (its descriptors, push constants and its region set up).
    const char* commands[] = { "vkCmdDispatch(s_cmd", "vkCmdCopyBufferToImage(s_cmd", "vkCmdCopyImage(s_cmd", "vkCmdCopyBuffer(s_cmd",
        "vkCmdCopyImageToBuffer(s_cmd", "vkCmdClearColorImage(s_cmd", "vkCmdClearDepthStencilImage(s_cmd", "vkCmdUpdateBuffer(s_cmd",
        "s_vk->cmdBeginRendering(s_cmd, &ri);" };
    int found = 0;
    for (size_t i = 0; i < lines.size(); i++)
        for (const char* c : commands)
            if (lines[i].find(c) != std::string::npos)
            {
                found++;
                bool step = false;
                for (size_t k = i >= 50 ? i - 50 : 0; k < i && !step; k++)
                    step = lines[k].find("StepBegin(") != std::string::npos;
                CHECK(step, "renderer.cpp:%zu: %s with no StepBegin in the 50 lines before", i + 1, c);
            }
    CHECK(found >= 25, "GPU commands found: %d", found);
    // The pass's step right before it begins (the main pass).
    std::string all;
    for (const std::string& l : lines)
        all += l + "\n";
    size_t pass = all.find("StepBegin(gpu::markers::Kind::Pass");
    size_t begin = pass == std::string::npos ? pass : all.find("s_vk->cmdBeginRendering(s_cmd, &ri);", pass);
    CHECK(pass != std::string::npos && begin != std::string::npos && begin - pass < 400, "the pass's step before cmdBeginRendering");
    // Each barrier that waits for what came before marks the done step.
    for (const char* fn : { "void FullBarrier()", "void NarrowBarrier(", "void ComputeBarrier()" })
    {
        size_t at = all.find(fn);
        size_t end = at == std::string::npos ? at : all.find("\n        }\n", at);
        CHECK(at != std::string::npos && end != std::string::npos && all.substr(at, end - at).find("StepDone();") != std::string::npos,
            "%s calls StepDone", fn);
    }
    // A lost device ends with the report, never another wait (GitHub #101).
    size_t ct = all.find("void CompletionThread()");
    size_t ctEnd = ct == std::string::npos ? ct : all.find("void WaitForSubmission(", ct);
    std::string body = ct != std::string::npos && ctEnd != std::string::npos ? all.substr(ct, ctEnd - ct) : "";
    CHECK(body.find("vkGetFenceStatus(s_dev, sub.fence)") != std::string::npos && body.find("status == VK_ERROR_DEVICE_LOST") != std::string::npos,
        "the fence's device loss");
    CHECK(body.find("event == VK_ERROR_DEVICE_LOST") != std::string::npos, "an event's device loss");
    CHECK(body.find("r == VK_ERROR_DEVICE_LOST") != std::string::npos, "the wait's device loss");
    CHECK(all.find("if (r == VK_ERROR_DEVICE_LOST)\n                    DeviceLost(what);") != std::string::npos, "Check() reports a lost device");
    CHECK(all.find("NFSMW_TEST_DEVICE_LOST") != std::string::npos, "the test switch");
}

int main(int argc, char** argv)
{
    TestNumbers();
    TestNewest();
    TestCheckpoints();
    TestReport();
    TestCheckpointLines();
    TestRenderer(argc > 1 ? argv[1] : "");
    if (g_failures)
    {
        printf("gpu_markers_test: %d FAILED\n", g_failures);
        return 1;
    }
    printf("gpu_markers_test: all passed\n");
    return 0;
}
