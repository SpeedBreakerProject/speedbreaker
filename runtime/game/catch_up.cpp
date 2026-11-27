// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See catch_up.h.
#include <stdafx.h>
#include "catch_up.h"

#include <kernel/function.h>
#include <user/settings.h>

#include <cmath>

// CatchUpAidWeight, before `lbz r11,46(r27)` at 0x823329D0 in sub_823328C0: f31
// holds the catch-up weight the AI is about to use, just loaded at 0x823329CC
// from 0x82061C90's { 0, 0.5, 1 } by sub_8233A6A0's bucket (or 1.0 when a
// branch above passed it in, or from the AI's own curve at r31+17764 for a
// race type of 2: those routes merge here too).
void CatchUpAidWeight(PPCRegister& f31)
{
    // Settings > Game > Catch Up (NFSMW_CATCHUP overrides it for a run). A
    // lock-free read, so it follows the menu without a listener.
    const float strength = settings::GetFloat(settings::Id::CatchUp);
    if (!game::catch_up::Scaled(strength))
        return;
    const double value = f31.f64;
    if (!std::isfinite(value) || value < 0.0 || value > 1.0)
        return;
    f31.f64 = double(game::catch_up::ScaleWeight(float(value), strength));
}
