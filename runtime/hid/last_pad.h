// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Which controller is player 1's (GitHub #66), apart from SDL so
// tests/last_pad_test.cpp can drive it alone; header only. hid/input.cpp
// keeps the SDL side. Pads are SDL joystick ids.
//
//   - Player 1 follows the controller last pressed: outside split screen
//     every controller is open, and a press on one that isn't player 1's
//     makes it player 1's (a docked Steam Deck in Desktop Mode, whose own
//     controls SDL lists first, or a pad left switched on). The keyboard is
//     player 1's whatever pad is.
//   - A press is a button going down, a trigger pressed past half, or a
//     stick pushed past half its travel: each counts again only once it has
//     come back (a trigger or stick below a quarter). So a stick resting off
//     centre (drift, a worn pad) or a pad lying on a button never takes
//     player 1, and neither does what a pad reads as it is first seen (held
//     while it connected).
//   - In split screen nothing changes hands (Choose's `locked`): player 1
//     keeps the pad it had when split screen began, and the others join as
//     hid/player_slots.h says.
//   - Player 1's pad gone: of the ones left, the one pressed last (Fallback);
//     none pressed yet, the first SDL lists, as before.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hid
{
    class LastPad
    {
    public:
        static constexpr float kPress = 0.5f;  // a stick's travel (radially) or a trigger's past this is a press...
        static constexpr float kRest = 0.25f;  // ...and once it is back below this, the next one counts

        // A controller's state, as SDL reads it (before the deadzones): XInput
        // button bits, sticks and triggers as fractions of full travel.
        struct Sample
        {
            uint16_t buttons = 0;
            float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
        };

        // Once a frame for each open controller. Returns whether it was
        // pressed now (and remembers it for Choose and Fallback).
        bool Note(uint32_t pad, const Sample& s)
        {
            Entry& e = Find(pad);
            const float travel[4] = { s.lx * s.lx + s.ly * s.ly, s.rx * s.rx + s.ry * s.ry, s.lt * s.lt, s.rt * s.rt };
            bool pressed = e.seen && (s.buttons & ~e.buttons) != 0;
            for (int i = 0; i < 4; i++)
            {
                // Past kPress, held until back below kRest (travel squared).
                const bool was = e.held[i];
                e.held[i] = was ? travel[i] >= kRest * kRest : travel[i] > kPress * kPress;
                pressed |= e.seen && e.held[i] && !was;
            }
            e.seen = true;
            e.buttons = s.buttons;
            if (pressed)
            {
                e.last = ++m_presses;
                m_pressed.push_back(pad);
            }
            return pressed;
        }

        // The controller is gone (removed or closed).
        void Remove(uint32_t pad)
        {
            for (size_t i = 0; i < m_pads.size(); i++)
                if (m_pads[i].pad == pad)
                {
                    m_pads.erase(m_pads.begin() + std::ptrdiff_t(i));
                    break;
                }
            for (size_t i = 0; i < m_pressed.size();)
                m_pressed[i] == pad ? (void)m_pressed.erase(m_pressed.begin() + std::ptrdiff_t(i)) : (void)i++;
        }

        // After the frame's Notes: player 1's pad from now on. `current`
        // unless another was pressed this frame (the one pressed last), or
        // whatever was pressed if `locked` (split screen). Forgets the
        // frame's presses.
        uint32_t Choose(uint32_t current, bool locked)
        {
            uint32_t pick = current;
            if (!locked)
            {
                bool currentPressed = false;
                for (uint32_t p : m_pressed)
                    currentPressed |= p == current;
                if (!currentPressed && !m_pressed.empty())
                    pick = m_pressed.back();
            }
            m_pressed.clear();
            return pick;
        }

        // Player 1's pad gone: of `candidates` (the controllers that may take
        // it, in SDL's order), the one pressed last; none pressed, the first;
        // 0 if there are none.
        uint32_t Fallback(const std::vector<uint32_t>& candidates) const
        {
            uint32_t pick = candidates.empty() ? 0 : candidates.front();
            uint64_t best = 0;
            for (uint32_t c : candidates)
                for (const Entry& e : m_pads)
                    if (e.pad == c && e.last > best)
                    {
                        best = e.last;
                        pick = c;
                    }
            return pick;
        }

    private:
        struct Entry
        {
            uint32_t pad = 0;
            bool seen = false;  // the first Note is how it was found, not a press
            uint16_t buttons = 0;
            bool held[4] = {};  // left stick, right stick, LT, RT: past kPress, until back below kRest
            uint64_t last = 0;  // its last press (m_presses), 0: none
        };
        std::vector<Entry> m_pads;
        std::vector<uint32_t> m_pressed;  // this frame's, in order
        uint64_t m_presses = 0;

        Entry& Find(uint32_t pad)
        {
            for (Entry& e : m_pads)
                if (e.pad == pad)
                    return e;
            m_pads.push_back(Entry{ pad });
            return m_pads.back();
        }
    };
}
