// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See report.h.
//
// Hang reports. A thread checks twice a second whether the game still makes
// frames. It calls a hang when either
//   - the command processor has sat in one WAIT_REG_MEM for 10 s (the
//     world-entry hang of 2026-09-28: a malformed ring left it waiting for a
//     value nothing would write), or
//   - no new frame came for 20 s while the game read almost nothing from
//     disk (a deadlock elsewhere: the CP idles, nothing is submitted).
// Loading screens and movies present frames all along (their gaps were
// measured). Time with the window hidden or the game suspended (iOS:
// the app isn't the active one) doesn't count, nor, for the no-frame check,
// time with the settings menu open. A report is written once per episode;
// the episode ends when frames come again.
//
// Each runtime thread's host stack is taken where it stands: the watchdog
// signals it (SIGURG, otherwise unused here) and its handler records
// backtrace() into the thread's slot. The recompiled game's calls are host
// calls, so the stack of a guest thread is its guest call chain. Windows has
// no signals to send: the watchdog suspends each thread in turn, copies its
// registers and the top of its stack, resumes it, and walks the copy
// (platform/stack.h), through a handle each thread opens on itself as it
// registers.
#include <stdafx.h>
#include "report.h"
#include "internal.h"

#include <apu/audio.h>
#include <cpu/guest_thread.h>
#include <cpu/guest_time.h>
#include <gpu/command_processor.h>
#include <ui/ui.h>
#include <video/presenter.h>

#include <SDL3/SDL_events.h>
#ifndef _WIN32
#include <execinfo.h>
#endif
#include <fstream>
#ifdef _WIN32
#include <platform/fault.h>
#include <platform/stack.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

namespace report
{
    namespace
    {
        constexpr int kMaxThreads = 256;
        constexpr int kMaxFrames = 48;
#ifndef _WIN32
        constexpr int kSampleSignal = SIGURG;
#endif

        struct ThreadSlot
        {
            std::atomic<bool> used{ false };
#ifdef _WIN32
            HANDLE handle = nullptr;  // the thread's own, to suspend it and read its registers
            uintptr_t stackHigh = 0;  // its stack's top (the stack grows down from it)
#else
            pthread_t thread{};
#endif
            char name[32] = "";
            PPCContext* guest = nullptr;  // under s_threadsMutex
            std::atomic<uint32_t> request{ 0 };  // the sample asked for
            std::atomic<uint32_t> answer{ 0 };   // the last one taken
            void* pc = nullptr;  // where the signal found it
            void* frames[kMaxFrames];
            int frameCount = 0;
        };
        ThreadSlot s_threads[kMaxThreads];
        std::mutex s_threadsMutex;  // registration, and a whole sampling pass
        uint32_t s_sampleSeq = 0;

        thread_local int t_slot = -1;
        thread_local char t_name[32] = "";

        // Frees the thread's slot as it ends (after any sampling pass under
        // way, so a signal never goes to a thread that is gone).
        struct Unregister
        {
            bool armed = false;
            ~Unregister()
            {
                if (!armed || t_slot < 0)
                    return;
                std::lock_guard lock(s_threadsMutex);
                s_threads[t_slot].guest = nullptr;
#ifdef _WIN32
                if (s_threads[t_slot].handle)
                    CloseHandle(s_threads[t_slot].handle);
                s_threads[t_slot].handle = nullptr;
#endif
                s_threads[t_slot].used.store(false, std::memory_order_release);
                t_slot = -1;
            }
        };
        thread_local Unregister t_unregister;

#ifndef _WIN32
        void SampleHandler(int, siginfo_t*, void* context)
        {
            int saved = errno;
            if (int slot = t_slot; slot >= 0)
            {
                ThreadSlot& s = s_threads[slot];
                s.pc = detail::ContextPc(context);
                s.frameCount = backtrace(s.frames, kMaxFrames);
                s.answer.store(s.request.load(std::memory_order_acquire), std::memory_order_release);
            }
            errno = saved;
        }
#endif

        // Under s_threadsMutex.
        void RegisterLocked(const char* name)
        {
            if (t_slot < 0)
            {
                for (int i = 0; i < kMaxThreads; i++)
                    if (!s_threads[i].used.load(std::memory_order_relaxed))
                    {
                        t_slot = i;
                        break;
                    }
                if (t_slot < 0)
                    return;
                t_unregister.armed = true;
            }
            ThreadSlot& s = s_threads[t_slot];
#ifdef _WIN32
            if (!s.handle)
            {
                s.handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE,
                    GetCurrentThreadId());
                ULONG_PTR low = 0, high = 0;
                GetCurrentThreadStackLimits(&low, &high);
                s.stackHigh = high;
                // A stack overflow's crash report runs on the stack that ran
                // out: 64 KB kept back for it (the report, StackWalk64 and
                // DbgHelp's lookups), where Windows keeps a few KB.
                ULONG guarantee = 64 * 1024;
                SetThreadStackGuarantee(&guarantee);
            }
#else
            s.thread = pthread_self();
#endif
            snprintf(s.name, sizeof(s.name), "%s", name);
            s.guest = nullptr;
            s.used.store(true, std::memory_order_release);
        }

        struct ThreadSample
        {
            std::string name;
            bool answered = false;
            bool hasGuest = false;
            uint32_t lr = 0, r1 = 0;
            void* pc = nullptr;
            std::vector<void*> frames;
        };

#ifdef _WIN32
        int s_unwound = 0;  // the last sample's threads walked without DbgHelp
#endif

        std::vector<ThreadSample> SampleThreads()
        {
#ifdef _WIN32
            // Each thread in turn: suspended only while its registers and the
            // top of its stack are copied into memory allocated before
            // (platform::stack::Capture), then walked from the copy.
            static std::vector<uint8_t> copy(1 << 20);
            static platform::stack::Snapshot snapshot;
            snapshot.buffer = copy.data();
            snapshot.capacity = copy.size();
            std::vector<ThreadSample> samples;
            s_unwound = 0;
            std::lock_guard lock(s_threadsMutex);
            for (int i = 0; i < kMaxThreads; i++)
            {
                ThreadSlot& s = s_threads[i];
                if (!s.used.load(std::memory_order_acquire) || i == t_slot)
                    continue;
                ThreadSample sample;
                sample.name = s.name;
                if (s.guest)
                {
                    // Racy, as on POSIX (the thread runs on), but the context
                    // stays: it goes away only through SetThreadGuestContext,
                    // which waits for this lock.
                    sample.hasGuest = true;
                    sample.lr = uint32_t(s.guest->lr);
                    sample.r1 = s.guest->r1.u32;
                }
                sample.frames.reserve(kMaxFrames);
                sample.answered = s.handle && platform::stack::Capture(s.handle, s.stackHigh, snapshot);
                if (sample.answered)
                {
                    void* frames[kMaxFrames];
                    int count = platform::stack::Walk(snapshot.context, frames, kMaxFrames, &snapshot);
                    if (count == 0)
                    {
                        // DbgHelp unusable (it didn't start, or failed in an
                        // earlier walk) or busy: the unwind tables alone.
                        count = platform::stack::Unwind(snapshot, frames, kMaxFrames);
                        s_unwound++;
                    }
                    sample.pc = reinterpret_cast<void*>(snapshot.context.Rip);
                    sample.frames.assign(frames, frames + count);
                }
                samples.push_back(std::move(sample));
            }
            return samples;
#else
            static std::once_flag installed;
            std::call_once(installed, [] {
                struct sigaction sa{};
                sa.sa_sigaction = SampleHandler;
                sa.sa_flags = SA_SIGINFO | SA_RESTART;
                sigemptyset(&sa.sa_mask);
                sigaction(kSampleSignal, &sa, nullptr);
            });
            std::vector<ThreadSample> samples;
            std::lock_guard lock(s_threadsMutex);
            uint32_t seq = ++s_sampleSeq;
            std::vector<int> asked;
            for (int i = 0; i < kMaxThreads; i++)
            {
                ThreadSlot& s = s_threads[i];
                if (!s.used.load(std::memory_order_acquire) || i == t_slot)
                    continue;
                s.request.store(seq, std::memory_order_release);
                if (pthread_kill(s.thread, kSampleSignal) == 0)
                    asked.push_back(i);
            }
            // A thread blocked where signals wait (in the kernel, uninterruptibly)
            // may not answer: half a second, then report it without a stack.
            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            while (std::chrono::steady_clock::now() < deadline)
            {
                bool all = true;
                for (int i : asked)
                    all &= s_threads[i].answer.load(std::memory_order_acquire) == seq;
                if (all)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            for (int i : asked)
            {
                ThreadSlot& s = s_threads[i];
                ThreadSample sample;
                sample.name = s.name;
                sample.answered = s.answer.load(std::memory_order_acquire) == seq;
                if (sample.answered)
                {
                    sample.pc = s.pc;
                    sample.frames.assign(s.frames, s.frames + std::clamp(s.frameCount, 0, kMaxFrames));
                }
                if (s.guest)
                {
                    // Racy (the thread runs on), but the stack stays: a
                    // context goes away only through SetThreadGuestContext,
                    // which waits for this lock.
                    sample.hasGuest = true;
                    sample.lr = uint32_t(s.guest->lr);
                    sample.r1 = s.guest->r1.u32;
                }
                samples.push_back(std::move(sample));
            }
            return samples;
#endif
        }

        // Innermost first: where the signal found the thread, then its
        // callers (the handler's own frames left out).
        std::string FormatStack(void* pc, const std::vector<void*>& frames)
        {
            std::string text;
            char line[1024];
            int n = 0;
            auto add = [&](void* frame) {
                detail::SafeBuf b{ line, sizeof(line) };
                b.Str("    #").Dec(n).Str(n < 10 ? "   " : "  ");
                detail::FormatFrame(b, frame, n > 0, true);
                text.append(line, b.size);
                text += '\n';
                n++;
            };
            if (pc)
                add(pc);
            for (size_t i = detail::SignalFrames(frames.data(), int(frames.size())); i < frames.size(); i++)
                if (frames[i] != pc)
                    add(frames[i]);
            return text;
        }

        std::atomic<bool> s_started{ false };

        struct Thresholds
        {
            double noFrame = 20.0;  // seconds without a new frame (and without loading)
            double cpWait = 10.0;   // seconds in one WAIT_REG_MEM
        };

        // What counts as loading: this much read from disk in the last 10 s.
        // Music keeps streaming through a hang, now and then (a test hang
        // read nothing for 4.6 s before the report); the boot's load read
        // 35 MB in 2 s.
        constexpr uint64_t kLoadingBytes = 4ull << 20;
        // Windows: a pipeline compile finished in the last 10 s counts as
        // loading too. Intel's driver (UHD 620, 101.2135) takes 400-800 ms a
        // pipeline, and the first race's ~120 held one frame for 19.5-21.6 s
        // on a cold cache with nothing read meanwhile: a hang report for a
        // game that was only compiling (the ThinkPad, 2026-10-06). A real
        // deadlock stops the compiles too (they come from the command
        // processor's draws), so it is reported 10 s after the last one at
        // most. Elsewhere as before for now: compiles there take 20-80 ms.
#ifdef _WIN32
        constexpr bool kCompilesAreLoading = true;
#else
        constexpr bool kCompilesAreLoading = false;
#endif

        void WriteHangReport(const std::string& reason, double sinceFrame, double sinceRead, int episode)
        {
            std::vector<ThreadSample> samples = SampleThreads();
            std::string text;
            text += "SpeedBreaker (NFS: Most Wanted, Xbox 360): hang report\n";
            text += std::format("Build: SpeedBreaker {}\n", BuildString());
            text += std::format("Session: {} (log: {}.log), {:.1f} s in; written {}\n", SessionName(), SessionName(),
                SessionSeconds(), detail::TimeStamp());
            text += "Why: " + reason + "\n";
            text += std::format("Game frames: {}, the last {:.1f} s ago. Read from disk: {:.1f} MB, the last read {:.1f} s ago.\n",
                gpu::FrameCount(), sinceFrame, double(g_fileBytesRead.load()) / (1 << 20), sinceRead);
            if (uint64_t suspensions = guesttime::Epoch())
                text += std::format("Suspended (the app inactive): {} times, {:.1f} s in all before the last resume{}.\n", suspensions,
                    double(guesttime::PausedNs()) / 1e9, guesttime::Suspended() ? "; suspended now" : "");
            text += "\nCommand processor:\n" + gpu::DescribeState();
            // The game's sound runs on the render callback, and some of its
            // waits on its sound commands (GitHub issue #20).
            text += "\nAudio:\n" + apu::DescribeState();
#ifdef _WIN32
            // Where the stacks' names come from (SpeedBreaker.pdb, or why
            // not), as in crash reports.
            text += std::format("\nNames: {}", platform::stack::SymbolsState());
            if (s_unwound)
                text += std::format("; {} of the stacks below walked from the unwind tables, without DbgHelp", s_unwound);
            text += "\n";
#endif
            text += "\nThreads, where each one is (host stacks, innermost first; recompiled guest functions are "
                    "sub_<guest address>):\n";
            for (const ThreadSample& s : samples)
            {
                text += "  " + s.name;
                if (s.hasGuest)
                    text += std::format("  (guest lr={:08X} r1={:08X})", s.lr, s.r1);
#ifdef _WIN32
                text += s.answered ? ":\n" : ": couldn't be suspended to read its stack\n";
#else
                text += s.answered ? ":\n" : ": didn't answer (blocked in the kernel?)\n";
#endif
                text += FormatStack(s.pc, s.frames);
            }
            text += "\nThe log's last lines:\n" + RecentOutput(400);
            text += "\nSystem:\n" + SystemSummary();

            std::filesystem::path dir = LogDirectory();
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            std::filesystem::path path = dir / std::format("hang-{}-{}.txt", SessionName(), episode);
            bool saved = false;
            {
                std::ofstream out(path, std::ios::binary | std::ios::trunc);
                out << text;
                saved = bool(out.flush());
            }
            detail::KeepNewest(dir.string(), "hang-", ".txt", 5);
            if (saved)
                fprintf(stderr, "[hang] %s. Hang report: %s\n", reason.c_str(), path.string().c_str());
            else
                fprintf(stderr, "[hang] %s. Couldn't write the hang report to %s\n", reason.c_str(), path.string().c_str());
            ui::Toast(std::format("The game seems to have stopped. A hang report was saved; to send it: {}.", detail::kSaveBugReportHint), 20.0);
        }

#ifdef _WIN32
        // The crash report's unhandled-exception filter, put back if a DLL
        // loaded since (a driver, an overlay) installed its own over it.
        void KeepCrashFilter()
        {
            static std::atomic<int> replaced{ 0 };
            if (!platform::KeepCrashFilter() && replaced.fetch_add(1) < 3)
                fprintf(stderr, "[crash] something replaced the crash report's exception filter; it is back\n");
        }
#endif

        void Watchdog(Thresholds limits)
        {
            SetHostThreadName("nfsmw-watchdog");
            using clock = std::chrono::steady_clock;
            auto seconds = [](clock::duration d) { return std::chrono::duration<double>(d).count(); };
            auto now = clock::now();
            // lastFrameAt restarts while paused (the thresholds' clock);
            // lastFrameSeen is when a frame really came (the log's).
            auto lastTick = now, lastFrameAt = now, lastFrameSeen = now, lastReadAt = now, resetAt = now;
            uint64_t lastFrames = gpu::FrameCount(), lastBytes = g_fileBytesRead.load(std::memory_order_relaxed);
            uint64_t gapBytes = lastBytes;  // read since the last frame
            uint64_t lastPipelines = g_pipelinesBuilt.load(std::memory_order_relaxed);
            uint64_t gapPipelines = lastPipelines;  // compiled since the last frame
            uint64_t lastEpoch = guesttime::Epoch();  // suspensions so far
            // Bytes read (and pipelines compiled) per tick over the last 10 s
            // (20 ticks).
            uint64_t window[20] = {}, pipelineWindow[20] = {};
            size_t windowPos = 0;
            bool episode = false, pausedInGap = false;
            int episodes = 0;
            while (true)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                if (g_endingWithMessage.load(std::memory_order_relaxed))
                    continue;  // (the game is ending with a message: no hang to report)
#ifdef _WIN32
                KeepCrashFilter();
#endif
                now = clock::now();
                // Stopped (a debugger, SIGSTOP) or the machine slept: start over.
                if (seconds(now - lastTick) > 5.0)
                {
                    lastFrameAt = lastReadAt = resetAt = now;
                    pausedInGap = true;
                }
                lastTick = now;

                uint64_t bytes = g_fileBytesRead.load(std::memory_order_relaxed);
                uint64_t pipelines = g_pipelinesBuilt.load(std::memory_order_relaxed);
                pipelineWindow[windowPos % 20] = pipelines - lastPipelines;
                window[windowPos++ % 20] = bytes - lastBytes;
                if (bytes != lastBytes)
                    lastReadAt = now;
                lastBytes = bytes;
                lastPipelines = pipelines;
                uint64_t recentBytes = 0, recentPipelines = 0;
                for (uint64_t b : window)
                    recentBytes += b;
                for (uint64_t n : pipelineWindow)
                    recentPipelines += n;
                const bool compiling = kCompilesAreLoading && recentPipelines > 0;

                const bool visible = video::WindowVisible();
                // Suspended now, or at any time since the last tick however
                // briefly: the game's time stood still, its frames and waits
                // with it.
                uint64_t epoch = guesttime::Epoch();
                const bool suspended = guesttime::Suspended() || epoch != lastEpoch;
                lastEpoch = epoch;
                bool paused = ui::CapturingInput() || !visible || suspended;
                uint64_t frames = gpu::FrameCount();
                if (frames != lastFrames)
                {
                    double gap = seconds(now - lastFrameAt);
                    if (episode)
                        fprintf(stderr, "[watchdog] frames again, %.1f s after the last one\n", seconds(now - lastFrameSeen));
                    else if (gap >= 2.0 && !pausedInGap && kCompilesAreLoading)
                        fprintf(stderr, "[watchdog] %.1f s without a new frame (%.1f MB read, %llu pipelines compiled meanwhile)\n",
                            gap, double(bytes - gapBytes) / (1 << 20), (unsigned long long)(pipelines - gapPipelines));
                    else if (gap >= 2.0 && !pausedInGap)
                        fprintf(stderr, "[watchdog] %.1f s without a new frame (%.1f MB read meanwhile)\n", gap,
                            double(bytes - gapBytes) / (1 << 20));
                    episode = false;
                    pausedInGap = false;
                    lastFrames = frames;
                    lastFrameAt = lastFrameSeen = now;
                    gapBytes = bytes;
                    gapPipelines = pipelines;
                }
                if (paused)
                {
                    // A menu, a hidden window or a suspension: not the game's doing.
                    lastFrameAt = now;
                    pausedInGap = true;
                }
                // A command processor wait counts under the menu (the game
                // draws its pause screen there, so its waits keep passing),
                // but not while the window is hidden: what a hidden window
                // holds up is untested. Nor while suspended: the command
                // processor's wait counts again from the resume.
                if (!visible || suspended)
                    resetAt = now;
                if (episode)
                    continue;

                int64_t since = gpu::WaitingSince();
                double waited = 0.0;
                if (since != 0)
                {
                    auto start = clock::time_point(std::chrono::nanoseconds(since));
                    waited = seconds(now - std::max(start, resetAt));
                }
                double noFrame = seconds(now - lastFrameAt);
                std::string reason;
                if (limits.cpWait > 0 && waited >= limits.cpWait)
                    reason = std::format("the command processor has waited {:.0f} s in one WAIT_REG_MEM", waited);
                else if (limits.noFrame > 0 && noFrame >= limits.noFrame && recentBytes < kLoadingBytes && !compiling)
                    reason = std::format("no new frame for {:.0f} s, and nothing is loading", noFrame);
                if (reason.empty())
                    continue;
                episode = true;
                WriteHangReport(reason, seconds(now - lastFrameSeen), seconds(now - lastReadAt), ++episodes);
            }
        }

        // NFSMW_TEST_CRASH / NFSMW_TEST_HANG / NFSMW_TEST_SUSPEND: flip the
        // switches on time.
        void ArmTestSwitches()
        {
            auto sleepUntil = [](double at) {
                double wait = at - SessionSeconds();
                if (wait > 0)
                    std::this_thread::sleep_for(std::chrono::duration<double>(wait));
            };
            if (const char* v = std::getenv("NFSMW_TEST_CRASH"))
            {
                double at = std::atof(v);
                fprintf(stderr, "[test] NFSMW_TEST_CRASH=%s: the game's render thread crashes at %.1f s\n", v, at);
                std::thread([at, sleepUntil] {
                    sleepUntil(at);
                    g_testCrash.store(true, std::memory_order_relaxed);
                }).detach();
            }
            if (const char* v = std::getenv("NFSMW_TEST_HANG"))
            {
                double at = std::atof(v), duration = 0.0;
                if (const char* plus = strchr(v, '+'))
                    duration = std::atof(plus + 1);
                fprintf(stderr, "[test] NFSMW_TEST_HANG=%s: the command processor's waits stop passing at %.1f s%s\n", v, at,
                    duration > 0 ? std::format(" for {:.1f} s", duration).c_str() : "");
                std::thread([at, duration, sleepUntil] {
                    sleepUntil(at);
                    fprintf(stderr, "[test] NFSMW_TEST_HANG: hanging the command processor now\n");
                    g_testHang.store(true, std::memory_order_relaxed);
                    if (duration <= 0)
                        return;
                    std::this_thread::sleep_for(std::chrono::duration<double>(duration));
                    g_testHang.store(false, std::memory_order_relaxed);
                    fprintf(stderr, "[test] NFSMW_TEST_HANG: the command processor's waits pass again\n");
                }).detach();
            }
            if (const char* v = std::getenv("NFSMW_TEST_SUSPEND"))
            {
                // <at>+<s>[b], comma-separated: the app is inactive from <at>
                // seconds for <s> seconds; "b" sends it to the background
                // too, as going home does.
                struct Window
                {
                    double at, secs;
                    bool background;
                };
                std::vector<Window> windows;
                for (const char* p = v; p && *p;)
                {
                    char* end = nullptr;
                    Window w{ std::strtod(p, &end), 0.0, false };
                    if (end != p && *end == '+')
                    {
                        const char* secs = end + 1;
                        w.secs = std::strtod(secs, &end);
                        w.background = *end == 'b';
                        if (end != secs && w.secs > 0)
                            windows.push_back(w);
                    }
                    p = strchr(p, ',');
                    p = p ? p + 1 : nullptr;
                }
                std::sort(windows.begin(), windows.end(), [](const Window& a, const Window& b) { return a.at < b.at; });
                // Without a window there is no main loop to take app events
                // (NFSMW_HEADLESS=1): this thread does what they would, as
                // often (the GPU gate is a no-op there).
                const bool headless = !video::GetVulkan();
                std::string plan;
                for (const Window& w : windows)
                    plan += std::format("{}at {:.1f} s for {:.1f} s{}", plan.empty() ? "" : ", ", w.at, w.secs,
                        w.background ? " (in the background)" : "");
                fprintf(stderr, "[test] NFSMW_TEST_SUSPEND=%s: the game suspends %s; %s\n", v, plan.empty() ? "never (nothing parsed)" : plan.c_str(),
                    headless ? "headless, so SuspendGame and ResumeGame are called directly, twice each"
                             : "the main loop gets the app events, each twice as from UIKit");
                std::thread([windows, headless, sleepUntil] {
                    int calls = 0;
                    auto callTwice = [&calls](const char* name, void (*f)()) {
                        for (int k = 0; k < 2; k++)
                        {
                            fprintf(stderr, "[test] NFSMW_TEST_SUSPEND: call %s (#%d)\n", name, ++calls);
                            f();
                        }
                    };
                    for (size_t i = 0; i < windows.size(); i++)
                    {
                        const Window& w = windows[i];
                        sleepUntil(w.at);
                        if (headless)
                            callTwice("SuspendGame", video::SuspendGame);
                        else
                        {
                            video::TestAppEvent(SDL_EVENT_WILL_ENTER_BACKGROUND);
                            if (w.background)
                                video::TestAppEvent(SDL_EVENT_DID_ENTER_BACKGROUND);
                        }
                        sleepUntil(w.at + w.secs);
                        if (!headless)
                        {
                            if (w.background)
                                video::TestAppEvent(SDL_EVENT_WILL_ENTER_FOREGROUND);
                            video::TestAppEvent(SDL_EVENT_DID_ENTER_FOREGROUND);
                            continue;
                        }
                        callTwice("ResumeGame", video::ResumeGame);
                        // RunFrame would log the audit 2 s on; a suspension
                        // before then would cancel it anyway.
                        if (i + 1 == windows.size() || windows[i + 1].at > SessionSeconds() + 2.0)
                        {
                            std::this_thread::sleep_for(std::chrono::seconds(2));
                            video::SuspendAudit();
                        }
                    }
                }).detach();
            }
        }
    }

    void RegisterThread(const char* name)
    {
        snprintf(t_name, sizeof(t_name), "%s", name);
        std::lock_guard lock(s_threadsMutex);
        RegisterLocked(name);
    }

    void SetThreadGuestContext(PPCContext* ctx)
    {
        std::lock_guard lock(s_threadsMutex);
        if (t_slot < 0 && ctx)
        {
            // A thread with a guest context but no name of its own (the vsync thread).
            char name[32] = "";
#ifdef _WIN32
            // Its description (SetThreadDescription), if anything gave it one.
            using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
            static const auto getDescription = reinterpret_cast<GetThreadDescriptionFn>(
                GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
            PWSTR description = nullptr;
            if (getDescription && SUCCEEDED(getDescription(GetCurrentThread(), &description)) && description)
            {
                if (WideCharToMultiByte(CP_UTF8, 0, description, -1, name, int(sizeof(name)), nullptr, nullptr) == 0)
                    name[0] = 0;  // (longer than a slot's name: none)
                name[sizeof(name) - 1] = 0;
                LocalFree(description);
            }
#else
            pthread_getname_np(pthread_self(), name, sizeof(name));
#endif
            snprintf(t_name, sizeof(t_name), "%s", name[0] ? name : "unnamed");
            RegisterLocked(t_name);
        }
        if (t_slot >= 0)
            s_threads[t_slot].guest = ctx;
    }

    void StartWatchdog()
    {
        if (s_started.exchange(true))
            return;
#ifdef _WIN32
        KeepCrashFilter();  // the window and the GPU's driver are up by now
#endif
        ArmTestSwitches();
        Thresholds limits;
        if (const char* v = std::getenv("NFSMW_HANG_SECS"))
        {
            limits.noFrame = std::atof(v);
            if (const char* comma = strchr(v, ','))
                limits.cpWait = std::atof(comma + 1);
            else if (limits.noFrame <= 0)
                limits.cpWait = 0;
        }
        if (limits.noFrame <= 0 && limits.cpWait <= 0)
        {
            fprintf(stderr, "[watchdog] off (NFSMW_HANG_SECS=0)\n");
            return;
        }
        std::string when;
        if (limits.noFrame > 0)
            when = std::format("{:.0f} s without a new frame (not loading)", limits.noFrame);
        if (limits.cpWait > 0)
            when += std::format("{}{:.0f} s in one command processor wait", when.empty() ? "" : " or ", limits.cpWait);
        fprintf(stderr, "[watchdog] a hang report after %s\n", when.c_str());
        std::thread(Watchdog, limits).detach();
    }

    namespace detail
    {
        const char* CurrentThreadName()
        {
            return t_name;
        }
    }
}
