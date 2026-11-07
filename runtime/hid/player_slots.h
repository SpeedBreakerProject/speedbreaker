// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Which controller is which player in split screen (game/split_screen.h),
// apart from SDL so tests/player_slots_test.cpp can drive it alone; header
// only. hid/input.cpp keeps the SDL side. Pads are SDL joystick ids. Player
// 1 is XInput user 0: the keyboard and the controller that has been driving
// the game (the one pressed last before split screen, hid/last_pad.h),
// never one of these.
//
//   - Split mode follows the game's own predicate: on the moment the game
//     says split screen (Quick Race > Split Screen chosen, before "Player 2
//     press START"), off kLeaveNs after it last did, so a flicker between
//     screens changes nothing.
//   - Outside split mode no other controller is a player: XInput users 1-3
//     read not connected, as in v0.1.7. Single player sees one controller,
//     so a second pad can't take over the title screen's "press START" (the
//     game makes whichever port presses it the active one) or anything else.
//   - In split mode a pad becomes a player the first time it is used (a
//     button, or a trigger pressed past half): the lowest free user 1-3, kept
//     while it stays connected. The game then does the rest: the first START
//     from a port other than player 1's at "Player 2 press START" is player 2
//     (FEDatabase byte 13). A pad that never sends input (Steam Input's or a
//     receiver's phantom devices) never takes a user.
//   - The press that made it a player still reaches the game, as one press:
//     its user reads connected and idle for kArrivalNs, then the buttons
//     that made it one held for at least kReplayNs (Shown), then the pad.
//     The game ignores a press already down when a port connects (its
//     PLUGGED flush sets each control's previous value to -1, and a press
//     needs a previous value in [0, 0.5): two polls after the connection),
//     and "Player 2 press START" (sub_825696C8) takes START's release
//     (PAD_START_RELEASED): so a tap shorter than the idle time is still a
//     press and a release.
//   - Removed (unplugged, Bluetooth gone): its user reads not connected, and
//     the game's own handling takes over (in a race it pauses under a bare
//     PAUSE banner: on a 360 the console's own overlay said what to do), and
//     the user is Missing until a pad takes it again (input.cpp shows a
//     notice meanwhile); plugged in again it takes the lowest free user, its
//     old one unless another pad took it meanwhile.
//   - Leaving split mode frees every user (input.cpp closes those pads).
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace hid
{
    class PlayerSlots
    {
    public:
        static constexpr int kUsers = 4;                       // XInput users 0-3; 0 is player 1
        static constexpr int64_t kLeaveNs = 2'000'000'000;     // split mode ends this long after the game's predicate
        static constexpr int64_t kArrivalNs = 50'000'000;      // a new player's user reads idle this long first
        static constexpr int64_t kReplayNs = 100'000'000;      // then the buttons that made it one, at least this long

        // Once a frame, with the game's predicate (guest ns). Returns the
        // mode; Changed() says whether this call changed it. Leaving frees
        // every user and forgets which pads were used.
        bool NoteSplit(bool predicate, int64_t now)
        {
            m_changed = false;
            if (predicate)
            {
                m_lastSaid = now;
                if (!m_split)
                    m_split = m_changed = true;
            }
            else if (m_split && now - m_lastSaid >= kLeaveNs)
            {
                m_split = false;
                m_changed = true;
                m_pads.clear();
                m_missing = 0;
            }
            return m_split;
        }
        bool Split() const { return m_split; }
        bool Changed() const { return m_changed; }

        // A pad's input now: its buttons (XInput bits), and whether a
        // trigger is past half. Returns its user (0: none). A pad not known
        // yet is added.
        int NoteInput(uint32_t pad, uint16_t buttons, bool trigger, int64_t now)
        {
            Entry& e = Find(pad);
            if (buttons || trigger)
                e.used = true;
            if (m_split && e.used && e.user == 0)
                for (int u = 1; u < kUsers; u++)
                    if (!Connected(u))
                    {
                        e.user = u;
                        e.since = now;
                        e.arrival = buttons;
                        m_missing &= ~(1u << u);
                        break;
                    }
            return e.user;
        }

        // The pad is gone (removed or closed). Returns the user it had (0: none).
        int Remove(uint32_t pad)
        {
            for (size_t i = 0; i < m_pads.size(); i++)
                if (m_pads[i].pad == pad)
                {
                    const int user = m_pads[i].user;
                    m_pads.erase(m_pads.begin() + std::ptrdiff_t(i));
                    if (m_split && user != 0)
                        m_missing |= 1u << user;
                    return user;
                }
            return 0;
        }

        // User `user` (1-3) lost its pad in split screen and no pad has taken
        // it since (a notice says to reconnect it).
        bool Missing(int user) const { return user > 0 && user < kUsers && ((m_missing >> user) & 1) != 0; }

        int UserOf(uint32_t pad) const
        {
            for (const Entry& e : m_pads)
                if (e.pad == pad)
                    return e.user;
            return 0;
        }

        // User `user` (1-3): whether a pad is it; its pad (0: none); whether
        // it is still arriving (reads idle).
        bool Connected(int user) const { return Has(user); }
        uint32_t PadOf(int user) const
        {
            for (const Entry& e : m_pads)
                if (e.user == user && user != 0)
                    return e.pad;
            return 0;
        }
        bool Arriving(int user, int64_t now) const
        {
            for (const Entry& e : m_pads)
                if (e.user == user && user != 0)
                    return now - e.since < kArrivalNs;
            return false;
        }
        // The buttons user `user` shows the game, its pad's being `buttons`:
        // none while arriving, then the arrival press held to kReplayNs.
        uint16_t Shown(int user, uint16_t buttons, int64_t now) const
        {
            for (const Entry& e : m_pads)
                if (e.user == user && user != 0)
                {
                    if (now - e.since < kArrivalNs)
                        return 0;
                    if (now - e.since < kArrivalNs + kReplayNs)
                        return uint16_t(buttons | e.arrival);
                    return buttons;
                }
            return buttons;
        }

    private:
        struct Entry
        {
            uint32_t pad = 0;
            bool used = false;
            int user = 0;
            int64_t since = 0;
            uint16_t arrival = 0;  // the buttons down when it became a player
        };
        std::vector<Entry> m_pads;
        bool m_split = false, m_changed = false;
        uint32_t m_missing = 0;  // users whose pad went in split screen, not taken since
        int64_t m_lastSaid = 0;

        Entry& Find(uint32_t pad)
        {
            for (Entry& e : m_pads)
                if (e.pad == pad)
                    return e;
            m_pads.push_back(Entry{ pad });
            return m_pads.back();
        }
        bool Has(int user) const
        {
            return user != 0 && std::any_of(m_pads.begin(), m_pads.end(), [user](const Entry& e) { return e.user == user; });
        }
    };
}
