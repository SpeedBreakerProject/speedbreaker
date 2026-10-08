// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// XAudio render driver. The game registers one client callback; the audio
// system calls it once per 256-sample frame (48 kHz, ~5.33 ms) and the
// callback submits the mixed frame with XAudioSubmitRenderDriverFrame
// (256 samples x 6 channels, planar big-endian float).
//
// Frames go to an SDL3 audio stream (5.1 float; SDL downmixes to whatever
// the device has). The callback runs like the hardware's: one frame every
// 5.33 ms, evenly (the game's voices hold only a couple of frames of
// decoded audio). Calling it in
// bursts of whatever the device pulls at once (1024+ samples on PipeWire)
// left every voice empty for half of each burst: gameplay audio came out
// chopped 50% (two frames on, two off), thin and "tinny". SDL's queue is
// kept near two device buffers to absorb the device's pulls, and the
// cadence gains or skips a frame to follow the device clock. NFSMW_MUTE=1
// (or no audio device) keeps the same cadence with no output. The driver
// handle and wrapped argument follow Xenia.
//
// While the game is suspended (iOS: the app isn't the active one) the
// cadence stands still with guest time and the main thread pauses the
// device (apu::SuspendOutput), which keeps what is queued: on the resume
// that plays first and the cadence carries on at its old phase, with no
// frames owed.
#include <stdafx.h>
#include "audio.h"
#include <kernel/function.h>
#include <cpu/guest_thread.h>
#include <cpu/guest_time.h>
#include <SDL3/SDL.h>
#ifdef __APPLE__
#include <pthread.h>
#endif
#include <user/settings.h>
#include <video/presenter.h>

namespace
{
    constexpr uint32_t DRIVER_HANDLE_BASE = 0x41550000;  // Xenia
    constexpr uint32_t SAMPLES_PER_FRAME = apu::kSamplesPerFrame;
    constexpr uint32_t SAMPLE_RATE = apu::kSampleRate;

    std::atomic<uint32_t> s_callback{ 0 };
    std::atomic<uint32_t> s_callbackArgPtr{ 0 };  // guest pointer to the argument
    std::atomic<bool> s_running{ false };
    std::atomic<uint64_t> s_framesSubmitted{ 0 };
    std::atomic<uint64_t> s_rendered{ 0 };  // render callbacks (apu::RenderedFrames)
    constexpr uint32_t CHANNELS = 6;               // FL FR FC LFE SL SR
    // Set once, under s_streamMutex, which also orders starting the device
    // (OpenDevice, a guest thread) against pausing it (the main thread), so
    // a suspension can't slip between the check and the start.
    std::mutex s_streamMutex;
    SDL_AudioStream* s_stream = nullptr;
    constexpr int FRAME_BYTES = SAMPLES_PER_FRAME * CHANNELS * sizeof(float);
    std::atomic<int> s_targetFrames{ 4 };  // SDL queue level to hold (frames of 256 samples)
    int s_deviceFrames = 0;                // the device's buffer at the stream's rate (FollowDevice)

    // The queue level for the stream's device as it is now: two device
    // buffers (at the stream's rate), at least 4 frames. Logs it when it
    // changes, as when iOS moves the output to Bluetooth headphones, whose
    // buffer can be larger: a target sized for the speaker's then ran dry.
    void FollowDevice(bool opening)
    {
        SDL_AudioSpec deviceSpec{};
        int deviceFrames = 0;
        SDL_AudioDeviceID device = SDL_GetAudioStreamDevice(s_stream);
        if (!SDL_GetAudioDeviceFormat(device, &deviceSpec, &deviceFrames))
            return;
        if (deviceFrames > 0 && deviceSpec.freq > 0)
            deviceFrames = int(int64_t(deviceFrames) * SAMPLE_RATE / deviceSpec.freq);
        if (!opening && deviceFrames == s_deviceFrames)
            return;
        s_deviceFrames = deviceFrames;
        s_targetFrames = std::max(4, (2 * deviceFrames + int(SAMPLES_PER_FRAME) - 1) / int(SAMPLES_PER_FRAME));
        fprintf(stderr, "[audio] %s: %s, 5.1 float at %u Hz (device %d ch, %s at %d Hz; device buffer %d samples, queue target %.1f ms)\n",
            opening ? "output" : "output now", SDL_GetAudioDeviceName(device), SAMPLE_RATE, deviceSpec.channels,
            SDL_GetAudioFormatName(deviceSpec.format), deviceSpec.freq, deviceFrames,
            1000.0 * s_targetFrames * SAMPLES_PER_FRAME / SAMPLE_RATE);
    }

    void OpenDevice()
    {
        // NFSMW_MUTE=1 (automated runs): no device at all. A mute from the
        // menu keeps it open at gain 0, so unmuting works live.
        if (settings::EnvOverride(settings::Id::Mute) && settings::GetBool(settings::Id::Mute))
            return;
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
        {
            fprintf(stderr, "[audio] SDL audio init failed: %s (silent)\n", SDL_GetError());
            return;
        }
        SDL_AudioSpec spec{ SDL_AUDIO_F32, int(CHANNELS), int(SAMPLE_RATE) };
        SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (!stream)
        {
            fprintf(stderr, "[audio] no playback device: %s (silent)\n", SDL_GetError());
            return;
        }
        {
            // It opens paused. Opened while the game is suspended, it stays
            // so until apu::ResumeOutput starts it.
            std::lock_guard lock(s_streamMutex);
            s_stream = stream;
            if (!guesttime::Suspended())
                SDL_ResumeAudioStreamDevice(s_stream);
        }
        FollowDevice(true);
    }

    void RenderFrame(GuestThreadContext& ctx)
    {
        uint32_t callback = s_callback;
        if (callback != 0)
        {
            ctx.ppcContext.r3.u64 = s_callbackArgPtr;
            g_memory.FindFunction(callback)(ctx.ppcContext, g_memory.base);
        }
    }

    void AudioThread()
    {
        using namespace std::chrono;
#ifdef __APPLE__
        // Every 5.3 ms, or the queue runs dry and the output crackles. At the
        // default QoS an iPhone (two performance cores, both busy with the
        // game and the command processor) ran it late on an efficiency core.
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
        GuestThreadContext ctx(0);
        SetHostThreadName("nfsmw-audio");
        constexpr auto INTERVAL = nanoseconds(1000000000ull * SAMPLES_PER_FRAME / SAMPLE_RATE);
        // The time suspended that `next` allows for. Both taken while running
        // (XAudio can start this thread in a suspension): `next` is a host
        // time at which `paused` held.
        steady_clock::time_point next;
        int64_t paused;
        do
        {
            guesttime::WaitWhileSuspended();
            paused = guesttime::PausedNs();
            next = steady_clock::now();
        } while (guesttime::Suspended() || guesttime::PausedNs() != paused);
        while (s_running)
        {
            std::this_thread::sleep_until(next);
            // No guest audio time passes while the game is suspended: render
            // nothing (the main thread has paused the device). After it the
            // cadence moves on by exactly the time suspended, keeping its
            // phase in guest time (the movies' clock follows the audio
            // played): no frame is owed, and the queue is still at its
            // level, so there is no burst either.
            guesttime::WaitWhileSuspended();
            if (int64_t nowPaused = guesttime::PausedNs(); nowPaused != paused)
            {
                next += nanoseconds(nowPaused - paused);
                paused = nowPaused;
                continue;
            }
            next += INTERVAL;
            auto now = steady_clock::now();
            if (next < now - milliseconds(100))
                next = now;  // fell far behind (debugger, stall): don't burst
            int frames = 1;
            if (s_stream)
            {
                // The device can change under the stream (headphones on or
                // off): its buffer sets the target, so check it now and then.
                static uint32_t sinceCheck = 0;
                if (++sinceCheck >= SAMPLE_RATE / SAMPLES_PER_FRAME)  // ~1 s
                {
                    sinceCheck = 0;
                    FollowDevice(false);
                }
                // Follow the device clock: one frame more or less when the
                // queue drifts from its target.
                int queued = SDL_GetAudioStreamQueued(s_stream) / FRAME_BYTES;
                if (queued > s_targetFrames + 2)
                    frames = 0;
                else if (queued < s_targetFrames - 2)
                    frames = 2;
            }
            for (int i = 0; i < frames && !guesttime::Suspended(); i++)
            {
                RenderFrame(ctx);
                s_rendered.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

namespace apu
{
    void SuspendOutput()
    {
        std::lock_guard lock(s_streamMutex);
        if (s_stream)
            SDL_PauseAudioStreamDevice(s_stream);
    }

    void ResumeOutput()
    {
        std::lock_guard lock(s_streamMutex);
        if (s_stream && !guesttime::Suspended())
            SDL_ResumeAudioStreamDevice(s_stream);
    }

    uint64_t RenderedFrames()
    {
        return s_rendered.load(std::memory_order_relaxed);
    }

    double QueuedMs()
    {
        std::lock_guard lock(s_streamMutex);
        if (!s_stream)
            return 0.0;
        int queued = std::max(0, SDL_GetAudioStreamQueued(s_stream));
        return 1000.0 * queued / (CHANNELS * sizeof(float) * SAMPLE_RATE);
    }
}

uint32_t XAudioRegisterRenderDriverClient(be<uint32_t>* callback, be<uint32_t>* driver)
{
    // callback[0] = function, callback[1] = argument. Like Xenia, the callback
    // receives a pointer to a guest copy of the argument.
    uint32_t function = callback[0];
    uint32_t argument = callback[1];
    auto* argCopy = static_cast<be<uint32_t>*>(g_userHeap.Alloc(sizeof(uint32_t)));
    *argCopy = argument;

    if (s_callback != 0)
    {
        fprintf(stderr, "[audio] a second render driver client is not supported yet\n");
        assert(false && "multiple XAudio render clients");
    }
    s_callbackArgPtr = g_memory.MapVirtual(argCopy);
    s_callback = function;
    *driver = DRIVER_HANDLE_BASE;
    fprintf(stderr, "[audio] render client %08X (arg %08X) registered\n", function, argument);

    if (!s_running.exchange(true))
    {
        OpenDevice();
        std::thread(AudioThread).detach();
    }
    return 0;
}

uint32_t XAudioUnregisterRenderDriverClient(uint32_t driver)
{
    s_callback = 0;
    return 0;
}

// 256 samples x 6 channels, planar, big-endian float.
uint32_t XAudioSubmitRenderDriverFrame(uint32_t driver, void* samples)
{
    s_framesSubmitted++;
    if (!s_stream || !samples)
        return 0;
    // Volume, mute and mute-in-background, when they or the focus change.
    {
        static uint64_t generation = ~0ull;
        static bool wasFocused = true;
        bool focused = video::WindowFocused();
        if (uint64_t g = settings::Generation(); g != generation || focused != wasFocused)
        {
            generation = g;
            wasFocused = focused;
            bool muted = settings::GetBool(settings::Id::Mute);
            bool background = !focused && settings::GetBool(settings::Id::MuteInBackground);
            float volume = float(settings::GetInt(settings::Id::MasterVolume)) / 100.0f;
            float gain = muted || background ? 0.0f : volume * volume;  // squared: closer to perceived loudness
            SDL_SetAudioStreamGain(s_stream, gain);
            static float logged = -1.0f;
            if (gain != logged)
            {
                // With the output position, so a recording's level can be matched to it.
                logged = gain;
                fprintf(stderr, "[audio] gain %.4f (volume %d%%%s%s) from %.2f s of output\n", gain, int(volume * 100.0f + 0.5f),
                    muted ? ", muted" : "", background ? ", muted in the background" : "",
                    double(s_framesSubmitted) * SAMPLES_PER_FRAME / SAMPLE_RATE);
            }
        }
    }
    const auto* in = static_cast<const uint32_t*>(samples);
    float out[SAMPLES_PER_FRAME * CHANNELS];
    for (uint32_t c = 0; c < CHANNELS; c++)
        for (uint32_t i = 0; i < SAMPLES_PER_FRAME; i++)
        {
            uint32_t bits = ByteSwap(in[c * SAMPLES_PER_FRAME + i]);
            float v;
            memcpy(&v, &bits, 4);
            out[i * CHANNELS + c] = v;
        }
    // Cut-out detection: the device ran dry, or the game's frames stopped.
    // Gaps in guest time, so a suspension (the device paused with it) isn't
    // one, even for a frame that was under way as it began.
    {
        static int64_t lastFrame = guesttime::NowNs();
        int64_t guestNow = guesttime::NowNs();
        double gapMs = double(guestNow - lastFrame) / 1e6;
        lastFrame = guestNow;
        auto now = std::chrono::steady_clock::now();
        int queued = SDL_GetAudioStreamQueued(s_stream);
        static uint32_t underruns = 0, gaps = 0;
        static double worstGap = 0;
        if (queued == 0)
            underruns++;
        if (gapMs > 50)
            gaps++;
        worstGap = std::max(worstGap, gapMs);
        // Every 2 s with NFSMW_AUDIO_LOG; otherwise only when it went wrong,
        // so a player's log shows where a crackle came from.
        static const bool logStats = std::getenv("NFSMW_AUDIO_LOG") != nullptr;
        static auto nextLog = now + std::chrono::seconds(2);
        if (now >= nextLog && (logStats || underruns || gaps))
        {
            fprintf(stderr, "[audio] frames: %u device-empty on submit, %u gaps > 50 ms (worst %.0f ms)\n", underruns, gaps, worstGap);
            underruns = gaps = 0;
            worstGap = 0;
            nextLog = now + std::chrono::seconds(2);
        }
    }
    SDL_PutAudioStreamData(s_stream, out, sizeof(out));

    // NFSMW_AUDIO_DUMP=<file>: the final mix as raw interleaved 6-channel
    // float (48 kHz), from NFSMW_AUDIO_DUMP_FROM seconds for
    // NFSMW_AUDIO_DUMP_SECONDS (default 30).
    {
        static const char* dumpPath = std::getenv("NFSMW_AUDIO_DUMP");
        static const double from = [] { const char* v = std::getenv("NFSMW_AUDIO_DUMP_FROM"); return v ? std::atof(v) : 0.0; }();
        static const double seconds = [] { const char* v = std::getenv("NFSMW_AUDIO_DUMP_SECONDS"); return v ? std::atof(v) : 30.0; }();
        static const auto start = std::chrono::steady_clock::now();
        static FILE* dump = nullptr;
        static uint64_t dumped = 0;
        if (dumpPath && dumped < uint64_t(seconds * SAMPLE_RATE) &&
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >= from)
        {
            if (!dump && !(dump = fopen(dumpPath, "wb")))
                dumpPath = nullptr;
            if (dump)
            {
                fwrite(out, sizeof(out), 1, dump);
                dumped += SAMPLES_PER_FRAME;
                if (dumped >= uint64_t(seconds * SAMPLE_RATE))
                {
                    fclose(dump);
                    fprintf(stderr, "[audio] dump written: %s\n", dumpPath);
                }
            }
        }
    }

    // NFSMW_AUDIO_LOG=1: level meter every ~2 s.
    static const bool logLevels = std::getenv("NFSMW_AUDIO_LOG") != nullptr;
    if (logLevels)
    {
        static double sumSquares = 0;
        static float peak = 0;
        static uint32_t frames = 0;
        for (float v : out)
        {
            sumSquares += double(v) * v;
            peak = std::max(peak, std::fabs(v));
        }
        if (++frames == 375)
        {
            fprintf(stderr, "[audio] level: peak %.3f, rms %.4f\n", peak,
                std::sqrt(sumSquares / (375.0 * SAMPLES_PER_FRAME * CHANNELS)));
            sumSquares = 0;
            peak = 0;
            frames = 0;
        }
    }
    return 0;
}

uint32_t XAudioGetVoiceCategoryVolumeChangeMask(uint32_t driver, be<uint32_t>* mask)
{
    *mask = 0;
    return 0;
}

uint32_t XAudioGetVoiceCategoryVolume(uint32_t category, be<float>* volume)
{
    *volume = 1.0f;
    return 0;
}

GUEST_FUNCTION_HOOK(__imp__XAudioRegisterRenderDriverClient, XAudioRegisterRenderDriverClient);
GUEST_FUNCTION_HOOK(__imp__XAudioUnregisterRenderDriverClient, XAudioUnregisterRenderDriverClient);
GUEST_FUNCTION_HOOK(__imp__XAudioSubmitRenderDriverFrame, XAudioSubmitRenderDriverFrame);
GUEST_FUNCTION_HOOK(__imp__XAudioGetVoiceCategoryVolumeChangeMask, XAudioGetVoiceCategoryVolumeChangeMask);
GUEST_FUNCTION_HOOK(__imp__XAudioGetVoiceCategoryVolume, XAudioGetVoiceCategoryVolume);
