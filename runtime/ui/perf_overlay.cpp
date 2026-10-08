// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The performance overlay (Settings > Advanced): frame rate and frame times
// as presented, and the per-frame cost of the command processor (the CPU
// side of rendering) and of the GPU.
#include <stdafx.h>
#include "ui.h"

#include <gpu/renderer.h>

#include <imgui.h>

namespace ui
{
    namespace
    {
        constexpr int kHistory = 180;
        float s_frameMs[kHistory] = {};  // presented-frame intervals, a ring
        int s_frameNext = 0;
        int s_frameCount = 0;
        std::chrono::steady_clock::time_point s_lastFrame{};
    }

    void NoteGameFrame()
    {
        auto now = std::chrono::steady_clock::now();
        if (s_lastFrame.time_since_epoch().count() != 0)
        {
            float ms = std::chrono::duration<float, std::milli>(now - s_lastFrame).count();
            s_frameMs[s_frameNext] = std::min(ms, 250.0f);
            s_frameNext = (s_frameNext + 1) % kHistory;
            s_frameCount = std::min(s_frameCount + 1, kHistory);
        }
        s_lastFrame = now;
    }

    void DrawPerfOverlay()
    {
        float s = Scale();
        const ImVec2 work = ImGui::GetMainViewport()->WorkPos;  // the safe area's corner
        ImGui::SetNextWindowPos(ImVec2(work.x + kSpaceL * s, work.y + kSpaceL * s), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.62f);
        // Compact, for its smaller font: the text's side inset matches the
        // space its line box leaves above it.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kSpaceM * s, kSpaceS * s));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(kSpaceS * s, kSpaceXS * s));
        ImGui::PushFont(nullptr, 16.0f);
        constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove;
        if (ImGui::Begin("##perf", nullptr, flags))
        {
            // The last second of presented frames (or all we have).
            float sum = 0.0f, worst = 0.0f;
            int n = 0;
            for (int i = 1; i <= s_frameCount && sum < 1000.0f; i++)
            {
                float ms = s_frameMs[(s_frameNext - i + kHistory) % kHistory];
                sum += ms;
                worst = std::max(worst, ms);
                n++;
            }
            float average = n ? sum / n : 0.0f;
            PushAccentText();
            ImGui::Text("%.0f fps", average > 0 ? 1000.0f / average : 0.0f);
            PopAccentText();
            ImGui::SameLine();
            ImGui::TextDisabled("%.1f ms  (worst %.1f)", average, worst);

            // Oldest first, for the plot.
            float ordered[kHistory];
            for (int i = 0; i < s_frameCount; i++)
                ordered[i] = s_frameMs[(s_frameNext - s_frameCount + i + kHistory) % kHistory];
            // The plot's box keeps a line's gap from the text around it.
            ImGui::Spacing();
            ImGui::PlotLines("##frames", ordered, s_frameCount, 0, nullptr, 0.0f, 50.0f, ImVec2(220 * s, 36 * s));
            ImGui::Spacing();

            gpu::renderer::PerfNumbers perf = gpu::renderer::GetPerfNumbers();
            ImGui::TextDisabled("CPU render %.1f ms   GPU %.1f ms", perf.cpMs, perf.gpuMs);
            ImGui::TextDisabled("%.0f draws per frame", perf.draws);
        }
        ImGui::End();
        ImGui::PopFont();
        ImGui::PopStyleVar(2);
    }
}
