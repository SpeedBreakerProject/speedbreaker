// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See report.h.
//
// The session log. A tee through a pipe rather than a hook on the stderr
// FILE: the drivers, SDL, the C library's own messages (an assert, a heap
// check) and a child process write to fd 2 directly. One thread copies the
// pipe to the original stderr, to the file and to a 64 KB ring in memory;
// each chunk reaches the kernel as soon as it is written, so nothing waits
// in a user-space buffer for a crash to lose. In the file and the ring each
// line starts with the seconds since launch ("[  612.3] "), so a tester's
// "it froze about ten minutes in" finds its place; the terminal gets the
// lines as they were written.
#include <stdafx.h>
#include "report.h"
#include "internal.h"

#include <cpu/guest_thread.h>
#include <user/paths.h>

#include <cerrno>
#include <ctime>

#include <fcntl.h>
#ifndef _WIN32
#include <pthread.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#include <platform/file_io.h>
#ifdef _WIN32
#include <platform/startup.h>
#endif

namespace report
{
    namespace
    {
        constexpr size_t kRingSize = 64 * 1024;
        constexpr uint64_t kDefaultCapBytes = 16ull << 20;
        constexpr size_t kKeepSessions = 5;

        char s_ring[kRingSize];
        std::atomic<uint64_t> s_ringTotal{ 0 };  // bytes ever appended; the ring holds the last kRingSize

        int s_pipeRead = -1;
        std::atomic<int> s_terminal{ -1 };  // the original stderr (-1 once it's gone)
        std::atomic<int> s_file{ -1 };
        uint64_t s_fileBytes = 0;  // writer thread only
        uint64_t s_capBytes = kDefaultCapBytes;
        std::atomic<bool> s_busy{ false };        // the writer holds a chunk it hasn't finished with
        std::atomic<uint64_t> s_chunks{ 0 };      // chunks finished
        std::atomic<bool> s_writerStarted{ false };
#ifdef _WIN32
        DWORD s_writer = 0;  // its thread id
#else
        pthread_t s_writer{};
#endif

        // fcntl(fd, F_DUPFD_CLOEXEC, 3): a copy of `fd` numbered 3 or more.
        int DupAbove2(int fd)
        {
#ifdef _WIN32
            // The C runtime's _dup takes the lowest free number: hold the low
            // ones until a copy lands above them.
            int held[3], heldCount = 0;
            int copy = _dup(fd);
            while (copy >= 0 && copy <= 2 && heldCount < 3)
            {
                held[heldCount++] = copy;
                copy = _dup(fd);
            }
            for (int i = 0; i < heldCount; i++)
                _close(held[i]);
            return copy;
#else
            return fcntl(fd, F_DUPFD_CLOEXEC, 3);
#endif
        }

#ifdef _WIN32
        // The pipe, as a named pipe whose reading end is overlapped: FlushLog
        // asks how much is waiting (PeekNamedPipe) while the writer thread's
        // read is pending, and on a synchronous handle Windows queues the
        // question behind that read, which waits for output that the asking
        // thread was about to write (a hang at exit). Both ends are C runtime
        // descriptors (the writing one becomes fd 2), not inherited.
        int MakeLogPipe(int fds[2])
        {
            static std::atomic<int> serial{ 0 };
            wchar_t name[96];
            swprintf(name, std::size(name), L"\\\\.\\pipe\\SpeedBreaker-log-%lu-%d", GetCurrentProcessId(), serial++);
            HANDLE reader = CreateNamedPipeW(name, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 0, 1 << 20, 0, nullptr);
            if (reader == INVALID_HANDLE_VALUE)
            {
                errno = EMFILE;
                return -1;
            }
            HANDLE writer = CreateFileW(name, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (writer == INVALID_HANDLE_VALUE)
            {
                CloseHandle(reader);
                errno = EMFILE;
                return -1;
            }
            fds[0] = _open_osfhandle(reinterpret_cast<intptr_t>(reader), _O_RDONLY | _O_NOINHERIT);
            fds[1] = _open_osfhandle(reinterpret_cast<intptr_t>(writer), _O_NOINHERIT);
            return 0;
        }

        // read(2) on the pipe's overlapped end: waits for data; 0 once every
        // writer has gone.
        ssize_t ReadPipe(int fd, void* buffer, size_t size)
        {
            static HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            HANDLE pipe = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
            OVERLAPPED at{};
            at.hEvent = done;
            DWORD got = 0;
            if (!ReadFile(pipe, buffer, DWORD(size), nullptr, &at) && GetLastError() != ERROR_IO_PENDING)
                return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
            if (!GetOverlappedResult(pipe, &at, &got, TRUE))
                return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
            return ssize_t(got);
        }
#endif

        char s_sessionName[64] = "";
        std::filesystem::path s_logPath;
        std::chrono::steady_clock::time_point s_start = std::chrono::steady_clock::now();

        void FileWrite(const char* data, size_t size)
        {
            int fd = s_file.load(std::memory_order_relaxed);
            if (fd < 0)
                return;
            if (s_fileBytes + size <= s_capBytes)
            {
                detail::WriteAll(fd, data, size);
                s_fileBytes += size;
                return;
            }
            // Whole lines up to the cap, then one notice. A runaway log can't
            // fill the disk; the ring (and so any crash or hang report) still
            // has the latest lines.
            size_t fit = s_capBytes > s_fileBytes ? size_t(s_capBytes - s_fileBytes) : 0;
            while (fit > 0 && data[fit - 1] != '\n')
                fit--;
            detail::WriteAll(fd, data, fit);
            static const char notice[] = "\n[log] this session's log reached its size cap (NFSMW_LOG_MAX_KB): later output is "
                                         "not saved here; crash and hang reports still get the last lines\n";
            detail::WriteAll(fd, notice, sizeof(notice) - 1);
            s_file.store(-1, std::memory_order_relaxed);  // stays open for the crash handler's direct writes
            s_fileBytes = s_capBytes;
        }

        int s_cappedFd = -1;  // the file after the cap: only the crash handler writes to it

        void RingAppend(const char* data, size_t size)
        {
            uint64_t total = s_ringTotal.load(std::memory_order_relaxed);
            if (size > kRingSize)
            {
                total += size - kRingSize;
                data += size - kRingSize;
                size = kRingSize;
            }
            size_t pos = size_t(total % kRingSize);
            size_t first = std::min(size, kRingSize - pos);
            memcpy(s_ring + pos, data, first);
            memcpy(s_ring, data + first, size - first);
            s_ringTotal.store(total + size, std::memory_order_release);
        }

        void Writer()
        {
            SetHostThreadName("nfsmw-log");
#ifndef _WIN32
            // A terminal that went away must not kill the game: with SIGPIPE
            // blocked here, the write fails with EPIPE instead. (Windows has
            // no SIGPIPE: the write just fails.)
            sigset_t set;
            sigemptyset(&set);
            sigaddset(&set, SIGPIPE);
            pthread_sigmask(SIG_BLOCK, &set, nullptr);
#endif
            static char buffer[16384];
            static char stamped[sizeof(buffer) * 13];  // room for a prefix on every byte
            bool lineStart = true;
            while (true)
            {
#ifdef _WIN32
                ssize_t n = ReadPipe(s_pipeRead, buffer, sizeof(buffer));
#else
                ssize_t n = read(s_pipeRead, buffer, sizeof(buffer));
#endif
                if (n < 0 && errno == EINTR)
                    continue;
                if (n <= 0)
                    return;
                s_busy.store(true, std::memory_order_release);
                char prefix[32];
                int prefixSize = snprintf(prefix, sizeof(prefix), "[%7.1f] ", SessionSeconds());
                prefixSize = std::clamp(prefixSize, 0, 12);
                size_t size = 0;
                for (ssize_t i = 0; i < n; i++)
                {
                    if (lineStart)
                    {
                        memcpy(stamped + size, prefix, size_t(prefixSize));
                        size += size_t(prefixSize);
                    }
                    stamped[size++] = buffer[i];
                    lineStart = buffer[i] == '\n';
                }
                if (int fd = s_terminal.load(std::memory_order_relaxed); fd >= 0)
                {
                    for (size_t done = 0; done < size_t(n);)
                    {
                        ssize_t w = write(fd, buffer + done, size_t(n) - done);
                        if (w < 0 && errno == EINTR)
                            continue;
                        if (w <= 0)
                        {
                            s_terminal.store(-1, std::memory_order_relaxed);  // closed: the file carries on alone
                            break;
                        }
                        done += size_t(w);
                    }
                }
                FileWrite(stamped, size);
                RingAppend(stamped, size);
                s_chunks.fetch_add(1, std::memory_order_release);
                s_busy.store(false, std::memory_order_release);
            }
        }

        // [first, last) of the ring: the last `maxLines` whole lines.
        void TailRange(size_t maxLines, uint64_t& first, uint64_t& last)
        {
            last = s_ringTotal.load(std::memory_order_acquire);
            uint64_t oldest = last > kRingSize ? last - kRingSize : 0;
            // Leave a margin the writer may be overwriting as we read.
            if (last > kRingSize)
                oldest += 4096;
            uint64_t i = last;
            size_t lines = 0;
            while (i > oldest)
            {
                if (s_ring[(i - 1) % kRingSize] == '\n' && i != last && ++lines == maxLines)
                    break;
                i--;
            }
            if (i == oldest && last > kRingSize)
            {
                // The oldest line may be cut: start at the next whole one.
                while (i < last && s_ring[i % kRingSize] != '\n')
                    i++;
                if (i < last)
                    i++;
            }
            first = i;
        }
    }

    namespace detail
    {
        SafeBuf& SafeBuf::Str(const char* s)
        {
            while (s && *s && size < capacity)
                data[size++] = *s++;
            return *this;
        }

        SafeBuf& SafeBuf::Str(const char* s, size_t n)
        {
            for (size_t i = 0; i < n && size < capacity; i++)
                data[size++] = s[i];
            return *this;
        }

        SafeBuf& SafeBuf::Char(char c)
        {
            if (size < capacity)
                data[size++] = c;
            return *this;
        }

        SafeBuf& SafeBuf::Dec(int64_t v)
        {
            char digits[24];
            int n = 0;
            uint64_t u = v < 0 ? uint64_t(0) - uint64_t(v) : uint64_t(v);
            do
                digits[n++] = char('0' + u % 10);
            while ((u /= 10) != 0);
            if (v < 0)
                Char('-');
            while (n > 0)
                Char(digits[--n]);
            return *this;
        }

        SafeBuf& SafeBuf::Hex(uint64_t v, int minDigits, bool upper)
        {
            char digits[16];
            int n = 0;
            do
            {
                digits[n++] = (upper ? "0123456789ABCDEF" : "0123456789abcdef")[v & 15];
                v >>= 4;
            } while (v != 0);
            while (n < minDigits && n < 16)
                digits[n++] = '0';
            while (n > 0)
                Char(digits[--n]);
            return *this;
        }

        void WriteAll(int fd, const void* data, size_t size)
        {
            if (fd < 0)
                return;
            const char* p = static_cast<const char*>(data);
#ifdef _WIN32
            // WriteFile on the descriptor's handle: the C runtime's write()
            // takes the descriptor's lock, which a crash may have left held.
            // (These descriptors are binary, and the log file is only ever
            // appended to through this one handle, so its position is its end.)
            HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
            if (handle == INVALID_HANDLE_VALUE)
                return;
            while (size > 0)
            {
                DWORD written = 0;
                if (!WriteFile(handle, p, DWORD(std::min<size_t>(size, 1u << 30)), &written, nullptr) || written == 0)
                    return;
                p += written;
                size -= written;
            }
#else
            while (size > 0)
            {
                ssize_t n = write(fd, p, size);
                if (n < 0 && errno == EINTR)
                    continue;
                if (n <= 0)
                    return;
                p += n;
                size -= size_t(n);
            }
#endif
        }

        int TerminalFd()
        {
            int fd = s_terminal.load(std::memory_order_relaxed);
            return s_pipeRead >= 0 ? fd : 2;  // no tee: stderr is still the terminal
        }

        bool LogCapped()
        {
            return s_file.load(std::memory_order_relaxed) < 0 && s_cappedFd >= 0;
        }

        int LogFileFd()
        {
            int fd = s_file.load(std::memory_order_relaxed);
            return fd >= 0 ? fd : s_cappedFd;
        }

        void WriteDirect(const char* data, size_t size)
        {
            WriteAll(TerminalFd(), data, size);
            WriteAll(LogFileFd(), data, size);
        }

        void WriteRecentOutput(int fd, size_t maxLines)
        {
            uint64_t first, last;
            TailRange(maxLines, first, last);
            while (first < last)
            {
                size_t pos = size_t(first % kRingSize);
                size_t n = size_t(std::min<uint64_t>(last - first, kRingSize - pos));
                WriteAll(fd, s_ring + pos, n);
                first += n;
            }
        }

        void KeepNewest(const std::string& dir, const char* prefix, const char* suffix, size_t keep)
        {
            std::vector<std::filesystem::path> files;
            std::error_code ec;
            for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
            {
                std::string name = it->path().filename().string();
                if (name.empty() || name[0] == '.' || !name.starts_with(prefix) || !name.ends_with(suffix))
                    continue;
                if (it->is_regular_file(ec))
                    files.push_back(it->path());
            }
            if (files.size() <= keep)
                return;
            std::sort(files.begin(), files.end());
            for (size_t i = 0; i + keep < files.size(); i++)
                std::filesystem::remove(files[i], ec);
        }

        std::string TimeStamp()
        {
            time_t now = time(nullptr);
            struct tm local{};
            localtime_r(&now, &local);
            char text[32];
            strftime(text, sizeof(text), "%Y-%m-%d_%H-%M-%S", &local);
            return text;
        }
    }

    void StartSessionLog()
    {
        s_start = std::chrono::steady_clock::now();
        std::string stamp = detail::TimeStamp();
        snprintf(s_sessionName, sizeof(s_sessionName), "%s", stamp.c_str());
        if (const char* v = std::getenv("NFSMW_SESSION_LOG"); v && v[0] == '0')
        {
            fprintf(stderr, "[build] SpeedBreaker %s\n", BuildString());
            fprintf(stderr, "[log] no session log (NFSMW_SESSION_LOG=0)\n");
            return;
        }
        if (const char* v = std::getenv("NFSMW_LOG_MAX_KB"))
            if (long kb = std::atol(v); kb > 0)
                s_capBytes = uint64_t(kb) << 10;

        // The file: <stamp>.log, or <stamp>-2.log if another instance started
        // in the same second. The newest kKeepSessions stay (with this one).
        std::filesystem::path dir = LogDirectory();
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        detail::KeepNewest(dir.string(), "", ".log", kKeepSessions - 1);
        int file = -1;
        for (int attempt = 1; attempt < 10 && file < 0; attempt++)
        {
            std::string name = attempt == 1 ? stamp : std::format("{}-{}", stamp, attempt);
            std::filesystem::path path = dir / (name + ".log");
            file = platform::OpenFile(path, O_WRONLY | O_CREAT | O_EXCL | O_APPEND | O_CLOEXEC, 0644);
            if (file >= 0)
            {
                snprintf(s_sessionName, sizeof(s_sessionName), "%s", name.c_str());
                s_logPath = path;
            }
            else if (errno != EEXIST)
                break;
        }
        if (file < 0)
            fprintf(stderr, "[log] can't create a session log in %s: %s\n", dir.string().c_str(), strerror(errno));
        else if (file <= 2)
        {
            // Launched with stderr (or stdin, stdout) closed, open() hands
            // out its number: as fd 2 the file would be replaced by the pipe
            // below, and the copying thread would feed the pipe to itself
            // forever (one core busy, the log growing without its cap).
            int moved = DupAbove2(file);
            close(file);
            file = moved;
        }

        // fd 2 becomes the pipe; the writer copies it on (StartLogWriter).
        int fds[2];
#ifdef _WIN32
        // Binary, not inherited, 1 MB: room for a burst (and for startup,
        // before the writer runs).
        if (MakeLogPipe(fds) != 0)
#else
        if (pipe(fds) != 0)
#endif
        {
            fprintf(stderr, "[build] SpeedBreaker %s\n", BuildString());
            fprintf(stderr, "[log] pipe: %s (no session log)\n", strerror(errno));
            if (file >= 0)
                close(file);
            return;
        }
        // With stdin, stdout or stderr closed, pipe() hands out their
        // numbers: move both ends above them before fd 2 is replaced.
        for (int& fd : fds)
            if (fd <= 2)
            {
                int moved = DupAbove2(fd);
                close(fd);
                fd = moved;
            }
        if (fds[0] < 0 || fds[1] < 0)
        {
            fprintf(stderr, "[build] SpeedBreaker %s\n", BuildString());
            fprintf(stderr, "[log] pipe: out of descriptors (no session log)\n");
            if (file >= 0)
                close(file);
            return;
        }
#ifndef _WIN32
        fcntl(fds[0], F_SETFD, FD_CLOEXEC);
#endif
#ifdef F_SETPIPE_SZ
        // Room for a burst (and for startup, before the writer runs).
        fcntl(fds[1], F_SETPIPE_SZ, 1 << 20);
#endif
        fflush(stderr);
        int terminal = DupAbove2(2);
        if (dup2(fds[1], 2) < 0)
        {
            fprintf(stderr, "[build] SpeedBreaker %s\n", BuildString());
            fprintf(stderr, "[log] dup2: %s (no session log)\n", strerror(errno));
            close(fds[0]);
            close(fds[1]);
            if (terminal >= 0)
                close(terminal);
            if (file >= 0)
                close(file);
            return;
        }
        close(fds[1]);
#ifdef _WIN32
        // Code that writes to the process's standard error handle rather
        // than to fd 2 (the C runtime's assert, a driver) reaches the pipe too.
        SetStdHandle(STD_ERROR_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(2)));
#endif
        // A write that failed before (stderr closed at launch: the
        // NFSMW_INPUT_SCRIPT line, any message before this) leaves the
        // stream's error flag set, and macOS's fprintf then writes nothing,
        // not even to the pipe (fputs still does): the log kept a handful
        // of lines out of hundreds.
        clearerr(stderr);
        s_pipeRead = fds[0];
        s_terminal.store(terminal, std::memory_order_relaxed);
        s_file.store(file, std::memory_order_relaxed);
        s_cappedFd = file;
        // The first lines of the file.
        fprintf(stderr, "[build] SpeedBreaker %s\n", BuildString());
        if (file >= 0)
            fprintf(stderr, "[log] session %s (pid %d): %s\n", s_sessionName, int(getpid()), s_logPath.string().c_str());
#ifdef _WIN32
        // The download (the Windows subsystem): where the console went.
        if (const char* note = platform::StandardStreamsNote(); note[0])
            fprintf(stderr, "[log] %s\n", note);
#endif
    }

    void StartLogWriter()
    {
        if (s_pipeRead < 0 || s_writerStarted.load())
            return;
        std::thread writer(Writer);
#ifdef _WIN32
        s_writer = GetThreadId(writer.native_handle());
#else
        s_writer = writer.native_handle();
#endif
        writer.detach();
        s_writerStarted.store(true, std::memory_order_release);
        // A return from main() or an exit() (a usage message, "the game
        // isn't installed") ends this thread with the process too: let the
        // pipe drain first, as every std::_Exit does, or the message never
        // reaches the terminal or the file.
        std::atexit(FlushLog);
    }

    const char* SessionName()
    {
        return s_sessionName;
    }

    std::filesystem::path LogDirectory()
    {
        return GetUserPath() / "logs";
    }

    std::filesystem::path SessionLogPath()
    {
        return s_logPath;
    }

    double SessionSeconds()
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - s_start).count();
    }

    std::string RecentOutput(size_t maxLines)
    {
        uint64_t first, last;
        TailRange(maxLines, first, last);
        std::string text;
        text.reserve(size_t(last - first));
        for (uint64_t i = first; i < last; i++)
            text.push_back(s_ring[i % kRingSize]);
        return text;
    }

    void FlushLog()
    {
#ifdef _WIN32
        if (s_pipeRead < 0 || !s_writerStarted.load(std::memory_order_acquire) || GetCurrentThreadId() == s_writer)
            return;
#else
        if (s_pipeRead < 0 || !s_writerStarted.load(std::memory_order_acquire) || pthread_equal(pthread_self(), s_writer))
            return;
#endif
        // Empty pipe and an idle writer, twice a millisecond apart with no
        // chunk finished in between (it may have just taken the last one).
        int quiet = 0;
        uint64_t chunks = s_chunks.load(std::memory_order_acquire);
        for (int i = 0; i < 250 && quiet < 2; i++)
        {
            int pending = 0;
#ifdef _WIN32
            DWORD available = 0;
            if (PeekNamedPipe(reinterpret_cast<HANDLE>(_get_osfhandle(s_pipeRead)), nullptr, 0, nullptr, &available, nullptr))
                pending = int(std::min<DWORD>(available, INT_MAX));
#else
            ioctl(s_pipeRead, FIONREAD, &pending);
#endif
            uint64_t now = s_chunks.load(std::memory_order_acquire);
            quiet = (pending == 0 && !s_busy.load(std::memory_order_acquire) && now == chunks) ? quiet + 1 : 0;
            chunks = now;
            struct timespec ms{ 0, 1000000 };
            nanosleep(&ms, nullptr);
        }
    }
}
