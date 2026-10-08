// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The device's thermal state, for the log, the [perf] line and Frame Rate's
// Auto (video/frame_rate.h). Apple (macOS and iOS) reads NSProcessInfo's
// thermalState (platform/apple/thermal.mm); other systems have none yet
// (Unknown). Read about once a second on the main thread (video::RunFrame
// calls Poll), off the command processor's path, so Current() costs one
// atomic load anywhere. Each change goes to the log ("[thermal] nominal ->
// fair at 123.4 s").
//
// NFSMW_THERMAL_SIM="60:serious,120:fair" (states nominal, fair, serious,
// critical; seconds since launch, the log's clock) reports those states from
// those times on instead, on any system: for testing. Before its first time,
// the real reading stands.
#pragma once
#include <cstdint>

namespace platform::thermal
{
    // iOS throttles the CPU and GPU from Serious on; Critical is the last
    // step before it shuts things down.
    enum class State : uint8_t { Unknown, Nominal, Fair, Serious, Critical };

    // Any thread: the state as last polled (Unknown before the first poll,
    // headless, or on a system without a reading).
    State Current();
    // "nominal", "fair", "serious", "critical", "unknown".
    const char* Name(State state);
    // Main thread, every frame: reads the state when a second has passed
    // since the last read.
    void Poll();

#ifdef __APPLE__
    namespace detail
    {
        // NSProcessInfo's thermalState (platform/apple/thermal.mm).
        State ReadSystem();
    }
#endif
}
