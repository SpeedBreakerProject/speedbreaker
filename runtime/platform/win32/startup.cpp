// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See platform/startup.h.
#include <platform/startup.h>
#include <platform/os.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <io.h>

// Read by NVIDIA's (Optimus) and AMD's (PowerXpress) drivers from the exe
// when it starts: run on the discrete GPU (platform/startup.h).
extern "C"
{
    __declspec(dllexport) DWORD NvOptimusEnablement = 1;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

namespace platform
{
    namespace
    {
        UINT s_terminalCodePage = 0;
        const char* s_streamsNote = "";

        // Standard streams without a handle (the Windows subsystem, started
        // without a console or redirects): see platform/startup.h.
        void BindStandardStreams()
        {
            if (_fileno(stdin) >= 0 && _fileno(stdout) >= 0 && _fileno(stderr) >= 0)
                return;
            const bool console = AttachConsole(ATTACH_PARENT_PROCESS) != 0;
            struct Standard
            {
                FILE* stream;
                int fd;
                DWORD id;
                const char* mode;
                const char* device;
            };
            const Standard streams[] = {
                { stdin, 0, STD_INPUT_HANDLE, "r", "NUL" },
                { stdout, 1, STD_OUTPUT_HANDLE, "w", console ? "CONOUT$" : "NUL" },
                { stderr, 2, STD_ERROR_HANDLE, "w", console ? "CONOUT$" : "NUL" },
            };
            bool moved = false;
            for (const Standard& s : streams)
            {
                if (_fileno(s.stream) >= 0)
                    continue;
                // The placeholder holds the number: free it, so the reopen
                // (which takes the lowest free one) lands on it. fd 2 must
                // be stderr's, as the log's tee replaces fd 2.
                _close(s.fd);
                FILE* reopened = nullptr;
                if (freopen_s(&reopened, s.device, s.mode, s.stream) != 0)
                    freopen_s(&reopened, "NUL", s.mode, s.stream);
                if (_fileno(s.stream) == s.fd)
                    SetStdHandle(s.id, reinterpret_cast<HANDLE>(_get_osfhandle(s.fd)));
                else
                    moved = true;
            }
            s_streamsNote = moved ? "no console, and stdout or stderr couldn't be reopened on its own number: lines may be "
                                    "missing from this log"
                : console ? "no console of its own (the Windows subsystem): output also goes to the console of the "
                            "terminal it was started from"
                          : "no console (the Windows subsystem, started from Explorer or a shortcut): output goes to "
                            "this log only";
        }

        void RestoreConsoleCodePage()
        {
            SetConsoleOutputCP(s_terminalCodePage);
        }

        // The C runtime's report of a call it refused. Written straight to
        // the stderr handle (the log, once it runs), not through stdio: the
        // refused call may be stdio's own. Return addresses in the program
        // are offsets into it (exe+...), for its .map or .pdb; the C runtime
        // is linked in (/MT), so the first few are its own.
        void OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t)
        {
            static std::atomic<int> s_reported{ 0 };
            if (s_reported.fetch_add(1, std::memory_order_relaxed) >= 10)
                return;
            void* frames[8];
            USHORT count = RtlCaptureStackBackTrace(1, 8, frames, nullptr);
            HMODULE exe = GetModuleHandleW(nullptr);
            char line[512];
            int size = snprintf(line, sizeof(line), "[crt] the C runtime refused a call (an invalid argument); it failed "
                "with an error, as on POSIX. Called from");
            for (USHORT i = 0; i < count && size > 0 && size < int(sizeof(line)) - 24; i++)
            {
                HMODULE module = nullptr;
                bool inExe = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    static_cast<LPCWSTR>(frames[i]), &module) && module == exe;
                size += inExe ? snprintf(line + size, sizeof(line) - size, " exe+0x%llX", static_cast<unsigned long long>(
                                    reinterpret_cast<uintptr_t>(frames[i]) - reinterpret_cast<uintptr_t>(exe)))
                              : snprintf(line + size, sizeof(line) - size, " %p", frames[i]);
            }
            if (size > 0 && size < int(sizeof(line)) - 1)
            {
                line[size++] = '\n';
                DWORD written = 0;
                WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, DWORD(size), &written, nullptr);
            }
        }
    }

    void PrepareProcess()
    {
        SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
        _set_invalid_parameter_handler(OnInvalidParameter);
        BindStandardStreams();

        UINT codePage = GetConsoleOutputCP();
        if (codePage == 0)
            return;  // no console (the release build, started from Explorer)
        // Only this process in the console: a window of its own.
        DWORD processes[2];
        bool ownConsole = GetConsoleProcessList(processes, 2) == 1;
        if (codePage != CP_UTF8 && SetConsoleOutputCP(CP_UTF8) && !ownConsole)
        {
            s_terminalCodePage = codePage;
            atexit(RestoreConsoleCodePage);
        }
        HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
        DWORD mode = 0;
        if (ownConsole && input && input != INVALID_HANDLE_VALUE && GetConsoleMode(input, &mode) && (mode & ENABLE_QUICK_EDIT_MODE))
            SetConsoleMode(input, (mode & ~DWORD(ENABLE_QUICK_EDIT_MODE)) | ENABLE_EXTENDED_FLAGS);
    }

    const char* StandardStreamsNote()
    {
        return s_streamsNote;
    }
}
