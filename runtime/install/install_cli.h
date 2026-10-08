// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The installer on the command line, for scripts, SSH sessions and machines
// without the first-run UI:
//   SpeedBreaker --install <image|folder> [--dest <dir>]   copy and verify (default dest: GetUserPath()/game)
//   SpeedBreaker --verify <image|folder>                   hash-check every file without copying
//   SpeedBreaker --find-images [folder...]                 list disc images in the usual places (or these)
//   SpeedBreaker --where                                   print where the game install was found
// Ctrl-C cancels an install cleanly (the partial copy is removed).
#pragma once
#include <optional>

namespace install
{
    // If argv holds one of the commands above, runs it and returns the exit
    // code for main to return (0 success, 1 failure, 2 usage, 130 cancelled);
    // otherwise returns nullopt and the game starts as usual.
    std::optional<int> RunInstallCommand(int argc, char** argv);
}
