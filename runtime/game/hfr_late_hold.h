// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Smooth Motion off Apple's systems (game/hfr.cpp): how long an even 60 lasts
// after a picture was shown on another refresh than the others (a ghost,
// hfr::NoteLatePicture). Every such picture drops the game to 60 at once;
// this only says for how long. Header only: tests/hfr_late_hold_test.cpp.
//
// v0.2.0's first rule (4491930) doubled the hold whenever a ghost came within
// a minute of coming back, up to 2 minutes. On the Steam Machine at 120 Hz
// (nested gamescope, the v0.2.0 stage) the menus, the race's movie and its
// load each had a lone one 20-45 s apart, at 120 fps menus with nothing over
// budget, so the hold went 8, 16, 32, 64 s and the race ran at 60 from its
// start (sm120-on: 60 from 110 s to 174 s; sm120b: holds to 120 s, the race
// at 60). What it is for, a machine that can't show the pictures on time
// (the Deck OLED at 90 Hz: every other picture late), ghosts again within a
// second or two of each return. So:
//   - a ghost within kRepeatNs of coming back is a repeat: twice the last
//     hold, at most kMaxNs;
//   - any other one is a one-off and gets kFirstNs again: a lone ghost after
//     a clean stretch (the repeats' count starts over), or one within
//     kNearHitchNs of a load or a long hitch (a render entry more than 100 ms
//     after the last), whose timing a load knocks about at any rate.
#pragma once
#include <algorithm>
#include <cstdint>

namespace hfr::late
{
    constexpr int64_t kFirstNs = 8'000'000'000, kMaxNs = 32'000'000'000;
    constexpr int64_t kRepeatNs = 10'000'000'000, kNearHitchNs = 5'000'000'000;

    struct Hold
    {
        int64_t hold = 0;  // the last ghost's hold (0: none yet)
        int64_t at = 0;    // when it came
    };

    enum class Kind
    {
        kFirst,   // none before
        kOneOff,  // a clean stretch since coming back
        kHitch,   // close to a load or a long hitch
        kRepeat,  // soon after coming back: twice the last hold
    };

    // A ghost at `now`: the hold it gets (stored in `h`). `resumedAt`: when
    // the in-between pictures last came back (0: never), `hitchAt`: the last
    // load's or long hitch's time (0: none).
    inline Kind Next(Hold& h, int64_t now, int64_t resumedAt, int64_t hitchAt)
    {
        Kind k;
        if (!h.hold)
            k = Kind::kFirst;
        else if (hitchAt && now - hitchAt < kNearHitchNs)
            k = Kind::kHitch;
        else if (resumedAt && resumedAt >= h.at && now - resumedAt < kRepeatNs)
            k = Kind::kRepeat;
        else
            k = Kind::kOneOff;
        h.hold = k == Kind::kRepeat ? std::min(h.hold * 2, kMaxNs) : kFirstNs;
        h.at = now;
        return k;
    }

    inline const char* Name(Kind k)
    {
        switch (k)
        {
        case Kind::kRepeat: return "again soon after coming back: twice the last hold";
        case Kind::kHitch: return "close to a load or a long hitch: a one-off";
        case Kind::kOneOff: return "after a clean stretch: a one-off";
        default: return "the first";
        }
    }
}
