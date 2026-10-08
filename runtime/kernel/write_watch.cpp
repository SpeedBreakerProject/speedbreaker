// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See write_watch.h.
#include <stdafx.h>
#include "write_watch.h"
#include "memory.h"

#include <cpu/guest_time.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>
#include <dlfcn.h>

namespace writewatch
{
    std::atomic<bool> g_submitRequested{ false };
    thread_local bool t_gpuThread = false;
    std::atomic<uint32_t> g_snapsOpen{ 0 };

    namespace
    {
        constexpr uint32_t kPhysicalSize = 0x20000000;
        constexpr uint32_t kWindows[3] = { 0xA0000000, 0xC0000000, 0xE0000000 };
        // GPU hazards are tracked per guest page (4 KB): host pages are 16 KB
        // on Apple silicon, and neighbouring guest resources (the two 64x64
        // luminance targets NFSMW reads back, a vertex buffer next to the
        // command ring) would otherwise wait on each other's GPU work.
        constexpr uint32_t kSubShift = 12;
        constexpr uint32_t kSubCount = kPhysicalSize >> kSubShift;

        size_t s_pageSize = 0;
        uint32_t s_pageShift = 0;
        uint32_t s_pageCount = 0;
        uint32_t s_subsPerPage = 1;
        std::atomic<uint64_t> s_sequence{ 1 };
        // Texture watches (per host page), and the write sequence of each
        // guest page (4 KB). A CPU write is only seen per host page (the
        // whole page is unprotected at once), so it marks every guest page
        // of it (unless NFSMW_WATCH_SUBPAGES, see s_watchSubpages); a resolve
        // marks just the guest pages it wrote. On Apple's
        // 16 KB pages a sequence per host page made each resolve dirty its
        // neighbours (the two shadow maps, the bloom chain share pages):
        // ~17 textures a frame untiled again, ~40 MB, where a 4 KB-page
        // Steam Machine reloads 3 in the same race.
        std::unique_ptr<std::atomic<uint8_t>[]> s_watched;
        std::unique_ptr<std::atomic<uint64_t>[]> s_written;
        // GPU guard: the last submissions reading and writing each guest
        // page. A pending GPU write makes the host page PROT_NONE (CPU reads
        // would be stale); a pending read, PROT_READ. s_prot is what is set.
        std::unique_ptr<std::atomic<uint64_t>[]> s_gpuRead, s_gpuWrite;
        std::unique_ptr<std::atomic<uint8_t>[]> s_prot;  // 0 RW, 1 read-only, 2 none
        std::atomic<uint64_t> s_completed{ 0 };
        // Host pages opened for an access to a guest page without hazards
        // while another of its guest pages still has one: protected again
        // at the next Rearm (submission boundary).
        std::vector<uint32_t> s_rearm;
        // NFSMW_LOG_GUARD=1: what each guest page was guarded for, logged on stalls.
        const bool s_logGuard = std::getenv("NFSMW_LOG_GUARD") != nullptr;
        std::unique_ptr<std::atomic<const char*>[]> s_gpuTag;
        std::unique_ptr<std::atomic<uint32_t>[]> s_gpuRange[2];
        std::atomic<uint64_t> s_stalls{ 0 }, s_stallUs{ 0 }, s_faults{ 0 }, s_opened{ 0 };
        // Shadow mode (a renderer that can't import guest memory, e.g. on a
        // discrete GPU): the GPU reads and writes its own copy of physical
        // memory. Per host page: the write sequence the copy was made at
        // (0 = never), and whether the GPU wrote it last (resolves), which
        // makes the guest's copy stale until a CPU access reads it back.
        uint8_t* s_shadow = nullptr;
        std::unique_ptr<std::atomic<uint64_t>[]> s_shadowSeq;
        std::unique_ptr<std::atomic<uint8_t>[]> s_gpuOwned;
        // Highest write sequence per 256 KB block: clean ranges are skipped
        // without looking at their pages.
        constexpr uint32_t kBlockShift = 18;
        std::unique_ptr<std::atomic<uint64_t>[]> s_blockWritten;
        // Per 256 KB block, for the command processor's per-draw checks
        // (GpuWritePending, AnyGpuOwned): the highest submission that writes
        // any of its guest pages (never lowered), and how many of its pages
        // are GPU-owned. A block without either is clean at a glance; the
        // page arrays are 1 MB each, and a look there is mostly a cache miss.
        std::unique_ptr<std::atomic<uint64_t>[]> s_blockGpuWrite;
        std::unique_ptr<std::atomic<uint32_t>[]> s_blockOwned;
        // NFSMW_WATCH_RANGES=0: the bookkeeping as before (see Put, Change,
        // MarkWritten, WrittenSince), for A/B.
        const bool s_ranges = [] { const char* v = std::getenv("NFSMW_WATCH_RANGES"); return !v || v[0] != '0'; }();

        // Per-page state that only changes under s_protectMutex, or only on
        // the command processor, is stored plainly. A sequentially consistent
        // store or exchange is a locked instruction on x86 (about 20 cycles):
        // a resolve made eight of them per page (write sequence, block, epoch,
        // guard, protection twice, ownership).
        template <class T>
        void Put(std::atomic<T>& a, T value)
        {
            if (s_ranges)
                a.store(value, std::memory_order_relaxed);
            else
                a.store(value);
        }
        // Put, returning whether the value changed.
        template <class T>
        bool Change(std::atomic<T>& a, T value)
        {
            if (!s_ranges)
                return a.exchange(value) != value;
            if (a.load(std::memory_order_relaxed) == value)
                return false;
            a.store(value, std::memory_order_relaxed);
            return true;
        }
        // NFSMW_LOG_UPLOADS: per guest page, what last bumped its write
        // sequence (kind << 60 | frame << 32 | tag), only allocated then.
        const bool s_logWriters = std::getenv("NFSMW_LOG_UPLOADS") != nullptr;
        std::unique_ptr<std::atomic<uint64_t>[]> s_lastWriter;
        std::unique_ptr<std::atomic<uint32_t>[]> s_lastWriterPc;  // a fault's t_faultPc - s_imageBase
        std::atomic<uint64_t> s_writerFrame{ 0 };
        const uint8_t* s_imageBase = nullptr;
        void NoteWriter(uint32_t firstSub, uint32_t count, Writer kind, uint32_t tag)
        {
            uint64_t v = uint64_t(kind) << 60 | (s_writerFrame.load(std::memory_order_relaxed) & 0x0FFFFFFF) << 32 | tag;
            auto pc = static_cast<const uint8_t*>(t_faultPc);
            uint32_t offset = kind == Writer::kCpuFault && pc > s_imageBase && pc - s_imageBase < 0xFFFFFFFF ? uint32_t(pc - s_imageBase) : 0;
            for (uint32_t i = 0; i < count; i++)
            {
                s_lastWriter[firstSub + i].store(v, std::memory_order_relaxed);
                s_lastWriterPc[firstSub + i].store(offset, std::memory_order_relaxed);
            }
        }

        // NFSMW_WATCH_SUBPAGES=1, on hosts whose pages hold several guest
        // pages (16 KB on Apple; off elsewhere): a CPU write fault marks just
        // the guest page it hit. The game rewrites a per-bind record every
        // frame in the 4 KB page next to many textures (and in the one before
        // HUD ones): marking the whole host page reloaded ~10 of them a frame
        // on Apple, each ending a render pass, where 4 KB-page hosts reload
        // none. The fault copies the host page first (still read-only, so
        // the copy is what the watch last vouched for) and opens it; its
        // other guest pages are pending until anything relies on the page
        // again: protecting it (the hook in SetProt) or reading sequences of
        // it (Settle). Either makes it read-only, then marks the pending
        // pages whose bytes changed (a store after that faults); if all of
        // them changed, the watch ends there, as at a whole-page fault. A
        // snapshot open for kSnapAge submissions (~8 frames: no texture there
        // is in use), on a page a host write opens, or the oldest when a
        // fault finds every slot taken, marks its pending pages unseen and
        // leaves the page open, as a fault always did. A fault waits for a
        // texture's untile still reading another guest page of it (see
        // OpenSnapshot); a guest page another GPU read is pending on is
        // marked at the fault, not compared (as is the untile's, if that
        // wait gives up: see WaitForGpu). Only watched pages get one: a
        // page protected just for the GPU's use (vertex data) has no texture
        // to keep (every path that ends a watch marks all its guest pages).
        // Unsure cases mark the whole host page as before. In use with host
        // pages of 2 to 32 guest pages: by default on Apple (iPad race:
        // 13.9 -> 0 texture reloads a frame with the renderer's fusion
        // toggles), only when set elsewhere; =0 turns it off (null arrays).
#ifdef __APPLE__
        constexpr bool kSubpagesByDefault = true;
#else
        constexpr bool kSubpagesByDefault = false;
#endif
        const bool s_watchSubpages = [] { const char* v = std::getenv("NFSMW_WATCH_SUBPAGES"); return v ? v[0] == '1' : kSubpagesByDefault; }();
        constexpr uint32_t kSnapSlots = 128, kSnapAge = 32;
        static_assert(kSnapSlots < 256, "s_snapOf holds a slot + 1 in a byte");
        // Under s_protectMutex: the slots (pending: the guest pages not
        // marked yet, a bit each; opened: s_rearms then; tag: the fault;
        // serial: s_snapSerial then, the oldest goes first when all are taken).
        struct Snap { uint32_t page = UINT32_MAX, pending = 0, tag = 0; uint64_t opened = 0, serial = 0; };
        Snap s_snap[kSnapSlots];
        uint32_t s_snapFree[kSnapSlots], s_snapFreeCount = 0;
        uint64_t s_rearms = 0, s_snapSerial = 0;
        uint32_t s_allSubs = 0;  // a pending mask with every guest page of a host page
        std::unique_ptr<uint8_t[]> s_snapData;  // the copies, a host page per slot
        // Per host page, its slot + 1 (0: none; the array is null when off);
        // how many are open per 256 KB block (SettleRange's quick check; in
        // all: g_snapsOpen, the sequence readers' inline one).
        std::unique_ptr<std::atomic<uint8_t>[]> s_snapOf;
        std::unique_ptr<std::atomic<uint16_t>[]> s_blockSnaps;
        // Per guest page, the last submission whose texture untile reads it
        // (GpuRead's `untile`; looked at while its s_gpuRead is pending).
        std::unique_ptr<std::atomic<uint64_t>[]> s_gpuUntile;
        std::atomic<uint64_t> s_snapOpened{ 0 }, s_snapClean{ 0 }, s_snapChanged{ 0 }, s_snapEvicted{ 0 }, s_snapFull{ 0 }, s_snapWhole{ 0 },
            s_snapReading{ 0 }, s_snapWaits{ 0 }, s_snapGaveUp{ 0 };

        std::atomic<uint64_t> s_uploadBytes{ 0 }, s_readbackBytes{ 0 }, s_protects{ 0 }, s_protectNs{ 0 }, s_hostWriteBytes{ 0 },
            s_firstUploadBytes{ 0 }, s_uploadNs{ 0 };
        std::mutex s_protectMutex;  // protection changes; the fault path takes it after any wait

        // Host address of physical page `page` in window `w`, or nullptr when
        // the page isn't visible there (the shifted 0xE window on 4 KB hosts).
        uint8_t* PageHost(uint32_t w, uint32_t page)
        {
            uint64_t physical = uint64_t(page) << s_pageShift;
            uint32_t shift = w == 2 ? g_memory.eWindowShift : 0;
            if (physical < shift)
                return nullptr;
            return g_memory.base + kWindows[w] + (physical - shift);
        }

        // Pages [first, first + count) in every window that shows them (a
        // window shows a suffix of physical memory: the 0xE one is shifted).
        void ProtectPages(uint32_t first, uint32_t count, int prot)
        {
            auto start = std::chrono::steady_clock::now();
            for (uint32_t w = 0; w < 3; w++)
            {
                uint32_t page = first;
                while (page < first + count && !PageHost(w, page))
                    page++;
                if (page == first + count)
                    continue;
                mprotect(PageHost(w, page), size_t(first + count - page) << s_pageShift, prot);
                s_protects.fetch_add(1, std::memory_order_relaxed);
            }
            s_protectNs.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()),
                std::memory_order_relaxed);
        }

        // Protection changes to consecutive pages, made with one mprotect per
        // window for each run that gets the same protection. Page by page,
        // the career intro's frame where 9.4 MB of new textures come in made
        // 7,275 calls (a write-protect flushes the TLB of every core running
        // the game): 11 ms of a 36 ms frame. In runs: 747 calls, 2.4 ms, and
        // the frame takes 25 ms (Steam Machine). NFSMW_PROTECT_RUNS=0: one
        // call per page. Flushed when it goes out of scope: declare it after
        // the s_protectMutex lock.
        const bool s_protectRuns = [] { const char* v = std::getenv("NFSMW_PROTECT_RUNS"); return !v || v[0] != '0'; }();
        void CloseSnapshot(uint32_t page, bool compare);
        struct ProtectRun
        {
            uint32_t first = 0, count = 0;
            int prot = 0;
            // NFSMW_WATCH_SUBPAGES: pages added read-only with a snapshot
            // open, compared once the run has protected them (SetProt).
            uint32_t compares[32], compareCount = 0;
            void Add(uint32_t page, int p)
            {
                if (count && (page != first + count || p != prot || !s_protectRuns))
                    Flush();
                if (count == 0)
                {
                    first = page;
                    prot = p;
                }
                count++;
            }
            // After Add(page).
            void Compare(uint32_t page)
            {
                if (compareCount == std::size(compares))
                    Flush();  // protects `page` too: it is compared at the next flush
                compares[compareCount++] = page;
            }
            void Flush()
            {
                if (count)
                    ProtectPages(first, count, prot);
                count = 0;
                for (uint32_t i = 0; i < compareCount; i++)
                    CloseSnapshot(compares[i], true);
                compareCount = 0;
            }
            ~ProtectRun() { Flush(); }
        };

        // Physical address of a host address inside one of the windows, or -1.
        int64_t PhysicalOf(const void* host)
        {
            auto* p = static_cast<const uint8_t*>(host);
            for (uint32_t w = 0; w < 3; w++)
            {
                const uint8_t* start = g_memory.base + kWindows[w];
                if (p >= start && p < start + kPhysicalSize)
                {
                    uint64_t physical = uint64_t(p - start) + (w == 2 ? g_memory.eWindowShift : 0);
                    return physical < kPhysicalSize ? int64_t(physical) : -1;
                }
            }
            return -1;
        }

        int64_t PageOf(const void* host)
        {
            int64_t physical = PhysicalOf(host);
            return physical < 0 ? -1 : physical >> s_pageShift;
        }

        // A CPU write to `page` (textures in it reload; its shadow copy is stale).
        // Bumped after each BumpWritten: unchanged means no page was written
        // (with NFSMW_WATCH_SUBPAGES, none a reader compared: see WriteEpoch).
        std::atomic<uint64_t> s_writeEpoch{ 1 };

        // A CPU write to host page `page`: every guest page of it.
        void BumpWritten(uint32_t page, Writer kind, uint32_t tag)
        {
            uint64_t seq = s_sequence.fetch_add(1) + 1;
            // The block first: a block's sequence is never below one of its
            // pages', even while a write is being recorded (WrittenSince).
            auto& block = s_blockWritten[(uint64_t(page) << s_pageShift) >> kBlockShift];
            uint64_t old = block.load();
            while (old < seq && !block.compare_exchange_weak(old, seq)) {}
            for (uint32_t i = 0, sub = page * s_subsPerPage; i < s_subsPerPage; i++, sub++)
                s_written[sub].store(seq);
            if (s_logWriters)
                NoteWriter(page * s_subsPerPage, s_subsPerPage, kind, tag);
            s_writeEpoch.fetch_add(1, std::memory_order_release);
        }

        // Guest pages [firstSub, lastSub] at once, with one sequence (a
        // resolve: up to ~3,600 guest pages, ~25 a frame). Readers compare
        // sequences with a Current() taken earlier, which every page's new
        // one exceeds. Only the command processor reads them, so plain
        // stores do; the epoch, bumped last, publishes them to any other
        // reader.
        uint64_t BumpWrittenRange(uint32_t firstSub, uint32_t lastSub, Writer kind, uint32_t tag)
        {
            uint64_t seq = s_sequence.fetch_add(1) + 1;
            for (uint64_t b = (uint64_t(firstSub) << kSubShift) >> kBlockShift; b <= (uint64_t(lastSub) << kSubShift) >> kBlockShift; b++)
            {
                auto& block = s_blockWritten[b];  // first, as in BumpWritten
                uint64_t old = block.load();
                while (old < seq && !block.compare_exchange_weak(old, seq)) {}
            }
            std::atomic<uint64_t>* written = s_written.get();  // not reloaded after every store
            for (uint32_t sub = firstSub; sub <= lastSub; sub++)
                written[sub].store(seq, std::memory_order_relaxed);
            if (s_logWriters)
                NoteWriter(firstSub, lastSub - firstSub + 1, kind, tag);
            s_writeEpoch.fetch_add(1, std::memory_order_release);
            return seq;
        }

        // NFSMW_WATCH_SUBPAGES: guest pages `subs` (a bit each) of host page
        // `page`, with one sequence and one epoch bump, as BumpWrittenRange.
        // A host page (at most 128 KB, aligned) lies in one 256 KB block.
        // kCompared notes each guest page's own address as its tag.
        void BumpSubs(uint32_t page, uint32_t subs, Writer kind, uint32_t tag)
        {
            uint64_t seq = s_sequence.fetch_add(1) + 1;
            auto& block = s_blockWritten[(uint64_t(page) << s_pageShift) >> kBlockShift];  // first, as in BumpWritten
            uint64_t old = block.load();
            while (old < seq && !block.compare_exchange_weak(old, seq)) {}
            for (; subs; subs &= subs - 1)
            {
                uint32_t sub = page * s_subsPerPage + uint32_t(__builtin_ctz(subs));
                s_written[sub].store(seq, std::memory_order_relaxed);
                if (s_logWriters)
                    NoteWriter(sub, 1, kind, kind == Writer::kCompared ? sub << kSubShift : tag);
            }
            s_writeEpoch.fetch_add(1, std::memory_order_release);
        }

        // The newest write to any guest page of host page `page`.
        uint64_t PageWritten(uint32_t page)
        {
            uint64_t highest = 0;
            for (uint32_t i = 0, sub = page * s_subsPerPage; i < s_subsPerPage; i++, sub++)
                highest = std::max(highest, s_written[sub].load());
            return highest;
        }

        uint8_t* GuestPage(uint32_t page)
        {
            return g_memory.base + kWindows[0] + (uint64_t(page) << s_pageShift);
        }

        // Under s_protectMutex: close host page `page`'s snapshot (see
        // s_watchSubpages). Compared: the page is read-only first (made so
        // here, or by the caller's run), so a store either landed before and
        // is compared, or faults after and waits for the lock; the pending
        // guest pages whose bytes changed are marked. The page stays watched
        // while one compared clean (its next store must fault); with all of
        // them changed, all are marked and nothing is left to vouch for, so
        // the watch ends as at a whole-page fault (a Watch closing it has set
        // it again already: never cleared here). Not compared: every pending
        // guest page is marked and the page is left as it is (open, as a
        // fault left it before). An open snapshot's page is RW (prot 0): every
        // change to its protection closes it (SetProt: at once, or once the
        // run that makes it read-only is flushed).
        void CloseSnapshot(uint32_t page, bool compare)
        {
            uint32_t slot = s_snapOf[page].load(std::memory_order_relaxed) - 1u;
            Snap& snap = s_snap[slot];
            if (compare && s_prot[page].load(std::memory_order_relaxed) == 0)
            {
                Put(s_prot[page], uint8_t(1));
                ProtectPages(page, 1, PROT_READ);
            }
            const uint8_t* copy = s_snapData.get() + (size_t(slot) << s_pageShift);
            const uint8_t* now = GuestPage(page);
            uint32_t changed = snap.pending;
            if (compare)
                for (uint32_t pending = snap.pending; pending; pending &= pending - 1)
                {
                    uint32_t i = uint32_t(__builtin_ctz(pending));
                    size_t offset = size_t(i) << kSubShift;
                    if (memcmp(now + offset, copy + offset, size_t(1) << kSubShift) == 0)
                        changed &= ~(1u << i);
                }
            if (changed)
                BumpSubs(page, changed, compare ? Writer::kCompared : Writer::kUnverified, snap.tag);
            if (compare && changed != snap.pending)
                s_watched[page].store(1);
            // Release: a reader that sees the page closed sees its marks.
            s_snapOf[page].store(0, std::memory_order_release);
            s_blockSnaps[(uint64_t(page) << s_pageShift) >> kBlockShift].fetch_sub(1, std::memory_order_relaxed);
            g_snapsOpen.fetch_sub(1, std::memory_order_relaxed);
            snap.page = UINT32_MAX;
            s_snapFree[s_snapFreeCount++] = slot;
            (!compare ? s_snapEvicted : changed ? s_snapChanged : s_snapClean).fetch_add(1, std::memory_order_relaxed);
        }

        // Under s_protectMutex. With `run`, the change is made when the run
        // is flushed.
        void SetProt(uint32_t page, uint8_t prot, ProtectRun* run = nullptr)
        {
            // Protected again (a watch, a GPU use, a rearm, a shadow sync, a
            // settle): the page's snapshot is compared once it is read-only.
            // Every path that protects a page comes through here. In a run,
            // after the run's flush (one mprotect per window for the run, not
            // one per page); one going to PROT_NONE is compared read-only
            // first (its bytes can't be read after).
            bool compare = prot != 0 && s_snapOf && s_snapOf[page].load(std::memory_order_relaxed);
            if (compare && (!run || prot == 2))
            {
                if (run)
                    run->Flush();  // the run's earlier pages first: CloseSnapshot protects this one itself
                // (Unless the flush compared it: the run made it read-only first.)
                if (s_snapOf[page].load(std::memory_order_relaxed))
                    CloseSnapshot(page, true);
                compare = false;
            }
            if (!Change(s_prot[page], prot))
                return;  // (not with `compare`: an open snapshot's page is prot 0)
            int os = prot == 2 ? PROT_NONE : prot == 1 ? PROT_READ : PROT_READ | PROT_WRITE;
            if (run)
            {
                run->Add(page, os);
                if (compare)
                    run->Compare(page);
            }
            else
                ProtectPages(page, 1, os);
        }

        // The protection the page's watch and guard state call for.
        uint8_t WantedProt(uint32_t page)
        {
            // Shadow mode: GPU reads use the copy, so only GPU-written pages
            // (stale here) and write tracking need protection.
            if (s_shadow)
                return s_gpuOwned[page].load() ? 2 : s_watched[page].load() ? 1 : 0;
            uint64_t completed = s_completed.load();
            uint8_t prot = s_watched[page].load() ? 1 : 0;
            for (uint32_t i = 0, sub = page * s_subsPerPage; i < s_subsPerPage; i++, sub++)
            {
                if (s_gpuWrite[sub].load() > completed)
                    return 2;
                if (s_gpuRead[sub].load() > completed)
                    prot = 1;
            }
            return prot;
        }

        // Shadow mode, under s_protectMutex: copy a GPU-written page back to
        // the guest (its GPU writes must be complete). The copy is then
        // current again, and further CPU writes are tracked.
        void ReadBackLocked(uint32_t page)
        {
            if (!s_shadow || !s_gpuOwned[page].load())
                return;
            SetProt(page, 0);
            memcpy(GuestPage(page), s_shadow + (uint64_t(page) << s_pageShift), s_pageSize);
            s_readbackBytes += s_pageSize;
            s_gpuOwned[page].store(0);
            s_blockOwned[(uint64_t(page) << s_pageShift) >> kBlockShift].fetch_sub(1, std::memory_order_release);  // after the copy
            s_shadowSeq[page].store(s_sequence.load());
            s_watched[page].store(1);
        }

        // Under s_protectMutex.
        void Reprotect(uint32_t page, ProtectRun* run = nullptr)
        {
            SetProt(page, WantedProt(page), run);
        }

        // Under s_protectMutex, for a CPU store that faulted on guest page
        // `sub` of host page `page` (`tag`: its address): mark just that
        // guest page and keep a copy of the host page to compare its other
        // guest pages with later (see s_watchSubpages). False: mark the whole
        // host page instead, or, with `untile` set (a guest page), wait for
        // that page's untile and fault again (HandleFault). `markUntiles`:
        // such a wait gave up, so pending untiles are marked as other reads.
        bool OpenSnapshot(uint32_t page, uint32_t sub, uint32_t tag, bool markUntiles, uint32_t& untile)
        {
            if (!s_snapOf || t_gpuThread)
                return false;
            uint32_t bit = 1u << (sub - page * s_subsPerPage);
            if (uint32_t open = s_snapOf[page].load(std::memory_order_relaxed))
            {
                // Another thread's fault opened the page while this one
                // waited for the lock: this guest page is marked now, the
                // others stay pending.
                s_snap[open - 1].pending &= ~bit;
                BumpSubs(page, bit, Writer::kCpuFault, tag);
                return true;
            }
            // Not watched (protected for a GPU read only): no texture relies
            // on its guest pages, so marking them all costs nothing, and a
            // copy here would be made under the lock for nothing.
            if (!s_watched[page].load(std::memory_order_relaxed))
                return false;
            // The copy must hold what the watch last vouched for: the page
            // still read-only (prot 0: another thread opened it whole), and
            // no GPU write to it pending, which changes its bytes without a
            // fault (a read fault can open such a page to read-only, see
            // HandleFault). In shadow mode PROT_NONE is a GPU-owned page.
            // A guest page with a GPU read pending can't be compared: stores
            // to it stop waiting for the GPU once the page is open, so the GPU
            // may read bytes the CPU changes and then restores, which a
            // compare can't see. A texture's untile is waited for first (as a
            // 4 KB host's store to the texture waits): marked, the texture
            // would reload, its new untile be pending at the next store to
            // its neighbour, and so on every frame. Only while the untile is
            // its guest page's newest GPU read: with a later one pending
            // there (vertex data in the texture's last page), the page would
            // be marked after the wait anyway. Any other GPU read (no
            // texture kept: vertex data) leaves its guest page marked now, as
            // the whole host page would be. (Not in shadow mode: the GPU reads
            // its copy. The renderer's NFSMW_EAGER_WRITES=0 records no GPU
            // uses: there fences wait for the GPU, so the game doesn't rewrite
            // what an untile still reads.)
            uint8_t prot = s_prot[page].load(std::memory_order_relaxed);
            uint64_t completed = s_completed.load();
            bool gpuWriting = false;
            uint32_t reading = 0, waitFor = UINT32_MAX;
            uint64_t untileUse = completed;
            for (uint32_t i = 0, s = page * s_subsPerPage; i < s_subsPerPage; i++, s++)
            {
                gpuWriting = gpuWriting || s_gpuWrite[s].load(std::memory_order_relaxed) > completed;
                uint64_t read = s_shadow ? 0 : s_gpuRead[s].load(std::memory_order_relaxed);
                if (read > completed)
                {
                    reading |= 1u << i;  // (never `sub`: HandleFault waited for its GPU uses)
                    uint64_t use = s_gpuUntile[s].load(std::memory_order_relaxed);
                    if (!markUntiles && use == read && use > untileUse)
                    {
                        untileUse = use;
                        waitFor = s;  // the last to complete
                    }
                }
            }
            if (prot == 0 || (prot == 2 && s_shadow) || gpuWriting)
            {
                s_snapWhole.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            if (waitFor != UINT32_MAX)
            {
                untile = waitFor;  // (only here: the whole-page cases above wait for nothing)
                return false;
            }
            uint32_t pending = s_allSubs & ~bit & ~reading;
            if (pending == 0)
            {
                s_snapWhole.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            // Every slot taken: the oldest snapshot closes unseen (as when it
            // ages out), which marks its pending guest pages just as marking
            // this whole host page would mark this one's.
            if (s_snapFreeCount == 0)
            {
                const Snap* oldest = &s_snap[0];
                for (const Snap& snap : s_snap)
                    if (snap.serial < oldest->serial)
                        oldest = &snap;
                CloseSnapshot(oldest->page, false);
                s_snapFull.fetch_add(1, std::memory_order_relaxed);
            }
            // PROT_NONE left by a GPU write that has completed (nothing lowers
            // it before a CPU access): readable now, and stores still fault.
            if (prot == 2)
                SetProt(page, 1);
            uint32_t slot = s_snapFree[--s_snapFreeCount];
            memcpy(s_snapData.get() + (size_t(slot) << s_pageShift), GuestPage(page), s_pageSize);
            s_snap[slot] = { page, pending, tag, s_rearms, ++s_snapSerial };
            s_snapOf[page].store(uint8_t(slot + 1), std::memory_order_relaxed);
            s_blockSnaps[(uint64_t(page) << s_pageShift) >> kBlockShift].fetch_add(1, std::memory_order_relaxed);
            g_snapsOpen.fetch_add(1, std::memory_order_relaxed);
            s_snapOpened.fetch_add(1, std::memory_order_relaxed);
            s_snapReading.fetch_add(uint32_t(__builtin_popcount(reading)), std::memory_order_relaxed);
            // Last: its release bump of the epoch publishes the snapshot, and
            // every texture checked before it is checked again (Settle first).
            BumpSubs(page, bit | reading, Writer::kCpuFault, tag);
            return true;
        }

        // Before a write sequence of physical [start, end) is read (start <
        // end), with a snapshot open somewhere (Settle checks inline): compare
        // the snapshots open on its host pages, so that their pending guest
        // pages' sequences are current. A snapshot opened after this is
        // published by an epoch bump (OpenSnapshot): such a reader checks
        // again. Every sequence reader settles first (Settle, Settled). Per
        // 256 KB block, one lock, and the open pages made read-only in runs
        // (one mprotect per window for a run of them), then compared. (Out of
        // line: one copy for SettleOpen and every Settled reader.)
        [[gnu::noinline]] void SettleRange(uint64_t start, uint64_t end)
        {
            for (uint64_t b = start >> kBlockShift; b <= (end - 1) >> kBlockShift; b++)
            {
                if (s_blockSnaps[b].load(std::memory_order_acquire) == 0)
                    continue;
                uint64_t from = std::max(start, b << kBlockShift), to = std::min(end, (b + 1) << kBlockShift);
                uint32_t page = uint32_t(from >> s_pageShift), last = uint32_t((to - 1) >> s_pageShift);
                while (page <= last && !s_snapOf[page].load(std::memory_order_acquire))
                    page++;
                if (page > last)
                    continue;
                std::lock_guard lock(s_protectMutex);
                ProtectRun run;
                for (; page <= last; page++)
                    if (s_snapOf[page].load(std::memory_order_relaxed))  // (still open: not closed meanwhile)
                        SetProt(page, 1, &run);
            }
        }

        // The sequence readers' scans of the range (size > 0), current once
        // the range is settled.
        uint64_t SequenceOf(uint32_t physical, uint32_t size)
        {
            uint32_t first = (physical & (kPhysicalSize - 1)) >> kSubShift;
            uint32_t last = std::min<uint64_t>(kSubCount - 1, (uint64_t(physical & (kPhysicalSize - 1)) + size - 1) >> kSubShift);
            uint64_t highest = 0;
            for (uint32_t sub = first; sub <= last; sub++)
                highest = std::max(highest, s_written[sub].load(std::memory_order_relaxed));
            return highest;
        }

        uint32_t PagesWrittenAfter(uint32_t physical, uint32_t size, uint64_t sequence, uint32_t* pages, uint32_t max)
        {
            uint32_t first = (physical & (kPhysicalSize - 1)) >> kSubShift;
            uint32_t last = std::min<uint64_t>(kSubCount - 1, (uint64_t(physical & (kPhysicalSize - 1)) + size - 1) >> kSubShift);
            uint32_t count = 0;
            for (uint32_t sub = first; sub <= last; sub++)
                if (s_written[sub].load(std::memory_order_relaxed) > sequence)
                {
                    if (count < max)
                        pages[count] = sub << kSubShift;
                    count++;
                }
            return count;
        }

        bool WrittenAfter(uint32_t physical, uint32_t size, uint64_t sequence)
        {
            if (!s_ranges)
                return SequenceOf(physical, size) > sequence;
            uint32_t first = (physical & (kPhysicalSize - 1)) >> kSubShift;
            uint32_t last = std::min<uint64_t>(kSubCount - 1, (uint64_t(physical & (kPhysicalSize - 1)) + size - 1) >> kSubShift);
            // A block's sequence is at least each of its pages' (BumpWritten
            // raises it first): a block not written since is skipped.
            uint32_t blockSubs = 1u << (kBlockShift - kSubShift);
            for (uint32_t sub = first; sub <= last;)
            {
                uint32_t blockLast = std::min(last, sub | (blockSubs - 1));
                if (s_blockWritten[(uint64_t(sub) << kSubShift) >> kBlockShift].load(std::memory_order_relaxed) > sequence)
                    for (uint32_t p = sub; p <= blockLast; p++)
                        if (s_written[p].load(std::memory_order_relaxed) > sequence)
                            return true;
                sub = blockLast + 1;
            }
            return false;
        }

        bool BlocksWrittenAfter(uint32_t physical, uint32_t size, uint64_t sequence)
        {
            uint64_t start = physical & (kPhysicalSize - 1);
            uint64_t end = std::min<uint64_t>(kPhysicalSize, start + size);
            for (uint64_t block = start >> kBlockShift; block <= (end - 1) >> kBlockShift; block++)
                if (s_blockWritten[block].load() > sequence)
                    return true;
            return false;
        }

        // A sequence reader with a snapshot open anywhere: Read(physical,
        // size, ...) once the range is settled (before the blocks: a compare
        // raises them). Out of line and tail-called, so that the readers'
        // common path, none open (always when off), is a load and a branch:
        // no call, no stack frame.
        template <auto Read, class... Args>
        [[gnu::noinline]] auto Settled(uint32_t physical, uint32_t size, Args... args)
        {
            SettleOpen(physical, size);
            return Read(physical, size, args...);
        }

        // The submission a CPU access to guest page `sub` must wait for: GPU
        // writes for a read, any GPU use for a write.
        uint64_t Hazard(uint32_t sub, bool write)
        {
            uint64_t use = s_gpuWrite[sub].load();
            if (s_shadow)
                return use;  // GPU reads use the shadow copy
            return write ? std::max(use, s_gpuRead[sub].load()) : use;
        }

        thread_local uint32_t t_faultPhysical = 0;
        thread_local bool t_faultWrite = false;

        // Wait (spinning; this runs in the fault handler) until the GPU is
        // done with guest page `sub` for this access (`untile`: with its
        // texture untile, see OpenSnapshot). False: an untile wait gave up.
        // That one waits for a guest page the store doesn't touch, which the
        // guest never ordered against the GPU: the storing thread may hold
        // what the command processor needs before it next submits (with
        // eager writes it runs the guest's interrupt handler itself, which
        // takes guest locks, and the kernel's dispatcher lock in KeSetEvent).
        // It gives up after kUntileWait and the untile's page is marked
        // instead (always correct: the texture reloads). The bound is well
        // above a frame of GPU latency: below it, each give-up would reload
        // the texture and the next store would wait again, every frame.
        constexpr auto kUntileWait = std::chrono::milliseconds(100);
        bool WaitForGpu(uint32_t sub, bool write, bool untile = false)
        {
            uint64_t use = untile ? s_gpuUntile[sub].load() : Hazard(sub, write);
            if (use <= s_completed.load() || t_gpuThread)
                return true;
            auto start = std::chrono::steady_clock::now();
            bool done = true;
            while (s_completed.load() < use)
            {
                if (untile && std::chrono::steady_clock::now() - start > kUntileWait)
                {
                    done = false;
                    break;
                }
                g_submitRequested.store(true);
                // While the game is suspended the GPU gate holds the
                // submission back until it's resumed: poll every 1 ms instead.
                // A signal handler can't wait on the gate, only read the flag
                // itself (a lock-free atomic; Suspended()'s seqlock would spin
                // forever in a fault that interrupted Suspend or Resume).
                struct timespec ts{ 0, guesttime::g_state.suspended.load(std::memory_order_relaxed) ? 1'000'000 : 20'000 };
                nanosleep(&ts, nullptr);
            }
            s_stalls++;
            uint64_t us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
            s_stallUs += us;
            if (s_logGuard)
            {
                static std::atomic<uint32_t> logged{ 0 };
                Dl_info dl{};
                dladdr(t_faultPc, &dl);
                if (logged++ < 400)
                    fprintf(stderr, "[guard] stall %llu us (%s %08X): %s %08X+%X (submission %llu%s) in %s\n", (unsigned long long)us,
                        t_faultWrite ? "write" : "read", t_faultPhysical, s_gpuTag[sub].load(), s_gpuRange[0][sub].load(),
                        s_gpuRange[1][sub].load(), (unsigned long long)use, done ? "" : ", gave up", dl.dli_sname ? dl.dli_sname : "?");
            }
            return done;
        }

        void Guard(uint32_t physical, uint32_t size, uint64_t submission, bool write, const char* tag, bool untile = false)
        {
            if (!s_pageCount || size == 0)
                return;
            uint64_t start = physical & (kPhysicalSize - 1);
            uint64_t end = std::min<uint64_t>(kPhysicalSize, start + size);
            uint32_t firstSub = uint32_t(start >> kSubShift), lastSub = uint32_t((end - 1) >> kSubShift);
            std::lock_guard lock(s_protectMutex);
            if (write)
                for (uint64_t b = start >> kBlockShift; b <= (end - 1) >> kBlockShift; b++)
                    if (s_blockGpuWrite[b].load(std::memory_order_relaxed) < submission)
                        s_blockGpuWrite[b].store(submission, std::memory_order_relaxed);
            // The array in a local: a store in the loop would reload the
            // global (-fno-strict-aliasing).
            std::atomic<uint64_t>* uses = (write ? s_gpuWrite : s_gpuRead).get();
            for (uint32_t sub = firstSub; sub <= lastSub; sub++)
            {
                auto& use = uses[sub];
                if (use.load(std::memory_order_relaxed) < submission)
                    Put(use, submission);
                if (s_logGuard)
                {
                    s_gpuTag[sub].store(tag);
                    s_gpuRange[0][sub].store(physical);
                    s_gpuRange[1][sub].store(size);
                }
            }
            if (untile && s_gpuUntile)
                for (uint32_t sub = firstSub; sub <= lastSub; sub++)
                    if (s_gpuUntile[sub].load(std::memory_order_relaxed) < submission)
                        Put(s_gpuUntile[sub], submission);
            if (s_shadow && !write)
                return;  // the GPU reads its copy: nothing to protect, the use is recorded for SyncShadow
            ProtectRun run;
            for (uint32_t page = firstSub / s_subsPerPage, lastPage = lastSub / s_subsPerPage; page <= lastPage; page++)
                SetProt(page, std::max(s_prot[page].load(), uint8_t(write ? 2 : 1)), &run);  // only tightens
        }
    }

    void Initialize()
    {
        s_pageSize = size_t(sysconf(_SC_PAGESIZE));
        s_pageShift = uint32_t(__builtin_ctzll(s_pageSize));
        s_pageCount = kPhysicalSize >> s_pageShift;
        s_subsPerPage = s_pageShift > kSubShift ? 1u << (s_pageShift - kSubShift) : 1u;
        s_watched.reset(new std::atomic<uint8_t>[s_pageCount]);
        s_written.reset(new std::atomic<uint64_t>[kSubCount]);
        s_prot.reset(new std::atomic<uint8_t>[s_pageCount]);
        for (uint32_t i = 0; i < s_pageCount; i++)
        {
            s_watched[i].store(0);
            s_prot[i].store(0);
        }
        for (uint32_t i = 0; i < kSubCount; i++)
            s_written[i].store(0);
        s_shadowSeq.reset(new std::atomic<uint64_t>[s_pageCount]);
        s_gpuOwned.reset(new std::atomic<uint8_t>[s_pageCount]);
        for (uint32_t i = 0; i < s_pageCount; i++)
        {
            s_shadowSeq[i].store(0);
            s_gpuOwned[i].store(0);
        }
        s_blockWritten.reset(new std::atomic<uint64_t>[kPhysicalSize >> kBlockShift]);
        s_blockGpuWrite.reset(new std::atomic<uint64_t>[kPhysicalSize >> kBlockShift]);
        s_blockOwned.reset(new std::atomic<uint32_t>[kPhysicalSize >> kBlockShift]);
        for (uint32_t i = 0; i < (kPhysicalSize >> kBlockShift); i++)
        {
            s_blockWritten[i].store(0);
            s_blockGpuWrite[i].store(0);
            s_blockOwned[i].store(0);
        }
        s_gpuRead.reset(new std::atomic<uint64_t>[kSubCount]);
        s_gpuWrite.reset(new std::atomic<uint64_t>[kSubCount]);
        for (uint32_t i = 0; i < kSubCount; i++)
        {
            s_gpuRead[i].store(0);
            s_gpuWrite[i].store(0);
        }
        if (s_logWriters)
        {
            s_lastWriter.reset(new std::atomic<uint64_t>[kSubCount]);
            s_lastWriterPc.reset(new std::atomic<uint32_t>[kSubCount]);
            for (uint32_t i = 0; i < kSubCount; i++)
            {
                s_lastWriter[i].store(0);
                s_lastWriterPc[i].store(0);
            }
            Dl_info dl{};
            if (dladdr(reinterpret_cast<const void*>(&Initialize), &dl))
                s_imageBase = static_cast<const uint8_t*>(dl.dli_fbase);
        }
        if (s_logGuard)
        {
            s_gpuTag.reset(new std::atomic<const char*>[kSubCount]);
            s_gpuRange[0].reset(new std::atomic<uint32_t>[kSubCount]);
            s_gpuRange[1].reset(new std::atomic<uint32_t>[kSubCount]);
        }
        // NFSMW_WATCH_SUBPAGES: a pending mask holds a host page's guest
        // pages, a bit each. The copies are touched now, not first in the
        // fault handler.
        if (s_watchSubpages && s_subsPerPage > 1 && s_subsPerPage <= 32)
        {
            s_snapData = std::make_unique<uint8_t[]>(size_t(kSnapSlots) << s_pageShift);
            s_snapOf.reset(new std::atomic<uint8_t>[s_pageCount]);
            for (uint32_t i = 0; i < s_pageCount; i++)
                s_snapOf[i].store(0, std::memory_order_relaxed);
            s_blockSnaps.reset(new std::atomic<uint16_t>[kPhysicalSize >> kBlockShift]);
            for (uint32_t i = 0; i < (kPhysicalSize >> kBlockShift); i++)
                s_blockSnaps[i].store(0, std::memory_order_relaxed);
            s_gpuUntile.reset(new std::atomic<uint64_t>[kSubCount]);
            for (uint32_t i = 0; i < kSubCount; i++)
                s_gpuUntile[i].store(0, std::memory_order_relaxed);
            for (uint32_t i = 0; i < kSnapSlots; i++)
                s_snapFree[i] = kSnapSlots - 1 - i;
            s_snapFreeCount = kSnapSlots;
            s_allSubs = s_subsPerPage == 32 ? ~0u : (1u << s_subsPerPage) - 1;
        }
    }

    uint64_t Current()
    {
        return s_sequence.load();
    }

    void Watch(uint32_t physical, uint32_t size)
    {
        if (!s_pageCount || size == 0)
            return;
        uint32_t first = (physical & (kPhysicalSize - 1)) >> s_pageShift;
        uint32_t last = std::min<uint64_t>(s_pageCount - 1, (uint64_t(physical & (kPhysicalSize - 1)) + size - 1) >> s_pageShift);
        std::lock_guard lock(s_protectMutex);
        ProtectRun run;
        for (uint32_t page = first; page <= last; page++)
            if (Change(s_watched[page], uint8_t(1)))
                SetProt(page, std::max<uint8_t>(s_prot[page].load(), 1), &run);
    }

    uint64_t WriteEpoch()
    {
        return s_writeEpoch.load(std::memory_order_acquire);
    }

    void SettleOpen(uint32_t physical, uint32_t size)
    {
        if (!s_snapOf || size == 0)
            return;
        uint64_t start = physical & (kPhysicalSize - 1);
        SettleRange(start, std::min<uint64_t>(kPhysicalSize, start + size));
    }

    uint64_t WriteSequence(uint32_t physical, uint32_t size)
    {
        if (!s_pageCount || size == 0)
            return 0;
        if (g_snapsOpen.load(std::memory_order_acquire))
            return Settled<SequenceOf>(physical, size);
        return SequenceOf(physical, size);
    }

    void SetWriterFrame(uint64_t frame)
    {
        s_writerFrame.store(frame, std::memory_order_relaxed);
    }

    LastWrite LastWriter(uint32_t physical)
    {
        if (!s_lastWriter)
            return {};
        uint32_t sub = (physical & (kPhysicalSize - 1)) >> kSubShift;
        uint64_t v = s_lastWriter[sub].load(std::memory_order_relaxed);
        return { Writer(v >> 60), uint32_t(v >> 32) & 0x0FFFFFFF, uint32_t(v), s_lastWriterPc[sub].load(std::memory_order_relaxed) };
    }

    uint32_t WrittenPages(uint32_t physical, uint32_t size, uint64_t sequence, uint32_t* pages, uint32_t max)
    {
        if (!s_pageCount || size == 0)
            return 0;
        if (g_snapsOpen.load(std::memory_order_acquire))
            return Settled<PagesWrittenAfter>(physical, size, sequence, pages, max);
        return PagesWrittenAfter(physical, size, sequence, pages, max);
    }

    uint64_t MarkWritten(uint32_t physical, uint32_t size, Writer kind, uint32_t tag)
    {
        if (!s_pageCount || size == 0)
            return 0;
        if (s_ranges)
        {
            uint32_t firstSub = (physical & (kPhysicalSize - 1)) >> kSubShift;
            uint32_t lastSub = std::min<uint64_t>(kSubCount - 1, (uint64_t(physical & (kPhysicalSize - 1)) + size - 1) >> kSubShift);
            return BumpWrittenRange(firstSub, lastSub, kind, tag);
        }
        uint32_t first = (physical & (kPhysicalSize - 1)) >> s_pageShift;
        uint32_t last = std::min<uint64_t>(s_pageCount - 1, (uint64_t(physical & (kPhysicalSize - 1)) + size - 1) >> s_pageShift);
        for (uint32_t page = first; page <= last; page++)
            BumpWritten(page, kind, tag);
        return 0;
    }

    bool WrittenSince(uint32_t physical, uint32_t size, uint64_t sequence)
    {
        if (!s_pageCount || size == 0)
            return false;
        if (g_snapsOpen.load(std::memory_order_acquire))
            return Settled<WrittenAfter>(physical, size, sequence);
        return WrittenAfter(physical, size, sequence);
    }

    void BeforeHostWrite(const void* host, size_t size)
    {
        if (!s_pageCount || size == 0)
            return;
        int64_t start = PhysicalOf(host);
        int64_t end = PhysicalOf(static_cast<const uint8_t*>(host) + size - 1);
        if (start < 0 || end < start)
            return;
        s_hostWriteBytes += size;
        for (int64_t sub = start >> kSubShift; sub <= end >> kSubShift; sub++)
            WaitForGpu(uint32_t(sub), true);
        std::lock_guard lock(s_protectMutex);
        ProtectRun run;
        for (int64_t page = start >> s_pageShift; page <= end >> s_pageShift; page++)
        {
            ReadBackLocked(uint32_t(page));
            // An open snapshot ends unseen (all its guest pages are marked
            // next): a compare would protect the page under the kernel's write.
            if (s_snapOf && s_snapOf[page].load(std::memory_order_relaxed))
                CloseSnapshot(uint32_t(page), false);
            BumpWritten(uint32_t(page), Writer::kHostWrite, uint32_t(start));
            s_watched[page].store(0);
            // The kernel can't fault: open the page whatever else it holds.
            SetProt(uint32_t(page), 0, &run);
            if (WantedProt(uint32_t(page)) != 0)
                s_rearm.push_back(uint32_t(page));
        }
    }

    bool HandleFault(const void* host, bool write)
    {
        if (!s_pageCount)
            return false;
        int64_t physical = PhysicalOf(host);
        if (physical < 0)
            return false;
        uint32_t page = uint32_t(physical >> s_pageShift);
        uint32_t sub = uint32_t(physical >> kSubShift);
        s_faults++;
        t_faultWrite = write;
        t_faultPhysical = uint32_t(physical);
        // Accessible again in every mirror (a racing thread may have done it
        // already; retrying the access is correct either way) once the GPU
        // is done with this guest page. The command processor may guard it
        // again while we wait: check under the lock.
        bool markUntiles = false;  // a wait for another guest page's untile gave up
        while (true)
        {
            WaitForGpu(sub, write);
            std::unique_lock lock(s_protectMutex);
            if (!t_gpuThread && Hazard(sub, write) > s_completed.load())
                continue;
            if (t_gpuThread)
            {
                // Never waits (GPU threads store through GpuThreadStore; this
                // is a fallback): the guest page's guard is dropped.
                s_gpuRead[sub].store(0);
                s_gpuWrite[sub].store(0);
            }
            if (s_shadow && s_gpuOwned[page].load())
            {
                // The GPU wrote this page (a resolve): once all of its writes
                // are done, bring the guest's copy up to date.
                uint32_t pendingSub = UINT32_MAX;
                for (uint32_t i = 0, s2 = page * s_subsPerPage; i < s_subsPerPage; i++, s2++)
                    if (!t_gpuThread && s_gpuWrite[s2].load() > s_completed.load())
                        pendingSub = s2;
                if (pendingSub != UINT32_MAX)
                {
                    lock.unlock();
                    WaitForGpu(pendingSub, false);
                    continue;
                }
                ReadBackLocked(page);
            }
            if (write)
            {
                // (NFSMW_WATCH_SUBPAGES: just this guest page, the others
                // compared later, once no texture untile reads them, or
                // marked if a wait for one gives up.)
                uint32_t untile = UINT32_MAX;
                if (!OpenSnapshot(page, sub, uint32_t(physical), markUntiles, untile))
                {
                    if (untile != UINT32_MAX)
                    {
                        lock.unlock();
                        s_snapWaits.fetch_add(1, std::memory_order_relaxed);
                        if (!WaitForGpu(untile, true, true))
                        {
                            s_snapGaveUp.fetch_add(1, std::memory_order_relaxed);
                            markUntiles = true;
                        }
                        continue;
                    }
                    BumpWritten(page, Writer::kCpuFault, uint32_t(physical));
                }
                s_watched[page].store(0);
            }
            uint8_t wanted = WantedProt(page);
            uint8_t allowed = write ? 0 : 1;
            if (wanted <= allowed)
            {
                SetProt(page, wanted);
                return true;
            }
            // Another guest page of this host page still has a GPU hazard:
            // open it for this access and protect it again soon.
            SetProt(page, allowed);
            s_rearm.push_back(page);
            s_opened++;
            return true;
        }
    }

    void Rearm()
    {
        std::lock_guard lock(s_protectMutex);
        // NFSMW_WATCH_SUBPAGES: snapshots nothing compared for kSnapAge
        // submissions give their slots back (pending pages marked unseen),
        // or pages the game writes and no texture checks would hold them all.
        s_rearms++;
        if (s_snapOf && g_snapsOpen.load(std::memory_order_relaxed))
            for (const Snap& snap : s_snap)
                if (snap.page != UINT32_MAX && s_rearms - snap.opened > kSnapAge)
                    CloseSnapshot(snap.page, false);
        if (s_rearm.empty())
            return;
        std::sort(s_rearm.begin(), s_rearm.end());
        s_rearm.erase(std::unique(s_rearm.begin(), s_rearm.end()), s_rearm.end());
        ProtectRun run;
        for (uint32_t page : s_rearm)
            Reprotect(page, &run);
        run.Flush();
        s_rearm.clear();
    }

    void GpuRead(uint32_t physical, uint32_t size, uint64_t submission, const char* tag, bool untile)
    {
        Guard(physical, size, submission, false, tag, untile);
    }

    void GpuWrite(uint32_t physical, uint32_t size, uint64_t submission, const char* tag)
    {
        Guard(physical, size, submission, true, tag);
    }

    bool GpuWritePending(uint32_t physical, uint32_t size)
    {
        if (!s_pageCount || size == 0)
            return false;
        uint64_t start = physical & (kPhysicalSize - 1);
        uint64_t end = std::min<uint64_t>(kPhysicalSize, start + size);
        uint64_t completed = s_completed.load();
        if (s_ranges)
        {
            // Blocks no pending GPU write reaches are skipped (see s_blockGpuWrite).
            for (uint64_t b = start >> kBlockShift; b <= (end - 1) >> kBlockShift; b++)
                if (s_blockGpuWrite[b].load(std::memory_order_relaxed) > completed)
                {
                    uint64_t from = std::max(start, b << kBlockShift), to = std::min(end, (b + 1) << kBlockShift);
                    for (uint64_t sub = from >> kSubShift; sub <= (to - 1) >> kSubShift; sub++)
                        if (s_gpuWrite[sub].load(std::memory_order_relaxed) > completed)
                            return true;
                }
            return false;
        }
        for (uint64_t sub = start >> kSubShift; sub <= (end - 1) >> kSubShift; sub++)
            if (s_gpuWrite[sub].load() > completed)
                return true;
        return false;
    }

    void SetCompleted(uint64_t submission)
    {
        s_completed.store(submission);
    }

    void Quiesce()
    {
        // Every recording thread holds the lock from taking its sequence
        // until its pages have it: one that took it before this waits here
        // or is done, and one that takes it after takes a newer one.
        std::lock_guard lock(s_protectMutex);
    }

    bool CpuReadable(uint32_t physical, uint32_t size)
    {
        if (!s_pageCount || size == 0)
            return true;
        uint64_t start = physical & (kPhysicalSize - 1);
        uint64_t end = std::min<uint64_t>(kPhysicalSize, start + size);
        for (uint64_t page = start >> s_pageShift; page <= (end - 1) >> s_pageShift; page++)
            if (s_prot[page].load(std::memory_order_relaxed) == 2)
                return false;
        return true;
    }

    void GpuThreadWrite(void* host, size_t size, void (*write)(const void* context), const void* context)
    {
        int64_t first = PageOf(host);
        int64_t last = PageOf(static_cast<const uint8_t*>(host) + size - 1);
        if (first < 0 || last < first)
        {
            write(context);
            return;
        }
        // Temporarily writable, stored, then protected as before; the pages
        // count as written (textures in them reload).
        std::lock_guard lock(s_protectMutex);
        bool open = true;
        for (int64_t page = first; page <= last; page++)
            open = open && s_prot[page].load() == 0;
        if (open)
        {
            write(context);
            return;
        }
        for (int64_t page = first; page <= last; page++)
        {
            ReadBackLocked(uint32_t(page));
            SetProt(uint32_t(page), 0);
        }
        write(context);
        for (int64_t page = first; page <= last; page++)
        {
            if (s_snapOf && s_snapOf[page].load(std::memory_order_relaxed))
                CloseSnapshot(uint32_t(page), false);  // (an open page of the range: marked whole next, as BeforeHostWrite)
            BumpWritten(uint32_t(page), Writer::kGpuStore, uint32_t(PhysicalOf(host)));
            s_watched[page].store(0);
            Reprotect(uint32_t(page));
        }
    }

    void GpuThreadStore(void* host, const void* data, size_t size)
    {
        GpuThreadWrite(host, size, [&] { memcpy(host, data, size); });
    }

    void EnableShadow(uint8_t* shadow)
    {
        s_shadow = shadow;
    }

    bool RangeWrittenSince(uint32_t physical, uint32_t size, uint64_t sequence)
    {
        if (!s_pageCount || size == 0)
            return false;
        if (g_snapsOpen.load(std::memory_order_acquire))
            return Settled<BlocksWrittenAfter>(physical, size, sequence);
        return BlocksWrittenAfter(physical, size, sequence);
    }

    uint64_t SyncShadow(uint32_t physical, uint32_t size, uint64_t since)
    {
        if (!s_shadow || !s_pageCount || size == 0)
            return 0;
        uint64_t start = physical & (kPhysicalSize - 1);
        uint64_t end = std::min<uint64_t>(kPhysicalSize, start + size);
        std::lock_guard lock(s_protectMutex);
        // Protect before copying: a store after this faults and makes the
        // page dirty again; one before it is in the copy. Up to 256 pages
        // are protected (in runs) and then copied at a time.
        ProtectRun run;
        uint32_t pending[256], pendingCount = 0;
        uint64_t sequence = s_sequence.load();
        auto copyPending = [&] {
            run.Flush();
            if (pendingCount == 0 && s_ranges)
                return;  // most calls copy nothing: no clock reads
            auto copyStart = std::chrono::steady_clock::now();
            for (uint32_t i = 0; i < pendingCount; i++)
            {
                if (s_shadowSeq[pending[i]].load() == 0)
                    s_firstUploadBytes += s_pageSize;
                memcpy(s_shadow + (uint64_t(pending[i]) << s_pageShift), GuestPage(pending[i]), s_pageSize);
                Put(s_shadowSeq[pending[i]], sequence);
            }
            s_uploadNs += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - copyStart).count());
            s_uploadBytes += uint64_t(pendingCount) * s_pageSize;
            pendingCount = 0;
            sequence = s_sequence.load();
        };
        uint32_t blockPages = 1u << (kBlockShift - s_pageShift);
        for (uint64_t page = start >> s_pageShift; page <= (end - 1) >> s_pageShift; page++)
        {
            // Every page of the range was copied, clean or GPU-owned when it
            // was last synced at `since`: a 256 KB block nothing was written
            // to since then has nothing to copy.
            if (since && s_ranges && s_blockWritten[(page << s_pageShift) >> kBlockShift].load(std::memory_order_relaxed) <= since)
            {
                page |= blockPages - 1;
                continue;
            }
            if (s_gpuOwned[page].load())
                continue;  // the GPU's copy is the newer one
            uint64_t copied = s_shadowSeq[page].load();
            if (copied != 0 && PageWritten(uint32_t(page)) <= copied)
                continue;
            // Work in flight may still read the old copy: wait for it first.
            for (uint32_t i = 0, sub = uint32_t(page) * s_subsPerPage; i < s_subsPerPage; i++, sub++)
                if (s_gpuRead[sub].load() > s_completed.load())
                {
                    copyPending();
                    return s_gpuRead[sub].load();
                }
            Put(s_watched[page], uint8_t(1));
            Reprotect(uint32_t(page), &run);
            pending[pendingCount++] = uint32_t(page);
            if (pendingCount == std::size(pending))
                copyPending();
        }
        copyPending();
        return 0;
    }

    void MarkGpuOwned(uint32_t physical, uint32_t size)
    {
        if (!s_shadow || !s_pageCount || size == 0)
            return;
        uint64_t start = physical & (kPhysicalSize - 1);
        uint64_t end = std::min<uint64_t>(kPhysicalSize, start + size);
        std::lock_guard lock(s_protectMutex);
        ProtectRun run;
        for (uint64_t page = start >> s_pageShift; page <= (end - 1) >> s_pageShift; page++)
        {
            bool owned = s_gpuOwned[page].load(std::memory_order_relaxed);
            if (!owned)
                s_blockOwned[(page << s_pageShift) >> kBlockShift].fetch_add(1, std::memory_order_relaxed);
            else if (s_ranges && s_prot[page].load(std::memory_order_relaxed) == 2)
                continue;  // owned and protected already (a target resolved every frame)
            Put(s_gpuOwned[page], uint8_t(1));
            Reprotect(uint32_t(page), &run);
        }
    }

    bool AnyGpuOwned(uint32_t physical, uint32_t size)
    {
        if (!s_shadow || !s_pageCount || size == 0)
            return false;
        uint64_t start = physical & (kPhysicalSize - 1);
        uint64_t end = std::min<uint64_t>(kPhysicalSize, start + size);
        if (s_ranges)
        {
            // Blocks without a GPU-owned page are skipped. Pages become owned
            // only in MarkGpuOwned, on the command processor, so a count read
            // here is never below the truth; a read-back on another thread
            // lowers it after its copy (acquire: the copy is then visible).
            for (uint64_t b = start >> kBlockShift; b <= (end - 1) >> kBlockShift; b++)
                if (s_blockOwned[b].load(std::memory_order_acquire))
                {
                    uint64_t from = std::max(start, b << kBlockShift), to = std::min(end, (b + 1) << kBlockShift);
                    for (uint64_t page = from >> s_pageShift; page <= (to - 1) >> s_pageShift; page++)
                        if (s_gpuOwned[page].load())
                            return true;
                }
            return false;
        }
        for (uint64_t page = start >> s_pageShift; page <= (end - 1) >> s_pageShift; page++)
            if (s_gpuOwned[page].load())
                return true;
        return false;
    }

    GuardStats GetGuardStats()
    {
        return { s_stalls.load(), s_stallUs.load(), s_faults.load(), s_opened.load(), s_uploadBytes.load(), s_readbackBytes.load(),
            s_protects.load(), s_protectNs.load(), s_hostWriteBytes.load(), s_firstUploadBytes.load(), s_uploadNs.load(),
            s_snapOf != nullptr, s_snapOpened.load(), s_snapClean.load(), s_snapChanged.load(), s_snapEvicted.load(), s_snapFull.load(),
            s_snapWhole.load(), s_snapReading.load(), s_snapWaits.load(), s_snapGaveUp.load() };
    }
}
