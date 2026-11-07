// Tests for update/startup_toast.h: the startup check's toast, frame by frame.
// From the repo root:
//   clang++ -std=c++20 -O2 -Iruntime tests/startup_toast_test.cpp -o build/startup_toast_test
#include <update/startup_toast.h>

#include <cstdio>

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

using namespace update;

// A run of frames: each frame's inputs, the toast's state carried over.
struct Sim
{
    bool shown = false, done = false, announced = false;
    int shows = 0, withdrawals = 0, skips = 0;
    void Frame(bool ready, bool available, bool modal, bool settings)
    {
        if (done)
            return;
        switch (NextToastStep({ ready, available, announced, modal, settings, shown }))
        {
        case ToastStep::Wait: break;
        case ToastStep::Show: shown = announced = true; shows++; break;
        case ToastStep::Skip: announced = done = true; skips++; break;
        case ToastStep::Withdraw: withdrawals++; done = true; break;
        case ToastStep::Done: done = true; break;
        }
    }
};

int main()
{
    // The coordinator's case: the toast up, then Settings opens: taken down at once, for good.
    Sim s;
    s.Frame(false, false, false, false);
    CHECK(s.shows == 0 && !s.done, "waits for the check");
    s.Frame(true, true, false, false);
    CHECK(s.shows == 1, "shown when the check finds a newer version");
    s.Frame(true, true, false, false);
    CHECK(s.shows == 1 && s.withdrawals == 0, "shown once, left up while Settings is closed");
    s.Frame(true, true, false, true);
    CHECK(s.withdrawals == 1 && s.done, "Settings opened: taken down");
    s.Frame(true, true, false, false);
    s.Frame(true, true, false, true);
    CHECK(s.shows == 1 && s.withdrawals == 1, "never again after closing and reopening Settings");

    // Settings already open when the check finishes: no toast, but announced (the footer offers it).
    Sim open;
    open.Frame(true, true, false, true);
    CHECK(open.shows == 0 && open.skips == 1 && open.announced && open.done, "no toast under an open Settings");
    open.Frame(true, true, false, false);
    CHECK(open.shows == 0, "nor after it closes");

    // A panel (installer, What's new) holds it back; then it shows.
    Sim modal;
    modal.Frame(true, true, true, false);
    CHECK(modal.shows == 0 && !modal.done, "waits under a panel");
    modal.Frame(true, true, false, false);
    CHECK(modal.shows == 1, "shown once the panel is gone");

    // Nothing newer, or announced before: nothing, and Settings opening touches no other toast.
    Sim none;
    none.Frame(true, false, false, false);
    none.Frame(true, false, false, true);
    CHECK(none.done && none.shows == 0 && none.withdrawals == 0, "no newer version: nothing to show or take down");
    Sim before;
    before.announced = true;
    before.Frame(true, true, false, false);
    before.Frame(true, true, false, true);
    CHECK(before.done && before.shows == 0 && before.withdrawals == 0, "announced before: nothing");

    // Every input combination: Withdraw only after a toast was shown, Show never under an open Settings or a panel.
    for (int bits = 0; bits < 64; bits++)
    {
        ToastInputs in{ bool(bits & 1), bool(bits & 2), bool(bits & 4), bool(bits & 8), bool(bits & 16), bool(bits & 32) };
        ToastStep step = NextToastStep(in);
        CHECK(step != ToastStep::Withdraw || (in.shown && in.settingsOpen), "withdraw only a shown toast under Settings (%d)", bits);
        CHECK(step != ToastStep::Show || (!in.settingsOpen && !in.modal && in.available && !in.announcedBefore && in.resultReady && !in.shown),
            "show only when nothing is in the way (%d)", bits);
    }

    printf("%s (%d failures)\n", g_failures ? "FAILED" : "passed", g_failures);
    return g_failures ? 1 : 0;
}
