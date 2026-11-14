// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See download.h.
#include "download.h"
#include "unpack.h"

#include <install/result.h>  // FormatSize
#include <install/sha256.h>
#include <platform/file_io.h>
#include <update/fetch.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

namespace download
{
    namespace fs = std::filesystem;
    using update::Fetched;
    using update::Transport;

    struct JobData
    {
        uint64_t generation = 0;
        std::shared_ptr<std::atomic<bool>> cancel;
        std::shared_ptr<const Task> task;
        Target target;  // sha256 filled in by Contacting
        uint64_t size = 0;
    };

    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr size_t kChunk = 1 << 20;
        constexpr size_t kMaxSha256Bytes = 4 << 10;

        // Never destroyed: job threads are detached and can still be
        // winding down when the game exits.
        struct Shared
        {
            std::mutex mutex;
            Status status;
            uint64_t generation = 0;  // current job; older threads don't post
            std::shared_ptr<std::atomic<bool>> cancel;
            JobData ready;            // what Contacting found, used by Download
            std::timed_mutex work;    // held by a job thread while running, one at a time
        };
        Shared& S()
        {
            static Shared* shared = new Shared;
            return *shared;
        }

        std::atomic<bool> s_exiting = false;

        // On exit: stops the running job and gives it a moment to end the transfer.
        void OnExit()
        {
            Shared& s = S();
            s_exiting = true;
            {
                std::lock_guard lock(s.mutex);
                s.generation++;
                if (s.cancel)
                    *s.cancel = true;
            }
            if (s.work.try_lock_for(std::chrono::seconds(3)))
                s.work.unlock();
        }

        // Changes the status if `job` is still the current one.
        template <class F>
        bool Post(const JobData& job, F&& change)
        {
            Shared& s = S();
            std::lock_guard lock(s.mutex);
            if (s.generation != job.generation)
                return false;
            change(s.status);
            return true;
        }

        // A stop that ended the job before it posted
        // again: back to Ready, the part is kept.
        void Stopped(const JobData& job, uint64_t have)
        {
            Post(job, [&](Status& st) {
                st.phase = Phase::Ready;
                st.stopping = false;
                st.have = have;
            });
        }

        uint64_t FileSize(const fs::path& path)
        {
            std::error_code ec;
            const auto size = fs::file_size(path, ec);
            return ec ? 0 : uint64_t(size);
        }

        void Remove(const fs::path& path)
        {
            std::error_code ec;
            fs::remove(path, ec);
        }

        void RemoveIfEmpty(const fs::path& folder)
        {
            std::error_code ec;
            if (fs::is_directory(folder, ec) && fs::is_empty(folder, ec) && !ec)
                fs::remove(folder, ec);
        }

        std::string ReadSmallFile(const fs::path& path, size_t max)
        {
            std::ifstream file(path, std::ios::binary);
            std::string text(max, '\0');
            if (!file)
                return {};
            file.read(text.data(), std::streamsize(max));
            text.resize(size_t(file.gcount()));
            return text;
        }

        bool WriteSmallFile(const fs::path& path, std::string_view text)
        {
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file.write(text.data(), std::streamsize(text.size()));
            return bool(file.flush());
        }

        std::string Message(const Task& task, const Failure& f)
        {
            if (!f.message.empty())
                return f.message;
            switch (f.error)
            {
            case Error::Offline:
                return std::format(
                    "Couldn't reach {}. Check the connection and try again.", task.host);
            case Error::NotAvailable: return "The download isn't available anymore.";
            case Error::Refused:
                return std::format("{} refused the download (error {}).", task.host,
                    f.status);
            case Error::Damaged: return "The download was damaged. Please try again.";
            case Error::NoSpace: return "Not enough free space.";
            case Error::CantWrite: return "Can't write the download.";
            case Error::Rejected: return "The download isn't what was expected.";
            case Error::NoCurl:
                return "Downloading needs curl, which this system doesn't have.";
            case Error::Cancelled:
            case Error::None: break;
            }
            return {};
        }

        fs::path PartPath(const JobData& job)
        {
            return job.task->folder / (job.target.name + ".part");
        }

        fs::path SidecarPath(const JobData& job)
        {
            return job.task->folder / (job.target.name + ".sha256");
        }

        void Fail(const JobData& job, Failure f)
        {
            // Stop, dialog closed or the game is exiting: nothing failed.
            if (*job.cancel)
            {
                fprintf(stderr, "[download] %s: stopped (%s)\n", job.task->label.c_str(),
                    f.log.c_str());
                Stopped(job, job.target.name.empty() ? 0 : FileSize(PartPath(job)));
                return;
            }
            std::string message = Message(*job.task, f);
            fprintf(stderr, "[download] %s%s%s: failed: %s (%s)\n",
                job.task->label.c_str(),
                job.target.version.empty() ? "" : " ", job.target.version.c_str(),
                message.c_str(), f.log.c_str());
            Post(job, [&](Status& st) {
                st.phase = Phase::Failed;
                st.stopping = false;
                st.message = std::move(message);
            });
        }

        // Failure of a small request, or none (a
        // 2xx answer). `what` is for the log.
        bool Answered(const Fetched& fetched, const char* what, Failure& f)
        {
            if (fetched.transport == Transport::Unavailable)
                f = { Error::NoCurl, std::format("{}: {}", what, fetched.detail) };
            else if (fetched.transport != Transport::Ok)
                f = { Error::Offline, std::format("{}: transport {}, {}", what,
                    int(fetched.transport), fetched.detail) };
            else if (fetched.response.status == 404)
                f = { Error::NotAvailable, std::format("{}: HTTP 404", what) };
            else if (fetched.response.status < 200 || fetched.response.status > 299)
                f = { Error::Refused,
                    std::format("{}: HTTP {}", what, fetched.response.status),
                    fetched.response.status };
            else
                return true;
            return false;
        }

        void RunPrepare(JobData job)
        {
            Shared& s = S();
            std::lock_guard work(s.work);
            if (*job.cancel)
                return;
            const Task& task = *job.task;
            const Context ctx(job);
            Failure f;
            Target target;
            if (task.resolve && !task.resolve(ctx, target, f))
            {
                if (f.error != Error::Cancelled)
                    Fail(job, std::move(f));
                return;
            }
            if (target.url.empty() || !PlainFileName(target.name))
            {
                Fail(job, { Error::NotAvailable,
                    std::format("a bad target \"{}\" ({})", target.name, target.url) });
                return;
            }
            target.spaceFactor = std::max<uint32_t>(target.spaceFactor, 1);
            job.target = target;

            std::string hash;
            if (!target.sha256.empty())
                hash = ParseSha256Line(target.sha256 + "  " + target.name, target.name);
            else
            {
                std::string body;
                if (!ctx.Get(target.url + ".sha256", kMaxSha256Bytes, body, f, ".sha256"))
                {
                    Fail(job, std::move(f));
                    return;
                }
                hash = ParseSha256Line(body, target.name);
            }
            if (hash.empty())
            {
                Fail(job,
                    { Error::NotAvailable,
                      ".sha256: not a sha256sum line for " + target.name });
                return;
            }
            job.target.sha256 = hash;

            const Fetched head = update::Head({ .url = target.url,
                .userAgent = task.userAgent, .headers = {}, .followRedirects = true,
                .cancel = job.cancel.get() });
            if (!Answered(head, "size", f))
            {
                Fail(job, std::move(f));
                return;
            }
            const std::string length(update::Header(head.response, "content-length"));
            char* end = nullptr;
            job.size = length.empty() ? 0 : std::strtoull(length.c_str(), &end, 10);
            if (!job.size || *end)
            {
                Fail(job,
                    { Error::Offline,
                      std::format("size: Content-Length \"{}\"", length) });
                return;
            }

            // Part of another release, or one longer than this: start over.
            const fs::path part = PartPath(job), sidecar = SidecarPath(job);
            std::error_code ec;
            if (fs::exists(part, ec) || fs::exists(sidecar, ec))
            {
                const std::string expected =
                    ParseSha256Line(ReadSmallFile(sidecar, kMaxSha256Bytes), target.name);
                if (expected != hash || FileSize(part) > job.size)
                {
                    fprintf(stderr, "[download] %s: deleted %s (%s)\n",
                        task.label.c_str(),
                        part.filename().string().c_str(),
                        expected != hash ? "another file's" : "longer than the file");
                    Remove(part);
                    Remove(sidecar);
                }
            }
            RemoveIfEmpty(task.folder);

            const uint64_t have = FileSize(part);
            const std::string downloaded =
                have ? std::format(", {} downloaded", install::FormatSize(have)) : "";
            fprintf(stderr, "[download] %s: %s ready (%s%s)\n", task.label.c_str(),
                target.name.c_str(), install::FormatSize(job.size).c_str(),
                downloaded.c_str());
            std::lock_guard lock(s.mutex);
            if (s.generation != job.generation)
                return;
            s.status.phase = Phase::Ready;
            s.status.version = target.version;
            s.status.size = job.size;
            s.status.have = have;
            s.ready = job;
        }

        void RunDownload(JobData job)
        {
            Shared& s = S();
            std::lock_guard work(s.work);
            const Task& task = *job.task;
            const fs::path part = PartPath(job), sidecar = SidecarPath(job);
            if (*job.cancel)
            {
                Stopped(job, FileSize(part));
                return;
            }
            const char* label = task.label.c_str();
            const auto started = Clock::now();
            const uint64_t size = job.size;
            auto dropPart = [&] {
                Remove(part);
                Remove(sidecar);
                RemoveIfEmpty(task.folder);
            };

            std::error_code ec;
            fs::create_directories(task.folder, ec);
            if (ec)
            {
                Fail(job, CantWrite(task.folder, ec.message()));
                return;
            }
            uint64_t have = FileSize(part);
            const uint64_t startedAt = have;
            const uint64_t need = size - have + (job.target.spaceFactor - 1) * size;
            const std::optional<uint64_t> free = FreeSpace(task.folder);
            if (free && *free < need)
            {
                Fail(job, NoSpace(need, *free, "before downloading"));
                RemoveIfEmpty(task.folder);
                return;
            }
            const std::string sidecarText =
                job.target.sha256 + "  " + job.target.name + "\n";
            if (ReadSmallFile(sidecar, kMaxSha256Bytes) != sidecarText
                && !WriteSmallFile(sidecar, sidecarText))
            {
                Fail(job, CantWrite(sidecar, "the hash sidecar"));
                dropPart();
                return;
            }

            // A server that ignores Range sends the whole
            // file, so go once more from the start.
            const update::Request request = { .url = job.target.url,
                .userAgent = task.userAgent, .headers = {},
                .timeoutSeconds = update::kStallSeconds, .followRedirects = true };
            auto progress = [&](uint64_t bytes) {
                Post(job, [&](Status& st) { st.have = bytes; });
            };
            for (int attempt = 0; have < size && attempt < 2; attempt++)
            {
                const update::Downloaded got =
                    update::Download(request, part, progress, *job.cancel);
                have = FileSize(part);
                if (got.cancelled)
                {
                    fprintf(stderr, "[download] %s: stopped at %s of %s\n", label,
                        install::FormatSize(have).c_str(),
                        install::FormatSize(size).c_str());
                    Stopped(job, have);
                    return;
                }
                if (got.writeFailed)
                {
                    dropPart();
                    const std::optional<uint64_t> left = FreeSpace(task.folder);
                    const uint64_t needed = job.target.spaceFactor * size;
                    if (left && *left < needed)
                        Fail(job, NoSpace(needed, *left, got.detail));
                    else
                        Fail(job, CantWrite(part, got.detail));
                    return;
                }
                if (got.rangeRefused)
                {
                    fprintf(stderr,
                        "[download] %s: the server ignored the resume: "
                        "starting over (%s)\n",
                        label, got.detail.c_str());
                    Remove(part);
                    have = 0;
                    continue;
                }
                if (got.transport == Transport::Unavailable)
                {
                    Fail(job, { Error::NoCurl, got.detail });
                    return;
                }
                if (got.transport != Transport::Ok)
                {
                    Fail(job, { Error::Offline, std::format("transport {}, {}, {} of {}",
                        int(got.transport), got.detail, have, size) });
                    return;
                }
                if (got.status == 404)
                {
                    dropPart();
                    Fail(job, { Error::NotAvailable, got.detail });
                    return;
                }
                if (got.status != 200 && got.status != 206 && got.status != 416)
                {
                    Fail(job, { Error::Refused, got.detail, got.status });
                    return;
                }
                break;
            }
            if (have != size)
            {
                if (have > size || have == startedAt)
                {
                    // Longer than the file, or the server wont continue.
                    dropPart();
                    Fail(job,
                        { Error::Damaged, std::format("{} of {} bytes", have, size) });
                }
                else
                    Fail(job,
                        { Error::Offline,
                          std::format("cut off at {} of {} bytes", have, size) });
                return;
            }

            // SHA-256 of the whole file.
            Post(job, [&](Status& st) {
                st.phase = Phase::Checking;
                st.have = 0;
            });
            {
                std::ifstream in(part, std::ios::binary);
                install::Sha256 sha;
                std::vector<char> buffer(kChunk);
                uint64_t hashed = 0;
                while (in && hashed < size)
                {
                    if (*job.cancel)
                    {
                        // The part stays and gets checked again next time.
                        Stopped(job, size);
                        return;
                    }
                    in.read(buffer.data(), std::streamsize(buffer.size()));
                    const size_t n = size_t(in.gcount());
                    sha.Update(buffer.data(), n);
                    hashed += n;
                    Post(job, [&](Status& st) { st.have = hashed; });
                }
                const std::string got = install::ToHex(sha.Finish());
                if (hashed != size || got != job.target.sha256)
                {
                    dropPart();
                    Fail(job, { Error::Damaged,
                        hashed != size
                            ? std::format("read {} of {} bytes", hashed, size)
                            : std::format("SHA-256 {}, expected {}", got,
                                  job.target.sha256) });
                    return;
                }
            }

            Post(job, [&](Status& st) {
                st.phase = Phase::Installing;
                st.files = st.filesDone = 0;
            });
            std::string log = "kept";
            if (task.finish)
            {
                Failure f;
                if (!task.finish(Context(job), part, log, f))
                {
                    if (f.error == Error::Cancelled)
                    {
                        Stopped(job, size);  // the verified part stays
                        return;
                    }
                    dropPart();
                    Fail(job, std::move(f));
                    return;
                }
                dropPart();  // used
            }
            const double seconds =
                std::chrono::duration<double>(Clock::now() - started).count();
            fprintf(stderr, "[download] %s: %s %s (%s downloaded in %.1f s)\n", label,
                job.target.name.c_str(), log.c_str(),
                install::FormatSize(size - startedAt).c_str(), seconds);
            Post(job, [&](Status& st) { st.phase = Phase::Done; });
        }

        // Main thread, under s.mutex.
        void Launch(void (*run)(JobData), JobData job)
        {
            const std::string label = job.task->label;
            try
            {
                std::thread(run, std::move(job)).detach();
            }
            catch (const std::system_error& e)
            {
                fprintf(stderr, "[download] %s: no thread for the download (%s)\n",
                    label.c_str(), e.what());
                Status& st = S().status;
                st.phase = Phase::Failed;
                st.message = "The game couldn't start the download. Try again later.";
            }
        }
    }

    Failure NoSpace(uint64_t need, uint64_t free, std::string log)
    {
        return { Error::NoSpace, std::move(log), 0,
            std::format("Not enough free space: needs {}, {} free.",
                install::FormatSize(need), install::FormatSize(free)) };
    }

    Failure CantWrite(const fs::path& path, std::string log)
    {
        return { Error::CantWrite, std::move(log), 0,
            std::format("Can't write to {}.", path.string()) };
    }

    std::optional<uint64_t> FreeSpace(const fs::path& folder)
    {
        return platform::FreeSpace(folder);
    }

    bool Context::Cancelled() const
    {
        return *m_job.cancel;
    }

    bool Context::Exiting() const
    {
        return s_exiting;
    }

    void Context::Progress(uint64_t done, uint64_t total) const
    {
        Post(m_job, [&](Status& st) {
            st.files = total;
            st.filesDone = done;
        });
    }

    bool Context::Get(const std::string& url, size_t maxBytes, std::string& body,
        Failure& failure, const char* what, bool followRedirects) const
    {
        const Fetched got = update::Fetch({ .url = url,
            .userAgent = m_job.task->userAgent, .headers = {}, .maxBytes = maxBytes,
            .followRedirects = followRedirects,
            .cancel = m_job.cancel.get() });
        if (!Answered(got, what, failure))
            return false;
        body = got.response.body;
        return true;
    }

    void Start(Task task)
    {
        static const bool exitHandler = (std::atexit(OnExit), true);
        (void)exitHandler;
        Shared& s = S();
        std::lock_guard lock(s.mutex);
        if (s.cancel)
            *s.cancel = true;
        JobData job;
        job.generation = ++s.generation;
        job.cancel = s.cancel = std::make_shared<std::atomic<bool>>(false);
        job.task = std::make_shared<const Task>(std::move(task));
        s.status = {};
        s.status.phase = Phase::Contacting;
        s.status.label = job.task->label;
        s.status.host = job.task->host;
        Launch(RunPrepare, std::move(job));
    }

    void Download()
    {
        Shared& s = S();
        std::lock_guard lock(s.mutex);
        if (s.status.phase != Phase::Ready || s.ready.generation != s.generation)
            return;
        JobData job = s.ready;
        job.cancel = s.cancel = std::make_shared<std::atomic<bool>>(false);
        s.status.phase = Phase::Downloading;
        s.status.stopping = false;
        Launch(RunDownload, std::move(job));
    }

    void Stop()
    {
        Shared& s = S();
        std::lock_guard lock(s.mutex);
        if (s.status.phase != Phase::Downloading || !s.cancel)
            return;
        s.status.stopping = true;
        *s.cancel = true;
    }

    void Close()
    {
        Shared& s = S();
        std::lock_guard lock(s.mutex);
        s.generation++;
        if (s.cancel)
            *s.cancel = true;
        s.status = {};
    }

    Status GetStatus()
    {
        Shared& s = S();
        std::lock_guard lock(s.mutex);
        return s.status;
    }
}
