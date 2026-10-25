// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
// Tiled addressing follows Xenia's gpu/texture_address.h (BSD license).
#include <stdafx.h>
#include "presenter.h"

#include <kernel/vmem.h>
#include <kernel/dispatcher.h>
#include <hid/input.h>
#include <apu/audio.h>
#include <gpu/renderer.h>
#include <game/ultrawide.h>
#include <game/movie_pacing.h>
#include <game/cover_callers.h>
#include <gpu/covers.h>
#include "gpu_choice.h"
#include <gpu/pcie_link.h>
#include "present_schedule.h"
#include "hfr_schedule.h"
#include <game/hfr.h>
#include "swapchain_use.h"
#include "picture_fit.h"
#include "frame_rate.h"
#include "swapchain_policy.h"
#include "fullscreen_policy.h"
#include <cpu/guest_thread.h>
#include <cpu/guest_time.h>
#include "vblank_lock.h"
#include "refresh_multiple.h"
#include "refresh_notice.h"
#include "display_tone.h"
#include "fsr1.h"
#include "upscale.h"
#include <ui/ui.h>
#if defined(__APPLE__) && TARGET_OS_IOS
#include <platform/ios_ui.h>
#endif
#if defined(__APPLE__) && !TARGET_OS_IOS
#include <platform/macos_window.h>
#endif
#include <user/settings.h>
#include <report/report.h>
#include <platform/thermal.h>
#include <platform/quit_signals.h>
#ifdef _WIN32
#include "repeats.h"
#include <platform/timer.h>
#endif
#include <kernel/memory_status.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <iterator>
#include <unordered_set>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <utility>
#include <vulkan/vulkan.h>

namespace video
{
    namespace
    {
        constexpr uint32_t FRAMES_IN_FLIGHT = 2;

        SDL_Window* s_window = nullptr;
        VkInstance s_instance = VK_NULL_HANDLE;
        VkSurfaceKHR s_surface = VK_NULL_HANDLE;
        VkPhysicalDevice s_physical = VK_NULL_HANDLE;
        VkDevice s_device = VK_NULL_HANDLE;
        uint32_t s_queueFamily = 0;
        VkQueue s_queue = VK_NULL_HANDLE;

        VkSwapchainKHR s_swapchain = VK_NULL_HANDLE;
        VkFormat s_swapFormat = VK_FORMAT_B8G8R8A8_UNORM;
        VkExtent2D s_swapExtent{};
        // The last swapchain's extent (width << 32 | height), for FrameRegion
        // on other threads: the renderer's Auto internal resolution follows it.
        std::atomic<uint64_t> s_regionExtent{ 0 };
        std::vector<VkImage> s_swapImages;
        std::vector<VkImageView> s_swapViews;  // for the post-processing pass
        bool s_swapReadable = false;
        // The present scale (iOS): Settings > Display > Screen Resolution's
        // 100% or 75%, or NFSMW_PRESENT_SCALE=<0.5..1> (an experiment for
        // phones, which wins). The drawables at that fraction of the screen's
        // pixels each way, which Core Animation scales up to the screen: the
        // scale pass and the UI draw (and store) fewer pixels. That upscale's
        // cost, if the GPU composites it, is outside this app's GPU timers,
        // and the picture is resampled twice: judge it by late frames, fps
        // and a look on the phone, not post-processing ms. A change applies
        // live, as V-Sync's does: a new swapchain between two frames, with
        // the view's scale set first (CreateSwapchain). Main thread.
#if defined(__APPLE__) && TARGET_OS_IOS
        const float s_envPresentScale = [] {
            const char* v = std::getenv("NFSMW_PRESENT_SCALE");
            float s = v ? std::strtof(v, nullptr) : 0.0f;
            return s >= 0.5f && s <= 1.0f ? s : 0.0f;
        }();
        float s_presentScale = 1.0f;
        // The view's scale was changed from the screen's: from then on each
        // swapchain sets it, at 1 too (back to the screen's own).
        bool s_viewScaled = false;

        float WantedPresentScale()
        {
            return s_envPresentScale > 0.0f ? s_envPresentScale : float(settings::GetInt(settings::Id::PresentScale)) / 100.0f;
        }
#else
        const float s_presentScale = 1.0f;
#endif

        VkCommandPool s_commandPool = VK_NULL_HANDLE;
        struct Frame
        {
            VkCommandBuffer cmd = VK_NULL_HANDLE;
            VkSemaphore imageAvailable = VK_NULL_HANDLE;
            VkFence inFlight = VK_NULL_HANDLE;
            VkBuffer staging = VK_NULL_HANDLE;
            VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
            void* stagingMapped = nullptr;
            VkDeviceSize stagingSize = 0;
            VkImageView sourceView = VK_NULL_HANDLE;  // this frame's view of the presented image
            bool timed = false;                       // wrote its post-processing timestamps
            upscale::Pass pass = upscale::Pass::Bicubic;  // the pass it drew with (NFSMW_UPSCALE_AB)
        } s_frames[FRAMES_IN_FLIGHT];
        // NFSMW_GPU_TIMING=1: 3 timestamps per frame slot around the
        // post-processing (before, after AA, after the scale and UI pass).
        VkQueryPool s_timestamps = VK_NULL_HANDLE;
        double s_timestampPeriod = 1.0;
        std::atomic<uint64_t> s_postFrames{ 0 }, s_postAaNs{ 0 }, s_postNs{ 0 };
        // NFSMW_UPSCALE_AB=1 (a diagnostic, with NFSMW_GPU_TIMING=1): frames
        // the FSR 1 setting enlarges alternate between FSR 1 and bicubic, one
        // frame each, and an [upscale-ab] line every 600 timed frames gives
        // each pass's post-processing GPU time per frame (anti-aliasing and
        // the UI included, as in [perf]): the same scenes, the same clocks.
        const bool s_upscaleAb = [] { const char* v = std::getenv("NFSMW_UPSCALE_AB"); return v && v[0] == '1'; }();
        uint64_t s_abFrames[3]{}, s_abNs[3]{}, s_abAaNs[3]{};  // by upscale::Pass; main thread
        std::vector<VkSemaphore> s_renderDone;  // one per swapchain image
        std::mutex s_queueMutex;
        VulkanContext s_context{};
        bool s_contextReady = false;
        uint32_t s_frameIndex = 0;

        // The source image the front buffer is copied into before scaling.
        VkImage s_source = VK_NULL_HANDLE;
        VkDeviceMemory s_sourceMemory = VK_NULL_HANDLE;
        uint32_t s_sourceWidth = 0, s_sourceHeight = 0;

        // Latest front buffer from the command processor, as BGRA8.
        std::mutex s_pendingMutex;
        std::vector<uint32_t> s_pending;
        uint32_t s_pendingWidth = 0, s_pendingHeight = 0;
        uint32_t s_pendingAspectW = 0, s_pendingAspectH = 0;  // the frame's shape (0: the image's)
        bool s_pendingNew = false;
        // Or a GPU image from the renderer (VK_IMAGE_LAYOUT_GENERAL, written by
        // an earlier queue submission).
        VkImage s_pendingImage = VK_NULL_HANDLE;
        int64_t s_pendingVblank = 0;  // the guest vblank the frame followed
        float s_pendingShift = 0.0f;  // shown that much of a vblank early (game/hfr.h: a step's own picture drawn between)
        int64_t s_pendingTarget = 0;  // when to show it (game/hfr.h on a grid that isn't a multiple of 60 Hz; 0: the usual way)
        // An in-between picture (game/hfr.h: drawn between two world steps,
        // presented before the real picture of the later one) from the
        // renderer, with its fraction of the step; shown before s_pending*.
        VkImage s_betweenImage = VK_NULL_HANDLE;
        uint32_t s_betweenWidth = 0, s_betweenHeight = 0, s_betweenAspectW = 0, s_betweenAspectH = 0;
        int64_t s_betweenVblank = 0;  // the guest vblank before it
        float s_betweenT = 0.0f;
        int64_t s_betweenTarget = 0;
        bool s_betweenNew = false;
        std::atomic<uint64_t> s_betweenDropped{ 0 }, s_betweenShown{ 0 }, s_betweenPresented{ 0 };
        std::atomic<uint64_t> s_dropped{ 0 };  // frames replaced here before they were shown
        std::atomic<uint64_t> s_repeated{ 0 }, s_shown{ 0 };  // see PresentStats
        std::atomic<int64_t> s_guestVblank{ 0 };
        std::atomic<uint64_t> s_guestVblanks{ 0 };

        // The last frame shown, drawn again under the UI while the game
        // presents nothing new (null image: the CPU path's, in s_source).
        // Main thread, under s_frontUseMutex.
        VkImage s_lastImage = VK_NULL_HANDLE;
        uint32_t s_lastWidth = 0, s_lastHeight = 0, s_lastAspectW = 0, s_lastAspectH = 0;
        // Held from taking the renderer's image until the frame using it is
        // submitted, so ReleaseFrontImage can make it safe to destroy.
        std::mutex s_frontUseMutex;
        std::chrono::steady_clock::time_point s_lastPresent{};
        std::chrono::steady_clock::time_point s_lastGameFrame{};

        bool s_uiReady = false;
        // The view scales the last new frame was rendered with (Hor+ or Vert+;
        // 1, 1: 16:9, letterboxed): a redraw keeps its picture's shape.
        float s_lastScaleX = 1.0f, s_lastScaleY = 1.0f;
        bool s_lastFreshImage = false;  // the frame being drawn is a new GPU frame
        std::string s_resolutionHint;  // Game Mode below the monitor's resolution
#ifdef _WIN32
        // The display the window is on, as SDL reports it (NoteDisplay): its
        // refresh as a period, for the waiter thread's repeat count (0:
        // unknown), and what was last logged. Main thread, but for the period.
        std::atomic<double> s_panelPeriodNs{ 0.0 };
        SDL_DisplayID s_notedDisplay = 0;
        int s_notedW = 0, s_notedH = 0;
        float s_notedHz = -1.0f;
        // The system summary's "Present" (and bug reports'): the swapchain's
        // present mode and images, and present timing's extensions.
        std::string s_presentText, s_timingText;
        void NoteDisplay(const char* why);
        // NFSMW_TEST_DISPLAY_HZ=<hz>: the rate the display is taken to run at
        // instead of SDL's (0: unset), for the log, the repeat count, the
        // refresh multiple and the refresh-rate notice, tested on a 60 Hz
        // laptop.
        float TestDisplayHz()
        {
            static const float hz = [] {
                const char* v = std::getenv("NFSMW_TEST_DISPLAY_HZ");
                return v ? float(std::atof(v)) : 0.0f;
            }();
            return hz;
        }
#endif
        std::atomic<bool> s_focused{ true };
        std::atomic<bool> s_visible{ true };  // not minimized, hidden or covered
        // The app is inactive or in the background (iOS, and other systems
        // that send SDL's app events): no GPU work may start. Changed under
        // s_queueMutex (WaitForeground waits on s_foreground for it); RunFrame
        // reads it without the lock.
        std::atomic<bool> s_background{ false };
        std::condition_variable s_foreground;
        bool SDLCALL OnAppEvent(void*, SDL_Event* e);
        // Main thread: RunFrame stopped the controller's rumble for the
        // suspension (hid::Suspend).
        bool s_inputSuspended = false;
        // When the swapchain is built again, and at what size (main thread;
        // swapchain_policy.h): not while the app is inactive, the surface
        // watched after a resume, a size event or a new swapchain,
        // SUBOPTIMAL/OUT_OF_DATE followed, SDL's view fitted back to its
        // window on iOS, and no portrait swapchain on a landscape iPhone.
        SwapchainPolicy s_swapPolicy;
        // What the next CreateSwapchain is for (its log line), and how many
        // rebuilds at an unchanged size have been logged.
        const char* s_swapReason = nullptr;
        int s_sameSizeLogged = 0;
        // Bug-report screenshots waiting for the next frame drawn (CaptureFrame).
        std::mutex s_captureMutex;
        std::vector<std::promise<CapturedFrame>> s_captureRequests;
        // Window settings as applied (settings::Generation() tells when to look again).
        uint64_t s_settingsGeneration = ~0ull;
        // Settings > Display > Fullscreen (the player's choice, which decides
        // how the game starts) and the window's own state, which the system
        // can change for the session (video/fullscreen_policy.h).
        FullscreenPolicy s_fullscreen;
        // SteamOS Game Mode: gamescope gives the game a screen of the size
        // Steam chooses and resizes only fullscreen windows to it, so the
        // window is always fullscreen there (not saved: Desktop Mode keeps
        // its own preference in the same settings.toml).
        bool s_gameMode = false;

        bool DetectGameMode()
        {
            if (const char* v = std::getenv("NFSMW_GAME_MODE"))
                return v[0] == '1';
            const char* wayland = std::getenv("GAMESCOPE_WAYLAND_DISPLAY");
            const char* desktop = std::getenv("XDG_CURRENT_DESKTOP");
            return (wayland && *wayland) || (desktop && strcmp(desktop, "gamescope") == 0);
        }

        // The connected monitor's preferred (native) mode, from the kernel:
        // under gamescope, SDL only sees the size Steam gave the game.
        // (The largest preferred mode of the connected, enabled displays,
        // external ones first: a Steam Deck docked to a monitor has both.)
        std::pair<int, int> NativeMonitorMode()
        {
            std::pair<int, int> best{ 0, 0 };
#ifdef __linux__
            bool bestExternal = false;
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator("/sys/class/drm", ec))
            {
                std::ifstream status(entry.path() / "status"), enabled(entry.path() / "enabled");
                std::string state, on;
                if (!(status >> state) || state != "connected" || !(enabled >> on) || on != "enabled")
                    continue;
                std::ifstream modes(entry.path() / "modes");
                std::string mode;
                int w = 0, h = 0;
                if (!(modes >> mode) || sscanf(mode.c_str(), "%dx%d", &w, &h) != 2)
                    continue;
                bool external = entry.path().filename().string().find("eDP") == std::string::npos;
                if ((external && !bestExternal) || (external == bestExternal && w * h > best.first * best.second))
                {
                    best = { w, h };
                    bestExternal = external;
                }
            }
#endif
            return best;
        }
        bool s_vsync = true;

        // vkDeviceWaitIdle needs every queue externally synchronised; with one
        // shared queue, wait on it under the queue mutex.
        void WaitIdle()
        {
            std::lock_guard lock(s_queueMutex);
            vkQueueWaitIdle(s_queue);
        }

        bool Check(VkResult r, const char* what)
        {
            if (r != VK_SUCCESS)
                fprintf(stderr, "[video] %s failed: %d\n", what, int(r));
            return r == VK_SUCCESS;
        }

        uint32_t FindMemoryType(uint32_t bits, VkMemoryPropertyFlags flags)
        {
            VkPhysicalDeviceMemoryProperties props;
            vkGetPhysicalDeviceMemoryProperties(s_physical, &props);
            for (uint32_t i = 0; i < props.memoryTypeCount; i++)
                if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags)
                    return i;
            return UINT32_MAX;
        }

        // ---------------------------------------------------------------
        // Present timing. With VK_KHR_present_id and VK_KHR_present_wait a
        // thread waits for each present to reach the screen and timestamps
        // it. The times measure the display's refresh (RefreshFit), which the
        // guest vblank locks to (GuestVblank), and show a game frame kept on
        // screen for more refreshes than the game gave it vblanks (repeated).
        // The waits run beside presents to the same swapchain, as the
        // extension is used: the WSIs synchronise the two internally (Mesa's,
        // MoltenVK's), though the spec leaves that to the caller. The waiter
        // uses a swapchain (its waits, the driver's reports) only through
        // s_swapchainUse, and destroying one waits until no use is on it
        // (video/swapchain_use.h).
        bool s_presentTiming = false;
        PFN_vkWaitForPresentKHR s_waitForPresent = nullptr;
        uint64_t s_presentId = 0;  // main thread: the last present's
        struct PresentTiming
        {
            uint64_t id;
            uint64_t swapchain;  // its generation (s_swapchainUse)
            int64_t presented;  // vkQueuePresentKHR (ns, steady clock)
            int64_t vblank;     // a new game frame: the guest vblank it followed; 0: a redraw
            int64_t ready;      // ready to present (= presented, but for a frame held for its refresh: VblankLock::PresentAt)
            float between = 0;  // an in-between picture's fraction of the step (game/hfr.h; its vblank is 0)
            int64_t desired = 0;  // macOS: the time it was asked to be shown at (0: when ready)
        };
        // Never destroyed: the waiter is detached and blocks on them, and
        // glibc's pthread_cond_destroy waits for blocked waiters, so an exit
        // through main's return (no xex, a cancelled install) would hang.
        std::mutex& s_timingMutex = *new std::mutex;
        std::condition_variable& s_timingCv = *new std::condition_variable;
        std::deque<PresentTiming>& s_timings = *new std::deque<PresentTiming>;  // presented, not yet seen on screen
        // The swapchain the waiter may use (never destroyed either), and its
        // generation (main thread; 0: none).
        SwapchainUse<VkSwapchainKHR>& s_swapchainUse = *new SwapchainUse<VkSwapchainKHR>;
        uint64_t s_swapchainGeneration = 0;
        FILE* s_timingLog = nullptr;          // NFSMW_VBLANK_LOG=<file>: one CSV line per present
        // Display refreshes per 60 Hz guest vblank the fit should expect: the
        // display mode's rate over 60 where that is 2-4 (video/refresh_multiple.h),
        // else 1. Set by the main thread (UpdateRefreshMultiple), read by the
        // waiter, which owns the fit.
        std::atomic<int> s_refreshMultiple{ 1 };
        // The refresh-rate notice's grid check (video/refresh_notice.h): the
        // mode's rate when it is no whole multiple of 60 Hz (0: no check;
        // main thread sets it), and whether the waiter's grid fit takes the
        // present completions at it, since when (ns, steady clock; 0: not).
        std::atomic<double> s_gridHz{ 0.0 };
        std::atomic<int64_t> s_gridFixedSince{ 0 };
        // The grid check's fit, for HFR (HfrGrid): valid, period, a refresh.
        std::atomic<bool> s_hfrGridValid{ false };
        std::atomic<int64_t> s_hfrGridPeriodNs{ 0 }, s_hfrGridNs{ 0 };
        // NFSMW_HFR_GRID_HZ=<hz>: a made-up grid at that rate (tests on any display).
        const double s_hfrGridForcedHz = [] { const char* v = std::getenv("NFSMW_HFR_GRID_HZ"); return v ? std::atof(v) : 0.0; }();

        // macOS: a new frame may be shown a set time after the guest vblank
        // that released it (VK_GOOGLE_display_timing's desiredPresentTime,
        // which MoltenVK hands to Metal's presentDrawable:atTime:): when and
        // where, and the watchdog, in video/present_schedule.h. A MacBook
        // Pro's ProMotion panel shows each present when it is ready (on a
        // 240 Hz timing grid, not every other 120 Hz refresh), so as presented
        // its frames came 8-29 ms apart while driving, with the GPU's work on
        // each; held to their vblanks' cadence they come 16.67 ms apart.
        // kShowAfterVblankNs: three of the panel's steps, past the 9-11 ms a
        // frame takes to be ready at the median (M1 Pro, the default
        // settings): 99.9% of frames 16.67 ms apart while driving, shown
        // 12-16 ms after their vblank at the median (10.7 when ready). A
        // frame not ready by then is shown when it is, as before.
        // NFSMW_PRESENT_AT=<ms> sets the time and shows frames at it on any
        // display; 0: always when ready.
        bool s_displayTiming = false;
        int64_t s_showAfterVblankNs = 0;
#if defined(__APPLE__) && !TARGET_OS_IOS
        constexpr int64_t kShowAfterVblankNs = 12'500'000;
        uint32_t s_displayTimingId = 0;  // main thread
        // The decision and the watchdog (main thread: Wanted; the waiter:
        // the driver's reports), and the frames shown at a time not reported
        // on yet: present id -> the time asked for (steady clock). The
        // mutex is never destroyed (the waiter may be in it at exit).
        std::mutex& s_scheduleMutex = *new std::mutex;
        PresentSchedule s_schedule;
        struct ScheduledPresent
        {
            uint32_t id = 0;
            int64_t desiredNs = 0;
            bool waited = false;
        };
        ScheduledPresent s_scheduled[64];
        uint32_t s_scheduledNext = 0;
        // HFR (game/hfr.h): both pictures of a step later than the set time
        // when the real ones aren't ready by it (video/hfr_schedule.h; under
        // s_scheduleMutex). Active while in-between pictures are presented
        // (until 1 s after the last one); the main thread's last asked-for time.
        HfrShowAdapt s_hfrAdapt;
        // NFSMW_HFR_SHOW_ADAPT=1 (DIAGNOSTIC): the adaptation on. Off: M2's
        // run dm2 showed it backfires (later times keep more pictures queued
        // than the swapchain has images: 80 pictures a second in a race that
        // held 120 without it).
        const bool s_hfrShowAdapt = [] { const char* v = std::getenv("NFSMW_HFR_SHOW_ADAPT"); return v && v[0] == '1'; }();
        std::atomic<int64_t> s_hfrActiveUntil{ 0 };
        int64_t s_hfrLastDesired = 0;
        constexpr int64_t kHfrMinGapNs = 4'000'000;  // between two pictures asked for (order kept, never at once)
        float s_displayHz = 0.0f;  // the display mode's, for the log (main thread)
        std::atomic<float> s_offeredHz{ 0.0f };  // the same, for HfrOffered (any thread)
#endif
        // NFSMW_VBLANK_LOG's <file>.actual.csv: the driver's own on-screen
        // time per present (Metal's presentedTime, mach_absolute_time ns, 0:
        // never shown; the first line gives the steady clock against it). The
        // present wait returns well before it there (Metal's scheduling, not
        // the panel).
        PFN_vkGetPastPresentationTimingGOOGLE s_pastTiming = nullptr;
        FILE* s_actualLog = nullptr;

        int64_t NowNs()
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

#if defined(__APPLE__) && !TARGET_OS_IOS
        // Metal's host time is mach_absolute_time's (CLOCK_UPTIME_RAW); the
        // steady clock goes on through sleep. Steady = host + this.
        int64_t HostToSteadyNs()
        {
            return NowNs() - int64_t(clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
        }

        // Waiter thread, after each present wait (`t` the present waited
        // for): the driver's reports on the presents shown since the last
        // call, to NFSMW_VBLANK_LOG's .actual.csv and, for frames shown at a
        // time, to the watchdog; and `t`, if shown at a time, counted as
        // presented (the driver reports it a present or so later). The
        // reports are read only while `t`'s swapchain is the current one,
        // and it isn't destroyed under the read: a swapchain destroyed since
        // the wait (a fullscreen toggle, a resize) has none left to read.
        void ReadDriverReports(const PresentTiming& t)
        {
            uint32_t count = 0;
            std::vector<VkPastPresentationTimingGOOGLE> past;
            s_swapchainUse.Use(t.swapchain, [&](VkSwapchainKHR swapchain) {
                if (s_pastTiming(s_device, swapchain, &count, nullptr) == VK_SUCCESS && count)
                {
                    past.resize(count);
                    if (s_pastTiming(s_device, swapchain, &count, past.data()) < VK_SUCCESS)
                        count = 0;
                    past.resize(count);
                }
            });
            const int64_t hostToSteady = HostToSteadyNs();
            bool tripped = false;
            {
                std::lock_guard lock(s_scheduleMutex);
                for (const VkPastPresentationTimingGOOGLE& e : past)
                {
                    if (s_actualLog)
                        fprintf(s_actualLog, "%u,%llu\n", e.presentID, (unsigned long long)e.actualPresentTime);
                    for (ScheduledPresent& p : s_scheduled)
                        if (p.id == e.presentID && p.desiredNs)
                        {
                            const bool shown = e.actualPresentTime != 0;
                            tripped |= s_schedule.Reported(shown, shown ? int64_t(e.actualPresentTime) + hostToSteady - p.desiredNs : 0);
                            if (const int64_t now = NowNs(); s_hfrShowAdapt && shown && now < s_hfrActiveUntil.load(std::memory_order_relaxed))
                                if (s_hfrAdapt.Report(int64_t(e.actualPresentTime) + hostToSteady - p.desiredNs, now))
                                    fprintf(stderr, "[hfr] pictures shown %.2f ms after their vblank's time (+%.2f: %s; next earlier after %.0f s "
                                                    "without a late one)\n", double(s_showAfterVblankNs + s_hfrAdapt.ExtraNs()) / 1e6,
                                        double(s_hfrAdapt.ExtraNs()) / 1e6, "the real pictures weren't ready by their time" ,
                                        double(s_hfrAdapt.HoldNs()) / 1e9);
                            p = {};
                            break;
                        }
                }
                for (ScheduledPresent& p : s_scheduled)
                    if (p.id == uint32_t(t.id) && p.desiredNs && !p.waited)
                    {
                        p.waited = true;
                        tripped |= s_schedule.Presented();
                        break;
                    }
            }
            if (tripped)
            {
                if (s_schedule.GetReason() == PresentSchedule::Reason::Unreported)
                    fprintf(stderr, "[vblank] new frames shown when ready from now on: %u frames shown at a time were presented and the "
                        "driver reported none of them (VK_GOOGLE_display_timing)\n", PresentSchedule::kUnreported);
                else
                    fprintf(stderr, "[vblank] new frames shown when ready from now on: the time asked for wasn't kept for %u of the last %u "
                        "frames shown at a time (the driver's reports, VK_GOOGLE_display_timing: %llu shown over %lld ms early, %llu never "
                        "shown)\n", s_schedule.BadInWindow(), s_schedule.ReportedInWindow(), (unsigned long long)s_schedule.Early(),
                        (long long)(PresentSchedule::kEarlyNs / 1'000'000), (unsigned long long)s_schedule.NotShown());
            }
        }
#endif

        // Waiter thread: a present was on screen at `onScreen`. `timed`: the
        // wait was under way when it happened (not caught up on afterwards).
        void OnScreen(const PresentTiming& t, int64_t onScreen, bool timed)
        {
            static RefreshFit& refresh = *new RefreshFit;  // never destroyed either (the waiter may be in it at exit)
            static int64_t lastOnScreen = 0, lastVblank = 0;
            VblankLock& vblank = GuestVblank();
            // A new multiple (another display, another mode) starts the fit
            // over: the lock free-runs until it is measured again.
            if (const int multiple = s_refreshMultiple.load(std::memory_order_relaxed); multiple != refresh.Multiple())
            {
                refresh.SetMultiple(multiple);
                vblank.SetDisplay(refresh.Estimate());
            }
            if (timed)
            {
                refresh.Add(onScreen);
                vblank.SetDisplay(refresh.Estimate());
            }
            // The notice's grid check, beside the lock's fit (never for the lock).
            if (const double gridHz = s_gridHz.load(std::memory_order_relaxed); gridHz > 0)
            {
                static RefreshFit& grid = *new RefreshFit;  // never destroyed, as `refresh`
                static double checking = 0;
                if (gridHz != checking)
                {
                    checking = gridHz;
                    grid.SetGridHz(gridHz);
                    s_gridFixedSince.store(0, std::memory_order_relaxed);
                }
                if (timed)
                {
                    grid.Add(onScreen);
                    const bool fixed = grid.Estimate().valid;
                    if (fixed)
                    {
                        s_hfrGridPeriodNs.store(int64_t(std::llround(grid.Estimate().periodNs)), std::memory_order_relaxed);
                        s_hfrGridNs.store(grid.Estimate().gridNs, std::memory_order_relaxed);
                    }
                    s_hfrGridValid.store(fixed, std::memory_order_relaxed);
                    const int64_t since = s_gridFixedSince.load(std::memory_order_relaxed);
                    if (fixed != (since != 0))
                        s_gridFixedSince.store(fixed ? onScreen : 0, std::memory_order_relaxed);
                }
            }
            const RefreshEstimate& display = refresh.Estimate();
            if (s_timingLog)
                fprintf(s_timingLog, "%llu,%lld,%lld,%lld,%d,%d,%.1f,%lld,%.0f,%lld,%d,%.3f,%lld\n", (unsigned long long)t.id, (long long)t.vblank,
                    (long long)t.presented, (long long)onScreen, timed, display.valid, display.periodNs, (long long)display.gridNs,
                    display.rmsNs, (long long)t.ready, display.multiple, t.between, (long long)t.desired);
#if defined(__APPLE__) && !TARGET_OS_IOS
            if (s_pastTiming)
                ReadDriverReports(t);
#endif
            if (t.between != 0)
                s_betweenShown++;
            if (!t.vblank)
                return;
            s_shown++;
            // A frame from before the last resume spent the suspension on
            // screen: neither a repeat nor a latency to learn from.
            if (!timed || t.vblank < guesttime::LastResumeNs())
            {
                lastOnScreen = 0;
                return;
            }
            if (lastOnScreen)
            {
#ifdef _WIN32
                // The last frame's time on screen against the time its
                // vblanks gave it, in the panel's own refreshes (SDL's rate;
                // the fit's period without one): a 144 Hz panel shows a 60 fps
                // frame for 2 or 3 refreshes, 13.9 or 20.8 ms against 16.7,
                // and each 3 counts one (the judder: the work notes' 144HZ-NOTE.md);
                // 1/60 s rounded both to one period, so it counted none.
                // At 60 Hz this is the count it always was (video/repeats.h).
                double panelNs = s_panelPeriodNs.load(std::memory_order_relaxed);
                double refreshNs = panelNs > 0.0 ? panelNs : display.valid ? display.periodNs : 1e9 / 60.0;
                int64_t vblanks = std::max<int64_t>(1, std::llround(double(t.vblank - lastVblank) / vblank.PeriodNs()));
                s_repeated += uint64_t(ExtraRefreshes(double(onScreen - lastOnScreen), vblanks, vblank.PeriodNs(), refreshNs));
#else
                // The last frame's time on screen, against the vblanks the
                // game gave it (a gap of more than 0.5 s is a pause, not a repeat).
                // At a multiple of 60 Hz (refresh_fit.h) a vblank is that
                // many refreshes, counted in refreshes.
                double refreshNs = display.valid ? display.periodNs : 1e9 / 60.0;
                int64_t shownFor = std::llround(double(onScreen - lastOnScreen) / refreshNs);
                int64_t vblanks = std::max<int64_t>(1, std::llround(double(t.vblank - lastVblank) / vblank.PeriodNs()));
                const int64_t multiple = display.valid ? display.multiple : 1;
                if (shownFor > vblanks * multiple && shownFor <= 30 * multiple)
                    s_repeated += uint64_t(shownFor - vblanks * multiple);
#endif
            }
            lastOnScreen = onScreen;
            lastVblank = t.vblank;
            vblank.Observe(t.vblank, t.presented, onScreen, t.ready);
        }

        void PresentWaiter()
        {
            SetHostThreadName("nfsmw-present");
#if defined(__APPLE__) && !TARGET_OS_IOS
            // As the threads that present (the main thread) and draw: it
            // takes MoltenVK's swapchain locks beside their presents.
            pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
            while (true)
            {
                PresentTiming t;
                {
                    std::unique_lock lock(s_timingMutex);
                    s_timingCv.wait(lock, [] { return !s_timings.empty(); });
                    t = s_timings.front();
                }
                int64_t start = NowNs();
                VkResult r = VK_ERROR_OUT_OF_DATE_KHR;  // its swapchain is gone
                s_swapchainUse.Use(t.swapchain, [&](VkSwapchainKHR swapchain) {
                    r = s_waitForPresent(s_device, swapchain, t.id, 50'000'000);
                });
                int64_t now = NowNs();
                {
                    std::lock_guard lock(s_timingMutex);
                    // Not on screen yet: wait again. A hidden window may show
                    // nothing for a long time; past 8 waiting, drop the oldest.
                    if (r == VK_TIMEOUT && s_timings.size() <= 8)
                        continue;
                    if (!s_timings.empty() && s_timings.front().id == t.id)
                        s_timings.pop_front();
                }
                if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR)
                    OnScreen(t, now, now - start > 50'000);
            }
        }

        // The surface's extent: Vulkan's current extent, else (Wayland: the
        // swapchain decides) the window's pixels.
        Extent SurfaceExtent(const VkSurfaceCapabilitiesKHR& caps)
        {
            if (caps.currentExtent.width != UINT32_MAX)
                return { caps.currentExtent.width, caps.currentExtent.height };
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(s_window, &w, &h);
            return { uint32_t(std::max(w, 0)), uint32_t(std::max(h, 0)) };
        }

        Extent SurfaceExtent()
        {
            VkSurfaceCapabilitiesKHR caps{};
            if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s_physical, s_surface, &caps) != VK_SUCCESS)
                return {};
            return SurfaceExtent(caps);
        }

        // The interface's orientation and whether the app is landscape-only
        // there: iOS's (an iPhone is), unknown and no elsewhere.
        Orientation InterfaceNow()
        {
#if defined(__APPLE__) && TARGET_OS_IOS
            return platform::ios::InterfaceOrientation(s_window);
#else
            return Orientation::Unknown;
#endif
        }

        bool LandscapeOnlyHere()
        {
#if defined(__APPLE__) && TARGET_OS_IOS
            return platform::ios::LandscapeOnly();
#else
            return false;
#endif
        }

        // iOS: the view, its window and the scene, for a log line ("" elsewhere).
        std::string GeometryNote()
        {
#if defined(__APPLE__) && TARGET_OS_IOS
            return ": " + platform::ios::DescribeGeometry(s_window);
#else
            return {};
#endif
        }

        void UpdateDisplayItem();

        double s_modeHz = 0.0;  // the window's display mode's rate (main thread; UpdateRefreshMultiple)
        std::chrono::steady_clock::time_point s_modeSince{};
        // Game frames for the refresh-rate notice: the first, and the first
        // after the last gap of RefreshNotice::kGapMs (a load). Main thread.
        std::chrono::steady_clock::time_point s_firstGameFrame{}, s_flowingSince{};

        // The refreshes per guest vblank the fit should expect on the
        // window's display (video/refresh_multiple.h), and a [vblank] line
        // when that or the mode's rate changes and there is something to say
        // (nothing at 60 Hz). Main thread: at start, each swapchain of a new
        // size, and SDL's display and mode changes.
        void UpdateRefreshMultiple(const SDL_DisplayMode* mode)
        {
            static const RefreshMultiple policy = RefreshMultiple::FromEnv(std::getenv("NFSMW_REFRESH_MULTIPLE"));
#if defined(__APPLE__)
            constexpr bool kApple = true;
#else
            constexpr bool kApple = false;
#endif
#ifdef _WIN32
            const double hz = TestDisplayHz() > 0.0f ? double(TestDisplayHz()) : mode ? double(mode->refresh_rate) : 0.0;
#else
            const double hz = mode ? double(mode->refresh_rate) : 0.0;
#endif
            const int multiple = policy.For(hz, kApple);
            s_refreshMultiple.store(multiple, std::memory_order_relaxed);
            // The refresh-rate notice: the mode's rate and since when; the
            // grid check where the rate is no whole multiple of 60 (not on
            // Apple's systems: their present waits sit on no grid).
            if (hz != s_modeHz)
            {
                s_modeHz = hz;
                s_modeSince = std::chrono::steady_clock::now();
            }
            s_gridHz.store(!kApple && hz >= RefreshFit::kMinHz && RefreshFit::MultipleOf(hz) == 0 ? hz : 0.0, std::memory_order_relaxed);
            static double loggedHz = -1.0;
            if (hz == loggedHz)
                return;
            loggedHz = hz;
            if (const std::string line = policy.Describe(hz, multiple, kApple); !line.empty())
                fprintf(stderr, "[vblank] %s\n", line.c_str());
        }

        void DestroySwapchain()
        {
            {
                s_swapchainUse.Retire();  // the waiter is off it after this, and stays off
                std::lock_guard lock(s_timingMutex);
                std::erase_if(s_timings, [](const PresentTiming& t) { return t.swapchain == s_swapchainGeneration; });
                s_swapchainGeneration = 0;
            }
            for (VkSemaphore s : s_renderDone)
                vkDestroySemaphore(s_device, s, nullptr);
            s_renderDone.clear();
            for (VkImageView v : s_swapViews)
                vkDestroyImageView(s_device, v, nullptr);
            s_swapViews.clear();
            if (s_swapchain)
                vkDestroySwapchainKHR(s_device, s_swapchain, nullptr);
            s_swapchain = VK_NULL_HANDLE;
            s_swapImages.clear();
        }

        bool CreateSwapchain()
        {
#if defined(__APPLE__) && TARGET_OS_IOS
            // The present scale: the view's bounds may have changed since its
            // scale was snapped, or the scale itself (before the surface's
            // extent is read).
            if ((s_presentScale < 1.0f || s_viewScaled) && platform::ios::SetDrawableScale(s_window, s_presentScale))
                s_viewScaled = true;
#endif
            VkSurfaceCapabilitiesKHR caps;
            vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s_physical, s_surface, &caps);
            const VkExtent2D previous = s_swapExtent;
            // A landscape-only iPhone takes no portrait swapchain under a
            // landscape interface (swapchain_policy.h): the same size turned,
            // while the view is fitted back to its window.
            const Extent surface = SurfaceExtent(caps);
            const ExtentChoice choice = ChooseExtent(surface, InterfaceNow(), LandscapeOnlyHere());
            s_swapExtent = { choice.extent.width, choice.extent.height };
            if (s_swapExtent.width == 0 || s_swapExtent.height == 0)
                return false;  // minimised
            if (choice.refused)
                fprintf(stderr, "[video] the surface is %ux%u, portrait, under a landscape interface: the swapchain is %ux%u instead\n",
                    surface.width, surface.height, s_swapExtent.width, s_swapExtent.height);
            s_regionExtent.store(uint64_t(s_swapExtent.width) << 32 | s_swapExtent.height, std::memory_order_relaxed);
            // The UI lays out at and draws to the swapchain's size (the
            // drawables' with iOS's present scale), not the window's pixels.
            ui::SetFramebufferSize(s_swapExtent.width, s_swapExtent.height);

            uint32_t count = 0;
            vkGetPhysicalDeviceSurfaceFormatsKHR(s_physical, s_surface, &count, nullptr);
            std::vector<VkSurfaceFormatKHR> formats(count);
            vkGetPhysicalDeviceSurfaceFormatsKHR(s_physical, s_surface, &count, formats.data());
            // B8G8R8A8_UNORM in plain sRGB: the picture is sRGB-encoded.
            // MoltenVK lists each format in every color space it knows (asked
            // for VK_EXT_swapchain_colorspace or not), and the last
            // B8G8R8A8_UNORM entry is Display P3 *linear*: the layer showed
            // the picture's values as linear light, washed out and pale
            // (captures of the swapchain looked right: its values were).
            auto pick = [&](VkFormat format) {
                return std::find_if(formats.begin(), formats.end(), [&](const VkSurfaceFormatKHR& f) {
                    return (format == VK_FORMAT_UNDEFINED || f.format == format) && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
                });
            };
            auto chosen = pick(VK_FORMAT_B8G8R8A8_UNORM);
            if (chosen == formats.end())
                chosen = pick(VK_FORMAT_UNDEFINED);
            if (chosen == formats.end())
                chosen = formats.begin();
            s_swapFormat = chosen->format;
            VkColorSpaceKHR colorSpace = chosen->colorSpace;
            static bool formatLogged = false;
            if (!std::exchange(formatLogged, true))
                fprintf(stderr, "[video] swapchain format %d, color space %d (%zu offered)\n", int(s_swapFormat), int(colorSpace), formats.size());

            VkSwapchainCreateInfoKHR info{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
            info.surface = s_surface;
#ifdef __APPLE__
            // MoltenVK takes the CAMetalLayer drawable inside vkQueuePresentKHR,
            // which holds the queue mutex: with 2 drawables (its minimum) that
            // waited for the display to free one, and the command processor
            // waited behind it (on an iPhone 86% of its time, "queue lock" in
            // NFSMW_LOG_LATENCY). Linux drivers ask for 3 themselves.
            info.minImageCount = std::max(caps.minImageCount, 3u);
#else
            info.minImageCount = std::max(caps.minImageCount, 2u);
#endif
            if (caps.maxImageCount)
                info.minImageCount = std::min(info.minImageCount, caps.maxImageCount);
            info.imageFormat = s_swapFormat;
            info.imageColorSpace = colorSpace;
            info.imageExtent = s_swapExtent;
            info.imageArrayLayers = 1;
            info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);  // NFSMW_CHECK_PRESENT_SEC captures
            s_swapReadable = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
            info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
            info.preTransform = caps.currentTransform;
            info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
            // V-Sync off: mailbox (no tearing, newest frame) where there is
            // one, else immediate.
            info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
            if (!s_vsync)
            {
                uint32_t modeCount = 0;
                vkGetPhysicalDeviceSurfacePresentModesKHR(s_physical, s_surface, &modeCount, nullptr);
                std::vector<VkPresentModeKHR> modes(modeCount);
                vkGetPhysicalDeviceSurfacePresentModesKHR(s_physical, s_surface, &modeCount, modes.data());
                for (VkPresentModeKHR wanted : { VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR })
                    if (std::find(modes.begin(), modes.end(), wanted) != modes.end())
                    {
                        info.presentMode = wanted;
                        break;
                    }
            }
            info.clipped = VK_TRUE;
            if (!Check(vkCreateSwapchainKHR(s_device, &info, nullptr, &s_swapchain), "vkCreateSwapchainKHR"))
                return false;
            {
                // Once per change: the V-Sync setting's effect, in the log.
                static int logged = -1;
                if (logged != int(info.presentMode) * 2 + s_vsync)
                {
                    logged = int(info.presentMode) * 2 + s_vsync;
                    const char* mode = info.presentMode == VK_PRESENT_MODE_FIFO_KHR ? "FIFO"
                        : info.presentMode == VK_PRESENT_MODE_MAILBOX_KHR           ? "MAILBOX"
                                                                                    : "IMMEDIATE";
                    fprintf(stderr, "[video] present mode %s (V-Sync %s%s)\n", mode, s_vsync ? "on" : "off",
                        !s_vsync && info.presentMode == VK_PRESENT_MODE_FIFO_KHR ? "; the surface offers nothing else" : "");
                }
            }
            s_swapchainGeneration = s_swapchainUse.Publish(s_swapchain);
            if (s_uiReady)
                ui::SetFormat(s_swapFormat);
            s_swapPolicy.Created({ s_swapExtent.width, s_swapExtent.height });
            static VkExtent2D logged{};
            const char* reason = std::exchange(s_swapReason, nullptr);
            if (logged.width != s_swapExtent.width || logged.height != s_swapExtent.height)
            {
                if (logged.width)  // the first one is logged with the device
                {
                    fprintf(stderr, "[video] swapchain %ux%u (was %ux%u%s%s)%s\n", s_swapExtent.width, s_swapExtent.height, logged.width,
                        logged.height, reason ? "; " : "", reason ? reason : "", GeometryNote().c_str());
                    UpdateDisplayItem();
                }
                logged = s_swapExtent;
            }
            else if (reason && previous.width && s_sameSizeLogged < 10)
            {
                s_sameSizeLogged++;
                fprintf(stderr, "[video] swapchain %ux%u again (%s)%s\n", s_swapExtent.width, s_swapExtent.height, reason,
                    s_sameSizeLogged == 10 ? "; no more same-size rebuilds logged" : "");
            }

            vkGetSwapchainImagesKHR(s_device, s_swapchain, &count, nullptr);
            s_swapImages.resize(count);
            vkGetSwapchainImagesKHR(s_device, s_swapchain, &count, s_swapImages.data());
#ifdef _WIN32
            {
                // The present mode with what else the surface offers, and
                // the images the driver made (once per change; the system
                // summary's "Present").
                uint32_t modeCount = 0;
                vkGetPhysicalDeviceSurfacePresentModesKHR(s_physical, s_surface, &modeCount, nullptr);
                std::vector<VkPresentModeKHR> modes(modeCount);
                vkGetPhysicalDeviceSurfacePresentModesKHR(s_physical, s_surface, &modeCount, modes.data());
                std::string offered;
                for (VkPresentModeKHR m : modes)
                    offered += std::format("{}{}", offered.empty() ? "" : ", ",
                        m == VK_PRESENT_MODE_IMMEDIATE_KHR ? "IMMEDIATE" : m == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX"
                        : m == VK_PRESENT_MODE_FIFO_KHR ? "FIFO" : m == VK_PRESENT_MODE_FIFO_RELAXED_KHR ? "FIFO_RELAXED"
                        : std::to_string(int(m)));
                std::string text = std::format("{} (V-Sync {}), {} images (the surface offers {}, and {} images)",
                    info.presentMode == VK_PRESENT_MODE_FIFO_KHR ? "FIFO" : info.presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX"
                        : "IMMEDIATE", s_vsync ? "on" : "off", count, offered,
                    caps.maxImageCount ? std::format("{} to {}", caps.minImageCount, caps.maxImageCount)
                                       : std::format("{} or more", caps.minImageCount));
                if (text != s_presentText)
                {
                    s_presentText = text;
                    fprintf(stderr, "[video] swapchain: %s\n", text.c_str());
                    if (s_contextReady)
                        report::SetSystemItem("Present", s_presentText + "; " + s_timingText);
                }
                static bool first = true;
                NoteDisplay(std::exchange(first, false) ? "seen at start" : "seen as the swapchain was rebuilt");
            }
#endif
            s_renderDone.resize(count);
            VkSemaphoreCreateInfo si{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            for (auto& s : s_renderDone)
                vkCreateSemaphore(s_device, &si, nullptr, &s);
            s_swapViews.resize(count);
            for (uint32_t i = 0; i < count; i++)
            {
                VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
                vci.image = s_swapImages[i];
                vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
                vci.format = s_swapFormat;
                vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                vkCreateImageView(s_device, &vci, nullptr, &s_swapViews[i]);
            }
            return true;
        }

        bool EnsureSource(uint32_t width, uint32_t height)
        {
            if (s_source && s_sourceWidth == width && s_sourceHeight == height)
                return true;
            WaitIdle();
            if (s_source)
            {
                vkDestroyImage(s_device, s_source, nullptr);
                vkFreeMemory(s_device, s_sourceMemory, nullptr);
            }
            VkImageCreateInfo ii{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
            ii.imageType = VK_IMAGE_TYPE_2D;
            ii.format = VK_FORMAT_B8G8R8A8_UNORM;
            ii.extent = { width, height, 1 };
            ii.mipLevels = 1;
            ii.arrayLayers = 1;
            ii.samples = VK_SAMPLE_COUNT_1_BIT;
            ii.tiling = VK_IMAGE_TILING_OPTIMAL;
            ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (!Check(vkCreateImage(s_device, &ii, nullptr, &s_source), "vkCreateImage"))
                return false;
            VkMemoryRequirements req;
            vkGetImageMemoryRequirements(s_device, s_source, &req);
            VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            ai.allocationSize = req.size;
            ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            vkAllocateMemory(s_device, &ai, nullptr, &s_sourceMemory);
            vkBindImageMemory(s_device, s_source, s_sourceMemory, 0);
            s_sourceWidth = width;
            s_sourceHeight = height;
            return true;
        }

        bool EnsureStaging(Frame& f, VkDeviceSize size)
        {
            if (f.staging && f.stagingSize >= size)
                return true;
            if (f.staging)
            {
                vkDestroyBuffer(s_device, f.staging, nullptr);
                vkFreeMemory(s_device, f.stagingMemory, nullptr);
            }
            VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bi.size = size;
            bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            vkCreateBuffer(s_device, &bi, nullptr, &f.staging);
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(s_device, f.staging, &req);
            VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            ai.allocationSize = req.size;
            ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            vkAllocateMemory(s_device, &ai, nullptr, &f.stagingMemory);
            vkBindBufferMemory(s_device, f.staging, f.stagingMemory, 0);
            vkMapMemory(s_device, f.stagingMemory, 0, size, 0, &f.stagingMapped);
            f.stagingSize = size;
            return true;
        }

        void Barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
            VkAccessFlags srcAccess, VkAccessFlags dstAccess, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
        {
            VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            b.oldLayout = from;
            b.newLayout = to;
            b.srcAccessMask = srcAccess;
            b.dstAccessMask = dstAccess;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = image;
            b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
        }

        // A copy of part of a swapchain image on its way to the host.
        struct RegionCapture
        {
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            uint32_t width = 0, height = 0;
        };

        // Records a copy of `rect` of `image` (in `layout`, last written at
        // `stage` with `access`) into a host-visible buffer, and puts the
        // image back in `layout` for the writes that follow.
        RegionCapture RecordRegionCapture(VkCommandBuffer cmd, VkImage image, VkImageLayout layout, VkAccessFlags access,
            VkPipelineStageFlags stage, VkRect2D rect)
        {
            RegionCapture c;
            c.width = rect.extent.width;
            c.height = rect.extent.height;
            VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bci.size = VkDeviceSize(c.width) * c.height * 4;
            bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            if (bci.size == 0 || vkCreateBuffer(s_device, &bci, nullptr, &c.buffer) != VK_SUCCESS)
                return {};
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(s_device, c.buffer, &req);
            VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            ai.allocationSize = req.size;
            ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (vkAllocateMemory(s_device, &ai, nullptr, &c.memory) != VK_SUCCESS)
            {
                vkDestroyBuffer(s_device, c.buffer, nullptr);
                return {};
            }
            vkBindBufferMemory(s_device, c.buffer, c.memory, 0);
            Barrier(cmd, image, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, access, VK_ACCESS_TRANSFER_READ_BIT, stage,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy region{};
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.imageOffset = { rect.offset.x, rect.offset.y, 0 };
            region.imageExtent = { c.width, c.height, 1 };
            vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c.buffer, 1, &region);
            VkBufferMemoryBarrier hostRead{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
            hostRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            hostRead.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            hostRead.srcQueueFamilyIndex = hostRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            hostRead.buffer = c.buffer;
            hostRead.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hostRead, 0, nullptr);
            Barrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout, VK_ACCESS_TRANSFER_READ_BIT, access,
                VK_PIPELINE_STAGE_TRANSFER_BIT, stage);
            return c;
        }

        // After the frame's fence: the pixels as RGB, then the buffer freed.
        CapturedFrame FinishRegionCapture(RegionCapture& c)
        {
            CapturedFrame frame;
            const bool bgr = s_swapFormat == VK_FORMAT_B8G8R8A8_UNORM || s_swapFormat == VK_FORMAT_B8G8R8A8_SRGB;
            const bool rgb = s_swapFormat == VK_FORMAT_R8G8B8A8_UNORM || s_swapFormat == VK_FORMAT_R8G8B8A8_SRGB;
            void* mapped = nullptr;
            if ((bgr || rgb) && vkMapMemory(s_device, c.memory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS)
            {
                const uint8_t* px = static_cast<const uint8_t*>(mapped);
                frame.width = c.width;
                frame.height = c.height;
                frame.rgb.resize(size_t(c.width) * c.height * 3);
                for (size_t i = 0, n = size_t(c.width) * c.height; i < n; i++)
                    for (int k = 0; k < 3; k++)
                        frame.rgb[i * 3 + k] = px[i * 4 + (bgr ? 2 - k : k)];
                vkUnmapMemory(s_device, c.memory);
            }
            vkDestroyBuffer(s_device, c.buffer, nullptr);
            vkFreeMemory(s_device, c.memory, nullptr);
            c = {};
            return frame;
        }

        // ---------------------------------------------------------------
        // Post-processing, when presenting: anti-aliasing at the game's
        // resolution (compute, into s_aa), then scaling into the letterboxed
        // part of the window (fragment). The 360 drew NFSMW with 2x MSAA; the
        // renderer draws single-sampled, so edges get an FXAA-style pass
        // (edge-directed, with a sub-pixel blend). Scaling is Catmull-Rom
        // bicubic with optional sharpening, clamped to the 2x2 source texels
        // around each sample so it can't ring.
        // NFSMW_AA=0 turns the anti-aliasing off; NFSMW_SCALING=linear (or
        // bicubic, the default); NFSMW_SHARPEN=<0..1> (default 0.3). If the
        // pipelines can't be built, frames are blitted as before.
        const char* kFullscreenVs = R"(#version 460
layout(location = 0) out vec2 uv;
void main()
{
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

        const char* kAaCs = R"(#version 460
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1, rgba8) uniform writeonly image2D dst;
// band: the 16:9 band's columns x..y and rows z..w (video::BandOf), used
// when bandOn is 1 (video/band_filter.h).
layout(push_constant) uniform P { ivec2 size; float subpixel; uint bandOn; ivec4 band; } p;

const float kEdgeMin = 0.0312;      // no AA on darker contrast than this
const float kEdgeRelative = 0.125;  // ... or than this fraction of the local maximum

// This pixel's span of the band's three along each axis, as uv: every fetch
// stays between its first and last texel centres (band_filter.h's Clamp).
vec2 spanLo = vec2(0.0), spanHi = vec2(1.0);

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
vec3 at(vec2 uv)
{
    if (p.bandOn != 0u)
        uv = clamp(uv, spanLo, spanHi);
    return textureLod(src, uv, 0.0).rgb;
}

// band_filter.h's SpanOf: [0, lo), [lo, hi) or [hi, size) holding t.
vec2 spanOf(float t, int lo, int hi, int size)
{
    return t < float(lo) ? vec2(0.0, float(lo)) : t < float(hi) ? vec2(float(lo), float(hi)) : vec2(float(hi), float(size));
}

void main()
{
    ivec2 pix = ivec2(gl_GlobalInvocationID.xy);
    if (pix.x >= p.size.x || pix.y >= p.size.y)
        return;
    vec2 texel = 1.0 / vec2(p.size);
    vec2 uv = (vec2(pix) + 0.5) * texel;
    if (p.bandOn != 0u)
    {
        vec2 sx = spanOf(float(pix.x) + 0.5, p.band.x, p.band.y, p.size.x), sy = spanOf(float(pix.y) + 0.5, p.band.z, p.band.w, p.size.y);
        spanLo = (vec2(sx.x, sy.x) + 0.5) * texel;
        spanHi = (vec2(sx.y, sy.y) - 0.5) * texel;
    }
    vec3 center = at(uv);
    float lM = luma(center);
    float lN = luma(at(uv + vec2(0.0, -texel.y))), lS = luma(at(uv + vec2(0.0, texel.y)));
    float lW = luma(at(uv + vec2(-texel.x, 0.0))), lE = luma(at(uv + vec2(texel.x, 0.0)));
    float lMin = min(lM, min(min(lN, lS), min(lW, lE)));
    float lMax = max(lM, max(max(lN, lS), max(lW, lE)));
    float range = lMax - lMin;
    if (range < max(kEdgeMin, lMax * kEdgeRelative))
    {
        imageStore(dst, pix, vec4(center, 1.0));
        return;
    }
    float lNW = luma(at(uv - texel)), lSE = luma(at(uv + texel));
    float lNE = luma(at(uv + vec2(texel.x, -texel.y))), lSW = luma(at(uv + vec2(-texel.x, texel.y)));

    // The edge's orientation: where luma changes most (across rows for a
    // horizontal edge).
    float acrossRows = abs(lNW + lSW - 2.0 * lW) + 2.0 * abs(lN + lS - 2.0 * lM) + abs(lNE + lSE - 2.0 * lE);
    float acrossColumns = abs(lNW + lNE - 2.0 * lN) + 2.0 * abs(lW + lE - 2.0 * lM) + abs(lSW + lSE - 2.0 * lS);
    bool horizontal = acrossRows >= acrossColumns;

    // Which side of this pixel the edge is on: the steeper neighbour.
    float lBefore = horizontal ? lN : lW, lAfter = horizontal ? lS : lE;
    float gBefore = abs(lBefore - lM), gAfter = abs(lAfter - lM);
    bool before = gBefore >= gAfter;
    float threshold = 0.25 * max(gBefore, gAfter);
    float step = horizontal ? texel.y : texel.x;
    float edgeLuma = 0.5 * ((before ? lBefore : lAfter) + lM);
    if (before)
        step = -step;

    // Walk along the edge both ways until the luma leaves the edge's.
    vec2 onEdge = uv;
    if (horizontal) onEdge.y += 0.5 * step; else onEdge.x += 0.5 * step;
    vec2 along = horizontal ? vec2(texel.x, 0.0) : vec2(0.0, texel.y);
    vec2 uvA = onEdge - along, uvB = onEdge + along;
    float dA = luma(at(uvA)) - edgeLuma, dB = luma(at(uvB)) - edgeLuma;
    bool endA = abs(dA) >= threshold, endB = abs(dB) >= threshold;
    const float strides[10] = float[](1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0);
    for (int i = 0; i < 10 && !(endA && endB); i++)
    {
        if (!endA) { uvA -= along * strides[i]; dA = luma(at(uvA)) - edgeLuma; endA = abs(dA) >= threshold; }
        if (!endB) { uvB += along * strides[i]; dB = luma(at(uvB)) - edgeLuma; endB = abs(dB) >= threshold; }
    }
    float distA = horizontal ? uv.x - uvA.x : uv.y - uvA.y;
    float distB = horizontal ? uvB.x - uv.x : uvB.y - uv.y;
    bool nearerA = distA < distB;
    float offset = 0.5 - min(distA, distB) / (distA + distB);
    // Only blend from the end whose luma variation goes the way this
    // pixel's does (otherwise this pixel is on the edge's far side).
    if (((nearerA ? dA : dB) < 0.0) == (lM < edgeLuma))
        offset = 0.0;

    // Sub-pixel aliasing (thin lines, single-pixel features).
    float average = (2.0 * (lN + lS + lW + lE) + lNW + lNE + lSW + lSE) / 12.0;
    float sub = clamp(abs(average - lM) / range, 0.0, 1.0);
    sub = (3.0 - 2.0 * sub) * sub * sub;
    offset = max(offset, sub * sub * p.subpixel);

    vec2 sampleUv = uv;
    if (horizontal) sampleUv.y += offset * step; else sampleUv.x += offset * step;
    imageStore(dst, pix, vec4(at(sampleUv), 1.0));
}
)";

        const char* kScaleFs = R"(#version 460
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(set = 0, binding = 0) uniform sampler2D src;
// band: the 16:9 band's columns x..y and rows z..w in texels (video::BandOf),
// used when bandOn is 1 (video/band_filter.h).
layout(push_constant) uniform P { vec2 size; float sharpen; uint bicubic; vec4 band; uint bandOn; } p;

// The span of the band's three along each axis that this pixel's centre
// falls in, as uv: every tap stays between its first and last texel centres
// (band_filter.h's Clamp), so a strip's pixel reads only the strip.
vec2 spanLo = vec2(0.0), spanHi = vec2(1.0);

vec3 at(vec2 t)
{
    if (p.bandOn != 0u)
        t = clamp(t, spanLo, spanHi);
    return textureLod(src, t, 0.0).rgb;
}

// band_filter.h's SpanOf: [0, lo), [lo, hi) or [hi, size) holding t.
vec2 spanOf(float t, float lo, float hi, float size)
{
    return t < lo ? vec2(0.0, lo) : t < hi ? vec2(lo, hi) : vec2(hi, size);
}

void setSpan()
{
    if (p.bandOn == 0u)
        return;
    vec2 pos = uv * p.size;
    vec2 sx = spanOf(pos.x, p.band.x, p.band.y, p.size.x), sy = spanOf(pos.y, p.band.z, p.band.w, p.size.y);
    spanLo = (vec2(sx.x, sy.x) + 0.5) / p.size;
    spanHi = (vec2(sx.y, sy.y) - 0.5) / p.size;
}

// Catmull-Rom bicubic from 9 bilinear taps: the middle two weights of each
// axis are folded into one bilinear fetch.
vec3 catmullRom(vec2 t, vec2 texel)
{
    vec2 pos = t * p.size;
    vec2 c = floor(pos - 0.5) + 0.5;
    vec2 f = pos - c;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;
    vec2 t0 = (c - 1.0) * texel, t12 = (c + w2 / w12) * texel, t3 = (c + 2.0) * texel;
    return at(vec2(t0.x, t0.y)) * w0.x * w0.y + at(vec2(t12.x, t0.y)) * w12.x * w0.y + at(vec2(t3.x, t0.y)) * w3.x * w0.y +
        at(vec2(t0.x, t12.y)) * w0.x * w12.y + at(vec2(t12.x, t12.y)) * w12.x * w12.y + at(vec2(t3.x, t12.y)) * w3.x * w12.y +
        at(vec2(t0.x, t3.y)) * w0.x * w3.y + at(vec2(t12.x, t3.y)) * w12.x * w3.y + at(vec2(t3.x, t3.y)) * w3.x * w3.y;
}

void main()
{
    setSpan();
    if (p.bicubic == 0u)
    {
        color = vec4(at(uv), 1.0);
        return;
    }
    vec2 texel = 1.0 / p.size;
    vec3 c = catmullRom(uv, texel);
    // Sharpen against the bilinear sample, then clamp to the 2x2 texels
    // around the sample: detail gets crisper without halos or ringing.
    vec2 corner = (floor(uv * p.size - 0.5) + 0.5) * texel;
    vec3 a = at(corner), b = at(corner + vec2(texel.x, 0.0)), d = at(corner + vec2(0.0, texel.y)), e = at(corner + texel);
    vec3 lo = min(min(a, b), min(d, e)), hi = max(max(a, b), max(d, e));
    c = clamp(c + p.sharpen * (c - at(uv)), lo, hi);
    color = vec4(c, 1.0);
}
)";

        // NFSMW_FAST_SCALE=1 (default on Apple, below; for phones, where this pass at
        // the screen's size is most of the post-processing): the same
        // scaling from 7 fetches a pixel instead of 14 (above). The 2x2
        // texels around the sample come from one gather per channel, and the
        // bilinear sample sharpened against and Catmull-Rom's middle tap are
        // blends of those 4; the 4 corner taps (at most 1.6% of the weight,
        // 1.5 texels away diagonally) are left out and the rest renormalised.
        // The clamp is the same. (A real frame: at most 3 levels apart.)
        const char* kScaleFastFs = R"(#version 460
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(set = 0, binding = 0) uniform sampler2D src;
// band, bandOn: as kScaleFs's.
layout(push_constant) uniform P { vec2 size; float sharpen; uint bicubic; vec4 band; uint bandOn; } p;

vec2 spanLo = vec2(0.0), spanHi = vec2(1.0);  // as kScaleFs's

vec3 at(vec2 t)
{
    if (p.bandOn != 0u)
        t = clamp(t, spanLo, spanHi);
    return textureLod(src, t, 0.0).rgb;
}

vec2 spanOf(float t, float lo, float hi, float size)
{
    return t < lo ? vec2(0.0, lo) : t < hi ? vec2(lo, hi) : vec2(hi, size);
}

void main()
{
    vec2 sx = vec2(0.0), sy = vec2(0.0);
    if (p.bandOn != 0u)
    {
        vec2 pos = uv * p.size;
        sx = spanOf(pos.x, p.band.x, p.band.y, p.size.x);
        sy = spanOf(pos.y, p.band.z, p.band.w, p.size.y);
        spanLo = (vec2(sx.x, sy.x) + 0.5) / p.size;
        spanHi = (vec2(sx.y, sy.y) - 0.5) / p.size;
    }
    if (p.bicubic == 0u)
    {
        color = vec4(at(uv), 1.0);
        return;
    }
    vec2 texel = 1.0 / p.size;
    vec2 pos = uv * p.size;
    vec2 c = floor(pos - 0.5) + 0.5;
    vec2 f = pos - c;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;
    vec2 g = w2 / w12;
    vec2 t0 = (c - 1.0) * texel, t12 = (c + g) * texel, t3 = (c + 2.0) * texel;
    // Gathers return (x0,y1), (x1,y1), (x1,y0), (x0,y0).
    vec2 mid = (c + 0.5) * texel;
    vec4 r = textureGather(src, mid, 0), gr = textureGather(src, mid, 1), b = textureGather(src, mid, 2);
    vec3 a = vec3(r.w, gr.w, b.w), e = vec3(r.z, gr.z, b.z), d = vec3(r.x, gr.x, b.x), h = vec3(r.y, gr.y, b.y);
    if (p.bandOn != 0u)
    {
        // The gather's row or column past this pixel's span reads the
        // span's edge one instead (band_filter.h's ClampTexel): rows a e
        // (first) and d h, columns a d (first) and e h.
        vec2 first = c - 0.5;
        if (first.y < sy.x) { a = d; e = h; } else if (first.y + 1.0 >= sy.y) { d = a; h = e; }
        if (first.x < sx.x) { a = e; d = h; } else if (first.x + 1.0 >= sx.y) { e = a; h = d; }
    }
    vec3 middle = mix(mix(a, e, g.x), mix(d, h, g.x), g.y);
    vec3 bilinear = mix(mix(a, e, f.x), mix(d, h, f.x), f.y);
    float wN = w12.x * w0.y, wW = w0.x * w12.y, wM = w12.x * w12.y, wE = w3.x * w12.y, wS = w12.x * w3.y;
    vec3 cr = (at(vec2(t12.x, t0.y)) * wN + at(vec2(t0.x, t12.y)) * wW + middle * wM + at(vec2(t3.x, t12.y)) * wE +
        at(vec2(t12.x, t3.y)) * wS) / (wN + wW + wM + wE + wS);
    vec3 lo = min(min(a, e), min(d, h)), hi = max(max(a, e), max(d, h));
    color = vec4(clamp(cr + p.sharpen * (cr - bilinear), lo, hi), 1.0);
}
)";
        // NFSMW_FAST_SCALE: on by default on Apple (iPad: scale pass 1.26 ->
        // 1.07 ms, the same picture within 3 levels) and arm64 Linux (Steam
        // Frame: post-processing 0.47 -> 0.39 ms), off elsewhere until
        // measured there; =0/=1 overrides.
#if defined(__APPLE__) || (defined(__linux__) && defined(__aarch64__))
        constexpr bool kFastScaleByDefault = true;
#else
        constexpr bool kFastScaleByDefault = false;
#endif
        const bool s_fastScale = [] { const char* v = std::getenv("NFSMW_FAST_SCALE"); return v ? v[0] == '1' : kFastScaleByDefault; }();

        // Brightness and the 360's display gamma (video/display_tone.h): the
        // scale shader in use, its main renamed scaleMain, then this main,
        // which looks each channel of its result up in the tone table (256
        // entries for the values k/255, linear between them). The tables
        // are made on the CPU. With Brightness 0 and the 360's gamma off the
        // plain scale pipeline draws, so that picture (v0.1.3's) is the same,
        // bit for bit, as without this.
        const char* kToneGlsl = R"(
layout(set = 0, binding = 1) uniform sampler2D toneTable;

vec3 toneOf(vec3 c)
{
    vec3 t = clamp(c, 0.0, 1.0) * (255.0 / 256.0) + (0.5 / 256.0);
    return vec3(textureLod(toneTable, vec2(t.r, 0.5), 0.0).r, textureLod(toneTable, vec2(t.g, 0.5), 0.0).g,
        textureLod(toneTable, vec2(t.b, 0.5), 0.0).b);
}

void main()
{
    scaleMain();
    color = vec4(toneOf(color.rgb), color.a);
}
)";

        // The tone variant of a scale shader's source ("" if it hasn't
        // exactly one main to rename).
        std::string ToneVariant(const char* scaleGlsl)
        {
            std::string glsl = scaleGlsl;
            const std::string_view kMain = "void main()";
            size_t at = glsl.find(kMain);
            if (at == std::string::npos || glsl.find(kMain, at + 1) != std::string::npos)
                return {};
            glsl.replace(at, kMain.size(), "void scaleMain()");
            return glsl + kToneGlsl;
        }

        struct ToneState
        {
            bool tried = false, ready = false;
            VkDescriptorSetLayout set = VK_NULL_HANDLE;
            VkPipelineLayout layout = VK_NULL_HANDLE;
            VkShaderModule fs = VK_NULL_HANDLE;
            VkPipeline pipeline = VK_NULL_HANDLE;
            VkFormat format = VK_FORMAT_UNDEFINED;  // the swapchain format the pipeline was made for
            // The table: 256 x 1 RGBA16F, written from a host-visible buffer.
            VkImage image = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;
            VkBuffer staging = VK_NULL_HANDLE;
            VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
            void* stagingMapped = nullptr;
            // What the table holds.
            bool filled = false;
            int32_t brightness = 0;
            bool ramp = false;
            uint64_t rampGeneration = 0;
        } s_tone;

        struct PostState
        {
            bool tried = false, ready = false;
            VkSampler sampler = VK_NULL_HANDLE;
            VkDescriptorSetLayout aaSet = VK_NULL_HANDLE, scaleSet = VK_NULL_HANDLE;
            VkPipelineLayout aaLayout = VK_NULL_HANDLE, scaleLayout = VK_NULL_HANDLE;
            VkPipeline aa = VK_NULL_HANDLE, scale = VK_NULL_HANDLE;
            VkFormat scaleFormat = VK_FORMAT_UNDEFINED;
            VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
            PFN_vkCmdPushDescriptorSetKHR pushDescriptorSet = nullptr;
            // The anti-aliased frame, at the game's resolution.
            VkImage aaImage = VK_NULL_HANDLE;
            VkDeviceMemory aaMemory = VK_NULL_HANDLE;
            VkImageView aaView = VK_NULL_HANDLE;
            uint32_t aaWidth = 0, aaHeight = 0;
        } s_post;
        // Read every frame: the settings menu changes them live.
        bool AaEnabled() { return settings::GetBool(settings::Id::AntiAliasing); }
        bool Bicubic() { return settings::GetInt(settings::Id::Scaling) == int32_t(settings::ScalingMode::Bicubic); }
        float Sharpen() { return std::clamp(settings::GetFloat(settings::Id::Sharpening), 0.0f, 1.0f); }

        VkShaderModule PostModule(const char* glsl, int stage)
        {
            std::string log;
            std::vector<uint32_t> spirv = gpu::renderer::CompileShader(glsl, stage, log);
            if (spirv.empty())
            {
                fprintf(stderr, "[video] post-processing shader failed to compile:\n%s\n", log.c_str());
                return VK_NULL_HANDLE;
            }
            VkShaderModuleCreateInfo smci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
            smci.codeSize = spirv.size() * 4;
            smci.pCode = spirv.data();
            VkShaderModule module = VK_NULL_HANDLE;
            vkCreateShaderModule(s_device, &smci, nullptr, &module);
            return module;
        }

        // A scale pipeline (the plain one or the tone variant) for the
        // swapchain's format; VK_NULL_HANDLE if it can't be made.
        VkPipeline MakeScalePipeline(VkShaderModule fs, VkPipelineLayout layout)
        {
            VkPipelineShaderStageCreateInfo stages[2] = {
                { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, s_post.vs, "main", nullptr },
                { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr },
            };
            VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
            VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
            ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
            vp.viewportCount = 1;
            vp.scissorCount = 1;
            VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
            rs.polygonMode = VK_POLYGON_MODE_FILL;
            rs.cullMode = VK_CULL_MODE_NONE;
            rs.lineWidth = 1.0f;
            VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
            ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineColorBlendAttachmentState att{};
            att.colorWriteMask = 0xF;
            VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
            cb.attachmentCount = 1;
            cb.pAttachments = &att;
            VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
            VkPipelineDynamicStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
            ds.dynamicStateCount = 2;
            ds.pDynamicStates = dyn;
            VkPipelineRenderingCreateInfoKHR rci{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR };
            rci.colorAttachmentCount = 1;
            rci.pColorAttachmentFormats = &s_swapFormat;
            VkGraphicsPipelineCreateInfo gpci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
            gpci.pNext = &rci;
            gpci.stageCount = 2;
            gpci.pStages = stages;
            gpci.pVertexInputState = &vi;
            gpci.pInputAssemblyState = &ia;
            gpci.pViewportState = &vp;
            gpci.pRasterizationState = &rs;
            gpci.pMultisampleState = &ms;
            gpci.pColorBlendState = &cb;
            gpci.pDynamicState = &ds;
            gpci.layout = layout;
            VkPipeline pipeline = VK_NULL_HANDLE;
            if (vkCreateGraphicsPipelines(s_device, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline) != VK_SUCCESS)
                return VK_NULL_HANDLE;
            return pipeline;
        }

        // The scale pipeline for the swapchain's format (made again if it changes).
        bool EnsureScalePipeline()
        {
            if (s_post.scale && s_post.scaleFormat == s_swapFormat)
                return true;
            if (s_post.scale)
                vkDestroyPipeline(s_device, s_post.scale, nullptr);
            s_post.scale = MakeScalePipeline(s_post.fs, s_post.scaleLayout);
            if (!s_post.scale)
                return false;
            s_post.scaleFormat = s_swapFormat;
            return true;
        }

        bool EnsurePostProcessing()
        {
            if (s_post.tried)
                return s_post.ready && EnsureScalePipeline();
            s_post.tried = true;
            s_post.pushDescriptorSet = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(
                vkGetDeviceProcAddr(s_device, "vkCmdPushDescriptorSetKHR"));
            if (!s_post.pushDescriptorSet || !s_context.cmdBeginRendering)
                return false;
            VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
            sci.magFilter = VK_FILTER_LINEAR;
            sci.minFilter = VK_FILTER_LINEAR;
            sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            vkCreateSampler(s_device, &sci, nullptr, &s_post.sampler);

            VkDescriptorSetLayoutBinding aaBindings[2] = {
                { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            };
            VkDescriptorSetLayoutBinding scaleBinding = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
            VkDescriptorSetLayoutCreateInfo dslci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            dslci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
            dslci.bindingCount = 2;
            dslci.pBindings = aaBindings;
            vkCreateDescriptorSetLayout(s_device, &dslci, nullptr, &s_post.aaSet);
            dslci.bindingCount = 1;
            dslci.pBindings = &scaleBinding;
            vkCreateDescriptorSetLayout(s_device, &dslci, nullptr, &s_post.scaleSet);
            VkPushConstantRange aaRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, 32 }, scaleRange{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, 36 };
            VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            plci.setLayoutCount = 1;
            plci.pSetLayouts = &s_post.aaSet;
            plci.pushConstantRangeCount = 1;
            plci.pPushConstantRanges = &aaRange;
            vkCreatePipelineLayout(s_device, &plci, nullptr, &s_post.aaLayout);
            plci.pSetLayouts = &s_post.scaleSet;
            plci.pPushConstantRanges = &scaleRange;
            vkCreatePipelineLayout(s_device, &plci, nullptr, &s_post.scaleLayout);

            VkShaderModule cs = PostModule(kAaCs, 2);
            s_post.vs = PostModule(kFullscreenVs, 0);
            s_post.fs = PostModule(s_fastScale ? kScaleFastFs : kScaleFs, 1);
            if (!cs || !s_post.vs || !s_post.fs)
                return false;
            VkComputePipelineCreateInfo cpci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
            cpci.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs, "main", nullptr };
            cpci.layout = s_post.aaLayout;
            if (vkCreateComputePipelines(s_device, VK_NULL_HANDLE, 1, &cpci, nullptr, &s_post.aa) != VK_SUCCESS)
                return false;
            if (!EnsureScalePipeline())
                return false;
            s_post.ready = true;
            const int32_t mode = settings::GetInt(settings::Id::Scaling);
            fprintf(stderr, "[video] post-processing: %s, %s scaling%s%s\n", AaEnabled() ? "anti-aliasing" : "no anti-aliasing",
                mode == int32_t(settings::ScalingMode::Fsr1) ? "FSR 1 (enlarging; else bicubic)" : Bicubic() ? "bicubic" : "linear",
                mode != int32_t(settings::ScalingMode::Linear) ? std::format(", sharpen {:.2f}", Sharpen()).c_str() : "",
                s_fastScale ? " (NFSMW_FAST_SCALE: 7 fetches)" : "");
            return true;
        }

        void EnsureAaImage(uint32_t width, uint32_t height)
        {
            if (s_post.aaImage && s_post.aaWidth == width && s_post.aaHeight == height)
                return;
            WaitIdle();
            if (s_post.aaImage)
            {
                vkDestroyImageView(s_device, s_post.aaView, nullptr);
                vkDestroyImage(s_device, s_post.aaImage, nullptr);
                vkFreeMemory(s_device, s_post.aaMemory, nullptr);
            }
            VkImageCreateInfo ii{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
            ii.imageType = VK_IMAGE_TYPE_2D;
            ii.format = VK_FORMAT_R8G8B8A8_UNORM;
            ii.extent = { width, height, 1 };
            ii.mipLevels = 1;
            ii.arrayLayers = 1;
            ii.samples = VK_SAMPLE_COUNT_1_BIT;
            ii.tiling = VK_IMAGE_TILING_OPTIMAL;
            ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            vkCreateImage(s_device, &ii, nullptr, &s_post.aaImage);
            VkMemoryRequirements req;
            vkGetImageMemoryRequirements(s_device, s_post.aaImage, &req);
            VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            ai.allocationSize = req.size;
            ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            vkAllocateMemory(s_device, &ai, nullptr, &s_post.aaMemory);
            vkBindImageMemory(s_device, s_post.aaImage, s_post.aaMemory, 0);
            VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            vci.image = s_post.aaImage;
            vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vci.format = VK_FORMAT_R8G8B8A8_UNORM;
            vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCreateImageView(s_device, &vci, nullptr, &s_post.aaView);
            s_post.aaWidth = width;
            s_post.aaHeight = height;
        }

        // The tone pipeline's pieces, made the first time Brightness or the
        // 360's display gamma is in use (the gamma is on by default, so
        // usually on the first frame).
        bool EnsureTone()
        {
            if (s_tone.tried)
            {
                if (!s_tone.ready)
                    return false;
            }
            else
            {
                s_tone.tried = true;
                std::string glsl = ToneVariant(s_fastScale ? kScaleFastFs : kScaleFs);
                if (glsl.empty())
                {
                    fprintf(stderr, "[video] tone: the scale shader has no single main to wrap; Brightness is off\n");
                    return false;
                }
                VkDescriptorSetLayoutBinding bindings[2] = {
                    { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
                    { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
                };
                VkDescriptorSetLayoutCreateInfo dslci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
                dslci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
                dslci.bindingCount = 2;
                dslci.pBindings = bindings;
                VkPushConstantRange range{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, 36 };  // the scale shaders' P
                VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
                plci.setLayoutCount = 1;
                plci.pSetLayouts = &s_tone.set;
                plci.pushConstantRangeCount = 1;
                plci.pPushConstantRanges = &range;
                if (!Check(vkCreateDescriptorSetLayout(s_device, &dslci, nullptr, &s_tone.set), "tone set layout") ||
                    !Check(vkCreatePipelineLayout(s_device, &plci, nullptr, &s_tone.layout), "tone pipeline layout"))
                    return false;
                s_tone.fs = PostModule(glsl.c_str(), 1);
                if (!s_tone.fs)
                    return false;

                VkImageCreateInfo ii{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
                ii.imageType = VK_IMAGE_TYPE_2D;
                ii.format = VK_FORMAT_R16G16B16A16_SFLOAT;  // sampled with linear filtering on every Vulkan device
                ii.extent = { 256, 1, 1 };
                ii.mipLevels = 1;
                ii.arrayLayers = 1;
                ii.samples = VK_SAMPLE_COUNT_1_BIT;
                ii.tiling = VK_IMAGE_TILING_OPTIMAL;
                ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
                if (!Check(vkCreateImage(s_device, &ii, nullptr, &s_tone.image), "tone table image"))
                    return false;
                VkMemoryRequirements req;
                vkGetImageMemoryRequirements(s_device, s_tone.image, &req);
                VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
                ai.allocationSize = req.size;
                ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                if (ai.memoryTypeIndex == UINT32_MAX || !Check(vkAllocateMemory(s_device, &ai, nullptr, &s_tone.memory), "tone table memory"))
                    return false;
                vkBindImageMemory(s_device, s_tone.image, s_tone.memory, 0);
                VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
                vci.image = s_tone.image;
                vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
                vci.format = VK_FORMAT_R16G16B16A16_SFLOAT;
                vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                if (!Check(vkCreateImageView(s_device, &vci, nullptr, &s_tone.view), "tone table view"))
                    return false;

                VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
                bci.size = 256 * 4 * sizeof(uint16_t);
                bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
                if (!Check(vkCreateBuffer(s_device, &bci, nullptr, &s_tone.staging), "tone staging buffer"))
                    return false;
                vkGetBufferMemoryRequirements(s_device, s_tone.staging, &req);
                ai.allocationSize = req.size;
                ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                if (ai.memoryTypeIndex == UINT32_MAX || !Check(vkAllocateMemory(s_device, &ai, nullptr, &s_tone.stagingMemory), "tone staging memory"))
                    return false;
                vkBindBufferMemory(s_device, s_tone.staging, s_tone.stagingMemory, 0);
                if (!Check(vkMapMemory(s_device, s_tone.stagingMemory, 0, VK_WHOLE_SIZE, 0, &s_tone.stagingMapped), "tone staging map"))
                    return false;
                s_tone.ready = true;
            }
            if (s_tone.pipeline && s_tone.format == s_swapFormat)
                return true;
            if (s_tone.pipeline)
                vkDestroyPipeline(s_device, s_tone.pipeline, nullptr);
            auto start = std::chrono::steady_clock::now();
            s_tone.pipeline = MakeScalePipeline(s_tone.fs, s_tone.layout);
            if (!s_tone.pipeline)
            {
                fprintf(stderr, "[video] tone: the pipeline couldn't be made; Brightness is off\n");
                s_tone.ready = false;
                return false;
            }
            s_tone.format = s_swapFormat;
            fprintf(stderr, "[video] tone: pipeline made in %.0f ms\n",
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            return true;
        }

        // Before the scale pass: whether this frame's picture goes through
        // the tone table (Settings > Display > Brightness, Xbox 360 Display
        // Gamma), with the table brought up to date in `cmd` when its inputs
        // changed. At Brightness 0 with the gamma off it does nothing and
        // returns false.
        bool PrepareTone(VkCommandBuffer cmd)
        {
            const int32_t brightness = std::clamp(settings::GetInt(settings::Id::Brightness), tone::kBrightnessMin, tone::kBrightnessMax);
            const bool useRamp = settings::GetBool(settings::Id::DisplayGamma);
            if (tone::Neutral(brightness, useRamp) || !EnsureTone())
                return false;
            const uint64_t rampGeneration = useRamp ? display_gamma::Generation() : 0;
            if (s_tone.filled && s_tone.brightness == brightness && s_tone.ramp == useRamp && s_tone.rampGeneration == rampGeneration)
                return true;
            display_gamma::Table ramp;
            const bool haveRamp = useRamp && display_gamma::Latest(ramp) != 0;
            const tone::Lut lut = tone::BuildLut(brightness, haveRamp ? &ramp : nullptr);
            const auto texels = tone::ToHalfTexels(lut);
            // The frames in flight may be reading the table: rare (a setting
            // or the game's own ramp changed), so wait for them.
            WaitIdle();
            memcpy(s_tone.stagingMapped, texels.data(), sizeof(texels));
            Barrier(cmd, s_tone.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.imageExtent = { 256, 1, 1 };
            vkCmdCopyBufferToImage(cmd, s_tone.staging, s_tone.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            Barrier(cmd, s_tone.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            fprintf(stderr, "[video] tone: brightness %+d, Xbox 360 display gamma %s; 64->%.1f 128->%.1f 192->%.1f (8-bit levels, red)\n",
                brightness, !useRamp ? "off" : haveRamp ? std::format("on (the game's ramp #{})", rampGeneration).c_str() : "on (no ramp from the game yet)",
                lut.entry[64][0] * 255.0, lut.entry[128][0] * 255.0, lut.entry[192][0] * 255.0);
            s_tone.filled = true;
            s_tone.brightness = brightness;
            s_tone.ramp = useRamp;
            s_tone.rampGeneration = rampGeneration;
            return true;
        }

        // ---------------------------------------------------------------
        // Settings > Graphics > Upscaling's FSR 1 (video/fsr1.h; when it is
        // used: video/upscale.h): EASU (compute) from the anti-aliased frame
        // into s_fsr.image, the picture's size on the screen, then RCAS
        // (fragment) where the bicubic pass draws, with the tone table and
        // then the UI after it as before. Made the first time a frame is
        // enlarged with it; if that fails, those frames take the bicubic
        // pass (logged once).
        struct FsrState
        {
            bool tried = false, ready = false, toneTried = false;
            VkDescriptorSetLayout easuSet = VK_NULL_HANDLE, rcasSet = VK_NULL_HANDLE;
            VkPipelineLayout easuLayout = VK_NULL_HANDLE, rcasLayout = VK_NULL_HANDLE;
            VkPipeline easu = VK_NULL_HANDLE;
            VkShaderModule rcasFs = VK_NULL_HANDLE, rcasToneFs = VK_NULL_HANDLE;
            VkPipeline rcas = VK_NULL_HANDLE, rcasTone = VK_NULL_HANDLE;
            VkFormat rcasFormat = VK_FORMAT_UNDEFINED, rcasToneFormat = VK_FORMAT_UNDEFINED;  // the swapchain's they were made for
            // EASU's picture, the size of the picture on the screen.
            VkImage image = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;
            uint32_t width = 0, height = 0;
        } s_fsr;

        // The shaders' push constants (their P in video/fsr1.cpp).
        struct EasuPush
        {
            uint32_t con[16];  // FsrEasuCon's con0..con3
            int32_t band[4];   // the 16:9 band's columns x0..x1 and rows y0..y1 (video::BandOf)
            int32_t outW, outH, srcW, srcH;
            uint32_t bandOn;
        };
        static_assert(sizeof(EasuPush) == 100);
        struct RcasPush
        {
            uint32_t con[4];  // FsrRcasCon's (con[0] 0: no sharpening)
            int32_t band[4];
            int32_t originX, originY, outW, outH, srcW, srcH;
            uint32_t bandOn;
        };
        static_assert(sizeof(RcasPush) == 60);

        bool EnsureFsr()
        {
            if (s_fsr.tried)
                return s_fsr.ready;
            s_fsr.tried = true;
            auto start = std::chrono::steady_clock::now();
            VkDescriptorSetLayoutBinding easuBindings[2] = {
                { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            };
            // RCAS: EASU's picture, and the tone table (its tone variant).
            VkDescriptorSetLayoutBinding rcasBindings[2] = {
                { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
                { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
            };
            VkDescriptorSetLayoutCreateInfo dslci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            dslci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
            dslci.bindingCount = 2;
            dslci.pBindings = easuBindings;
            VkDescriptorSetLayoutCreateInfo rcasDslci = dslci;
            rcasDslci.pBindings = rcasBindings;
            VkPushConstantRange easuRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(EasuPush) };
            VkPushConstantRange rcasRange{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(RcasPush) };
            VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            plci.setLayoutCount = 1;
            plci.pSetLayouts = &s_fsr.easuSet;
            plci.pushConstantRangeCount = 1;
            plci.pPushConstantRanges = &easuRange;
            VkPipelineLayoutCreateInfo rcasPlci = plci;
            rcasPlci.pSetLayouts = &s_fsr.rcasSet;
            rcasPlci.pPushConstantRanges = &rcasRange;
            if (!Check(vkCreateDescriptorSetLayout(s_device, &dslci, nullptr, &s_fsr.easuSet), "FSR 1 EASU set layout") ||
                !Check(vkCreateDescriptorSetLayout(s_device, &rcasDslci, nullptr, &s_fsr.rcasSet), "FSR 1 RCAS set layout") ||
                !Check(vkCreatePipelineLayout(s_device, &plci, nullptr, &s_fsr.easuLayout), "FSR 1 EASU pipeline layout") ||
                !Check(vkCreatePipelineLayout(s_device, &rcasPlci, nullptr, &s_fsr.rcasLayout), "FSR 1 RCAS pipeline layout"))
            {
                fprintf(stderr, "[video] FSR 1 is off: its pipeline layouts couldn't be made; bicubic upscaling instead\n");
                return false;
            }
            VkShaderModule cs = PostModule(fsr1::EasuGlsl().c_str(), 2);
            s_fsr.rcasFs = PostModule(fsr1::RcasGlsl().c_str(), 1);
            if (!cs || !s_fsr.rcasFs)
            {
                fprintf(stderr, "[video] FSR 1 is off: its shaders didn't compile; bicubic upscaling instead\n");
                return false;
            }
            VkComputePipelineCreateInfo cpci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
            cpci.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs, "main", nullptr };
            cpci.layout = s_fsr.easuLayout;
            const bool made = Check(vkCreateComputePipelines(s_device, VK_NULL_HANDLE, 1, &cpci, nullptr, &s_fsr.easu), "FSR 1 EASU pipeline");
            vkDestroyShaderModule(s_device, cs, nullptr);
            if (!made)
            {
                fprintf(stderr, "[video] FSR 1 is off: its EASU pipeline couldn't be made; bicubic upscaling instead\n");
                return false;
            }
            s_fsr.ready = true;
            fprintf(stderr, "[video] FSR 1: EASU and RCAS compiled in %.0f ms\n",
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            return true;
        }

        // RCAS's pipeline for the swapchain's format, plain or with the tone
        // table (Brightness, the 360's display gamma); VK_NULL_HANDLE if it
        // can't be made (the frame then takes the bicubic pass).
        VkPipeline RcasPipeline(bool tone)
        {
            if (tone && !s_fsr.toneTried)
            {
                s_fsr.toneTried = true;
                const std::string glsl = ToneVariant(fsr1::RcasGlsl().c_str());
                s_fsr.rcasToneFs = glsl.empty() ? VK_NULL_HANDLE : PostModule(glsl.c_str(), 1);
                if (!s_fsr.rcasToneFs)
                    fprintf(stderr, "[video] FSR 1: the RCAS shader with Brightness couldn't be made; bicubic upscaling while the tone is in use\n");
            }
            VkShaderModule fs = tone ? s_fsr.rcasToneFs : s_fsr.rcasFs;
            VkPipeline& pipeline = tone ? s_fsr.rcasTone : s_fsr.rcas;
            VkFormat& format = tone ? s_fsr.rcasToneFormat : s_fsr.rcasFormat;
            if (!fs)
                return VK_NULL_HANDLE;
            if (pipeline && format == s_swapFormat)
                return pipeline;
            if (pipeline)
                vkDestroyPipeline(s_device, pipeline, nullptr);
            pipeline = MakeScalePipeline(fs, s_fsr.rcasLayout);
            format = pipeline ? s_swapFormat : VK_FORMAT_UNDEFINED;
            return pipeline;
        }

        void EnsureFsrImage(uint32_t width, uint32_t height)
        {
            if (s_fsr.image && s_fsr.width == width && s_fsr.height == height)
                return;
            WaitIdle();
            if (s_fsr.image)
            {
                vkDestroyImageView(s_device, s_fsr.view, nullptr);
                vkDestroyImage(s_device, s_fsr.image, nullptr);
                vkFreeMemory(s_device, s_fsr.memory, nullptr);
            }
            VkImageCreateInfo ii{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
            ii.imageType = VK_IMAGE_TYPE_2D;
            ii.format = VK_FORMAT_R8G8B8A8_UNORM;
            ii.extent = { width, height, 1 };
            ii.mipLevels = 1;
            ii.arrayLayers = 1;
            ii.samples = VK_SAMPLE_COUNT_1_BIT;
            ii.tiling = VK_IMAGE_TILING_OPTIMAL;
            ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            vkCreateImage(s_device, &ii, nullptr, &s_fsr.image);
            VkMemoryRequirements req;
            vkGetImageMemoryRequirements(s_device, s_fsr.image, &req);
            VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            ai.allocationSize = req.size;
            ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            vkAllocateMemory(s_device, &ai, nullptr, &s_fsr.memory);
            vkBindImageMemory(s_device, s_fsr.image, s_fsr.memory, 0);
            VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            vci.image = s_fsr.image;
            vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vci.format = VK_FORMAT_R8G8B8A8_UNORM;
            vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCreateImageView(s_device, &vci, nullptr, &s_fsr.view);
            s_fsr.width = width;
            s_fsr.height = height;
        }

        // EASU: the w x h frame (scaleInput) enlarged to dw x dh in
        // s_fsr.image, ready for RCAS's reads. Outside a rendering pass.
        void RecordEasu(VkCommandBuffer cmd, VkImageView scaleInput, uint32_t w, uint32_t h, int32_t dw, int32_t dh,
            const video::FrameBand& band, bool bandOn)
        {
            EnsureFsrImage(uint32_t(dw), uint32_t(dh));
            // After the last frame's RCAS read it.
            Barrier(cmd, s_fsr.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT,
                VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
            VkDescriptorImageInfo in{ s_post.sampler, scaleInput, VK_IMAGE_LAYOUT_GENERAL };
            VkDescriptorImageInfo out{ VK_NULL_HANDLE, s_fsr.view, VK_IMAGE_LAYOUT_GENERAL };
            VkWriteDescriptorSet writes[2] = {
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &in, nullptr, nullptr },
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &out, nullptr, nullptr },
            };
            EasuPush pc{};
            const std::array<uint32_t, 16> con = fsr1::EasuConstants(w, h, uint32_t(dw), uint32_t(dh));
            std::copy(con.begin(), con.end(), pc.con);
            pc.band[0] = band.x0;
            pc.band[1] = band.x1;
            pc.band[2] = band.y0;
            pc.band[3] = band.y1;
            pc.outW = dw;
            pc.outH = dh;
            pc.srcW = int32_t(w);
            pc.srcH = int32_t(h);
            pc.bandOn = bandOn ? 1u : 0u;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_fsr.easu);
            s_post.pushDescriptorSet(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_fsr.easuLayout, 0, 2, writes);
            vkCmdPushConstants(cmd, s_fsr.easuLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
            vkCmdDispatch(cmd, (uint32_t(dw) + 7) / 8, (uint32_t(dh) + 7) / 8, 1);
            Barrier(cmd, s_fsr.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        }

        // Inside a rendering pass on the swapchain image (or a capture's
        // image of its format): the picture at dx, dy, dw x dh, by `pass`
        // (FSR 1: RCAS over RecordEasu's image, with `rcas` its pipeline),
        // through the tone table when `tone`.
        void DrawPicture(VkCommandBuffer cmd, upscale::Pass pass, VkPipeline rcas, bool tone, VkImageView scaleInput, uint32_t w,
            uint32_t h, const video::FrameBand& band, bool bandOn, int32_t dx, int32_t dy, int32_t dw, int32_t dh, float sharpen)
        {
            VkViewport viewport{ float(dx), float(dy), float(dw), float(dh), 0.0f, 1.0f };
            VkRect2D scissor{ { dx, dy }, { uint32_t(dw), uint32_t(dh) } };
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            const bool fsr = pass == upscale::Pass::Fsr1;
            const VkPipelineLayout layout = fsr ? s_fsr.rcasLayout : tone ? s_tone.layout : s_post.scaleLayout;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fsr ? rcas : tone ? s_tone.pipeline : s_post.scale);
            VkDescriptorImageInfo in{ s_post.sampler, fsr ? s_fsr.view : scaleInput, VK_IMAGE_LAYOUT_GENERAL };
            VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 0, 0, 1,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &in, nullptr, nullptr };
            if (tone)
            {
                VkDescriptorImageInfo table{ s_post.sampler, s_tone.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                VkWriteDescriptorSet writes[2] = { write, write };
                writes[1].dstBinding = 1;
                writes[1].pImageInfo = &table;
                s_post.pushDescriptorSet(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 2, writes);
            }
            else
                s_post.pushDescriptorSet(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &write);
            if (fsr)
            {
                RcasPush pc{};
                const std::array<uint32_t, 4> con = fsr1::RcasConstants(upscale::RcasStops(sharpen));
                std::copy(con.begin(), con.end(), pc.con);
                pc.band[0] = band.x0;
                pc.band[1] = band.x1;
                pc.band[2] = band.y0;
                pc.band[3] = band.y1;
                pc.originX = dx;
                pc.originY = dy;
                pc.outW = dw;
                pc.outH = dh;
                pc.srcW = int32_t(w);
                pc.srcH = int32_t(h);
                pc.bandOn = bandOn ? 1u : 0u;
                vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
            }
            else
            {
                struct
                {
                    float width, height, sharpen;
                    uint32_t bicubic;
                    float band[4];
                    uint32_t bandOn;
                } pc = { float(w), float(h), sharpen, pass == upscale::Pass::Bicubic ? 1u : 0u,
                    { float(band.x0), float(band.x1), float(band.y0), float(band.y1) }, bandOn ? 1u : 0u };
                static_assert(sizeof(pc) == 36);
                vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
            }
            vkCmdDraw(cmd, 3, 1, 0, 0);
        }

        // The pass a frame takes (video/upscale.h), FSR 1 only where its
        // pipelines are ready (made here the first time); a line in the log
        // when the pass or the sizes change.
        upscale::Pass ChoosePass(bool tone, uint32_t w, uint32_t h, int32_t dw, int32_t dh, VkPipeline& rcas)
        {
            rcas = VK_NULL_HANDLE;
            upscale::Pass pass = upscale::Choose(settings::GetInt(settings::Id::Scaling), w, h, dw, dh);
            if (pass == upscale::Pass::Fsr1 && EnsureFsr())
                rcas = RcasPipeline(tone);
            if (pass == upscale::Pass::Fsr1 && !rcas)
                pass = upscale::Pass::Bicubic;
            static upscale::Pass lastPass = upscale::Pass::Linear;
            static uint32_t lastW = 0, lastH = 0;
            static int32_t lastDw = 0, lastDh = 0;
            static int lines = 0;
            if ((pass != lastPass || w != lastW || h != lastH || dw != lastDw || dh != lastDh) && lines < 32)
            {
                lines++;
                const bool fsrChosen = settings::GetInt(settings::Id::Scaling) == int32_t(settings::ScalingMode::Fsr1);
                fprintf(stderr, "[video] upscaling: %s, %ux%u -> %dx%d%s\n", upscale::PassName(pass), w, h, dw, dh,
                    pass == upscale::Pass::Fsr1 ? std::format(" (RCAS at {:.2f} of its strength)", upscale::RcasAmount(Sharpen())).c_str()
                    : !fsrChosen ? ""
                    : upscale::Enlarged(w, h, dw, dh) ? " (FSR 1 unavailable)" : " (FSR 1 only enlarges)");
            }
            lastPass = pass;
            lastW = w;
            lastH = h;
            lastDw = dw;
            lastDh = dh;
            return pass;
        }

        // NFSMW_UPSCALE_COMPARE=<passes> (a diagnostic): with each
        // NFSMW_CHECK_PRESENT_SEC capture, the same frame drawn again by each
        // listed pass into an image of its own (the swapchain's size and
        // format, without the UI), saved as
        // build/present_check_<k>_<name>.ppm: side-by-sides of one frame.
        // Passes: bicubic, linear, fsr1, each at Sharpening or with
        // :<sharpening> (fsr1:0 is EASU alone). fsr1 where the frame isn't
        // enlarged draws bicubic, as the setting does. "source": the frame
        // the passes start from (after the anti-aliasing), at its own size
        // (through the tone table when that is in use).
        struct CompareEntry
        {
            std::string name;
            int32_t mode;
            float sharpen;  // < 0: Sharpening's
            bool source = false;
        };
        const std::vector<CompareEntry>& CompareList()
        {
            static const std::vector<CompareEntry> list = [] {
                std::vector<CompareEntry> entries;
                const char* v = std::getenv("NFSMW_UPSCALE_COMPARE");
                std::string text = v ? v : "";
                for (size_t at = 0; at < text.size();)
                {
                    size_t end = text.find(',', at);
                    std::string item = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
                    at = end == std::string::npos ? text.size() : end + 1;
                    std::string kind = item.substr(0, item.find(':'));
                    float sharpen = item.find(':') == std::string::npos ? -1.0f : std::strtof(item.c_str() + item.find(':') + 1, nullptr);
                    int32_t mode = kind == "fsr1" ? int32_t(upscale::Mode::Fsr1) : kind == "linear" || kind == "source" ? int32_t(upscale::Mode::Linear)
                        : kind == "bicubic" ? int32_t(upscale::Mode::Bicubic) : -1;
                    if (mode < 0)
                    {
                        fprintf(stderr, "[video] NFSMW_UPSCALE_COMPARE: \"%s\" isn't bicubic, linear, fsr1[:<sharpening>] or source\n", item.c_str());
                        continue;
                    }
                    std::string name = item;
                    std::replace(name.begin(), name.end(), ':', '-');
                    entries.push_back({ name, mode, sharpen, kind == "source" });
                }
                return entries;
            }();
            return list;
        }

        struct CompareShot
        {
            std::string name;
            VkImage image = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceMemory bufferMemory = VK_NULL_HANDLE;
            uint32_t width = 0, height = 0;
        };

        // Records each listed pass's picture of this frame and its copy to
        // the host. Outside a rendering pass.
        std::vector<CompareShot> RecordCompare(VkCommandBuffer cmd, bool tone, VkImageView scaleInput, uint32_t w, uint32_t h,
            const video::FrameBand& band, bool bandOn, int32_t dx, int32_t dy, int32_t dw, int32_t dh)
        {
            std::vector<CompareShot> shots;
            for (const CompareEntry& entry : CompareList())
            {
                CompareShot shot;
                shot.name = entry.name;
                // The source: the frame image 1:1 (a bilinear fetch at each texel's centre).
                shot.width = entry.source ? w : s_swapExtent.width;
                shot.height = entry.source ? h : s_swapExtent.height;
                const int32_t x = entry.source ? 0 : dx, y = entry.source ? 0 : dy;
                const int32_t width = entry.source ? int32_t(w) : dw, height = entry.source ? int32_t(h) : dh;
                VkImageCreateInfo ii{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
                ii.imageType = VK_IMAGE_TYPE_2D;
                ii.format = s_swapFormat;
                ii.extent = { shot.width, shot.height, 1 };
                ii.mipLevels = 1;
                ii.arrayLayers = 1;
                ii.samples = VK_SAMPLE_COUNT_1_BIT;
                ii.tiling = VK_IMAGE_TILING_OPTIMAL;
                ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
                if (vkCreateImage(s_device, &ii, nullptr, &shot.image) != VK_SUCCESS)
                    break;
                VkMemoryRequirements req;
                vkGetImageMemoryRequirements(s_device, shot.image, &req);
                VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
                ai.allocationSize = req.size;
                ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                vkAllocateMemory(s_device, &ai, nullptr, &shot.memory);
                vkBindImageMemory(s_device, shot.image, shot.memory, 0);
                VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
                vci.image = shot.image;
                vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
                vci.format = s_swapFormat;
                vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                vkCreateImageView(s_device, &vci, nullptr, &shot.view);
                VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
                bci.size = VkDeviceSize(shot.width) * shot.height * 4;
                bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                vkCreateBuffer(s_device, &bci, nullptr, &shot.buffer);
                vkGetBufferMemoryRequirements(s_device, shot.buffer, &req);
                ai.allocationSize = req.size;
                ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                vkAllocateMemory(s_device, &ai, nullptr, &shot.bufferMemory);
                vkBindBufferMemory(s_device, shot.buffer, shot.bufferMemory, 0);

                VkPipeline rcas = VK_NULL_HANDLE;
                upscale::Pass pass = upscale::Choose(entry.mode, w, h, width, height);
                if (pass == upscale::Pass::Fsr1 && EnsureFsr())
                    rcas = RcasPipeline(tone);
                if (pass == upscale::Pass::Fsr1 && !rcas)
                    pass = upscale::Pass::Bicubic;
                if (pass == upscale::Pass::Fsr1)
                    RecordEasu(cmd, scaleInput, w, h, width, height, band, bandOn);
                Barrier(cmd, shot.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
                VkRenderingAttachmentInfoKHR color{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR };
                color.imageView = shot.view;
                color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                VkRenderingInfoKHR ri{ VK_STRUCTURE_TYPE_RENDERING_INFO_KHR };
                ri.renderArea = { { 0, 0 }, { shot.width, shot.height } };
                ri.layerCount = 1;
                ri.colorAttachmentCount = 1;
                ri.pColorAttachments = &color;
                s_context.cmdBeginRendering(cmd, &ri);
                DrawPicture(cmd, pass, rcas, tone, scaleInput, w, h, band, bandOn, x, y, width, height,
                    entry.sharpen >= 0.0f ? std::min(entry.sharpen, 1.0f) : Sharpen());
                s_context.cmdEndRendering(cmd);
                Barrier(cmd, shot.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);
                VkBufferImageCopy region{};
                region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                region.imageExtent = { shot.width, shot.height, 1 };
                vkCmdCopyImageToBuffer(cmd, shot.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shot.buffer, 1, &region);
                VkBufferMemoryBarrier hostRead{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
                hostRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                hostRead.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                hostRead.srcQueueFamilyIndex = hostRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                hostRead.buffer = shot.buffer;
                hostRead.size = VK_WHOLE_SIZE;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hostRead, 0, nullptr);
                fprintf(stderr, "[video] NFSMW_UPSCALE_COMPARE: %s drawn by %s\n", entry.name.c_str(), upscale::PassName(pass));
                shots.push_back(shot);
            }
            return shots;
        }

        // After the frame's fence: RecordCompare's pictures saved and freed.
        // NFSMW_LOG_COVERS=1 (gpu/covers.h): every presented picture, before
        // the UI, scaled to 1/8 by a linear blit and read back a frame later
        // (the slot's fence, waited for anyway), checked for a dark box with
        // all four edges inside it, or a dark layer across it short of its
        // top and bottom or its sides (FindInsetBox). Logged once per box (to 2%
        // of the picture) and kind of picture (an in-between one or not), with
        // the present's number and time; the lines are counted (PictureCoverHits).
        struct CoverCheck
        {
            VkImage image = VK_NULL_HANDLE;
            VkDeviceMemory imageMemory = VK_NULL_HANDLE;
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceMemory bufferMemory = VK_NULL_HANDLE;
            void* mapped = nullptr;
            uint32_t w = 0, h = 0;  // the scaled picture
            uint32_t srcW = 0, srcH = 0;
            bool pending = false;
            uint64_t present = 0;
            float between = 0.0f;
            double seconds = 0.0;
        };
        CoverCheck s_coverChecks[FRAMES_IN_FLIGHT];
        std::atomic<uint64_t> s_pictureCoverHits{ 0 };
        constexpr uint32_t kCoverCheckDiv = 8;

        void CoverCheckRecord(VkCommandBuffer cmd, VkImage dst, uint32_t slotIndex, float between)
        {
            static uint64_t presents = 0;
            static const auto launch = std::chrono::steady_clock::now();
            presents++;
            CoverCheck& c = s_coverChecks[slotIndex];
            if (!s_swapReadable)
                return;
            const uint32_t w = s_swapExtent.width / kCoverCheckDiv, h = s_swapExtent.height / kCoverCheckDiv;
            if (w < 16 || h < 16)
                return;
            if (c.w != w || c.h != h)
            {
                vkDeviceWaitIdle(s_device);
                if (c.buffer)
                {
                    vkUnmapMemory(s_device, c.bufferMemory);
                    vkDestroyBuffer(s_device, c.buffer, nullptr);
                    vkFreeMemory(s_device, c.bufferMemory, nullptr);
                    vkDestroyImage(s_device, c.image, nullptr);
                    vkFreeMemory(s_device, c.imageMemory, nullptr);
                }
                c = CoverCheck{};
                c.w = w;
                c.h = h;
                VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
                ici.imageType = VK_IMAGE_TYPE_2D;
                ici.format = s_swapFormat;
                ici.extent = { w, h, 1 };
                ici.mipLevels = ici.arrayLayers = 1;
                ici.samples = VK_SAMPLE_COUNT_1_BIT;
                ici.tiling = VK_IMAGE_TILING_OPTIMAL;
                ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
                vkCreateImage(s_device, &ici, nullptr, &c.image);
                VkMemoryRequirements req;
                vkGetImageMemoryRequirements(s_device, c.image, &req);
                VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
                ai.allocationSize = req.size;
                ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                vkAllocateMemory(s_device, &ai, nullptr, &c.imageMemory);
                vkBindImageMemory(s_device, c.image, c.imageMemory, 0);
                VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
                bci.size = VkDeviceSize(w) * h * 4;
                bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                vkCreateBuffer(s_device, &bci, nullptr, &c.buffer);
                vkGetBufferMemoryRequirements(s_device, c.buffer, &req);
                ai.allocationSize = req.size;
                ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                vkAllocateMemory(s_device, &ai, nullptr, &c.bufferMemory);
                vkBindBufferMemory(s_device, c.buffer, c.bufferMemory, 0);
                vkMapMemory(s_device, c.bufferMemory, 0, VK_WHOLE_SIZE, 0, &c.mapped);
            }
            Barrier(cmd, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            Barrier(cmd, c.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkImageBlit blit{};
            blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            blit.srcOffsets[1] = { int32_t(s_swapExtent.width), int32_t(s_swapExtent.height), 1 };
            blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            blit.dstOffsets[1] = { int32_t(w), int32_t(h), 1 };
            vkCmdBlitImage(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
            Barrier(cmd, c.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy region{};
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.imageExtent = { w, h, 1 };
            vkCmdCopyImageToBuffer(cmd, c.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c.buffer, 1, &region);
            VkBufferMemoryBarrier hostRead{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
            hostRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            hostRead.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            hostRead.srcQueueFamilyIndex = hostRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            hostRead.buffer = c.buffer;
            hostRead.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hostRead, 0, nullptr);
            // Back to the pass's layout: the UI is drawn after it.
            Barrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            c.pending = true;
            c.srcW = s_swapExtent.width;
            c.srcH = s_swapExtent.height;
            c.present = presents;
            c.between = between;
            c.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - launch).count();
        }

        // After the slot's fence.
        void CoverCheckHarvest(uint32_t slotIndex)
        {
            CoverCheck& c = s_coverChecks[slotIndex];
            if (!c.pending)
                return;
            c.pending = false;
            const bool bgr = s_swapFormat == VK_FORMAT_B8G8R8A8_UNORM || s_swapFormat == VK_FORMAT_B8G8R8A8_SRGB;
            static std::vector<uint8_t> luma;
            luma.resize(size_t(c.w) * c.h);
            const uint8_t* px = static_cast<const uint8_t*>(c.mapped);
            for (size_t i = 0; i < luma.size(); i++)
            {
                const uint32_t r = px[i * 4 + (bgr ? 2 : 0)], g = px[i * 4 + 1], b = px[i * 4 + (bgr ? 0 : 2)];
                luma[i] = uint8_t((2 * r + 5 * g + b) / 8);
            }
            const gpu::covers::Luma l{ luma.data(), int32_t(c.w), int32_t(c.h), int32_t(c.w) };
            const gpu::covers::Box box = gpu::covers::FindInsetBox(l);
            if (!box.found)
                return;
            // Once per box (2% of the picture) and kind of picture.
            const auto q = [](int32_t v, uint32_t size) { return uint64_t(std::lround(double(v) / double(size) * 50.0)); };
            const uint64_t key = (q(box.box.x0, c.w) << 24) | (q(box.box.y0, c.h) << 16) | (q(box.box.x1, c.w) << 8) | q(box.box.y1, c.h) |
                (uint64_t(c.between != 0.0f) << 32) | (uint64_t(c.srcW) << 33) | (uint64_t(c.srcH) << 48) | (uint64_t(box.kind) << 62);
            static std::unordered_set<uint64_t> logged;
            if (logged.size() >= 256 || !logged.insert(key).second)
                return;
            s_pictureCoverHits.fetch_add(1, std::memory_order_relaxed);
            const uint32_t d = kCoverCheckDiv;
            auto ring = [](float r) { return r < 0.0f ? std::string("its edge") : std::format("{:.0f}%", r * 100.0f); };
            fprintf(stderr,
                "[covers] picture: %s at present %llu (%.2f s%s): [%d,%d)x[%d,%d) of %ux%u px (%.0f%% of it; short by left %d top %d "
                "right %d bottom %d px), lit just outside it: left %s top %s right %s bottom %s\n",
                gpu::covers::BoxKindName(box.kind), (unsigned long long)c.present, c.seconds,
                c.between != 0.0f ? std::format(", an in-between picture at {:.3f}", c.between).c_str() : "", box.box.x0 * d, box.box.x1 * d,
                box.box.y0 * d, box.box.y1 * d, c.srcW, c.srcH, box.share * 100.0f, box.box.x0 * d, box.box.y0 * d,
                int32_t(c.srcW) - box.box.x1 * int32_t(d), int32_t(c.srcH) - box.box.y1 * int32_t(d), ring(box.ringLit[0]).c_str(),
                ring(box.ringLit[1]).c_str(), ring(box.ringLit[2]).c_str(), ring(box.ringLit[3]).c_str());
        }

        void SaveCompare(std::vector<CompareShot>& shots, int captureIndex)
        {
            const bool bgr = s_swapFormat == VK_FORMAT_B8G8R8A8_UNORM || s_swapFormat == VK_FORMAT_B8G8R8A8_SRGB;
            for (CompareShot& shot : shots)
            {
                void* mapped = nullptr;
                const size_t pixels = size_t(shot.width) * shot.height;
                if (vkMapMemory(s_device, shot.bufferMemory, 0, VkDeviceSize(pixels) * 4, 0, &mapped) == VK_SUCCESS)
                {
                    std::string path = std::format("build/present_check_{}_{}.ppm", captureIndex, shot.name);
                    if (FILE* out = fopen(path.c_str(), "wb"))
                    {
                        fprintf(out, "P6\n%u %u\n255\n", shot.width, shot.height);
                        const uint8_t* px = static_cast<const uint8_t*>(mapped);
                        std::vector<uint8_t> rgb(pixels * 3);
                        for (size_t i = 0; i < pixels; i++)
                            for (int k = 0; k < 3; k++)
                                rgb[i * 3 + k] = px[i * 4 + (bgr ? 2 - k : k)];
                        fwrite(rgb.data(), 1, rgb.size(), out);
                        fclose(out);
                        fprintf(stderr, "[video] NFSMW_UPSCALE_COMPARE: saved %s\n", path.c_str());
                    }
                    vkUnmapMemory(s_device, shot.bufferMemory);
                }
                vkDestroyImageView(s_device, shot.view, nullptr);
                vkDestroyImage(s_device, shot.image, nullptr);
                vkFreeMemory(s_device, shot.memory, nullptr);
                vkDestroyBuffer(s_device, shot.buffer, nullptr);
                vkFreeMemory(s_device, shot.bufferMemory, nullptr);
            }
            shots.clear();
        }

        // Xenia's Tiled2D for 32x32-aligned pitches (texture_address.h).
        uint32_t Tiled2D(uint32_t x, uint32_t y, uint32_t pitchAligned, uint32_t bppLog2)
        {
            uint32_t outer = (((y >> 5) * (pitchAligned >> 5)) + (x >> 5)) << 6;
            uint32_t inner = (((y >> 1) & 7) << 3) | (x & 7);
            uint32_t oi = (outer | inner) << bppLog2;
            uint32_t bank = (y >> 4) & 1;
            uint32_t pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1);
            return ((y & 1) << 4) | (pipe << 6) | (bank << 11) | (oi & 0xF) | (((oi >> 4) & 1) << 5) |
                (((oi >> 5) & 7) << 8) | ((oi >> 8) << 12);
        }
    }

    namespace
    {
        // Settings the window and swapchain follow live (main thread).
        void ApplySettings()
        {
            uint64_t generation = settings::Generation();
            if (generation == s_settingsGeneration)
                return;
            s_settingsGeneration = generation;
            // Only when the setting itself changes (the menu, Reset page; and
            // NFSMW_FULLSCREEN pins it): the window then follows it. A change
            // the system made since stays until then.
            if (std::optional<bool> fullscreen = s_fullscreen.SettingIs(settings::GetBool(settings::Id::Fullscreen)))
            {
                bool ok = SDL_SetWindowFullscreen(s_window, *fullscreen);
                fprintf(stderr, "[video] fullscreen %s (setting)%s%s\n", *fullscreen ? "on" : "off", ok ? "" : ": ",
                    ok ? "" : SDL_GetError());
            }
            bool rebuild = false;
            bool vsync = settings::GetBool(settings::Id::VSync);
            if (vsync != s_vsync)
            {
                s_vsync = vsync;
                rebuild = true;
            }
#if defined(__APPLE__) && TARGET_OS_IOS
            // Screen Resolution: CreateSwapchain sets the view's scale first.
            if (float scale = WantedPresentScale(); scale != s_presentScale)
            {
                fprintf(stderr, "[video] present scale %.3f -> %.3f (Screen Resolution)\n", s_presentScale, scale);
                s_presentScale = scale;
                rebuild = true;
            }
#endif
            if (rebuild)
            {
                WaitIdle();
                DestroySwapchain();
                s_swapReason = "V-Sync or Screen Resolution";
                CreateSwapchain();
                s_swapReason = nullptr;
            }
        }

        // The window's size for the next launch: only a window the player
        // sized (not fullscreen, maximised, or filling the display, as under
        // gamescope).
        void RememberWindowSize()
        {
            // NFSMW_WINDOW sets this run's size; keep the player's.
            if (s_gameMode || settings::EnvOverride(settings::Id::WindowWidth) || settings::EnvOverride(settings::Id::WindowHeight))
                return;
            if (SDL_GetWindowFlags(s_window) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED))
                return;
            int w = 0, h = 0;
            SDL_GetWindowSize(s_window, &w, &h);
            if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(s_window)))
                if (w >= mode->w && h >= mode->h)
                    return;
            if (w > 0 && h > 0)
            {
                settings::Set(settings::Id::WindowWidth, w);
                settings::Set(settings::Id::WindowHeight, h);
            }
        }

#ifdef _WIN32
        // Windows on an Intel integrated GPU starts with the defaults measured
        // on one: a ThinkPad T14 Gen 1's UHD 620 (Gen9.5, driver 101.2135) on
        // AC power, where after ~90 s of load the whole chip is held at 11.9 W
        // (the GPU gets ~6 W). A race at 1x in its 1280x720 window ran 20.4
        // fps, GPU-bound at 49 ms a frame (P4 tuning, 2026-10-07: 35
        // interleaved runs, base runs within 0.19 fps of each other). Race and
        // intro cinematic, frames a second:
        //   - fewer and lighter render passes (the renderer, as on Apple but
        //     for the pass barrier, which stays full: renderer.cpp,
        //     s_passBarrierLevel): with occlusion counting +1.8 / +1.5
        //     (alone: lighter +1.15 / +0.85, fewer +0.55 / +0.87; the
        //     barrier's share of lighter ~0.1 in the race). No artifact seen
        //     in the parked race's captures, by eye on zoomed crops: they
        //     fire at wall-clock times, so they aren't the same game frame and
        //     no pixel comparison was possible; the cinematic's weren't even
        //     the same shot.
        //   - Anti-Aliasing Off: +0.9 / +0.9 (its pass alone is 1.8 ms);
        //     edges stair-step.
        //   - Anisotropic Filtering 2x, not Auto's 4x: +0.3 / +0.4; the far
        //     road a little softer (Off: +0.55, plainly blurred).
        //   - Internal Resolution Auto at 1x (the renderer's budget, as on
        //     iOS): Auto would take 2x2, four times the scene's pixels, for
        //     a maximized window or a 1080p screen.
        //   - Frame Rate stays 60: no setting there holds 30, so a 30 cap
        //     gave the same frame rate.
        // All of it with the barrier at level 2 then (4b9b0c3) against
        // 7098f3f, each with the game's own defaults (3 runs against 4,
        // interleaved, drift-corrected): race 20.2 -> 22.8 fps (+2.6, +2.3
        // to +2.9; GPU -5.6 ms), cinematic 17.1 -> 19.4 (+2.3, +2.0 to +2.7;
        // the one run without other processes' CPU +2.5); maximized at
        // 1920x991 the race ran 8.7 fps (Auto's 2x2), then 22.2. No black
        // frame but the game's own cuts; sync validation through the
        // cinematic and the race found no hazard. Windows' Best performance
        // power mode (the player's setting) lets the same chip hold ~25 W
        // (the GPU 9-12 W, the thermal zone at 93-97 C): with these defaults
        // the race ran 25-29 fps there (1280x720 or maximized, 7 runs) and
        // the cinematic 23-24, and a 30 cap still gave the same rate
        // (maximized, 25.4 against 25.6 at 60).
        // Only Intel's: AMD's APUs (another driver, and several times this
        // GPU in a Radeon 680M-890M) and discrete GPUs are unmeasured on
        // Windows and keep the table's defaults and today's passes. Intel's
        // Xe and Arc integrated GPUs, faster than the UHD 620 but sharing
        // its driver's lineage, take them unmeasured: 1x Auto still renders
        // a quarter of 2x2's pixels. The settings are device defaults: a
        // value in settings.toml wins, as do the NFSMW_* variables (each pass
        // toggle has one). Occlusion counting is on for every GPU (see
        // Initialize).
        bool IntegratedGpuDefaults(const VkPhysicalDeviceProperties& props)
        {
            constexpr uint32_t kIntel = 0x8086;
            if (props.deviceType != VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU || props.vendorID != kIntel)
            {
                const char* kind = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? "a discrete GPU"
                    : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? "an integrated GPU, not Intel's"
                    : "not an integrated GPU";
                fprintf(stderr, "[video] %s is %s: the full-quality defaults (the integrated-GPU ones were measured on Intel's)\n",
                    props.deviceName, kind);
                return false;
            }
            fprintf(stderr, "[video] %s is an Intel integrated GPU: fewer and lighter render passes, Internal Resolution Auto at 1x, "
                "Anti-Aliasing Off and Anisotropic Filtering 2x by default (measured on a UHD 620, GPU-bound at ~20 fps in a race; "
                "settings.toml and NFSMW_* variables win)\n", props.deviceName);
            std::string why = std::format("an Intel integrated GPU on Windows, {}", props.deviceName);
            settings::SetDeviceDefault(settings::Id::AntiAliasing, 0, why);
            settings::SetDeviceDefault(settings::Id::Anisotropy, 2, why);
            return true;
        }

        // Windows: the display the window is on, logged when it changes (at
        // start, a swapchain rebuilt, another display, another mode). Its
        // refresh rate decides how the game's 60 fps shows: at 120 or 240 Hz
        // the guest vblank locks to every 2nd or 4th refresh (with present
        // timing: video/refresh_multiple.h), at 144 Hz each frame stays for 2
        // or 3 refreshes (the work notes' 144HZ-NOTE.md), and the repeat
        // count measures in its refreshes (OnScreen). It passes the rate on
        // for the refresh multiple (UpdateRefreshMultiple), so a rate changed
        // in Windows' settings while the game runs counts too. The one notice
        // a player sees about the rate is the refresh-rate notice
        // (MaybeRefreshNotice, video/refresh_notice.h: 90, 144 and 165 Hz,
        // not with variable refresh); the port's own refresh-rate toast
        // (MaybeHintRefresh, held in labelled builds) is gone with it.
        // NFSMW_TEST_DISPLAY_HZ=<hz> reports that rate instead (TestDisplayHz).
        void NoteDisplay(const char* why)
        {
            if (!s_window)
                return;
            SDL_DisplayID id = SDL_GetDisplayForWindow(s_window);
            const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(id);
            UpdateRefreshMultiple(mode);
            const float testHz = TestDisplayHz();
            float hz = testHz > 0.0f ? testHz : mode ? mode->refresh_rate : 0.0f;
            s_panelPeriodNs.store(hz > 0.0f ? 1e9 / hz : 0.0, std::memory_order_relaxed);
            int w = mode ? mode->w : 0, h = mode ? mode->h : 0;
            if (id == s_notedDisplay && w == s_notedW && h == s_notedH && hz == s_notedHz)
                return;
            s_notedDisplay = id;
            s_notedW = w;
            s_notedH = h;
            s_notedHz = hz;
            const char* name = SDL_GetDisplayName(id);
            const int multiple = RefreshFit::MultipleOf(hz);
            std::string cadence = hz <= 0.0f ? std::string("; its refresh rate is unknown")
                : multiple == 1 ? std::string()
                : multiple >= 2 ? std::format("; the game runs at 60 fps, {} of these refreshes a frame: even once the guest vblank "
                                              "locks to them (the [vblank] lines; it needs present timing)", multiple)
                : std::format("; the game runs at 60 fps, {:.2f} of these refreshes a frame: whole refreshes each, so they can "
                              "come unevenly unless G-SYNC/FreeSync is on ([perf] \"repeated\" counts the long ones)", hz / 60.0f);
            fprintf(stderr, "[video] display %s: %dx%d at %.2f Hz%s (%s)%s\n", name ? name : "?", w, h, hz,
                testHz > 0.0f ? " (NFSMW_TEST_DISPLAY_HZ)" : "", why, cadence.c_str());
        }
#endif

        // The device extensions the game can't start without: Initialize
        // enables them, and ChooseDevice puts a device with all of them first.
        constexpr const char* kNeededDeviceExtensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME,
            VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME, VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME,
            VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME, VK_KHR_MULTIVIEW_EXTENSION_NAME, VK_KHR_MAINTENANCE_2_EXTENSION_NAME,
            VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME };

        // The Vulkan device to draw with (s_physical, s_queueFamily; left
        // null when none can present to the window), and the log's one line
        // listing every device the loader found, the one used and why
        // (video/gpu_choice.h: a discrete GPU first, NFSMW_GPU=<index or part
        // of a name> to pick another). Up to v0.1.1 it was the loader's first
        // that could present, which on a laptop with two GPUs was usually the
        // integrated one; where only one device can present it still is that
        // one. A device's queue family is its first with graphics that can
        // present to the window, as before. On Windows the exe's
        // NvOptimusEnablement and AmdPowerXpressRequestHighPerformance ask the
        // same of the drivers (platform/startup.h).
        void ChooseDevice(const std::vector<VkPhysicalDevice>& devices)
        {
            std::vector<GpuCandidate> candidates;
            std::vector<uint32_t> families;
            for (size_t i = 0; i < devices.size(); i++)
            {
                VkPhysicalDevice d = devices[i];
                VkPhysicalDeviceProperties props;
                vkGetPhysicalDeviceProperties(d, &props);
                uint32_t qc = 0;
                vkGetPhysicalDeviceQueueFamilyProperties(d, &qc, nullptr);
                std::vector<VkQueueFamilyProperties> qs(qc);
                vkGetPhysicalDeviceQueueFamilyProperties(d, &qc, qs.data());
                uint32_t family = UINT32_MAX;
                for (uint32_t f = 0; f < qc && family == UINT32_MAX; f++)
                {
                    VkBool32 present = VK_FALSE;
                    vkGetPhysicalDeviceSurfaceSupportKHR(d, f, s_surface, &present);
                    if ((qs[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present)
                        family = f;
                }
                VkPhysicalDeviceMemoryProperties memory;
                vkGetPhysicalDeviceMemoryProperties(d, &memory);
                uint64_t local = 0;
                for (uint32_t h = 0; h < memory.memoryHeapCount; h++)
                    if (memory.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                        local += memory.memoryHeaps[h].size;
                uint32_t extCount = 0;
                vkEnumerateDeviceExtensionProperties(d, nullptr, &extCount, nullptr);
                std::vector<VkExtensionProperties> exts(extCount);
                vkEnumerateDeviceExtensionProperties(d, nullptr, &extCount, exts.data());
                std::string lacks;
                for (const char* needed : kNeededDeviceExtensions)
                    if (lacks.empty() && std::none_of(exts.begin(), exts.end(),
                            [needed](const VkExtensionProperties& e) { return strcmp(e.extensionName, needed) == 0; }))
                        lacks = needed;
                // Mesa's Dozen (Vulkan on Direct3D 12: Windows, WSL) lists GPUs
                // a native driver may list too.
                bool dozen = false;
                if (props.apiVersion >= VK_API_VERSION_1_2)
                {
                    VkPhysicalDeviceDriverProperties driver{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
                    VkPhysicalDeviceProperties2 props2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
                    props2.pNext = &driver;
                    vkGetPhysicalDeviceProperties2(d, &props2);
                    dozen = driver.driverID == VK_DRIVER_ID_MESA_DOZEN;
                }
                candidates.push_back({ i, int(props.deviceType), dozen, props.deviceName, local, family != UINT32_MAX, lacks });
                families.push_back(family);
            }
            GpuChoice choice = ChooseGpu(candidates, std::getenv("NFSMW_GPU"));
            fprintf(stderr, "[video] %s\n", GpuLogLine(candidates, choice).c_str());
            if (choice.pick == kNoGpu)
                return;
            s_physical = devices[choice.pick];
            s_queueFamily = families[choice.pick];
        }
    }

    bool Initialize()
    {
        if (const char* h = std::getenv("NFSMW_HEADLESS"); h && h[0] == '1')
            return false;
#if defined(__APPLE__) && TARGET_OS_IOS
        // MoltenVK encodes Metal at vkQueueSubmit on the submitting thread,
        // the command processor: ~2 ms a frame on an iPad Pro M2 (127 ms/s
        // in the scripted race), more in heavy scenes, and an iPhone has
        // two performance cores for the CP and the game. Asynchronous
        // submits encode on MoltenVK's own queue instead (1 ms/s left on the
        // CP; the same frames, screenshots alike). Read when MoltenVK first
        // loads its configuration, so before any Vulkan call;
        // MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=1 turns it back.
        setenv("MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS", "0", 0);
#endif
#ifdef __APPLE__
        // SpeedBreaker.app (scripts/macos/make_app.py) carries MoltenVK and
        // its manifest: the Vulkan loader uses only that driver, not one a
        // Homebrew install also has (two MoltenVKs, the system's chosen).
        // VK_DRIVER_FILES or VK_ICD_FILENAMES set by hand still win.
        if (!std::getenv("VK_DRIVER_FILES") && !std::getenv("VK_ICD_FILENAMES"))
            if (const char* base = SDL_GetBasePath())
            {
                std::string manifest = std::string(base) + "vulkan/icd.d/MoltenVK_icd.json";
                std::error_code ec;
                if (std::filesystem::exists(manifest, ec))
                    setenv("VK_DRIVER_FILES", manifest.c_str(), 1);
            }
#endif
        if (!SDL_Init(SDL_INIT_VIDEO))
        {
            fprintf(stderr, "[video] SDL_Init: %s\n", SDL_GetError());
            return false;
        }
        SDL_AddEventWatch(OnAppEvent, nullptr);
        hid::Initialize();
        // The size it had last time (NFSMW_WINDOW=<width>x<height> overrides).
        int windowW = settings::GetInt(settings::Id::WindowWidth), windowH = settings::GetInt(settings::Id::WindowHeight);
        s_gameMode = DetectGameMode();
        const bool startFullscreen = s_fullscreen.Launch(settings::GetBool(settings::Id::Fullscreen), FullscreenLocked());
        s_vsync = settings::GetBool(settings::Id::VSync);
#if defined(__APPLE__) && TARGET_OS_IOS
        // Always fullscreen there (FullscreenLocked), which hides the status
        // bar over the picture. SDL then also defers every screen edge's
        // system gestures, so going home would take two swipes: "0" keeps
        // the one swipe the windowed app had. ("2" defers them, if touch
        // players leave the game by accident.)
        SDL_SetHint(SDL_HINT_IOS_HIDE_HOME_INDICATOR, "0");
        // On iPhone, landscape only to SDL too: else it turns its view
        // portrait when a keyboard shows or hides (ios_ui.h).
        platform::ios::PrepareOrientation();
#endif
        s_window = SDL_CreateWindow("SpeedBreaker", windowW, windowH,
            SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | (startFullscreen ? SDL_WINDOW_FULLSCREEN : 0));
        if (!s_window)
        {
            fprintf(stderr, "[video] SDL_CreateWindow: %s\n", SDL_GetError());
            return false;
        }
#if defined(__APPLE__) && !TARGET_OS_IOS
        // The title bar over the picture (platform/macos_window.h); a window
        // made fullscreen gets it when it leaves fullscreen.
        platform::macos::ConfigureWindow(s_window, !startFullscreen);
#endif
#if defined(__APPLE__) && TARGET_OS_IOS
        platform::ios::WatchKeyboards(s_window);
#endif

        Uint32 extCount = 0;
        const char* const* sdlExts = SDL_Vulkan_GetInstanceExtensions(&extCount);
        std::vector<const char*> exts(sdlExts, sdlExts + extCount);
        exts.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
        // Portability enumeration is the Vulkan loader's (it lists MoltenVK
        // on macOS only when asked). iOS links MoltenVK itself, without a
        // loader, and MoltenVK doesn't offer it: ask only when it's there.
        uint32_t available = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &available, nullptr);
        std::vector<VkExtensionProperties> instanceExts(available);
        vkEnumerateInstanceExtensionProperties(nullptr, &available, instanceExts.data());
        bool portability = false;
        for (const VkExtensionProperties& e : instanceExts)
            portability |= strcmp(e.extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0;
        if (portability)
            exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);

        VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
        app.pApplicationName = "SpeedBreaker";
        app.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo ici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
        ici.flags = portability ? VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR : 0;
        ici.pApplicationInfo = &app;
        ici.enabledExtensionCount = uint32_t(exts.size());
        ici.ppEnabledExtensionNames = exts.data();
        if (!Check(vkCreateInstance(&ici, nullptr, &s_instance), "vkCreateInstance"))
            return false;
        if (!SDL_Vulkan_CreateSurface(s_window, s_instance, nullptr, &s_surface))
        {
            fprintf(stderr, "[video] SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
            return false;
        }
#if defined(__APPLE__) && TARGET_OS_IOS
        // Before the first swapchain: MoltenVK's surface extent follows the
        // layer's contentsScale.
        s_presentScale = WantedPresentScale();
        if (s_presentScale < 1.0f)
        {
            if (platform::ios::SetDrawableScale(s_window, s_presentScale))
                s_viewScaled = true;
            else
                fprintf(stderr, "[video] present scale: no Metal view to scale; full size\n");
        }
#endif

        uint32_t count = 0;
        vkEnumeratePhysicalDevices(s_instance, &count, nullptr);
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(s_instance, &count, devices.data());
        devices.resize(count);
        ChooseDevice(devices);
        if (!s_physical)
        {
            fprintf(stderr, "[video] no Vulkan device can present to the window\n");
            return false;
        }
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(s_physical, &props);
#ifdef _WIN32
        const bool integratedGpuDefaults = IntegratedGpuDefaults(props);  // before anything reads those settings
#endif

        // The renderer needs dynamic rendering, and imports guest memory as
        // a buffer where the device can (host-memory import; Turnip, the
        // Steam Frame's driver, can't: the renderer's GPU copy instead).
        std::vector<const char*> devExts(std::begin(kNeededDeviceExtensions), std::end(kNeededDeviceExtensions));
        uint32_t extAvail = 0;
        vkEnumerateDeviceExtensionProperties(s_physical, nullptr, &extAvail, nullptr);
        std::vector<VkExtensionProperties> avail(extAvail);
        vkEnumerateDeviceExtensionProperties(s_physical, nullptr, &extAvail, avail.data());
        bool presentId = false, presentWait = false, hostImport = false, shadingRate = false, displayTiming = false;
        bool pciBusInfo = false;  // (a property query only: not enabled)
        for (auto& e : avail)
        {
            pciBusInfo |= strcmp(e.extensionName, VK_EXT_PCI_BUS_INFO_EXTENSION_NAME) == 0;
            displayTiming |= strcmp(e.extensionName, VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME) == 0;
            if (strcmp(e.extensionName, "VK_KHR_portability_subset") == 0)
                devExts.push_back("VK_KHR_portability_subset");
            hostImport |= strcmp(e.extensionName, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME) == 0;
            shadingRate |= strcmp(e.extensionName, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME) == 0;
            presentId |= strcmp(e.extensionName, VK_KHR_PRESENT_ID_EXTENSION_NAME) == 0;
            presentWait |= strcmp(e.extensionName, VK_KHR_PRESENT_WAIT_EXTENSION_NAME) == 0;
        }
        // Present timing (the guest vblank lock), where the device has it.
        // NFSMW_PRESENT_TIMING=0 leaves it off, should a driver object to the
        // waits beside presents: no lock, and no repeat counts.
        const char* timing = std::getenv("NFSMW_PRESENT_TIMING");
        const bool timingOff = timing && timing[0] == '0';
        VkPhysicalDevicePresentIdFeaturesKHR presentIdFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR };
        VkPhysicalDevicePresentWaitFeaturesKHR presentWaitFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR };
        if (presentId && presentWait && !timingOff)
        {
            presentIdFeatures.pNext = &presentWaitFeatures;
            VkPhysicalDeviceFeatures2 features2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
            features2.pNext = &presentIdFeatures;
            vkGetPhysicalDeviceFeatures2(s_physical, &features2);
            s_presentTiming = presentIdFeatures.presentId && presentWaitFeatures.presentWait;
        }
        if (hostImport)
            devExts.push_back(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
        // NFSMW_SHADING_RATE=<w>x<h>: an experiment (presenter.h).
        uint32_t rateW = 0, rateH = 0;
        VkPhysicalDeviceFragmentShadingRateFeaturesKHR rateFeatures{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR };
        if (const char* v = std::getenv("NFSMW_SHADING_RATE"); v && shadingRate && sscanf(v, "%ux%u", &rateW, &rateH) == 2)
        {
            VkPhysicalDeviceFeatures2 features2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
            features2.pNext = &rateFeatures;
            vkGetPhysicalDeviceFeatures2(s_physical, &features2);
            if (rateFeatures.pipelineFragmentShadingRate)
            {
                rateFeatures = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR };
                rateFeatures.pipelineFragmentShadingRate = VK_TRUE;
                devExts.push_back(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);
                fprintf(stderr, "[video] shading rate %ux%u for the game's draws (NFSMW_SHADING_RATE, an experiment)\n", rateW, rateH);
            }
            else
                rateW = rateH = 0;
        }
        else
            rateW = rateH = 0;
        if (s_presentTiming)
        {
            devExts.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);
            devExts.push_back(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
        }
#if defined(__APPLE__) && !TARGET_OS_IOS
        {
            // Frames shown at a time need the watchdog, which needs the
            // present waiter (VK_KHR_present_wait).
            const char* v = std::getenv("NFSMW_PRESENT_AT");
            const double ms = v ? std::atof(v) : double(kShowAfterVblankNs) / 1e6;
            s_showAfterVblankNs = int64_t(std::clamp(ms, 0.0, 100.0) * 1e6);
            s_schedule = PresentSchedule(!v ? PresentSchedule::Mode::Auto : s_showAfterVblankNs > 0 ? PresentSchedule::Mode::On
                                                                                                   : PresentSchedule::Mode::Off);
            // Auto, on a display that isn't a 120 Hz one at launch (a 60 Hz
            // monitor, the macOS 15 VM): the extension isn't even enabled, so
            // the session's device and presents are v0.1.3's (it isn't asked
            // for again should the window move to a 120 Hz display later).
            const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(s_window));
            PresentSchedule launch;
            launch.SetDisplayHz(mode ? mode->refresh_rate : 0.0f);
            const bool mayShowAtTime = s_schedule.GetMode() == PresentSchedule::Mode::On ||
                (s_schedule.GetMode() == PresentSchedule::Mode::Auto && launch.FastDisplay());
            s_displayTiming = displayTiming && s_presentTiming && (mayShowAtTime || std::getenv("NFSMW_VBLANK_LOG"));
        }
#endif
        if (s_displayTiming)
            devExts.push_back(VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);

        float priority = 1.0f;
        VkDeviceQueueCreateInfo qci{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        qci.queueFamilyIndex = s_queueFamily;
        qci.queueCount = 1;
        qci.pQueuePriorities = &priority;
        VkPhysicalDeviceFeatures supported;
        vkGetPhysicalDeviceFeatures(s_physical, &supported);
        VkPhysicalDeviceFeatures features{};
        features.vertexPipelineStoresAndAtomics = supported.vertexPipelineStoresAndAtomics;
        features.fragmentStoresAndAtomics = supported.fragmentStoresAndAtomics;
        features.independentBlend = supported.independentBlend;
        features.dualSrcBlend = supported.dualSrcBlend;
        features.depthClamp = supported.depthClamp;
        features.samplerAnisotropy = supported.samplerAnisotropy;
        features.textureCompressionBC = supported.textureCompressionBC;
        features.shaderInt16 = supported.shaderInt16;
        VkPhysicalDeviceDynamicRenderingFeaturesKHR dynamicRendering{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR };
        dynamicRendering.dynamicRendering = VK_TRUE;
        if (s_presentTiming)
            dynamicRendering.pNext = &presentIdFeatures;  // and presentWaitFeatures
        // NFSMW_OCCLUSION (=1 on, anything else off): count the game's
        // occlusion queries on the GPU (presenter.h), which the sun's glare
        // fades by. On by default on Apple, Linux and Windows, for every GPU
        // there. Checked on MoltenVK (an M4 Max), RADV (the Steam Machine),
        // Turnip (the Steam Frame) and Intel's Windows driver (a UHD 620: the
        // sun's counts as the Mac's in the same view, the glare drawn; +0.2
        // fps in 3 of 3 runs, GPU-bound), the cost within noise everywhere;
        // NVIDIA's drivers and AMD's on Windows are on unchecked: precise
        // occlusion queries are core Vulkan, and without them the glare never
        // shows. Needs precise occlusion queries (counting, not boolean);
        // host query reset (core 1.2) is optional. Off, the device is created
        // exactly as without it.
#if defined(__APPLE__) || defined(__linux__) || defined(_WIN32)
        constexpr bool kOcclusionByDefault = true;
#else
        constexpr bool kOcclusionByDefault = false;
#endif
        static const bool occlusion = [] { const char* v = std::getenv("NFSMW_OCCLUSION"); return v ? strcmp(v, "1") == 0 : kOcclusionByDefault; }();
        bool occlusionCounting = false;
        VkPhysicalDeviceHostQueryResetFeatures hostQueryReset{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES };
        if (occlusion)
        {
            occlusionCounting = supported.occlusionQueryPrecise;
            if (occlusionCounting && props.apiVersion >= VK_API_VERSION_1_2)
            {
                VkPhysicalDeviceFeatures2 features2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
                features2.pNext = &hostQueryReset;
                vkGetPhysicalDeviceFeatures2(s_physical, &features2);
            }
            hostQueryReset.pNext = nullptr;
            hostQueryReset.hostQueryReset = occlusionCounting && hostQueryReset.hostQueryReset;
            features.occlusionQueryPrecise = occlusionCounting ? VK_TRUE : VK_FALSE;
            if (occlusionCounting)
                fprintf(stderr, "[video] occlusion queries counted on the GPU (NFSMW_OCCLUSION=0 turns it off; host query reset: %s)\n",
                    hostQueryReset.hostQueryReset ? "yes" : "no, reset in command buffers");
            else
                fprintf(stderr, "[video] NFSMW_OCCLUSION=1, but this device has no precise occlusion queries: not counted (zero samples, as without it)\n");
        }

        VkDeviceCreateInfo dci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        dci.pNext = &dynamicRendering;
        if (rateW)
        {
            rateFeatures.pNext = &dynamicRendering;
            dci.pNext = &rateFeatures;
        }
        if (hostQueryReset.hostQueryReset)
        {
            hostQueryReset.pNext = const_cast<void*>(dci.pNext);
            dci.pNext = &hostQueryReset;
        }
        dci.pEnabledFeatures = &features;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = uint32_t(devExts.size());
        dci.ppEnabledExtensionNames = devExts.data();
        if (!Check(vkCreateDevice(s_physical, &dci, nullptr, &s_device), "vkCreateDevice"))
            return false;
        vkGetDeviceQueue(s_device, s_queueFamily, 0, &s_queue);
        if (s_presentTiming)
            s_waitForPresent = (PFN_vkWaitForPresentKHR)vkGetDeviceProcAddr(s_device, "vkWaitForPresentKHR");
        s_presentTiming = s_waitForPresent != nullptr;
#if defined(__APPLE__) && !TARGET_OS_IOS
        // No frame is shown at a time without the driver's reports and the
        // present waiter that reads them (the watchdog).
        if (s_displayTiming && s_presentTiming)
            s_pastTiming = (PFN_vkGetPastPresentationTimingGOOGLE)vkGetDeviceProcAddr(s_device, "vkGetPastPresentationTimingGOOGLE");
        if (!s_pastTiming)
            s_displayTiming = false;
#endif
        if (s_presentTiming)
        {
            if (const char* path = std::getenv("NFSMW_VBLANK_LOG"))
                if ((s_timingLog = fopen(path, "w")))
                {
                    fprintf(s_timingLog, "id,vblank,presented,on_screen,timed,fit_valid,period_ns,grid_ns,rms_ns,ready,multiple,between,desired\n");
#if defined(__APPLE__) && !TARGET_OS_IOS
                    if (s_pastTiming && (s_actualLog = fopen((std::string(path) + ".actual.csv").c_str(), "w")))
                        fprintf(s_actualLog, "# steady_ns %lld uptime_raw_ns %llu\nid,actual_ns\n", (long long)NowNs(),
                            (unsigned long long)clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
#endif
                }
            std::thread(PresentWaiter).detach();
        }
#if defined(__APPLE__) && !TARGET_OS_IOS
        if (s_displayTiming && s_showAfterVblankNs > 0 && s_schedule.GetMode() != PresentSchedule::Mode::Off)
            fprintf(stderr, "[vblank] new frames shown %.2f ms after their vblank %s (VK_GOOGLE_display_timing, watched; "
                "NFSMW_PRESENT_AT=0: never)\n", double(s_showAfterVblankNs) / 1e6, s_schedule.GetMode() == PresentSchedule::Mode::On
                    ? "on any display (NFSMW_PRESENT_AT)" : "on a 120 Hz display the vblank isn't locked to");
        else if (s_schedule.GetMode() == PresentSchedule::Mode::Auto)
            fprintf(stderr, "[vblank] new frames shown when ready: not a 120 Hz display at launch, or no VK_GOOGLE_display_timing (not used)\n");
#endif
        fprintf(stderr, "[vblank] %s\n", s_presentTiming ? "presents are timed (VK_KHR_present_wait)"
            : timingOff ? "presents aren't timed (NFSMW_PRESENT_TIMING=0): the guest vblank can't lock to the display"
            : "no present timing (VK_KHR_present_wait) on this device: the guest vblank can't lock to the display");
#ifdef _WIN32
        // Which of the two the device has (NVIDIA's Windows driver is
        // expected to have both, Intel's 101.2135 has neither).
        s_timingText = std::format("VK_KHR_present_id {}, VK_KHR_present_wait {}{}", presentId ? "yes" : "no",
            presentWait ? "yes" : "no", s_presentTiming ? ": presents timed" : timingOff ? ": not used (NFSMW_PRESENT_TIMING=0)"
                : presentId && presentWait ? ": listed, but not usable" : ": presents not timed");
        fprintf(stderr, "[vblank] %s on %s\n", s_timingText.c_str(), props.deviceName);
#endif

        VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = s_queueFamily;
        vkCreateCommandPool(s_device, &pci, nullptr, &s_commandPool);
        for (Frame& f : s_frames)
        {
            VkCommandBufferAllocateInfo cai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            cai.commandPool = s_commandPool;
            cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cai.commandBufferCount = 1;
            vkAllocateCommandBuffers(s_device, &cai, &f.cmd);
            VkSemaphoreCreateInfo si{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            vkCreateSemaphore(s_device, &si, nullptr, &f.imageAvailable);
            VkFenceCreateInfo fi{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            vkCreateFence(s_device, &fi, nullptr, &f.inFlight);
        }
        if (const char* v = std::getenv("NFSMW_GPU_TIMING"); v && v[0] == '1')
        {
            uint32_t familyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(s_physical, &familyCount, nullptr);
            std::vector<VkQueueFamilyProperties> families(familyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(s_physical, &familyCount, families.data());
            VkQueryPoolCreateInfo qpci{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
            qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qpci.queryCount = FRAMES_IN_FLIGHT * 3;
            if (families[s_queueFamily].timestampValidBits == 0 ||
                vkCreateQueryPool(s_device, &qpci, nullptr, &s_timestamps) != VK_SUCCESS)
                s_timestamps = VK_NULL_HANDLE;
            s_timestampPeriod = props.limits.timestampPeriod;
        }
        CreateSwapchain();
        s_context = { s_instance, s_physical, s_device, s_queueFamily, s_queue, &s_queueMutex,
            hostImport ? (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(s_device, "vkGetMemoryHostPointerPropertiesEXT") : nullptr,
            (PFN_vkCmdBeginRenderingKHR)vkGetDeviceProcAddr(s_device, "vkCmdBeginRenderingKHR"),
            (PFN_vkCmdEndRenderingKHR)vkGetDeviceProcAddr(s_device, "vkCmdEndRenderingKHR"), rateW, rateH, occlusionCounting,
            hostQueryReset.hostQueryReset ? (PFN_vkResetQueryPool)vkGetDeviceProcAddr(s_device, "vkResetQueryPool") : nullptr };
#ifdef _WIN32
        s_context.integratedGpuDefaults = integratedGpuDefaults;
#endif
        s_contextReady = true;
        s_settingsGeneration = settings::Generation();
        fprintf(stderr, "[video] %s, swapchain %ux%u%s\n", props.deviceName, s_swapExtent.width, s_swapExtent.height, GeometryNote().c_str());
#if defined(__APPLE__) && TARGET_OS_IOS
        if (s_presentScale < 1.0f)
        {
            int pw = 0, ph = 0;
            SDL_GetWindowSizeInPixels(s_window, &pw, &ph);
            fprintf(stderr, "[video] present scale %.3f (%s): drawables %ux%u for a %dx%d screen\n", s_presentScale,
                s_envPresentScale > 0.0f ? "NFSMW_PRESENT_SCALE" : "Screen Resolution", s_swapExtent.width, s_swapExtent.height, pw, ph);
        }
#endif
        {
            // What the game was given, against the monitor (Game Mode: the
            // screen is the size Steam's per-game Game Resolution sets).
            const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(s_window));
            auto [nativeW, nativeH] = NativeMonitorMode();
            fprintf(stderr, "[video] screen %dx%d%s, monitor %dx%d, %s\n", mode ? mode->w : 0, mode ? mode->h : 0,
                s_fullscreen.Window() ? " (fullscreen)" : "", nativeW, nativeH, s_gameMode ? "Game Mode (gamescope)" : "desktop");
            // For the logs' system summary and bug reports (report/system_info.cpp).
            {
                const char* type = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? "discrete"
                    : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? "integrated"
                    : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU ? "virtual"
                    : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? "software" : "other";
                std::string driver = std::format("driver version {:08X}", props.driverVersion);
                if (props.apiVersion >= VK_API_VERSION_1_2)
                {
                    VkPhysicalDeviceDriverProperties driverProps{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
                    VkPhysicalDeviceProperties2 props2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
                    props2.pNext = &driverProps;
                    vkGetPhysicalDeviceProperties2(s_physical, &props2);
                    driver = std::format("{} {}", driverProps.driverName, driverProps.driverInfo);
                }
                // Issue #40: a discrete GPU's PCIe link (Linux: sysfs), which
                // bounds it when guest memory is in system memory (gpu/pcie_link.h).
                std::string link;
#if defined(__linux__) && !defined(__ANDROID__)
                if (pciBusInfo && props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU && props.apiVersion >= VK_API_VERSION_1_1)
                {
                    VkPhysicalDevicePCIBusInfoPropertiesEXT pci{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT };
                    VkPhysicalDeviceProperties2 props2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
                    props2.pNext = &pci;
                    vkGetPhysicalDeviceProperties2(s_physical, &props2);
                    link = gpu::pcie::Describe(gpu::pcie::Read(gpu::pcie::SysfsDir(pci.pciDomain, pci.pciBus, pci.pciDevice, pci.pciFunction)));
                    if (!link.empty())
                        fprintf(stderr, "[video] PCIe link: %s\n", link.c_str());
                }
#else
                (void)pciBusInfo;
#endif
                uint32_t loader = VK_API_VERSION_1_0;
                vkEnumerateInstanceVersion(&loader);
                report::SetSystemItem("GPU", std::format("{} ({}, {:04x}:{:04x}), {}, Vulkan {}.{}.{} (loader {}.{}.{}){}",
                    props.deviceName, type, props.vendorID, props.deviceID, driver, VK_API_VERSION_MAJOR(props.apiVersion),
                    VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion), VK_API_VERSION_MAJOR(loader),
                    VK_API_VERSION_MINOR(loader), VK_API_VERSION_PATCH(loader), link.empty() ? std::string() : ", " + link));
                UpdateDisplayItem();
#ifdef _WIN32
                report::SetSystemItem("Present", s_presentText + "; " + s_timingText);
#endif
            }
            if (s_gameMode && mode && nativeW > 0 && int64_t(mode->w) * mode->h < int64_t(nativeW) * nativeH)
                s_resolutionHint = std::format("Steam gives this game {}x{} of your {}x{} display. For full sharpness, set this game's "
                    "Properties > General > Game Resolution to Native in Steam.", mode->w, mode->h, nativeW, nativeH);
        }
        s_uiReady = ui::Initialize(s_window, { s_instance, s_physical, s_device, s_queueFamily, s_queue, &s_queueMutex,
            VK_API_VERSION_1_2, s_swapFormat });
        if (!s_resolutionHint.empty())
        {
            fprintf(stderr, "[video] %s\n", s_resolutionHint.c_str());
            ui::Toast(s_resolutionHint, 15.0);
        }
        return true;
    }

    namespace
    {
        // The bug report's "Display" line: at start, and again with each
        // swapchain of a new size (it said what the window was at launch).
        void UpdateDisplayItem()
        {
            const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(s_window));
#if defined(__APPLE__) && !TARGET_OS_IOS
            s_displayHz = mode ? mode->refresh_rate : 0.0f;
            s_offeredHz.store(s_displayHz, std::memory_order_relaxed);
            {
                std::lock_guard lock(s_scheduleMutex);
                s_schedule.SetDisplayHz(s_displayHz);
            }
#endif
            UpdateRefreshMultiple(mode);
            static const std::pair<int, int> native = NativeMonitorMode();
            const auto [nativeW, nativeH] = native;
            int windowW = 0, windowH = 0;
            SDL_GetWindowSizeInPixels(s_window, &windowW, &windowH);
            const char* name = SDL_GetDisplayName(SDL_GetDisplayForWindow(s_window));
            report::SetSystemItem("Display", std::format("{} {}x{} at {:.2f} Hz{}; window {}x{} pixels{}, "
                "swapchain {}x{}, V-Sync {}; SDL video driver {}; {}", name ? name : "?", mode ? mode->w : 0, mode ? mode->h : 0,
                mode ? mode->refresh_rate : 0.0f, nativeW > 0 ? std::format(", monitor {}x{}", nativeW, nativeH) : std::string(),
                windowW, windowH, s_fullscreen.Window() ? " (fullscreen)" : "",
                s_swapExtent.width, s_swapExtent.height, s_vsync ? "on" : "off", SDL_GetCurrentVideoDriver(),
                s_gameMode ? "Game Mode (gamescope)" : "desktop"));
        }
    }

    void ReleaseFrontImage(VkImage image)
    {
        std::lock_guard use(s_frontUseMutex);
        std::lock_guard lock(s_pendingMutex);
        if (s_pendingImage == image)
        {
            s_pendingImage = VK_NULL_HANDLE;
            s_pendingNew = !s_pending.empty();
        }
        if (s_betweenImage == image)
        {
            s_betweenImage = VK_NULL_HANDLE;
            s_betweenNew = false;
        }
        if (s_lastImage == image)
        {
            s_lastImage = VK_NULL_HANDLE;
            s_lastWidth = s_lastHeight = 0;
        }
    }

    void SubmitFrontBuffer(const uint32_t fetch[6], uint32_t width, uint32_t height)
    {
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            fprintf(stderr, "[video] first front buffer: %ux%u, fetch %08X %08X %08X %08X %08X %08X, window %s\n",
                width, height, fetch[0], fetch[1], fetch[2], fetch[3], fetch[4], fetch[5], s_window ? "yes" : "no");
        }
        if (!s_window)
            return;
        bool tiled = (fetch[0] >> 31) & 1;
        uint32_t pitch = ((fetch[0] >> 22) & 0x1FF) << 5;  // texels, 32-aligned
        uint32_t format = fetch[1] & 0x3F;
        uint32_t endian = (fetch[1] >> 6) & 3;
        uint32_t swizzle = (fetch[3] >> 1) & 0xFFF;
        uint32_t base = fetch[1] & 0xFFFFF000;  // physical
        if (pitch == 0)
            pitch = (width + 31) & ~31u;

        const uint8_t* src = static_cast<const uint8_t*>(g_memory.Translate(vmem::Physical().Base() | (base & 0x1FFFFFFF)));
        std::vector<uint32_t> pixels(size_t(width) * height);
        for (uint32_t y = 0; y < height; y++)
        {
            for (uint32_t x = 0; x < width; x++)
            {
                uint32_t offset = tiled ? Tiled2D(x, y, pitch, 2) : (y * pitch + x) * 4;
                uint32_t raw;
                memcpy(&raw, src + offset, 4);
                // The GPU reads the little-endian word, applies the fetch
                // constant's endian swap, and x/y/z/w are then the low-to-high
                // components; the fetch swizzle picks R, G and B from them.
                uint32_t v = raw;
                switch (endian)
                {
                case 1: v = ((v << 8) & 0xFF00FF00u) | ((v >> 8) & 0x00FF00FFu); break;
                case 2: v = ByteSwap(v); break;
                case 3: v = (v >> 16) | (v << 16); break;
                default: break;
                }
                uint32_t comp[6];
                if (format == 54 || format == 7)  // 2_10_10_10 (optionally AS_16_16_16_16)
                {
                    comp[0] = ((v >> 0) & 0x3FF) >> 2;
                    comp[1] = ((v >> 10) & 0x3FF) >> 2;
                    comp[2] = ((v >> 20) & 0x3FF) >> 2;
                    comp[3] = (v >> 30) * 85;
                }
                else  // k_8_8_8_8
                {
                    for (int c = 0; c < 4; c++)
                        comp[c] = (v >> (8 * c)) & 0xFF;
                }
                comp[4] = 0;
                comp[5] = 255;
                auto pick = [&](uint32_t sel) { return comp[std::min(sel & 7, 5u)]; };
                uint32_t bgra = 0xFF000000u | (pick(swizzle) << 16) | (pick(swizzle >> 3) << 8) | pick(swizzle >> 6);
                pixels[size_t(y) * width + x] = bgra;
            }
        }
        // NFSMW_DUMP_FRAMES=<dir>: save every 60th front buffer as a PPM.
        static const char* dumpDir = std::getenv("NFSMW_DUMP_FRAMES");
        static uint32_t frameNumber = 0;
        if (dumpDir && (frameNumber++ % 60) == 0)
        {
            char path[512];
            snprintf(path, sizeof(path), "%s/frame_%05u.ppm", dumpDir, frameNumber - 1);
            if (FILE* f = fopen(path, "wb"))
            {
                fprintf(f, "P6\n%u %u\n255\n", width, height);
                for (uint32_t p : pixels)
                {
                    uint8_t rgb[3] = { uint8_t(p >> 16), uint8_t(p >> 8), uint8_t(p) };
                    fwrite(rgb, 1, 3, f);
                }
                fclose(f);
            }
        }

        std::lock_guard lock(s_pendingMutex);
        if (s_pendingNew)
            s_dropped++;
        s_pending = std::move(pixels);
        s_pendingWidth = width;
        s_pendingHeight = height;
        s_pendingAspectW = s_pendingAspectH = 0;
        s_pendingNew = true;
        s_pendingVblank = s_guestVblank.load(std::memory_order_relaxed);
    }

    namespace
    {
        // NFSMW_WINDOW_AT=<s>:<w>x<h>,...: resize the window at those seconds
        // (NFSMW_UI_KEYS's clock), to test what follows the window's size,
        // such as the Auto internal resolution, without a person. A step of
        // <s>:full or <s>:windowed toggles fullscreen there instead (on
        // macOS a fullscreen Space, as the green button makes), as the system
        // would: the Fullscreen setting stays as it is.
        void ScriptedWindowSizes()
        {
            struct Step { double at; int w, h; };  // w -1: fullscreen, -2: windowed
            static std::vector<Step> steps = [] {
                std::vector<Step> out;
                const char* v = std::getenv("NFSMW_WINDOW_AT");
                for (const char* p = v; p && *p;)
                {
                    Step step{};
                    char word[16] = {};
                    if (sscanf(p, "%lf:%dx%d", &step.at, &step.w, &step.h) == 3 && step.w > 0 && step.h > 0)
                        out.push_back(step);
                    else if (sscanf(p, "%lf:%15[a-z]", &step.at, word) == 2 && (!strcmp(word, "full") || !strcmp(word, "windowed")))
                    {
                        step.w = word[0] == 'f' ? -1 : -2;
                        out.push_back(step);
                    }
                    p = strchr(p, ',');
                    p = p ? p + 1 : nullptr;
                }
                return out;
            }();
            static const auto start = std::chrono::steady_clock::now();
            static size_t next = 0;
            if (next >= steps.size() ||
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < steps[next].at)
                return;
            const Step& step = steps[next++];
            if (step.w < 0)
            {
                fprintf(stderr, "[video] NFSMW_WINDOW_AT: %s\n", step.w == -1 ? "fullscreen" : "windowed");
                SDL_SetWindowFullscreen(s_window, step.w == -1);
                return;
            }
            fprintf(stderr, "[video] NFSMW_WINDOW_AT: window %dx%d\n", step.w, step.h);
            SDL_SetWindowSize(s_window, step.w, step.h);
        }
    }

    namespace
    {
        // SDL calls this as the app event happens (on iOS, on the main
        // thread inside SDL_PollEvent, in UIKit's scene callbacks): the GPU
        // must be idle before the callback returns, not at the next frame.
        // From "will resign active" (Control Center, a call, the app
        // switcher) to "did become active", not just while in the background:
        // the game stops as soon as it isn't the app in front. UIKit reports
        // each transition twice, so every case here is idempotent.
        bool SDLCALL OnAppEvent(void*, SDL_Event* e)
        {
            switch (e->type)
            {
            case SDL_EVENT_WILL_ENTER_BACKGROUND:
                SuspendGame();
                break;
            case SDL_EVENT_DID_ENTER_BACKGROUND:
                // Suspended already, unless "will resign active" never came:
                // Metal must be idle in the background either way.
                SuspendGame();
                [[fallthrough]];
            case SDL_EVENT_TERMINATING:
                // The system may end a background app without another word.
                if (settings::Dirty())
                    settings::Save();
                report::FlushLog();
                break;
            case SDL_EVENT_DID_ENTER_FOREGROUND:
                ResumeGame();
                break;
            default:
                // SDL_EVENT_WILL_ENTER_FOREGROUND among them: the app is
                // still inactive then.
                break;
            }
            return true;
        }

        // NFSMW_TEST_SUSPEND's app events, waiting for the main thread.
        std::mutex s_testEventsMutex;
        std::vector<uint32_t> s_testEvents;
        std::atomic<bool> s_testEventsWaiting{ false };  // so a frame without any takes no lock

        const char* AppEventName(uint32_t type)
        {
            switch (type)
            {
            case SDL_EVENT_WILL_ENTER_BACKGROUND:
                return "SDL_EVENT_WILL_ENTER_BACKGROUND";
            case SDL_EVENT_DID_ENTER_BACKGROUND:
                return "SDL_EVENT_DID_ENTER_BACKGROUND";
            case SDL_EVENT_WILL_ENTER_FOREGROUND:
                return "SDL_EVENT_WILL_ENTER_FOREGROUND";
            case SDL_EVENT_DID_ENTER_FOREGROUND:
                return "SDL_EVENT_DID_ENTER_FOREGROUND";
            case SDL_EVENT_TERMINATING:
                return "SDL_EVENT_TERMINATING";
            default:
                return "an unknown event";
            }
        }

        // First thing in a frame: the same handler, thread and place in the
        // frame as on iOS (inside SDL_PollEvent, before any of the frame's
        // work), and twice, as UIKit delivers them.
        void DeliverTestAppEvents()
        {
            if (!s_testEventsWaiting.exchange(false, std::memory_order_acquire))
                return;
            std::vector<uint32_t> events;
            {
                std::lock_guard lock(s_testEventsMutex);
                events.swap(s_testEvents);
            }
            static int delivered = 0;
            for (uint32_t type : events)
                for (int k = 0; k < 2; k++)
                {
                    fprintf(stderr, "[test] NFSMW_TEST_SUSPEND: deliver %s (#%d)\n", AppEventName(type), ++delivered);
                    SDL_Event e{};
                    e.type = type;
                    OnAppEvent(nullptr, &e);
                }
        }

        // What the game's clocks and counters stood at, for the [suspend]
        // lines. Only the thread that suspends and resumes the game uses
        // them (see SuspendGame).
        struct SuspendMarks
        {
            double session = 0;  // the log's clock (s)
            int64_t host = 0;    // steady clock (ns)
            int64_t guest = 0, timebase = 0, system = 0;
            uint64_t vblanks = 0, audioFrames = 0, timerFirings = 0;
        };
        SuspendMarks s_suspendedAt, s_resumedAt;
        bool s_auditPending = false;
        uint64_t s_auditEpoch = 0;  // the suspension before the resume the audit follows

        SuspendMarks TakeMarks()
        {
            SuspendMarks m;
            m.session = report::SessionSeconds();
            m.host = guesttime::RawNs();
            m.guest = guesttime::NowNs();
            m.timebase = guesttime::Timebase();
            m.system = guesttime::SystemTime100ns();
            m.vblanks = GuestVblanks();
            m.audioFrames = apu::RenderedFrames();
            m.timerFirings = dispatcher::TimerFirings();
            return m;
        }

        // Guest seconds since the session began: the log's clock less the
        // time spent suspended (guest = host - offset, or frozen).
        double GuestSessionSeconds(const SuspendMarks& m)
        {
            return m.session - double(m.host - m.guest) / 1e9;
        }
    }

    void WaitForeground(std::unique_lock<std::mutex>& queueLock)
    {
        s_foreground.wait(queueLock, [] { return !s_background.load(std::memory_order_relaxed); });
    }

    // NFSMW_TEST_SUSPEND_GPU_ONLY=1: hold only the GPU, as before the game's
    // clocks could stop (7303475): scripts/test_suspend.sh's red baseline, in
    // which the movie runs ahead after each resume.
    static bool SuspendGpuOnly()
    {
        static const bool only = [] {
            const char* v = std::getenv("NFSMW_TEST_SUSPEND_GPU_ONLY");
            return v && v[0] == '1';
        }();
        return only;
    }

    void SuspendGame()
    {
        if (s_window)
            s_swapPolicy.Suspended();
        // 1. Every guest clock stops at one instant. The game's threads, its
        // timers, the vblank and the audio cadence then park on their own:
        // nothing here waits for them.
        bool changed = !SuspendGpuOnly() && guesttime::Suspend();
        if (changed)
        {
            s_suspendedAt = TakeMarks();
            // 2. The output plays silence and keeps what is queued for the
            // resume. Now, not after the drain: nothing renders from here, so
            // a device still playing through it would empty the queue.
            apu::SuspendOutput();
        }
        // 3. Then no GPU work may start (Metal refuses it from an inactive
        // app). What was submitted finishes, its completions running their
        // guest callbacks at the frozen time.
        {
            std::lock_guard lock(s_queueMutex);
            if (!s_background.load(std::memory_order_relaxed) && s_queue)
            {
                s_background.store(true, std::memory_order_release);
                vkQueueWaitIdle(s_queue);
                if (SuspendGpuOnly())
                {
                    s_visible.store(false, std::memory_order_relaxed);  // as 7303475 did: the watchdog pauses
                    fprintf(stderr, "[suspend] GPU only: held at vblank %llu\n", (unsigned long long)GuestVblanks());
                }
            }
        }
        if (!changed)
            return;
        fprintf(stderr, "[suspend] suspended: the game's clocks stop at guest %.3f s (vblank %llu, audio frame %llu)\n",
            GuestSessionSeconds(s_suspendedAt), (unsigned long long)s_suspendedAt.vblanks,
            (unsigned long long)s_suspendedAt.audioFrames);
    }

    void ResumeGame()
    {
        // 1. GPU work may start: a submit parked in WaitForeground goes out,
        // still at the frozen time.
        bool held;
        {
            std::lock_guard lock(s_queueMutex);
            held = s_background.exchange(false, std::memory_order_acq_rel);
        }
        if (s_window)
            s_swapPolicy.Resumed();  // the swapchain is checked against the surface for a while
        if (held)
        {
            s_foreground.notify_all();
            if (SuspendGpuOnly())
            {
                s_visible.store(true, std::memory_order_relaxed);
                fprintf(stderr, "[suspend] GPU only: released at vblank %llu\n", (unsigned long long)GuestVblanks());
            }
        }
        // 2. Then guest time moves on. What the counters did meanwhile is
        // read before, so nothing that runs once time moves counts.
        uint64_t vblanks = GuestVblanks(), audioFrames = apu::RenderedFrames(), firings = dispatcher::TimerFirings();
        if (SuspendGpuOnly() || !guesttime::Resume())
            return;
        s_resumedAt = TakeMarks();
        // 3. The output plays again, what was queued at the freeze first.
        apu::ResumeOutput();
        const SuspendMarks& a = s_suspendedAt;
        const SuspendMarks& b = s_resumedAt;
        fprintf(stderr, "[suspend] resumed after %.3f s: guest time %+.6f s, mftb %+lld ticks, system time %+.6f s; "
            "meanwhile %llu vblanks, %llu audio frames, %llu timer firings\n",
            double(b.host - a.host) / 1e9, double(b.guest - a.guest) / 1e9, (long long)(b.timebase - a.timebase),
            double(b.system - a.system) / 1e7, (unsigned long long)(vblanks - a.vblanks),
            (unsigned long long)(audioFrames - a.audioFrames), (unsigned long long)(firings - a.timerFirings));
        s_auditPending = true;
        s_auditEpoch = guesttime::Epoch();
    }

    void SuspendAudit()
    {
        if (!s_auditPending || guesttime::RawNs() - s_resumedAt.host < 2'000'000'000)
            return;
        s_auditPending = false;
        // Suspended again since: the numbers would span that suspension too.
        if (guesttime::Epoch() != s_auditEpoch)
            return;
        SuspendMarks now = TakeMarks();
        const SuspendMarks& r = s_resumedAt;
        double host = double(now.host - r.host) / 1e9, guestNs = double(now.guest - r.guest);
        // A guest vblank fires every GuestVblankDivider() display periods.
        double vblankNs = GuestVblank().PeriodNs() * GuestVblankDivider();
        fprintf(stderr, "[suspend] %.1f s after resuming: guest %.3f s, mftb %.3f s (host %.3f s), %llu vblanks (%.1f expected), "
            "%llu audio frames (%.1f expected), audio queued %.1f ms\n",
            host, guestNs / 1e9, double(now.timebase - r.timebase) / double(guesttime::kTimebaseHz), host,
            (unsigned long long)(now.vblanks - r.vblanks), guestNs / vblankNs, (unsigned long long)(now.audioFrames - r.audioFrames),
            guestNs / apu::kFrameNs, apu::QueuedMs());
    }

    void TestAppEvent(uint32_t type)
    {
        std::lock_guard lock(s_testEventsMutex);
        s_testEvents.push_back(type);
        s_testEventsWaiting.store(true, std::memory_order_release);
    }

    namespace
    {
        // Builds the swapchain again when the policy says so
        // (swapchain_policy.h), or (iOS) fits SDL's view back to its window.
        // Once a frame, before the acquire. Main thread.
        void CheckSwapchain()
        {
            SurfaceState state;
            state.active = !s_background.load(std::memory_order_acquire);
            state.swapchain = s_swapchain ? Extent{ s_swapExtent.width, s_swapExtent.height } : Extent{};
            if (s_swapPolicy.WantsSurface())  // active or not: the policy holds off while inactive
            {
                state.surfaceRead = true;
                state.surface = SurfaceExtent();
                state.interface = InterfaceNow();
                state.landscapeOnly = LandscapeOnlyHere();
#if defined(__APPLE__) && TARGET_OS_IOS
                state.viewFitsWindow = platform::ios::ViewFitsWindow(s_window);
                // Turning the phone over (landscape left and right) changes
                // no size, so nothing else would say so.
                static const char* lastScene = nullptr;
                if (const char* scene = platform::ios::SceneOrientationName(s_window); scene != lastScene)
                {
                    if (lastScene)
                        fprintf(stderr, "[video] the interface turned: %s -> %s (swapchain %ux%u, surface %ux%u)\n", lastScene, scene,
                            s_swapExtent.width, s_swapExtent.height, state.surface.width, state.surface.height);
                    lastScene = scene;
                }
#endif
            }
            const SwapDecision d = s_swapPolicy.Decide(state);
            if (d.settled)
                fprintf(stderr, "[video] %u frames after the resume: swapchain %ux%u, surface %ux%u%s\n", s_swapPolicy.Rules().settleFrames,
                    s_swapExtent.width, s_swapExtent.height, state.surface.width, state.surface.height, GeometryNote().c_str());
            if (d.action == SwapAction::RepairView)
            {
                fprintf(stderr, "[video] size check: %s (surface %ux%u, swapchain %ux%u, interface %s)\n", d.reason, state.surface.width,
                    state.surface.height, state.swapchain.width, state.swapchain.height, OrientationName(state.interface));
#if defined(__APPLE__) && TARGET_OS_IOS
                platform::ios::FitViewToWindow(s_window);  // SDL's resize events follow; the next frames look again
#endif
            }
            else if (d.action == SwapAction::Recreate)
            {
                WaitIdle();
                DestroySwapchain();
                s_swapReason = d.reason;
                CreateSwapchain();  // at ChooseExtent of the surface then (d.extent, unless it moved since)
                s_swapReason = nullptr;
            }
        }
    }

    namespace
    {
        // Linux: a monitor or TV is connected (and on), not only a built-in
        // panel (eDP): the Deck docked, a Steam Machine. Elsewhere false.
        bool ExternalScreen()
        {
#ifdef __linux__
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator("/sys/class/drm", ec))
            {
                const std::string name = entry.path().filename().string();
                if (name.find('-') == std::string::npos || name.find("eDP") != std::string::npos || name.find("Writeback") != std::string::npos)
                    continue;
                std::ifstream status(entry.path() / "status"), enabled(entry.path() / "enabled");
                std::string state, on;
                if ((status >> state) && state == "connected" && (enabled >> on) && on == "enabled")
                    return true;
            }
#endif
            return false;
        }

        // The one-time refresh-rate notice (video/refresh_notice.h), judged
        // twice a second; a line for each rate once it is shown or won't be,
        // and for the first few reasons it waits. Main thread.
        void MaybeRefreshNotice()
        {
            using Notice = RefreshNotice;
            static const int forced = [] {
                const char* v = std::getenv("NFSMW_REFRESH_NOTICE");
                return v && (v[0] == '0' || v[0] == '1') ? v[0] - '0' : -1;
            }();
            static auto lastCheck = std::chrono::steady_clock::time_point{};
            const auto now = std::chrono::steady_clock::now();
            if (now - lastCheck < std::chrono::milliseconds(500) || !s_window)
                return;
            lastCheck = now;
            const int key = Notice::Key(s_modeHz);
            static int decided = -1, waitedFor = -1, waitLines = 0;  // the rate decided (shown or never), the rate waited on
            static const char* lastWait = nullptr;
            if (key == decided)
                return;
            Notice::Inputs in;
            in.modeHz = s_modeHz;
            in.modeStableSec = std::chrono::duration<double>(now - s_modeSince).count();
#if (defined(__APPLE__) && TARGET_OS_IOS) || (defined(__linux__) && defined(__aarch64__))
            in.platform = Notice::Platform::Excluded;
#elif defined(__APPLE__)
            in.platform = Notice::Platform::Mac;
#else
            in.platform = s_gameMode ? Notice::Platform::GameMode : Notice::Platform::Desktop;
#endif
            const Notice::Decision early = Notice::Decide(in);  // NotApplicable, without the costlier inputs
            if (early.verdict == Notice::Verdict::NotApplicable)
                return;
            static double macMin = 0, macMax = 0;
#if defined(__APPLE__) && !TARGET_OS_IOS
            in.macFixed = platform::macos::DisplayRefreshFixed(s_window, &macMin, &macMax);
#endif
            // NFSMW_REFRESH_NOTICE_SCREEN=builtin|external: which screen Game
            // Mode's words are for, whatever is connected (tests: a Steam
            // Machine has only a TV).
            static const char* screen = std::getenv("NFSMW_REFRESH_NOTICE_SCREEN");
            if (in.platform == Notice::Platform::GameMode)
                in.externalScreen = screen && !strcmp(screen, "builtin") ? false : screen && !strcmp(screen, "external") ? true : ExternalScreen();
            const int64_t gridSince = s_gridFixedSince.load(std::memory_order_relaxed);
            in.gridFixed = gridSince != 0;
            in.gridStableSec = gridSince ? double(NowNs() - gridSince) / 1e9 : 0.0;
#ifdef _WIN32
            // Without present timing (Intel's 101.2135 driver has none) there
            // is no grid check, so a fixed refresh can't be told from
            // variable refresh: no notice. With NFSMW_TEST_DISPLAY_HZ the grid
            // is taken as fixed at that rate (the panel tested on a laptop
            // whose driver has none).
            if (!s_presentTiming)
            {
                if (TestDisplayHz() > 0.0f)
                {
                    in.gridFixed = true;
                    in.gridStableSec = in.modeStableSec;
                }
                else
                {
                    decided = key;
                    fprintf(stderr, "[video] refresh notice for %d Hz: not shown: no present timing on this device (VK_KHR_present_wait), "
                        "so a fixed refresh rate can't be told from variable refresh\n", key);
                    return;
                }
            }
#endif
            in.shownBefore = settings::RefreshNoticeShown(key);
            {
                double period = 0;
                int64_t grid = 0;
                in.hfrHandles = hfr::Enabled() && HfrGrid(period, grid);
            }
            in.forced = forced;
            const bool frames = s_firstGameFrame != std::chrono::steady_clock::time_point{};
            in.sessionSec = frames ? std::chrono::duration<double>(now - s_firstGameFrame).count() : 0.0;
            in.flowingSec = frames && now - s_lastGameFrame <= std::chrono::duration<double, std::milli>(Notice::kGapMs)
                ? std::chrono::duration<double>(now - s_flowingSince).count() : 0.0;
            if (const int64_t movie = game::LastMovieFrameNs())
                in.movieQuietSec = double(NowNs() - movie) / 1e9;
            in.idleSec = hid::SecondsSinceInput();
            in.uiBusy = ui::HasModal() || ui::SettingsOpen();
            const Notice::Decision d = Notice::Decide(in);
            const char* where = in.platform == Notice::Platform::GameMode ? (in.externalScreen ? "Game Mode, a monitor or TV" : "Game Mode, the built-in screen")
                : in.platform == Notice::Platform::Mac ? "macOS" : "a desktop";
            switch (d.verdict)
            {
            case Notice::Verdict::NotApplicable:
                return;
            case Notice::Verdict::Never:
                decided = key;
                if (in.platform == Notice::Platform::Mac)
                    fprintf(stderr, "[video] refresh notice for %d Hz: not shown: %s (the screen: %.1f-%.1f Hz)\n", key, d.reason, macMin, macMax);
                else
                    fprintf(stderr, "[video] refresh notice for %d Hz: not shown: %s\n", key, d.reason);
                return;
            case Notice::Verdict::Wait:
                if (waitedFor != key)
                {
                    waitedFor = key;
                    waitLines = 0;
                    lastWait = nullptr;
                }
                if (d.reason != lastWait && waitLines < 4)
                {
                    waitLines++;
                    lastWait = d.reason;
                    fprintf(stderr, "[video] refresh notice for %d Hz (%s): not yet: %s (%.0f s in)\n", key, where, d.reason, in.sessionSec);
                }
                return;
            case Notice::Verdict::Show:
                break;
            }
            const bool shown = ui::ShowNotice(Notice::kTitle, Notice::Text(in), [key](const char*) {
                if (settings::RecordRefreshNotice(key))
                    fprintf(stderr, "[video] refresh notice for %d Hz: recorded in settings.toml (not shown again for it)\n", key);
            });
            if (!shown)
                return;
            decided = key;
            fprintf(stderr, "[video] refresh notice for %d Hz: shown (%s; %.0f s in, the controls idle %.1f s, %s)\n", key, where,
                in.sessionSec, in.idleSec, in.platform == Notice::Platform::Mac ? "macOS says the screen's refresh is fixed"
                    : std::format("frames on a fixed {:.2f} Hz grid for {:.0f} s", s_modeHz, in.gridStableSec).c_str());
        }
    }

    bool RunFrame()
    {
        DeliverTestAppEvents();
        ui::Tick();
        platform::thermal::Poll();  // about once a second
        memstatus::Poll();          // iOS: the memory footprint, once a minute
#if defined(__APPLE__) && TARGET_OS_IOS
        platform::ios::PollBattery();  // every 5 seconds
#endif
        ScriptedWindowSizes();
        // SIGTERM, SIGINT, SIGHUP (platform/quit_signals.h): the window's
        // way out, from the game, the installer or the startup error page.
        if (quitsignals::Requested())
            return false;
        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            ui::ProcessEvent(e);
            hid::HandleEvent(e);
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                return false;
            if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED || e.type == SDL_EVENT_WINDOW_RESIZED ||
                e.type == SDL_EVENT_WINDOW_METAL_VIEW_RESIZED || e.type == SDL_EVENT_DISPLAY_ORIENTATION)
            {
                // Only a reason to look: CheckSwapchain compares the surface
                // with the swapchain, once the app is active (a size seen
                // while it isn't may be iOS's snapshot of it, or SDL's guess).
                s_swapPolicy.SizeChanged();
#if defined(__APPLE__) && TARGET_OS_IOS
                if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
                    fprintf(stderr, "[video] window pixels %dx%d%s%s\n", e.window.data1, e.window.data2,
                        s_background.load(std::memory_order_acquire) ? " (app inactive: checked after the resume)" : "", GeometryNote().c_str());
#endif
            }
            if (e.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED || e.type == SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED)
                UpdateRefreshMultiple(SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(s_window)));
            if (e.type == SDL_EVENT_WINDOW_RESIZED)
                RememberWindowSize();
            else if (e.type == SDL_EVENT_WINDOW_FOCUS_GAINED || e.type == SDL_EVENT_WINDOW_FOCUS_LOST)
                s_focused.store(e.type == SDL_EVENT_WINDOW_FOCUS_GAINED, std::memory_order_relaxed);
            else if (e.type == SDL_EVENT_WINDOW_MINIMIZED || e.type == SDL_EVENT_WINDOW_HIDDEN || e.type == SDL_EVENT_WINDOW_OCCLUDED)
                s_visible.store(false, std::memory_order_relaxed);
            else if (e.type == SDL_EVENT_WINDOW_RESTORED || e.type == SDL_EVENT_WINDOW_SHOWN || e.type == SDL_EVENT_WINDOW_EXPOSED ||
                e.type == SDL_EVENT_WINDOW_MAXIMIZED)
                s_visible.store(true, std::memory_order_relaxed);
#ifdef _WIN32
            else if (e.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED || e.type == SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED ||
                e.type == SDL_EVENT_DISPLAY_DESKTOP_MODE_CHANGED)
            {
                // Another display, or its refresh rate changed in Windows'
                // settings: logged, and the refresh multiple (NoteDisplay).
                NoteDisplay(e.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED ? "the window moved to another display" : "a display's mode changed");
            }
#endif
            else if (e.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN || e.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN)
            {
                // The window's state only. A change the game didn't ask for
                // (the green button or Window > Enter Full Screen on a Mac, a
                // window manager's key, a macOS fullscreen Space that didn't
                // open at launch) lasts this session: the Fullscreen setting,
                // the player's choice, still decides the next launch.
                const bool on = e.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN;
                if (s_fullscreen.WindowIs(on) == FullscreenPolicy::Change::System)
                    fprintf(stderr, "[video] fullscreen %s by the system (the window's fullscreen button or menu, or the window "
                        "manager), for this session; the Fullscreen setting (%s) is unchanged and decides the next launch\n",
                        on ? "on" : "off", settings::GetBool(settings::Id::Fullscreen) ? "On" : "Off");
#if defined(__APPLE__) && !TARGET_OS_IOS
                // SDL took the title bar off the picture for the fullscreen
                // Space: back on here, in the event loop, after SDL's own
                // callbacks (platform/macos_window.h says why not sooner).
                int windowW = 0, windowH = 0;
                if (!on && platform::macos::RestoreAfterFullscreen(s_window) && SDL_GetWindowSize(s_window, &windowW, &windowH))
                    fprintf(stderr, "[video] the title bar over the picture again (window %dx%d)\n", windowW, windowH);
#endif
            }
        }
        // Without the queue lock: the command processor's submits take it,
        // and the main thread would hold it nearly all the suspension.
        if (s_background.load(std::memory_order_acquire))
        {
            // A rumble the game asked for can last seconds: it stops with
            // the game (here, not inside UIKit's callback).
            if (!s_inputSuspended)
            {
                hid::Suspend();
                s_inputSuspended = true;
            }
            SDL_Delay(16);
            return true;
        }
        if (s_inputSuspended)
        {
            hid::Resume();  // the game's last rumble again
            s_inputSuspended = false;
        }
        CheckSwapchain();
        SuspendAudit();
        hid::SetBlocked(ui::CapturingInput());
        hid::Update();
        ApplySettings();
        MaybeRefreshNotice();
        if (!s_swapchain && !CreateSwapchain())
        {
            SDL_Delay(16);
            return true;
        }

        // Held until the frame that uses the renderer's image is submitted
        // (ReleaseFrontImage waits for it before that image is destroyed).
        std::unique_lock frontLock(s_frontUseMutex);
        std::vector<uint32_t> pixels;
        uint32_t w = 0, h = 0, aspectW = 0, aspectH = 0;
        VkImage gpuImage = VK_NULL_HANDLE;
        int64_t frameVblank = 0;
        float frameBetween = 0.0f;  // an in-between picture: its fraction of the step
        float frameShift = 0.0f;    // a real picture shown that much of a vblank early (a step's own picture drawn between)
        int64_t frameTarget = 0;    // when to show it (HFR on a grid that isn't a multiple of 60 Hz; 0: the usual way)
        int64_t frameDesired = 0;   // macOS: when it is to be shown (0: when ready)
        {
            std::lock_guard lock(s_pendingMutex);
            if (s_betweenNew)
            {
                gpuImage = s_betweenImage;
                s_betweenImage = VK_NULL_HANDLE;
                w = s_betweenWidth;
                h = s_betweenHeight;
                aspectW = s_betweenAspectW;
                aspectH = s_betweenAspectH;
                s_betweenNew = false;
                frameVblank = s_betweenVblank;
                frameBetween = s_betweenT;
                frameTarget = s_betweenTarget;
            }
            else if (s_pendingNew)
            {
                pixels.swap(s_pending);
                gpuImage = s_pendingImage;
                s_pendingImage = VK_NULL_HANDLE;
                w = s_pendingWidth;
                h = s_pendingHeight;
                aspectW = s_pendingAspectW;
                aspectH = s_pendingAspectH;
                s_pendingNew = false;
                frameVblank = s_pendingVblank;
                frameShift = s_pendingShift;
                frameTarget = s_pendingTarget;
            }
        }
        const bool upload = !pixels.empty();
        s_lastFreshImage = gpuImage != VK_NULL_HANDLE;  // this frame is new (not a redraw)
        if (!upload && !s_lastFreshImage)
            frameVblank = 0;
        if (upload || gpuImage)
        {
            s_lastImage = gpuImage;  // null: the CPU path's frame stays in s_source
            s_lastWidth = w;
            s_lastHeight = h;
            s_lastAspectW = aspectW;
            s_lastAspectH = aspectH;
            {
                const auto now = std::chrono::steady_clock::now();
                if (s_firstGameFrame == std::chrono::steady_clock::time_point{})
                    s_firstGameFrame = s_flowingSince = now;
                else if (now - s_lastGameFrame > std::chrono::duration<double, std::milli>(RefreshNotice::kGapMs))
                    s_flowingSince = now;
                s_lastGameFrame = now;
            }
            ui::NoteGameFrame();
        }
        else
        {
            if (!ui::WantsFrames())
            {
                frontLock.unlock();
                SDL_Delay(2);
                return true;
            }
            // The game has stopped presenting (loading, or not started yet):
            // keep the UI moving at about 60 Hz over the last frame. While
            // the game presents, the UI rides its frames; a redraw between
            // them would only delay the next one behind the vblank.
            auto now = std::chrono::steady_clock::now();
            if (now - s_lastGameFrame < std::chrono::milliseconds(50) || now - s_lastPresent < std::chrono::milliseconds(15))
            {
                frontLock.unlock();
                SDL_Delay(1);
                return true;
            }
            gpuImage = s_lastImage;
            w = s_lastWidth;
            h = s_lastHeight;
            aspectW = s_lastAspectW;
            aspectH = s_lastAspectH;
            if (!gpuImage && !(s_source && s_sourceWidth == w && s_sourceHeight == h))
                w = h = 0;
        }
        const bool haveSource = w && h;
        const bool uiFrame = s_uiReady && ui::BeginFrame();
        // Screenshots asked for (CaptureFrame): this frame's game image,
        // taken before the UI is drawn over it. Without one, none.
        std::vector<std::promise<CapturedFrame>> captures;
        {
            std::lock_guard lock(s_captureMutex);
            captures.swap(s_captureRequests);
        }
        if (!captures.empty() && !(haveSource && s_swapReadable))
        {
            for (auto& promise : captures)
                promise.set_value({});
            captures.clear();
        }
        RegionCapture gameCapture;

        Frame& f = s_frames[s_frameIndex];
        vkWaitForFences(s_device, 1, &f.inFlight, VK_TRUE, UINT64_MAX);
        if (game::covers::Enabled())
            CoverCheckHarvest(s_frameIndex);
        if (f.timed)
        {
            uint64_t ts[3];
            if (vkGetQueryPoolResults(s_device, s_timestamps, s_frameIndex * 3, 3, sizeof(ts), ts, sizeof(uint64_t),
                    VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ts[2] >= ts[1] && ts[1] >= ts[0])
            {
                s_postFrames++;
                s_postAaNs += uint64_t(double(ts[1] - ts[0]) * s_timestampPeriod);
                s_postNs += uint64_t(double(ts[2] - ts[0]) * s_timestampPeriod);
                if (s_upscaleAb)
                {
                    const int k = int(f.pass);
                    s_abFrames[k]++;
                    s_abNs[k] += uint64_t(double(ts[2] - ts[0]) * s_timestampPeriod);
                    s_abAaNs[k] += uint64_t(double(ts[1] - ts[0]) * s_timestampPeriod);
                    if ((s_abFrames[0] + s_abFrames[1] + s_abFrames[2]) % 600 == 0)
                    {
                        std::string text;
                        for (upscale::Pass pass : { upscale::Pass::Bicubic, upscale::Pass::Fsr1, upscale::Pass::Linear })
                        {
                            const int i = int(pass);
                            if (s_abFrames[i])
                                text += std::format(" | {}: {:.3f} ms (AA {:.3f} ms) over {} frames", upscale::PassName(pass),
                                    double(s_abNs[i]) / 1e6 / double(s_abFrames[i]), double(s_abAaNs[i]) / 1e6 / double(s_abFrames[i]), s_abFrames[i]);
                            s_abFrames[i] = s_abNs[i] = s_abAaNs[i] = 0;
                        }
                        fprintf(stderr, "[upscale-ab] post-processing GPU per frame%s\n", text.c_str());
                    }
                }
            }
            f.timed = false;
        }
        if (f.sourceView)
        {
            vkDestroyImageView(s_device, f.sourceView, nullptr);  // the last use of this frame slot has finished
            f.sourceView = VK_NULL_HANDLE;
        }
        uint32_t imageIndex;
        VkResult acquire = vkAcquireNextImageKHR(s_device, s_swapchain, UINT64_MAX, f.imageAvailable, VK_NULL_HANDLE, &imageIndex);
        // Either is for the next frame's CheckSwapchain, which runs before
        // its acquire (one decision a frame: the policy counts in frames).
        if (acquire == VK_SUBOPTIMAL_KHR)
            s_swapPolicy.Reported(SurfaceResult::Suboptimal);  // this frame is drawn and presented anyway
        if (acquire == VK_ERROR_OUT_OF_DATE_KHR)
        {
            s_swapPolicy.Reported(SurfaceResult::OutOfDate);
            // Screenshots wait for the next frame drawn.
            std::lock_guard lock(s_captureMutex);
            for (auto& promise : captures)
                s_captureRequests.push_back(std::move(promise));
            return true;
        }
        vkResetFences(s_device, 1, &f.inFlight);

        if (upload)
        {
            EnsureSource(w, h);
            EnsureStaging(f, VkDeviceSize(w) * h * 4);
            memcpy(f.stagingMapped, pixels.data(), size_t(w) * h * 4);
        }

        VkCommandBuffer cmd = f.cmd;
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);

        VkImage source = gpuImage;
        VkImageLayout sourceLayout = VK_IMAGE_LAYOUT_GENERAL;
        if (gpuImage)
        {
            // Written by the renderer's earlier submission: a memory barrier
            // orders this read after it (and its next write after this read).
            VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
        }
        else if (haveSource)
        {
            // The CPU path's frame, uploaded now or (a redraw) still there
            // from an earlier frame; it is left in TRANSFER_SRC between frames.
            source = s_source;
            sourceLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            if (upload)
            {
                // After the other frame in flight's reads (blit, AA, scaling).
                Barrier(cmd, s_source, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);
                VkBufferImageCopy copy{};
                copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                copy.imageExtent = { w, h, 1 };
                vkCmdCopyBufferToImage(cmd, f.staging, s_source, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
                Barrier(cmd, s_source, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            }
        }

        VkImage dst = s_swapImages[imageIndex];
        VkBuffer captureBuffer = VK_NULL_HANDLE;
        VkDeviceMemory captureMemory = VK_NULL_HANDLE;
        VkDeviceSize captureSize = 0;
        int captureIndex = -1;
        std::vector<CompareShot> compareShots;  // NFSMW_UPSCALE_COMPARE
        // Letterbox to the source aspect ratio; or, on a screen of another
        // shape with Aspect Ratio on Auto, fill it (video/picture_fit): the
        // game draws that frame with its view widened (a wider screen, Hor+)
        // or made taller (a narrower one, Vert+) by the same factor
        // (game/ultrawide). (The frame's shape, not the image's: at a
        // non-uniform internal resolution such as 3x2 they differ.)
        float frameW = float(aspectW ? aspectW : w), frameH = float(aspectH ? aspectH : h);
        int32_t dw = int32_t(frameW), dh = int32_t(frameH);
        float frameScaleX = 1.0f, frameScaleY = 1.0f;  // the view scales this frame is shown for
        if (haveSource)
        {
            // A new frame decides (and tells the game for the next ones); a
            // redraw keeps the shape its frame was rendered with. Only with
            // the hooks in the recompiled code (a stale ppc/ lacks them, or
            // the Vert+ ones).
            PictureFit fit;
            if (upload || s_lastFreshImage)
            {
                const bool fillMode = settings::GetInt(settings::Id::AspectRatio) == int32_t(settings::AspectMode::Auto);
                fit = FitPicture(s_swapExtent.width, s_swapExtent.height, frameW, frameH, fillMode && game::HooksActive(),
                    fillMode && game::TallHooksActive());
                game::SetAspectScale(fit.scaleX, fit.scaleY);
                s_lastScaleX = fit.scaleX;
                s_lastScaleY = fit.scaleY;
            }
            else
                fit = PlacePicture(s_swapExtent.width, s_swapExtent.height, frameW, frameH, s_lastScaleX, s_lastScaleY);
            dw = fit.width;
            dh = fit.height;
            frameScaleX = fit.scaleX;
            frameScaleY = fit.scaleY;
        }
        // Under Hor+ and Vert+ the post-processing keeps to each side of the
        // 16:9 band's edge (video/band_filter.h): the frame's band and the
        // strips past it are filtered as if each were the whole image.
        // NFSMW_BAND_FILTER=0 filters across it as before (an A/B switch).
        static const bool s_bandFilter = [] {
            const char* v = std::getenv("NFSMW_BAND_FILTER");
            const bool on = !(v && v[0] == '0');
            if (!on)
                fprintf(stderr, "[video] NFSMW_BAND_FILTER=0: anti-aliasing and scaling read across the 16:9 band's edge\n");
            return on;
        }();
        const video::FrameBand band = video::BandOf(w, h, frameScaleX, frameScaleY);
        const bool bandOn = s_bandFilter && (band.x0 > 0 || band.x1 < int32_t(w) || band.y0 > 0 || band.y1 < int32_t(h));
        int32_t dx = (int32_t(s_swapExtent.width) - dw) / 2, dy = (int32_t(s_swapExtent.height) - dh) / 2;
        // The picture without its letterbox bars, for a screenshot.
        VkRect2D pictureRect{};
        {
            int32_t x0 = std::max(dx, 0), y0 = std::max(dy, 0);
            int32_t x1 = std::min(dx + dw, int32_t(s_swapExtent.width)), y1 = std::min(dy + dh, int32_t(s_swapExtent.height));
            if (x1 > x0 && y1 > y0)
                pictureRect = { { x0, y0 }, { uint32_t(x1 - x0), uint32_t(y1 - y0) } };
        }
        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        const bool post = haveSource && EnsurePostProcessing();
        if (post || !haveSource)
        {
            // Draw into the swapchain image: the scaled game frame (if any),
            // then the UI over it.
            waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            VkImageView scaleInput = VK_NULL_HANDLE;
            if (post)
            {
                if (!gpuImage)
                    Barrier(cmd, s_source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                else
                {
                    VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
                    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
                    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        0, 1, &mb, 0, nullptr, 0, nullptr);
                }
                VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
                vci.image = source;
                vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
                vci.format = gpuImage ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_B8G8R8A8_UNORM;  // the renderer's front image / s_source
                vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                vkCreateImageView(s_device, &vci, nullptr, &f.sourceView);
                scaleInput = f.sourceView;
                if (s_timestamps)
                {
                    // From when the work before it is done (the renderer's frame).
                    vkCmdResetQueryPool(cmd, s_timestamps, s_frameIndex * 3, 3);
                    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s_timestamps, s_frameIndex * 3);
                    f.timed = true;
                }

                if (AaEnabled())
                {
                    EnsureAaImage(w, h);
                    Barrier(cmd, s_post.aaImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT,
                        VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
                    VkDescriptorImageInfo in{ s_post.sampler, f.sourceView, VK_IMAGE_LAYOUT_GENERAL };
                    VkDescriptorImageInfo out{ VK_NULL_HANDLE, s_post.aaView, VK_IMAGE_LAYOUT_GENERAL };
                    VkWriteDescriptorSet writes[2] = {
                        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &in, nullptr, nullptr },
                        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &out, nullptr, nullptr },
                    };
                    struct
                    {
                        int32_t width, height;
                        float subpixel;
                        uint32_t bandOn;
                        int32_t band[4];
                    } pc = { int32_t(w), int32_t(h), 0.5f, bandOn ? 1u : 0u, { band.x0, band.x1, band.y0, band.y1 } };
                    static_assert(sizeof(pc) == 32);
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_post.aa);
                    s_post.pushDescriptorSet(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_post.aaLayout, 0, 2, writes);
                    vkCmdPushConstants(cmd, s_post.aaLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
                    vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
                    Barrier(cmd, s_post.aaImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT,
                        VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                    scaleInput = s_post.aaView;
                }
                if (f.timed)
                    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s_timestamps, s_frameIndex * 3 + 1);
            }

            // Brightness and the 360's display gamma: false at Brightness 0
            // with the gamma off, and then the scale pass below is exactly as
            // before.
            const bool tone = scaleInput && PrepareTone(cmd);
            // The pass that draws the picture (video/upscale.h): FSR 1's
            // EASU first, outside the swapchain image's pass.
            VkPipeline rcas = VK_NULL_HANDLE;
            upscale::Pass pass = scaleInput ? ChoosePass(tone, w, h, dw, dh, rcas) : upscale::Pass::Bicubic;
            static uint64_t abCount = 0;
            if (s_upscaleAb && pass == upscale::Pass::Fsr1 && (abCount++ & 1))
                pass = upscale::Pass::Bicubic;
            f.pass = pass;
            if (scaleInput && pass == upscale::Pass::Fsr1)
                RecordEasu(cmd, scaleInput, w, h, dw, dh, band, bandOn);
            Barrier(cmd, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            VkRenderingAttachmentInfoKHR color{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR };
            color.imageView = s_swapViews[imageIndex];
            color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;  // black bars
            color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            if (!haveSource)  // the installer, before the game: a dark backdrop
                color.clearValue.color = { { 0.02f, 0.025f, 0.035f, 1.0f } };
            VkRenderingInfoKHR ri{ VK_STRUCTURE_TYPE_RENDERING_INFO_KHR };
            ri.renderArea = { { 0, 0 }, s_swapExtent };
            ri.layerCount = 1;
            ri.colorAttachmentCount = 1;
            ri.pColorAttachments = &color;
            s_context.cmdBeginRendering(cmd, &ri);
            if (scaleInput)
            {
                DrawPicture(cmd, pass, rcas, tone, scaleInput, w, h, band, bandOn, dx, dy, dw, dh, Sharpen());
                if (!captures.empty() && pictureRect.extent.width)
                {
                    // The game's picture alone: pause the pass, copy it out,
                    // and carry on with the UI over it.
                    s_context.cmdEndRendering(cmd);
                    gameCapture = RecordRegionCapture(cmd, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, pictureRect);
                    VkRenderingAttachmentInfoKHR resume = color;
                    resume.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                    VkRenderingInfoKHR again = ri;
                    again.pColorAttachments = &resume;
                    s_context.cmdBeginRendering(cmd, &again);
                }
            }
            // NFSMW_LOG_COVERS=1: the picture as drawn, before the UI (the
            // settings menu is a dark box of its own), checked for a dark box
            // inside it (CoverCheckRecord, read back a frame later).
            if (game::covers::Enabled() && haveSource)
            {
                s_context.cmdEndRendering(cmd);
                CoverCheckRecord(cmd, dst, s_frameIndex, 0.0f);
                VkRenderingAttachmentInfoKHR resume = color;
                resume.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                VkRenderingInfoKHR again = ri;
                again.pColorAttachments = &resume;
                s_context.cmdBeginRendering(cmd, &again);
            }
            if (uiFrame)
                ui::Draw(cmd);
            s_context.cmdEndRendering(cmd);
            if (f.timed)
                vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s_timestamps, s_frameIndex * 3 + 2);
            // NFSMW_CHECK_PRESENT_SEC=<s>: from s seconds after the first
            // frame, save 3 presented frames 30 apart as
            // build/present_check_<k>.ppm (what the window shows, after
            // post-processing and the UI). A list (2,6.5,30) saves one frame
            // at each time instead.
            static const std::vector<double> checkTimes = [] {
                std::vector<double> times;
                if (const char* v = std::getenv("NFSMW_CHECK_PRESENT_SEC"))
                    for (const char* p = v; *p;)
                    {
                        times.push_back(std::atof(p));
                        p = strchr(p, ',');
                        if (!p)
                            break;
                        p++;
                    }
                return times;
            }();
            static const auto launch = std::chrono::steady_clock::now();
            static int presents = 0, taken = 0, shots = 0;
            presents++;
            double sinceLaunch = std::chrono::duration<double>(std::chrono::steady_clock::now() - launch).count();
            bool check = checkTimes.size() == 1 ? taken < 3 && presents % 30 == 0 && sinceLaunch >= checkTimes[0]
                                                : size_t(taken) < checkTimes.size() && sinceLaunch >= checkTimes[taken];
            // NFSMW_UI_KEYS "Shot": this frame, named apart from the timed ones
            // (taken even when it can't be saved: the keys after it wait for it).
            bool shot = !check && ui::TakeShot();
            if (shot && !s_swapReadable)
                fprintf(stderr, "[video] NFSMW_UI_KEYS Shot: this swapchain can't be read back\n");
            if (check && !s_swapReadable)
            {
                fprintf(stderr, "[video] NFSMW_CHECK_PRESENT_SEC: this swapchain can't be read back\n");
                taken = int(std::max<size_t>(checkTimes.size(), 3));  // once
            }
            if ((check || shot) && s_swapReadable)
            {
                captureSize = VkDeviceSize(s_swapExtent.width) * s_swapExtent.height * 4;
                VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
                bci.size = captureSize;
                bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                vkCreateBuffer(s_device, &bci, nullptr, &captureBuffer);
                VkMemoryRequirements req;
                vkGetBufferMemoryRequirements(s_device, captureBuffer, &req);
                VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
                ai.allocationSize = req.size;
                ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                vkAllocateMemory(s_device, &ai, nullptr, &captureMemory);
                vkBindBufferMemory(s_device, captureBuffer, captureMemory, 0);
                Barrier(cmd, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                VkBufferImageCopy region{};
                region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                region.imageExtent = { s_swapExtent.width, s_swapExtent.height, 1 };
                vkCmdCopyImageToBuffer(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, captureBuffer, 1, &region);
                VkBufferMemoryBarrier hostRead{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
                hostRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                hostRead.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                hostRead.srcQueueFamilyIndex = hostRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                hostRead.buffer = captureBuffer;
                hostRead.size = VK_WHOLE_SIZE;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hostRead, 0, nullptr);
                Barrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                    VK_ACCESS_TRANSFER_READ_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
                captureIndex = shot ? -2 - shots++ : taken++;
            }
            else
                Barrier(cmd, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
            if (captureIndex >= 0 && scaleInput)
                compareShots = RecordCompare(cmd, tone, scaleInput, w, h, band, bandOn, dx, dy, dw, dh);
            if (post && !gpuImage)
                Barrier(cmd, s_source, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        }
        else
        {
            // No post-processing pipeline: a linear blit, then the UI in a pass
            // of its own. (The transition waits at the acquire's wait stage.)
            Barrier(cmd, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkClearColorValue black{};
            VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdClearColorImage(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
            Barrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);  // the blit after the clear
            VkImageBlit blit{};
            blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            blit.srcOffsets[1] = { int32_t(w), int32_t(h), 1 };
            blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            blit.dstOffsets[0] = { dx, dy, 0 };
            blit.dstOffsets[1] = { dx + dw, dy + dh, 1 };
            vkCmdBlitImage(cmd, source, sourceLayout, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1, &blit, VK_FILTER_LINEAR);
            if (!captures.empty() && pictureRect.extent.width)
                gameCapture = RecordRegionCapture(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, pictureRect);
            if (uiFrame)
            {
                waitStage |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
                Barrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
                VkRenderingAttachmentInfoKHR color{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR };
                color.imageView = s_swapViews[imageIndex];
                color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                VkRenderingInfoKHR ri{ VK_STRUCTURE_TYPE_RENDERING_INFO_KHR };
                ri.renderArea = { { 0, 0 }, s_swapExtent };
                ri.layerCount = 1;
                ri.colorAttachmentCount = 1;
                ri.pColorAttachments = &color;
                s_context.cmdBeginRendering(cmd, &ri);
                ui::Draw(cmd);
                s_context.cmdEndRendering(cmd);
                Barrier(cmd, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
            }
            else
                Barrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                    VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        }
        vkEndCommandBuffer(cmd);

        VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &f.imageAvailable;
        si.pWaitDstStageMask = &waitStage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &s_renderDone[imageIndex];
        std::unique_lock queueLock(s_queueMutex);
        vkQueueSubmit(s_queue, 1, &si, f.inFlight);
        frontLock.unlock();

        VkPresentInfoKHR pi{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &s_renderDone[imageIndex];
        pi.swapchainCount = 1;
        pi.pSwapchains = &s_swapchain;
        pi.pImageIndices = &imageIndex;
        VkPresentIdKHR presentId{ VK_STRUCTURE_TYPE_PRESENT_ID_KHR };
        uint64_t id = 0;
        if (s_presentTiming)
        {
            id = ++s_presentId;
            presentId.swapchainCount = 1;
            presentId.pPresentIds = &id;
            pi.pNext = &presentId;
        }
#if defined(__APPLE__) && !TARGET_OS_IOS
        VkPresentTimeGOOGLE presentTime{};
        VkPresentTimesInfoGOOGLE presentTimes{ VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE };
        if (s_displayTiming)
        {
            presentTime.presentID = s_presentTiming ? uint32_t(id) : ++s_displayTimingId;
            // A new frame, at its vblank's time where present_schedule.h
            // says so. A redraw (menus over a paused game): when ready.
            if (frameVblank && s_showAfterVblankNs > 0)
            {
                const bool locked = GuestVblank().Locked();
                bool wanted, tripped;
                {
                    std::lock_guard lock(s_scheduleMutex);
                    wanted = s_schedule.Wanted(locked);
                    tripped = s_schedule.Tripped();
                    if (frameTarget)
                    {
                        // HFR on a grid (HfrGrid; on a Mac only made up, NFSMW_HFR_GRID_HZ): its refresh.
                        frameDesired = frameTarget;
                        s_scheduled[s_scheduledNext++ % std::size(s_scheduled)] = { presentTime.presentID, frameTarget, false };
                        presentTime.desiredPresentTime = uint64_t(std::max<int64_t>(frameTarget - HostToSteadyNs(), 0));
                    }
                    else if (wanted)
                    {
                        // An in-between picture: its fraction of the vblank after its own.
                        int64_t desired = frameVblank + s_showAfterVblankNs +
                            int64_t(std::llround(double(frameBetween != 0.0f ? frameBetween : frameShift) * GuestVblank().PeriodNs()));
                        // HFR: later while real pictures miss their time, and
                        // never before (or with) the picture asked for last.
                        const int64_t nowNs = NowNs();
                        if (frameBetween != 0.0f)
                            s_hfrActiveUntil.store(nowNs + 1'000'000'000, std::memory_order_relaxed);
                        if (nowNs < s_hfrActiveUntil.load(std::memory_order_relaxed))
                        {
                            desired = std::max(desired + s_hfrAdapt.ExtraNs(), s_hfrLastDesired + kHfrMinGapNs);
                            s_hfrLastDesired = desired;
                        }
                        frameDesired = desired;
                        s_scheduled[s_scheduledNext++ % std::size(s_scheduled)] = { presentTime.presentID, desired, false };
                        presentTime.desiredPresentTime = uint64_t(std::max<int64_t>(desired - HostToSteadyNs(), 0));
                    }
                }
                // A line when it changes (the first few).
                static int last = -1, lines = 0;
                if (int(wanted) != last && lines < 8)
                {
                    lines++;
                    fprintf(stderr, "[vblank] new frames shown %s\n", wanted ? "at their vblank's time"
                        : tripped ? "when ready (the watchdog)"
                        : locked ? "when ready: the vblank is locked to the display"
                        : std::format("when ready: a {:.2f} Hz display", s_displayHz).c_str());
                }
                last = int(wanted);
            }
            // Only a frame shown at a time carries the driver's present
            // times (and, for NFSMW_VBLANK_LOG, every present): the rest are
            // presented exactly as in v0.1.3.
            if (presentTime.desiredPresentTime || s_actualLog)
            {
                presentTimes.swapchainCount = 1;
                presentTimes.pTimes = &presentTime;
                presentTimes.pNext = pi.pNext;
                pi.pNext = &presentTimes;
            }
        }
#endif
        const int64_t ready = NowNs();
        int64_t presented = ready;
        // On a display at a multiple of 60 Hz, a new frame waits for the
        // refresh before its own, so FIFO doesn't show it early (VblankLock::
        // PresentAt; 0 everywhere else). Its GPU work is submitted; the queue
        // is free meanwhile.
        // (No hold of more than 100 ms: three vblanks are the most it gives.)
        int64_t at = frameVblank ? GuestVblank().PresentAt(frameVblank, frameBetween != 0.0f ? frameBetween : frameShift) : 0;
        // HFR on a grid that isn't a multiple of 60 Hz: just after the refresh
        // before the picture's own, so FIFO shows it on that one (a Mac shows
        // it at its time instead: desiredPresentTime above).
        if (frameTarget && !frameDesired)
        {
            double period = 0;
            int64_t grid = 0;
            if (HfrGrid(period, grid))
                at = frameTarget - int64_t(std::llround(period)) + VblankLock::kHoldAfterRefreshNs;
        }
        if (at > ready && at - ready < 100'000'000)
        {
            queueLock.unlock();
#ifdef _WIN32
            platform::SleepUntil(at);  // a high-resolution timer: a 1 ms sleep_until wakes ~1.3 ms late here
#else
            std::this_thread::sleep_until(std::chrono::steady_clock::time_point(std::chrono::nanoseconds(at)));
#endif
            queueLock.lock();
            presented = NowNs();
        }
        VkResult present = vkQueuePresentKHR(s_queue, &pi);
        queueLock.unlock();
        if (s_presentTiming && (present == VK_SUCCESS || present == VK_SUBOPTIMAL_KHR))
        {
            std::lock_guard lock(s_timingMutex);
            s_timings.push_back({ id, s_swapchainGeneration, presented, frameBetween != 0.0f ? 0 : frameVblank, ready, frameBetween, frameDesired });
            s_timingCv.notify_one();
        }
        s_lastPresent = std::chrono::steady_clock::now();
        if (frameBetween != 0.0f)
        {
            // A line every 20 s of in-between pictures at 60 a second.
            if (++s_betweenPresented % 1200 == 0)
                fprintf(stderr, "[hfr] presenter: %llu in-between pictures presented, %llu seen on screen, %llu replaced before they were "
                                "shown\n", (unsigned long long)s_betweenPresented.load(), (unsigned long long)s_betweenShown.load(),
                    (unsigned long long)s_betweenDropped.load());
        }
        if (!captures.empty())
        {
            CapturedFrame frame;
            if (gameCapture.buffer)
            {
                vkWaitForFences(s_device, 1, &f.inFlight, VK_TRUE, UINT64_MAX);
                frame = FinishRegionCapture(gameCapture);
            }
            for (auto& promise : captures)
                promise.set_value(frame);
        }
        if (captureBuffer)
        {
            vkWaitForFences(s_device, 1, &f.inFlight, VK_TRUE, UINT64_MAX);
            void* mapped = nullptr;
            vkMapMemory(s_device, captureMemory, 0, captureSize, 0, &mapped);
            std::string path = captureIndex >= 0 ? std::format("build/present_check_{}.ppm", captureIndex)
                                                 : std::format("build/ui_shot_{}.ppm", -2 - captureIndex);
            if (FILE* out = fopen(path.c_str(), "wb"))
            {
                fprintf(out, "P6\n%u %u\n255\n", s_swapExtent.width, s_swapExtent.height);
                const uint8_t* px = static_cast<const uint8_t*>(mapped);
                std::vector<uint8_t> rgb(size_t(s_swapExtent.width) * s_swapExtent.height * 3);
                bool bgr = s_swapFormat == VK_FORMAT_B8G8R8A8_UNORM || s_swapFormat == VK_FORMAT_B8G8R8A8_SRGB;
                for (size_t i = 0; i < rgb.size() / 3; i++)
                    for (int k = 0; k < 3; k++)
                        rgb[i * 3 + k] = px[i * 4 + (bgr ? 2 - k : k)];
                fwrite(rgb.data(), 1, rgb.size(), out);
                fclose(out);
                fprintf(stderr, "[video] presented frame saved to %s\n", path.c_str());
                gpu::renderer::LogOcclusionCounts(path.c_str());  // NFSMW_OCCLUSION=1: the counts behind it
            }
            else
                fprintf(stderr, "[video] can't save a presented frame to %s: %s\n", path.c_str(), strerror(errno));
            vkUnmapMemory(s_device, captureMemory);
            vkDestroyBuffer(s_device, captureBuffer, nullptr);
            vkFreeMemory(s_device, captureMemory, nullptr);
        }
        if (!compareShots.empty())
        {
            vkWaitForFences(s_device, 1, &f.inFlight, VK_TRUE, UINT64_MAX);
            SaveCompare(compareShots, captureIndex);
        }
        // The next frame's CheckSwapchain builds again (before its acquire).
        if (present == VK_ERROR_OUT_OF_DATE_KHR || present == VK_SUBOPTIMAL_KHR)
            s_swapPolicy.Reported(present == VK_ERROR_OUT_OF_DATE_KHR ? SurfaceResult::OutOfDate : SurfaceResult::Suboptimal);
        s_frameIndex = (s_frameIndex + 1) % FRAMES_IN_FLIGHT;
        return true;
    }

    void SubmitFrontImage(VkImage image, uint32_t width, uint32_t height, uint32_t aspectWidth, uint32_t aspectHeight, float between,
        float shift, int64_t target)
    {
        if (between != 0.0f)
        {
            std::lock_guard lock(s_pendingMutex);
            if (s_betweenNew)
                s_betweenDropped++;
            s_betweenImage = image;
            s_betweenWidth = width;
            s_betweenHeight = height;
            s_betweenAspectW = aspectWidth;
            s_betweenAspectH = aspectHeight;
            s_betweenVblank = s_guestVblank.load(std::memory_order_relaxed);
            s_betweenT = between;
            s_betweenTarget = target;
            s_betweenNew = true;
            return;
        }
        if (!s_window)
            return;
        std::lock_guard lock(s_pendingMutex);
        if (s_pendingNew)
            s_dropped++;
        s_pending.clear();
        s_pendingImage = image;
        s_pendingWidth = width;
        s_pendingHeight = height;
        s_pendingAspectW = aspectWidth;
        s_pendingAspectH = aspectHeight;
        s_pendingNew = true;
        s_pendingVblank = s_guestVblank.load(std::memory_order_relaxed);
        s_pendingShift = shift;
        s_pendingTarget = target;
    }

    void WaitGpuIdle()
    {
        if (s_device)
            WaitIdle();
    }

    PresentStats GetPresentStats()
    {
        return { s_dropped.load(std::memory_order_relaxed), s_repeated.load(std::memory_order_relaxed), s_shown.load(std::memory_order_relaxed) };
    }

    VblankLock& GuestVblank()
    {
        // Today's free-running period, 16667 us, when the display can't be
        // measured. NFSMW_VBLANK_HZ=<30..240> fixes the rate instead (no
        // lock); NFSMW_VBLANK_LOCK=0 keeps the free-running one.
        static VblankLock lock = [] {
            const char* hz = std::getenv("NFSMW_VBLANK_HZ");
            double rate = hz ? std::atof(hz) : 0.0;
            const char* on = std::getenv("NFSMW_VBLANK_LOCK");
            bool fixed = rate >= 30.0 && rate <= 240.0;
            if (fixed)
                fprintf(stderr, "[vblank] fixed at %.4f Hz (NFSMW_VBLANK_HZ)\n", rate);
            else if (on && on[0] == '0')
                fprintf(stderr, "[vblank] lock off (NFSMW_VBLANK_LOCK=0): free-running every 16.667 ms\n");
            return VblankLock(fixed ? int64_t(std::llround(1e9 / rate)) : 16'667'000, !fixed && !(on && on[0] == '0'));
        }();
        // At a multiple of 60 Hz: NFSMW_PRESENT_HOLD=0 presents new frames
        // when ready (the lock alone, for comparison).
        static const bool holdChecked = [] {
            if (const char* hold = std::getenv("NFSMW_PRESENT_HOLD"); hold && hold[0] == '0')
            {
                lock.SetHold(false);
                fprintf(stderr, "[vblank] new frames presented when ready at a multiple of 60 Hz too (NFSMW_PRESENT_HOLD=0)\n");
            }
            return true;
        }();
        (void)holdChecked;
        return lock;
    }

    void NoteGuestVblank(int64_t t)
    {
        s_guestVblank.store(t, std::memory_order_relaxed);
        s_guestVblanks.fetch_add(1, std::memory_order_release);
    }

    bool HfrGrid(double& periodNs, int64_t& gridNs)
    {
        if (GuestVblankDivider() != 1)
            return false;
        if (s_hfrGridForcedHz > 0)
        {
            periodNs = 1e9 / s_hfrGridForcedHz;
            gridNs = 0;
            return true;
        }
        if (!s_hfrGridValid.load(std::memory_order_relaxed) || s_gridHz.load(std::memory_order_relaxed) <= 0)
            return false;
        periodNs = double(s_hfrGridPeriodNs.load(std::memory_order_relaxed));
        gridNs = s_hfrGridNs.load(std::memory_order_relaxed);
        return periodNs > 0;
    }

    bool HfrOffered()
    {
#if defined(__APPLE__) && !TARGET_OS_IOS
        // A 120 Hz screen (present_schedule.h's FastDisplay: ProMotion, or a
        // 120 Hz monitor): a Mac draws in-between pictures nowhere else yet
        // (HfrPicturesPerStep; no refresh grid on a Mac: a 144 Hz monitor
        // shows the game at 60).
        const float hz = s_offeredHz.load(std::memory_order_relaxed);
        return s_displayTiming && hz >= PresentSchedule::kMinFastHz && hz <= PresentSchedule::kMaxFastHz;
#else
        return false;
#endif
    }

    int HfrPicturesPerStep()
    {
        if (GuestVblankDivider() != 1 || s_hfrGridForcedHz > 0)
            return 1;  // Frame Rate 30; a made-up grid (HfrGrid)
        if (const int multiple = GuestVblank().HoldMultiple(); multiple >= 2)
            return std::min(multiple, 4);
#if defined(__APPLE__) && !TARGET_OS_IOS
        if (s_displayTiming && s_showAfterVblankNs > 0)
        {
            const bool locked = GuestVblank().Locked();
            std::lock_guard lock(s_scheduleMutex);
            if (s_schedule.FastDisplay() && s_schedule.Wanted(locked))
                return 2;
        }
#endif
        return 1;
    }

    uint64_t GuestVblanks()
    {
        return s_guestVblanks.load(std::memory_order_acquire);
    }

    float PresentScaleOverride()
    {
#if defined(__APPLE__) && TARGET_OS_IOS
        return s_envPresentScale;
#else
        return 0.0f;
#endif
    }

    int GuestVblankDivider()
    {
        return FrameRate() == 30 ? 2 : 1;  // the setting's, or Auto's (video/frame_rate.h)
    }

    int64_t LastGuestVblank()
    {
        return s_guestVblank.load(std::memory_order_relaxed);
    }

    PostTiming GetPostTiming()
    {
        return { s_postFrames.load(), s_postAaNs.load(), s_postNs.load() };
    }

    const VulkanContext* GetVulkan()
    {
        return s_contextReady ? &s_context : nullptr;
    }

    bool WindowFullscreen()
    {
        return s_fullscreen.Window();
    }

    bool FullscreenWindowDiffers()
    {
        return s_fullscreen.WindowDiffers();
    }

    bool FullscreenLocked()
    {
#if defined(__APPLE__) && TARGET_OS_IOS
        // An iOS app has the whole screen: windowed, it only showed the
        // status bar over the picture.
        return true;
#else
        return s_gameMode;
#endif
    }

    bool GameMode()
    {
        return s_gameMode;
    }

    void FrameRegion(uint32_t& width, uint32_t& height)
    {
        width = height = 0;
        uint64_t extent = s_regionExtent.load(std::memory_order_relaxed);
        uint32_t ew = uint32_t(extent >> 32), eh = uint32_t(extent);
        if (!s_window || ew == 0 || eh == 0)
            return;
        // As the presenter fits a new frame, without asking for the hooks:
        // the internal resolution doesn't change when they first run.
        const bool fillMode = settings::GetInt(settings::Id::AspectRatio) == int32_t(settings::AspectMode::Auto);
        const PictureFit fit = FitPicture(ew, eh, 1280.0f, 720.0f, fillMode, fillMode);
        width = uint32_t(fit.width);
        height = uint32_t(fit.height);
    }

    bool WindowFocused()
    {
        return s_focused.load(std::memory_order_relaxed);
    }

    bool WindowVisible()
    {
        return s_visible.load(std::memory_order_relaxed);
    }

    std::future<CapturedFrame> CaptureFrame()
    {
        std::promise<CapturedFrame> promise;
        std::future<CapturedFrame> future = promise.get_future();
        if (!s_window)
        {
            promise.set_value({});
            return future;
        }
        std::lock_guard lock(s_captureMutex);
        s_captureRequests.push_back(std::move(promise));
        return future;
    }

    void Shutdown()
    {
        if (s_device)
            WaitIdle();
        if (s_uiReady)
        {
            ui::Shutdown();
            s_uiReady = false;
        }
        hid::Shutdown();  // a rumble under way would otherwise carry on
        s_swapchainUse.Retire();  // the present waiter is off the swapchain, and stays off
        // Then the title bar's styling lets go of the window (Retire first:
        // nothing presents to it while it is taken apart), then SDL frees it.
#if defined(__APPLE__) && !TARGET_OS_IOS
        platform::macos::ForgetWindow(s_window);
#endif
        if (s_window)
            SDL_DestroyWindow(s_window);
        // No SDL_Quit: the process exits next, and the game's audio thread
        // may still be feeding its stream (SDL_Quit freed it under it).
    }
}

namespace video
{
    uint64_t PictureCoverHits()
    {
        return s_pictureCoverHits.load(std::memory_order_relaxed);
    }
}
