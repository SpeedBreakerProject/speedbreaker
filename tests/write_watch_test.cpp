// Tests for the write watch (runtime/kernel/write_watch.cpp) around CPU write
// faults on hosts whose pages hold several 4 KB guest pages (16 KB on Apple):
// NFSMW_WATCH_SUBPAGES=1 marks just the faulting guest page and compares the
// host page's others before anything reads their sequences. From the repo root:
//   clang++ -std=c++20 -O2 -pthread -Iruntime -Iruntime/kernel -Iruntime/thirdparty/o1heap -Ippc
//     -Itools/XenonRecomp/XenonUtils -Itools/XenonRecomp/thirdparty/simde tests/write_watch_test.cpp
//     runtime/kernel/write_watch.cpp runtime/cpu/guest_time.cpp -o build/write_watch_test
//   build/write_watch_test
// Started without NFSMW_WATCH_SUBPAGES (whose default differs by platform),
// it runs itself with it at 0 (a write fault marks every guest page of its
// host page, as before) and at 1 (in use only where host pages are over 4 KB:
// a 4 KB host, where a host page is a guest page, checks the old rule both
// times); with it set, it runs once. Physical memory
// is mapped as the runtime maps it (three windows of one shared object), and
// SIGSEGV and SIGBUS go to HandleFault as main.cpp sends them. Under a second.
#include <stdafx.h>
#include <kernel/write_watch.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <mutex>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

// What write_watch.cpp needs of the runtime's memory: the base and the 0xE
// window's shift (memory.cpp is not linked).
Memory::Memory() {}
Memory g_memory;

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

constexpr uint32_t kGuestPage = 0x1000;
constexpr size_t kPhysicalSize = 0x20000000;

static size_t s_hostPage = 0;
static bool s_subpages = false;  // NFSMW_WATCH_SUBPAGES in use (GuardStats)

// The 0xA0000000 window: what the game's stores (and HandleFault's addresses) use.
static uint8_t* Guest(uint32_t physical, uint32_t window = 0xA0000000)
{
    return g_memory.base + window + physical;
}

static void FaultHandler(int sig, siginfo_t* info, void* context)
{
    bool isWrite = true;
#if defined(__APPLE__) && defined(__aarch64__)
    isWrite = (static_cast<ucontext_t*>(context)->uc_mcontext->__es.__esr >> 6) & 1;  // ESR WnR
#elif defined(__linux__) && defined(__x86_64__)
    isWrite = (static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_ERR] & 2) != 0;
#else
    (void)context;
#endif
    if (writewatch::HandleFault(info->si_addr, isWrite))
        return;
    fprintf(stderr, "unexpected signal %d at %p\n", sig, info->si_addr);
    _exit(2);
}

// Physical memory, mapped three times as memory.cpp maps it (the 0xE window
// shifted 4 KB where the host page size allows).
static bool MapMemory()
{
    void* reserved = mmap(nullptr, size_t(1) << 32, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (reserved == MAP_FAILED)
        return false;
    g_memory.base = static_cast<uint8_t*>(reserved);
    char name[64];
    snprintf(name, sizeof(name), "/ww-test-%d", int(getpid()));
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        return false;
    shm_unlink(name);
    if (ftruncate(fd, off_t(kPhysicalSize)) != 0)
        return false;
    for (uint32_t window : { 0xA0000000u, 0xC0000000u, 0xE0000000u })
    {
        size_t offset = window == 0xE0000000u && s_hostPage == 0x1000 ? 0x1000 : 0;
        if (window == 0xE0000000u)
            g_memory.eWindowShift = uint32_t(offset);
        if (mmap(g_memory.base + window, kPhysicalSize - offset, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, off_t(offset)) == MAP_FAILED)
            return false;
    }
    return true;
}

static writewatch::GuardStats Stats()
{
    return writewatch::GetGuardStats();
}

// A host page of four guest pages (16 KB hosts; on 4 KB hosts each is its
// own): j, a record the game rewrites every frame; k, a texture; l and m,
// more of either. `page` is host-page aligned.
struct Quad
{
    uint32_t page;
    uint32_t j() const { return page; }
    uint32_t k() const { return page + kGuestPage; }
    uint32_t l() const { return page + 2 * kGuestPage; }
    uint32_t m() const { return page + 3 * kGuestPage; }
};

// A texture's load: the sequence it is current at, then its watch.
static uint64_t Load(uint32_t physical, uint32_t size)
{
    writewatch::Settle(physical, size);
    uint64_t q = writewatch::Current();
    writewatch::Watch(physical, size);
    return q;
}

static void Store(uint32_t physical, uint8_t value)
{
    *reinterpret_cast<volatile uint8_t*>(Guest(physical) + 100) = value;
}

// The GPU, for a store that waits for it (WaitForGpu asks for a submit): a
// thread completes the submissions up to s_gpuTarget when asked, as the
// command processor's submit and the GPU would. Completions go through
// Complete, which never lowers what is published (SetCompleted stores).
static std::atomic<uint64_t> s_gpuTarget{ 0 };
static std::atomic<bool> s_gpuQuit{ false };
static std::mutex s_completeMutex;
static uint64_t s_completed = 0;  // under s_completeMutex

static void Complete(uint64_t submission)
{
    std::lock_guard lock(s_completeMutex);
    s_completed = std::max(s_completed, submission);
    writewatch::SetCompleted(s_completed);
}

static uint64_t Completed()
{
    std::lock_guard lock(s_completeMutex);
    return s_completed;
}

static void GpuThread()
{
    while (!s_gpuQuit.load())
    {
        uint64_t target = s_gpuTarget.load();  // before the request: one left from an earlier wait completes no newer target
        if (writewatch::g_submitRequested.exchange(false))
            Complete(target);
        std::this_thread::sleep_for(std::chrono::microseconds(20));
    }
}

// From here on, a store that waits for the GPU gets `submission` completed
// (everything recorded so far, as a submit).
static void GpuAnswers(uint64_t submission)
{
    writewatch::g_submitRequested.store(false);  // (left by an earlier wait)
    s_gpuTarget.store(submission);
}

// 1) A store to the neighbouring guest page leaves the texture current (with
// subpages; else it is marked whenever the two share a host page).
static void NeighbourStore(Quad h)
{
    printf("neighbour store\n");
    uint64_t q = Load(h.k(), kGuestPage);
    auto before = Stats();
    Store(h.j(), 1);
    auto after = Stats();
    bool shared = s_hostPage > kGuestPage;  // (else j is a host page of its own, not watched: no fault)
    CHECK(after.faults == before.faults + shared, "%llu faults, want %d", (unsigned long long)(after.faults - before.faults), int(shared));
    bool expected = shared && !s_subpages;
    CHECK(writewatch::WrittenSince(h.k(), kGuestPage, q) == expected, "texture written since its load: %d, want %d",
        int(!expected), int(expected));
    CHECK(writewatch::WrittenSince(h.j(), kGuestPage, q) == shared, "the faulting guest page isn't marked");
    if (s_subpages)
    {
        auto settled = Stats();
        CHECK(settled.snapOpened == before.snapOpened + 1, "%llu snapshots opened, want 1", (unsigned long long)(settled.snapOpened - before.snapOpened));
        CHECK(settled.snapClean == before.snapClean + 1, "%llu clean compares, want 1", (unsigned long long)(settled.snapClean - before.snapClean));
        // The compare protected the page again: the next store faults.
        Store(h.j(), 2);
        CHECK(Stats().faults == settled.faults + 1, "a store after the compare didn't fault");
        // Through the 0xC mirror too (protected in every window).
        writewatch::WrittenSince(h.k(), kGuestPage, q);  // compare that snapshot
        auto mirrored = Stats();
        *reinterpret_cast<volatile uint8_t*>(Guest(h.j(), 0xC0000000) + 200) = 3;
        CHECK(Stats().faults == mirrored.faults + 1, "a store through the 0xC window didn't fault");
        CHECK(!writewatch::WrittenSince(h.k(), kGuestPage, q), "texture marked by a neighbour store through the 0xC window");
    }
}

// 2) A store to the texture's own guest page while its host page is open
// (no fault) is found by the compare before its sequence is read.
static void StoreWhileOpen(Quad h)
{
    printf("store while open\n");
    uint64_t q = Load(h.k(), kGuestPage);
    auto before = Stats();
    Store(h.j(), 4);  // opens the host page
    Store(h.k(), 5);  // faults only without subpages, or on a 4 KB host
    CHECK(writewatch::WrittenSince(h.k(), kGuestPage, q), "a store into the open page's texture isn't seen");
    if (s_subpages)
    {
        auto after = Stats();
        CHECK(after.faults == before.faults + 1, "%llu faults, want 1 (the second store lands in the open page)",
            (unsigned long long)(after.faults - before.faults));
        CHECK(after.snapChanged == before.snapChanged + 1, "%llu changed compares, want 1",
            (unsigned long long)(after.snapChanged - before.snapChanged));
        // Only the changed page: l and m are unchanged.
        CHECK(!writewatch::WrittenSince(h.l(), 2 * kGuestPage, q), "unchanged guest pages marked by the compare");
    }
}

// 3) Through WriteSequence (the texture cache's revalidation and the epoch
// rule): it compares too, so a store after it faults and moves the epoch.
static void EpochAfterRevalidation(Quad h)
{
    printf("epoch after a revalidation\n");
    uint64_t q = Load(h.k(), kGuestPage);
    Store(h.j(), 6);
    uint64_t epoch = writewatch::WriteEpoch();  // read before the check, as GetTexture does
    bool valid = writewatch::WriteSequence(h.k(), kGuestPage) <= q;
    CHECK(valid || (s_hostPage > kGuestPage && !s_subpages), "texture marked by a neighbour store");
    Store(h.k(), 7);
    // Found valid, it is cached at `epoch`: the store must move it on.
    CHECK(!valid || writewatch::WriteEpoch() != epoch, "a store after a revalidation left the epoch as it was: a cached validity would stay");
    CHECK(writewatch::WrittenSince(h.k(), kGuestPage, q), "the texture's store isn't marked");
}

// 4) A fault while a GPU write to another guest page of the host page is
// pending marks the whole host page (its bytes change without a fault).
static void GpuWritePending(Quad h, uint64_t& submission)
{
    printf("GPU write pending\n");
    uint64_t q = Load(h.k(), kGuestPage);
    Complete(submission);
    writewatch::GpuWrite(h.m(), kGuestPage, ++submission, "test resolve");
    CHECK(!writewatch::CpuReadable(h.m(), kGuestPage), "a page with a GPU write pending reads as readable");
    auto before = Stats();
    Store(h.j(), 8);  // no GPU use of j itself: no wait
    auto after = Stats();
    bool shared = s_hostPage > kGuestPage;
    CHECK(writewatch::WrittenSince(h.k(), kGuestPage, q) == shared, "texture written since: %d, want %d", int(!shared), int(shared));
    if (s_subpages)
    {
        CHECK(after.snapWhole == before.snapWhole + 1, "%llu whole-page fallbacks, want 1", (unsigned long long)(after.snapWhole - before.snapWhole));
        CHECK(after.snapOpened == before.snapOpened, "a snapshot opened with a GPU write pending");
    }
    Complete(submission);
    writewatch::Rearm();
}

// 5) A snapshot nothing compares for kSnapAge submissions closes unseen,
// marking every pending guest page, even with no rearm pending (Rearm's
// common case), and leaves the page open.
static void Eviction(Quad h)
{
    printf("eviction\n");
    uint64_t q = Load(h.k(), kGuestPage);
    writewatch::Rearm();  // nothing to rearm from here on
    auto before = Stats();
    Store(h.j(), 9);
    for (int i = 0; i < 40; i++)
        writewatch::Rearm();
    auto after = Stats();
    if (s_subpages)
    {
        CHECK(after.snapEvicted == before.snapEvicted + 1, "%llu evictions, want 1", (unsigned long long)(after.snapEvicted - before.snapEvicted));
        CHECK(writewatch::WrittenSince(h.l(), kGuestPage, q), "an evicted snapshot's unchanged pending page isn't marked");
        Store(h.k(), 10);
        CHECK(Stats().faults == after.faults, "the evicted snapshot's page isn't open");
    }
    CHECK(writewatch::WrittenSince(h.k(), kGuestPage, q) == (s_hostPage > kGuestPage), "texture written since its load");
}

// 6) A host write (pread) into an open page closes its snapshot unseen: a
// compare would protect the page under the kernel's write.
static void HostWrite(Quad h)
{
    printf("host write\n");
    uint64_t q = Load(h.k(), kGuestPage);
    Store(h.j(), 11);
    auto before = Stats();
    writewatch::BeforeHostWrite(Guest(h.l()), 64);
    auto after = Stats();
    if (s_subpages)
        CHECK(after.snapEvicted == before.snapEvicted + 1, "%llu snapshots closed by the host write, want 1",
            (unsigned long long)(after.snapEvicted - before.snapEvicted));
    // A sequence reader now must not protect the page (the kernel may still be writing).
    CHECK(writewatch::WrittenSince(h.k(), kGuestPage, q) == (s_hostPage > kGuestPage), "texture written since");
    memset(Guest(h.l()), 0x5A, 64);  // the kernel's write: no fault
    CHECK(Stats().faults == after.faults, "the host-written page was protected again");
}

// 7) A page protected only for a GPU read (vertex data, no texture) takes no
// snapshot: marking all its guest pages costs nothing there.
static void Unwatched(Quad h, uint64_t& submission)
{
    printf("unwatched page\n");
    uint64_t q = writewatch::Current();
    writewatch::GpuRead(h.k(), kGuestPage, ++submission, "test vertices");
    Complete(submission);
    auto before = Stats();
    Store(h.j(), 12);
    auto after = Stats();
    CHECK(after.faults == before.faults + (s_hostPage > kGuestPage ? 1 : 0), "%llu faults", (unsigned long long)(after.faults - before.faults));
    CHECK(after.snapOpened == before.snapOpened, "a snapshot opened on an unwatched page");
    CHECK(after.snapWhole == before.snapWhole, "an unwatched page counted as a whole-page fallback");
    CHECK(writewatch::WrittenSince(h.k(), kGuestPage, q) == (s_hostPage > kGuestPage), "unwatched neighbour marked");
}

// 8) A GPU read is still pending on the texture's guest page when a store
// to the neighbouring one faults. Once the host page is open, stores to the
// texture's page stop waiting for the GPU, so the GPU may read bytes the CPU
// changes and then restores (dcbz and a copy, a clear and a redraw), which
// no compare can see. A texture's untile is waited for first: the store
// returns once it is done, and the texture's page is compared as any other.
// Another GPU read (vertex data) leaves its page marked at the fault, as the
// whole host page would be; with one on every other guest page, nothing is
// left to compare: the whole page.
static void AbaWhileGpuReads(Quad h, uint64_t& submission)
{
    printf("ABA while a GPU read is in flight\n");
    bool shared = s_hostPage > kGuestPage;
    auto flip = [&] {
        volatile uint8_t* texel = Guest(h.k()) + 100;
        uint8_t original = *texel;
        *texel = uint8_t(original ^ 0xFF);  // what the GPU can read
        *texel = original;  // the same bytes again
    };
    Complete(submission);
    uint64_t q = Load(h.k(), kGuestPage);
    uint64_t ql = Load(h.l(), kGuestPage);
    uint64_t untile = ++submission;
    writewatch::GpuRead(h.k(), kGuestPage, untile, "texture", true);  // as GuardGpu after a load: the untile runs later
    GpuAnswers(submission);
    auto before = Stats();
    Store(h.j(), 13);  // the per-bind record: no GPU use of j itself
    auto stored = Stats();
    if (s_subpages)
    {
        CHECK(Completed() >= untile, "the neighbour's store didn't wait for the texture's untile");
        CHECK(stored.snapWaits == before.snapWaits + 1, "%llu untile waits, want 1", (unsigned long long)(stored.snapWaits - before.snapWaits));
        CHECK(stored.snapOpened == before.snapOpened + 1, "%llu snapshots opened, want 1 (after the untile)",
            (unsigned long long)(stored.snapOpened - before.snapOpened));
    }
    else
        CHECK(Completed() < untile, "a store waited for an untile of another guest page");
    flip();  // (with subpages, after the untile: what it read stays right)
    CHECK(writewatch::WrittenSince(h.k(), kGuestPage, q) == !s_subpages, "texture written since: %d, want %d", int(s_subpages), int(!s_subpages));
    CHECK(writewatch::WrittenSince(h.l(), kGuestPage, ql) == (shared && !s_subpages), "a neighbour without a GPU read marked");
    Complete(submission);
    writewatch::Rearm();

    q = Load(h.k(), kGuestPage);
    ql = Load(h.l(), kGuestPage);
    writewatch::GpuRead(h.k(), kGuestPage, ++submission, "test vertices");
    GpuAnswers(submission);  // (a 4 KB host's store to k waits for the read)
    before = Stats();
    Store(h.j(), 14);
    auto opened = Stats();
    flip();
    CHECK(writewatch::WrittenSince(h.k(), kGuestPage, q), "the texture changed under its GPU read and isn't marked: it keeps what the GPU read");
    CHECK(writewatch::WrittenSince(h.l(), kGuestPage, ql) == (shared && !s_subpages), "a neighbour without a GPU read marked");
    if (s_subpages)
    {
        CHECK(opened.snapOpened == before.snapOpened + 1, "%llu snapshots opened, want 1 (for the guest pages without a GPU read)",
            (unsigned long long)(opened.snapOpened - before.snapOpened));
        CHECK(opened.snapReading == before.snapReading + 1, "%llu guest pages marked for a GPU read, want 1",
            (unsigned long long)(opened.snapReading - before.snapReading));
        CHECK(opened.snapWaits == before.snapWaits, "a store waited for a GPU read that isn't an untile");
        CHECK(opened.opened == before.opened + 1, "the page wasn't opened with its GPU hazard left (for a rearm)");
    }
    Complete(submission);
    writewatch::Rearm();

    for (bool untiles : { true, false })
    {
        uint64_t qj = Load(h.j(), kGuestPage);
        q = Load(h.k(), 3 * kGuestPage);
        writewatch::GpuRead(h.k(), 3 * kGuestPage, ++submission, untiles ? "textures" : "test vertices", untiles);  // k, l and m
        GpuAnswers(submission);
        before = Stats();
        Store(h.j(), 15);
        auto after = Stats();
        if (s_subpages)
        {
            CHECK(after.snapWhole == before.snapWhole + !untiles, "%llu whole-page fallbacks, want %d (%s)",
                (unsigned long long)(after.snapWhole - before.snapWhole), int(!untiles), untiles ? "untiles" : "no guest page left to compare");
            CHECK(after.snapOpened == before.snapOpened + untiles, "%llu snapshots opened, want %d", (unsigned long long)(after.snapOpened - before.snapOpened),
                int(untiles));
        }
        CHECK(writewatch::WrittenSince(h.j(), kGuestPage, qj), "the faulting guest page isn't marked");
        CHECK(writewatch::WrittenSince(h.k(), 3 * kGuestPage, q) == (shared && !(s_subpages && untiles)), "the textures beside it written since: want %d",
            int(shared && !(s_subpages && untiles)));
        Complete(submission);
        writewatch::Rearm();
    }
}

// 9) A compare that finds every pending guest page changed marks them all,
// and the watch ends there as at a fault that marks the whole host page: the
// next store takes no snapshot (a page the CPU rewrites every frame isn't
// copied and compared every frame). A reload watches the page again.
static void AllChanged(Quad h)
{
    printf("all pending pages changed\n");
    bool shared = s_hostPage > kGuestPage;
    uint64_t q = Load(h.k(), kGuestPage);
    Store(h.j(), 16);  // opens the host page
    Store(h.k(), 17);
    Store(h.l(), 18);
    Store(h.m(), 19);
    auto before = Stats();
    CHECK(writewatch::WrittenSince(h.k(), kGuestPage, q), "the texture's store isn't marked");
    CHECK(writewatch::WrittenSince(h.l(), 2 * kGuestPage, q) == shared, "the other changed guest pages aren't marked");
    auto compared = Stats();
    Store(h.j(), 20);  // with subpages, the compare left the page read-only: a fault, no snapshot
    auto after = Stats();
    if (s_subpages)
    {
        CHECK(compared.snapChanged == before.snapChanged + 1, "%llu changed compares, want 1",
            (unsigned long long)(compared.snapChanged - before.snapChanged));
        CHECK(after.faults == compared.faults + 1, "%llu faults, want 1", (unsigned long long)(after.faults - compared.faults));
        CHECK(after.snapOpened == compared.snapOpened, "a snapshot opened on a page every guest page of which changed (its watch should have ended)");
        CHECK(after.snapWhole == compared.snapWhole, "an unwatched page counted as a whole-page fallback");
    }
    q = Load(h.k(), kGuestPage);
    auto reloaded = Stats();
    Store(h.j(), 21);
    if (s_subpages)
        CHECK(Stats().snapOpened == reloaded.snapOpened + 1, "a store after the reload took no snapshot");
    CHECK(writewatch::WrittenSince(h.k(), kGuestPage, q) == (shared && !s_subpages), "texture marked by a neighbour store after its reload");
}

// 10) A fault that finds every snapshot slot taken closes the oldest snapshot
// unseen (its pending guest pages marked, its page left open, as when it ages
// out) and takes its slot, instead of marking its own whole host page.
// `region`: free memory for 1,024 host pages.
static void PoolFull(uint32_t region)
{
    printf("snapshot pool full\n");
    for (int i = 0; i < 40; i++)
        writewatch::Rearm();  // what is open ages out
    bool shared = s_hostPage > kGuestPage;
    uint32_t stride = std::max<uint32_t>(uint32_t(s_hostPage), 4 * kGuestPage);
    auto quad = [&](uint32_t i) { return Quad{ region + i * stride }; };
    auto start = Stats();
    std::vector<uint64_t> loads;
    for (uint32_t i = 0; i < (s_subpages ? 1024u : 4u) && Stats().snapFull == start.snapFull; i++)
    {
        loads.push_back(Load(quad(i).k(), kGuestPage));
        Store(quad(i).j(), 22);
    }
    uint32_t n = uint32_t(loads.size());
    auto after = Stats();
    if (s_subpages)
    {
        printf("  the pool was full at fault %u\n", n);
        CHECK(after.snapFull == start.snapFull + 1, "no fault found the pool full in %u", n);
        CHECK(after.snapOpened == start.snapOpened + n, "%llu snapshots opened, want %u", (unsigned long long)(after.snapOpened - start.snapOpened), n);
        CHECK(after.snapEvicted == start.snapEvicted + 1, "%llu evictions, want 1", (unsigned long long)(after.snapEvicted - start.snapEvicted));
        CHECK(after.snapWhole == start.snapWhole, "a fault on a full pool marked the whole host page");
        CHECK(writewatch::WrittenSince(quad(0).k(), kGuestPage, loads[0]), "the oldest snapshot's pending guest page isn't marked");
        Store(quad(0).l(), 23);
        CHECK(Stats().faults == after.faults, "the oldest snapshot's page isn't open");
        uint32_t marked = 0;
        for (uint32_t i = 1; i < n; i++)
            marked += writewatch::WrittenSince(quad(i).k(), kGuestPage, loads[i]);
        CHECK(marked == 0, "%u textures beside the newer snapshots marked", marked);
    }
    else
        for (uint32_t i = 0; i < n; i++)
            CHECK(writewatch::WrittenSince(quad(i).k(), kGuestPage, loads[i]) == shared, "texture %u written since: want %d", i, int(shared));
}

// 11) Snapshots open on consecutive host pages are made read-only together:
// one mprotect per window for the run, from a settle as from a GPU read's
// guard (a compare waits for its run), not one per page.
static void Batched(uint32_t region, uint64_t& submission)
{
    printf("batched compares\n");
    if (!s_subpages)
        return;
    const uint32_t pages = 4;  // in one 256 KB block (`region` is aligned)
    const char* runs = std::getenv("NFSMW_PROTECT_RUNS");  // =0: a call per page, by design
    uint32_t calls = runs && runs[0] == '0' ? 3 * pages : 3;
    uint64_t q = Load(region, pages * uint32_t(s_hostPage));
    for (int pass = 0; pass < 2; pass++)
    {
        for (uint32_t i = 0; i < pages; i++)
            Store(region + i * uint32_t(s_hostPage), 24);
        auto before = Stats();
        if (pass == 0)
            writewatch::Settle(region, pages * uint32_t(s_hostPage));
        else
            writewatch::GpuRead(region, pages * uint32_t(s_hostPage), ++submission, "test vertices");
        auto after = Stats();
        CHECK(after.snapClean == before.snapClean + pages, "%llu clean compares, want %u", (unsigned long long)(after.snapClean - before.snapClean), pages);
        CHECK(after.protects - before.protects <= calls, "%llu mprotect calls for %u pages (%s), want %u",
            (unsigned long long)(after.protects - before.protects), pages, pass == 0 ? "settle" : "guard", calls);
    }
    Complete(submission);
    writewatch::Rearm();
    CHECK(!writewatch::WrittenSince(region + kGuestPage, kGuestPage, q), "an unchanged guest page marked");
}

// 12) The loop that marking a texture's guest page for its pending untile
// would make: texture k beside the per-bind record j, the GPU a submission
// behind. Each frame the game stores to j, then the command processor binds
// k (WrittenSince) and reloads it if marked, recording a new untile that is
// pending at the next frame's store, and so on. With subpages the first
// store waits for the load's untile and k stays current; without them, on
// 16 KB hosts, every store marks k (as before) and no store waits.
static void UntileLoop(Quad h, uint64_t& submission)
{
    printf("untile loop\n");
    bool shared = s_hostPage > kGuestPage;
    Complete(submission);
    uint64_t q = Load(h.k(), kGuestPage);
    writewatch::GpuRead(h.k(), kGuestPage, ++submission, "texture", true);  // the load's untile
    auto before = Stats();
    int reloads = 0;
    for (int frame = 0; frame < 20; frame++)
    {
        GpuAnswers(submission);
        Store(h.j(), uint8_t(frame));
        if (writewatch::WrittenSince(h.k(), kGuestPage, q))
        {
            reloads++;
            q = Load(h.k(), kGuestPage);
            writewatch::GpuRead(h.k(), kGuestPage, ++submission, "texture", true);
        }
        ++submission;  // the frame's submission
        Complete(submission - 2);  // the GPU a submission behind
        writewatch::Rearm();
    }
    auto after = Stats();
    int want = shared && !s_subpages ? 20 : 0;
    CHECK(reloads == want, "%d reloads in 20 frames, want %d", reloads, want);
    if (s_subpages)
    {
        CHECK(after.snapWaits == before.snapWaits + 1, "%llu untile waits, want 1 (the first store, for the load's untile)",
            (unsigned long long)(after.snapWaits - before.snapWaits));
        CHECK(after.snapReading == before.snapReading, "%llu guest pages marked for a GPU read", (unsigned long long)(after.snapReading - before.snapReading));
    }
    else
        CHECK(after.stalls == before.stalls, "%llu stalls: a store to the record waited", (unsigned long long)(after.stalls - before.stalls));
    Complete(submission);
    writewatch::Rearm();
}

// 13) The untile wait is bounded: the storing thread may hold what the
// command processor needs before it submits (a guest lock its interrupt
// handler takes). Here the untile's submission is never submitted: the
// store gives up after the bound and marks the texture's guest page instead
// (always correct: the texture reloads), and its neighbours are compared.
static void UntileGivesUp(Quad h, uint64_t& submission)
{
    printf("untile wait gives up\n");
    bool shared = s_hostPage > kGuestPage;
    Complete(submission);
    uint64_t q = Load(h.k(), kGuestPage);
    uint64_t ql = Load(h.l(), kGuestPage);
    uint64_t untile = ++submission;
    writewatch::GpuRead(h.k(), kGuestPage, untile, "texture", true);
    GpuAnswers(untile - 1);  // asked, the GPU completes everything but the untile
    auto before = Stats();
    auto start = std::chrono::steady_clock::now();
    Store(h.j(), 25);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    auto after = Stats();
    CHECK(Completed() < untile, "the untile completed");
    CHECK(ms < 1000, "the store took %.0f ms", ms);
    if (s_subpages)
    {
        CHECK(ms >= 100, "the store gave up after %.0f ms, want at least 100", ms);
        CHECK(after.snapWaits == before.snapWaits + 1 && after.snapGaveUp == before.snapGaveUp + 1, "%llu untile waits, %llu gave up; want 1 and 1",
            (unsigned long long)(after.snapWaits - before.snapWaits), (unsigned long long)(after.snapGaveUp - before.snapGaveUp));
        CHECK(after.snapReading == before.snapReading + 1, "%llu guest pages marked for a GPU read, want 1 (the untile's)",
            (unsigned long long)(after.snapReading - before.snapReading));
        CHECK(after.snapOpened == before.snapOpened + 1, "%llu snapshots opened, want 1 (for the other guest pages)",
            (unsigned long long)(after.snapOpened - before.snapOpened));
    }
    else
        CHECK(after.stalls == before.stalls, "a store waited for an untile of another guest page");
    CHECK(writewatch::WrittenSince(h.k(), kGuestPage, q) == shared, "texture written since: %d, want %d", int(!shared), int(shared));
    CHECK(writewatch::WrittenSince(h.l(), kGuestPage, ql) == (shared && !s_subpages), "a neighbour without a GPU read marked");
    Complete(submission);
    writewatch::Rearm();
}

// 14) A GPU write pending on the host page makes a store mark it whole
// (case 4): with an untile pending too, in the same submission or an
// earlier one, it marks it at once, without waiting for the untile first.
static void UntileAndGpuWrite(Quad h, uint64_t& submission)
{
    printf("untile and GPU write pending\n");
    bool shared = s_hostPage > kGuestPage;
    for (bool later : { true, false })
    {
        Complete(submission);
        uint64_t q = Load(h.k(), kGuestPage);
        uint64_t untile = ++submission;
        writewatch::GpuRead(h.k(), kGuestPage, untile, "texture", true);
        writewatch::GpuWrite(h.m(), kGuestPage, later ? ++submission : untile, "test resolve");
        GpuAnswers(untile);  // a waiting store would get the untile completed
        auto before = Stats();
        Store(h.j(), 26);
        auto after = Stats();
        CHECK(Completed() < untile, "the store waited for the untile (resolve %s)", later ? "later" : "in the same submission");
        CHECK(after.stalls == before.stalls, "%llu stalls", (unsigned long long)(after.stalls - before.stalls));
        if (s_subpages)
            CHECK(after.snapWhole == before.snapWhole + 1 && after.snapWaits == before.snapWaits && after.snapOpened == before.snapOpened,
                "%llu whole-page fallbacks, %llu untile waits, %llu snapshots opened; want 1, 0 and 0",
                (unsigned long long)(after.snapWhole - before.snapWhole), (unsigned long long)(after.snapWaits - before.snapWaits),
                (unsigned long long)(after.snapOpened - before.snapOpened));
        CHECK(writewatch::WrittenSince(h.k(), kGuestPage, q) == shared, "texture written since: %d, want %d", int(!shared), int(shared));
        Complete(submission);
        writewatch::Rearm();
    }
}

// 15) An untile is waited for only while it is its guest page's newest GPU
// read. Behind a later one (vertex data in the texture's last page), the
// page would be marked after the wait anyway: it is marked at once. In the
// same submission, the wait completes both and the page is compared.
static void UntileBehindRead(Quad h, uint64_t& submission)
{
    printf("untile behind a later GPU read\n");
    bool shared = s_hostPage > kGuestPage;
    for (bool later : { true, false })
    {
        Complete(submission);
        uint64_t q = Load(h.k(), kGuestPage);
        uint64_t untile = ++submission;
        writewatch::GpuRead(h.k(), kGuestPage, untile, "texture", true);
        writewatch::GpuRead(h.k(), kGuestPage, later ? ++submission : untile, "test vertices");
        GpuAnswers(submission);
        auto before = Stats();
        Store(h.j(), 27);
        auto after = Stats();
        bool marked = shared && (later || !s_subpages);
        CHECK(writewatch::WrittenSince(h.k(), kGuestPage, q) == marked, "texture written since: %d, want %d (read %s)", int(!marked), int(marked),
            later ? "later" : "in the same submission");
        if (s_subpages)
        {
            CHECK((Completed() >= untile) == !later, "the store %s for the untile (read %s)", later ? "waited" : "didn't wait",
                later ? "later" : "in the same submission");
            CHECK(after.snapWaits == before.snapWaits + !later, "%llu untile waits, want %d", (unsigned long long)(after.snapWaits - before.snapWaits),
                int(!later));
            CHECK(after.snapReading == before.snapReading + later, "%llu guest pages marked for a GPU read, want %d",
                (unsigned long long)(after.snapReading - before.snapReading), int(later));
            CHECK(after.snapOpened == before.snapOpened + 1, "%llu snapshots opened, want 1", (unsigned long long)(after.snapOpened - before.snapOpened));
        }
        Complete(submission);
        writewatch::Rearm();
    }
}

static int Run()
{
    s_subpages = Stats().subpages;
    printf("host pages %zu KB, NFSMW_WATCH_SUBPAGES %s, %s\n", s_hostPage / 1024, std::getenv("NFSMW_WATCH_SUBPAGES") ? std::getenv("NFSMW_WATCH_SUBPAGES") : "unset",
        s_subpages ? "in use" : "not in use");
    CHECK(!s_subpages || s_hostPage > kGuestPage, "subpages in use with 4 KB host pages");
    // Edge calls: empty ranges must do nothing (Settle has no end before its start).
    writewatch::Settle(0, 0);
    CHECK(!writewatch::WrittenSince(0, 0, 0) && writewatch::WriteSequence(0, 0) == 0, "an empty range has a write");
    writewatch::Quiesce();
    std::thread gpu(GpuThread);

    uint64_t submission = 1;
    uint32_t page = 0x01000000;  // 16 MB, every test in a host page of its own (64 KB apart)
    NeighbourStore({ page });
    StoreWhileOpen({ page += 0x10000 });
    EpochAfterRevalidation({ page += 0x10000 });
    GpuWritePending({ page += 0x10000 }, submission);
    Eviction({ page += 0x10000 });
    HostWrite({ page += 0x10000 });
    Unwatched({ page += 0x10000 }, submission);
    AbaWhileGpuReads({ page += 0x10000 }, submission);
    AllChanged({ page += 0x10000 });
    PoolFull(0x04000000);  // (up to 64 MB of it)
    Batched(0x0C000000, submission);
    UntileLoop({ page += 0x10000 }, submission);
    UntileGivesUp({ page += 0x10000 }, submission);
    UntileAndGpuWrite({ page += 0x10000 }, submission);
    UntileBehindRead({ page += 0x10000 }, submission);
    s_gpuQuit.store(true);
    gpu.join();
    printf(g_failures ? "%d failure(s)\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}

int main(int, char** argv)
{
    s_hostPage = size_t(sysconf(_SC_PAGESIZE));
    if (!MapMemory())
    {
        perror("map physical memory");
        return 1;
    }
    struct sigaction sa{};
    sa.sa_sigaction = FaultHandler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    writewatch::Initialize();
    if (std::getenv("NFSMW_WATCH_SUBPAGES"))
        return Run();
    // Off and on, each in a process of its own (the variable is read once, at
    // static initialization).
    int result = 0;
    for (const char* value : { "0", "1" })
    {
        setenv("NFSMW_WATCH_SUBPAGES", value, 1);
        pid_t child;
        if (posix_spawn(&child, argv[0], nullptr, nullptr, argv, environ) != 0)
        {
            perror("posix_spawn");
            return 1;
        }
        int status = 0;
        waitpid(child, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            result = 1;
        printf("\n");
        fflush(stdout);
    }
    return result;
}
