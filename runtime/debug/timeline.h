// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// NFSMW_TIMELINE=<seconds>: from that many seconds after launch, record
// 200 ms of pipeline events (game waits, command processor batches,
// submissions, completions, fence publication) and write them to
// build/timeline.txt with a per-event summary. Near-zero cost when off.
#pragma once
#include <atomic>
#include <cstdint>

namespace timeline
{
    enum Kind : uint8_t
    {
        GameWaitBegin, GameWaitEnd,     // D3D wait for GPU progress (arg: target)
        GamePresentBegin, GamePresentEnd,
        BatchBegin, BatchEnd,           // command processor ring batch (arg: dwords)
        Submit,                         // arg: slot | writes << 8
        Complete,                       // arg: slot
        WriteByEvent, WriteAtComplete,  // deferred write published
        Swap,                           // XE_SWAP processed
        RegMemBegin, RegMemEnd,         // a blocking WAIT_REG_MEM (arg: address/register)
        Interrupt,                      // graphics interrupt delivered (arg: source)
        SubmitBegin,                    // SubmitRecorded entered (arg: site)
        GameWaitTarget,                 // first poll of a D3D wait (arg: issued - target fence)
        Count
    };
    extern bool g_enabled;
    inline thread_local bool t_waitTargetMarked = true;
    // Always counted, for the command processor's hitch reports: time the
    // game spent in D3D's GPU waits and in Present, and its longest frame
    // (Present to Present) since the report last took it.
    inline std::atomic<uint64_t> g_gameWaitNs{ 0 }, g_gamePresentNs{ 0 }, g_gameLongestFrameNs{ 0 };
    void Record(Kind kind, uint32_t arg = 0);
    inline void Mark(Kind kind, uint32_t arg = 0)
    {
        if (g_enabled) [[unlikely]]
            Record(kind, arg);
    }
}
