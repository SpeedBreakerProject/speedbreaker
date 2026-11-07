// A manual probe of the update check's transport (update/fetch.h) on Windows
// (WinHTTP) and Linux (the system's curl): one GET, as the game makes it, and
// what Evaluate makes of the answer. Not a ctest test: it goes online (point
// it at a local stand-in, never GitHub in an automated run:
// update-check/tools/serve.py).
//   update_fetch_probe <url> [<running version> [<running tag>]]
// prints the transport's outcome, the status, the headers it read, the body's
// size and the result's log line; exit 0 when there was an HTTP answer.
#include <update/fetch.h>

#include <cstdio>
#include <ctime>
#include <string>

using namespace update;

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s <url> [<running version> [<running tag>]]\n", argv[0]);
        return 2;
    }
    Request request;
    request.url = argv[1];
    request.userAgent = "SpeedBreaker/0.0.0-probe";
    request.headers = { { "Accept", "application/vnd.github+json" }, { "X-GitHub-Api-Version", "2022-11-28" } };
    request.allowHttp = true;
    const std::time_t started = std::time(nullptr);
    Fetched f = Fetch(request);
    const char* names[] = { "Ok", "Offline", "Timeout", "Secure", "TooLarge", "Garbled", "Unavailable", "Failed" };
    printf("transport: %s%s%s (%lld s)\n", names[int(f.transport)], f.detail.empty() ? "" : ": ", f.detail.c_str(),
        (long long)(std::time(nullptr) - started));
    if (f.transport == Transport::Ok)
    {
        printf("status: %d, %zu headers, body %zu bytes\n", f.response.status, f.response.headers.size(), f.response.body.size());
        for (const char* h : { "content-type", "x-ratelimit-remaining", "x-ratelimit-reset" })
            if (!Header(f.response, h).empty())
                printf("  %s: %s\n", h, std::string(Header(f.response, h)).c_str());
    }
    Running running;
    running.version = argc > 2 ? argv[2] : "0.2.3";
    running.tag = argc > 3 ? argv[3] : "";
    running.devBuild = running.tag.empty();
    running.device = ThisDevice();
    Result r = Evaluate(f, running, int64_t(std::time(nullptr)));
    printf("result: %s | %s\n", r.headline.c_str(), r.message.c_str());
    if (!r.whyLine.empty())
        printf("why: %s\n", r.whyLine.c_str());
    printf("log: %s\n", r.log.c_str());
    return f.transport == Transport::Ok ? 0 : 1;
}
