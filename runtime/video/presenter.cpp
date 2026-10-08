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
#include "picture_fit.h"
#include "frame_rate.h"
#include <cpu/guest_thread.h>
#include <cpu/guest_time.h>
#include "vblank_lock.h"
#include <ui/ui.h>
#if defined(__APPLE__) && TARGET_OS_IOS
#include <platform/ios_ui.h>
#endif
#include <user/settings.h>
#include <report/report.h>
#include <platform/thermal.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
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
        } s_frames[FRAMES_IN_FLIGHT];
        // NFSMW_GPU_TIMING=1: 3 timestamps per frame slot around the
        // post-processing (before, after AA, after the scale and UI pass).
        VkQueryPool s_timestamps = VK_NULL_HANDLE;
        double s_timestampPeriod = 1.0;
        std::atomic<uint64_t> s_postFrames{ 0 }, s_postAaNs{ 0 }, s_postNs{ 0 };
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
        // suspension (hid::Suspend), and a window resize waits for the
        // resume to rebuild the swapchain (no GPU work before it).
        bool s_inputSuspended = false;
        bool s_swapchainStale = false;
        // Bug-report screenshots waiting for the next frame drawn (CaptureFrame).
        std::mutex s_captureMutex;
        std::vector<std::promise<CapturedFrame>> s_captureRequests;
        // Window settings as applied (settings::Generation() tells when to look again).
        uint64_t s_settingsGeneration = ~0ull;
        bool s_fullscreen = false;         // the window's state
        bool s_fullscreenSetting = false;  // the setting as last applied
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
        // MoltenVK's), though the spec leaves that to the caller. Destroying
        // a swapchain waits until no wait is on it.
        bool s_presentTiming = false;
        PFN_vkWaitForPresentKHR s_waitForPresent = nullptr;
        uint64_t s_presentId = 0;  // main thread: the last present's
        struct PresentTiming
        {
            uint64_t id;
            VkSwapchainKHR swapchain;
            int64_t presented;  // vkQueuePresentKHR (ns, steady clock)
            int64_t vblank;     // a new game frame: the guest vblank it followed; 0: a redraw
        };
        // Never destroyed: the waiter is detached and blocks on them, and
        // glibc's pthread_cond_destroy waits for blocked waiters, so an exit
        // through main's return (no xex, a cancelled install) would hang.
        std::mutex& s_timingMutex = *new std::mutex;
        std::condition_variable& s_timingCv = *new std::condition_variable;
        std::deque<PresentTiming>& s_timings = *new std::deque<PresentTiming>;  // presented, not yet seen on screen
        std::mutex s_waitSwapchainMutex;      // held by the waiter while it waits
        VkSwapchainKHR s_waitSwapchain = VK_NULL_HANDLE;  // the one it may wait on
        FILE* s_timingLog = nullptr;          // NFSMW_VBLANK_LOG=<file>: one CSV line per present

        int64_t NowNs()
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        // Waiter thread: a present was on screen at `onScreen`. `timed`: the
        // wait was under way when it happened (not caught up on afterwards).
        void OnScreen(const PresentTiming& t, int64_t onScreen, bool timed)
        {
            static RefreshFit& refresh = *new RefreshFit;  // never destroyed either (the waiter may be in it at exit)
            static int64_t lastOnScreen = 0, lastVblank = 0;
            VblankLock& vblank = GuestVblank();
            if (timed)
            {
                refresh.Add(onScreen);
                vblank.SetDisplay(refresh.Estimate());
            }
            const RefreshEstimate& display = refresh.Estimate();
            if (s_timingLog)
                fprintf(s_timingLog, "%llu,%lld,%lld,%lld,%d,%d,%.1f,%lld,%.0f\n", (unsigned long long)t.id, (long long)t.vblank,
                    (long long)t.presented, (long long)onScreen, timed, display.valid, display.periodNs, (long long)display.gridNs,
                    display.rmsNs);
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
                // The last frame's time on screen, against the vblanks the
                // game gave it (a gap of more than 0.5 s is a pause, not a repeat).
                double refreshNs = display.valid ? display.periodNs : 1e9 / 60.0;
                int64_t shownFor = std::llround(double(onScreen - lastOnScreen) / refreshNs);
                int64_t vblanks = std::max<int64_t>(1, std::llround(double(t.vblank - lastVblank) / vblank.PeriodNs()));
                if (shownFor > vblanks && shownFor <= 30)
                    s_repeated += uint64_t(shownFor - vblanks);
            }
            lastOnScreen = onScreen;
            lastVblank = t.vblank;
            vblank.Observe(t.vblank, t.presented, onScreen);
        }

        void PresentWaiter()
        {
            SetHostThreadName("nfsmw-present");
            while (true)
            {
                PresentTiming t;
                {
                    std::unique_lock lock(s_timingMutex);
                    s_timingCv.wait(lock, [] { return !s_timings.empty(); });
                    t = s_timings.front();
                }
                int64_t start = NowNs();
                VkResult r = VK_ERROR_OUT_OF_DATE_KHR;
                {
                    std::lock_guard lock(s_waitSwapchainMutex);
                    if (t.swapchain == s_waitSwapchain)
                        r = s_waitForPresent(s_device, t.swapchain, t.id, 50'000'000);
                }
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

        void DestroySwapchain()
        {
            {
                std::lock_guard wait(s_waitSwapchainMutex);  // the waiter is off it after this
                s_waitSwapchain = VK_NULL_HANDLE;
                std::lock_guard lock(s_timingMutex);
                std::erase_if(s_timings, [](const PresentTiming& t) { return t.swapchain == s_swapchain; });
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
            int w, h;
            SDL_GetWindowSizeInPixels(s_window, &w, &h);
            s_swapExtent = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent
                : VkExtent2D{ uint32_t(w), uint32_t(h) };
            if (s_swapExtent.width == 0 || s_swapExtent.height == 0)
                return false;  // minimised
            s_regionExtent.store(uint64_t(s_swapExtent.width) << 32 | s_swapExtent.height, std::memory_order_relaxed);
            // The UI draws to the drawables' size, not the window's pixels
            // (0, 0: they're the window's).
            ui::SetFramebufferSize(s_presentScale < 1.0f ? s_swapExtent.width : 0, s_presentScale < 1.0f ? s_swapExtent.height : 0);

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
            {
                std::lock_guard lock(s_waitSwapchainMutex);
                s_waitSwapchain = s_swapchain;
            }
            if (s_uiReady)
                ui::SetFormat(s_swapFormat);
            static VkExtent2D logged{};
            if (logged.width != s_swapExtent.width || logged.height != s_swapExtent.height)
            {
                if (logged.width)  // the first one is logged with the device
                    fprintf(stderr, "[video] swapchain %ux%u\n", s_swapExtent.width, s_swapExtent.height);
                logged = s_swapExtent;
            }

            vkGetSwapchainImagesKHR(s_device, s_swapchain, &count, nullptr);
            s_swapImages.resize(count);
            vkGetSwapchainImagesKHR(s_device, s_swapchain, &count, s_swapImages.data());
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
layout(push_constant) uniform P { ivec2 size; float subpixel; float pad; } p;

const float kEdgeMin = 0.0312;      // no AA on darker contrast than this
const float kEdgeRelative = 0.125;  // ... or than this fraction of the local maximum

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
vec3 at(vec2 uv) { return textureLod(src, uv, 0.0).rgb; }

void main()
{
    ivec2 pix = ivec2(gl_GlobalInvocationID.xy);
    if (pix.x >= p.size.x || pix.y >= p.size.y)
        return;
    vec2 texel = 1.0 / vec2(p.size);
    vec2 uv = (vec2(pix) + 0.5) * texel;
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
layout(push_constant) uniform P { vec2 size; float sharpen; uint bicubic; } p;

vec3 at(vec2 t) { return textureLod(src, t, 0.0).rgb; }

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
layout(push_constant) uniform P { vec2 size; float sharpen; uint bicubic; } p;

vec3 at(vec2 t) { return textureLod(src, t, 0.0).rgb; }

void main()
{
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

        // The scale pipeline for the swapchain's format (made again if it changes).
        bool EnsureScalePipeline()
        {
            if (s_post.scale && s_post.scaleFormat == s_swapFormat)
                return true;
            if (s_post.scale)
                vkDestroyPipeline(s_device, s_post.scale, nullptr);
            s_post.scale = VK_NULL_HANDLE;
            VkPipelineShaderStageCreateInfo stages[2] = {
                { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, s_post.vs, "main", nullptr },
                { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, s_post.fs, "main", nullptr },
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
            gpci.layout = s_post.scaleLayout;
            if (vkCreateGraphicsPipelines(s_device, VK_NULL_HANDLE, 1, &gpci, nullptr, &s_post.scale) != VK_SUCCESS)
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
            VkPushConstantRange aaRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, 16 }, scaleRange{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 };
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
            fprintf(stderr, "[video] post-processing: %s, %s scaling%s%s\n", AaEnabled() ? "anti-aliasing" : "no anti-aliasing",
                Bicubic() ? "bicubic" : "linear", Bicubic() ? std::format(", sharpen {:.2f}", Sharpen()).c_str() : "",
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
            // Only when the setting itself changes: the window may have been
            // toggled by the system since (and NFSMW_FULLSCREEN pins the setting).
            bool fullscreen = settings::GetBool(settings::Id::Fullscreen);
            if (fullscreen != s_fullscreenSetting && !FullscreenLocked())
            {
                s_fullscreenSetting = fullscreen;
                if (fullscreen != s_fullscreen)
                {
                    s_fullscreen = fullscreen;
                    bool ok = SDL_SetWindowFullscreen(s_window, fullscreen);
                    fprintf(stderr, "[video] fullscreen %s (setting)%s%s\n", fullscreen ? "on" : "off", ok ? "" : ": ",
                        ok ? "" : SDL_GetError());
                }
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
                CreateSwapchain();
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
        s_fullscreenSetting = settings::GetBool(settings::Id::Fullscreen);
        s_fullscreen = FullscreenLocked() || s_fullscreenSetting;
        s_vsync = settings::GetBool(settings::Id::VSync);
#if defined(__APPLE__) && TARGET_OS_IOS
        // Always fullscreen there (FullscreenLocked), which hides the status
        // bar over the picture. SDL then also defers every screen edge's
        // system gestures, so going home would take two swipes: "0" keeps
        // the one swipe the windowed app had. ("2" defers them, if touch
        // players leave the game by accident.)
        SDL_SetHint(SDL_HINT_IOS_HIDE_HOME_INDICATOR, "0");
#endif
        s_window = SDL_CreateWindow("SpeedBreaker", windowW, windowH,
            SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | (s_fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
        if (!s_window)
        {
            fprintf(stderr, "[video] SDL_CreateWindow: %s\n", SDL_GetError());
            return false;
        }

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
        for (VkPhysicalDevice d : devices)
        {
            uint32_t qc = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(d, &qc, nullptr);
            std::vector<VkQueueFamilyProperties> qs(qc);
            vkGetPhysicalDeviceQueueFamilyProperties(d, &qc, qs.data());
            for (uint32_t i = 0; i < qc; i++)
            {
                VkBool32 present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(d, i, s_surface, &present);
                if ((qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present)
                {
                    s_physical = d;
                    s_queueFamily = i;
                    break;
                }
            }
            if (s_physical)
                break;
        }
        if (!s_physical)
        {
            fprintf(stderr, "[video] no Vulkan device can present to the window\n");
            return false;
        }
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(s_physical, &props);

        // The renderer needs dynamic rendering, and imports guest memory as
        // a buffer where the device can (host-memory import; Turnip, the
        // Steam Frame's driver, can't: the renderer's GPU copy instead).
        std::vector<const char*> devExts = { VK_KHR_SWAPCHAIN_EXTENSION_NAME,
            VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME, VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME,
            VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME, VK_KHR_MULTIVIEW_EXTENSION_NAME, VK_KHR_MAINTENANCE_2_EXTENSION_NAME,
            VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME };
        uint32_t extAvail = 0;
        vkEnumerateDeviceExtensionProperties(s_physical, nullptr, &extAvail, nullptr);
        std::vector<VkExtensionProperties> avail(extAvail);
        vkEnumerateDeviceExtensionProperties(s_physical, nullptr, &extAvail, avail.data());
        bool presentId = false, presentWait = false, hostImport = false, shadingRate = false;
        for (auto& e : avail)
        {
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
        // fades by. On by default where checked (MoltenVK on an M4 Max,
        // RADV on the Steam Machine, Turnip on the Steam Frame: cost within
        // noise); off elsewhere. Needs precise occlusion queries (counting,
        // not boolean); host query reset (core 1.2) is optional. Off, the
        // device is created exactly as without it.
#if defined(__APPLE__) || defined(__linux__)
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
        if (s_presentTiming)
        {
            if (const char* path = std::getenv("NFSMW_VBLANK_LOG"))
                if ((s_timingLog = fopen(path, "w")))
                    fprintf(s_timingLog, "id,vblank,presented,on_screen,timed,fit_valid,period_ns,grid_ns,rms_ns\n");
            std::thread(PresentWaiter).detach();
        }
        fprintf(stderr, "[vblank] %s\n", s_presentTiming ? "presents are timed (VK_KHR_present_wait)"
            : timingOff ? "presents aren't timed (NFSMW_PRESENT_TIMING=0): the guest vblank can't lock to the display"
            : "no present timing (VK_KHR_present_wait) on this device: the guest vblank can't lock to the display");

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
        s_contextReady = true;
        s_settingsGeneration = settings::Generation();
        fprintf(stderr, "[video] %s, swapchain %ux%u\n", props.deviceName, s_swapExtent.width, s_swapExtent.height);
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
                s_fullscreen ? " (fullscreen)" : "", nativeW, nativeH, s_gameMode ? "Game Mode (gamescope)" : "desktop");
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
                uint32_t loader = VK_API_VERSION_1_0;
                vkEnumerateInstanceVersion(&loader);
                report::SetSystemItem("GPU", std::format("{} ({}, {:04x}:{:04x}), {}, Vulkan {}.{}.{} (loader {}.{}.{})",
                    props.deviceName, type, props.vendorID, props.deviceID, driver, VK_API_VERSION_MAJOR(props.apiVersion),
                    VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion), VK_API_VERSION_MAJOR(loader),
                    VK_API_VERSION_MINOR(loader), VK_API_VERSION_PATCH(loader)));
                int windowW = 0, windowH = 0;
                SDL_GetWindowSizeInPixels(s_window, &windowW, &windowH);
                const char* name = SDL_GetDisplayName(SDL_GetDisplayForWindow(s_window));
                report::SetSystemItem("Display", std::format("{} {}x{} at {:.2f} Hz{}; window {}x{} pixels{}, "
                    "swapchain {}x{}, V-Sync {}; SDL video driver {}; {}", name ? name : "?", mode ? mode->w : 0, mode ? mode->h : 0,
                    mode ? mode->refresh_rate : 0.0f, nativeW > 0 ? std::format(", monitor {}x{}", nativeW, nativeH) : std::string(),
                    windowW, windowH, s_fullscreen ? " (fullscreen)" : "",
                    s_swapExtent.width, s_swapExtent.height, s_vsync ? "on" : "off", SDL_GetCurrentVideoDriver(),
                    s_gameMode ? "Game Mode (gamescope)" : "desktop"));
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

    void ReleaseFrontImage(VkImage image)
    {
        std::lock_guard use(s_frontUseMutex);
        std::lock_guard lock(s_pendingMutex);
        if (s_pendingImage == image)
        {
            s_pendingImage = VK_NULL_HANDLE;
            s_pendingNew = !s_pending.empty();
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
        // such as the Auto internal resolution, without a person.
        void ScriptedWindowSizes()
        {
            struct Step { double at; int w, h; };
            static std::vector<Step> steps = [] {
                std::vector<Step> out;
                const char* v = std::getenv("NFSMW_WINDOW_AT");
                for (const char* p = v; p && *p;)
                {
                    Step step{};
                    if (sscanf(p, "%lf:%dx%d", &step.at, &step.w, &step.h) == 3 && step.w > 0 && step.h > 0)
                        out.push_back(step);
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

    bool RunFrame()
    {
        DeliverTestAppEvents();
        ui::Tick();
        platform::thermal::Poll();  // about once a second
        ScriptedWindowSizes();
        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            ui::ProcessEvent(e);
            hid::HandleEvent(e);
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                return false;
            if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
            {
                // No GPU work while the app is inactive: the first frame
                // after the resume rebuilds it.
                if (s_background.load(std::memory_order_acquire))
                    s_swapchainStale = true;
                else
                {
                    WaitIdle();
                    DestroySwapchain();
                    CreateSwapchain();
                }
            }
            else if (e.type == SDL_EVENT_WINDOW_RESIZED)
                RememberWindowSize();
            else if (e.type == SDL_EVENT_WINDOW_FOCUS_GAINED || e.type == SDL_EVENT_WINDOW_FOCUS_LOST)
                s_focused.store(e.type == SDL_EVENT_WINDOW_FOCUS_GAINED, std::memory_order_relaxed);
            else if (e.type == SDL_EVENT_WINDOW_MINIMIZED || e.type == SDL_EVENT_WINDOW_HIDDEN || e.type == SDL_EVENT_WINDOW_OCCLUDED)
                s_visible.store(false, std::memory_order_relaxed);
            else if (e.type == SDL_EVENT_WINDOW_RESTORED || e.type == SDL_EVENT_WINDOW_SHOWN || e.type == SDL_EVENT_WINDOW_EXPOSED ||
                e.type == SDL_EVENT_WINDOW_MAXIMIZED)
                s_visible.store(true, std::memory_order_relaxed);
            else if (e.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN || e.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN)
            {
                // Also when the system toggled it (the green button on macOS).
                s_fullscreen = e.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN;
                if (!FullscreenLocked() && !settings::EnvOverride(settings::Id::Fullscreen))
                    settings::Set(settings::Id::Fullscreen, s_fullscreen);
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
        if (s_swapchainStale)
        {
            s_swapchainStale = false;
            WaitIdle();
            DestroySwapchain();
            CreateSwapchain();
        }
        SuspendAudit();
        hid::SetBlocked(ui::CapturingInput());
        hid::Update();
        ApplySettings();
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
        {
            std::lock_guard lock(s_pendingMutex);
            if (s_pendingNew)
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
            s_lastGameFrame = std::chrono::steady_clock::now();
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
        if (f.timed)
        {
            uint64_t ts[3];
            if (vkGetQueryPoolResults(s_device, s_timestamps, s_frameIndex * 3, 3, sizeof(ts), ts, sizeof(uint64_t),
                    VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ts[2] >= ts[1] && ts[1] >= ts[0])
            {
                s_postFrames++;
                s_postAaNs += uint64_t(double(ts[1] - ts[0]) * s_timestampPeriod);
                s_postNs += uint64_t(double(ts[2] - ts[0]) * s_timestampPeriod);
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
        if (acquire == VK_ERROR_OUT_OF_DATE_KHR)
        {
            WaitIdle();
            DestroySwapchain();
            CreateSwapchain();
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
        // Letterbox to the source aspect ratio; or, on a screen of another
        // shape with Aspect Ratio on Auto, fill it (video/picture_fit): the
        // game draws that frame with its view widened (a wider screen, Hor+)
        // or made taller (a narrower one, Vert+) by the same factor
        // (game/ultrawide). (The frame's shape, not the image's: at a
        // non-uniform internal resolution such as 3x2 they differ.)
        float frameW = float(aspectW ? aspectW : w), frameH = float(aspectH ? aspectH : h);
        int32_t dw = int32_t(frameW), dh = int32_t(frameH);
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
        }
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
                    struct { int32_t width, height; float subpixel, pad; } pc = { int32_t(w), int32_t(h), 0.5f, 0.0f };
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
                VkViewport viewport{ float(dx), float(dy), float(dw), float(dh), 0.0f, 1.0f };
                VkRect2D scissor{ { dx, dy }, { uint32_t(dw), uint32_t(dh) } };
                vkCmdSetViewport(cmd, 0, 1, &viewport);
                vkCmdSetScissor(cmd, 0, 1, &scissor);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_post.scale);
                VkDescriptorImageInfo in{ s_post.sampler, scaleInput, VK_IMAGE_LAYOUT_GENERAL };
                VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 0, 0, 1,
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &in, nullptr, nullptr };
                s_post.pushDescriptorSet(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_post.scaleLayout, 0, 1, &write);
                bool bicubic = Bicubic();
                struct { float width, height, sharpen; uint32_t bicubic; } pc = { float(w), float(h), Sharpen(), bicubic ? 1u : 0u };
                vkCmdPushConstants(cmd, s_post.scaleLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
                vkCmdDraw(cmd, 3, 1, 0, 0);
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
        int64_t presented = NowNs();
        VkResult present = vkQueuePresentKHR(s_queue, &pi);
        queueLock.unlock();
        if (s_presentTiming && (present == VK_SUCCESS || present == VK_SUBOPTIMAL_KHR))
        {
            std::lock_guard lock(s_timingMutex);
            s_timings.push_back({ id, s_swapchain, presented, frameVblank });
            s_timingCv.notify_one();
        }
        s_lastPresent = std::chrono::steady_clock::now();
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
        if (present == VK_ERROR_OUT_OF_DATE_KHR || present == VK_SUBOPTIMAL_KHR)
        {
            WaitIdle();
            DestroySwapchain();
            CreateSwapchain();
        }
        s_frameIndex = (s_frameIndex + 1) % FRAMES_IN_FLIGHT;
        return true;
    }

    void SubmitFrontImage(VkImage image, uint32_t width, uint32_t height, uint32_t aspectWidth, uint32_t aspectHeight)
    {
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
        return lock;
    }

    void NoteGuestVblank(int64_t t)
    {
        s_guestVblank.store(t, std::memory_order_relaxed);
        s_guestVblanks.fetch_add(1, std::memory_order_release);
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
        {
            std::lock_guard wait(s_waitSwapchainMutex);  // the present waiter is off the swapchain
            s_waitSwapchain = VK_NULL_HANDLE;
        }
        if (s_window)
            SDL_DestroyWindow(s_window);
        // No SDL_Quit: the process exits next, and the game's audio thread
        // may still be feeding its stream (SDL_Quit freed it under it).
    }
}
