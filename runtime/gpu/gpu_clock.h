// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The GPU's clock for the [perf] line, where the kernel keeps the time spent
// at each frequency (devfreq's trans_stat: the Steam Frame's Adreno, under
// /sys/class/devfreq/<...>.gpu). It tells a GPU that is slower because it
// clocks down (heat, power) from one that is shared (the headset's compositor).
#pragma once

namespace gpu::clock
{
    // The average clock since the previous call, in MHz; 0 where unknown
    // (no devfreq GPU: amdgpu, macOS) and on the first call.
    double AverageMHzSinceLast();
}
