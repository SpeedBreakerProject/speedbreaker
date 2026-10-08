// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Movies on whole pairs of vblanks.
//
// The game's movies (29.97 fps WMVs) play through the XDK's XMV player. Its
// render call (sub_826E8CF8, the player's vtable slot 18, from the game's
// movie update sub_8245DD20) works out how many whole milliseconds are left
// until the next frame is due, Sleeps that long and renders the frame; the
// game then swaps with presentation interval ONE. D3D's swap sync
// (sub_82598670) has the command processor raise an interrupt whose
// callback (sub_825985A0, argument 0x0001000A: interval 1, immediate
// threshold 10%) sets how many vblank interrupts to wait, and the CP's
// WAIT_REG_MEM on 0x06449006 lets the swap through when the vblank
// interrupt (sub_82597768) counts that down: the frame goes out at the first
// vblank after the CP reaches the swap. The player's clock (sub_826DCA50) is
// the performance counter in whole ms against a base that, by the
// disassembly, its audio side moves to "now minus the audio played"
// (sub_826DCCF0), and the audio moves 256 samples (5.33 ms) at a time. So
// the kicks scatter around their ideal times: over 4 s of the attract movie
// (Steam Machine, nested gamescope at 60 Hz) they sat -5.6..+5.8 ms (sd 2.4
// ms) from a straight line, with 5-8 ms steps. The sleeps wake on time (10 us
// late) and the WAIT_REG_MEM ends 0.05-0.15 ms after the vblank interrupt.
// While the kicks' phase is within that scatter of a vblank, frames fall on
// either side of it and show for 1 and 3 vblanks instead of 2 and 2: 7-10% of
// movie frames (the "WAIT_REG_MEM" late frames of the player's hitch reports).
//
// At 60 Hz, or the rate the vblank locks to (the Deck's panel: 60.16 Hz),
// a 29.97 fps movie's phase crosses a vblank every 4.6-17 s. So a movie
// frame that would go out one vblank after the last one waits for the next
// vblank when its swap came less than the guard (10 ms) before that vblank:
// a kick that early is the clock's scatter, not a movie running ahead. One
// that comes earlier still (a clock faster than two vblanks a frame) goes out
// at once: that is where such a movie needs its 1. A late frame can't be
// helped, so a 3 stays a 3, but the frames after it keep the later phase,
// clear of the next vblank: a slow clock gets one 3 per crossing and no 1.
// A held frame shows one vblank later than it would have
// (tests/movie_cadence_sim.py replays the measured scatter).
//
// Only frames the XMV player rendered are held, after four movie frames of
// two vblanks or more in a row, at guest vblank rates near 60 Hz (RefreshFit's
// range, which takes in the free-running 16.667 ms under a display the lock
// rejects, 120 Hz say): the game's own frames, and anything that runs at a
// vblank a frame, are never touched.
// NFSMW_MOVIE_CADENCE=0 turns it off, NFSMW_MOVIE_CADENCE=<ms> sets the
// guard (default 10, at most 12). Each movie leaves a [movie] line: its
// frames by vblanks shown, and those held.
#include <stdafx.h>

#include <cpu/guest_time.h>
#include <kernel/function.h>
#include <video/presenter.h>
#include <video/refresh_fit.h>
#include <video/vblank_lock.h>

extern "C" PPC_FUNC(__imp__sub_826E8CF8);  // XMV player: sleep until the next frame is due, render it
extern "C" PPC_FUNC(__imp__sub_825985A0);  // D3D: the swap's interrupt callback

namespace
{
    int64_t NowNs()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    const double s_guardMs = [] {
        const char* v = std::getenv("NFSMW_MOVIE_CADENCE");
        double ms = v ? std::atof(v) : 10.0;
        if (v && ms <= 0)
            fprintf(stderr, "[movie] cadence off (NFSMW_MOVIE_CADENCE=0): movie frames go out at the first vblank after their swap\n");
        return std::clamp(ms, 0.0, 12.0);
    }();

    constexpr uint32_t kSteadyFrames = 4;  // movie frames of 2+ vblanks before any hold

    // D3D's device: vblank count, the vblank that released the last swap,
    // and the vblanks left before the pending swap goes (sub_825985A0 and
    // the vblank interrupt, sub_82597768, keep them).
    constexpr uint32_t kDeviceVblanks = 15600, kDeviceReleased = 15604, kDeviceCountdown = 15608;

    std::atomic<uint64_t> s_movieRenders{ 0 };  // XMV frames rendered

    // The swap callback's thread only: the command processor's (with
    // NFSMW_EAGER_WRITES=0 also the renderer's completion thread, one swap at
    // a time).
    struct
    {
        uint64_t renders = 0;       // s_movieRenders at the previous swap
        bool lastWasMovie = false;  // the previous swap was a movie frame
        uint32_t lastReleased = 0;  // the release vblank before the previous swap's
        uint32_t steady = 0;        // consecutive movie frames of 2+ vblanks
        uint64_t frames = 0, spans[5] = {}, held = 0;
    } s_state;

    void EndMovie()
    {
        auto& s = s_state;
        if (s.frames)
            fprintf(stderr, "[movie] %llu frames: %llu on 1 vblank, %llu on 2, %llu on 3, %llu on 4+; %llu held a vblank (%.1f ms guard%s)\n",
                (unsigned long long)s.frames, (unsigned long long)s.spans[1], (unsigned long long)s.spans[2],
                (unsigned long long)s.spans[3], (unsigned long long)s.spans[4], (unsigned long long)s.held, s_guardMs,
                s_guardMs > 0 ? "" : ": off");
        s.frames = s.held = 0;
        std::fill(std::begin(s.spans), std::end(s.spans), 0);
        s.steady = 0;
    }
}

PPC_FUNC(sub_826E8CF8)
{
    __imp__sub_826E8CF8(ctx, base);
    s_movieRenders.fetch_add(1, std::memory_order_release);
}

PPC_FUNC(sub_825985A0)
{
    uint32_t arg = ctx.r3.u32;
    __imp__sub_825985A0(ctx, base);

    auto& s = s_state;
    int64_t now = NowNs();
    uint32_t device = PPC_LOAD_U32(PPC_LOAD_U32(0x82000B48));  // (KeGetCurrentProcessType() is 1: the title's)
    uint32_t released = PPC_LOAD_U32(device + kDeviceReleased);
    // A movie frame: the XMV player rendered since the last swap (the game
    // thread waits for that swap before it renders the next frame).
    uint64_t renders = s_movieRenders.load(std::memory_order_acquire);
    bool movie = renders != s.renders;
    s.renders = renders;

    // The previous swap went out at `released`: its vblanks on screen.
    if (s.lastWasMovie)
    {
        uint32_t span = released - s.lastReleased;
        s.frames++;
        s.spans[std::min<uint32_t>(span, 4)]++;
        s.steady = span >= 2 ? s.steady + 1 : 0;
    }
    if (!movie && s.lastWasMovie)
        EndMovie();
    s.lastReleased = released;
    s.lastWasMovie = movie;
    if (!movie || s_guardMs <= 0 || arg >> 16 != 1 || s.steady < kSteadyFrames)
        return;

    // Due one vblank after the last release (no vblank since it, one to
    // wait): within the guard of that vblank, wait for the one after.
    if (PPC_LOAD_U32(device + kDeviceVblanks) != released)
        return;
    double period = video::GuestVblank().PeriodNs();
    if (period < 1e9 / video::RefreshFit::kMaxHz || period > 1e9 / video::RefreshFit::kMinHz)
        return;
    // Suspended (iOS: the app isn't active), or no vblank since the resume:
    // the time since the last vblank (host clock, like LastResumeNs) spans
    // the suspension, so the hold would keep the frame back for nothing.
    if (guesttime::Suspended() || video::LastGuestVblank() < guesttime::LastResumeNs())
        return;
    double sinceVblank = double(now - video::LastGuestVblank());
    if (sinceVblank < period - s_guardMs * 1e6)
        return;
    // The vblank interrupt may have run since the callback set 1; hold only
    // a countdown still at 1 (0 means it already let this swap go).
    std::atomic_ref countdown(*reinterpret_cast<uint32_t*>(base + device + kDeviceCountdown));
    uint32_t expected = ByteSwap(1u);
    if (countdown.compare_exchange_strong(expected, ByteSwap(2u)))
        s.held++;
}
