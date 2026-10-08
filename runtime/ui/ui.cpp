// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See ui.h.
#include <stdafx.h>
#include "ui.h"

#include <kernel/xam.h>
#include <report/report.h>
#include <user/settings.h>

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>
#if defined(__APPLE__) && TARGET_OS_IOS
#include <imgui_internal.h>  // DragToScroll: the window under a touch, its scroll and the active item
#include <platform/ios_ui.h>
#endif

#include <SDL3/SDL.h>

#include <deque>

namespace ui
{
    namespace
    {
        // Roboto Medium (Apache 2.0), embedded by CMake from thirdparty/imgui/fonts.
        const unsigned char kFont[] = {
#include "ui_font.inc"
        };
        constexpr float kFontSize = 21.0f;  // at 720 points of window height
        // Its capitals are 1456 units tall, of the 2400 (hhea ascender 1900,
        // descender 500) that ImGui scales to the font size: another font
        // needs its own (OS/2 sCapHeight), or CentredTextTop is off by up to
        // a pixel.
        constexpr float kCapHeight = 1456.0f / 2400.0f;

        SDL_Window* s_window = nullptr;
        VulkanTarget s_target{};
        bool s_ready = false;
        bool s_rendererReady = false;
        bool s_frameBuilt = false;
        bool s_settingsOpen = false;
        std::function<void()> s_modal;
        std::atomic<bool> s_capturing{ false };
        int s_shots = 0;  // NFSMW_UI_KEYS captures not yet taken (main thread)

        float s_scale = 1.0f;
        // Toasts come from any thread (a crash notice, the hang watchdog, a
        // bug report being saved) and take turns: one shows, the rest wait,
        // and one that is waiting cuts the showing one short (after 5 s).
        struct QueuedToast
        {
            std::string text;
            double seconds;
        };
        std::mutex s_toastMutex;
        std::string s_toast;  // showing; empty: none
        std::chrono::steady_clock::time_point s_toastFrom{}, s_toastUntil{};
        std::deque<QueuedToast> s_toastQueue;

        // Under s_toastMutex: the toast to show now ("" for none).
        const std::string& CurrentToastLocked()
        {
            auto now = std::chrono::steady_clock::now();
            if (!s_toast.empty() && now >= s_toastUntil)
                s_toast.clear();
            if (s_toast.empty() && !s_toastQueue.empty())
            {
                s_toast = std::move(s_toastQueue.front().text);
                s_toastFrom = now;
                s_toastUntil = now + std::chrono::milliseconds(int64_t(s_toastQueue.front().seconds * 1000));
                s_toastQueue.pop_front();
            }
            if (!s_toast.empty() && !s_toastQueue.empty())
                s_toastUntil = std::min(s_toastUntil, std::max(s_toastFrom + std::chrono::seconds(5), now));
            return s_toast;
        }

        void DrawToast(const std::string& text)
        {
            float s = s_scale;
            ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y - kSpaceL * s),
                ImGuiCond_Always, ImVec2(0.5f, 1.0f));
            // As wide as the screen allows but for kSpaceL each side (as far
            // as it keeps from the bottom), so the bug report's one line fits
            // on the Deck's 1280x800 too (at 80% of the width it took two).
            const float maxWidth = viewport->WorkSize.x - 2 * kSpaceL * s;
            ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(maxWidth, FLT_MAX));
            // A slim bar: one line of it fits under the settings panel
            // (16:9 and 16:10), so a toast over the open menu hides none of it.
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kSpaceL * s, kSpaceS * s));
            constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove;
            if (ImGui::Begin("##toast", nullptr, flags))
            {
                // (A window-local x: the text wraps where the window's
                // right padding starts at its widest.)
                ImGui::PushTextWrapPos(maxWidth - kSpaceL * s);
                ImGui::TextUnformatted(text.c_str());
                ImGui::PopTextWrapPos();
            }
            ImGui::End();
            ImGui::PopStyleVar();
        }

        float s_appliedScale = 0.0f;
        uint32_t s_framebufferW = 0, s_framebufferH = 0;  // SetFramebufferSize
        ImGuiStyle s_baseStyle;

        // Back + Start on one controller toggles the settings menu; both must
        // be released before the chord can fire again.
        struct Chord
        {
            bool back = false, start = false, fired = false;
        };
        std::unordered_map<SDL_JoystickID, Chord> s_chords;

        // A three-finger tap on a touchscreen (an iPhone's or iPad's, the
        // Steam Deck's) opens and closes the settings menu, as F1 does: the
        // fingers land within kTapLandNs of the first, all have lifted
        // kTapLiftNs after it, and none moves more than kTapSlop points.
        // Trackpads don't count (SDL reports them as indirect), nor do the
        // touches SDL makes up for a mouse or a pen. Times are the events'
        // (SDL ticks), not when a frame got round to them.
        constexpr Uint64 kTapLandNs = 250'000'000, kTapLiftNs = 400'000'000;
        constexpr float kTapSlop = 24.0f;
        // A finger still listed this long after the touch began lifted
        // without our seeing it: start over rather than let it spoil every
        // later tap.
        constexpr Uint64 kTouchStaleNs = 10'000'000'000;

        struct Touches
        {
            std::unordered_map<SDL_FingerID, ImVec2> down;  // fingers on the screen: where each landed (points)
            int landed = 0;      // fingers that have landed since the screen was last clear
            Uint64 since = 0;    // when the first of them landed
            SDL_FingerID first = 0;  // that one: the finger SDL makes the mouse, whose press may be held
            bool tap = false;    // it can still be a three-finger tap
            bool multi = false;  // a second finger landed in time: the first one's press was dropped
        };
        Touches s_touches;

        // SDL also makes the first finger on the screen the mouse
        // (SDL_TOUCH_MOUSEID), which is how ImGui sees touch: a tap is a
        // click. While the settings menu has the input that finger's press
        // waits here until the touch can't be a three-finger tap (no second
        // finger in time, it moved, it lifted alone) and then goes to ImGui;
        // if a second finger lands in time it is dropped, with the rest of
        // that finger's mouse events, so the tap that closes the menu presses
        // nothing in it. Under a modal (the installer) the tap does nothing,
        // so nothing waits there. (The game never sees a touch: it reads
        // controllers and the keyboard only.)
        std::vector<SDL_Event> s_heldPress;
        // When ui::Tick ran, just before this frame's events were polled
        // (SDL ticks; main thread).
        Uint64 s_pollAt = 0;

        bool ScreenClear(Uint64 now)
        {
            return s_touches.down.empty() || now > s_touches.since + kTouchStaleNs;
        }

        void ReleaseHeldPress(bool deliver)
        {
            // (A menu closed meanwhile: nothing to give it to.)
            if (deliver && s_capturing.load(std::memory_order_relaxed))
                for (const SDL_Event& e : s_heldPress)
                    ImGui_ImplSDL3_ProcessEvent(&e);
            s_heldPress.clear();
        }

        // The touch can no longer be a three-finger tap: a press held for it
        // is a press after all (unless a second finger already dropped it).
        void NotATap()
        {
            s_touches.tap = false;
            ReleaseHeldPress(!s_touches.multi);
        }

        // A finger event. True: a three-finger tap has just ended.
        bool TrackFinger(const SDL_TouchFingerEvent& f)
        {
            if (f.touchID == SDL_MOUSE_TOUCHID || f.touchID == SDL_PEN_TOUCHID || SDL_GetTouchDeviceType(f.touchID) != SDL_TOUCH_DEVICE_DIRECT)
                return false;
            int w = 0, h = 0;
            if (s_window)
                SDL_GetWindowSize(s_window, &w, &h);
            const ImVec2 at(f.x * float(w), f.y * float(h));
            Touches& t = s_touches;
            switch (f.type)
            {
            case SDL_EVENT_FINGER_DOWN:
                if (ScreenClear(f.timestamp))
                {
                    t.down.clear();
                    t.landed = 0;
                    t.since = f.timestamp;
                    t.first = f.fingerID;
                    t.tap = true;
                    t.multi = false;
                }
                t.down[f.fingerID] = at;
                t.landed++;
                if (t.landed > 3 || f.timestamp > t.since + kTapLandNs)
                    NotATap();
                else if (t.landed >= 2 && t.tap && !t.multi)
                {
                    t.multi = true;
                    ReleaseHeldPress(false);
                }
                return false;
            case SDL_EVENT_FINGER_MOTION:
                if (auto it = t.down.find(f.fingerID); it != t.down.end() && t.tap &&
                    std::hypot(at.x - it->second.x, at.y - it->second.y) > kTapSlop)
                    NotATap();
                return false;
            case SDL_EVENT_FINGER_UP:
            case SDL_EVENT_FINGER_CANCELED:
            {
                auto it = t.down.find(f.fingerID);
                if (it == t.down.end())
                    return false;
                if (f.type == SDL_EVENT_FINGER_CANCELED && f.fingerID == t.first)
                {
                    // The system took the touch (an alert, Siri, a gesture
                    // of its own): no tap, and no click either. A press still
                    // held for it is dropped with its release, which SDL
                    // sends just before the cancel (SDL_touch.c), so the row
                    // under the finger isn't pressed. (One ImGui already has
                    // can't be taken back.)
                    t.tap = false;
                    ReleaseHeldPress(false);
                }
                else if (f.type == SDL_EVENT_FINGER_CANCELED || f.timestamp > t.since + kTapLiftNs)
                    NotATap();
                t.down.erase(it);
                if (!t.down.empty())
                    return false;
                // The screen is clear: the touch is over.
                const bool tapped = t.tap && t.landed == 3;
                NotATap();
                t.multi = false;
                return tapped;
            }
            default:
                return false;
            }
        }

        // A mouse event while the settings menu has the input. True: it is
        // the touch finger's and is held back or dropped (above), not for
        // ImGui now.
        bool HoldTouchMouse(const SDL_Event& e)
        {
            const bool button = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP;
            if ((button ? e.button.which : e.motion.which) != SDL_TOUCH_MOUSEID)
                return false;
            if (!s_heldPress.empty())
            {
                s_heldPress.push_back(e);
                return true;
            }
            // A finger landing on a clear screen (its SDL_EVENT_FINGER_DOWN
            // comes just after this): it might be the first of three.
            if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT && ScreenClear(e.button.timestamp))
            {
                s_heldPress.push_back(e);
                return true;
            }
            return s_touches.multi && !ScreenClear(e.common.timestamp);
        }

#if defined(__APPLE__) && TARGET_OS_IOS
        // ImGui scrolls a list with a wheel or its scrollbar, and a
        // touchscreen has neither: there a one-finger drag in a list (the
        // settings' rows, the installer's images) scrolls it, the content
        // following the finger, and the row it started on isn't pressed.
        constexpr float kDragScrollSlop = 10.0f;  // points a press moves before it is a drag
        ImGuiID s_dragScrollWindow = 0;           // the list the finger pressed in (0: none)
        bool s_dragScrolling = false;

        // After NewFrame, before the screens lay out (the scroll lands this frame).
        void DragToScroll()
        {
            ImGuiContext& g = *ImGui::GetCurrentContext();
            const ImGuiIO& io = g.IO;
            static const ImGuiID self = ImHashStr("##drag-to-scroll");
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                s_dragScrollWindow = 0;
                s_dragScrolling = false;
                // As the mouse wheel picks one: the window under the finger,
                // or the nearest one around it that scrolls.
                if (io.MouseSource == ImGuiMouseSource_TouchScreen)
                    for (ImGuiWindow* w = g.HoveredWindow; w; w = (w->Flags & ImGuiWindowFlags_ChildWindow) ? w->ParentWindow : nullptr)
                        if (w->ScrollMax.y > 0.0f && !(w->Flags & ImGuiWindowFlags_NoScrollWithMouse))
                        {
                            s_dragScrollWindow = w->ID;
                            break;
                        }
            }
            ImGuiWindow* window = s_dragScrollWindow ? ImGui::FindWindowByID(s_dragScrollWindow) : nullptr;
            if (!window || !ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                if (g.ActiveId == self)
                    ImGui::ClearActiveID();
                s_dragScrollWindow = 0;
                s_dragScrolling = false;
                return;
            }
            float dy = io.MouseDelta.y;
            if (!s_dragScrolling)
            {
                if (!ImGui::IsMouseDragging(ImGuiMouseButton_Left, kDragScrollSlop))
                    return;
                // A drag on the scrollbar stays the scrollbar's (its grab
                // moves the other way).
                if (g.ActiveId == ImGui::GetWindowScrollbarID(window, ImGuiAxis_Y))
                {
                    s_dragScrollWindow = 0;
                    return;
                }
                s_dragScrolling = true;
                dy = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f).y;  // all the way from the press
                // The drag takes the press from the row it started on, and
                // while it lasts no row it passes over lights up or takes it
                // (as with a scrollbar's grab).
                ImGui::SetActiveID(self, window);
            }
            ImGui::KeepAliveID(self);
            ImGui::SetScrollY(window, window->Scroll.y - dy);
        }
#endif

        const ImVec4 kAccent(1.00f, 0.66f, 0.12f, 1.00f);

        void CheckVk(VkResult result)
        {
            if (result != VK_SUCCESS)
                fprintf(stderr, "[ui] Vulkan error %d\n", int(result));
        }

        void UpdateCapture()
        {
            s_capturing.store(s_settingsOpen || bool(s_modal), std::memory_order_release);
            // A touch press is held only for the settings menu: once it
            // closes, or a modal (the installer) covers it, one still held
            // was meant for nothing on screen. It is dropped, as SetModal
            // drops the mouse, and the rest of that touch reaches a modal as
            // it comes (a release without a press, which ImGui ignores).
            // Held on, it would reach ImGui after its own release, or ahead
            // of a later touch's.
            if (!s_settingsOpen || s_modal)
                s_heldPress.clear();
        }

        void ApplyStyle()
        {
            ImGuiStyle& style = ImGui::GetStyle();
            style.WindowRounding = 6.0f;
            style.ChildRounding = 4.0f;
            style.FrameRounding = 4.0f;
            style.PopupRounding = 4.0f;
            style.GrabRounding = 3.0f;
            style.TabRounding = 4.0f;
            style.ScrollbarRounding = 4.0f;
            style.WindowBorderSize = 1.0f;
            style.FrameBorderSize = 0.0f;
            style.WindowPadding = ImVec2(18, 16);
            // A button's or a tab's label keeps the inset a row's label
            // keeps inside its highlight (ui.h).
            style.FramePadding = ImVec2(kSpaceL, 7);
            style.ItemSpacing = ImVec2(10, 8);
            style.ItemInnerSpacing = ImVec2(8, 6);
            style.ScrollbarSize = 12.0f;
            style.WindowTitleAlign = ImVec2(0.5f, 0.5f);
            style.SelectableTextAlign = ImVec2(0.0f, 0.5f);

            ImVec4* c = style.Colors;
            const ImVec4 text(0.93f, 0.94f, 0.96f, 1.00f);
            const ImVec4 dim(0.56f, 0.59f, 0.64f, 1.00f);
            const ImVec4 panel(0.055f, 0.065f, 0.085f, 0.95f);
            const ImVec4 raised(0.11f, 0.125f, 0.15f, 1.00f);
            auto accent = [](float a) { return ImVec4(kAccent.x, kAccent.y, kAccent.z, a); };
            c[ImGuiCol_Text] = text;
            c[ImGuiCol_TextDisabled] = dim;
            c[ImGuiCol_WindowBg] = panel;
            c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
            c[ImGuiCol_PopupBg] = ImVec4(0.07f, 0.08f, 0.10f, 0.98f);
            c[ImGuiCol_Border] = ImVec4(1, 1, 1, 0.08f);
            c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
            c[ImGuiCol_FrameBg] = raised;
            c[ImGuiCol_FrameBgHovered] = ImVec4(0.16f, 0.18f, 0.21f, 1.00f);
            c[ImGuiCol_FrameBgActive] = ImVec4(0.20f, 0.22f, 0.26f, 1.00f);
            c[ImGuiCol_TitleBg] = panel;
            c[ImGuiCol_TitleBgActive] = panel;
            c[ImGuiCol_TitleBgCollapsed] = panel;
            // A list that goes on past the panel shows it: the track's
            // whole length, and a grab that reads at a glance (at 15% on no
            // track, a player missed the last two Video rows).
            c[ImGuiCol_ScrollbarBg] = ImVec4(1, 1, 1, 0.06f);
            c[ImGuiCol_ScrollbarGrab] = ImVec4(1, 1, 1, 0.35f);
            c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1, 1, 1, 0.5f);
            c[ImGuiCol_ScrollbarGrabActive] = accent(0.8f);
            c[ImGuiCol_CheckMark] = kAccent;
            c[ImGuiCol_SliderGrab] = accent(0.85f);
            c[ImGuiCol_SliderGrabActive] = kAccent;
            c[ImGuiCol_Button] = raised;
            c[ImGuiCol_ButtonHovered] = accent(0.30f);
            c[ImGuiCol_ButtonActive] = accent(0.55f);
            c[ImGuiCol_Header] = accent(0.22f);
            c[ImGuiCol_HeaderHovered] = accent(0.16f);
            c[ImGuiCol_HeaderActive] = accent(0.32f);
            c[ImGuiCol_Separator] = ImVec4(1, 1, 1, 0.10f);
            c[ImGuiCol_Tab] = ImVec4(0, 0, 0, 0);
            c[ImGuiCol_TabHovered] = accent(0.25f);
            c[ImGuiCol_TabSelected] = accent(0.22f);
            c[ImGuiCol_TabSelectedOverline] = kAccent;
            c[ImGuiCol_TabDimmed] = ImVec4(0, 0, 0, 0);
            c[ImGuiCol_TabDimmedSelected] = accent(0.15f);
            c[ImGuiCol_PlotLines] = kAccent;
            c[ImGuiCol_PlotHistogram] = kAccent;
            c[ImGuiCol_NavCursor] = kAccent;
            c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.55f);
            c[ImGuiCol_TextSelectedBg] = accent(0.35f);
            s_baseStyle = style;
        }

        // Sizes follow the window's height, so the menu reads the same on a
        // laptop and across the room on a 4K TV.
        void UpdateScale()
        {
            float height = ImGui::GetIO().DisplaySize.y;
            s_scale = std::clamp(height > 0 ? height / 720.0f : 1.0f, 0.75f, 4.0f);
            if (std::abs(s_scale - s_appliedScale) < 0.01f)
                return;
            s_appliedScale = s_scale;
            ImGuiStyle& style = ImGui::GetStyle();
            style = s_baseStyle;
            style.ScaleAllSizes(s_scale);
            style.FontSizeBase = kFontSize;
            style.FontScaleMain = s_scale;
        }

        bool InitRenderer(VkFormat format)
        {
            ImGui_ImplVulkan_InitInfo info{};
            info.ApiVersion = s_target.apiVersion;
            info.Instance = s_target.instance;
            info.PhysicalDevice = s_target.physical;
            info.Device = s_target.device;
            info.QueueFamily = s_target.queueFamily;
            info.Queue = s_target.queue;
            info.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE + 8;
            // Buffers and textures are recycled after ImageCount UI frames;
            // the presenter keeps 2 frames in flight.
            info.MinImageCount = 2;
            info.ImageCount = 3;
            info.UseDynamicRendering = true;
            info.PipelineInfoMain.PipelineRenderingCreateInfo = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR };
            info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
            info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &format;  // copied
            info.CheckVkResultFn = CheckVk;
            std::lock_guard lock(*s_target.queueMutex);
            return ImGui_ImplVulkan_Init(&info);
        }
    }

    bool Initialize(SDL_Window* window, const VulkanTarget& target)
    {
        if (s_ready)
            return true;
        s_window = window;
        s_target = target;
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;  // no imgui.ini: layouts are fixed
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        // Controller first: the focused item always shows, so A (Enter) works
        // on a screen's first frame without a d-pad press to reveal it.
        io.ConfigNavCursorVisibleAlways = true;

        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;
        io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(kFont), int(sizeof(kFont)), kFontSize, &config);
        ApplyStyle();

        if (!ImGui_ImplSDL3_InitForVulkan(window))
        {
            fprintf(stderr, "[ui] SDL3 backend failed\n");
            ImGui::DestroyContext();
            return false;
        }
        if (!InitRenderer(target.format))
        {
            fprintf(stderr, "[ui] Vulkan backend failed\n");
            ImGui_ImplSDL3_Shutdown();
            ImGui::DestroyContext();
            return false;
        }
        s_rendererReady = true;
        s_ready = true;
#if defined(__APPLE__) && TARGET_OS_IOS
        // Else iOS takes the three-finger tap for its edit menu (and a
        // three-finger swipe for undo), cancelling the touches.
        platform::ios::DisableSystemEditGestures(window);
        fprintf(stderr, "[ui] ready (Dear ImGui %s): View + Menu or a three-finger tap opens the settings\n", IMGUI_VERSION);
#else
        fprintf(stderr, "[ui] ready (Dear ImGui %s): F1 or Back + Start opens the settings\n", IMGUI_VERSION);
#endif
        return true;
    }

    void SetFormat(VkFormat format)
    {
        if (!s_ready || format == s_target.format)
            return;
        s_target.format = format;
        ImGui_ImplVulkan_PipelineInfo pipeline{};
        pipeline.PipelineRenderingCreateInfo = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR };
        pipeline.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
        pipeline.PipelineRenderingCreateInfo.pColorAttachmentFormats = &format;
        ImGui_ImplVulkan_CreateMainPipeline(&pipeline);
    }

    void SetFramebufferSize(uint32_t width, uint32_t height)
    {
        s_framebufferW = width;
        s_framebufferH = height;
    }

    void Shutdown()
    {
        if (!s_ready)
            return;
        if (s_settingsOpen)
            CloseSettings();
        {
            std::lock_guard lock(*s_target.queueMutex);
            ImGui_ImplVulkan_Shutdown();
        }
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        s_ready = false;
        s_rendererReady = false;
        s_modal = nullptr;
        s_heldPress.clear();
        UpdateCapture();
    }

    void ProcessEvent(const SDL_Event& e)
    {
        if (!s_ready)
            return;
        bool forward = s_capturing.load(std::memory_order_relaxed);
        switch (e.type)
        {
        case SDL_EVENT_KEY_DOWN:
        {
            // F1, and on macOS Cmd+, (F1 is the brightness key on Mac keyboards).
            bool toggle = e.key.key == SDLK_F1;
#ifdef __APPLE__
            toggle |= e.key.key == SDLK_COMMA && (e.key.mod & SDL_KMOD_GUI);
#endif
            if (toggle && !e.key.repeat && !s_modal)
            {
                s_settingsOpen ? CloseSettings() : OpenSettings();
                return;
            }
            break;
        }
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
        {
            bool down = e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
            Chord& chord = s_chords[e.gbutton.which];
            if (e.gbutton.button == SDL_GAMEPAD_BUTTON_BACK)
                chord.back = down;
            else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_START)
                chord.start = down;
            else
                break;
            if (!chord.back && !chord.start)
                chord.fired = false;
            if (s_modal)
                break;
            if (chord.back && chord.start && !chord.fired)
            {
                chord.fired = true;
                s_settingsOpen ? CloseSettings() : OpenSettings();
                return;
            }
            // In the menu, Start or Back alone closes it (and counts as the
            // chord, so the other button of a Back + Start press can't reopen it).
            if (down && s_settingsOpen && !chord.fired)
            {
                chord.fired = true;
                CloseSettings();
                return;
            }
            break;
        }
        case SDL_EVENT_GAMEPAD_ADDED:
        case SDL_EVENT_GAMEPAD_REMOVED:
            s_chords.erase(e.gdevice.which);
            forward = true;  // the backend re-opens its controllers
            break;
        case SDL_EVENT_FINGER_DOWN:
        case SDL_EVENT_FINGER_MOTION:
        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED:
            if (TrackFinger(e.tfinger) && !s_modal)
            {
                fprintf(stderr, "[ui] three-finger tap\n");
                s_settingsOpen ? CloseSettings() : OpenSettings();
            }
            return;  // (ImGui's backend takes touch as the mouse SDL makes of it)
        case SDL_EVENT_MOUSE_MOTION:
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            // Held only where the tap can act: under a modal it toggles
            // nothing, and a touch there is the mouse at once (so on iOS a
            // drag in the installer's list scrolls after its own 10 points).
            if (forward && !s_modal && HoldTouchMouse(e))
                return;
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
        case SDL_EVENT_WINDOW_FOCUS_LOST:
        case SDL_EVENT_WINDOW_MOUSE_ENTER:
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            forward = true;
            break;
        default:
            break;
        }
        // Input events only while a menu has the input: nothing drains
        // ImGui's queue while no frame is laid out.
        if (forward)
            ImGui_ImplSDL3_ProcessEvent(&e);
    }

    void Tick()
    {
        s_pollAt = SDL_GetTicksNS();
#if defined(__APPLE__) && TARGET_OS_IOS
        // Every frame: SDL's text input or the Files picker can have the
        // first responder for a while (ios_ui.h). Cheap when nothing changed.
        if (s_ready)
            platform::ios::DisableSystemEditGestures(s_window);
#endif

        // NFSMW_UI_KEYS="3:F1,4:Down,4.5:Right": key presses (SDL key names)
        // at seconds since launch, for testing the menus without a person.
        // FocusLost and FocusGained send the window those events instead (as
        // switching to another window would: Mute in Background). Shot saves
        // the next presented frame as build/ui_shot_<n>.ppm: on the keys'
        // own clock, so a capture lands between two keys (the
        // NFSMW_CHECK_PRESENT_SEC clock starts at the first frame, a second
        // or more later).
        constexpr SDL_Keycode kFocusLost = SDLK_EXTENDED_MASK | 0x7F01, kFocusGained = SDLK_EXTENDED_MASK | 0x7F02,
                              kShot = SDLK_EXTENDED_MASK | 0x7F03;
        struct Key { double time; SDL_Keycode key; };
        static const auto start = std::chrono::steady_clock::now();
        static std::vector<Key> keys = [] {
            std::vector<Key> parsed;
            if (const char* v = std::getenv("NFSMW_UI_KEYS"))
                for (std::string_view rest = v; !rest.empty();)
                {
                    size_t comma = rest.find(',');
                    std::string item(rest.substr(0, comma));
                    rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
                    size_t colon = item.find(':');
                    if (colon == std::string::npos)
                        continue;
                    std::string name = item.substr(colon + 1);
                    SDL_Keycode key = name == "FocusLost" ? kFocusLost : name == "FocusGained" ? kFocusGained
                        : name == "Shot"                                                         ? kShot
                                                                                                 : SDL_GetKeyFromName(name.c_str());
                    if (key != SDLK_UNKNOWN)
                        parsed.push_back({ std::atof(item.c_str()), key });
                    else
                        fprintf(stderr, "[ui] NFSMW_UI_KEYS: unknown key %s\n", item.c_str() + colon + 1);
                }
            std::sort(parsed.begin(), parsed.end(), [](const Key& a, const Key& b) { return a.time < b.time; });
            return parsed;
        }();
        static size_t next = 0;
        // A Shot stalls the main thread while the frame is read back and
        // written (15 MB at 3440x1440), and the keys due meanwhile would all
        // go in the next frame: a Down right after a page change was lost, and a
        // Shot followed closely by F1 caught the menu open. So the keys after
        // a Shot wait until it is taken and then keep their gaps from it: the
        // list runs later by the captures' time (only theirs, so it stays
        // with NFSMW_INPUT_SCRIPT's clock until the first Shot).
        static double lag = 0.0;
        static double shotDue = -1.0;  // a Shot not taken yet: its time in the list
        if (next >= keys.size() || !s_window)
            return;
        double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (shotDue >= 0.0)
        {
            if (s_shots > 0)
                return;
            lag = std::max(lag, t - shotDue);
            shotDue = -1.0;
        }
        for (; next < keys.size() && keys[next].time + lag <= t; next++)
        {
            if (keys[next].key == kShot)
            {
                s_shots++;
                shotDue = keys[next++].time;
                return;
            }
            if (keys[next].key == kFocusLost || keys[next].key == kFocusGained)
            {
                SDL_Event e{};
                e.type = keys[next].key == kFocusLost ? SDL_EVENT_WINDOW_FOCUS_LOST : SDL_EVENT_WINDOW_FOCUS_GAINED;
                e.window.timestamp = SDL_GetTicksNS();
                e.window.windowID = SDL_GetWindowID(s_window);
                SDL_PushEvent(&e);
                fprintf(stderr, "[ui] NFSMW_UI_KEYS: window focus %s\n", keys[next].key == kFocusLost ? "lost" : "gained");
                continue;
            }
            for (bool down : { true, false })
            {
                SDL_Event e{};
                e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
                e.key.timestamp = SDL_GetTicksNS();
                e.key.windowID = SDL_GetWindowID(s_window);
                e.key.key = keys[next].key;
                e.key.scancode = SDL_GetScancodeFromKey(keys[next].key, nullptr);
                e.key.down = down;
                SDL_PushEvent(&e);
            }
        }
    }

    bool TakeShot()
    {
        if (s_shots == 0)
            return false;
        s_shots--;
        return true;
    }

    void Toast(std::string text, double seconds)
    {
        if (text.empty())
            return;
        std::replace(text.begin(), text.end(), '\n', ' ');
        std::replace(text.begin(), text.end(), '\r', ' ');
        std::lock_guard lock(s_toastMutex);
        if (text == s_toast)
        {
            s_toastUntil = std::max(s_toastUntil, std::chrono::steady_clock::now() + std::chrono::milliseconds(int64_t(seconds * 1000)));
            return;
        }
        for (const QueuedToast& queued : s_toastQueue)
            if (queued.text == text)
                return;
        if (s_toastQueue.size() >= 4)
            s_toastQueue.pop_front();
        s_toastQueue.push_back({ std::move(text), seconds });
    }

    void WithdrawToast(std::string text)
    {
        if (text.empty())
            return;
        std::replace(text.begin(), text.end(), '\n', ' ');  // as Toast keeps it
        std::replace(text.begin(), text.end(), '\r', ' ');
        std::lock_guard lock(s_toastMutex);
        if (text == s_toast)
            s_toast.clear();  // the next one waiting, if any, shows from the next frame
        std::erase_if(s_toastQueue, [&](const QueuedToast& queued) { return queued.text == text; });
    }

    bool CapturingInput()
    {
        return s_capturing.load(std::memory_order_acquire);
    }

    bool WantsFrames()
    {
        if (!s_ready)
            return false;
        if (s_settingsOpen || s_modal)
            return true;
        std::lock_guard lock(s_toastMutex);
        return !s_toast.empty() || !s_toastQueue.empty();
    }

    bool BeginFrame()
    {
        s_frameBuilt = false;
        if (!s_ready)
            return false;
        // A touch press held back for a three-finger tap goes to ImGui once
        // a second finger can no longer land in time (a finger resting on a
        // row, say), without waiting for another touch event. Judged here,
        // after the frame's events, and by when they were polled rather than
        // by now: SDL takes touches in only inside the poll (on iOS, UIKit
        // hands them over there), so a finger that landed in time came in
        // with it (bar the last few ms, while UIKit hands it over), however
        // long the frame has taken since (a swapchain rebuild after a video
        // setting changed).
        if (!s_heldPress.empty() && s_pollAt > s_heldPress.front().common.timestamp + kTapLandNs)
            NotATap();
        bool perf = settings::GetBool(settings::Id::PerformanceOverlay);
        std::string toastText;
        {
            std::lock_guard lock(s_toastMutex);
            toastText = CurrentToastLocked();
        }
        bool toast = !toastText.empty();
        if (!s_modal && !s_settingsOpen && !perf && !toast)
            return false;
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        // SDL gave the window's pixels at the screen's scale; a smaller
        // swapchain (iOS's present scale) needs its own for the viewport,
        // clip rectangles and font density.
        if (ImGuiIO& io = ImGui::GetIO(); s_framebufferW && io.DisplaySize.x > 0 && io.DisplaySize.y > 0)
            io.DisplayFramebufferScale = ImVec2(float(s_framebufferW) / io.DisplaySize.x, float(s_framebufferH) / io.DisplaySize.y);
        UpdateScale();
        ImGui::NewFrame();
        // Panels and toasts lay out in the work area: the window's safe area,
        // clear of an iPhone's Dynamic Island and home indicator (elsewhere
        // the whole window, as before).
        if (SDL_Rect safe; s_window && SDL_GetWindowSafeArea(s_window, &safe) && safe.w > 0 && safe.h > 0)
        {
            ImGuiViewport* viewport = ImGui::GetMainViewport();
            viewport->WorkPos = ImVec2(viewport->Pos.x + float(safe.x), viewport->Pos.y + float(safe.y));
            viewport->WorkSize = ImVec2(float(safe.w), float(safe.h));
        }
#if defined(__APPLE__) && TARGET_OS_IOS
        DragToScroll();
#endif
        if (s_modal)
        {
            auto modal = s_modal;  // it may clear itself
            modal();
        }
        else
        {
            if (s_settingsOpen)
                DrawSettings();
            else if (perf)
                DrawPerfOverlay();
        }
        if (toast)
            DrawToast(toastText);
        ImGui::Render();
        ImDrawData* data = ImGui::GetDrawData();
        s_frameBuilt = data && data->CmdLists.Size > 0;  // (CmdListsCount is obsolete in 1.92)
        return s_frameBuilt;
    }

    void Draw(VkCommandBuffer cmd)
    {
        if (!s_frameBuilt)
            return;
        s_frameBuilt = false;
        // Font uploads submit and wait on the shared queue.
        std::lock_guard lock(*s_target.queueMutex);
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    }

    void SetModal(std::function<void()> draw)
    {
        if (draw && !s_modal && s_ready)
        {
            ImGui::GetIO().ClearInputKeys();
            ImGui::GetIO().ClearInputMouse();
        }
        s_modal = std::move(draw);
        if (s_modal && s_settingsOpen)
            CloseSettings();
        UpdateCapture();
    }

    bool HasModal()
    {
        return bool(s_modal);
    }

    bool Ready()
    {
        return s_ready;
    }

    void OpenSettings()
    {
        if (!s_ready || s_settingsOpen)
            return;
        s_settingsOpen = true;
        ResetSettingsMenu();
        // Keys released while the UI wasn't listening would still read as held.
        ImGui::GetIO().ClearInputKeys();
        ImGui::GetIO().ClearInputMouse();
        UpdateCapture();
        // The game as it is now, before it pauses: a bug report's screenshot.
        report::NoteMenuOpened();
        // As the 360's Guide does: the game pauses (a race doesn't carry on
        // under the menu).
        XamNotifySystemUi(true);
    }

    void CloseSettings()
    {
        if (!s_settingsOpen)
            return;
        s_settingsOpen = false;
        UpdateCapture();
        report::NoteMenuClosed();
        XamNotifySystemUi(false);
        if (settings::Dirty() && !settings::Save())
            Toast(settings::LastError());
    }

    bool SettingsOpen()
    {
        return s_settingsOpen;
    }

    SDL_Window* GetWindow()
    {
        return s_window;
    }

    float Scale()
    {
        return s_scale;
    }

    void PushAccentText()
    {
        ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
    }

    void PopAccentText()
    {
        ImGui::PopStyleColor();
    }

    void TitleText(const char* text, const char* hint)
    {
        ImVec2 start = ImGui::GetCursorPos();
        float right = start.x + ImGui::GetContentRegionAvail().x;
        ImGui::PushFont(nullptr, kFontSize * 1.55f);
        // Ascent over size is the font's, whatever the size.
        const ImFontBaked* baked = ImGui::GetFontBaked();
        float ascent = baked->Size > 0 ? baked->Ascent / baked->Size : 0.0f;
        float titleSize = ImGui::GetFontSize();
        PushAccentText();
        ImGui::TextUnformatted(text);
        PopAccentText();
        ImGui::PopFont();
        if (hint)
        {
            ImVec2 after = ImGui::GetCursorPos();
            ImGui::SetCursorPos(ImVec2(right - ImGui::CalcTextSize(hint).x, start.y + ascent * (titleSize - ImGui::GetFontSize())));
            ImGui::TextDisabled("%s", hint);
            ImGui::SetCursorPos(after);
        }
    }

    float CentredTextTop(float y)
    {
        // ImGui centres a line box and truncates, which can leave text a
        // pixel off a highlight's centre. The baseline is where ImGui put the
        // glyphs ('H' ends on it).
        ImFontBaked* baked = ImGui::GetFontBaked();
        const ImFontGlyph* h = baked->FindGlyph('H');
        float baseline = h ? h->Y1 : baked->Ascent;
        return std::round(y - (baseline - ImGui::GetFontSize() * kCapHeight * 0.5f));
    }

    void ButtonRowAtBottom()
    {
        ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), ImGui::GetWindowHeight() - ImGui::GetStyle().WindowPadding.y - ImGui::GetFrameHeight()));
    }
}
