// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Write watches on guest physical memory (as Xenia's shared-memory watches):
// the renderer write-protects the pages behind a texture after loading it;
// the first CPU store to such a page faults, the handler bumps that page's
// write sequence and makes it writable again, and the texture reloads on its
// next use. GPU writes (resolves) bump the sequence explicitly (MarkWritten).
// No hashing of texture memory is needed. Sequences are kept per guest page
// (4 KB): a CPU write bumps every guest page of its host page (16 KB on
// Apple), a resolve just the guest pages it wrote. With
// NFSMW_WATCH_SUBPAGES=1 on such hosts a CPU write marks just the guest page
// it faulted on: the host page's other guest pages are copied at the fault
// and compared before anything relies on them again (write_watch.cpp).
//
// The same protection guards memory the GPU still has to access (Xenia
// publishes fences when the command processor reaches them; so do we, see
// renderer::QueueWrite). Recording a draw marks the pages it reads (vertex
// data, texture sources) or writes (resolves) with the submission that
// uses them; a CPU access to such a page before that submission completes
// waits for it. This restores the hardware's ordering exactly where the
// guest reuses memory early, instead of holding every fence until the GPU
// catches up (which throttled the game to one frame in flight).
//
// Physical memory is mapped three times (0xA/0xC/0xE windows); a watched
// page is protected in every mirror. Host code that writes guest memory
// through a syscall (pread) must call BeforeHostWrite first, since the
// kernel returns EFAULT for protected pages instead of faulting.
#pragma once
#include <cstddef>
#include <cstdint>
#include <atomic>

namespace writewatch
{
    void Initialize();

    // Current global sequence: store it when (re)loading a resource, then
    // compare with WriteSequence() to know whether it was written since.
    uint64_t Current();

    // Protect the pages of [physical, physical + size) against CPU writes.
    void Watch(uint32_t physical, uint32_t size);

    // Changes after every page write is recorded (a fault, MarkWritten, a
    // host write): read it before WriteSequence; unchanged since then means
    // no page anywhere was written. With NFSMW_WATCH_SUBPAGES, stores into
    // an open snapshot's other guest pages move neither this nor a sequence
    // until something compares them: unchanged then means nothing was
    // written in the ranges last checked through WriteSequence or
    // WrittenSince (they compare first, see Settle), and opening a snapshot
    // moves it, so that ranges checked before are checked again.
    uint64_t WriteEpoch();

    // Highest write sequence of the pages of the range.
    uint64_t WriteSequence(uint32_t physical, uint32_t size);
    // NFSMW_WATCH_SUBPAGES: compare the snapshots open on the range's host
    // pages now (the sequence readers do this themselves), e.g. before
    // Current() is taken for a load that Watch then protects. Else nothing:
    // inline, a load and a branch while no snapshot is open anywhere (always
    // when off), the walk out of line (SettleOpen).
    extern std::atomic<uint32_t> g_snapsOpen;
    void SettleOpen(uint32_t physical, uint32_t size);
    inline void Settle(uint32_t physical, uint32_t size)
    {
        if (g_snapsOpen.load(std::memory_order_acquire) != 0 && size != 0)
            SettleOpen(physical, size);
    }

    // NFSMW_LOG_UPLOADS (the renderer's texture-load log): what last moved
    // each guest page's write sequence on, kept only while that is set.
    // `tag`: the faulting or written address, or the resolve's destination;
    // `pc`: a fault's host pc, as an offset in the executable (0: unknown).
    // NFSMW_WATCH_SUBPAGES: kCompared, a CPU store into a guest page its
    // host page's snapshot found (`tag`: that page); kUnverified, a guest
    // page marked because its snapshot closed without a compare (`tag`: the
    // fault that opened it).
    enum class Writer : uint8_t { kNone, kCpuFault, kResolve, kHostWrite, kGpuStore, kRange, kCompared, kUnverified };
    struct LastWrite { Writer kind = Writer::kNone; uint32_t frame = 0, tag = 0, pc = 0; };
    void SetWriterFrame(uint64_t frame);  // stamped on the records from now on
    LastWrite LastWriter(uint32_t physical);  // of the guest page holding `physical`
    // Guest pages of the range written after `sequence`: how many; the
    // addresses of the first `max` go to `pages`.
    uint32_t WrittenPages(uint32_t physical, uint32_t size, uint64_t sequence, uint32_t* pages, uint32_t max);

    // GPU or host writes that don't fault (resolves). Returns the pages' new
    // sequence when they share one (range sequences, the default), else 0.
    uint64_t MarkWritten(uint32_t physical, uint32_t size, Writer kind = Writer::kRange, uint32_t tag = 0);
    // WriteSequence(physical, size) > sequence, skipping 256 KB blocks
    // written before `sequence` without looking at their pages.
    bool WrittenSince(uint32_t physical, uint32_t size, uint64_t sequence);
    // Returns once every write being recorded on another thread (a fault, a
    // host write) has stored its sequences: one that took its sequence
    // before this call is seen by WrittenSince after it (they are recorded
    // under one lock). For a reader that compares with a sequence it just
    // made (MarkWritten), not with an older Current().
    void Quiesce();
    // No host page of the range is unreadable now (PROT_NONE: a GPU write
    // to it, pending, or done and not accessed since): reading it can't
    // fault. Only the command processor makes pages unreadable, so this
    // holds there until it does (for debug checks that read guest memory).
    bool CpuReadable(uint32_t physical, uint32_t size);

    // A syscall is about to write host memory at [host, host + size): if it
    // is guest physical memory, unprotect and mark it.
    void BeforeHostWrite(const void* host, size_t size);

    // From the SIGSEGV/SIGBUS handler: true if this was a watched page (now
    // writable again, so the faulting store can simply be retried). Waits
    // first while the GPU still uses the page (not on GPU threads); with
    // NFSMW_WATCH_SUBPAGES, a store also waits for a texture's untile still
    // reading another guest page of its host page (see GpuRead; at most
    // 100 ms, then that guest page is marked instead).
    bool HandleFault(const void* host, bool write = true);

    // GPU use guard. `submission` is the renderer's number for the command
    // buffer being recorded; SetCompleted publishes finished ones (in order).
    // `tag` names the use for NFSMW_LOG_GUARD (a short static string).
    // `untile`: a texture's load, whose image the texture cache keeps.
    void GpuRead(uint32_t physical, uint32_t size, uint64_t submission, const char* tag = "read", bool untile = false);
    void GpuWrite(uint32_t physical, uint32_t size, uint64_t submission, const char* tag = "write");  // reads fault too
    void SetCompleted(uint64_t submission);
    // A GPU write to the range not completed yet (its CPU copy is stale).
    bool GpuWritePending(uint32_t physical, uint32_t size);
    // Protect again host pages a fault opened while another of their guest
    // pages still had a GPU hazard (command processor, at submissions). Also
    // closes NFSMW_WATCH_SUBPAGES snapshots nothing compared for a while.
    void Rearm();
    // Set by a waiter whose submission is still being recorded; the command
    // processor submits when it sees it.
    extern std::atomic<bool> g_submitRequested;
    // Threads that complete GPU work (command processor, completion thread)
    // never wait on it: they store through GpuThreadStore instead.
    extern thread_local bool t_gpuThread;
    inline thread_local const void* t_faultPc = nullptr;  // host pc of the fault, for NFSMW_LOG_GUARD
    void GpuThreadStore(void* host, const void* data, size_t size);
    // As GpuThreadStore, for several ordered stores into [host, host + size)
    // under one opening of its pages (an occlusion query's report):
    // `write(context)` runs with them writable and the write watch's lock
    // held, so it only stores.
    void GpuThreadWrite(void* host, size_t size, void (*write)(const void* context), const void* context);
    template<typename Write>
    void GpuThreadWrite(void* host, size_t size, const Write& write)
    {
        GpuThreadWrite(host, size, [](const void* w) { (*static_cast<const Write*>(w))(); }, &write);
    }

    // Shadow mode: the renderer keeps its own GPU copy of physical memory
    // (`shadow`, 512 MB, CPU-mapped) instead of importing guest memory.
    void EnableShadow(uint8_t* shadow);
    // Any page of the range written by the CPU after `sequence` (Current())?
    // Coarse and cheap: true may still mean nothing to copy.
    bool RangeWrittenSince(uint32_t physical, uint32_t size, uint64_t sequence);
    // Copy the range's pages the CPU changed since their last copy into the
    // shadow (and track further writes). Returns 0, or a submission that
    // still reads a page's old copy: wait for it, then call again. `since`:
    // Current() before the range's last complete sync, 0 if none.
    uint64_t SyncShadow(uint32_t physical, uint32_t size, uint64_t since = 0);
    // A GPU write (resolve) into the shadow: the guest's copy of those pages
    // is stale until a CPU access reads them back (after the write completes).
    void MarkGpuOwned(uint32_t physical, uint32_t size);
    bool AnyGpuOwned(uint32_t physical, uint32_t size);

    // Totals so far, for the [perf] line and hitch reports (protects: mprotect
    // calls, and their time on all threads; hostWriteBytes: file reads into
    // guest memory; firstUploadBytes: shadow pages copied for the first time;
    // uploadNs: time copying to the shadow). NFSMW_WATCH_SUBPAGES (`subpages`:
    // in use): snapshots a fault opened, closed by a compare that found no
    // pending page changed or some changed, or closed without one (aged out,
    // a host or GPU-thread write, or the oldest when a fault found every slot
    // taken: `snapFull` of them); write faults that marked the whole host
    // page instead, for the GPU (a GPU write to it pending, or just read back
    // in shadow mode; or a GPU read pending on each of its other guest pages);
    // guest pages a snapshot left out and marked at its fault, for a GPU read
    // pending on them (an untile only behind a later read there, or after a
    // wait for it gave up); write faults that waited for an untile on another
    // guest page first (in `stalls` too), and those waits that gave up.
    struct GuardStats
    {
        uint64_t stalls = 0, stallUs = 0, faults = 0, opened = 0, uploadBytes = 0, readbackBytes = 0, protects = 0, protectNs = 0,
            hostWriteBytes = 0, firstUploadBytes = 0, uploadNs = 0;
        bool subpages = false;
        uint64_t snapOpened = 0, snapClean = 0, snapChanged = 0, snapEvicted = 0, snapFull = 0, snapWhole = 0, snapReading = 0,
            snapWaits = 0, snapGaveUp = 0;
    };
    GuardStats GetGuardStats();
}
