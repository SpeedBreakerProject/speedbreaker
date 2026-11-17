// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Where the GPU was when it stopped (GitHub #57, #99, #101: Intel integrated
// GPUs on Windows lose the device, VK_ERROR_DEVICE_LOST, after Windows resets
// a GPU that stopped answering). NFSMW_GPU_MARKERS (renderer.cpp): the
// renderer numbers each step of the GPU's work it records (a render pass, a
// resolve, a texture load, a copy, a clear; the presenter's frame) and the
// GPU itself writes that number into a small buffer the CPU reads, which
// stays readable after the device is lost:
//   - as a step begins, into a slot of its own (kBegunBase + id % kBegunSlots:
//     steps recorded back to back, with no barrier between them, never write
//     the same word);
//   - once the barrier after it has passed, into kDoneWord (one word: those
//     barriers order the writes);
//   - the presenter's frame (another command buffer, unordered with the
//     renderer's) into kPresentDoneWord after a barrier of its own.
// A device with VK_NV_device_diagnostic_checkpoints also gets the step's
// number as a checkpoint (and, at level 2, each draw's), which the driver
// reports per pipeline stage after the loss.
//
// This header is the CPU's side: the last steps recorded and what they were,
// the newest number among the GPU's words, and the report that names the
// step the GPU had finished and the one it had begun when it stopped. A
// marker write can still sit in a GPU cache when the GPU stops, so the
// report reads "about": one step either way.
#pragma once
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace gpu::markers
{
    enum class Kind : uint8_t { Pass, Resolve, Compute, Clear, Upload, Copy, Present };

    inline const char* KindName(Kind k)
    {
        switch (k)
        {
        case Kind::Pass: return "pass";
        case Kind::Resolve: return "resolve";
        case Kind::Compute: return "compute";
        case Kind::Clear: return "clear";
        case Kind::Upload: return "upload";
        case Kind::Copy: return "copy";
        case Kind::Present: return "present";
        }
        return "?";
    }

    // The marker buffer: 128 words (512 bytes).
    constexpr uint32_t kDoneWord = 0;
    constexpr uint32_t kPresentDoneWord = 1;
    constexpr uint32_t kBegunBase = 64;
    constexpr uint32_t kBegunSlots = 64;
    constexpr uint32_t kWords = kBegunBase + kBegunSlots;
    constexpr uint32_t BegunWord(uint32_t id) { return kBegunBase + id % kBegunSlots; }

    // Step numbers: 1, 2, ... in 32 bits, wrapping past 0 (0 = never written).
    constexpr uint32_t NextId(uint32_t id) { return id + 1 ? id + 1 : 1; }

    // How far `id` is behind `latest` (the newest one recorded), for
    // ordering (across the wrap it counts the skipped 0 too); UINT32_MAX for
    // 0 or a number from more than 2^31 steps ago (a GPU can't be ahead of
    // the CPU that recorded its work).
    constexpr uint32_t Behind(uint32_t latest, uint32_t id)
    {
        if (id == 0)
            return UINT32_MAX;
        uint32_t d = latest - id;
        return d < 0x80000000u ? d : UINT32_MAX;
    }

    // The newest number among `n` words (each read once), judged against
    // `latest`; 0 when none was written.
    inline uint32_t Newest(const volatile uint32_t* words, uint32_t n, uint32_t latest)
    {
        uint32_t best = 0, bestBehind = UINT32_MAX;
        for (uint32_t i = 0; i < n; i++)
        {
            uint32_t v = words[i];
            uint32_t b = Behind(latest, v);
            if (b < bestBehind)
                bestBehind = b, best = v;
        }
        return best;
    }

    // A checkpoint's marker (VK_NV_device_diagnostic_checkpoints): the step,
    // and 0 for the step itself or 1 + the draw's index in it.
    constexpr uint64_t Checkpoint(uint32_t step, uint32_t draw) { return uint64_t(step) << 20 | (draw & 0xFFFFFu); }
    constexpr uint32_t CheckpointStep(uint64_t marker) { return uint32_t(marker >> 20); }
    constexpr uint32_t CheckpointDraw(uint64_t marker) { return uint32_t(marker & 0xFFFFFu); }

    struct Step
    {
        uint32_t id = 0;
        Kind kind = Kind::Pass;
        uint64_t submission = 0, frame = 0;
        double at = 0;  // seconds into the session, when recorded
        uint32_t draws = 0;
        uint64_t firstVs = 0, firstPs = 0, lastVs = 0, lastPs = 0;
        char text[136] = {};
    };

    // Level 2's draws (each a checkpoint of its own).
    struct Draw
    {
        uint32_t seq = 0;              // 1, 2, ... over the session (the ring's key)
        uint32_t step = 0, index = 0;  // index: 1, 2, ... in its step
        uint64_t vs = 0, ps = 0;
        uint32_t count = 0;            // vertices or indices
        uint32_t primitive = 0;        // the guest's primitive type
    };

    // The last N steps (and draws) recorded, by number.
    template<typename T, uint32_t N>
    struct Ring
    {
        T items[N] = {};
        T& Put(uint32_t key) { T& t = items[key % N]; t = T{}; return t; }
        T* Find(uint32_t key, uint32_t T::*field) { T& t = items[key % N]; return key && t.*field == key ? &t : nullptr; }
        const T* Find(uint32_t key, uint32_t T::*field) const { const T& t = items[key % N]; return key && t.*field == key ? &t : nullptr; }
    };
    constexpr uint32_t kSteps = 2048;
    using StepRing = Ring<Step, kSteps>;
    constexpr uint32_t kDraws = 8192;
    using DrawRing = Ring<Draw, kDraws>;

    // A checkpoint the driver reported (`stage`: its pipeline stage's name)
    // as one line: the step, and the draw in it if it was one.
    inline std::string DescribeCheckpoint(const char* stage, uint64_t marker, const StepRing& steps, const DrawRing& draws)
    {
        const uint32_t step = CheckpointStep(marker), draw = CheckpointDraw(marker);
        std::string text = std::string(stage) + ": step " + std::to_string(step);
        const Step* s = steps.Find(step, &Step::id);
        text += s ? std::string(" (") + KindName(s->kind) + ")" : " (no longer in the record)";
        if (draw)
        {
            text += ", draw " + std::to_string(draw);
            if (s && s->draws)
                text += " of " + std::to_string(s->draws);
            for (const Draw& d : draws.items)
                if (d.seq && d.step == step && d.index == draw)
                {
                    char b[160];
                    snprintf(b, sizeof(b), " (vs_%016" PRIx64 " ps_%016" PRIx64 ", %u vertices, primitive %u)", d.vs, d.ps, d.count, d.primitive);
                    text += b;
                    break;
                }
        }
        else
            text += ", its start";
        return text;
    }

    inline std::string Describe(const Step& s)
    {
        char line[400];
        int n = snprintf(line, sizeof(line), "step %u: %s, submission %" PRIu64 ", frame %" PRIu64 ", %.2f s: %s", s.id, KindName(s.kind),
            s.submission, s.frame, s.at, s.text);
        if (s.kind == Kind::Pass && n > 0 && size_t(n) < sizeof(line))
        {
            if (s.draws == 0)
                snprintf(line + n, sizeof(line) - n, ", no draws");
            else if (s.draws == 1)
                snprintf(line + n, sizeof(line) - n, ", 1 draw (vs_%016" PRIx64 " ps_%016" PRIx64 ")", s.firstVs, s.firstPs);
            else
                snprintf(line + n, sizeof(line) - n, ", %u draws (the first vs_%016" PRIx64 " ps_%016" PRIx64 ", the last vs_%016" PRIx64
                    " ps_%016" PRIx64 ")", s.draws, s.firstVs, s.firstPs, s.lastVs, s.lastPs);
        }
        return line;
    }

    // What the GPU's words said.
    struct GpuState
    {
        uint32_t done = 0;         // kDoneWord
        uint32_t begun = 0;        // Newest over the begun slots
        uint32_t presentDone = 0;  // kPresentDoneWord
        uint32_t latest = 0;       // the CPU's newest step
    };

    inline GpuState Read(const volatile uint32_t* words, uint32_t latest)
    {
        GpuState g;
        g.latest = latest;
        g.done = words[kDoneWord];
        g.presentDone = words[kPresentDoneWord];
        g.begun = Newest(words + kBegunBase, kBegunSlots, latest);
        if (Behind(latest, g.done) == UINT32_MAX)
            g.done = 0;
        if (Behind(latest, g.presentDone) == UINT32_MAX)
            g.presentDone = 0;
        return g;
    }

    inline const char* Mark(const GpuState& g, const Step& s);

    // The report: one line on where the GPU was, then the steps around it
    // (`before` before the finished one, through `after` past the begun one),
    // each marked. Lines start with `prefix`.
    inline std::vector<std::string> Report(const StepRing& ring, const GpuState& g, const char* prefix, uint32_t before = 4, uint32_t after = 3)
    {
        std::vector<std::string> out;
        auto name = [&](uint32_t id) -> std::string {
            if (!id)
                return "none";
            const Step* s = ring.Find(id, &Step::id);
            return s ? std::to_string(id) + " (" + KindName(s->kind) + ")" : std::to_string(id) + " (no longer in the record)";
        };
        char head[512];
        snprintf(head, sizeof(head), "%sthe GPU had finished about step %s and begun about step %s; the CPU had recorded up to step %u%s",
            prefix, name(g.done).c_str(), name(g.begun).c_str(), g.latest,
            g.presentDone ? (", the presenter's last finished frame step " + std::to_string(g.presentDone)).c_str() : "");
        out.push_back(head);
        // From before the finished step (or the begun one) to past the begun one.
        uint32_t anchor = g.done ? g.done : g.begun;
        if (!anchor)
            return out;
        uint32_t last = Behind(g.latest, g.begun) < Behind(g.latest, anchor) ? g.begun : anchor;
        uint32_t first = anchor - before;
        uint32_t span = (last - first) + 1 + after;
        if (span > 64)
            span = 64;
        for (uint32_t k = 0, id = first; k < span; k++, id = NextId(id))
        {
            if (Behind(g.latest, id) == UINT32_MAX)
                break;
            const Step* s = ring.Find(id, &Step::id);
            if (!s)
                continue;
            out.push_back(std::string(prefix) + "  " + Describe(*s) + Mark(g, *s));
        }
        return out;
    }

    // A step's place against the GPU's words: finished (at or before the
    // done word: the presenter's own for the presenter's frames, which
    // carry no submission), begun and not finished (after it, at or before
    // `begun`), or not begun.
    inline const char* Mark(const GpuState& g, const Step& s)
    {
        const uint32_t id = s.id;
        const bool presenter = s.kind == Kind::Present && s.submission == 0;
        const uint32_t done = presenter ? g.presentDone : g.done;
        const bool finished = done && Behind(done, id) != UINT32_MAX;
        const bool begun = g.begun && Behind(g.begun, id) != UINT32_MAX;
        if (finished && presenter)
            return id == done ? "  <- finished (the presenter's newest finished)" : "  <- finished (the presenter's)";
        if (finished)
            return id == g.done ? "  <- finished (the newest finished)" : "";
        if (begun)
            return id == g.begun ? "  <- begun (the newest begun), not finished" : "  <- begun, not finished";
        return "  <- not begun";
    }
}
