// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The settings menu, built from settings::Options(): one page per category,
// one row per option. Made for a controller first: the D-pad picks a row,
// left/right changes it, A toggles, LB/RB turn pages, B closes. The mouse
// works too (a click selects a row, its arrows change it, the tabs turn
// pages), and touch as the mouse (on iOS a drag scrolls the rows: ui.cpp).
// Advanced also has Save Bug Report (report/bug_report.cpp), the build's
// identity (version and commit) and the legal notice.
#include <stdafx.h>
#include "ui.h"

#include <report/report.h>
#include <user/settings.h>
#include <video/frame_rate.h>
#include <video/presenter.h>

#include <imgui.h>

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
        bool s_opened = true;        // just opened: take focus

        constexpr int kCategories = int(Category::Count);

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

        // An action, not an option: A (or a click) saves the report.
        void BugReportRow(float valueWidth)
        {
            const float rowHeight = RowHeight();
            ImGui::PushID("bug-report");
            ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);
            ImGui::TableNextColumn();
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + LabelInset());
            ImVec2 labelPos = ImGui::GetCursorScreenPos();
            bool activated = ImGui::Selectable("##row", false,
                ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0, rowHeight));
            bool focused = ImGui::IsItemFocused();
            if (focused || (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem) && s_described == Id::Count))
                s_describedBugReport = true;
            const float mid = RowLabel(labelPos, "Save Bug Report");
            ImGui::TableNextColumn();
            float arrow = ImGui::GetFrameHeight();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (rowHeight - arrow) * 0.5f);
            ImVec2 min = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(valueWidth + arrow * 2, arrow));
            const char* text = BugReportValue();
            ImVec2 size = ImGui::CalcTextSize(text);
            ImGui::GetWindowDrawList()->AddText(ImVec2(min.x + arrow + (valueWidth - size.x) * 0.5f, CentredTextTop(mid)),
                ImGui::GetColorU32(focused ? ImGuiCol_PlotLines : ImGuiCol_Text), text);
            if (activated)
                report::SaveBugReport();
            ImGui::PopID();
        }

        void HelpLine()
        {
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
                    size_t cut = saved ? message.rfind('/') : message.rfind(": ");
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
            if (s_described == Id::Count)
            {
                ImGui::TextDisabled(" ");
                ImGui::TextDisabled(" ");
                return;
            }
            const settings::Option& o = settings::Info(s_described);
            ImGui::TextWrapped("%s", o.description);
            if (o.id == Id::Fullscreen && video::FullscreenLocked())
            {
                ImGui::TextDisabled("Always on in Game Mode: Steam sets the resolution (Properties > Game Resolution).");
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
            else if (o.apply == settings::Apply::Restart)
                ImGui::TextDisabled("Changes take effect the next time the game starts.");
            else if (std::string note = o.id == Id::FrameRate ? video::AutoFrameRateNote() : std::string(); !note.empty())
                ImGui::TextDisabled("%s", note.c_str());  // what Auto runs at now
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

        // Pages: LB/RB (Q/E) or the tabs.
        if (int delta = PageDelta())
        {
            s_category = Category((int(s_category) + delta + kCategories) % kCategories);
            s_tabPending = true;
            s_focusFirstRow = true;
        }
        s_described = Id::Count;
        s_describedBugReport = false;
        if (ImGui::BeginTabBar("pages", ImGuiTabBarFlags_NoTooltip))
        {
            for (int c = 0; c < kCategories; c++)
            {
                Category category = Category(c);
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
                    for (const auto& o : settings::Options())
                        if (o.category == category && !o.hidden)
                            valueWidth = std::max(valueWidth, ValueWidth(o));
                    if (category == Category::Advanced)
                        valueWidth = std::max(valueWidth, ImGui::CalcTextSize("Saving...").x);
                    valueWidth += 2 * kSpaceM * s;
                    float arrow = ImGui::GetFrameHeight();
                    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, cellPadding);
                    if (ImGui::BeginTable("options", 2))
                    {
                        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthFixed, valueWidth + arrow * 2 + ValueInset());
                        for (const auto& o : settings::Options())
                            if (o.category == category && !o.hidden)
                            {
                                OptionRow(o, valueWidth, focusFirst);
                                focusFirst = false;
                            }
                        if (category == Category::Advanced)
                            BugReportRow(valueWidth);
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
        for (const auto& o : settings::Options())
            if (o.category == s_category && !o.hidden && !settings::IsDefault(o.id))
                anyChanged = true;
        ImGui::BeginDisabled(!anyChanged);
        if (ImGui::Button("Reset page"))
            ImGui::OpenPopup("reset");
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Close"))
            CloseSettings();
        ImGui::SameLine();
        {
            const char* legend = "D-pad: choose, change   LB RB: page   B: close";
            float w = ImGui::CalcTextSize(legend).x;
            ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - w));
            ImGui::TextDisabled("%s", legend);
        }

        if (ImGui::BeginPopupModal("reset", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
        {
            ImGui::Text("Reset every %s setting to its default?", settings::CategoryName(s_category));
            ImGui::Spacing();
            if (ImGui::Button("Reset"))
            {
                for (const auto& o : settings::Options())
                    if (o.category == s_category && !o.hidden)
                    {
                        double from = settings::GetStoredValue(o.id);
                        LogChange(o, from, settings::Reset(o.id));
                    }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
                ImGui::CloseCurrentPopup();
            ImGui::SetItemDefaultFocus();
            ImGui::EndPopup();
        }
        else if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsAnyItemActive() &&
            (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
            CloseSettings();
        ImGui::End();
    }

    void ResetSettingsMenu()
    {
        s_opened = true;
    }
}
