// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Settings for the whole process, made once, first thing in main. Windows
// only (elsewhere nothing):
//
// - No system dialogs for drive errors (SetErrorMode). Asking an empty card
//   reader or a USB drive that isn't ready for its name (the drive list, the
//   image search, the installer's destinations) would otherwise show "There
//   is no disk in the drive" and wait for a click; the call fails instead,
//   and the drive is skipped. Crashes still go to the crash report.
//
// - UTF-8 in the console. The runtime's text is UTF-8 (the manifest makes it
//   the process code page: paths, the environment), but a console turns the
//   bytes it is given into characters with its own output code page, the
//   OEM one (437 on an English system): C:\Users\José printed as Jos├⌐. The
//   console's code page is UTF-8 while the program runs. A console shared
//   with the terminal it was started from gets its own back when main
//   returns or exit() is called; std::_Exit (the game's own exits) and a
//   crash leave it at UTF-8, which Windows 10 and 11 consoles show fine.
//
// - C runtime calls given an invalid argument (close(-1), a write to a
//   descriptor that isn't open, fclose(nullptr)) fail with an error, as on
//   POSIX, which the shared code was written for, and are logged. By
//   default the C runtime ends the process there and then (__fastfail),
//   with no crash report and no log line. (A /GS stack-cookie failure
//   still ends that way, in Windows Error Reporting: macOS checks the same
//   cookies, but its failure is an abort, which the crash report catches.)
//
// - No QuickEdit in a console window of the program's own (started from
//   Explorer, a shortcut or a scheduled task, not from a terminal): there a
//   click in the window starts a selection, and while it lasts the console
//   accepts no output, so the log's writer stops, and with it every thread
//   that logs (the game freezes). A terminal it shares keeps its settings.
//
// - Standard streams that work without a console. A build linked for the
//   Windows subsystem (SB_WINDOWS_GUI: the download, no console window)
//   and started from Explorer has no stdin, stdout or stderr: the C runtime
//   gives each a placeholder without a handle (fileno -2), and every write
//   to them fails, so the session log's tee (report/log.cpp), which makes
//   fd 2 its pipe, would get none of stderr's lines. Each stream without a
//   handle is reopened on its own number: on the console of the terminal it
//   was started from, if there is one (AttachConsole: --where, --install and
//   the log show there), else on NUL (the log has everything). stdin is
//   always NUL: the terminal's shell reads the console's input. Streams the
//   parent redirected (a pipe, a file) stay as they are.
//
// - The discrete GPU on a PC with two (a laptop's integrated one beside an
//   NVIDIA or AMD one): the exe exports NvOptimusEnablement and
//   AmdPowerXpressRequestHighPerformance, which those drivers read at start.
//   (The Vulkan device itself is the presenter's choice, which prefers a
//   discrete GPU too: video/gpu_choice.h.)
#pragma once

namespace platform
{
    void PrepareProcess();

#ifdef _WIN32
    // What PrepareProcess did with the standard streams, for the session
    // log: "" when they came from the parent (a console, or redirects),
    // else a sentence.
    const char* StandardStreamsNote();
#endif

#ifndef _WIN32
    inline void PrepareProcess()
    {
    }
#endif
}
