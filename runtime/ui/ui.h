// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The runtime's own screens, drawn with Dear ImGui over the game's image:
// the installer (before the game starts), the settings menu (F1, Back +
// Start on a controller, or a three-finger tap on a touchscreen) and the
// performance overlay.
//
//   - The presenter owns the frame: it calls BeginFrame() before recording,
//     and Draw() inside the dynamic-rendering pass that writes the swapchain
//     image, after the game's image.
//   - While a menu is open the UI owns the keyboard, mouse and controller;
//     the game gets neutral input (CapturingInput()).
//   - Touch reaches ImGui as the mouse SDL makes of the first finger: a tap
//     is a click, and on iOS a drag scrolls a list. While the settings menu
//     is open a finger's press waits until it can't be the start of a
//     three-finger tap (a quarter of a second at most); under a modal (the
//     installer), where the tap does nothing, it doesn't wait.
//   - ImGui is main-thread only. The Vulkan backend submits to the queue the
//     renderer shares (font uploads), so Draw() and Initialize() hold the
//     presenter's queue mutex around it.
#pragma once
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_video.h>
#include <vulkan/vulkan.h>

namespace ui
{
    struct VulkanTarget
    {
        VkInstance instance;
        VkPhysicalDevice physical;
        VkDevice device;
        uint32_t queueFamily;
        VkQueue queue;
        std::mutex* queueMutex;  // held around every use of the queue
        uint32_t apiVersion;     // the instance's
        VkFormat format;         // the swapchain's
    };

    // Main thread, once the swapchain exists. False: no UI (the game still runs).
    bool Initialize(SDL_Window* window, const VulkanTarget& target);
    // The swapchain was recreated in another format.
    void SetFormat(VkFormat format);
    // The swapchain's size when it isn't the window's pixels (iOS's present
    // scale: Screen Resolution or NFSMW_PRESENT_SCALE); 0, 0: the window's
    // (the default).
    void SetFramebufferSize(uint32_t width, uint32_t height);
    // Main thread, with the device idle.
    void Shutdown();

    // Main thread, every frame just before polling events: scripted test
    // keys (NFSMW_UI_KEYS), and the time of the poll, by which BeginFrame
    // judges a touch press held back for a three-finger tap. On iOS it also
    // keeps the system's three-finger editing gestures off.
    void Tick();
    // Main thread: NFSMW_UI_KEYS asked for a capture of the next presented
    // frame ("Shot"); true once per request.
    bool TakeShot();

    // Main thread: every SDL event, before the game's input sees it. Opens
    // and closes the settings menu (keys, controller chord, three-finger
    // tap), and feeds the UI while it has input.
    void ProcessEvent(const SDL_Event& event);

    // Any thread: a menu is open and owns the input.
    bool CapturingInput();

    // The UI needs frames even while the game presents none (a menu or the
    // installer is showing).
    bool WantsFrames();

    // Main thread, once per presented frame, after the frame's events and
    // before recording it: lays out this frame. False: nothing to draw
    // (don't call Draw).
    bool BeginFrame();
    // Records the frame laid out by BeginFrame(), inside a dynamic-rendering
    // pass on the swapchain image (any viewport; it sets its own).
    void Draw(VkCommandBuffer cmd);

    // The presenter showed a new game frame (the performance overlay counts them).
    void NoteGameFrame();

    // Any thread: a one-line notice at the bottom of the screen for a few
    // seconds (a settings file that didn't load, a save that failed, where a
    // bug report went). Several take turns.
    void Toast(std::string text, double seconds = 8.0);
    // Any thread: takes down the toast with this text, showing or waiting
    // its turn, once it no longer holds (a controller connected). None:
    // nothing.
    void WithdrawToast(std::string text);

    // A full-screen page drawn every frame instead of the game until it is
    // cleared (the installer). Call from the main thread; `draw` runs inside
    // BeginFrame().
    void SetModal(std::function<void()> draw);
    bool HasModal();
    // Initialize() succeeded: the UI draws (a modal set without it would
    // hold the game's input with nothing on screen).
    bool Ready();

    // This version's CHANGELOG.md entry, compiled in ("" without one).
    std::string_view WhatsNewEntry();
    // Main thread: the one-time "What's new" note ("Updated to v<version>"
    // and WhatsNewEntry()) as a modal panel over the game until the player
    // closes it (A, B, Start, Esc or OK). Nothing without an entry.
    void ShowWhatsNew(std::string_view version);

    void OpenSettings();
    void CloseSettings();  // saves any change
    bool SettingsOpen();

    // Screens (settings_menu.cpp, perf_overlay.cpp, installer_screen.cpp).
    void DrawSettings();
    void ResetSettingsMenu();  // on opening: focus the first row
    void DrawPerfOverlay();

    // Shared look (ui.cpp).
    SDL_Window* GetWindow();
    float Scale();         // 1 at 720 points of window height
    void PushAccentText();  // ImGui::PushStyleColor(ImGuiCol_Text, accent)
    void PopAccentText();
    // Large accent heading; `hint`, if any, right-aligned on its baseline.
    void TitleText(const char* text, const char* hint = nullptr);
    // The top of a line of text in the current font whose capitals centre
    // on `y`, to the pixel (a label in a row's highlight).
    float CentredTextTop(float y);
    // Puts the next row of buttons on the window's bottom edge, so it stays
    // put whatever the text above it runs to.
    void ButtonRowAtBottom();

    // Spacing: one scale for our own screens, in points at 720 points of
    // window height (times Scale()). Headings, text, frames, highlights and
    // separators start at a window's content edge; text inside a frame or a
    // highlight keeps kSpaceL from its edge (a row's label, the arrow at a
    // row's end, a list entry, a button's or a tab's label: FramePadding.x),
    // and text under a list of rows lines up with their labels.
    constexpr float kSpaceXS = 4.0f;   // between the lines of one entry
    constexpr float kSpaceS = 8.0f;    // between related things; a compact box's padding
    constexpr float kSpaceM = 12.0f;   // between groups; the room either side of a value
    constexpr float kSpaceL = 16.0f;   // text inside a frame or highlight; a floating box
                                       // from the screen's edge
}
