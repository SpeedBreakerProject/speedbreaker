// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
// PM4 packet semantics follow Xenia (gpu/command_processor.cc, BSD license).
#include <stdafx.h>
#include "command_processor.h"

#include <kernel/vmem.h>
#include <cpu/guest_thread.h>
#include <cpu/host_cpu.h>
#include <cpu/guest_time.h>
#include <kernel/function.h>
#include <video/presenter.h>
#include <video/frame_rate.h>
#include <video/vblank_lock.h>
#include <platform/thermal.h>
#include <game/frame_time.h>
#include "renderer.h"
#include "gpu_clock.h"
#include "zpd_report.h"
#include <kernel/write_watch.h>
#include <debug/timeline.h>
#include <report/report.h>
#include <unordered_set>

namespace gpu
{
    namespace
    {
        // Registers the sync logic touches (Xenia's register_table.inc).
        constexpr uint32_t REG_CP_RB_WPTR = 0x01C5;
        constexpr uint32_t REG_SCRATCH_UMSK = 0x01DC;
        constexpr uint32_t REG_SCRATCH_ADDR = 0x01DD;
        constexpr uint32_t REG_SCRATCH_REG0 = 0x0578;
        constexpr uint32_t REG_SCRATCH_REG7 = 0x057F;
        constexpr uint32_t REG_COHER_STATUS_HOST = 0x0A31;
        constexpr uint32_t REG_VGT_EVENT_INITIATOR = 0x21F9;
        constexpr uint32_t REG_RB_SAMPLE_COUNT_ADDR = 0x2325;
        constexpr uint32_t REG_PA_SC_VIZ_QUERY_STATUS_0 = 0x0C44;
        constexpr uint32_t REG_PA_SC_VIZ_QUERY_STATUS_1 = 0x0C45;

        enum Opcode : uint32_t
        {
            PM4_ME_INIT = 0x48, PM4_NOP = 0x10, PM4_INDIRECT_BUFFER = 0x3F, PM4_INDIRECT_BUFFER_PFD = 0x37,
            PM4_WAIT_FOR_IDLE = 0x26, PM4_WAIT_REG_MEM = 0x3C, PM4_REG_RMW = 0x21, PM4_REG_TO_MEM = 0x3E,
            PM4_MEM_WRITE = 0x3D, PM4_COND_WRITE = 0x45, PM4_EVENT_WRITE = 0x46, PM4_EVENT_WRITE_SHD = 0x58,
            PM4_EVENT_WRITE_EXT = 0x5A, PM4_EVENT_WRITE_ZPD = 0x5B, PM4_DRAW_INDX = 0x22, PM4_DRAW_INDX_2 = 0x36,
            PM4_VIZ_QUERY = 0x23, PM4_SET_CONSTANT = 0x2D, PM4_SET_CONSTANT2 = 0x55, PM4_SET_SHADER_CONSTANTS = 0x56,
            PM4_LOAD_ALU_CONSTANT = 0x2F, PM4_IM_LOAD = 0x27, PM4_IM_LOAD_IMMEDIATE = 0x2B,
            PM4_INVALIDATE_STATE = 0x3B, PM4_SET_BIN_MASK = 0x50, PM4_SET_BIN_SELECT = 0x51,
            PM4_CONTEXT_UPDATE = 0x5E, PM4_INTERRUPT = 0x54, PM4_XE_SWAP = 0x64,
            PM4_SET_BIN_MASK_LO = 0x60, PM4_SET_BIN_MASK_HI = 0x61, PM4_SET_BIN_SELECT_LO = 0x62, PM4_SET_BIN_SELECT_HI = 0x63,
        };

        // Page-aligned, as is s_history: where the hot arrays fall within a
        // 4 KB page otherwise depends on every global linked before them, and
        // loads that share their low 12 address bits with a recent store wait
        // for it (4K aliasing: ls_bad_status2.stli_other). The play-test
        // report code's buffers moved them and the command processor went
        // from 12.5 to 15 ms in the fly-in, the same instructions at an IPC
        // of 0.94 instead of 1.28.
        alignas(4096) uint32_t s_regs[REGISTER_COUNT];

        std::atomic<uint32_t> s_primaryBuffer{ 0 };  // physical
        uint32_t s_primarySize = 0;                  // bytes
        uint32_t s_readIndex = 0;                    // dwords
        std::atomic<uint32_t> s_readWriteback{ 0 };  // physical, 0 = off
        std::atomic<uint32_t> s_interruptCallback{ 0 };
        std::atomic<uint32_t> s_interruptData{ 0 };
        std::atomic<uint64_t> s_frames{ 0 };
        std::atomic<uint64_t> s_draws{ 0 };
        std::atomic<uint32_t> s_counter{ 0 };        // Xenia's counter_: swaps + vblanks (two threads count)
        uint64_t s_binMask = ~0ull, s_binSelect = ~0ull;
        std::atomic<bool> s_started{ false };
        std::atomic<uint32_t> s_vblanks{ 0 };        // guest vblank interrupts raised
        // The WAIT_REG_MEM being polled past its first check, for the hang
        // watchdog (WaitingSince, DescribeState): since when (steady clock,
        // ns; 0: none), and what it waits for.
        std::atomic<int64_t> s_waitSince{ 0 };
        std::atomic<uint32_t> s_waitInfo{ 0 }, s_waitPoll{ 0 }, s_waitRef{ 0 }, s_waitMask{ 0 };

        // Busy time (renderer::g_cpBusyNs) is ring execution not blocked in a
        // wait, accounted at each batch's end and at each swap (mid-batch),
        // so a frame's share of it is exact for hitch reports.
        std::chrono::steady_clock::time_point s_busyFrom;
        uint64_t s_busyWaitFrom = 0;
        void AccountBusy()
        {
            auto now = std::chrono::steady_clock::now();
            uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now - s_busyFrom).count());
            renderer::g_cpBusyNs += ns - std::min(ns, renderer::g_cpWaitNs - s_busyWaitFrom);
            s_busyFrom = now;
            s_busyWaitFrom = renderer::g_cpWaitNs;
        }

        uint8_t* Physical(uint32_t address)
        {
            return static_cast<uint8_t*>(g_memory.Translate(vmem::Physical().Base() | (address & 0x1FFFFFFF)));
        }

        be<uint32_t>* MmioRegister(uint32_t index)
        {
            return static_cast<be<uint32_t>*>(g_memory.Translate(MMIO_BASE + index * 4));
        }

        uint32_t GpuSwap(uint32_t value, uint32_t endian)
        {
            switch (endian & 3)
            {
            case 1: return ((value << 8) & 0xFF00FF00) | ((value >> 8) & 0x00FF00FF);  // 8in16
            case 2: return ByteSwap(value);                                            // 8in32
            case 3: return (value >> 16) | (value << 16);                             // 16in32
            default: return value;
            }
        }

        // Stores `value` so that the guest (big-endian) reads it after the
        // GPU's endian swap, as xe::store(TranslatePhysical(...)) does in Xenia.
        // Guest stores by the command processor (and the completion thread)
        // go around the write-watch GPU guard: these threads never wait on
        // the GPU they feed.
        void GuestStore(uint32_t address, const void* data, size_t size)
        {
            writewatch::GpuThreadStore(Physical(address), data, size);
        }

        void StoreSwapped(uint32_t address, uint32_t value)
        {
            uint32_t v = GpuSwap(value, address & 3);
            GuestStore(address & ~3u, &v, 4);
        }

        uint32_t LoadSwapped(uint32_t address)
        {
            uint32_t v;
            memcpy(&v, Physical(address & ~3u), 4);
            return GpuSwap(v, address & 3);
        }

        // NFSMW_OCCLUSION_FAKE=<n> (diagnostic, never shipped): every
        // occlusion query END reports n samples passed, so nothing is ever
        // occluded (the sun glare's hypothesis test; 0 or unset: zeros). It
        // overrides NFSMW_OCCLUSION=1, whose [zpd] lines then count nothing:
        // logged.
        const uint32_t s_occlusionFake = [] {
            const char* v = std::getenv("NFSMW_OCCLUSION_FAKE");
            uint32_t samples = v ? uint32_t(std::strtoul(v, nullptr, 0)) : 0u;
            if (samples)
                fprintf(stderr, "[gpu] NFSMW_OCCLUSION_FAKE=%u: every occlusion query END reports %u samples (a diagnostic; "
                    "it overrides NFSMW_OCCLUSION=1)\n", samples, samples);
            return samples;
        }();

        // An END report of `samples`, in the order D3D's GetData needs
        // (gpu/zpd_report.h: B lanes, ZPass_A, then the other A lanes).
        void StoreZpdEnd(uint32_t address, uint32_t samples)
        {
            uint8_t* host = Physical(address);
            writewatch::GpuThreadWrite(host, 32, [host, samples] {
                zpd::StoreEnd(samples, [host](uint32_t offset, uint32_t value) { memcpy(host + offset, &value, 4); });
            });
        }

        // Memory stores of the command stream are deferred to GPU completion
        // (renderer::QueueWrite). The ones still pending are tracked, so that
        // a WAIT_REG_MEM on a value the stream itself will store (D3D's
        // pipeline-drain idiom: EVENT_WRITE_SHD then wait for it) can go on
        // at once: everything recorded after it executes after it in queue
        // order, and its CPU-visible effects are deferred behind it too.
        // Waiting instead drains the whole host GPU queue (5-10 ms, several
        // times a frame). NFSMW_SELF_WAITS=0 waits like the hardware.
        struct PendingStore { uint32_t value; uint64_t sequence; };
        std::mutex s_pendingMutex;
        std::unordered_map<uint32_t, PendingStore> s_pendingStores;  // by physical address
        uint64_t s_pendingSequence = 0;
        const bool s_selfWaits = [] { const char* v = std::getenv("NFSMW_SELF_WAITS"); return !v || v[0] != '0'; }();

        uint64_t AddPending(uint32_t address, uint32_t value)
        {
            std::lock_guard lock(s_pendingMutex);
            uint64_t sequence = ++s_pendingSequence;
            s_pendingStores[address & 0x1FFFFFFC] = { value, sequence };
            return sequence;
        }

        void ClearPending(uint32_t address, uint64_t sequence)
        {
            std::lock_guard lock(s_pendingMutex);
            auto it = s_pendingStores.find(address & 0x1FFFFFFC);
            if (it != s_pendingStores.end() && it->second.sequence == sequence)
                s_pendingStores.erase(it);
        }

        void QueueStores(uint32_t address, std::vector<uint32_t> values)
        {
            std::vector<uint64_t> sequences(values.size());
            {
                std::lock_guard lock(s_pendingMutex);
                for (size_t i = 0; i < values.size(); i++)
                {
                    sequences[i] = ++s_pendingSequence;
                    s_pendingStores[(address + uint32_t(i) * 4) & 0x1FFFFFFC] = { values[i], sequences[i] };
                }
            }
            renderer::QueueWrite([address, values = std::move(values), sequences = std::move(sequences)] {
                std::lock_guard lock(s_pendingMutex);
                for (size_t i = 0; i < values.size(); i++)
                {
                    uint32_t a = address + uint32_t(i) * 4;
                    StoreSwapped(a, values[i]);
                    auto it = s_pendingStores.find(a & 0x1FFFFFFC);
                    if (it != s_pendingStores.end() && it->second.sequence == sequences[i])
                        s_pendingStores.erase(it);
                }
            });
        }

        void QueueStore(uint32_t address, uint32_t value)
        {
            QueueStores(address, { value });
        }

        // The value the stream will have stored at `address` once the GPU
        // catches up, if a store to it is pending.
        std::optional<uint32_t> PendingValue(uint32_t address)
        {
            std::lock_guard lock(s_pendingMutex);
            auto it = s_pendingStores.find(address & 0x1FFFFFFC);
            if (it == s_pendingStores.end())
                return std::nullopt;
            return it->second.value;
        }

        void WriteRegister(uint32_t index, uint32_t value)
        {
            if (index >= REGISTER_COUNT)
                return;
            s_regs[index] = value;
            if (index >= 0x4000 && index < 0x4928)  // float/fetch/bool/loop constants
                renderer::g_constantsDirty.store(true, std::memory_order_relaxed);
            if (index >= REG_SCRATCH_REG0 && index <= REG_SCRATCH_REG7)
            {
                // Scratch registers are fences: the CPU sees them (MMIO and
                // the optional memory copy) once the work before is done.
                uint32_t n = index - REG_SCRATCH_REG0;
                bool toMemory = ((1u << n) & s_regs[REG_SCRATCH_UMSK]) != 0;
                uint32_t address = s_regs[REG_SCRATCH_ADDR] + n * 4;
                uint64_t sequence = toMemory ? AddPending(address, value) : 0;
                renderer::QueueWrite([=] {
                    if (toMemory)
                    {
                        be<uint32_t> big = value;
                        GuestStore(address, &big, 4);
                        ClearPending(address, sequence);
                    }
                    *MmioRegister(index) = value;
                });
                return;
            }
            else if (index == REG_COHER_STATUS_HOST)
            {
                s_regs[index] |= 0x80000000u;
            }
            if (index != REG_CP_RB_WPTR)  // the game owns that one
                *MmioRegister(index) = s_regs[index];
        }

        // A run of registers WriteRegister has nothing special for (it treats
        // only the scratch fences, COHER_STATUS_HOST and the write pointer
        // specially) can be written in bulk.
        bool BulkSafe(uint32_t base, uint32_t count)
        {
            uint32_t end = base + count;
            if (!(renderer::g_cpOpt & renderer::CP_OPT_BULK_REGISTERS) || end > REGISTER_COUNT)
                return false;
            auto hits = [&](uint32_t lo, uint32_t hi) { return base <= hi && end > lo; };
            return !hits(REG_CP_RB_WPTR, REG_CP_RB_WPTR) && !hits(REG_SCRATCH_REG0, REG_SCRATCH_REG7) &&
                !hits(REG_COHER_STATUS_HOST, REG_COHER_STATUS_HOST);
        }

        // WriteRegister for `count` registers from `src`, big-endian dwords
        // (the ring's or guest memory's bytes, which is also how the MMIO
        // mirror holds them). A heavy frame writes ~600k registers, mostly
        // shader constants in runs.
        void WriteRegistersBulk(uint32_t base, const uint8_t* src, uint32_t count)
        {
            memcpy(MmioRegister(base), src, size_t(count) * 4);
            for (uint32_t m = 0; m < count; m++)
            {
                uint32_t v;
                memcpy(&v, src + size_t(m) * 4, 4);
                s_regs[base + m] = ByteSwap(v);
            }
            if (base < 0x4928 && base + count > 0x4000)  // float/fetch/bool/loop constants
                renderer::g_constantsDirty.store(true, std::memory_order_relaxed);
        }

        // One host thread per interrupt source, each with its own guest
        // context, as Xenia runs the callback on the GPU/vsync threads.
        void DispatchInterrupt(PPCContext& ctx, uint32_t source, uint32_t cpu)
        {
            timeline::Mark(timeline::Interrupt, source | cpu << 8);
            uint32_t callback = s_interruptCallback;
            if (callback == 0)
                return;
            if (ImportTraceEnabled())
                fprintf(stderr, "[trace t%u] GPU interrupt source %u cpu %u -> %08X\n",
                    GuestThread::GetCurrentThreadId(), source, cpu, callback);
            // Run the handler "on" the target CPU, as Xenia's SetActiveCpu does:
            // the PCR's processor number (r13 + 0x10C) selects D3D's per-CPU state.
            *static_cast<uint8_t*>(g_memory.Translate(ctx.r13.u32 + 0x10C)) = uint8_t(cpu);
            ctx.r3.u64 = source;
            ctx.r4.u64 = s_interruptData;
            g_memory.FindFunction(callback)(ctx, g_memory.base);
        }

        struct Reader
        {
            uint8_t* base;
            uint32_t size;    // bytes (ring size for the primary buffer)
            uint32_t offset;  // bytes
            uint32_t end;     // bytes (write offset)

            uint32_t Available() const { return (end + size - offset) % size + (end == offset ? 0 : 0); }
            uint32_t Read()
            {
                uint32_t v;
                memcpy(&v, base + offset, 4);
                offset += 4;
                if (offset >= size)
                    offset = 0;
                return ByteSwap(v);
            }
            void Skip(uint32_t dwords) { offset = (offset + dwords * 4) % size; }
            uint8_t* Here() const { return base + offset; }
            // The next `dwords` as one contiguous run (nullptr if they wrap);
            // consumes them.
            const uint8_t* Take(uint32_t dwords)
            {
                if (offset + dwords * 4 > size)
                    return nullptr;
                const uint8_t* p = base + offset;
                offset += dwords * 4;
                if (offset >= size)
                    offset = 0;
                return p;
            }
        };

        void ExecuteBuffer(Reader& r, PPCContext& ctx);

        // NFSMW_DUMP_SHADERS=<dir>: save each distinct shader's microcode
        // (guest byte order) as <dir>/<vs|ps>_<hash>.bin for offline work.
        void DumpShader(uint32_t type, const uint32_t* words, uint32_t dwords)
        {
            static const char* dir = std::getenv("NFSMW_DUMP_SHADERS");
            if (dir == nullptr || dwords == 0)
                return;
            static std::mutex mutex;
            static std::unordered_set<uint64_t> seen;
            uint64_t h = 0xCBF29CE484222325ull ^ type;
            for (uint32_t i = 0; i < dwords; i++)
                h = (h ^ words[i]) * 0x100000001B3ull;
            std::lock_guard lock(mutex);
            if (!seen.insert(h).second)
                return;
            char path[512];
            snprintf(path, sizeof(path), "%s/%s_%016llx.bin", dir, type == 0 ? "vs" : "ps", (unsigned long long)h);
            if (FILE* f = fopen(path, "wb"))
            {
                fwrite(words, 4, dwords, f);
                fclose(f);
            }
        }

        // Last packets executed, for diagnosing stuck waits.
        struct PacketRecord { uint32_t header; uint32_t args[4]; };
        alignas(4096) PacketRecord s_history[64];  // (see s_regs)
        uint32_t s_historyPos = 0;

        void Record(uint32_t header, const Reader& r)
        {
            PacketRecord& rec = s_history[s_historyPos++ % 64];
            rec.header = header;
            uint32_t count = (header >> 30) == 3 ? ((header >> 16) & 0x3FFF) + 1 : 0;
            uint32_t n = std::min(count, 4u);
            if (r.offset + n * 4 <= r.size)
            {
                for (uint32_t i = 0; i < 4; i++)
                {
                    uint32_t v = 0;
                    if (i < n)
                        memcpy(&v, r.base + r.offset + i * 4, 4);
                    rec.args[i] = ByteSwap(v);
                }
                return;
            }
            Reader peek = r;
            for (uint32_t i = 0; i < 4; i++)
                rec.args[i] = i < count ? peek.Read() : 0;
        }

        // (Also read by a hang report on the watchdog's thread, racily: by
        // then this thread is stuck and the records hold still.)
        std::string FormatHistory()
        {
            std::string text = "[gpu] last packets (oldest first):\n";
            for (uint32_t i = 0; i < 64; i++)
            {
                const PacketRecord& rec = s_history[(s_historyPos + i) % 64];
                if (rec.header == 0)
                    continue;
                uint32_t type = rec.header >> 30;
                if (type == 3)
                    text += std::format("  type3 op {:02X} count {}: {:08X} {:08X} {:08X} {:08X}\n", (rec.header >> 8) & 0x7F,
                        ((rec.header >> 16) & 0x3FFF) + 1, rec.args[0], rec.args[1], rec.args[2], rec.args[3]);
                else if (type == 0)
                    text += std::format("  type0 reg {:04X} count {}\n", rec.header & 0x7FFF, ((rec.header >> 16) & 0x3FFF) + 1);
            }
            return text;
        }

        void DumpHistory()
        {
            fputs(FormatHistory().c_str(), stderr);
        }

        bool ExecuteType3(Reader& r, uint32_t packet, PPCContext& ctx)
        {
            uint32_t opcode = (packet >> 8) & 0x7F;
            uint32_t count = ((packet >> 16) & 0x3FFF) + 1;

            if ((packet & 1) && ((s_binSelect & s_binMask) == 0 || opcode == PM4_XE_SWAP))
            {
                r.Skip(count);  // predicated packet that doesn't apply
                return true;
            }

            auto compare = [](uint32_t func, uint32_t value, uint32_t ref) {
                switch (func & 7)
                {
                case 1: return value < ref;
                case 2: return value <= ref;
                case 3: return value == ref;
                case 4: return value != ref;
                case 5: return value >= ref;
                case 6: return value > ref;
                case 7: return true;
                default: return false;
                }
            };

            switch (opcode)
            {
            case PM4_ME_INIT:
            case PM4_NOP:
            case PM4_INVALIDATE_STATE:
            case PM4_CONTEXT_UPDATE:
            case PM4_WAIT_FOR_IDLE:  // we execute in order; idle is implied
                r.Skip(count);
                return true;

            case PM4_INTERRUPT:
            {
                // Raised when the GPU reaches this point: dispatched by the
                // renderer's completion thread (with a guest context of its
                // own) once the work before it has finished, not by draining.
                uint32_t cpuMask = r.Read();
                r.Skip(count - 1);
                renderer::QueueWrite([cpuMask] {
                    // Runs on the completion thread, or right here when the GPU
                    // is idle (this thread already has a guest context).
                    PPCContext* guest = GetPPCContext();
                    if (!guest)
                    {
                        static thread_local GuestThreadContext* interruptThread = new GuestThreadContext(2);
                        guest = &interruptThread->ppcContext;
                    }
                    for (uint32_t n = 0; n < 6; n++)
                        if (cpuMask & (1u << n))
                            DispatchInterrupt(*guest, 1, n);
                });
                return true;
            }

            case PM4_XE_SWAP:
            {
                r.Read();  // 'SWAP'
                r.Read();  // front buffer (physical); also in the fetch constant
                uint32_t width = r.Read(), height = r.Read();
                r.Skip(count - 4);
                // VdSwap wrote the front buffer's fetch constant into fetch slot 0.
                // Presented from the GPU without waiting; frame dumps (and the
                // headless case) take the CPU path, which needs the GPU idle.
                static const bool dumping = std::getenv("NFSMW_DUMP_FRAMES") != nullptr;
                if (dumping || !renderer::PresentFrontBuffer(&s_regs[0x4800], width, height))
                {
                    renderer::Flush();
                    video::SubmitFrontBuffer(&s_regs[0x4800], width, height);
                }
                s_counter++;
                timeline::Mark(timeline::Swap);
                uint64_t frame = ++s_frames;
                renderer::OnSwap(frame);
                // Every 120 frames: frame rate and per-frame costs since the last line.
                // Frame times are guest time (guesttime::NowNs: the host's
                // on desktop), so a frame across a suspension isn't a hitch
                // and the frame rate counts only the time the game ran.
                static int64_t lastTime = guesttime::NowNs();
                static renderer::Stats last{};
                static uint64_t lastDraws = 0;
                static writewatch::GuardStats lastGuard{};
                // Stutter: the longest gap between two swaps in the window.
                static int64_t lastSwap = guesttime::NowNs();
                static int64_t overlayTime = guesttime::NowNs();  // the last 30-swap mark
                static double worstMs = 0;
                {
                    AccountBusy();
                    int64_t swapNow = guesttime::NowNs();
                    double frameMs = double(swapNow - lastSwap) / 1e6;
                    worstMs = std::max(worstMs, frameMs);
                    static uint32_t lastVblanks = 0;
                    uint32_t vblanks = s_vblanks.load();
                    renderer::HitchReport(frame, frameMs, vblanks - lastVblanks);
                    lastVblanks = vblanks;
                    lastSwap = swapNow;
                }
                if (frame % 30 == 0)
                {
                    static uint64_t overlayBusyNs = 0, overlayDraws = 0, overlayGpuUs = 0;
                    // GPU: execution time from timestamps where the device has them.
                    auto st = renderer::GetStats();
                    uint64_t gpuUs = st.gpuExecUs ? st.gpuExecUs : st.gpuBusyUs, draws = s_draws.load();
                    float cpMs = float(double(renderer::g_cpBusyNs - overlayBusyNs) / 1e6 / 30);
                    float gpuMs = float(double(gpuUs - overlayGpuUs) / 1000.0 / 30);
                    float drawsPerFrame = float(double(draws - overlayDraws) / 30);
                    renderer::PublishPerfNumbers({ cpMs, gpuMs, drawsPerFrame });
                    overlayBusyNs = renderer::g_cpBusyNs;
                    overlayDraws = draws;
                    overlayGpuUs = gpuUs;
                    // The same numbers, with the game time they took, for
                    // Frame Rate's Auto (every half second at 60).
                    int64_t costNow = guesttime::NowNs();
                    video::NoteFrameCosts({ double(costNow - overlayTime) / 1e9, 30, cpMs, gpuMs, drawsPerFrame });
                    overlayTime = costNow;
                }
                if (frame % 120 == 0)
                {
                    auto st = renderer::GetStats();
                    auto guard = writewatch::GetGuardStats();
                    // The scaled data path, and the presenter's GPU time per
                    // game frame it post-processed (NFSMW_GPU_TIMING=1): with
                    // the renderer's executing time, the whole GPU frame.
                    auto scaled = renderer::GetScaledStats();
                    auto memory = renderer::GetMemoryStats();
                    auto post = video::GetPostTiming();
                    static renderer::ScaledStats lastScaled{};
                    static renderer::MemoryStats lastMemory{};
                    static video::PostTiming lastPost{};
                    double postFrames = double(std::max<uint64_t>(post.frames - lastPost.frames, 1));
                    int64_t now = guesttime::NowNs();
                    double secs = double(now - lastTime) / 1e9;
                    // CP busy: time in ring batches not blocked on the GPU or the game.
                    static uint64_t lastBusyNs = 0;
                    double cpBusyMs = double(renderer::g_cpBusyNs - lastBusyNs) / 1e6 / 120;
                    double drawsPerFrame = double(s_draws.load() - lastDraws) / 120;
                    lastBusyNs = renderer::g_cpBusyNs;
                    // On screen: frames the presenter dropped or kept up
                    // past their vblanks; the world's steps per game frame.
                    static video::PresentStats lastShown{};
                    static game::FrameTimeStats lastWorld{};
                    auto shown = video::GetPresentStats();
                    auto world = game::GetFrameTimeStats();
                    // The GPU's average clock, where the kernel keeps it (the Frame).
                    double mhz = gpu::clock::AverageMHzSinceLast();
                    std::string clockText = mhz > 0.0 ? std::format(", at {:.0f} MHz", mhz) : std::string();
                    // At the end, so parsers of the fields before are untouched:
                    // the device's thermal state (Apple, or NFSMW_THERMAL_SIM),
                    // and Frame Rate Auto's rate.
                    platform::thermal::State thermal = platform::thermal::Current();
                    std::string tailText = (thermal == platform::thermal::State::Unknown ? std::string()
                        : std::format(" | thermal {}", platform::thermal::Name(thermal))) + video::AutoFrameRatePerfText();
                    // NFSMW_WATCH_SUBPAGES in use: its snapshots (write_watch.cpp).
                    // Pool-full evictions, whole-page fallbacks and guest pages
                    // marked for a GPU read are where it loses its gain (which,
                    // for a free-roam run); untile waits, what it costs instead
                    // (gave up: the wait hit its bound, see WaitForGpu).
                    std::string subpagesText = !guard.subpages ? std::string()
                        : std::format(", subpages: {:.1f} opened, {:.1f} clean, {:.1f} changed, {:.1f} evicted ({:.1f} pool full), {:.1f} whole (GPU),"
                            " {:.1f} marked (GPU read), {:.2f} untile waits ({:.2f} gave up)",
                            double(guard.snapOpened - lastGuard.snapOpened) / 120, double(guard.snapClean - lastGuard.snapClean) / 120,
                            double(guard.snapChanged - lastGuard.snapChanged) / 120, double(guard.snapEvicted - lastGuard.snapEvicted) / 120,
                            double(guard.snapFull - lastGuard.snapFull) / 120, double(guard.snapWhole - lastGuard.snapWhole) / 120,
                            double(guard.snapReading - lastGuard.snapReading) / 120, double(guard.snapWaits - lastGuard.snapWaits) / 120,
                            double(guard.snapGaveUp - lastGuard.snapGaveUp) / 120);
                    fprintf(stderr, "[perf] %.1f fps (worst frame %.1f ms) | CP busy %.1f ms/frame (%.2f us/draw) | per frame: %.0f draws (%.0f skipped as later tiles, %.0f constant uploads), %.1f passes, %.1f resolves, %.1f submits, %.1f uploads, GPU busy %.1f ms (executing %.1f ms%s) | guard: %.1f faults (%.1f opened), %.2f stalls, %.2f ms stalled%s | %.0f KB snapshots | shadow: %.0f KB up, %.0f KB back, %.2f waits"
                        " | scaled: %.1f loads, %.0f MB untiled, %.0f MB resolved, %.2f allocations, %.2f evictions, %.2f fallbacks | post-processing GPU %.2f ms (AA %.2f ms)"
                        " | display: %llu dropped, %llu repeated | world: %llu frames, %llu without a step, %llu with 2+ | late %llu"
                        " | memory: %.0f MB (targets %.0f in %u, textures %.0f in %u, scaled %.0f, front %.0f), device %.0f of %.0f MB,"
                        " image blocks %u (%.0f MB: %.0f MB in %u images), %u spare (%.0f MB), %u images alone (%.0f MB)"
                        " | images: %llu made (%.0f us each), %llu allocations (%.1f ms); blocks: %llu from the spare, %llu allocated here,"
                        " %llu by the memory thread (%.1f ms each), %llu freed%s\n",
                        120.0 / secs, worstMs, cpBusyMs, drawsPerFrame > 0 ? cpBusyMs * 1000.0 / drawsPerFrame : 0.0,
                        drawsPerFrame, double(st.tileSkips - last.tileSkips) / 120,
                        double(st.constantUploads - last.constantUploads) / 120,
                        double(st.passes - last.passes) / 120, double(st.resolves - last.resolves) / 120,
                        double(st.submits - last.submits) / 120, double(st.uploads - last.uploads) / 120,
                        double(st.gpuBusyUs - last.gpuBusyUs) / 1000.0 / 120, double(st.gpuExecUs - last.gpuExecUs) / 1000.0 / 120,
                        clockText.c_str(), double(guard.faults - lastGuard.faults) / 120, double(guard.opened - lastGuard.opened) / 120, double(guard.stalls - lastGuard.stalls) / 120,
                        double(guard.stallUs - lastGuard.stallUs) / 1000.0 / 120, subpagesText.c_str(),
                        double(st.snapshotBytes - last.snapshotBytes) / 1024.0 / 120,
                        double(guard.uploadBytes - lastGuard.uploadBytes) / 1024.0 / 120, double(guard.readbackBytes - lastGuard.readbackBytes) / 1024.0 / 120,
                        double(st.shadowWaits - last.shadowWaits) / 120, double(scaled.loads - lastScaled.loads) / 120,
                        double(scaled.untiledBytes - lastScaled.untiledBytes) / 1048576.0 / 120,
                        double(scaled.resolvedBytes - lastScaled.resolvedBytes) / 1048576.0 / 120,
                        double(scaled.allocations - lastScaled.allocations) / 120, double(scaled.evictions - lastScaled.evictions) / 120,
                        double(scaled.fallbacks - lastScaled.fallbacks) / 120, double(post.totalNs - lastPost.totalNs) / 1e6 / postFrames,
                        double(post.aaNs - lastPost.aaNs) / 1e6 / postFrames,
                        (unsigned long long)(shown.dropped - lastShown.dropped), (unsigned long long)(shown.repeated - lastShown.repeated),
                        (unsigned long long)(world.frames - lastWorld.frames), (unsigned long long)(world.noStep - lastWorld.noStep),
                        (unsigned long long)(world.multiStep - lastWorld.multiStep), (unsigned long long)(st.lateFrames - last.lateFrames),
                        double(memory.targets + memory.textures + memory.scaled + memory.front) / 1048576.0, double(memory.targets) / 1048576.0,
                        memory.targetCount, double(memory.textures) / 1048576.0, memory.textureCount, double(memory.scaled) / 1048576.0,
                        double(memory.front) / 1048576.0, double(memory.deviceUsed) / 1048576.0, double(memory.deviceBudget) / 1048576.0,
                        memory.blocks, double(memory.blockBytes) / 1048576.0, double(memory.rangeBytes) / 1048576.0, memory.ranges,
                        memory.spares, double(memory.spareBytes) / 1048576.0, memory.own, double(memory.ownBytes) / 1048576.0,
                        (unsigned long long)(memory.images - lastMemory.images),
                        memory.images > lastMemory.images ? double(memory.imageNs - lastMemory.imageNs) / 1000.0 / double(memory.images - lastMemory.images) : 0.0,
                        (unsigned long long)(memory.allocations - lastMemory.allocations), double(memory.allocationNs - lastMemory.allocationNs) / 1e6,
                        (unsigned long long)(memory.fromSpare - lastMemory.fromSpare), (unsigned long long)(memory.blocksHere - lastMemory.blocksHere),
                        (unsigned long long)(memory.spareBlocks - lastMemory.spareBlocks),
                        memory.spareBlocks > lastMemory.spareBlocks ? double(memory.spareNs - lastMemory.spareNs) / 1e6 / double(memory.spareBlocks - lastMemory.spareBlocks) : 0.0,
                        (unsigned long long)(memory.blocksFreed - lastMemory.blocksFreed), tailText.c_str());
                    lastMemory = memory;
                    lastShown = shown;
                    lastWorld = world;
                    lastGuard = guard;
                    lastScaled = scaled;
                    lastPost = post;
                    worstMs = 0;
                    lastTime = now;
                    last = st;
                    lastDraws = s_draws.load();
                }
                if (frame == 1 || frame % 300 == 0)
                {
                    auto st = renderer::GetStats();
                    fprintf(stderr, "[gpu] frame %llu (%llu draws so far; rendered %llu, skipped %llu, resolves %llu, "
                        "submits %llu, pipelines %llu, uploads %llu, shaders from cache %llu)\n", (unsigned long long)frame, (unsigned long long)s_draws.load(),
                        (unsigned long long)st.draws, (unsigned long long)st.skipped, (unsigned long long)st.resolves,
                        (unsigned long long)st.submits, (unsigned long long)st.pipelines, (unsigned long long)st.uploads,
                        (unsigned long long)st.shaderCacheHits);
                }
                return true;
            }

            case PM4_INDIRECT_BUFFER:
            case PM4_INDIRECT_BUFFER_PFD:
            {
                uint32_t ptr = r.Read() & 0x1FFFFFFF;
                uint32_t length = r.Read() & 0xFFFFF;
                r.Skip(count - 2);
                Reader ib{ Physical(ptr), length * 4 + 4, 0, length * 4 };  // +4: never wrap
                ExecuteBuffer(ib, ctx);
                return true;
            }

            case PM4_WAIT_REG_MEM:
            {
                uint32_t info = r.Read(), poll = r.Read(), ref = r.Read(), mask = r.Read(), wait = r.Read();
                r.Skip(count - 5);
                bool memory = (info & 0x10) != 0;
                std::optional<renderer::CpWaitTimer> waitTimer;  // from the first poll after the submit
                auto waitStart = std::chrono::steady_clock::now();
                // Suspensions so far: one that begins during the wait, even
                // one spent parked in the GPU gate inside Submit() (it closes
                // after the count moves), restarts the wait's clock.
                uint64_t waitEpoch = guesttime::Epoch();
                bool reported = false, submitted = false;
                // NFSMW_TEST_HANG: no wait passes (report/watchdog.cpp).
                if (memory && s_selfWaits && !report::g_testHang.load(std::memory_order_relaxed))
                    if (auto pending = PendingValue(poll); pending && compare(info, *pending & mask, ref))
                        return true;
                while (true)
                {
                    uint32_t value = memory ? LoadSwapped(poll) : s_regs[poll % REGISTER_COUNT];
                    if (!memory && poll == REG_COHER_STATUS_HOST)
                    {
                        s_regs[poll] = 0;  // MakeCoherent: nothing to flush in a null GPU
                        value = 0;
                    }
                    if (compare(info, value & mask, ref) && !report::g_testHang.load(std::memory_order_relaxed))
                    {
                        if (submitted)
                        {
                            timeline::Mark(timeline::RegMemEnd, value);
                            s_waitSince.store(0, std::memory_order_relaxed);
                        }
                        break;
                    }
                    // Deferred fence writes may be what the CPU is waiting for
                    // before it produces what we wait for: get them moving.
                    // (Most of these waits, MakeCoherent's, pass at once and
                    // submitted nothing: ~30 command buffers a frame.)
                    if (!submitted)
                    {
                        submitted = true;
                        timeline::Mark(timeline::RegMemBegin, poll);
                        s_waitInfo.store(info, std::memory_order_relaxed);
                        s_waitPoll.store(poll, std::memory_order_relaxed);
                        s_waitRef.store(ref, std::memory_order_relaxed);
                        s_waitMask.store(mask, std::memory_order_relaxed);
                        s_waitSince.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            waitStart.time_since_epoch()).count(), std::memory_order_release);
                        static const bool logWaits = std::getenv("NFSMW_LOG_REGMEM") != nullptr;
                        static int logged = 0;
                        if (logWaits && logged < 40)
                        {
                            logged++;
                            auto pending = PendingValue(poll);
                            fprintf(stderr, "[regmem] %s %08X func %u value %08X mask %08X ref %08X pending %s%08X | last packets:\n",
                                memory ? "mem" : "reg", poll, info & 7, value, mask, ref, pending ? "" : "none ", pending.value_or(0));
                            DumpHistory();
                        }
                        renderer::Submit();
                        waitTimer.emplace(renderer::CP_WAIT_REGMEM);
                        continue;
                    }
                    if (guesttime::Suspended() || guesttime::Epoch() != waitEpoch)
                    {
                        // What it waits for comes from game threads or the
                        // vblank, which stand still while the game is
                        // suspended: wait for the resume instead of polling,
                        // then count the wait (its stuck report, the hang
                        // watchdog's) from there.
                        waitTimer.reset();
                        {
                            renderer::CpWaitTimer held(renderer::CP_WAIT_SUSPENDED);
                            guesttime::WaitWhileSuspended();
                        }
                        waitEpoch = guesttime::Epoch();
                        waitTimer.emplace(renderer::CP_WAIT_REGMEM);
                        waitStart = std::chrono::steady_clock::now();
                        reported = false;
                        s_waitSince.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            waitStart.time_since_epoch()).count(), std::memory_order_release);
                        continue;
                    }
                    if (!reported && std::chrono::steady_clock::now() - waitStart > std::chrono::seconds(1))
                    {
                        reported = true;
                        fprintf(stderr, "[gpu] WAIT_REG_MEM stuck >1s: %s %08X, func %u, value %08X & mask %08X vs ref %08X\n",
                            memory ? "memory" : "register", poll, info & 7, value, mask, ref);
                        DumpHistory();
                    }
                    // The guest's wait interval is a hint for hardware polling;
                    // sleeping whole milliseconds per poll added a millisecond
                    // or more to every wait. Spin with yields for 2 ms, then
                    // back off to short sleeps.
                    (void)wait;
                    if (std::chrono::steady_clock::now() - waitStart < std::chrono::milliseconds(2))
                        std::this_thread::yield();
                    else
                        std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
                return true;
            }

            case PM4_REG_RMW:
            {
                uint32_t info = r.Read(), andMask = r.Read(), orMask = r.Read();
                r.Skip(count - 3);
                uint32_t value = s_regs[info & 0x1FFF];
                value &= (info >> 31) & 1 ? s_regs[andMask & 0x1FFF] : andMask;
                value |= (info >> 30) & 1 ? s_regs[orMask & 0x1FFF] : orMask;
                WriteRegister(info & 0x1FFF, value);
                return true;
            }

            case PM4_REG_TO_MEM:
            {
                uint32_t reg = r.Read(), address = r.Read();
                r.Skip(count - 2);
                uint32_t value = s_regs[reg % REGISTER_COUNT];
                QueueStore(address, value);
                return true;
            }

            case PM4_MEM_WRITE:
            {
                uint32_t address = r.Read();
                std::vector<uint32_t> values(count - 1);
                for (uint32_t& v : values)
                    v = r.Read();
                QueueStores(address, std::move(values));
                return true;
            }

            case PM4_COND_WRITE:
            {
                uint32_t info = r.Read(), poll = r.Read(), ref = r.Read(), mask = r.Read();
                uint32_t writeAddr = r.Read(), data = r.Read();
                r.Skip(count - 6);
                uint32_t value = (info & 0x10) ? LoadSwapped(poll) : s_regs[poll % REGISTER_COUNT];
                if (compare(info, value & mask, ref))
                {
                    if (info & 0x100)
                        QueueStore(writeAddr, data);
                    else
                        WriteRegister(writeAddr, data);
                }
                return true;
            }

            case PM4_EVENT_WRITE:
                WriteRegister(REG_VGT_EVENT_INITIATOR, r.Read() & 0x3F);
                r.Skip(count - 1);
                return true;

            case PM4_EVENT_WRITE_SHD:
            {
                uint32_t initiator = r.Read(), address = r.Read(), value = r.Read();
                r.Skip(count - 3);
                static const bool logFences = std::getenv("NFSMW_LOG_D3D_WAIT") != nullptr;
                if (logFences)
                {
                    static std::unordered_set<uint32_t> seen;
                    if (seen.insert(address).second)
                        fprintf(stderr, "[d3dwait] EVENT_WRITE_SHD fence address %08X (read pointer write-back %08X)\n",
                            address, uint32_t(s_readWriteback));
                }
                WriteRegister(REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
                QueueStore(address, (initiator >> 31) & 1 ? s_counter.load() : value);
                return true;
            }

            case PM4_EVENT_WRITE_EXT:
            {
                uint32_t initiator = r.Read(), address = r.Read();
                r.Skip(count - 2);
                WriteRegister(REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
                renderer::QueueWrite([=] {
                    // Screen extents (8in16): the whole 8192x8192 range, z 0..1.
                    const uint16_t extents[] = { 0, 8192 >> 3, 0, 8192 >> 3, 0, 1 };
                    be<uint16_t> out[6];
                    for (int i = 0; i < 6; i++)
                        out[i] = extents[i];
                    GuestStore(address & ~3u, out, sizeof(out));
                });
                return true;
            }

            case PM4_EVENT_WRITE_ZPD:
            {
                // Occlusion queries: report "finished" with zero samples,
                // unless they are counted.
                WriteRegister(REG_VGT_EVENT_INITIATOR, r.Read() & 0x3F);
                r.Skip(count - 1);
                uint32_t addr = s_regs[REG_RB_SAMPLE_COUNT_ADDR];
                // D3D's reports are 64-byte blocks: BEGIN at +0x20, END at +0.
                // NFSMW_OCCLUSION=1 counts them (an END's report is then the
                // renderer's, stored at GPU completion); the fake overrides it.
                if (addr != 0 && s_occlusionFake && zpd::Classify(addr) == zpd::Kind::End)
                    renderer::QueueWrite([=] { StoreZpdEnd(addr, s_occlusionFake); });
                else if (addr != 0 && (s_occlusionFake || !renderer::ZpassDone(addr)))
                    renderer::QueueWrite([=] {
                        const uint8_t zeros[32] = {};
                        GuestStore(addr, zeros, sizeof(zeros));
                    });
                return true;
            }

            case PM4_DRAW_INDX:
            case PM4_DRAW_INDX_2:
            {
                s_draws++;
                // DRAW_INDX: viz query id, VGT_DRAW_INITIATOR, then VGT_DMA_BASE
                // and VGT_DMA_SIZE for DMA indices. DRAW_INDX_2 has no viz dword.
                uint32_t used = 0;
                if (opcode == PM4_DRAW_INDX)
                {
                    r.Read();
                    used++;
                }
                uint32_t initiator = r.Read();
                used++;
                WriteRegister(0x21FC, initiator);  // VGT_DRAW_INITIATOR
                uint32_t dmaBase = 0, dmaSize = 0;
                if (((initiator >> 6) & 3) == 0 && count >= used + 2)  // SourceSelect::kDMA
                {
                    dmaBase = r.Read();
                    dmaSize = r.Read();
                    used += 2;
                    WriteRegister(0x21FA, dmaBase);  // VGT_DMA_BASE
                    WriteRegister(0x21FB, dmaSize);  // VGT_DMA_SIZE
                }
                r.Skip(count - used);
                renderer::Draw(initiator, dmaBase, dmaSize);
                return true;
            }

            case PM4_VIZ_QUERY:
            {
                uint32_t d = r.Read();
                r.Skip(count - 1);
                uint32_t id = d & 0x3F;
                if (d & 0x100)
                    s_regs[id < 32 ? REG_PA_SC_VIZ_QUERY_STATUS_0 : REG_PA_SC_VIZ_QUERY_STATUS_1] |= 1u << (id & 31);
                return true;
            }

            case PM4_SET_CONSTANT:
            case PM4_LOAD_ALU_CONSTANT:
            {
                uint32_t address = opcode == PM4_LOAD_ALU_CONSTANT ? (r.Read() & 0x3FFFFFFF) : 0;
                uint32_t offsetType = r.Read();
                uint32_t size = opcode == PM4_LOAD_ALU_CONSTANT ? (r.Read() & 0xFFF) : count - 1;
                uint32_t index = offsetType & 0x7FF;
                static constexpr uint32_t bases[] = { 0x4000, 0x4800, 0x4900, 0x4908, 0x2000 };
                uint32_t type = (offsetType >> 16) & 0xFF;
                if (type > 4)
                {
                    r.Skip(opcode == PM4_LOAD_ALU_CONSTANT ? count - 3 : count - 1);
                    return true;
                }
                index += bases[type];
                if (BulkSafe(index, size))
                {
                    if (opcode == PM4_LOAD_ALU_CONSTANT)
                    {
                        WriteRegistersBulk(index, Physical(address), size);
                        r.Skip(count - 3);
                        return true;
                    }
                    if (const uint8_t* run = r.Take(size))
                    {
                        WriteRegistersBulk(index, run, size);
                        return true;
                    }
                }
                for (uint32_t n = 0; n < size; n++, index++)
                    WriteRegister(index, opcode == PM4_LOAD_ALU_CONSTANT
                        ? uint32_t(*reinterpret_cast<be<uint32_t>*>(Physical(address + n * 4)))
                        : r.Read());
                if (opcode == PM4_LOAD_ALU_CONSTANT)
                    r.Skip(count - 3);
                return true;
            }

            case PM4_SET_CONSTANT2:
            case PM4_SET_SHADER_CONSTANTS:
            {
                uint32_t index = r.Read() & 0xFFFF;
                if (BulkSafe(index, count - 1))
                    if (const uint8_t* run = r.Take(count - 1))
                    {
                        WriteRegistersBulk(index, run, count - 1);
                        return true;
                    }
                for (uint32_t n = 0; n < count - 1; n++, index++)
                    WriteRegister(index, r.Read());
                return true;
            }

            case PM4_IM_LOAD:
            {
                uint32_t addrType = r.Read(), startSize = r.Read();
                r.Skip(count - 2);
                const auto* words = reinterpret_cast<const uint32_t*>(Physical(addrType & ~3u));
                DumpShader(addrType & 3, words, startSize & 0xFFFF);
                renderer::LoadShader(addrType & 3, words, startSize & 0xFFFF);
                return true;
            }
            case PM4_IM_LOAD_IMMEDIATE:
            {
                uint32_t type = r.Read(), startSize = r.Read();
                uint32_t size = startSize & 0xFFFF;
                std::vector<uint32_t> words(size);
                for (uint32_t i = 0; i < size; i++)
                {
                    uint32_t v = r.Read();
                    words[i] = ByteSwap(v);  // keep guest (big-endian) order, like IM_LOAD memory
                }
                r.Skip(count - 2 - size);
                DumpShader(type & 3, words.data(), size);
                renderer::LoadShader(type & 3, words.data(), size);
                return true;
            }

            case PM4_SET_BIN_MASK_LO: s_binMask = (s_binMask & ~0xFFFFFFFFull) | r.Read(); r.Skip(count - 1); return true;
            case PM4_SET_BIN_MASK_HI: s_binMask = (s_binMask & 0xFFFFFFFFull) | (uint64_t(r.Read()) << 32); r.Skip(count - 1); return true;
            case PM4_SET_BIN_SELECT_LO: s_binSelect = (s_binSelect & ~0xFFFFFFFFull) | r.Read(); r.Skip(count - 1); return true;
            case PM4_SET_BIN_SELECT_HI: s_binSelect = (s_binSelect & 0xFFFFFFFFull) | (uint64_t(r.Read()) << 32); r.Skip(count - 1); return true;
            case PM4_SET_BIN_MASK: { uint64_t hi = r.Read(); s_binMask = (hi << 32) | r.Read(); r.Skip(count - 2); return true; }
            case PM4_SET_BIN_SELECT: { uint64_t hi = r.Read(); s_binSelect = (hi << 32) | r.Read(); r.Skip(count - 2); return true; }
            }

            static uint32_t s_reported[128];
            if (!s_reported[opcode]++)
                fprintf(stderr, "[gpu] PM4 opcode %02X not handled (skipped, %u dwords)\n", opcode, count);
            r.Skip(count);
            return true;
        }

        void ExecuteBuffer(Reader& r, PPCContext& ctx)
        {
            while (r.offset != r.end)
            {
                uint32_t packet = r.Read();
                if (packet == 0)
                    continue;
                if ((packet >> 30) != 2)
                    Record(packet, r);
                switch (packet >> 30)
                {
                case 0:  // write `count` registers from `base` (or one register repeatedly)
                {
                    uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
                    uint32_t base = packet & 0x7FFF;
                    bool one = (packet >> 15) & 1;
                    if (!one && BulkSafe(base, count))
                        if (const uint8_t* run = r.Take(count))
                        {
                            WriteRegistersBulk(base, run, count);
                            break;
                        }
                    for (uint32_t m = 0; m < count; m++)
                        WriteRegister(one ? base : base + m, r.Read());
                    break;
                }
                case 1:  // two registers
                {
                    uint32_t a = r.Read(), b = r.Read();
                    WriteRegister(packet & 0x7FF, a);
                    WriteRegister((packet >> 11) & 0x7FF, b);
                    break;
                }
                case 2:  // filler
                    break;
                case 3:
                    ExecuteType3(r, packet, ctx);
                    break;
                }
            }
        }

        void Worker()
        {
#ifdef __APPLE__
            // The idle poll sleeps 100 us at a time; at the default QoS macOS
            // coalesces those timers into ~10 ms wake-ups, leaving the GPU
            // starved with recorded work pending.
            pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
            writewatch::t_gpuThread = true;
            renderer::t_commandProcessor = true;
            SetHostThreadName("nfsmw-cp");
            // Dense scenes are limited by this thread. On the Steam Machine the
            // scheduler left it on a 3.5 GHz core, or on the sibling of a
            // spinning game thread, while those held the 4.8 GHz cores.
            hostcpu::UseReservedCore("command processor");
            GuestThreadContext thread(2);
            PPCContext& ctx = thread.ppcContext;
            renderer::Initialize(s_regs);
            auto lastWork = std::chrono::steady_clock::now();
            while (true)
            {
                uint32_t primary = s_primaryBuffer;
                uint32_t writeIndex = *MmioRegister(REG_CP_RB_WPTR);
                if (primary == 0 || writeIndex == s_readIndex || writeIndex * 4 >= s_primarySize)
                {
                    // Kicks come in bursts and the game often waits on their
                    // results: poll with yields for 2 ms after the last work,
                    // then back off to short sleeps.
                    // No new commands: whatever is recorded goes out now (the
                    // game may be waiting on a fence in it).
                    if (writewatch::g_submitRequested.exchange(false))
                        renderer::Submit();  // a guest thread waits on recorded work
                    renderer::SubmitIfDue();
                    // The game is suspended: its threads stand still, so
                    // wait for the resume rather than poll all the while.
                    if (guesttime::Suspended())
                    {
                        guesttime::WaitWhileSuspended();
                        lastWork = std::chrono::steady_clock::now();
                        continue;
                    }
                    if (std::chrono::steady_clock::now() - lastWork < std::chrono::milliseconds(2))
                        std::this_thread::yield();
                    else
                        std::this_thread::sleep_for(std::chrono::microseconds(100));
                    continue;
                }
                s_regs[REG_CP_RB_WPTR] = writeIndex;
                Reader r{ Physical(primary), s_primarySize, s_readIndex * 4, writeIndex * 4 };
                timeline::Mark(timeline::BatchBegin, (writeIndex - s_readIndex) & (s_primarySize / 4 - 1));
                s_busyFrom = std::chrono::steady_clock::now();
                s_busyWaitFrom = renderer::g_cpWaitNs;
                ExecuteBuffer(r, ctx);
                AccountBusy();
                timeline::Mark(timeline::BatchEnd);
                if (writewatch::g_submitRequested.exchange(false))
                    renderer::Submit();
                renderer::SubmitIfIdle();
                s_readIndex = writeIndex;
                // The read pointer is fetch progress (as on the hardware), not
                // completion: everything the batch needs has been consumed or
                // copied, so its ring space is free now. D3D tracks resource
                // lifetimes with fences, which stay deferred to GPU completion.
                if (uint32_t wb = s_readWriteback)
                    *reinterpret_cast<be<uint32_t>*>(Physical(wb)) = writeIndex;
                lastWork = std::chrono::steady_clock::now();
            }
        }

        // One line for the vblank lock's state (on changes, and each minute).
        void LogVblank(const video::VblankLock::Status& s)
        {
            const video::RefreshEstimate& d = s.display;
            if (s.locked)
                fprintf(stderr, "[vblank] locked to the display: %.4f Hz (fit %u presents over %.1f s, rms %.2f ms, %u set aside); "
                    "vblank %.2f ms before its refresh, error %+.3f ms; vblank to screen %.1f ms (median), to present %.1f ms (99%%); %llu slips\n",
                    1e9 / d.periodNs, d.samples, d.spanNs / 1e9, d.rmsNs / 1e6, d.rejected, s.leadNs / 1e6, s.errorNs / 1e6,
                    s.latencyNs / 1e6, s.readyNs / 1e6, (unsigned long long)s.slips);
            else if (s.follows)
                fprintf(stderr, "[vblank] free-running every %.3f ms: the display follows the vblank (variable refresh?), "
                    "so there is no refresh to lock to\n", s.periodNs / 1e6);
            else if (d.samples)
                fprintf(stderr, "[vblank] free-running every %.3f ms; display %s: %.4f Hz (fit %u presents over %.1f s, rms %.2f ms, %u set aside)\n",
                    s.periodNs / 1e6, d.valid ? "measured (lock off)" : "not accepted", 1e9 / d.periodNs, d.samples, d.spanNs / 1e9,
                    d.rmsNs / 1e6, d.rejected);
            else
                fprintf(stderr, "[vblank] free-running every %.3f ms\n", s.periodNs / 1e6);
        }

        void Vsync()
        {
            SetHostThreadName("nfsmw-vsync");  // (Linux would give it its creator's name, nfsmw-main)
            GuestThreadContext thread(2);
            using namespace std::chrono;
            video::VblankLock& clock = video::GuestVblank();
            auto now = [] { return int64_t(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count()); };
            // The time suspended that `next` has been moved on for. Kept
            // across vblanks and changed only with a shift, so a suspension
            // that begins and ends while this thread is elsewhere (in the
            // guest's interrupt handler, say) still moves the next vblank.
            // Both taken while running (a boot thread can start this one in
            // a suspension): `next` is a host time at which `paused` held.
            int64_t next, paused;
            do
            {
                guesttime::WaitWhileSuspended();
                paused = guesttime::PausedNs();
                next = now();
            } while (guesttime::Suspended() || guesttime::PausedNs() != paused);
            bool locked = false;
            uint64_t slips = 0, count = 0;
            while (true)
            {
                next = clock.Next(next);
                for (int k = video::GuestVblankDivider(); k > 1; k--)  // Frame Rate 30: every other refresh
                    next = clock.Next(next);
                // To the vblank in guest time: a suspension before it moves
                // it on by exactly the time suspended, so the guest sees one
                // ordinary period across it, and no vblank fires, queues or
                // is replayed meanwhile.
                while (true)
                {
                    std::this_thread::sleep_until(steady_clock::time_point(nanoseconds(next)));
                    guesttime::WaitWhileSuspended();
                    int64_t nowPaused = guesttime::PausedNs();
                    if (nowPaused == paused)
                        break;
                    next += nowPaused - paused;
                    paused = nowPaused;
                    clock.Resumed();
                }
                video::NoteGuestVblank(now());
                s_counter++;
                s_vblanks++;
                DispatchInterrupt(thread.ppcContext, 0, 2);
                auto status = clock.GetStatus();
                if (status.locked != locked || status.slips != slips || ++count % 3600 == 0)
                {
                    LogVblank(status);
                    locked = status.locked;
                    slips = status.slips;
                }
            }
        }

        void StartOnce()
        {
            if (s_started.exchange(true))
                return;
            // Registers Xenia returns fixed values for (GraphicsSystem::ReadRegister).
            WriteRegister(0x0F00, 0x08100748);  // RB_EDRAM_TIMING
            WriteRegister(0x0F01, 0x0000200E);  // RB_BC_CONTROL
            WriteRegister(0x194C, 0x000002D0);  // D1MODE_V_COUNTER
            WriteRegister(0x1951, 0x00000001);  // interrupt status: vblank
            WriteRegister(0x1961, 0x050002D0);  // D1MODE_VIEWPORT_SIZE
            std::thread(Worker).detach();
            std::thread(Vsync).detach();
        }
    }

    void InitializeRingBuffer(uint32_t guestAddress, uint32_t sizeLog2)
    {
        s_primarySize = 1u << (sizeLog2 + 3);
        s_readIndex = 0;
        s_primaryBuffer = guestAddress & 0x1FFFFFFF;
        fprintf(stderr, "[gpu] ring buffer %08X, %u bytes\n", guestAddress, s_primarySize);
        StartOnce();
    }

    void EnableReadPointerWriteBack(uint32_t guestAddress, uint32_t blockSizeLog2)
    {
        s_readWriteback = guestAddress & 0x1FFFFFFF;
    }

    void SetInterruptCallback(uint32_t callback, uint32_t userData)
    {
        s_interruptData = userData;
        s_interruptCallback = callback;
        fprintf(stderr, "[gpu] interrupt callback %08X (data %08X)\n", callback, userData);
    }

    uint64_t FrameCount()
    {
        return s_frames;
    }

    int64_t WaitingSince()
    {
        return s_waitSince.load(std::memory_order_acquire);
    }

    std::string DescribeState()
    {
        std::string text;
        if (int64_t since = s_waitSince.load(std::memory_order_acquire))
        {
            uint32_t info = s_waitInfo.load(std::memory_order_relaxed), poll = s_waitPoll.load(std::memory_order_relaxed);
            bool memory = (info & 0x10) != 0;
            uint32_t value = memory ? LoadSwapped(poll) : s_regs[poll % REGISTER_COUNT];
            static const char* const kFunc[8] = { "never", "<", "<=", "==", "!=", ">=", ">", "always" };
            double seconds = double(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count() - since) / 1e9;
            text += std::format("In a WAIT_REG_MEM for {:.1f} s: {} {:08X}: value {:08X} & mask {:08X} {} ref {:08X}{}\n",
                seconds, memory ? "memory" : "register", poll, value, s_waitMask.load(), kFunc[info & 7], s_waitRef.load(),
                report::g_testHang.load() ? " (NFSMW_TEST_HANG)" : "");
        }
        else
            text += "Not in a WAIT_REG_MEM.\n";
        text += std::format("Frames {}, draws {}, guest vblanks {}; ring buffer {:08X} ({} bytes), read index {} of write index {}\n",
            s_frames.load(), s_draws.load(), s_vblanks.load(), s_primaryBuffer.load(), s_primarySize, s_readIndex,
            uint32_t(*MmioRegister(REG_CP_RB_WPTR)));
        text += FormatHistory();
        return text;
    }
}
