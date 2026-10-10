// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// macOS: the system file dialogs SDL shows (open panels, as sheets on the
// game's window), for automated runs of the installer screen.
#pragma once

namespace platform::file_dialog
{
    // Cancels every file dialog on screen, as its Cancel button would: SDL's
    // callback then gets the cancel, as from the player. Main thread only.
    // Returns how many there were. (NFSMW_TEST_DIALOG_CANCEL in
    // ui/installer_screen.cpp; macOS only.)
    int CancelOpenPanels();
}
