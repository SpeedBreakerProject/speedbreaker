// SpeedBreaker. GPL-3.0-or-later (see COPYING).
//
// game/hfr_late_hold.h: how long Smooth Motion stays at an even 60 after a
// picture shown on another refresh than the others (off Apple's systems).
#include <game/hfr_late_hold.h>

#include <cstdio>
#include <vector>

namespace
{
    int s_failures = 0;
    void Check(bool ok, const char* what)
    {
        if (!ok)
        {
            fprintf(stderr, "FAIL: %s\n", what);
            s_failures++;
        }
    }
    constexpr int64_t S = 1'000'000'000;

    // Ghosts at `times` (s); the pictures come back 2 s after each hold ends
    // (120 frames' costs), hitches at `hitches`. The holds given, in s.
    std::vector<int64_t> Run(const std::vector<double>& times, const std::vector<double>& hitches = {})
    {
        hfr::late::Hold h;
        int64_t resumedAt = 0;
        std::vector<int64_t> holds;
        for (double t : times)
        {
            const int64_t now = int64_t(t * S);
            int64_t hitch = 0;
            for (double x : hitches)
                if (int64_t(x * S) <= now)
                    hitch = int64_t(x * S);
            if (resumedAt > now)
                resumedAt = 0;  // (a test's ghost inside a hold: not the case here)
            hfr::late::Next(h, now, resumedAt, hitch);
            holds.push_back(h.hold / S);
            resumedAt = now + h.hold + 2 * S;
        }
        return holds;
    }
}

int main()
{
    using hfr::late::Kind;
    // The v0.2.0 SteamOS stage's sm120b at 120 Hz: lone ghosts in the menus,
    // the race's movie and load, and the race, each a clean 10 s or more after
    // coming back. v0.2.0's first rule held 8, 16, 32, 64, 120 s; now each is
    // a one-off.
    for (int64_t s : Run({ 27, 50, 73, 119, 183 }))
        Check(s == 8, "sm120b's lone ghosts: 8 s each");
    // sm120-on: 27, 50, 75, 110 s (held to 174 s before): 8 s each.
    for (int64_t s : Run({ 27, 50, 75, 110 }))
        Check(s == 8, "sm120-on's lone ghosts: 8 s each");
    // A machine that can't show them on time ghosts again a second or two
    // after each return: 8, 16, 32, then at most 32.
    {
        hfr::late::Hold h;
        int64_t now = 10 * S, resumed = 0;
        const int64_t want[] = { 8, 16, 32, 32, 32 };
        for (int64_t w : want)
        {
            hfr::late::Next(h, now, resumed, 0);
            Check(h.hold == w * S, "repeats double up to 32 s");
            resumed = now + h.hold + 2 * S;
            now = resumed + 1 * S;
        }
        // Then a clean stretch: the next one starts over.
        now = resumed + 15 * S;
        Check(hfr::late::Next(h, now, resumed, 0) == Kind::kOneOff && h.hold == 8 * S, "a clean 15 s: 8 s again");
    }
    // A repeat within 5 s of a load or a long hitch is a one-off.
    {
        hfr::late::Hold h;
        Check(hfr::late::Next(h, 20 * S, 0, 0) == Kind::kFirst && h.hold == 8 * S, "the first: 8 s");
        const int64_t resumed = 30 * S;
        Check(hfr::late::Next(h, 33 * S, resumed, 31 * S) == Kind::kHitch && h.hold == 8 * S, "near a hitch: 8 s");
        Check(hfr::late::Next(h, 45 * S, 44 * S, 31 * S) == Kind::kRepeat && h.hold == 16 * S, "1 s after coming back, the hitch long past: 16 s");
    }
    // Repeat only once it came back since the last ghost (a resume time from
    // before it isn't one).
    {
        hfr::late::Hold h;
        hfr::late::Next(h, 20 * S, 15 * S, 0);
        Check(hfr::late::Next(h, 22 * S, 15 * S, 0) == Kind::kOneOff, "no return since the last ghost: not a repeat");
    }
    // Exactly 10 s after coming back is no longer a repeat; just under is.
    {
        hfr::late::Hold h;
        hfr::late::Next(h, 1 * S, 0, 0);
        hfr::late::Hold h2 = h;
        Check(hfr::late::Next(h, 11 * S + hfr::late::kRepeatNs, 11 * S, 0) == Kind::kOneOff, "10 s: a one-off");
        Check(hfr::late::Next(h2, 11 * S + hfr::late::kRepeatNs - 1, 11 * S, 0) == Kind::kRepeat, "just under 10 s: a repeat");
    }
    if (s_failures)
        return 1;
    printf("hfr_late_hold_test: all passed\n");
    return 0;
}
