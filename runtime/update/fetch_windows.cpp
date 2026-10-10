// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See fetch.h.
//
// Windows: no transport yet, so Check for Updates says it isn't available
// (the rest of the feature, the dialog, the QR code and the log line, is
// shared and works as it is).
//
// The port: WinHTTP (winhttp.dll is part of every Windows, so nothing to
// bundle; link winhttp under WIN32 in runtime/CMakeLists.txt):
//   1. WinHttpOpen(userAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, ...) so
//      the system's proxy settings apply, as they do on macOS.
//   2. WinHttpCrackUrl; WinHttpConnect(host, port); WinHttpOpenRequest(L"GET",
//      path, ..., WINHTTP_FLAG_SECURE) (plain http only for request.allowHttp);
//      WinHttpSetOption: WINHTTP_OPTION_DISABLE_FEATURE with
//      WINHTTP_DISABLE_COOKIES, WINHTTP_OPTION_REDIRECT_POLICY never, and
//      WINHTTP_OPTION_AUTOLOGON_POLICY high (no Windows credentials sent).
//   3. WinHttpSetTimeouts for resolve, connect, send and receive. Those are
//      per phase, so also keep the whole exchange's deadline: an async
//      session (WINHTTP_FLAG_ASYNC with a callback and an event), waited on
//      for timeoutSeconds, then WinHttpCloseHandle to cancel it.
//   4. WinHttpAddRequestHeaders (request.headers), WinHttpSendRequest,
//      WinHttpReceiveResponse; the status from WinHttpQueryHeaders
//      (WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER) and the
//      headers from WINHTTP_QUERY_RAW_HEADERS_CRLF (ParseHttpResponse reads
//      them, for x-ratelimit-*); the body with WinHttpReadData, stopping past
//      request.maxBytes (Transport::TooLarge).
//   5. Errors: ERROR_WINHTTP_NAME_NOT_RESOLVED, _CANNOT_CONNECT and
//      _CONNECTION_ERROR are Transport::Offline, ERROR_WINHTTP_TIMEOUT
//      Timeout, ERROR_WINHTTP_SECURE_FAILURE Secure.
// Test it as the other platforms are (docs: update/updater.h's hooks).
#ifdef _WIN32
#include "fetch.h"

namespace update
{
    Fetched Fetch(const Request&)
    {
        Fetched result;
        result.transport = Transport::Unavailable;
        result.detail = "Checking for updates isn't available on Windows yet. The releases page has every version.";
        return result;
    }
}
#endif
