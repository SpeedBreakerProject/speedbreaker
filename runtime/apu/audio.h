// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The XAudio render driver (audio.cpp), as the rest of the runtime sees it
// while the game is suspended (iOS: the app isn't the active one; see
// cpu/guest_time.h).
#pragma once
#include <cstdint>

namespace apu
{
    // Main thread (video's SuspendGame and ResumeGame), idempotent. Suspend
    // pauses the output device: it plays silence without taking what is
    // queued, which then plays first on the resume. Resume starts it again,
    // after guesttime::Resume(): never while the game is still suspended.
    // The render cadence stops and starts with guest time on its own.
    void SuspendOutput();
    void ResumeOutput();

    // A render callback's frame: 256 samples at 48 kHz.
    inline constexpr uint32_t kSamplesPerFrame = 256, kSampleRate = 48000;
    inline constexpr double kFrameNs = kSamplesPerFrame * 1e9 / kSampleRate;  // 5.33 ms

    // Any thread: render callbacks so far (one frame of guest time each),
    // and the audio queued for the device (ms; 0 without one).
    uint64_t RenderedFrames();
    double QueuedMs();
}
