// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Everything a play tester hands over when something goes wrong, in one
// action (Settings > Advanced > Save Bug Report). Nothing is ever sent
// anywhere; the player sends the file.
//
//   - Session logs (log.cpp). Everything written to stderr also goes to
//     GetUserPath()/logs/<date>_<time>.log: fd 2 becomes a pipe, and a
//     thread copies it to the original stderr (a terminal still sees it)
//     and to the file. The newest five sessions are kept, each capped at
//     16 MB, and the last 64 KB also stay in memory for crash and hang
//     reports. Each write reaches the kernel as it happens, so a crash or a
//     SIGKILL loses at most what was in the pipe that instant.
//   - The build's identity (build_info.cpp: version, git commit, dirty flag,
//     build date, compiler, platform) and a system summary (system_info.cpp: OS,
//     CPU, RAM, GPU and driver, display, Game Mode, the game install, the
//     settings in effect) at the top of every log.
//   - Crash reports (crash.cpp): the crash handler writes
//     logs/crash-<session>.txt with async-signal-safe calls only; what it
//     needs is prepared at startup. The next launch tells the player once.
//   - Hang reports (watchdog.cpp): a watchdog notices the game has stopped
//     (no new frame, or the command processor in one WAIT_REG_MEM, for far
//     longer than any loading screen or movie) and writes
//     logs/hang-<session>-<n>.txt once per episode: every runtime thread's
//     stack, the command processor's last packets, the recent log.
//   - Bug reports (bug_report.cpp): one .zip on the Desktop (else the user
//     data folder; on iOS the Documents folder the Files app shows) with all
//     of that, settings.toml and a screenshot. Home-folder paths in it read
//     "~".
#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

#include <signal.h>

struct PPCContext;

namespace report
{
    // --- Session log (log.cpp) ---

    // Main thread, first thing in a game session (the install commands have
    // no session). NFSMW_SESSION_LOG=0: no files, stderr untouched.
    // NFSMW_LOG_MAX_KB caps each file (default 16384).
    void StartSessionLog();
    // After hostcpu::ReserveFastCore: the thread that copies stderr, so it
    // stays off the command processor's core. Until then output waits in
    // the pipe.
    void StartLogWriter();
    // Names this session's files: "2026-09-28_14-03-22".
    const char* SessionName();
    // GetUserPath()/logs.
    std::filesystem::path LogDirectory();
    // This session's log; empty when there is none.
    std::filesystem::path SessionLogPath();
    // Seconds since StartSessionLog (the test switches count from there).
    double SessionSeconds();
    // The last `maxLines` lines written to stderr this session, from memory
    // (so also past the file's cap). Empty without a session log.
    std::string RecentOutput(size_t maxLines);
    // Waits (a quarter of a second at most) until what was written to
    // stderr so far has been copied out. Async-signal-safe.
    void FlushLog();

    // --- Build identity (build_info.cpp) ---

    // The release version, from project() in the top-level CMakeLists.txt:
    // "0.1.0".
    const char* Version();
    // "fae4bd1" or "fae4bd1-dirty" ("unknown" outside a git checkout).
    const char* BuildId();
    // "v0.1.0 (fae4bd1)".
    const char* VersionString();
    // One line: the version and id, when it was built, compiler, platform,
    // build type: "v0.1.0 (fae4bd1), built 2026-10-07 21:30 UTC, clang
    // 21.1.0, macOS arm64, Release" (the log's first line).
    const char* BuildString();

    // --- System summary (system_info.cpp) ---

    // Parts only other modules know, set as startup reaches them: "GPU",
    // "Display", "Game". Any thread.
    void SetSystemItem(const char* key, std::string value);
    // OS, CPU, RAM, the parts above, NFSMW_* variables and the settings in
    // effect (and which ones an environment variable overrides).
    std::string SystemSummary();
    // Logs SystemSummary() as [system] lines, and keeps a copy for crash
    // reports.
    void LogSystemSummary();

    // --- Crash reports (crash.cpp) ---

    // Main thread, before the crash handler is installed: opens the report
    // file and prepares everything the handler can't do safely itself.
    void PrepareCrashReports();
    // The crash handler (a signal handler; async-signal-safe): logs the
    // crash and writes the report. The caller exits afterwards.
    void WriteCrashReport(int sig, siginfo_t* info, void* context);
    // Main thread, once toasts can show: tells the player, once, about a
    // crash report from an earlier session.
    void NoticePreviousCrash();

    // --- Threads and hangs (watchdog.cpp) ---

    // SetHostThreadName: the thread's stack goes into hang reports.
    void RegisterThread(const char* name);
    // GuestThreadContext: the thread's guest registers (lr, r1) go into
    // crash and hang reports. nullptr when it ends.
    void SetThreadGuestContext(PPCContext* ctx);
    // Once the game's entry point is running. Also arms the test switches:
    //   NFSMW_TEST_CRASH=<s>[:abort]  crash in VdSwap (the game's render
    //                                 thread) <s> seconds after launch;
    //                                 ":abort" aborts after a message instead
    //   NFSMW_TEST_HANG=<s>[+<d>]     from <s> seconds, the command processor's
    //                                 waits never pass (for <d> seconds, or for good)
    //   NFSMW_TEST_SUSPEND=<s>+<d>[b][,...]
    //                                 the app is inactive from <s> seconds for
    //                                 <d> seconds ("b": in the background too):
    //                                 SDL's app events, each delivered twice, as
    //                                 on iOS (scripts/test_suspend.sh)
    //   NFSMW_TEST_SUSPEND_GPU_ONLY=1 inactive holds only the GPU, as before the
    //                                 game's clocks could stop (the test's red
    //                                 baseline)
    //   NFSMW_HANG_SECS=<f>[,<w>]     thresholds: no new frame for <f> s, one
    //                                 WAIT_REG_MEM for <w> s (0: no watchdog;
    //                                 the test switches stay armed)
    void StartWatchdog();

    // Fed by hot paths (relaxed; costs nothing that shows).
    inline std::atomic<uint64_t> g_fileBytesRead{ 0 };   // NtReadFile
    inline std::atomic<bool> g_testCrash{ false };       // VdSwap calls TestCrash()
    inline std::atomic<bool> g_testHang{ false };        // WAIT_REG_MEM never passes
    [[noreturn]] void TestCrash();

    // --- Bug reports (bug_report.cpp) ---

    enum class BugReportState : uint8_t { Idle, Saving, Saved, Failed };
    // Main thread (ui.cpp): the settings menu opened or closed. The frame
    // under the menu is captured as it opens, so a bug report's screenshot
    // shows the game as it was, not paused under the menu; closing lets it go.
    void NoteMenuOpened();
    void NoteMenuClosed();
    // Main thread (the settings menu): takes that frame (or the next one)
    // and writes the archive on a thread of its own; a toast says where it went.
    void SaveBugReport();
    BugReportState GetBugReportState();
    // "Saved to ~/Desktop/speedbreaker-bug-report-v0.1.0-....zip" (on iOS,
    // "Saved to the Files app, SpeedBreaker folder: ..."), or why it failed.
    std::string BugReportMessage();

    // HOME (and its real path) as "~", for text that leaves the machine.
    std::string ScrubHome(std::string text);
}
