// AudioPacingTest: does this PC wake a 5.33 ms audio-style tick on time?
// Runs three ways for 8 s each and writes the result next to the exe.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
#include "audio_wait.h"
using namespace std::chrono;

static std::string g_out;
static void Log(const char* fmt, double a = 0, double b = 0, double c = 0, double d = 0)
{
    char buf[512]; snprintf(buf, sizeof buf, fmt, a, b, c, d);
    fputs(buf, stdout); g_out += buf;
}

template <class WaitFn>
static void Run(const char* name, WaitFn wait)
{
    constexpr auto INTERVAL = nanoseconds(1000000000ull * 256 / 48000);  // 5.333 ms
    const int N = 1500;                                                  // ~8 s
    std::vector<double> late; late.reserve(N);
    auto next = steady_clock::now();
    int bursts = 0;
    for (int i = 0; i < N; i++)
    {
        wait(next);
        double l = duration<double, std::milli>(steady_clock::now() - next).count();
        late.push_back(l);
        if (l >= 5.333) bursts++;
        next += INTERVAL;
    }
    std::sort(late.begin(), late.end());
    char head[160]; snprintf(head, sizeof head, "%-52s", name); fputs(head, stdout); g_out += head;
    Log("median %5.2f ms   99%% %5.2f ms   worst %6.2f ms   late>=1 tick: %.0f of 1500\n",
        late[N / 2], late[N * 99 / 100], late.back(), bursts);
}

int main()
{
    Log("AudioPacingTest - wake lateness of a 5.33 ms tick (lower is better;\n");
    Log("'late >= 1 tick' means a catch-up burst, i.e. silent frames in the game)\n\n");

    Run("1. sleep_until, no timer request", [](steady_clock::time_point t) { std::this_thread::sleep_until(t); });

    timeBeginPeriod(1);
    Run("2. sleep_until + timeBeginPeriod(1) (like SDL)", [](steady_clock::time_point t) { std::this_thread::sleep_until(t); });
    timeEndPeriod(1);

    {
        apu::AudioWait w;
        char b[160]; snprintf(b, sizeof b, "3. patch: %s", w.Describe());
        Run(b, [&](steady_clock::time_point t) { w.Until(t); });
    }

    Log("\nSend this text back. Press Enter to close.\n");
    if (FILE* f = fopen("AudioPacingTest-result.txt", "w")) { fputs(g_out.c_str(), f); fclose(f); }
    getchar();
}
