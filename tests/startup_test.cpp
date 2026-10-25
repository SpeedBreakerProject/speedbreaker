// platform::PrepareProcess (runtime/platform/startup.h), Windows only: the
// settings it makes for the whole process. Built and run by
// tests/CMakeLists.txt on Windows.
// - Drive errors come back to the caller as errors, with no system dialog
//   (SEM_FAILCRITICALERRORS).
// - A C runtime call with an invalid argument fails with errno set, as on
//   POSIX, and the process goes on. Without the handler the C runtime ends
//   the process on the spot (__fastfail, exit code 0xC0000409), and this
//   test never prints its result.
// - Standard streams without handles (a Windows-subsystem build started
//   from Explorer: stdin, stdout and stderr are placeholders, fileno -2)
//   come back on their own numbers, 0, 1 and 2, so fprintf(stderr) reaches
//   whatever fd 2 becomes, as the session log's tee makes it its pipe. A
//   child of this test started with no standard handles and no console
//   checks it, twice: started from here (it may attach this test's console,
//   when it has one), and through a relay without a console (NUL). Without
//   the reopen the child's line never reaches the pipe.
// (The console's code page needs a real console: the work notes' script
// p2_console.py runs SpeedBreaker in one and reads what it shows.)
#include <platform/startup.h>
#include <platform/os.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <io.h>
#include <string>

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

namespace
{
    // Starts this test again with `args`, no standard handles (none, not the
    // parent's) and no console of its own; inheritable handles (the pipe)
    // go along. Waits for it: its exit code, or -1.
    int StartBare(const std::string& args)
    {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring command = L"\"" + std::wstring(exe) + L"\" " + std::wstring(args.begin(), args.end());
        STARTUPINFOW si{ sizeof(si) };
        si.dwFlags = STARTF_USESTDHANDLES;  // all three null
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(exe, command.data(), nullptr, nullptr, TRUE, DETACHED_PROCESS, nullptr, nullptr, &si, &pi))
            return -1;
        CloseHandle(pi.hThread);
        DWORD code = DWORD(-1);
        WaitForSingleObject(pi.hProcess, 60000);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        return int(code);
    }

    void Send(HANDLE pipe, const std::string& text)
    {
        DWORD written = 0;
        WriteFile(pipe, text.data(), DWORD(text.size()), &written, nullptr);
    }

    // The bare child: what it was given, PrepareProcess, then the log's tee
    // in miniature (fd 2 becomes the pipe) and a line through stderr.
    int StreamsChild(HANDLE pipe)
    {
        int before[3] = { _fileno(stdin), _fileno(stdout), _fileno(stderr) };
        platform::PrepareProcess();
        int after[3] = { _fileno(stdin), _fileno(stdout), _fileno(stderr) };
        char line[512];
        snprintf(line, sizeof(line), "filenos before %d %d %d, after %d %d %d; note: %s\n", before[0], before[1], before[2], after[0],
            after[1], after[2], platform::StandardStreamsNote());
        Send(pipe, line);
        int fd = _open_osfhandle(reinterpret_cast<intptr_t>(pipe), _O_WRONLY | _O_BINARY);
        if (fd < 0 || _dup2(fd, 2) != 0)
        {
            Send(pipe, "the pipe couldn't become fd 2\n");
            return 1;
        }
        fprintf(stderr, "stderr reached the pipe\n");
        fflush(stderr);
        return before[2] == -2 && after[0] == 0 && after[1] == 1 && after[2] == 2 ? 0 : 1;
    }

    // Starts the bare child, through a relay without a console when asked;
    // what it wrote, and its exit code.
    std::string RunChild(bool relay, int& code)
    {
        SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
        HANDLE read = nullptr, write = nullptr;
        if (!CreatePipe(&read, &write, &sa, 0))
            return "CreatePipe failed";
        SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
        char args[64];
        snprintf(args, sizeof(args), "%s %llu", relay ? "--relay" : "--streams-child",
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(write)));
        code = StartBare(args);
        CloseHandle(write);
        std::string text;
        char buffer[512];
        DWORD got = 0;
        while (ReadFile(read, buffer, sizeof(buffer), &got, nullptr) && got > 0)
            text.append(buffer, got);
        CloseHandle(read);
        return text;
    }
}

int main(int argc, char** argv)
{
    if (argc == 3 && (strcmp(argv[1], "--streams-child") == 0 || strcmp(argv[1], "--relay") == 0))
    {
        HANDLE pipe = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(std::strtoull(argv[2], nullptr, 10)));
        if (strcmp(argv[1], "--streams-child") == 0)
            return StreamsChild(pipe);
        // The relay has no console, so the child can't attach one.
        return StartBare(std::string("--streams-child ") + argv[2]);
    }

    platform::PrepareProcess();

    printf("1. drive errors: no dialog\n");
    CHECK(GetErrorMode() & SEM_FAILCRITICALERRORS, "error mode 0x%X", GetErrorMode());

    printf("2. C runtime calls with an invalid argument fail, as on POSIX\n");
    fflush(stdout);
    errno = 0;
    int closed = _close(-1);
    CHECK(closed == -1 && errno == EBADF, "close(-1): %d, errno %d", closed, errno);
    char byte = 0;
    errno = 0;
    int got = _read(4000, &byte, 1);
    CHECK(got == -1 && errno == EBADF, "read(4000): %d, errno %d", got, errno);

    printf("3. standard streams without handles come back on 0, 1 and 2\n");
    for (bool relay : { false, true })
    {
        int code = -1;
        std::string text = RunChild(relay, code);
        printf("   %s: %s", relay ? "through a relay without a console" : "started from here", text.c_str());
        CHECK(code == 0, "the child's exit code %d", code);
        CHECK(text.find("stderr reached the pipe") != std::string::npos, "no line through stderr");
        if (relay)
            CHECK(text.find("note: no console (") != std::string::npos, "the relay's child found a console");
    }

    printf(g_failures ? "%d failure(s)\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
