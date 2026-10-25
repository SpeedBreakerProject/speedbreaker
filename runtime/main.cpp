// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Startup: find the installed game (or run the first-run installer), load
// its default.xex into guest memory, wire the imports, and run the game's
// entry point on a guest thread while the main thread runs the window.
//
//   SpeedBreaker                       the installed game (install::FindGameInstall)
//   SpeedBreaker path/to/default.xex   a specific XEX (development)
//   SpeedBreaker --install <image>     and the other commands in install/install_cli.h

#include "stdafx.h"
#include "kernel/arena.h"
#include "kernel/memory_status.h"
#include "kernel/module.h"
#include "kernel/write_watch.h"
#include "kernel/dispatcher.h"
#include "cpu/guest_time.h"
#include <video/presenter.h>
#include <gpu/renderer.h>
#include <game/hfr.h>
#include <hid/input.h>
#include <cpu/guest_thread.h>
#include <cpu/host_cpu.h>
#include <install/install_cli.h>
#include <install/locate.h>
#include <install/placement.h>
#include <install/sha256.h>
#include <platform/quit_signals.h>
#include <report/report.h>
#include <ui/installer_screen.h>
#include <ui/ui.h>
#include <user/paths.h>
#include <user/settings.h>
#include <user/version_check.h>

#include <SDL3/SDL.h>
#if defined(__APPLE__) && TARGET_OS_IOS
#include <SDL3/SDL_main.h>  // SDL starts UIKit and calls main() from it
#include <platform/ios_files.h>
#include <platform/ios_ui.h>
#endif

#include <cerrno>
#include <csignal>
#include <map>
#ifndef _WIN32
#include <unistd.h>
#endif
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <unordered_map>

#include <platform/fault.h>
#include <platform/startup.h>
#include <platform/thread.h>
#include <platform/timer.h>

#include <file.h>
#include <image.h>
#include <xex.h>

extern std::unordered_map<size_t, const char*> XamExports;
extern std::unordered_map<size_t, const char*> XboxKernelExports;

Memory g_memory;

namespace
{
    constexpr size_t DEFAULT_GUEST_STACK = 1024 * 1024;
    constexpr size_t HOST_STACK = 64 * 1024 * 1024;  // each guest call is a host call

    uint32_t ReadBE32(const uint8_t* p)
    {
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    }

    // Walks the import-library header on the raw file. Xex2LoadImage has
    // already replaced function thunks (type 1) with stubs and byteswapped
    // every record to host order. Type-0 records are pointer slots: a slot
    // whose ordinal is a function gets that function's thunk address (so
    // indirect calls through it hit our stub); a data export gets a zeroed
    // guest block the runtime owns.
    void WireImports(const std::vector<uint8_t>& file)
    {
        auto* header = static_cast<const uint8_t*>(getOptHeaderPtr(file.data(), XEX_HEADER_IMPORT_LIBRARIES));
        if (header == nullptr)
            return;

        uint32_t stringTableSize = ReadBE32(header + 4);
        uint32_t numLibraries = ReadBE32(header + 8);
        std::vector<std::string> names;
        const char* strings = reinterpret_cast<const char*>(header + 12);
        for (size_t i = 0, off = 0; i < numLibraries; i++)
        {
            names.emplace_back(strings + off);
            off += ((names.back().size() + 1) + 3) & ~size_t(3);
        }

        size_t functionSlots = 0, dataExports = 0;
        const uint8_t* lib = header + 12 + stringTableSize;
        for (uint32_t i = 0; i < numLibraries; i++)
        {
            uint16_t nameIndex = (lib[0x24] << 8) | lib[0x25];
            uint16_t count = (lib[0x26] << 8) | lib[0x27];
            const std::string& libName = names[nameIndex];
            const auto* table = libName == "xam.xex" ? &XamExports
                : libName == "xboxkrnl.exe" ? &XboxKernelExports : nullptr;

            // First pass: ordinal -> thunk address for this library's functions.
            // The loader overwrote function thunks, so recover their ordinals
            // from the raw (still encrypted-free) header order: in a XEX, a
            // function's type-0 slot record precedes its type-1 thunk record.
            std::map<uint32_t, uint32_t> thunkByOrdinal;
            uint32_t pendingOrdinal = UINT32_MAX;
            for (uint16_t im = 0; im < count; im++)
            {
                uint32_t addr = ReadBE32(lib + 0x28 + im * 4);
                auto* rec = static_cast<uint8_t*>(g_memory.Translate(addr));
                if (ReadBE32(rec) == 0x60000000)  // our nop/blr stub (big-endian nop)
                {
                    if (pendingOrdinal != UINT32_MAX)
                        thunkByOrdinal[pendingOrdinal] = addr;
                    pendingOrdinal = UINT32_MAX;
                    continue;
                }
                uint32_t data;
                memcpy(&data, rec, 4);  // host order after the loader's swap
                pendingOrdinal = data & 0xFFFF;
            }

            for (uint16_t im = 0; im < count; im++)
            {
                uint32_t addr = ReadBE32(lib + 0x28 + im * 4);
                auto* rec = static_cast<uint8_t*>(g_memory.Translate(addr));
                if (ReadBE32(rec) == 0x60000000)
                    continue;

                uint32_t data;
                memcpy(&data, rec, 4);
                uint32_t ordinal = data & 0xFFFF;

                uint32_t value;
                if (auto it = thunkByOrdinal.find(ordinal); it != thunkByOrdinal.end())
                {
                    value = it->second;
                    functionSlots++;
                }
                else
                {
                    // Data export (KeTimeStampBundle, ExLoadedCommandLine, ...).
                    // Real contents come in Phase 4; a zeroed block keeps early
                    // reads from faulting.
                    value = arena::Alloc(0x1000);
                    dataExports++;
                    const char* name = nullptr;
                    if (table != nullptr)
                        if (auto n = table->find(ordinal); n != table->end())
                            name = n->second;
                    std::string_view shortName = name ? name : "?";
                    if (shortName.starts_with("__imp__"))
                        shortName.remove_prefix(7);
                    if (!module::InitializeDataExport(std::string(shortName).c_str(), value))
                        fprintf(stderr, "[loader] data import %s!%.*s (ordinal %u) has no initialiser (zeroed)\n",
                            libName.c_str(), int(shortName.size()), shortName.data(), ordinal);
                }

                uint32_t be = ByteSwap(value);
                memcpy(rec, &be, 4);
            }

            lib += ReadBE32(lib);
        }

        fprintf(stderr, "[loader] wired %zu function import slots, %zu data imports\n", functionSlots, dataExports);
    }

    struct GuestMainParams
    {
        uint32_t entry;
        uint32_t stackSize;
    };
    std::atomic<bool> s_guestReturned{ false };  // the game's entry point returned

    void* GuestMain(void* arg)
    {
        auto* params = static_cast<GuestMainParams*>(arg);
        GuestThread::SetDefaultStackSize(params->stackSize);
        SetHostThreadName("nfsmw-main");
        if (g_memory.FindFunction(params->entry) == nullptr)
        {
            fprintf(stderr, "[runtime] no recompiled function at the entry point\n");
            report::FlushLog();
            std::_Exit(2);
        }
        fprintf(stderr, "[runtime] calling entry %08X\n", params->entry);
        uint32_t result = GuestThread::Start({ params->entry, 0, 0 });
        fprintf(stderr, "[runtime] entry point returned %08X\n", result);
        s_guestReturned = true;
        return nullptr;
    }

    // default.xex in a game folder, whatever its case (a tool may have
    // written DEFAULT.XEX; Linux file systems are case-sensitive).
    std::filesystem::path FindXex(const std::filesystem::path& dir)
    {
        std::filesystem::path exact = dir / "default.xex";
        std::error_code ec;
        if (std::filesystem::exists(exact, ec))
            return exact;
        for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        {
            std::string name = it->path().filename().string();
            if (name.size() == 11 && std::equal(name.begin(), name.end(), "default.xex",
                    [](char a, char b) { return std::tolower(uint8_t(a)) == b; }))
                return it->path();
        }
        return exact;
    }

#ifndef _WIN32
#if defined(__linux__) && defined(__aarch64__)
    // Whether a data abort was a write: ESR_EL1's ISS bit 6 (WnR), which the
    // kernel passes in an esr_context record among those after the
    // registers (asm/sigcontext.h; declared here, as that header clashes
    // with glibc's). No record: a write, which unprotects.
    bool FaultWasWrite(const ucontext_t* uc)
    {
        struct Record { uint32_t magic, size; };
        constexpr uint32_t kEsrMagic = 0x45535201;
        const uint8_t* p = uc->uc_mcontext.__reserved;
        const uint8_t* end = p + sizeof(uc->uc_mcontext.__reserved);
        while (p + sizeof(Record) + sizeof(uint64_t) <= end)
        {
            Record head;
            memcpy(&head, p, sizeof(head));
            if (head.magic == 0 || head.size < sizeof(Record))
                break;
            if (head.magic == kEsrMagic)
            {
                uint64_t esr;
                memcpy(&esr, p + sizeof(Record), sizeof(esr));
                return (esr >> 6) & 1;
            }
            p += head.size;
        }
        return true;
    }
#endif

    void CrashHandler(int sig, siginfo_t* info, void* context)
    {
        // A store to a write-watched page (textures), or an access to one
        // the GPU still uses: unprotect (after waiting) and retry.
        bool isWrite = true;
#if defined(__APPLE__) && defined(__aarch64__)
        // ESR_EL1 data abort, ISS bit 6: WnR.
        isWrite = (static_cast<ucontext_t*>(context)->uc_mcontext->__es.__esr >> 6) & 1;
#elif defined(__linux__) && defined(__x86_64__)
        // Page-fault error code, bit 1: the access was a write.
        isWrite = (static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_ERR] & 2) != 0;
#elif defined(__linux__) && defined(__aarch64__)
        isWrite = FaultWasWrite(static_cast<ucontext_t*>(context));
#else
        (void)context;
#endif
#if defined(__APPLE__) && defined(__aarch64__)
        writewatch::t_faultPc = reinterpret_cast<const void*>(static_cast<ucontext_t*>(context)->uc_mcontext->__ss.__pc);
#elif defined(__linux__) && defined(__x86_64__)
        writewatch::t_faultPc = reinterpret_cast<const void*>(static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_RIP]);
#elif defined(__linux__) && defined(__aarch64__)
        writewatch::t_faultPc = reinterpret_cast<const void*>(static_cast<ucontext_t*>(context)->uc_mcontext.pc);
#endif
        if ((sig == SIGSEGV || sig == SIGBUS) && writewatch::HandleFault(info->si_addr, isWrite))
            return;
        // A real crash: the log, and a crash report the next launch mentions
        // (report/crash.cpp; async-signal-safe).
        report::WriteCrashReport(sig, info, context);
        _exit(128 + sig);
    }
#endif
}

int main(int argc, char** argv)
{
    // Windows: no drive-error dialogs, UTF-8 in the console
    // (platform/startup.h), and 1 ms sleeps instead of the 15.6 ms tick
    // (platform/timer.h).
    platform::PrepareProcess();
    platform::RaiseTimerResolution();
#if defined(__APPLE__) && TARGET_OS_IOS
    // iOS starts an app in /, which it can't write. Relative paths (test
    // output in build/, a ./game/files development copy) mean the app's
    // Documents folder instead: the one Finder and the Files app show.
    if (const char* home = std::getenv("HOME"))
        chdir((std::string(home) + "/Documents").c_str());
#endif
    // Which folder the game runs from, before anything changes directory:
    // the installer never puts the game inside it (install/placement.h), and
    // a portable.txt there keeps all of SpeedBreaker's data in it
    // (user/portable.h).
    install::SetProgramPath(argc > 0 ? argv[0] : nullptr);
    // Before anything uses the per-user folders: portable mode or not, and
    // then (not in portable mode) nfsmw-recomp's folders become
    // SpeedBreaker's (user/paths.h). A portable folder that can't be written
    // (a read-only drive, a locked card) is still used: the game plays, and
    // the player is told settings and saves won't be kept; never a quiet
    // switch to the home folder, which would split saves between two places.
    const portable::Layout& portableLayout = portable::Detect(install::BundleFolder());
    const int portableError = portable::CheckWritable(portableLayout);
    std::string migrated = MigrateUserFolders() + portable::Describe(portableLayout);
    if (portableError)
        migrated += std::format("[paths] portable mode: {} can't be written ({}): settings and saves won't be kept\n",
            portableLayout.data.string(), strerror(portableError));
    // The name volume mixers and the desktop show (before SDL starts).
    SDL_SetAppMetadata("SpeedBreaker", report::BuildString(), nullptr);
    if (std::optional<int> code = install::RunInstallCommand(argc, argv))
    {
        fputs(migrated.c_str(), stderr);
        return *code;
    }

    // stderr also goes to GetUserPath()/logs/<date>_<time>.log from here on
    // (report/log.cpp); its copying thread starts after the core split.
    report::StartSessionLog();
    fputs(migrated.c_str(), stderr);
    hostcpu::ReserveFastCore();  // before any other thread starts
    report::StartLogWriter();
    report::RegisterThread("main (window)");

    // The guest's 4 GB (kernel/memory.cpp, reserved before main): where it
    // went, or why it couldn't, and on iOS what the app's signature allows
    // (kernel/memory_status.h). For the log, the system summary and, when
    // the game can't start, the page that says why.
    memstatus::Status memory;
    memory.base = reinterpret_cast<uintptr_t>(g_memory.base);
    memory.failure = memstatus::Failure(g_memory.base ? 0 : std::max(g_memory.failedStep, 1));
    memory.error = g_memory.failedError;
    memstatus::Gather(memory);
    fprintf(stderr, "%s\n", memstatus::LogLine(memory).c_str());
    report::SetSystemItem("Memory", memstatus::SummaryItem(memory));
    if (g_memory.base == nullptr)
    {
        // An iOS copy signed without Extended Virtual Addressing (a free
        // Apple Account's sideload) ends up here: say so on screen rather
        // than vanish. Before the game's code runs at all.
        memstatus::Message message = memstatus::StartupFailure(memory);
        fprintf(stderr, "[runtime] could not reserve 4 GB of guest address space: %s\n", message.title.c_str());
        for (const std::string& p : message.paragraphs)
            fprintf(stderr, "[runtime]   %s\n", p.c_str());
        settings::Load();
        if (video::Initialize())
        {
            ui::RunStartupError(message.title, message.paragraphs, message.details, [] { return video::RunFrame(); });
            video::Shutdown();
        }
        else if (const char* h = std::getenv("NFSMW_HEADLESS"); !h || h[0] != '1')
        {
            // No window for the page: the system's own message box.
            std::string text;
            for (const std::string& p : message.paragraphs)
                text += p + "\n\n";
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, message.title.c_str(), (text + message.details).c_str(), nullptr);
        }
        report::FlushLog();
        return 1;
    }

    writewatch::Initialize();
    // Waits parked without a timeout while the game was suspended wake once
    // it's back (from guesttime's own thread: see guest_time.h).
    guesttime::SetResumeHook(dispatcher::WakeAfterResume);

    report::PrepareCrashReports();
#ifdef _WIN32
    // A vectored handler for the write watch, the crash report for the rest.
    platform::InstallFaultHandlers();
#else
    struct sigaction sa{};
    sa.sa_sigaction = CrashHandler;
    sa.sa_flags = SA_SIGINFO;
    for (int sig : { SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE, SIGABRT })
        sigaction(sig, &sa, nullptr);
#endif
    // SIGTERM, SIGINT and SIGHUP quit the game as its window's close button
    // does (platform/quit_signals.h; Windows: SIGINT and SIGBREAK, Ctrl+C and
    // Ctrl+Break in a console build's console): before SDL starts, which
    // leaves them to us.
    quitsignals::Install();
    // From here on a return from main() is a clean end too (an unknown
    // option, a cancelled install, a XEX that can't be read): the session's
    // crash marker goes (report.h). The quit paths' std::_Exit runs no
    // destructors: they clear it themselves.
    struct ClearMarkerOnReturn
    {
        ~ClearMarkerOnReturn() { report::ClearCrashMarker(); }
    } clearMarkerOnReturn;

    // Anything else starting with '-' is a mistake, not a XEX path.
    if (argc > 1 && argv[1][0] == '-')
    {
        fprintf(stderr, "[runtime] unknown option %s. Usage:\n"
                        "  %s                          run the installed game (or install it)\n"
                        "  %s path/to/default.xex      run a specific XEX (development)\n"
                        "  %s --install <image|folder> [--dest <dir>] | --verify <image|folder> | --find-images | --where\n",
            argv[1], argv[0], argv[0], argv[0]);
        return 2;
    }

#if defined(__APPLE__) && TARGET_OS_IOS
    // Frame Rate on an iPhone (an A-series GPU; MoltenVK names the Vulkan
    // device after this Metal one): 30 up to the A17 Pro, Auto after it. An
    // iPhone 15 Pro Max (A17 Pro) needs ~20 ms of GPU a frame in a busy race
    // even before it warms up: Auto dropped it to 30 a minute in, and its
    // player found the steady 30 better than the 45-55 at 60 (and the
    // switching messages a distraction). Newer GPUs start at 60 under
    // Auto. iPads keep 60, the A-series ones too (iPad mini, iPad): 30 and
    // Auto are a choice there.
    if (std::string gpu = platform::ios::GpuName(); platform::ios::IsPhone() && gpu.starts_with("Apple A"))
    {
        int generation = std::atoi(gpu.c_str() + 7);  // "Apple A17 Pro GPU" -> 17
        bool older = generation > 0 && generation <= 17;
        settings::SetDeviceDefault(settings::Id::FrameRate, older ? 30 : settings::kFrameRateAuto, "an iPhone, " + gpu);
    }
#endif
    bool settingsLoaded = settings::Load();
    hfr::Init();  // Smooth Motion, for this run
    // Which version ran last: a line in the log (so a bug report shows when a
    // problem began), the "What's new" note below, and this version recorded
    // as the last (user/version_check.h).
    version::Change versionChange = version::Change::Same;
    std::string lastVersion;  // the last session's ("": not known)
    if (settingsLoaded)
    {
        std::string last = lastVersion = settings::LastVersion();
        versionChange = version::Classify(last, settings::FileFound(), report::Version());
        fprintf(stderr, "[version] %s\n", version::Describe(versionChange, last, report::Version()).c_str());
        if (versionChange != version::Change::Same)
            settings::RecordVersion(report::Version());
    }
    else
        fprintf(stderr, "[version] v%s (the last version isn't known: settings.toml didn't load)\n", report::Version());
    // The window must live on the main thread (macOS); the game gets its own.
    bool windowed = video::Initialize();
#if defined(_WIN32) && defined(SB_WINDOWS_GUI)
    // The Windows-subsystem build (the download) has no console: without
    // a window there would be nothing to see at all, the game running on
    // unseen (the headless mode, meant for tests) or ending without a word.
    // Say so in a box instead; the log has why.
    if (const char* headless = std::getenv("NFSMW_HEADLESS"); !windowed && !(headless && headless[0] == '1'))
    {
        fprintf(stderr, "[runtime] no window: a message box says so, then the game ends\n");
        report::FlushLog();
        // Windows' own box: SDL's may need the video driver that just failed.
        int answer = MessageBoxA(nullptr,
            "SpeedBreaker couldn't start its graphics. It needs a graphics card with Vulkan 1.2 and an up-to-date "
            "driver: update your graphics driver and try again.\n\nThe game's log is in %LOCALAPPDATA%\\SpeedBreaker\\logs.",
            "SpeedBreaker", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        fprintf(stderr, "[runtime] message box closed (%d)\n", answer);
        return 1;
    }
#endif
    if (!settingsLoaded)
        ui::Toast(settings::LastError(), 12.0);  // a mistake in settings.toml: it was kept as .bad
    if (portableError)
        ui::Toast(std::format("SpeedBreaker's folder can't be written ({}): settings and saves won't be kept this time. "
            "Move the folder to a drive it can write to.", strerror(portableError)), 12.0);
    // iOS: a copy without Increased Memory Limit (a free Apple Account's
    // sideload) starts, but iOS may close it in a busy scene: say so once.
    if (std::string notice = memstatus::LowMemoryNotice(memory); !notice.empty())
    {
        fprintf(stderr, "[memory] %s\n", notice.c_str());
        ui::Toast(notice, 12.0);
    }
    // A crash marker left by a session of this version or a later one is a
    // session that ended without quitting; v0.1.5 and older left theirs after
    // every session. iOS ends apps without a word (none quits there).
#if defined(__APPLE__) && TARGET_OS_IOS
    report::NoticePreviousCrash(false);
#else
    report::NoticePreviousCrash(!lastVersion.empty() && version::Compare(lastVersion, report::Version()) >= 0);
#endif

#ifndef _WIN32
    // NFSMW_LIFELINE_PID (scripts/steamos/speedbreaker.sh): the launcher's PID. The
    // game runs in a container, not as the launcher's child, so Steam
    // stopping the launcher never reaches it: exit once the launcher is gone.
    if (const char* v = std::getenv("NFSMW_LIFELINE_PID"))
        if (pid_t lifeline = pid_t(std::atoi(v)); lifeline > 1)
            std::thread([lifeline] {
                while (kill(lifeline, 0) == 0 || errno != ESRCH)
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                fprintf(stderr, "[runtime] launcher (pid %d) is gone: exiting\n", int(lifeline));
                if (settings::Dirty())
                    settings::Save();
                gpu::renderer::Shutdown();
                report::ClearCrashMarker();
                report::FlushLog();
                std::_Exit(0);
            }).detach();
#endif

    std::filesystem::path xexPath;
    std::string gameOrigin;  // for the system summary
    if (argc > 1)
    {
        // A specific XEX (development): its folder is the disc, unless
        // NFSMW_GAME_DIR says otherwise.
        xexPath = argv[1];
        if (!std::getenv("NFSMW_GAME_DIR"))
            SetGamePath(std::filesystem::absolute(xexPath).parent_path());
        gameOrigin = "the XEX on the command line";
    }
    else if (std::optional<install::GameInstall> game = install::FindGameInstall())
    {
        SetGamePath(game->path);
        xexPath = game->xex;  // as spelled on disk
        gameOrigin = game->origin == install::InstallOrigin::Environment ? "NFSMW_GAME_DIR"
            : game->origin == install::InstallOrigin::Development ? "./game/files"
            : game->marker ? "installed by the game's installer" : "installed";
#if defined(__APPLE__) && TARGET_OS_IOS
        // The installer keeps its install out of iCloud and computer backups,
        // but one made by an earlier build wasn't: mark it now (the same mark
        // again is harmless). Only the installer's own, in SpeedBreaker's
        // folder: never a folder set up by hand.
        if (game->origin == install::InstallOrigin::User && game->marker)
        {
            std::error_code ec;
            std::filesystem::path user = std::filesystem::absolute(GetUserPath(), ec).lexically_normal();
            std::filesystem::path inside = game->path.lexically_normal().lexically_relative(user);
            if (!inside.empty() && *inside.begin() != ".." && *inside.begin() != ".")
                platform::ios::ExcludeFromBackup(game->path);
        }
#endif
    }
    else if (windowed)
    {
        // First run: install from the player's disc image.
        std::optional<std::filesystem::path> installed = ui::RunInstaller([] { return video::RunFrame(); });
        if (!installed)
        {
            if (settings::Dirty())
                settings::Save();
            video::Shutdown();
            return 0;
        }
        SetGamePath(*installed);
        xexPath = FindXex(*installed);
        gameOrigin = "installed by the game's installer just now";
    }
    else
    {
        fprintf(stderr, "[runtime] the game isn't installed. Install it from your disc image with:\n"
                        "  %s --install <image.iso or extracted folder>\n", argv[0]);
        return 1;
    }
    fprintf(stderr, "[runtime] game files: %s\n", GetGamePath().string().c_str());

    std::vector<uint8_t> file = LoadFile(xexPath);
    if (file.empty())
    {
        fprintf(stderr, "[runtime] cannot read %s\n", xexPath.string().c_str());
        return 1;
    }

    Image image = Xex2LoadImage(file.data(), file.size());
    if (image.base != PPC_IMAGE_BASE || image.size > PPC_IMAGE_SIZE)
    {
        fprintf(stderr, "[runtime] %s doesn't match this build (base %08zX size %X; expected %08llX size <= %llX)\n",
            xexPath.string().c_str(), image.base, image.size, PPC_IMAGE_BASE, PPC_IMAGE_SIZE);
        return 1;
    }
    memcpy(g_memory.Translate(image.base), image.data.get(), image.size);
    fprintf(stderr, "[loader] image %08zX-%08zX, entry %08zX\n", image.base, image.base + image.size, image.entry_point);

    // Which game files this is, then the whole system summary for the log.
    {
        std::string sha = install::ToHex(install::Sha256Of(file.data(), file.size()));
        const install::Manifest& manifest = install::GameManifest();
        report::SetSystemItem("Game", std::format("{} ({}); default.xex {} bytes, SHA-256 {} ({}); manifest {}",
            GetGamePath().string(), gameOrigin, file.size(), sha, sha == manifest.Xex().sha256 ? "the supported version" : "NOT the supported version",
            manifest.version));
    }
    report::LogSystemSummary();

    g_userHeap.Init();
    module::Initialize(file, uint32_t(image.base), image.size, uint32_t(image.entry_point));
    WireImports(file);

    GuestMainParams params{ uint32_t(image.entry_point), DEFAULT_GUEST_STACK };
    if (auto* stack = static_cast<const be<uint32_t>*>(getOptHeaderPtr(file.data(), XEX_HEADER_DEFAULT_STACK_SIZE)))
        params.stackSize = std::max<uint32_t>(*stack, DEFAULT_GUEST_STACK);

    platform::Thread thread{};
    platform::StartThread(&thread, HOST_STACK, GuestMain, &params);
    report::StartWatchdog();  // hang reports (and the NFSMW_TEST_* switches)

#ifdef __linux__
    // NFSMW_ALLOW_DEBUGGER=1: let gdb/perf attach with Yama ptrace_scope 1.
    if (const char* v = std::getenv("NFSMW_ALLOW_DEBUGGER"); v && v[0] == '1')
        prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
#endif

    if (!windowed)
    {
        // No window (NFSMW_HEADLESS=1): the game runs until its entry point
        // returns, or until a requested termination, which ends it as the
        // window's quit path below does.
        while (!s_guestReturned.load() && !quitsignals::Requested())
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (int sig = quitsignals::Requested())
        {
            fprintf(stderr, "[runtime] %s: quitting\n", quitsignals::Name(sig));
            if (settings::Dirty())
                settings::Save();
            gpu::renderer::Shutdown();
            report::ClearCrashMarker();
            report::FlushLog();
            std::_Exit(0);
        }
        platform::JoinThread(thread);
        report::FlushLog();
        return 0;
    }
    // The first launch of a newer version: what's new in it, once.
    // NFSMW_WHATS_NEW=1 shows it every launch (testing), =0 never.
    {
        const char* force = std::getenv("NFSMW_WHATS_NEW");
        bool show = force && *force ? force[0] == '1' : version::ShowWhatsNew(versionChange, ui::WhatsNewEntry());
        if (show)
            ui::ShowWhatsNew(report::Version());
    }
    // iOS: the "needs a controller" notice, a few seconds into the game
    // (not over the installer).
    hid::NoteGameStarting();
    while (video::RunFrame())
        ;
    if (int sig = quitsignals::Requested())
        fprintf(stderr, "[runtime] %s: quitting, as the window's close button does\n", quitsignals::Name(sig));
    fprintf(stderr, "[runtime] window closed\n");
    if (settings::Dirty())
        settings::Save();
    gpu::renderer::Shutdown();
    video::Shutdown();
    // A clean end: no crash marker left (a crash in the shutdown above still
    // reported one).
    report::ClearCrashMarker();
    // The session log's last lines are still in its pipe: let them out
    // before _Exit takes the thread that copies them.
    report::FlushLog();
    std::_Exit(0);
}
