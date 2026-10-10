// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Window and presentation (Phase 5, milestone 1). An SDL3 window with a
// Vulkan swapchain (MoltenVK on macOS). Each XE_SWAP hands over the game's
// front buffer (a texture in guest physical memory); it is untiled and
// converted on the CPU and scaled into the window.
#pragma once
#include <cstdint>
#include <future>
#include <mutex>
#include <vector>

#include <vulkan/vulkan.h>

namespace video
{
    // Main thread only (macOS requires windows there). Returns false if no
    // window could be created; the game then runs headless.
    bool Initialize();

    // Any thread (the command processor): the front buffer to show next.
    // `fetch` is its texture fetch constant (6 dwords, host order).
    void SubmitFrontBuffer(const uint32_t fetch[6], uint32_t width, uint32_t height);

    // Any thread (the renderer): present this image (RGBA8, VK_IMAGE_LAYOUT_GENERAL),
    // written by a queue submission made before this call. `aspectWidth` x
    // `aspectHeight`: the frame's shape when the image is at another
    // resolution (internal resolution scaling; 0: the image's own).
    void SubmitFrontImage(VkImage image, uint32_t width, uint32_t height, uint32_t aspectWidth = 0, uint32_t aspectHeight = 0);

    // Wait until the queue is idle (resource replacement).
    void WaitGpuIdle();

    // Since launch, for the [perf] line: game frames replaced here before
    // they were shown (dropped), refreshes a game frame stayed on screen past
    // the vblanks the game gave it (repeated; needs present timing), and game
    // frames seen on screen (shown; likewise).
    struct PresentStats
    {
        uint64_t dropped, repeated, shown;
    };
    PresentStats GetPresentStats();

    // The guest vblank's clock (gpu/command_processor.cpp's Vsync thread),
    // locked to the display when presents are timed (video/vblank_lock.h).
    class VblankLock& GuestVblank();
    // Vsync thread: a guest vblank fired at `t` (ns, steady clock).
    void NoteGuestVblank(int64_t t);
    // Any thread: guest vblanks so far, and when the last one fired.
    uint64_t GuestVblanks();
    // Display refreshes per guest vblank: 1, or 2 at Frame Rate 30 (the
    // game then shows a frame every other refresh, and each guest vblank
    // is two world steps: game/frame_time.cpp).
    int GuestVblankDivider();
    int64_t LastGuestVblank();

    // NFSMW_GPU_TIMING=1: GPU time of the post-processing of the game's
    // frames (AA, then the scale into the swapchain image with the UI; that
    // part also holds any wait for the image), totals so far.
    struct PostTiming
    {
        uint64_t frames, aaNs, totalNs;
    };
    PostTiming GetPostTiming();

    // Any thread: `image` (given to SubmitFrontImage) is about to be
    // destroyed. Returns once the presenter holds no reference it could still
    // record; the caller then waits for the GPU (WaitGpuIdle) and destroys it.
    void ReleaseFrontImage(VkImage image);

    // Main thread: handle window events and present the latest front buffer.
    // Returns false when the user closes the window.
    bool RunFrame();

    void Shutdown();

    // Shared with the renderer (gpu/renderer), which records on the command
    // processor thread. Every vkQueueSubmit/vkQueuePresentKHR holds queueMutex.
    struct VulkanContext
    {
        VkInstance instance;
        VkPhysicalDevice physical;
        VkDevice device;
        uint32_t queueFamily;
        VkQueue queue;
        std::mutex* queueMutex;
        PFN_vkGetMemoryHostPointerPropertiesEXT getMemoryHostPointerProperties;  // nullptr: no host-memory import
        PFN_vkCmdBeginRenderingKHR cmdBeginRendering;
        PFN_vkCmdEndRenderingKHR cmdEndRendering;
        // NFSMW_SHADING_RATE=<w>x<h> (an experiment: what reduced shading,
        // as foveation would do at the edges, could save at most): the game's
        // draws shade once per w x h pixels. 0x0: off or unsupported.
        uint32_t shadingRateW = 0, shadingRateH = 0;
        // NFSMW_OCCLUSION=1 on a device with precise occlusion queries: the
        // game's occlusion queries (the sun glare) are counted on the GPU
        // (gpu/renderer.cpp). resetQueryPool: host query reset (nullptr: the
        // renderer resets them in its command buffers).
        bool occlusionCounting = false;
        PFN_vkResetQueryPool resetQueryPool = nullptr;
    };

    // nullptr when running headless.
    const VulkanContext* GetVulkan();

    // Any thread, holding queueMutex (`queueLock`), before a vkQueueSubmit or
    // vkQueuePresentKHR: waits while the app is in the background. iOS lets
    // no GPU work start then (Metal fails the command buffer, and MoltenVK
    // then loses the device for good), so the game's GPU work stops there
    // until the app is active again.
    void WaitForeground(std::unique_lock<std::mutex>& queueLock);

    // The whole game stops while the app isn't the active one (iOS: from
    // "will resign active" to "did become active", SDL's app events), and
    // carries on where it stopped. SuspendGame stops every guest clock
    // (cpu/guest_time.h), then lets no GPU work start and drains the queue,
    // then pauses the audio output. ResumeGame lets GPU work start, then
    // guest time move on, then the audio play. Idempotent: UIKit reports
    // each transition twice. The main thread (the app events come there,
    // and RunFrame stops and restarts the input), or NFSMW_TEST_SUSPEND's
    // thread when headless (no main loop): never both.
    void SuspendGame();
    void ResumeGame();
    // The same thread: 2 s after a resume, logs the clocks, vblanks and
    // audio frames since, against guest time. RunFrame calls it each frame.
    void SuspendAudit();
    // NFSMW_TEST_SUSPEND (report/watchdog.cpp), any thread: SDL app event
    // `type` (SDL_EVENT_WILL_ENTER_BACKGROUND...), for the main thread to
    // hand to the same handler SDL would, twice as UIKit does, first thing
    // in its next frame.
    void TestAppEvent(uint32_t type);

    // Any thread: the window has the keyboard focus (true when headless).
    bool WindowFocused();

    // Any thread: the window can be seen (not minimized, hidden or covered;
    // true when headless). The hang watchdog doesn't count time without it
    // (nor time suspended, which it reads from guesttime itself).
    bool WindowVisible();

    // A game frame as the window shows it, without the UI over it and
    // without letterbox bars: RGB8, top row first.
    struct CapturedFrame
    {
        std::vector<uint8_t> rgb;
        uint32_t width = 0, height = 0;
    };
    // Any thread (a bug report): the next frame the main thread draws, once
    // drawn. Empty when there is no game frame to capture (headless, before
    // the game's first frame, a swapchain that can't be read back).
    std::future<CapturedFrame> CaptureFrame();

    // SteamOS Game Mode (gamescope) and iOS: the window is always fullscreen
    // there.
    bool FullscreenLocked();

    // SteamOS Game Mode (gamescope; NFSMW_GAME_MODE=1|0 says otherwise), as
    // detected when the window was made. No desktop is on screen there, so a
    // system file dialog can't be seen either.
    bool GameMode();

    // NFSMW_PRESENT_SCALE (iOS): the present scale it sets for this run, over
    // Settings > Display > Screen Resolution; 0 when it isn't set.
    float PresentScaleOverride();

    // The area of the swapchain a 1280x720 frame covers now (in Fill mode the
    // whole of a wider or narrower screen, or its 4:3 box when it is
    // narrower than that; else its 16:9 box); 0x0 without a window.
    void FrameRegion(uint32_t& width, uint32_t& height);
}
