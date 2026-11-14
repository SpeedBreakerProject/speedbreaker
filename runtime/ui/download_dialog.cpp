// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See ui.h
// (DownloadDialog).
#include <stdafx.h>
#include "ui.h"
#include "quit_game.h"

#include <download/download.h>
#include <install/result.h>  // FormatSize
#include <update/release_names.h>  // SB_APP_NAME

#include <imgui.h>

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_timer.h>

#include <chrono>
#include <cstdio>
#include <format>
#include <string>

namespace ui
{
    namespace
    {
        constexpr const char* kPopup = "Download dialog";

        using download::Phase;
        using download::Status;
        using Clock = std::chrono::steady_clock;

        // What the dialog shows on top of the job phase.
        enum class Step
        {
            Job,       // job phase as it is
            Question,  // Done: the dialog question
            Restart,   // Done: a restart applies it
            Finished,  // Done: nothing more to ask (OK)
        };

        bool s_openRequested = false;
        bool s_showing = false;
        DownloadDialog s_dialog;
        Step s_step = Step::Job;
        std::string s_focusKey;

        // Download rate: an average since the download started.
        Clock::time_point s_downloadStart{};
        uint64_t s_downloadStartBytes = 0;

        void FocusOnce(const std::string& key)
        {
            if (s_focusKey != key)
            {
                s_focusKey = key;
                ImGui::SetKeyboardFocusHere();
            }
        }

        void Legend(const char* text)
        {
            ImGui::SameLine();
            float w = ImGui::CalcTextSize(text).x;
            const float right = ImGui::GetWindowContentRegionMax().x - w;
            ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), right));
            ImGui::TextDisabled("%s", text);
        }

        std::string Duration(double seconds)
        {
            if (seconds < 60)
                return std::format("{} s", std::max(1, int(seconds + 0.5)));
            int minutes = int(seconds / 60 + 0.5);
            return minutes < 60
                ? std::format("{} min", minutes)
                : std::format("{} h {} min", minutes / 60, minutes % 60);
        }

        void Wrapped(const std::string& text)
        {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(text.c_str());
            ImGui::PopTextWrapPos();
        }

        void PhaseLine(const std::string& text)
        {
            PushAccentText();
            ImGui::TextUnformatted(text.c_str());
            PopAccentText();
            ImGui::Spacing();
        }

        // e.g. "HD HUD Mod v2"
        std::string Named(const Status& st)
        {
            return st.version.empty() ? s_dialog.name : s_dialog.name + " " + st.version;
        }

        void Start()
        {
            if (s_dialog.start)
                s_dialog.start();
            s_step = Step::Job;
            s_downloadStart = {};
        }

        void Quit()
        {
            fprintf(stderr,
                "[download] dialog: Quit to apply %s (as closing the window does)\n",
                s_dialog.name.c_str());
            SDL_Event e{};
            e.type = SDL_EVENT_QUIT;
            e.quit.timestamp = SDL_GetTicksNS();
            if (!SDL_PushEvent(&e))
            {
                fprintf(stderr, "[download] dialog: couldn't queue the quit: %s\n",
                    SDL_GetError());
                Toast("Couldn't quit. Close the game's window instead.");
            }
        }

        Step AfterQuestion()
        {
            const bool restart = s_dialog.restart && s_dialog.restart();
            return restart ? Step::Restart : Step::Finished;
        }

        // True: close the dialog.
        bool DrawContacting(const Status& st)
        {
            const std::string host = st.host.empty() ? "the server" : st.host;
            PhaseLine(std::format("Contacting {}...", host));
            ImGui::ProgressBar(-float(ImGui::GetTime()), ImVec2(-1, 0), "");
            ButtonRowAtBottom();
            FocusOnce("contacting");
            bool back = ImGui::Button("Back");
            Legend("A: back");
            return back;
        }

        bool DrawReady(const Status& st)
        {
            Wrapped(std::format("{}: {} to download.", Named(st),
                install::FormatSize(st.size)));
            if (st.have > 0)
                ImGui::TextDisabled("%s is already downloaded.",
                    install::FormatSize(st.have).c_str());
            if (!s_dialog.have.empty())
                ImGui::TextDisabled("%s", s_dialog.have.c_str());
            ButtonRowAtBottom();
            FocusOnce("ready");
            if (ImGui::Button(st.have > 0 ? "Resume" : "Download"))
            {
                fprintf(stderr, "[download] dialog: %s %s\n",
                    st.have > 0 ? "Resume" : "Download", Named(st).c_str());
                s_downloadStart = {};
                download::Download();
            }
            ImGui::SameLine();
            bool back = ImGui::Button("Back");
            Legend("D-pad: choose   A: select");
            return back;
        }

        void DrawDownloading(const Status& st)
        {
            if (s_downloadStart.time_since_epoch().count() == 0)
            {
                s_downloadStart = Clock::now();
                s_downloadStartBytes = st.have;
            }
            PhaseLine(st.stopping ? "Stopping..." : "Downloading...");
            const float s = Scale();
            const float done = st.size ? float(double(st.have) / double(st.size)) : 0.0f;
            ImGui::ProgressBar(done, ImVec2(-1, 28 * s));
            std::string line = std::format("{} of {}", install::FormatSize(st.have),
                install::FormatSize(st.size));
            double seconds =
                std::chrono::duration<double>(Clock::now() - s_downloadStart).count();
            double rate =
                seconds > 1.0 ? double(st.have - s_downloadStartBytes) / seconds : 0.0;
            if (rate > 0)
            {
                line += std::format("   {}/s", install::FormatSize(uint64_t(rate)));
                if (st.size > st.have)
                    line += "   about " + Duration(double(st.size - st.have) / rate)
                        + " left";
            }
            ImGui::TextUnformatted(line.c_str());
            ButtonRowAtBottom();
            FocusOnce("downloading");
            ImGui::BeginDisabled(st.stopping);
            if (ImGui::Button(st.stopping ? "Cancelling..." : "Cancel"))
            {
                fprintf(stderr, "[download] dialog: Cancel\n");
                download::Stop();
            }
            ImGui::EndDisabled();
        }

        void DrawChecking(const Status& st)
        {
            PhaseLine("Checking the download...");
            const float s = Scale();
            const float done = st.size ? float(double(st.have) / double(st.size)) : 0.0f;
            ImGui::ProgressBar(done, ImVec2(-1, 28 * s), "");
        }

        void DrawInstalling(const Status& st)
        {
            PhaseLine("Installing...");
            const float s = Scale();
            const float done =
                st.files ? float(double(st.filesDone) / double(st.files)) : 0.0f;
            ImGui::ProgressBar(done, ImVec2(-1, 28 * s), "");
            if (st.files)
                ImGui::TextDisabled("(%llu of %llu files)",
                    (unsigned long long)std::min(st.filesDone + 1, st.files),
                    (unsigned long long)st.files);
        }

        bool DrawFailed(const Status& st)
        {
            Wrapped(st.message);
            ButtonRowAtBottom();
            FocusOnce("failed " + st.message);
            bool retry = ImGui::Button("Try again");
            ImGui::SameLine();
            bool back = ImGui::Button("Back");
            Legend("D-pad: choose   A: select");
            if (retry)
            {
                fprintf(stderr, "[download] dialog: Try again\n");
                Start();
            }
            return back;
        }

        // Text and buttons for Done, by step. True: close.
        bool DrawAfterDone(const Status& st)
        {
            if (s_step == Step::Job)
            {
                const bool ask = s_dialog.question && !s_dialog.question().empty();
                s_step = ask ? Step::Question : AfterQuestion();
            }
            PushAccentText();
            Wrapped(std::format("{} is downloaded.", Named(st)));
            PopAccentText();
            ImGui::Spacing();
            switch (s_step)
            {
            case Step::Question:
            {
                Wrapped(s_dialog.question());
                ButtonRowAtBottom();
                FocusOnce("question");
                bool yes = ImGui::Button(s_dialog.yes.c_str());
                ImGui::SameLine();
                bool later = ImGui::Button("Not now");
                Legend("D-pad: choose   A: select");
                if (yes)
                {
                    if (s_dialog.confirm)
                        s_dialog.confirm();
                    s_step = AfterQuestion();
                }
                return later;
            }
            case Step::Restart:
            {
                Wrapped(SB_APP_NAME " should be restarted to apply changes.");
                ButtonRowAtBottom();
                FocusOnce("restart");
#if SB_QUIT_GAME
                bool quit = ImGui::Button("Quit");
                ImGui::SameLine();
                bool stay = ImGui::Button("Stay");
                Legend("D-pad: choose   A: select");
                if (quit)
                    Quit();
                return quit || stay;
#else
                bool ok = ImGui::Button("OK");
                Legend("A: back");
                return ok;
#endif
            }
            default:
            {
                if (!s_dialog.note.empty())
                    Wrapped(s_dialog.note);
                ButtonRowAtBottom();
                FocusOnce("finished");
                bool ok = ImGui::Button("OK");
                Legend("A: OK");
                return ok;
            }
            }
        }
    }

    void OpenDownloadDialog(DownloadDialog dialog)
    {
        s_dialog = std::move(dialog);
        fprintf(stderr, "[download] dialog: %s\n", s_dialog.title.c_str());
        Start();
        s_focusKey.clear();
        s_openRequested = true;
    }

    bool DrawDownloadDialog()
    {
        if (s_openRequested)
        {
            s_openRequested = false;
            ImGui::OpenPopup(kPopup);
        }
        // B and Esc do nothing here: stop ImGui from clearing the focused
        // button on them (the menu underneath keeps its own behaviour).
        ImGui::GetIO().ConfigNavEscapeClearFocusItem = !ImGui::IsPopupOpen(kPopup);
        if (!ImGui::IsPopupOpen(kPopup))
        {
            if (s_showing)
                ForgetDownloadDialog();
            return false;
        }

        const float s = Scale();
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        const float width = std::min(viewport->WorkSize.x - 2 * kSpaceL * s, 760.0f * s);
        const float height = std::min(viewport->WorkSize.y - 2 * kSpaceL * s, 300.0f * s);
        ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_Always,
            ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
        constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar
            | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar
            | ImGuiWindowFlags_NoScrollWithMouse;
        ImVec4 background = ImGui::GetStyleColorVec4(ImGuiCol_PopupBg);
        background.w = 1.0f;
        ImGui::PushStyleColor(ImGuiCol_PopupBg, background);
        const bool open = ImGui::BeginPopupModal(kPopup, nullptr, flags);
        ImGui::PopStyleColor();

        if (!open)
        {
            if (s_showing)
                ForgetDownloadDialog();
            return false;
        }
        s_showing = true;

        const Status st = download::GetStatus();
        TitleText(s_dialog.title.c_str());
        ImGui::Spacing();

        bool close = false;
        switch (st.phase)
        {
        case Phase::Idle:
        case Phase::Contacting:
            close = DrawContacting(st);
            break;
        case Phase::Ready:
            close = DrawReady(st);
            break;
        case Phase::Downloading:
            DrawDownloading(st);
            break;
        case Phase::Checking:
            DrawChecking(st);
            break;
        case Phase::Installing:
            DrawInstalling(st);
            break;
        case Phase::Done:
            close = DrawAfterDone(st);
            break;
        case Phase::Failed:
            close = DrawFailed(st);
            break;
        }
        if (close)
        {
            fprintf(stderr, "[download] dialog: closed\n");
            ImGui::CloseCurrentPopup();
            ForgetDownloadDialog();
        }
        ImGui::EndPopup();
        return true;
    }

    bool DownloadDialogOpen()
    {
        return ImGui::IsPopupOpen(kPopup);
    }

    void ForgetDownloadDialog()
    {
        if (s_showing || s_openRequested)
        {
            const Status st = download::GetStatus();
            if (st.phase == Phase::Downloading || st.phase == Phase::Checking
                || st.phase == Phase::Installing)
                fprintf(stderr,
                    "[download] dialog: closed with the menu: %s stopped, "
                    "the part kept\n",
                    st.label.c_str());
            download::Close();
        }

        s_openRequested = false;
        s_showing = false;
        if (ImGui::GetCurrentContext())
            ImGui::GetIO().ConfigNavEscapeClearFocusItem = true;
        s_step = Step::Job;
        s_downloadStart = {};
        s_dialog = {};
    }
}
