// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See fetch.h.
//
// Windows: WinHTTP (winhttp.dll is part of every Windows: nothing to bundle;
// linked in runtime/CMakeLists.txt), in a session of its own:
//   - the system's proxy settings (WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, the
//     default proxy before Windows 8.1) and certificates, as on macOS;
//   - no cookies, no Windows credentials sent (the autologon policy high),
//     no cache (WinHTTP keeps none); redirects only if asked for, never from
//     https to http, kMaxRedirects at most;
//   - https only, but for request.allowHttp (the test hooks' local server).
// WinHTTP's timeouts are per phase (resolve, connect, send, receive), so the
// whole exchange's deadline is kept here too: the request runs on a thread
// of its own and Fetch waits for it for timeoutSeconds plus two seconds;
// then it closes the request handle, which cancels whatever WinHTTP was
// waiting on (WinHttpCloseHandle's documented way to end a request in
// progress), and answers Timeout. What the thread writes lives in a
// shared_ptr it holds, so it never touches a returned frame.
// The answer: the status, the headers (WINHTTP_QUERY_RAW_HEADERS_CRLF, read
// by ParseHttpResponse, for x-ratelimit-*) and the body, cut off past
// request.maxBytes (Transport::TooLarge).
// Download works the same way but without a deadline:
// the per-phase timeouts act as the stall limit, and
// Download closes the request handle on cancel.
#ifdef _WIN32
#include "fetch.h"

#include <condition_variable>
#include <cstdio>
#include <format>
#include <memory>
#include <mutex>
#include <thread>

#include <windows.h>
#include <winhttp.h>

namespace update
{
    namespace
    {
        std::wstring Wide(const std::string& s)
        {
            if (s.empty())
                return {};
            int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
            std::wstring w(size_t(n > 0 ? n : 0), L'\0');
            if (n > 0)
                MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
            return w;
        }

        std::string Narrow(const wchar_t* w, size_t length)
        {
            if (!length)
                return {};
            int n = WideCharToMultiByte(CP_UTF8, 0, w, int(length), nullptr, 0, nullptr, nullptr);
            std::string s(size_t(n > 0 ? n : 0), '\0');
            if (n > 0)
                WideCharToMultiByte(CP_UTF8, 0, w, int(length), s.data(), n, nullptr, nullptr);
            return s;
        }

        // WinHTTP's errors as the transport's outcomes.
        Transport FromError(DWORD error)
        {
            switch (error)
            {
            case ERROR_WINHTTP_NAME_NOT_RESOLVED:
            case ERROR_WINHTTP_CANNOT_CONNECT:
            case ERROR_WINHTTP_CONNECTION_ERROR:
            case ERROR_WINHTTP_AUTO_PROXY_SERVICE_ERROR:
            case ERROR_WINHTTP_BAD_AUTO_PROXY_SCRIPT:
            case ERROR_WINHTTP_UNABLE_TO_DOWNLOAD_SCRIPT:
                return Transport::Offline;
            case ERROR_WINHTTP_TIMEOUT:
            case ERROR_WINHTTP_OPERATION_CANCELLED:  // the deadline closed it
                return Transport::Timeout;
            case ERROR_WINHTTP_SECURE_FAILURE:
            case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
            case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
            case ERROR_WINHTTP_SECURE_INVALID_CA:
            case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:
            case ERROR_WINHTTP_SECURE_CHANNEL_ERROR:
            case ERROR_WINHTTP_SECURE_INVALID_CERT:
            case ERROR_WINHTTP_SECURE_CERT_REVOKED:
            case ERROR_WINHTTP_SECURE_CERT_WRONG_USAGE:
                return Transport::Secure;
            case ERROR_WINHTTP_INVALID_SERVER_RESPONSE:
            case ERROR_WINHTTP_HEADER_SIZE_OVERFLOW:
                return Transport::Garbled;
            default:
                return Transport::Failed;
            }
        }

        struct Pending
        {
            std::mutex mutex;
            std::condition_variable done;
            bool finished = false;
            bool cancelled = false;
            HINTERNET request = nullptr;  // under mutex: the one the deadline closes
            Fetched result;
            Downloaded downloaded;
            std::atomic<uint64_t> bytes = 0;  // Download: in `part` so far
        };

        // Closes what the deadline hasn't (the request handle, under the lock).
        struct Handles
        {
            std::shared_ptr<Pending> pending;
            HINTERNET session = nullptr, connect = nullptr;
            ~Handles()
            {
                {
                    std::lock_guard lock(pending->mutex);
                    if (pending->request)
                        WinHttpCloseHandle(pending->request);
                    pending->request = nullptr;
                }
                if (connect)
                    WinHttpCloseHandle(connect);
                if (session)
                    WinHttpCloseHandle(session);
            }
        };

        // Sends a request and reads the answer headers: false with f set.
        // `extra`: more header lines ("Name: value\r\n").
        bool Open(const Request& request, const std::shared_ptr<Pending>& pending,
            Handles& h, const wchar_t* method, const std::string& extra, Fetched& f,
            DWORD& status, HttpResponse& response)
        {
            auto fail = [&f](const char* step) {
                const DWORD error = GetLastError();
                f.transport = FromError(error);
                f.detail = std::format("WinHTTP {}: error {}", step, error);
                return false;
            };
            const std::wstring agent = Wide(request.userAgent);
            h.session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            if (!h.session && GetLastError() == ERROR_INVALID_PARAMETER)  // before Windows 8.1
                h.session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            if (!h.session)
                return fail("open");
            const int ms = int(request.timeoutSeconds * 1000.0);
            WinHttpSetTimeouts(h.session, ms, ms, ms, ms);

            const std::wstring url = Wide(request.url);
            URL_COMPONENTS parts{};
            parts.dwStructSize = sizeof(parts);
            parts.dwSchemeLength = parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = DWORD(-1);
            if (!WinHttpCrackUrl(url.c_str(), DWORD(url.size()), 0, &parts))
                return fail("address");
            const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
            if (!secure && !(request.allowHttp && parts.nScheme == INTERNET_SCHEME_HTTP))
            {
                f.transport = Transport::Failed;
                f.detail = "not an https address";
                return false;
            }
            const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
            std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
            path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);  // ?per_page=10
            if (path.empty())
                path = L"/";
            h.connect = WinHttpConnect(h.session, host.c_str(), parts.nPort, 0);
            if (!h.connect)
                return fail("connect");
            HINTERNET req = WinHttpOpenRequest(h.connect, method, path.c_str(), nullptr,
                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                secure ? WINHTTP_FLAG_SECURE : 0);
            if (!req)
                return fail("request");
            {
                std::lock_guard lock(pending->mutex);
                if (pending->cancelled)
                {
                    WinHttpCloseHandle(req);
                    f.transport = Transport::Timeout;
                    f.detail = "cancelled";
                    return false;
                }
                pending->request = req;
            }
            DWORD option = WINHTTP_DISABLE_COOKIES;
            WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &option, sizeof(option));
            option = request.followRedirects
                ? WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP
                : WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
            WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &option, sizeof(option));
            option = kMaxRedirects;
            WinHttpSetOption(req, WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS, &option,
                sizeof(option));
            option = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
            WinHttpSetOption(req, WINHTTP_OPTION_AUTOLOGON_POLICY, &option, sizeof(option));
            std::string lines = extra;
            for (const auto& [name, value] : request.headers)
                lines += name + ": " + value + "\r\n";
            const std::wstring headers = Wide(lines);
            if (!headers.empty() && !WinHttpAddRequestHeaders(req, headers.c_str(), DWORD(headers.size()), WINHTTP_ADDREQ_FLAG_ADD))
                return fail("headers");
            if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
                return fail("send");
            if (!WinHttpReceiveResponse(req, nullptr))
                return fail("receive");

            status = 0;
            DWORD size = sizeof(status);
            if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                    WINHTTP_NO_HEADER_INDEX))
                return fail("status");
            // The headers as a raw block, for ParseHttpResponse.
            size = 0;
            WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &size,
                WINHTTP_NO_HEADER_INDEX);
            if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && size > 0 && size < 256 * 1024)
            {
                std::wstring raw(size / sizeof(wchar_t) + 1, L'\0');
                if (WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw.data(), &size,
                        WINHTTP_NO_HEADER_INDEX))
                {
                    std::string block = Narrow(raw.data(), size / sizeof(wchar_t));
                    while (!block.empty() && (block.back() == '\r' || block.back() == '\n' || block.back() == '\0'))
                        block.pop_back();
                    ParseHttpResponse(block + "\r\n\r\n", response);
                }
            }
            response.status = int(status);
            return true;
        }

        HINTERNET RequestHandle(const std::shared_ptr<Pending>& pending)
        {
            std::lock_guard lock(pending->mutex);
            return pending->request;
        }

        Fetched Run(const Request& request, const std::shared_ptr<Pending>& pending,
            const wchar_t* method)
        {
            Fetched f;
            auto fail = [&f](const char* step) {
                const DWORD error = GetLastError();
                f.transport = FromError(error);
                f.detail = std::format("WinHTTP {}: error {}", step, error);
                return f;
            };
            Handles h{ pending };
            DWORD status = 0;
            HttpResponse response;
            if (!Open(request, pending, h, method, {}, f, status, response))
                return f;
            HINTERNET req = RequestHandle(pending);
            if (!req)
            {
                f.transport = Transport::Timeout;
                f.detail = "cancelled";
                return f;
            }

            // The body, at most maxBytes.
            std::string body;
            while (true)
            {
                DWORD available = 0;
                if (!WinHttpQueryDataAvailable(req, &available))
                    return fail("read");
                if (available == 0)
                    break;
                if (body.size() + available > request.maxBytes)
                {
                    f.transport = Transport::TooLarge;
                    f.detail = std::format("more than {} bytes", request.maxBytes);
                    return f;
                }
                const size_t at = body.size();
                body.resize(at + available);
                DWORD read = 0;
                if (!WinHttpReadData(req, body.data() + at, available, &read))
                    return fail("read");
                body.resize(at + read);
                if (read == 0)
                    break;
            }
            response.body = std::move(body);
            f.transport = Transport::Ok;
            f.response = std::move(response);
            return f;
        }
    }

    namespace
    {
        Fetched Exchange(const Request& request, const wchar_t* method)
        {
            auto pending = std::make_shared<Pending>();
            try
            {
                std::thread([request, pending, method] {
                    Fetched f = Run(request, pending, method);
                    std::lock_guard lock(pending->mutex);
                    pending->result = std::move(f);
                    pending->finished = true;
                    pending->done.notify_all();
                }).detach();
            }
            catch (const std::system_error& e)
            {
                Fetched f;
                f.transport = Transport::Failed;
                f.detail = std::string("no thread for the request: ") + e.what();
                return f;
            }
            std::unique_lock lock(pending->mutex);
            const auto deadline = std::chrono::steady_clock::now()
                + std::chrono::duration<double>(request.timeoutSeconds + 2.0);
            bool cancelled = false;
            auto finished = [&] { return pending->finished; };
            const auto tick = std::chrono::milliseconds(250);
            while (!pending->done.wait_for(lock, tick, finished))
            {
                cancelled = request.cancel && request.cancel->load();
                if (cancelled || std::chrono::steady_clock::now() >= deadline)
                    break;
            }
            if (!pending->finished)
            {
                // Cut it off: closing the request handle ends the WinHTTP wait.
                pending->cancelled = true;
                if (pending->request)
                    WinHttpCloseHandle(pending->request);
                pending->request = nullptr;
                Fetched f;
                f.transport = cancelled ? Transport::Failed : Transport::Timeout;
                f.detail = cancelled
                    ? "cancelled"
                    : std::format("no answer within {} seconds",
                          int(request.timeoutSeconds));
                return f;
            }
            return std::move(pending->result);
        }
    }

    Fetched Fetch(const Request& request)
    {
        return Exchange(request, L"GET");
    }

    Fetched Head(const Request& request)
    {
        return Exchange(request, L"HEAD");
    }

    namespace
    {
        // The download itself, on its own thread;
        // updates pending->bytes as it goes.
        Downloaded RunDownload(const Request& request, const std::filesystem::path& part,
            const std::shared_ptr<Pending>& pending)
        {
            Downloaded d;
            std::error_code ec;
            const auto existing = std::filesystem::file_size(part, ec);
            const uint64_t have = ec ? 0 : uint64_t(existing);
            pending->bytes = have;

            Handles h{ pending };
            Fetched f;
            DWORD status = 0;
            HttpResponse response;
            const std::string range =
                have ? std::format("Range: bytes={}-\r\n", have) : std::string();
            if (!Open(request, pending, h, L"GET", range, f, status, response))
            {
                d.transport = f.transport;
                d.detail = f.detail;
                return d;
            }
            d.transport = Transport::Ok;
            d.status = int(status);
            d.detail = std::format("HTTP {}", status);
            if (status == 200 && have)
            {
                d.rangeRefused = true;
                return d;
            }
            if (status != 200 && status != 206)
                return d;  // `part` untouched

            HINTERNET req = RequestHandle(pending);
            const wchar_t* mode = status == 206 ? L"ab" : L"wb";
            FILE* file = req ? _wfopen(part.c_str(), mode) : nullptr;
            if (!file)
            {
                d.transport = Transport::Failed;
                d.writeFailed = req != nullptr;
                d.detail = req
                    ? std::format("can't open {}: errno {}", part.string(), errno)
                    : "cancelled";
                return d;
            }
            uint64_t bytes = status == 206 ? have : 0;
            pending->bytes = bytes;
            std::vector<char> buffer(256 * 1024);
            while (true)
            {
                DWORD read = 0;
                if (!WinHttpReadData(req, buffer.data(), DWORD(buffer.size()), &read))
                {
                    const DWORD error = GetLastError();
                    d.transport = FromError(error);
                    d.detail = std::format("WinHTTP read: error {}", error);
                    break;
                }
                if (read == 0)
                    break;  // the end of the body
                if (fwrite(buffer.data(), 1, read, file) != read || fflush(file) != 0)
                {
                    d.transport = Transport::Failed;
                    d.writeFailed = true;
                    d.detail =
                        std::format("can't write {}: errno {}", part.string(), errno);
                    break;
                }
                bytes += read;
                pending->bytes = bytes;
            }
            fclose(file);
            return d;
        }
    }

    Downloaded Download(const Request& request, const std::filesystem::path& part,
        const std::function<void(uint64_t)>& progress, const std::atomic<bool>& cancel)
    {
        auto pending = std::make_shared<Pending>();
        try
        {
            std::thread([request, part, pending] {
                Downloaded d = RunDownload(request, part, pending);
                std::lock_guard lock(pending->mutex);
                pending->downloaded = std::move(d);
                pending->finished = true;
                pending->done.notify_all();
            }).detach();
        }
        catch (const std::system_error& e)
        {
            Downloaded d;
            d.detail = std::string("no thread for the download: ") + e.what();
            return d;
        }
        std::unique_lock lock(pending->mutex);
        auto finished = [&] { return pending->finished; };
        while (!pending->done.wait_for(lock, std::chrono::milliseconds(250), finished))
        {
            if (cancel.load())
            {
                // Closing the request handle ends the WinHTTP wait, then the
                // thread closes `part` (we give it two seconds).
                pending->cancelled = true;
                if (pending->request)
                    WinHttpCloseHandle(pending->request);
                pending->request = nullptr;
                pending->done.wait_for(lock, std::chrono::seconds(2), finished);
                Downloaded d;
                d.cancelled = true;
                d.detail = "cancelled";
                return d;
            }
            if (progress)
            {
                const uint64_t bytes = pending->bytes;
                lock.unlock();
                progress(bytes);
                lock.lock();
            }
        }
        if (progress)
            progress(pending->bytes);
        return std::move(pending->downloaded);
    }
}
#endif
