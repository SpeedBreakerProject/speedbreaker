// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Host CPU placement and waiting for threads that ran on a hardware thread of
// their own on the 360: the command processor, and guest code that spins
// while it waits for it.
#pragma once

namespace hostcpu
{
    // Linux, first thing in main(): keeps one physical core, with its SMT
    // sibling, for the command processor: one of the fastest when the host's
    // cores differ in top speed (the Steam Machine's CPU has two 4.8 GHz
    // cores and four 3.5 GHz ones), the last one when they are alike and
    // there are at least four (the Deck: cpus 6,7). Every other thread,
    // created by this one from now on, runs on the remaining CPUs. Nothing
    // changes on 2-3 alike cores, or elsewhere. NFSMW_CP_OWN_CORE=0 turns it
    // off, =fast keeps it to CPUs with fast and slow cores.
    void ReserveFastCore();

    // The command processor moves itself onto the reserved core.
    void UseReservedCore(const char* who);

    // For threads the command processor starts (they inherit its CPUs): back
    // to the shared ones.
    void LeaveReservedCore();

    // Linux: a 1 us timer slack for the calling thread (the default 50 us
    // turns a 50 us sleep into ~100 us). Cheap to call again.
    void TightenTimerSlack();
}
