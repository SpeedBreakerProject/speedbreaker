// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See ui.h.
//
// Settings > Advanced > Check for Updates: the row's value and help, and the
// dialog over the menu with the result (update/updater.h runs the check).
//   - Checking: says so; Close leaves the check running (the row, the log
//     and a toast get the result).
//   - Up to date, or a one-line reason it couldn't check (and Try again).
//   - A newer version: its notes (plain text, scrolled by the D-pad), "Open
//     download page" (the system's browser) and the page as a QR code with
//     its address, for when no browser opens (Steam Deck Game Mode, the
//     Steam Frame's headset): a phone's camera reads it off the screen.
// Controller first, like the menu: left/right picks a button, A presses it,
// up/down scrolls the notes, B goes back to the menu.
#include <stdafx.h>
#include "ui.h"

#include <update/updater.h>

#include <thirdparty/qrcodegen/qrcodegen.h>

#include <imgui.h>

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_misc.h>

namespace ui
{
    namespace
    {
        constexpr const char* kPopup = "Check for Updates";

        bool s_openRequested = false;  // OpenUpdateDialog: open it inside the settings window, next frame
        bool s_showing = false;        // drawn last frame
        std::string s_focusKey;        // what the dialog showed when its button was last focused

        // The download page's QR code, made once per address.
        struct Qr
        {
            std::string text;
            int size = 0;               // modules per side; 0: none
            std::vector<uint8_t> dark;  // size * size, row by row
        };
        Qr s_qr;

        const Qr& QrFor(const std::string& text)
        {
            if (s_qr.text == text)
                return s_qr;
            s_qr = {};
            s_qr.text = text;
            std::vector<uint8_t> code(qrcodegen_BUFFER_LEN_MAX), temp(qrcodegen_BUFFER_LEN_MAX);
            // Medium error correction (15%) for a picture of a screen,
            // raised where the version has room; version 10 at most (57
            // modules), which a release page's address never comes near.
            if (qrcodegen_encodeText(text.c_str(), temp.data(), code.data(), qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN, 10,
                    qrcodegen_Mask_AUTO, true))
            {
                s_qr.size = qrcodegen_getSize(code.data());
                s_qr.dark.resize(size_t(s_qr.size) * size_t(s_qr.size));
                for (int y = 0; y < s_qr.size; y++)
                    for (int x = 0; x < s_qr.size; x++)
                        s_qr.dark[size_t(y) * size_t(s_qr.size) + size_t(x)] = qrcodegen_getModule(code.data(), x, y) ? 1 : 0;
                fprintf(stderr, "[update] QR code of %s: version %d, %d modules a side\n", text.c_str(), (s_qr.size - 17) / 4, s_qr.size);
            }
            else
                fprintf(stderr, "[update] QR code of %s: too long to encode\n", text.c_str());
            return s_qr;
        }

        // Dark modules on white with the four-module quiet zone the standard
        // asks for; every module the same whole number of points, at most
        // `side` points in all (at least 2 a module).
        void DrawQr(const Qr& qr, float side)
        {
            constexpr int kQuiet = 4;
            const int total = qr.size + 2 * kQuiet;
            const float m = std::max(2.0f, std::floor(side / float(total)));
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const ImVec2 origin(std::floor(at.x), std::floor(at.y));
            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(origin, ImVec2(origin.x + total * m, origin.y + total * m), IM_COL32_WHITE);
            for (int y = 0; y < qr.size; y++)
                for (int x = 0; x < qr.size;)
                {
                    if (!qr.dark[size_t(y) * size_t(qr.size) + size_t(x)])
                    {
                        x++;
                        continue;
                    }
                    int run = 1;  // a row's dark modules side by side are one rectangle
                    while (x + run < qr.size && qr.dark[size_t(y) * size_t(qr.size) + size_t(x + run)])
                        run++;
                    const ImVec2 a(origin.x + (kQuiet + x) * m, origin.y + (kQuiet + y) * m);
                    draw->AddRectFilled(a, ImVec2(a.x + run * m, a.y + m), IM_COL32_BLACK);
                    x += run;
                }
            ImGui::Dummy(ImVec2(total * m, total * m));
        }

        void OpenPage(const std::string& url)
        {
            if (const char* v = std::getenv("NFSMW_TEST_NO_BROWSER"); v && *v && *v != '0')
            {
                fprintf(stderr, "[update] Open download page: %s (NFSMW_TEST_NO_BROWSER: not opened)\n", url.c_str());
                return;
            }
            if (SDL_OpenURL(url.c_str()))
                fprintf(stderr, "[update] Open download page: %s (handed to the system)\n", url.c_str());
            else
            {
                fprintf(stderr, "[update] Open download page: %s failed: %s\n", url.c_str(), SDL_GetError());
                Toast("No browser opened. Scan the code with a phone, or go to " + url, 12.0);
            }
        }

        void Legend(const char* text)
        {
            ImGui::SameLine();
            float w = ImGui::CalcTextSize(text).x;
            ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - w));
            ImGui::TextDisabled("%s", text);
        }

        // The focused button, once per thing shown (the result replaces
        // "Checking..." under the player's thumb).
        void FocusOnce(const std::string& key)
        {
            if (s_focusKey != key)
            {
                s_focusKey = key;
                ImGui::SetKeyboardFocusHere();
            }
        }

        void DrawNotes(const update::Result& r, ImVec2 size)
        {
            const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
            if (ImGui::BeginChild("notes", size, ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened))
            {
                ImGui::PushTextWrapPos(0.0f);
                if (!r.name.empty() && r.name != "v" + r.latest && r.name != r.latest)
                {
                    PushAccentText();
                    ImGui::TextWrapped("%s", r.name.c_str());
                    PopAccentText();
                    ImGui::Spacing();
                }
                for (const version::NoteLine& line : r.notes.lines)
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
                if (r.notes.lines.empty())
                    ImGui::TextDisabled("This release has no notes.");
                else if (r.notes.truncated)
                {
                    ImGui::Spacing();
                    ImGui::TextDisabled("The rest is on the download page.");
                }
                ImGui::PopTextWrapPos();
                // The D-pad, the stick or the arrow keys scroll (the buttons keep the focus).
                const float step = lineHeight * 0.5f;
                if (ImGui::IsKeyDown(ImGuiKey_GamepadDpadDown) || ImGui::IsKeyDown(ImGuiKey_GamepadLStickDown) || ImGui::IsKeyDown(ImGuiKey_DownArrow))
                    ImGui::SetScrollY(ImGui::GetScrollY() + step);
                if (ImGui::IsKeyDown(ImGuiKey_GamepadDpadUp) || ImGui::IsKeyDown(ImGuiKey_GamepadLStickUp) || ImGui::IsKeyDown(ImGuiKey_UpArrow))
                    ImGui::SetScrollY(ImGui::GetScrollY() - step);
            }
            ImGui::EndChild();
        }

        // True: close the dialog.
        bool DrawAvailable(const update::Result& r)
        {
            const float s = Scale();
            TitleText(r.headline.c_str());
            ImGui::TextDisabled("%s", r.message.c_str());
            ImGui::Spacing();

            // The notes on the left, the code on the right, the address under both.
            const ImGuiStyle& style = ImGui::GetStyle();
            const float footer = ImGui::GetFrameHeight() + ImGui::GetTextLineHeightWithSpacing() * 2 + style.ItemSpacing.y * 2;
            const float body = std::max(ImGui::GetContentRegionAvail().y - footer, ImGui::GetTextLineHeightWithSpacing() * 4);
            const Qr& qr = QrFor(r.url);
            const float caption = ImGui::GetTextLineHeightWithSpacing() * 2;
            const float side = qr.size ? std::min(body - caption - style.ItemSpacing.y, 300.0f * s) : 0.0f;
            const float gutter = kSpaceL * s;
            const float notesWidth = ImGui::GetContentRegionAvail().x - (qr.size ? side + gutter : 0.0f);
            DrawNotes(r, ImVec2(notesWidth, body));
            if (qr.size)
            {
                ImGui::SameLine(0.0f, gutter);
                ImGui::BeginGroup();
                DrawQr(qr, side);
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + side);
                ImGui::TextDisabled("No browser? Point a phone's camera at the code.");
                ImGui::PopTextWrapPos();
                ImGui::EndGroup();
            }
            ImGui::TextDisabled("Download page: %s", r.url.c_str());

            ButtonRowAtBottom();
            FocusOnce("available " + r.latest);
            bool open = ImGui::Button("Open download page");
            ImGui::SameLine();
            bool close = ImGui::Button("Close");
            Legend("D-pad: scroll, choose   A: select   B: back");
            if (open)
                OpenPage(r.url);
            return close;
        }

        // Up to date, or why not: one line, and OK (and Try again).
        bool DrawShort(const update::Result& r, uint64_t checks)
        {
            TitleText(r.headline.c_str());
            ImGui::Spacing();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(r.message.c_str());
            if (r.outcome == update::Outcome::Unavailable)
                ImGui::TextDisabled("Releases: %s", update::kReleasesPage);
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            ImGui::Spacing();
            const bool failed = r.outcome != update::Outcome::UpToDate && r.outcome != update::Outcome::Unavailable;
            FocusOnce(std::format("result {}", checks));
            bool close = ImGui::Button("OK");
            bool retry = false;
            if (failed)
            {
                ImGui::SameLine();
                retry = ImGui::Button("Try again");
            }
            Legend(failed ? "D-pad: choose   A: select   B: back" : "A or B: back");
            if (retry)
                update::StartCheck();
            return close;
        }

        bool DrawChecking()
        {
            TitleText("Checking for updates");
            ImGui::Spacing();
            const int dots = int(ImGui::GetTime() * 2.0) % 4;
            ImGui::Text("Asking GitHub for the newest release%.*s", dots, "...");
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("It takes a few seconds at most. Only the question goes out: nothing about you or this device.");
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            ImGui::Spacing();
            FocusOnce("checking");
            bool close = ImGui::Button("Close");
            Legend("A or B: back (the check carries on)");
            return close;
        }
    }

    const char* UpdateRowValue()
    {
        static std::string text;
        update::Status status = update::GetStatus();
        if (status.phase == update::Phase::Checking)
            return "Checking...";
        if (!status.result)
            return "Check";
        switch (status.result->outcome)
        {
        case update::Outcome::UpToDate: return "Up to date";
        case update::Outcome::Available:
            text = "v" + status.result->latest + " is out";
            return text.c_str();
        default: return "Failed";
        }
    }

    void UpdateRowHelp()
    {
        ImGui::TextWrapped("Asks GitHub whether a newer SpeedBreaker is out. The game goes online only when you press this, "
                           "and sends nothing but the question.");
        update::Status status = update::GetStatus();
        if (status.phase == update::Phase::Checking)
            ImGui::TextDisabled("Checking...");
        else if (!status.result)
            ImGui::TextDisabled("This version: v%s", update::RunningVersion().c_str());
        else if (status.result->outcome == update::Outcome::Available)
        {
            PushAccentText();
            ImGui::Text("%s (you have v%s)", status.result->headline.c_str(), status.result->current.c_str());
            PopAccentText();
        }
        else if (status.result->outcome == update::Outcome::UpToDate)
            ImGui::TextDisabled("%s", status.result->headline.c_str());
        else
            ImGui::TextDisabled("%s", status.result->message.c_str());
    }

    void OpenUpdateDialog()
    {
        fprintf(stderr, "[update] Check for Updates pressed\n");
        update::StartCheck();
        s_openRequested = true;
    }

    bool DrawUpdateDialog()
    {
        if (s_openRequested)
        {
            s_openRequested = false;
            s_focusKey.clear();
            ImGui::OpenPopup(kPopup);
        }
        if (!ImGui::IsPopupOpen(kPopup))
        {
            if (s_showing)
                ForgetUpdateDialog();
            return false;
        }
        const float s = Scale();
        update::Status status = update::GetStatus();
        const update::Result* result = status.phase == update::Phase::Done ? status.result.get() : nullptr;
        const bool available = result && result->outcome == update::Outcome::Available;
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        const float width = std::min(viewport->WorkSize.x - 2 * kSpaceL * s, (available ? 900.0f : 760.0f) * s);
        const float height = available ? std::min(viewport->WorkSize.y - 2 * kSpaceL * s, 600.0f * s) : 0.0f;  // 0: fit
        ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
        constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        // Opaque: the menu's text mustn't show through the notes.
        ImVec4 background = ImGui::GetStyleColorVec4(ImGuiCol_PopupBg);
        background.w = 1.0f;
        ImGui::PushStyleColor(ImGuiCol_PopupBg, background);
        const bool open = ImGui::BeginPopupModal(kPopup, nullptr, flags);
        ImGui::PopStyleColor();
        if (!open)
        {
            if (s_showing)
                ForgetUpdateDialog();
            return false;
        }
        if (!s_showing)
        {
            s_showing = true;
            update::SetDialogShowing(true);
        }
        bool close;
        if (!result)
            close = DrawChecking();
        else if (available)
            close = DrawAvailable(*result);
        else
            close = DrawShort(*result, status.checks);
        if (!close && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
            close = true;
        if (close)
        {
            ImGui::CloseCurrentPopup();
            ForgetUpdateDialog();
        }
        ImGui::EndPopup();
        return true;
    }

    bool UpdateDialogOpen()
    {
        return ImGui::IsPopupOpen(kPopup);
    }

    void ForgetUpdateDialog()
    {
        s_openRequested = false;
        s_showing = false;
        update::SetDialogShowing(false);
    }
}
