// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The settings menu, built from settings::Options(): one page per category,
// one row per option. Made for a controller first: the D-pad picks a row,
// left/right changes it, A toggles, LB/RB turn pages, B closes. The mouse
// works too (a click selects a row, its arrows change it, the tabs turn
// pages), and touch as the mouse (on iOS a drag scrolls the rows: ui.cpp).
// Advanced also has Save Bug Report (report/bug_report.cpp), Check for
// Updates (update_dialog.cpp), the build's identity (version and commit) and
// the legal notice. The footer's last button is Quit Game, which asks first
// (quit_game.h; not on iPhone or iPad).
#include <stdafx.h>
#include "ui.h"
#include "quit_game.h"

#include <game/language.h>  // the Language row's note: what the installed files give
#include <report/report.h>
#include <user/settings.h>
#include <hid/key_map.h>     // the Controls page: one row per control, two keys each
#include <hid/wheel.h>       // the Wheel page: the device, its pedals and what they read
#include <user/key_names.h>  // a key's name in the file, when SDL has none
#include <video/frame_rate.h>
#include <video/presenter.h>

#include <imgui.h>
#include <imgui_internal.h>  // ClosePopupToLevel: a dialog left open when the menu closed
#include <SDL3/SDL_keyboard.h>  // a key row's label on this layout

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_timer.h>

namespace ui
{
    namespace
    {
        using settings::Category;
        using settings::Id;

        Category s_category = Category::Display;
        bool s_tabPending = false;   // s_category was changed by LB/RB; the tab bar catches up next frame
        bool s_focusFirstRow = true; // put the controller's cursor on the page's first row
        Id s_described = Id::Count;  // the row the help line describes
        bool s_describedBugReport = false;  // ...or Save Bug Report's
        bool s_describedUpdate = false;     // ...or Check for Updates'
        bool s_opened = true;        // just opened: take focus
        // The Controls page (KeyRow). A row's two keys are cells; the chosen
        // cell is the focused row's, and a wait is for one option of one
        // row (SettingsMenuTakesKey takes the press before ImGui sees it).
        Id s_waiting = Id::Count;    // the Key option a row waits for; Count: no wait
        SDL_Scancode s_refused = SDL_SCANCODE_UNKNOWN;  // a press the wait wouldn't take; the help line says so
        SDL_Scancode s_takenKey = SDL_SCANCODE_UNKNOWN;  // the key a wait took or refused, until its release
        Id s_slotRow = Id::Count;    // the row whose chosen cell s_slot is
        int s_slot = 0;              // 0: the row's first key, 1: its second
        constexpr const char* kPressAKey = "Press a key";
        constexpr const char* kNoKey = "none";
        // The Wheel page's Calibrate row (CalibrateRow; hid/wheel.h runs it).
        bool s_describedCal = false;  // the help line describes that row

        void EndWait()
        {
            s_waiting = Id::Count;
            s_refused = SDL_SCANCODE_UNKNOWN;
        }

        constexpr int kCategories = int(Category::Count);

#if SB_QUIT_GAME
        constexpr const char* kQuitPopup = "quit";
        quit::Prompt s_quit;
#endif

        bool Pressed(ImGuiKey key)
        {
            return ImGui::IsKeyPressed(key, true);
        }

        int PageDelta()
        {
            int delta = 0;
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_Q, false))
                delta--;
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_E, false))
                delta++;
            return delta;
        }

        // iPhone and iPad: the Controls page only while a keyboard is
        // connected (nothing to bind without one). The Wheel page, on every
        // device, only while a wheel is plugged in: nothing on it means
        // anything otherwise. Switching the wheel off doesn't take the page
        // away with it (hid/wheel.h's Found against Playing), or the row
        // that switched it off couldn't switch it back on.
        bool PageShown(Category c)
        {
            if (c == Category::Wheel)
                return hid::wheel::Found();
            if (c == Category::Game)  // its one row, Language, isn't shown on iPhone and iPad
                return !settings::Info(Id::GameLanguage).hidden;
#if defined(__APPLE__) && TARGET_OS_IOS
            if (c == Category::Controls)
                return SDL_HasKeyboard();
#endif
            return true;
        }

        // A key's name for a cell: on this keyboard's layout ("Z" on a
        // French Mac for the position the file calls "w"), else SDL's name
        // of the position (the platform's: "Left Option" on a Mac), else the
        // file's. "none" for no key. Main thread.
        std::string KeyLabel(double stored)
        {
            if (stored == double(settings::keynames::kNone))
                return kNoKey;
            if (!(stored >= 1 && stored < double(SDL_SCANCODE_COUNT)))
                return "?";
            SDL_Scancode sc = SDL_Scancode(int(stored));
            const char* name = SDL_GetKeyName(SDL_GetKeyFromScancode(sc, SDL_KMOD_NONE, false));
            if (!name || !name[0])
                name = SDL_GetScancodeName(sc);
            if (!name || !name[0])
                name = settings::keynames::Token(uint16_t(stored));
            return name && name[0] ? name : "?";
        }

        // A key the wait refused, for the help line.
        std::string RefusedName(SDL_Scancode sc)
        {
            std::string name = KeyLabel(double(sc));
            return name == "?" || name == kNoKey ? "That key" : name;
        }

        // The other controls that have any of this row's keys ("D-pad
        // Left"), in page order, each once; "" when none.
        std::string SharedWith(const hid::keymap::Entry& e)
        {
            const double mine[2] = { settings::GetStoredValue(e.id), settings::GetStoredValue(e.id2) };
            std::string text;
            for (const auto& other : hid::keymap::kEntries)
            {
                if (other.control == e.control)
                    continue;
                const double theirs[2] = { settings::GetStoredValue(other.id), settings::GetStoredValue(other.id2) };
                bool shared = false;
                for (double m : mine)
                    for (double th : theirs)
                        shared |= m != double(settings::keynames::kNone) && m == th;
                if (shared)
                    text += (text.empty() ? "" : ", ") + std::string(settings::Info(other.id).label);
            }
            return text;
        }

        // A key cell is as wide as the widest name among the 48 keys, "Press
        // a key" or "none"; a row has two and a gap.
        float KeyCellWidth()
        {
            float width = std::max(ImGui::CalcTextSize(kPressAKey).x, ImGui::CalcTextSize(kNoKey).x);
            for (const auto& o : settings::Options())
                if (o.type == settings::Type::Key)
                    width = std::max(width, ImGui::CalcTextSize(KeyLabel(settings::GetStoredValue(o.id)).c_str()).x);
            return width;
        }

        float KeyCellGap()
        {
            return kSpaceM * Scale();
        }

        // Widest value this option can show, for the value column.
        float ValueWidth(const settings::Option& o)
        {
            float width = 0.0f;
            auto measure = [&](double v) { width = std::max(width, ImGui::CalcTextSize(settings::FormatValue(o.id, v).c_str()).x); };
            switch (o.type)
            {
            case settings::Type::Bool:
                measure(0);
                measure(1);
                break;
            case settings::Type::Enum:
                for (const auto& e : o.entries)
                    measure(e.value);
                break;
            case settings::Type::Key:
                width = 2 * KeyCellWidth() + KeyCellGap();
                break;
            default:
                measure(o.min);
                measure(o.max);
                measure(settings::GetStoredValue(o.id));
                break;
            }
            return width;
        }

        // Every change the menu makes goes to the log (a player's report
        // then says what they changed and when, and tests read it).
        void LogChange(const settings::Option& o, double from, settings::SetResult result)
        {
            const char* how = "";
            switch (result)
            {
            case settings::SetResult::Applied: how = "applied now"; break;
            case settings::SetResult::Deferred: how = "at the next launch"; break;
            case settings::SetResult::Overridden: how = "saved; the environment keeps this run's value"; break;
            case settings::SetResult::Unchanged: return;
            case settings::SetResult::Invalid: how = "invalid"; break;
            }
            fprintf(stderr, "[settings] menu: %s.%s %s -> %s (%s)\n", o.table, o.key, settings::FormatValue(o.id, from).c_str(),
                settings::FormatValue(o.id, settings::GetStoredValue(o.id)).c_str(), how);
        }

        void Change(const settings::Option& o, double stored, int delta, bool wrap)
        {
            double next = settings::StepValue(o.id, stored, delta);
            // A (or a click) on an enum at its last entry starts over.
            if (wrap && next == stored && o.type == settings::Type::Enum && !o.entries.empty())
                next = delta > 0 ? o.entries.front().value : o.entries.back().value;
            if (next != stored)
                LogChange(o, stored, settings::Set(o.id, next));
        }

        // A row the menu shows: not hidden, and Smooth Motion only where the
        // display can show it (video::HfrOffered) or while it's on (so it
        // can be turned off from any screen).
        bool Shown(const settings::Option& o)
        {
            if (o.hidden)
                return false;
            if (o.id == Id::SmoothMotion)
                return video::HfrOffered() || settings::GetStoredValue(o.id) != 0.0 || settings::GetBool(o.id);
            return true;
        }

        float RowHeight()
        {
            return ImGui::GetFrameHeight() + 10.0f * Scale();
        }

        // Rows keep their text kSpaceL inside the highlight, which spans the
        // table: the label from its left edge, and the right arrow's point
        // from its right edge (ImGui::RenderArrow draws the point a fifth of
        // the font inside the glyph box centred in the square button).
        float LabelInset()
        {
            return kSpaceL * Scale();
        }

        float ValueInset()
        {
            float arrow = ImGui::GetFrameHeight(), font = ImGui::GetFontSize();
            return std::max(0.0f, LabelInset() - ((arrow - font) * 0.5f + font * 0.2f));
        }

        // A row's label, drawn here rather than by its Selectable (after it:
        // the highlight is the Selectable's item rect).
        float RowLabel(ImVec2 pos, const char* label)
        {
            float mid = (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f;
            ImGui::GetWindowDrawList()->AddText(ImVec2(pos.x, CentredTextTop(mid)), ImGui::GetColorU32(ImGuiCol_Text), label);
            return mid;
        }

        void OptionRow(const settings::Option& o, float valueWidth, bool focus)
        {
            float s = Scale();
            const float rowHeight = RowHeight();
            double stored = settings::GetStoredValue(o.id);
            // Game Mode always runs fullscreen: shown, not changeable.
            const bool locked = o.id == Id::Fullscreen && video::FullscreenLocked();
            if (locked)
                stored = 1.0;
            ImGui::PushID(int(o.id));
            ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);
            ImGui::TableNextColumn();

            if (focus)
                ImGui::SetKeyboardFocusHere();
            // The highlight spans the row wherever the label starts.
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + LabelInset());
            ImVec2 labelPos = ImGui::GetCursorScreenPos();
            bool activated = ImGui::Selectable("##row", false,
                ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0, rowHeight));
            bool focused = ImGui::IsItemFocused();
            bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem);
            if (focused || (hovered && s_described == Id::Count))
                s_described = o.id;
            const float mid = RowLabel(labelPos, o.label);

            // Markers after the label: set by the environment, waits for a restart.
            {
                float x = labelPos.x + ImGui::CalcTextSize(o.label).x + 10.0f * s;
                ImDrawList* draw = ImGui::GetWindowDrawList();
                auto marker = [&](const char* text, ImU32 color) {
                    ImGui::PushFont(nullptr, 15.0f);
                    ImVec2 size = ImGui::CalcTextSize(text);
                    float y = CentredTextTop(mid);
                    ImVec2 pad(5.0f * s, 1.0f * s);
                    draw->AddRectFilled(ImVec2(x, y - pad.y), ImVec2(x + size.x + pad.x * 2, y + size.y + pad.y),
                        (color & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 0x40), 3.0f * s);
                    draw->AddText(ImVec2(x + pad.x, y), color, text);
                    ImGui::PopFont();
                    x += size.x + pad.x * 2 + 6.0f * s;
                };
                if (settings::EnvOverride(o.id))
                    marker("ENV", ImGui::GetColorU32(ImGuiCol_TextDisabled));
                if (settings::RestartPending(o.id))
                    marker("RESTART", ImGui::GetColorU32(ImGuiCol_PlotLines));
                // The row is the saved choice; the system (the green button)
                // changed the window this session: say what it is now.
                if (o.id == Id::Fullscreen && !locked && video::FullscreenWindowDiffers())
                    marker(video::WindowFullscreen() ? "NOW FULLSCREEN" : "NOW WINDOWED", ImGui::GetColorU32(ImGuiCol_TextDisabled));
            }

            int delta = 0;
            bool wrap = false;
            if (focused)
            {
                if (Pressed(ImGuiKey_GamepadDpadLeft) || Pressed(ImGuiKey_GamepadLStickLeft) || Pressed(ImGuiKey_LeftArrow))
                    delta = -1;
                if (Pressed(ImGuiKey_GamepadDpadRight) || Pressed(ImGuiKey_GamepadLStickRight) || Pressed(ImGuiKey_RightArrow))
                    delta = 1;
            }
            // A (or Enter) on an on/off or a choice row moves it on. A click
            // or tap on the row only selects it (and shows what it does): the
            // arrows change it, so a touch that only meant to look, or to
            // scroll, changes nothing. A pointer activates on its release.
            const bool byPointer = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
            if (activated && !byPointer && (o.type == settings::Type::Bool || o.type == settings::Type::Enum))
            {
                delta = 1;
                wrap = true;
            }

            // The value, between arrows (mouse only: the controller changes
            // the focused row with left/right, so the arrows stay out of its way).
            ImGui::TableNextColumn();
            float arrow = ImGui::GetFrameHeight();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (rowHeight - arrow) * 0.5f);
            ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            if (ImGui::ArrowButton("##less", ImGuiDir_Left))
                delta = -1;
            ImGui::SameLine(0, 0);
            ImVec2 valueMin = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(valueWidth, arrow));
            ImGui::SameLine(0, 0);
            if (ImGui::ArrowButton("##more", ImGuiDir_Right))
                delta = 1;
            ImGui::PopStyleColor();
            ImGui::PopItemFlag();

            std::string text = settings::FormatValue(o.id, stored);
            ImVec2 size = ImGui::CalcTextSize(text.c_str());
            ImDrawList* draw = ImGui::GetWindowDrawList();
            ImU32 color = ImGui::GetColorU32(focused ? ImGuiCol_PlotLines : ImGuiCol_Text);
            draw->AddText(ImVec2(valueMin.x + (valueWidth - size.x) * 0.5f, CentredTextTop(mid)), color, text.c_str());
            // Numbers get a bar showing where they sit in their range.
            if ((o.type == settings::Type::Int || o.type == settings::Type::Float) && o.max > o.min)
            {
                float t = float(std::clamp((stored - o.min) / (o.max - o.min), 0.0, 1.0));
                float y = valueMin.y + arrow - 3.0f * s;
                draw->AddRectFilled(ImVec2(valueMin.x, y), ImVec2(valueMin.x + valueWidth, y + 2.0f * s), ImGui::GetColorU32(ImGuiCol_FrameBg));
                draw->AddRectFilled(ImVec2(valueMin.x, y), ImVec2(valueMin.x + valueWidth * t, y + 2.0f * s), ImGui::GetColorU32(ImGuiCol_SliderGrab));
            }

            if (delta && !locked)
                Change(o, stored, delta, wrap);
            ImGui::PopID();
        }

        // A Controls row: the two keys that drive the control, as two cells.
        // Left and Right choose a cell (the focused row's choice; another
        // row starts on its first); A or Enter waits for a key for it, which
        // SettingsMenuTakesKey takes from the event loop before ImGui sees
        // it; Y or Backspace empties it; X or Delete puts both keys back to
        // their defaults. A click or tap only selects the row, and the cell
        // under the pointer, as on the other pages. The wait follows the
        // focus (the D-pad moving on ends it) and B ends it (below, in
        // DrawSettings, where B would otherwise close the menu).
        void KeyRow(const hid::keymap::Entry& e, float valueWidth, bool focus)
        {
            const float s = Scale();
            const float rowHeight = RowHeight();
            const settings::Option& o = settings::Info(e.id);
            const Id ids[2] = { e.id, e.id2 };
            ImGui::PushID(int(e.id));
            ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);
            ImGui::TableNextColumn();
            if (focus)
                ImGui::SetKeyboardFocusHere();
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + LabelInset());
            ImVec2 labelPos = ImGui::GetCursorScreenPos();
            bool activated = ImGui::Selectable("##row", false,
                ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0, rowHeight));
            bool focused = ImGui::IsItemFocused();
            if (focused || (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem) && s_described == Id::Count))
                s_described = e.id;
            const float mid = RowLabel(labelPos, o.label);

            if (focused && s_slotRow != e.id)
            {
                s_slotRow = e.id;
                s_slot = 0;
            }
            const bool waiting = s_waiting == e.id || s_waiting == e.id2;
            if (waiting && !focused)
                EndWait();

            // The cells, where other pages put their value (the arrows'
            // room kept so the columns line up from page to page).
            ImGui::TableNextColumn();
            const float arrow = ImGui::GetFrameHeight();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (rowHeight - arrow) * 0.5f);
            const ImVec2 min = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(valueWidth + arrow * 2, arrow));
            const float gap = KeyCellGap(), cell = (valueWidth - gap) * 0.5f;
            const float cellX[2] = { min.x + arrow, min.x + arrow + cell + gap };

            const bool byPointer = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
            if (activated && byPointer)
            {
                const float x = ImGui::GetMousePos().x;
                for (int i = 0; i < 2; i++)
                    if (x >= cellX[i] && x < cellX[i] + cell)
                        s_slot = i;
            }
            else if (activated)
            {
                s_waiting = ids[s_slot];
                s_refused = SDL_SCANCODE_UNKNOWN;
            }
            else if (focused && s_waiting == Id::Count)
            {
                if (Pressed(ImGuiKey_GamepadDpadLeft) || Pressed(ImGuiKey_GamepadLStickLeft) || Pressed(ImGuiKey_LeftArrow))
                    s_slot = 0;
                if (Pressed(ImGuiKey_GamepadDpadRight) || Pressed(ImGuiKey_GamepadLStickRight) || Pressed(ImGuiKey_RightArrow))
                    s_slot = 1;
                if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))
                {
                    const settings::Option& slot = settings::Info(ids[s_slot]);
                    LogChange(slot, settings::GetStoredValue(slot.id), settings::Set(slot.id, settings::keynames::kNone));
                }
                if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false) || ImGui::IsKeyPressed(ImGuiKey_Delete, false))
                    for (Id id : ids)
                        LogChange(settings::Info(id), settings::GetStoredValue(id), settings::Reset(id));
            }

            ImDrawList* draw = ImGui::GetWindowDrawList();
            for (int i = 0; i < 2; i++)
            {
                const bool waitingHere = s_waiting == ids[i];
                const bool chosen = focused && s_slot == i;
                const double stored = settings::GetStoredValue(ids[i]);
                const std::string text = waitingHere ? kPressAKey : KeyLabel(stored);
                const ImVec2 size = ImGui::CalcTextSize(text.c_str());
                ImU32 color;
                if (waitingHere)
                {
                    PushAccentText();
                    color = ImGui::GetColorU32(ImGuiCol_Text);
                    PopAccentText();
                }
                else if (chosen)
                    color = ImGui::GetColorU32(ImGuiCol_PlotLines);
                else
                    color = ImGui::GetColorU32(stored == double(settings::keynames::kNone) ? ImGuiCol_TextDisabled : ImGuiCol_Text);
                const ImVec2 at(cellX[i] + (cell - size.x) * 0.5f, CentredTextTop(mid));
                draw->AddText(at, color, text.c_str());
                if (chosen)  // underlined too, so an empty cell shows which it is
                    draw->AddLine(ImVec2(at.x, at.y + size.y + 2.0f * s), ImVec2(at.x + size.x, at.y + size.y + 2.0f * s), color, 1.0f * s);
            }
            ImGui::PopID();
        }

        const char* BugReportValue()
        {
            switch (report::GetBugReportState())
            {
            case report::BugReportState::Saving: return "Saving...";
            case report::BugReportState::Saved: return "Saved";
            case report::BugReportState::Failed: return "Failed";
            default: return "Save";
            }
        }

        // An action, not an option: a label and what it shows now (no
        // arrows). True when A (or a click) pressed it.
        bool ActionRow(const char* id, const char* label, const char* value, float valueWidth, bool& described)
        {
            const float rowHeight = RowHeight();
            ImGui::PushID(id);
            ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);
            ImGui::TableNextColumn();
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + LabelInset());
            ImVec2 labelPos = ImGui::GetCursorScreenPos();
            bool activated = ImGui::Selectable("##row", false,
                ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0, rowHeight));
            bool focused = ImGui::IsItemFocused();
            if (focused || (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem) && s_described == Id::Count))
                described = true;
            const float mid = RowLabel(labelPos, label);
            ImGui::TableNextColumn();
            float arrow = ImGui::GetFrameHeight();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (rowHeight - arrow) * 0.5f);
            ImVec2 min = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(valueWidth + arrow * 2, arrow));
            ImVec2 size = ImGui::CalcTextSize(value);
            ImGui::GetWindowDrawList()->AddText(ImVec2(min.x + arrow + (valueWidth - size.x) * 0.5f, CentredTextTop(mid)),
                ImGui::GetColorU32(focused ? ImGuiCol_PlotLines : ImGuiCol_Text), value);
            ImGui::PopID();
            return activated;
        }

        // Settings > Wheel > Calibrate. The wheels we know say which axis
        // each pedal is, and where the axes rest tells a pedal from the
        // wheel itself (hid/wheel_map.h) - but three identical springs on a
        // wheel we don't know can only be told apart by someone pressing
        // them, and which way the wheel turns only by someone turning it.
        //
        // So: one step at a time, each showing what it has found and
        // waiting for A to accept it. B gives up. The help line shows what
        // every axis reads as it happens, so a wheel can be checked over
        // without leaving the page.
        using Step = hid::wheel::Step;

        void CancelCalibrate()
        {
            if (hid::wheel::Calibrating())
                hid::wheel::StopCalibrate();
        }

        // Every axis and what it reads now, for the help line.
        std::string WheelReadings()
        {
            const hid::wheel::Assignment axes = hid::wheel::Axes();
            const hid::wheel::Reading now = hid::wheel::Last();
            std::string text = axes.steer < 0
                ? "Steering: none"
                : std::format("Steering axis {}{}: {:+.0f}%", axes.steer,
                      settings::GetBool(Id::InvertSteering) ? " inverted" : "", double(now.steer) * 100.0);
            struct Row { const char* name; int axis; float value; };
            for (const Row& row : { Row{ "Throttle", axes.throttle, now.throttle }, Row{ "Brake", axes.brake, now.brake },
                                    Row{ "Clutch", axes.clutch, now.clutch } })
                text += row.axis < 0 ? std::format("   {}: none", row.name)
                                     : std::format("   {} {}: {:.0f}%", row.name, row.axis, double(row.value) * 100.0);
            return text;
        }

        void CalibrateRow(float valueWidth)
        {
            const bool calibrating = hid::wheel::Calibrating();
            // While it runs, the value column counts the steps off: the
            // names are too long for it, and the help line has them.
            const std::string value = calibrating
                ? std::format("{}/{}", size_t(hid::wheel::CalibratingStep()) + 1, hid::wheel::kStepCount)
                : std::string("Start");
            const bool activated = ActionRow("wheel-calibrate", "Calibrate", value.c_str(), valueWidth, s_describedCal);
            if (!calibrating)
            {
                if (activated)
                    hid::wheel::StartCalibrate();
                return;
            }
            // The keyboard's and a controller's keys (the wheel's own go to
            // Calibrate itself): A, Enter or a click keeps, right skips, left
            // goes back; B or Esc stops, at the end of the menu's frame. The
            // page owns these keys meanwhile (SettingsMenu), so the cursor stays
            // here and A can't change another row on the way.
            if (activated || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false) || ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) || ImGui::IsKeyPressed(ImGuiKey_Space, false))
                hid::wheel::KeepStep();
            else if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, false) || ImGui::IsKeyPressed(ImGuiKey_RightArrow, false))
                hid::wheel::SkipStep();
            else if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, false) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false))
                hid::wheel::BackStep();
        }

        // A (or a click) saves the report.
        void BugReportRow(float valueWidth)
        {
            if (ActionRow("bug-report", "Save Bug Report", BugReportValue(), valueWidth, s_describedBugReport))
                report::SaveBugReport();
        }

        // A (or a click) asks GitHub, and the dialog shows the answer.
        void UpdateRow(float valueWidth)
        {
            if (ActionRow("check-updates", "Check for Updates", UpdateRowValue(), valueWidth, s_describedUpdate))
                OpenUpdateDialog();
        }

        bool QuitPromptOpen()
        {
#if SB_QUIT_GAME
            return ImGui::IsPopupOpen(kQuitPopup);
#else
            return false;
#endif
        }

#if SB_QUIT_GAME
        // Quit Game's prompt (quit_game.h), at the settings window's top
        // level like the other dialogs. True while it is open: B is its, not
        // the menu's.
        bool DrawQuitPrompt()
        {
            if (!ImGui::IsPopupOpen(kQuitPopup))
            {
                if (s_quit.Dismiss() == quit::Outcome::Cancelled)
                    fprintf(stderr, "[ui] Quit Game: cancelled (the prompt closed)\n");
                return false;
            }
            ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            if (!ImGui::BeginPopupModal(kQuitPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
            {
                if (s_quit.Dismiss() == quit::Outcome::Cancelled)
                    fprintf(stderr, "[ui] Quit Game: cancelled (the prompt closed)\n");
                return false;
            }
            s_quit.Shown();
            ImGui::TextUnformatted("Quit SpeedBreaker? Progress since your last save will be lost.");
            ImGui::Spacing();
            const bool quitPressed = ImGui::Button("Quit");
            ImGui::SameLine();
            if (s_quit.TakeFocus())
                ImGui::SetKeyboardFocusHere();  // Cancel (quit::Prompt::kDefaultFocus)
            const bool cancelPressed = ImGui::Button("Cancel");
            ImGui::SetItemDefaultFocus();
            quit::Outcome outcome = quit::Outcome::None;
            const char* how = "";
            if (cancelPressed)
            {
                outcome = s_quit.Press(quit::Button::Cancel);
                how = "Cancel";
            }
            else if (quitPressed)
                outcome = s_quit.Press(quit::Button::Quit);
            else if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            {
                outcome = s_quit.Back();
                how = "B or Escape";
            }
            if (outcome == quit::Outcome::Cancelled)
            {
                fprintf(stderr, "[ui] Quit Game: cancelled (%s)\n", how);
                ImGui::CloseCurrentPopup();
            }
            else if (outcome == quit::Outcome::Quit)
            {
                // The way out a window close takes: RunFrame returns false on
                // its next poll and main() shuts the game down.
                fprintf(stderr, "[ui] Quit Game: quitting (as closing the window does)\n");
                ImGui::CloseCurrentPopup();
                SDL_Event e{};
                e.type = SDL_EVENT_QUIT;
                e.quit.timestamp = SDL_GetTicksNS();
                if (!SDL_PushEvent(&e))
                {
                    fprintf(stderr, "[ui] Quit Game: couldn't queue the quit: %s\n", SDL_GetError());
                    Toast("Couldn't quit. Close the game's window instead.");
                    s_quit.QuitFailed();
                }
            }
            ImGui::EndPopup();
            return true;
        }
#endif

        void HelpLine()
        {
            if (s_describedUpdate)
            {
                UpdateRowHelp();
                return;
            }
            if (s_describedBugReport)
            {
#if defined(__APPLE__) && TARGET_OS_IOS
                ImGui::TextWrapped("Saves the recent logs, crash and hang reports, your settings, a system summary and a screenshot "
                                   "in one .zip in the Files app (SpeedBreaker folder). Nothing is uploaded: send the file yourself.");
#else
                ImGui::TextWrapped("Saves the recent logs, crash and hang reports, your settings, a system summary and a screenshot "
                                   "in one .zip on the Desktop (or the game's data folder). Nothing is uploaded: send the file yourself.");
#endif
                std::string message = report::BugReportMessage();
                const bool saved = report::GetBugReportState() == report::BugReportState::Saved;
                // One line under the description (the footer has room for no
                // more): a path too long for it gives way to the file's name,
                // or to the reason it failed. The toast has the whole text.
                if (ImGui::CalcTextSize(message.c_str()).x > ImGui::GetContentRegionAvail().x)
                {
#ifdef _WIN32
                    size_t cut = saved ? message.find_last_of("\\/") : message.rfind(": ");
#else
                    size_t cut = saved ? message.rfind('/') : message.rfind(": ");
#endif
                    if (saved && cut == std::string::npos && (cut = message.rfind(": ")) != std::string::npos)
                        cut++;  // iOS: "Saved to the Files app, SpeedBreaker folder: <name>"
                    if (cut != std::string::npos)
                        message = saved ? "Saved: " + message.substr(cut + 1) : "Couldn't save the bug report: " + message.substr(cut + 2);
                }
                if (saved)
                {
                    PushAccentText();
                    ImGui::TextUnformatted(message.c_str());
                    PopAccentText();
                }
                else
                    ImGui::TextDisabled("%s", message.empty() ? " " : message.c_str());
                return;
            }
            if (s_describedCal)
            {
                if (hid::wheel::Calibrating())
                {
                    const hid::wheel::Step step = hid::wheel::CalibratingStep();
                    const hid::wheel::Candidate c = hid::wheel::Watch();
                    const char* name = hid::wheel::StepName(step);
                    PushAccentText();
                    if (!c.found)
                        ImGui::TextWrapped("%zu of %zu, %s. %s%s", size_t(step) + 1, hid::wheel::kStepCount, name,
                            hid::wheel::StepAsk(step), hid::wheel::StepIsButton(step) ? "." : ", then A to keep it.");
                    else if (hid::wheel::StepIsButton(step))
                        ImGui::TextWrapped("%s: button %d. A, or the same button again, keeps it; another button replaces it.",
                            name, c.button);
                    else
                        ImGui::TextWrapped("%s: axis %d, %+d to %+d, %.0f%% of its travel. A keeps it.", name, c.axis, int(c.rest),
                            int(c.full), double(c.moved) * 100.0);
                    PopAccentText();
                    ImGui::TextDisabled(hid::wheel::StepIsButton(step)
                        ? "D-pad right skips, left goes back, down stops (keeping what's set). Esc stops too."
                        : "D-pad right skips, left goes back; B, Esc or D-pad down stops (keeping what's set).");
                }
                else
                    ImGui::TextWrapped("Teaches the game your wheel: both ends of its turn, each pedal, and a button for "
                                       "every control, one at a time. A keeps what each step saw; the D-pad skips a step "
                                       "or goes back. Only a wheel that comes out wrong needs this.");
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextWrapped("%s", WheelReadings().c_str());
                ImGui::PopStyleColor();
                return;
            }
            if (s_described == Id::Count)
            {
                ImGui::TextDisabled(" ");
                ImGui::TextDisabled(" ");
                return;
            }
            const settings::Option& o = settings::Info(s_described);
            if (o.type == settings::Type::Key)
            {
                const hid::keymap::Entry& e = hid::keymap::EntryOf(o.id);
                if (s_waiting == e.id || s_waiting == e.id2)
                {
                    PushAccentText();
                    if (s_refused != SDL_SCANCODE_UNKNOWN)
                        ImGui::Text("%s can't be used here. Press another key; Esc or B cancels.", RefusedName(s_refused).c_str());
                    else
                        ImGui::Text("Press the key for %s. Esc or B cancels.", o.label);
                    PopAccentText();
                    ImGui::TextDisabled(" ");
                    return;
                }
                ImGui::TextWrapped("%s", o.description);
                // The keyboard's keys for what the legend names in
                // controller terms (Y, X); and a key on several controls is
                // fine (the defaults share the arrows), but say which, in
                // case it wasn't meant.
                std::string also = SharedWith(e);
                ImGui::TextDisabled("Enter sets the key, Backspace clears it, Delete restores the row.%s%s%s",
                    also.empty() ? "" : "  Also: ", also.c_str(), also.empty() ? "" : ".");
                return;
            }
            if (o.id == Id::Fullscreen && video::FullscreenLocked())
            {
                ImGui::TextDisabled("Always on in Game Mode: Steam sets the resolution (Properties > Game Resolution).");
                return;
            }
            // The window's own fullscreen button (or the system) changed it for
            // this session; the setting still decides how the game starts.
            if (o.id == Id::Fullscreen && video::FullscreenWindowDiffers() && !settings::EnvOverride(o.id))
            {
                ImGui::TextDisabled("The window is %s for now. The game starts %s, as set here.",
                    video::WindowFullscreen() ? "fullscreen" : "windowed", settings::GetBool(Id::Fullscreen) ? "fullscreen" : "windowed");
                return;
            }
            // Language: what the installed files give the language chosen
            // here (the one this run has, under NFSMW_LANGUAGE), then when
            // it applies.
            if (o.id == Id::GameLanguage)
            {
                const char* env = settings::EnvOverride(o.id);
                const double value = env ? settings::GetValue(o.id) : settings::GetStoredValue(o.id);
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextWrapped("%s", game::language::MenuNote(int32_t(value)).c_str());
                ImGui::PopStyleColor();
                if (env)
                    ImGui::TextDisabled("%s sets this for this run: %s. The choice here is saved for later runs.", env,
                        settings::FormatValue(o.id, value).c_str());
                else if (settings::RestartPending(o.id))
                {
                    PushAccentText();
                    ImGui::TextUnformatted("Takes effect the next time the game starts.");
                    PopAccentText();
                }
                else
                    ImGui::TextDisabled("Changes take effect the next time the game starts.");
                return;
            }
            // The experiment's finer scale wins over Screen Resolution's.
            if (float scale = video::PresentScaleOverride(); o.id == Id::PresentScale && scale > 0.0f)
                ImGui::TextDisabled("NFSMW_PRESENT_SCALE sets %.2f for this run.", double(scale));
            else if (const char* env = settings::EnvOverride(o.id))
                ImGui::TextDisabled("%s sets this for this run: %s. The choice here is saved for later runs.", env,
                    settings::FormatValue(o.id, settings::GetValue(o.id)).c_str());
            else if (settings::RestartPending(o.id))
            {
                PushAccentText();
                ImGui::TextUnformatted("Takes effect the next time the game starts.");
                PopAccentText();
            }
            else if (o.id == Id::SmoothMotion && settings::GetBool(o.id) && video::FrameRate() == 30)
                ImGui::TextDisabled("Off while the game runs at 30 fps (Frame Rate).");
            else if (o.id == Id::SmoothMotion && settings::GetBool(o.id) && !video::HfrOffered())
                ImGui::TextDisabled("This screen shows 60 pictures a second: nothing to add here.");
            else if (o.apply == settings::Apply::Restart)
                ImGui::TextDisabled("Changes take effect the next time the game starts.");
            else if (std::string note = o.id == Id::FrameRate ? video::AutoFrameRateNote() : std::string(); !note.empty())
                ImGui::TextDisabled("%s", note.c_str());  // what Auto runs at now
            // The wheel the page is about - and, for the two rows that need
            // its motors, whether this system can drive them at all (on a Mac
            // only the wheels SDL's own Logitech driver takes: hid/wheel.h).
            else if (o.category == Category::Wheel)
            {
                const std::string name = hid::wheel::Name();
                if ((o.id == Id::ForceFeedback || o.id == Id::CenteringForce) && !hid::wheel::HasForceFeedback())
                    ImGui::TextDisabled("%s: this system can't drive its motors, so this does nothing. Steering, the pedals and "
                                        "the buttons all work.", name.empty() ? "This wheel" : name.c_str());
                else if (!name.empty())
                    ImGui::TextDisabled("%s is connected.", name.c_str());
                else
                    ImGui::TextDisabled(" ");
            }
            else
                ImGui::TextDisabled(" ");
        }
    }

    void DrawSettings()
    {
        float s = Scale();
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImVec2 size(std::min(viewport->WorkSize.x - 2 * kSpaceL * s, 860.0f * s), std::min(viewport->WorkSize.y - 2 * kSpaceL * s, 600.0f * s));
        ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(size, ImGuiCond_Always);
        constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar;
        // Keep the focus (a click or a touchscreen tap outside the panel
        // would leave the D-pad nothing to move).
        if (s_opened || !ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow))
            ImGui::SetNextWindowFocus();
        if (s_opened)
        {
            s_opened = false;
            s_focusFirstRow = true;
        }
        if (!ImGui::Begin("Settings", nullptr, flags))
        {
            ImGui::End();
            return;
        }

#if defined(__APPLE__) && TARGET_OS_IOS
        // No F1 key to speak of: the controller's chord (its Back and Start
        // are View and Menu on an Xbox pad), or touch.
        TitleText("Settings", "View + Menu on a controller, or tap with three fingers");
#elif defined(__APPLE__)
        TitleText("Settings", "F1 or Cmd+, / Back + Start");
#else
        TitleText("Settings", "F1 / Back + Start");
#endif

        // Pages: LB/RB (Q/E) or the tabs; not under a dialog, which keeps them.
        const bool dialog = ImGui::IsPopupOpen("reset") || UpdateDialogOpen() || QuitPromptOpen();
        if (int delta = dialog ? 0 : PageDelta())
        {
            Category next = s_category;
            do
                next = Category((int(next) + delta + kCategories) % kCategories);
            while (!PageShown(next));
            s_category = next;
            s_tabPending = true;
            s_focusFirstRow = true;
        }
        if (!PageShown(s_category))  // the keyboard, or the wheel, went away while on its page
        {
            s_category = Category::Input;
            s_tabPending = true;
            s_focusFirstRow = true;
        }
        // X on the Controls page is the rows' (KeyRow: the row's default
        // keys). To ImGui it is also the window-switching key: a press
        // starts the toggle and the release moves the cursor to the root
        // window. Owning the key while the page shows keeps that off; the
        // rows' IsKeyPressed, which accepts any owner, still sees it.
        if (s_category == Category::Controls)
            ImGui::SetKeyOwner(ImGuiKey_GamepadFaceLeft, ImHashStr("controls-x"));
        s_described = Id::Count;
        s_describedBugReport = false;
        s_describedUpdate = false;
        s_describedCal = false;
        if (s_waiting != Id::Count && settings::Info(s_waiting).category != s_category)
            EndWait();
        if (hid::wheel::Calibrating() && s_category != Category::Wheel)
            CancelCalibrate();  // the page has gone (or the wheel has)
        // While Calibrate runs, the keys that move around and pick rows are
        // its own (CalibrateRow reads them; IsKeyPressed accepts any owner):
        // the cursor stays on its row and A can't change another on the way.
        if (hid::wheel::Calibrating())
            for (ImGuiKey key : { ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight,
                     ImGuiKey_UpArrow, ImGuiKey_DownArrow, ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_GamepadFaceDown,
                     ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Space })
                ImGui::SetKeyOwner(key, ImHashStr("wheel-calibrate"));
        // For the B check at the end: B's nav cancel (in ImGui's NewFrame,
        // before this) takes the waiting row's focus, and KeyRow then ends
        // the wait before that check could see it.
        const bool wasWaiting = s_waiting != Id::Count;
        if (ImGui::BeginTabBar("pages", ImGuiTabBarFlags_NoTooltip))
        {
            for (int c = 0; c < kCategories; c++)
            {
                Category category = Category(c);
                if (!PageShown(category))
                    continue;
                ImGuiTabItemFlags tabFlags = (s_tabPending && category == s_category) ? ImGuiTabItemFlags_SetSelected : 0;
                ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
                bool open = ImGui::BeginTabItem(settings::CategoryName(category), nullptr, tabFlags);
                ImGui::PopItemFlag();
                if (!open)
                    continue;
                if (!s_tabPending && category != s_category)
                {
                    s_category = category;  // clicked
                    s_focusFirstRow = true;
                }
                if (category == s_category)
                    s_tabPending = false;
                // Only once the page shown is the one chosen: the frame after
                // LB/RB still draws the old page.
                bool focusFirst = s_focusFirstRow && category == s_category;
                if (focusFirst)
                    s_focusFirstRow = false;

                float footer = ImGui::GetTextLineHeightWithSpacing() * 3.0f + ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 3;
                // A row's highlight reaches half an item spacing past the
                // row (Selectable pads itself to meet its neighbours), and
                // its focus outline a pixel or so further: so past the first
                // and last rows too, where the list's edge cut it. The list
                // keeps room for it, and starts that much higher so the rows
                // stay where they were.
                const ImVec2 cellPadding(kSpaceM * s, 2.0f * s);
                const float overhang = std::ceil(std::max(0.0f, ImGui::GetStyle().ItemSpacing.y * 0.5f - cellPadding.y + s));
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() - overhang);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, overhang));
                bool rows = ImGui::BeginChild("rows", ImVec2(0, -footer), ImGuiChildFlags_NavFlattened | ImGuiChildFlags_AlwaysUseWindowPadding);
                ImGui::PopStyleVar();
                if (rows)
                {
                    float valueWidth = 0.0f;
                    if (category == Category::Controls)
                        valueWidth = 2 * KeyCellWidth() + KeyCellGap();
                    else
                        for (const auto& o : settings::Options())
                            if (o.category == category && Shown(o))
                                valueWidth = std::max(valueWidth, ValueWidth(o));
                    if (category == Category::Advanced)
                        for (const char* text : { "Saving...", "Checking...", UpdateRowValue() })
                            valueWidth = std::max(valueWidth, ImGui::CalcTextSize(text).x);
                    if (category == Category::Wheel)
                        for (const char* text : { "Start", "17/17" })
                            valueWidth = std::max(valueWidth, ImGui::CalcTextSize(text).x);
                    valueWidth += 2 * kSpaceM * s;
                    float arrow = ImGui::GetFrameHeight();
                    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, cellPadding);
                    if (ImGui::BeginTable("options", 2))
                    {
                        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthFixed, valueWidth + arrow * 2 + ValueInset());
                        if (category == Category::Controls)
                            for (const auto& e : hid::keymap::kEntries)
                            {
                                KeyRow(e, valueWidth, focusFirst);
                                focusFirst = false;
                            }
                        else
                            for (const auto& o : settings::Options())
                                if (o.category == category && Shown(o))
                                {
                                    OptionRow(o, valueWidth, focusFirst);
                                    focusFirst = false;
                                }
                        if (category == Category::Wheel)
                            CalibrateRow(valueWidth);
                        if (category == Category::Advanced)
                        {
                            BugReportRow(valueWidth);
                            UpdateRow(valueWidth);
                        }
                        ImGui::EndTable();
                    }
                    ImGui::PopStyleVar();
                    // Which build this is, for bug reports by hand: under
                    // the rows, lined up with their labels.
                    if (category == Category::Advanced)
                    {
                        ImGui::Spacing();
                        ImGui::Indent(LabelInset());
                        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - LabelInset());
                        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                        ImGui::TextWrapped("SpeedBreaker %s", report::BuildString());
                        // GPL-3.0 section 5(d): the interface shows the
                        // legal notices (NOTICE, THIRD_PARTY_NOTICES.md).
                        ImGui::TextWrapped("Copyright (C) 2026 project(u) and SpeedBreaker contributors. Free software under the "
                                           "GPL-3.0-or-later, with no warranty; third-party licenses come with the game. Not affiliated "
                                           "with Electronic Arts or Valve.");
                        ImGui::PopStyleColor();
                        ImGui::PopTextWrapPos();
                        ImGui::Unindent(LabelInset());
                    }
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::Separator();
        HelpLine();

        // Buttons (the help above runs to one, two or three lines).
        ButtonRowAtBottom();
        bool anyChanged = false;
        // The Wheel page's pedal axes are hidden options, and Reset page
        // does put them back, so they count as a change of the page.
        const bool hiddenCounts = s_category == Category::Wheel;
        for (const auto& o : settings::Options())
            if (o.category == s_category && (Shown(o) || (o.hidden && hiddenCounts)) && !settings::IsDefault(o.id))
                anyChanged = true;
        ImGui::BeginDisabled(!anyChanged);
        if (ImGui::Button("Reset page"))
            ImGui::OpenPopup("reset");
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Close"))
            CloseSettings();
#if SB_QUIT_GAME
        // Last, after Close: Down from the rows lands on the footer's first
        // button, so Quit Game takes a Right more, and then asks.
        ImGui::SameLine();
        if (ImGui::Button("Quit Game") && s_quit.Choose() == quit::Outcome::Asking)
        {
            fprintf(stderr, "[ui] Quit Game: asking\n");
            ImGui::OpenPopup(kQuitPopup);
        }
#endif
        ImGui::SameLine();
        {
            // (The Controls page leaves out "D-pad: choose": with Quit Game in the
            // footer its legend was wider than the window and lost "B: close".)
            const char* legend = s_category == Category::Controls ? "A: set key   Y: clear   X: default   LB RB: page   B: close"
                                                                 : "D-pad: choose, change   LB RB: page   B: close";
            float w = ImGui::CalcTextSize(legend).x;
            ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - w));
            ImGui::TextDisabled("%s", legend);
        }

        bool popup = false;  // a dialog has the input: B is its, not the menu's
        if (ImGui::BeginPopupModal("reset", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
        {
            popup = true;
            ImGui::Text("Reset every %s setting to its default?", settings::CategoryName(s_category));
            ImGui::Spacing();
            if (ImGui::Button("Reset"))
            {
                for (const auto& o : settings::Options())
                    if (o.category == s_category && Shown(o))
                    {
                        double from = settings::GetStoredValue(o.id);
                        LogChange(o, from, settings::Reset(o.id));
                    }
                // The pedals' axes are hidden options: the loop above
                // leaves them, so the wheel puts them back itself.
                if (s_category == Category::Wheel)
                {
                    CancelCalibrate();
                    hid::wheel::ResetAxes();
                    fprintf(stderr, "[settings] menu: calibrate: back to the device's own axes\n");
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
                ImGui::CloseCurrentPopup();
            ImGui::SetItemDefaultFocus();
            ImGui::EndPopup();
        }
        if (DrawUpdateDialog())
            popup = true;
#if SB_QUIT_GAME
        if (DrawQuitPrompt())
            popup = true;
#endif
        // B (or Esc) closes the menu; while a Controls row was waiting for
        // a key at the top of this frame it ends the wait instead (the
        // cursor goes with it, as after the Reset dialog: the next D-pad
        // press brings it back). The Esc key itself never gets here then:
        // SettingsMenuTakesKey took its press.
        if (!popup && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsAnyItemActive() &&
            (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
        {
            if (wasWaiting)
                EndWait();
            else if (hid::wheel::Calibrating())
                CancelCalibrate();  // wheel.cpp logs where it stopped
            else
                CloseSettings();
        }
        ImGui::End();
    }

    void ResetSettingsMenu()
    {
        s_opened = true;
        EndWait();  // a wait the menu was closed under is over
        CancelCalibrate();  // ...and so is a step it was asking for
        s_takenKey = SDL_SCANCODE_UNKNOWN;
        s_slotRow = Id::Count;
        s_slot = 0;
        // A dialog the menu was closed under (Start or Back alone closes it
        // from anywhere) doesn't come back with it.
        if (ImGuiContext* g = ImGui::GetCurrentContext(); g && g->OpenPopupStack.Size > 0)
            ImGui::ClosePopupToLevel(0, false);
        ForgetUpdateDialog();
    }

    bool SettingsMenuTakesKey(const SDL_KeyboardEvent& key)
    {
        // The key a wait took or refused stays down a moment longer. ImGui never saw
        // it go down, so its OS repeats would read as fresh presses (Enter
        // would start a new wait, an arrow would move the cursor): they and
        // its release stay out of ImGui too.
        if (s_takenKey != SDL_SCANCODE_UNKNOWN && key.scancode == s_takenKey)
        {
            if (!key.down)
                s_takenKey = SDL_SCANCODE_UNKNOWN;
            if (!key.down || key.repeat)
                return true;
            s_takenKey = SDL_SCANCODE_UNKNOWN;  // pressed anew: its release went by unseen
        }
        if (s_waiting == Id::Count || !key.down || key.repeat)
            return false;
        const settings::Option& o = settings::Info(s_waiting);
        s_takenKey = key.scancode;
        if (key.scancode == SDL_SCANCODE_ESCAPE)
        {
            fprintf(stderr, "[settings] menu: %s.%s: no new key (Esc)\n", o.table, o.key);
            EndWait();
            return true;
        }
        // Not a key of the table (Cmd / Windows, F1, the media keys), or a
        // key pressed with Cmd / Windows held: the wait goes on and the help
        // line says so. (Cmd+, still closes the menu on a Mac: ProcessEvent's
        // toggle sees the comma before this does; the Cmd press before it
        // lands here and is refused.) Its repeats and release stay out of
        // ImGui like a taken key's: s_takenKey is already set.
        if (key.scancode == SDL_SCANCODE_UNKNOWN || !settings::keynames::Known(uint16_t(key.scancode)) || (key.mod & SDL_KMOD_GUI))
        {
            s_refused = key.scancode;
            fprintf(stderr, "[settings] menu: %s.%s: %s can't be used\n", o.table, o.key, RefusedName(key.scancode).c_str());
            return true;
        }
        double from = settings::GetStoredValue(o.id);
        LogChange(o, from, settings::Set(o.id, double(key.scancode)));
        EndWait();
        return true;
    }

    void ForgetQuitPrompt()
    {
#if SB_QUIT_GAME
        if (s_quit.Dismiss() == quit::Outcome::Cancelled)
            fprintf(stderr, "[ui] Quit Game: cancelled (the menu closed)\n");
#endif
    }
}
