// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The first-run installer: shown instead of the game when no install is
// found (install::FindGameInstall), until the player has installed it from
// their own disc image or quits.
#pragma once
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

namespace ui
{
    class InstallerScreen
    {
    public:
        InstallerScreen();   // starts looking for disc images
        ~InstallerScreen();  // cancels and waits for any work under way
        void Draw();         // inside ui::BeginFrame (a modal)

        bool finished = false;  // installed, and the player chose to start
        bool quit = false;
        std::filesystem::path installedPath;

    private:
        struct State;
        std::unique_ptr<State> state;
    };

    // Main thread: shows the installer until the game is installed (returns
    // its folder) or the player quits or closes the window (nullopt).
    // `runFrame` is video::RunFrame: false once the window has closed.
    std::optional<std::filesystem::path> RunInstaller(const std::function<bool()>& runFrame);
}
