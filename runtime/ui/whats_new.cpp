// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See ui.h.
//
// The one-time "What's new" note: the first launch of a newer version shows
// this version's CHANGELOG.md entry, compiled in at build time
// (generated/whats_new.inc, runtime/CMakeLists.txt), over the game's first
// frames until the player closes it. Offline: the note is built in and
// nothing is sent (Check for Updates, update/updater.h, goes online only when
// pressed). main.cpp decides when (user/version_check.h); the version that
// ran last lives in settings.toml, so it shows once.
#include <stdafx.h>
#include "ui.h"

#include <user/version_check.h>

#include <imgui.h>

#include <cfloat>

namespace ui
{
    namespace
    {
#if __has_include("whats_new.inc")
#include "whats_new.inc"
#else
        constexpr const char kWhatsNewEntry[] = "";
#endif

        struct Note
        {
            std::string title;
            std::vector<version::NoteLine> lines;
            bool firstFrame = true;
        };

        void Close(const char* how)
        {
            fprintf(stderr, "[ui] What's new closed (%s)\n", how);
            SetModal(nullptr);
        }

        void Draw(Note& note)
        {
            float s = Scale();
            ImGuiViewport* viewport = ImGui::GetMainViewport();
            float width = std::min(viewport->WorkSize.x - 2 * kSpaceL * s, 760.0f * s);
            float maxHeight = std::min(viewport->WorkSize.y - 2 * kSpaceL * s, 640.0f * s);
            ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0), ImVec2(width, maxHeight));
            constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                ImGuiWindowFlags_AlwaysAutoResize;
            if (note.firstFrame || !ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow))
                ImGui::SetNextWindowFocus();
            if (!ImGui::Begin("What's new", nullptr, flags))
            {
                ImGui::End();
                return;
            }
            TitleText(note.title.c_str());
            ImGui::Spacing();

            // The entry, scrolled by the D-pad, the stick or the arrow keys
            // when it doesn't fit (the button below keeps the focus).
            float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
            float room = maxHeight - ImGui::GetCursorPosY() - footer - ImGui::GetStyle().WindowPadding.y;
            float lineHeight = ImGui::GetTextLineHeightWithSpacing();
            ImGui::SetNextWindowSizeConstraints(ImVec2(0, lineHeight), ImVec2(FLT_MAX, std::max(room, lineHeight * 3)));
            if (ImGui::BeginChild("entry", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY))
            {
                ImGui::PushTextWrapPos(0.0f);
                for (const version::NoteLine& line : note.lines)
                    switch (line.kind)
                    {
                    case version::NoteLine::Kind::Gap:
                        ImGui::Spacing();
                        break;
                    case version::NoteLine::Kind::Heading:
                        PushAccentText();
                        ImGui::TextWrapped("%s", line.text.c_str());
                        PopAccentText();
                        break;
                    case version::NoteLine::Kind::Bullet:
                        ImGui::Bullet();
                        ImGui::TextWrapped("%s", line.text.c_str());
                        break;
                    case version::NoteLine::Kind::Text:
                        ImGui::TextWrapped("%s", line.text.c_str());
                        break;
                    }
                ImGui::PopTextWrapPos();
                float step = lineHeight * 0.5f;
                if (ImGui::IsKeyDown(ImGuiKey_GamepadDpadDown) || ImGui::IsKeyDown(ImGuiKey_GamepadLStickDown) || ImGui::IsKeyDown(ImGuiKey_DownArrow))
                    ImGui::SetScrollY(ImGui::GetScrollY() + step);
                if (ImGui::IsKeyDown(ImGuiKey_GamepadDpadUp) || ImGui::IsKeyDown(ImGuiKey_GamepadLStickUp) || ImGui::IsKeyDown(ImGuiKey_UpArrow))
                    ImGui::SetScrollY(ImGui::GetScrollY() - step);
            }
            ImGui::EndChild();

            ImGui::Spacing();
            if (note.firstFrame)
                ImGui::SetKeyboardFocusHere();
            bool close = ImGui::Button("OK");
            ImGui::SameLine();
            {
                const char* legend = "A or B: close";
                float w = ImGui::CalcTextSize(legend).x;
                ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - w));
                ImGui::TextDisabled("%s", legend);
            }
            note.firstFrame = false;
            const char* how = close ? "OK" : nullptr;
            if (!how && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
                how = "B or Esc";
            else if (!how && ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false))
                how = "Start";
            ImGui::End();
            if (how)
                Close(how);
        }
    }

    std::string_view WhatsNewEntry()
    {
        return kWhatsNewEntry;
    }

    void ShowWhatsNew(std::string_view version)
    {
        if (!Ready())
        {
            fprintf(stderr, "[ui] What's new: no UI to show it in\n");
            return;
        }
        auto note = std::make_shared<Note>();
        note->title = "Updated to v" + std::string(version);
        note->lines = version::ParseNote(WhatsNewEntry());
        if (note->lines.empty())
            return;
        fprintf(stderr, "[ui] What's new: v%.*s (%zu lines)\n", int(version.size()), version.data(), note->lines.size());
        SetModal([note] { Draw(*note); });
    }
}
