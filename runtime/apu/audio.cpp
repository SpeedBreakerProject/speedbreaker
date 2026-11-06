// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// XAudio render driver. The game registers one client callback; the audio
// system calls it once per 256-sample frame (48 kHz, ~5.33 ms) and the
// callback submits the mixed frame with XAudioSubmitRenderDriverFrame
// (256 samples x 6 channels, planar big-endian float).
//
// Frames go to an SDL3 audio stream (5.1 float; SDL downmixes to whatever
// the device has; with Settings > Audio > 7.1 Speakers, 7.1 on an 8-channel
// device: apu/speaker_layout.h). The callback runs like the hardware's: one
// frame every 5.33 ms, evenly (the game's voices hold only a couple of frames of
// decoded audio). Calling it in
// bursts of whatever the device pulls at once (1024+ samples on PipeWire)
// left every voice empty for half of each burst: gameplay audio came out
// chopped 50% (two frames on, two off), thin and "tinny". SDL's queue is
// kept near two device buffers to absorb the device's pulls, and the
// cadence gains or skips a frame to follow the device clock, but never stops
// (apu/audio_cadence.h): all of the game's sound, its sound commands
// included, runs on this callback, so a device that stops taking audio must
// not stop it (GitHub issue #20: the sound stopped for good, then the game
// hung after the race). What such a device can't take is dropped, and the
// log says when it stopped and started taking audio again. NFSMW_MUTE=1
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
#include "audio_cadence.h"
#include "speaker_layout.h"
#include <kernel/function.h>
#include <cpu/guest_thread.h>
#include <cpu/guest_time.h>
#include <platform/timer.h>
#include <SDL3/SDL.h>
#ifdef __APPLE__
#include <pthread.h>
#endif
#include <user/settings.h>
#include <video/presenter.h>
#include <report/report.h>

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
    constexpr uint32_t CHANNELS = apu::kGameChannels;  // the game's frames: FL FR FC LFE SL SR
    // Set once, under s_streamMutex, which also orders starting the device
    // (OpenDevice, a guest thread) against pausing it (the main thread), so
    // a suspension can't slip between the check and the start.
    std::mutex s_streamMutex;
    SDL_AudioStream* s_stream = nullptr;
    // What the stream takes (apu/speaker_layout.h): 5.1, or 7.1 with 7.1
    // Speakers on and an 8-channel device. The audio thread sets it (and
    // OpenDevice before that thread starts); s_channels and
    // s_deviceChannels are read from any thread (QueuedMs, the menu).
    apu::SpeakerLayout s_layout = apu::SpeakerLayout::Game51;
    std::atomic<uint32_t> s_channels{ CHANNELS };
    std::atomic<int> s_deviceChannels{ 0 };  // 0: no device (yet)
    int FrameBytes() { return int(SAMPLES_PER_FRAME * s_channels.load(std::memory_order_relaxed) * sizeof(float)); }
    std::atomic<int> s_targetFrames{ 4 };  // SDL queue level to hold (frames of 256 samples)
    int s_deviceFrames = 0;                // the device's buffer at the stream's rate (FollowDevice)
    // What the hang report says about the audio (apu::DescribeState): when
    // the render callback last ran, when the game's mix last had sound, and
    // since when the device hasn't taken audio (0: it has); steady-clock ns.
    std::atomic<int64_t> s_lastRenderNs{ 0 }, s_lastSoundNs{ 0 }, s_deviceStalledNs{ 0 };
    std::atomic<uint64_t> s_dropped{ 0 };  // frames the device didn't take (dropped)
    // The cadence (the audio thread; the frames it renders are submitted on
    // that thread too).
    apu::AudioCadence s_cadence;

    int64_t SteadyNs()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // The stream's layout for its device's channels and the 7.1 Speakers
    // setting (apu/speaker_layout.h): when it changes (the setting, or a
    // device with other channels), the stream takes the new layout from
    // the next frame on; what is queued plays out in the old one (SDL keeps
    // each format's data apart). Off, or not 7.1, it stays 5.1 as before.
    void LogWide()
    {
        fprintf(stderr, "[audio] 7.1 Speakers: 7.1 output, the game's two surround channels on the back and side pairs (%.1f dB each)\n",
            20.0 * std::log10(double(apu::kSurroundShare)));
    }

    void FollowLayout(int deviceChannels)
    {
        const bool on = settings::GetBool(settings::Id::Speakers71);
        const apu::SpeakerLayout layout = apu::SpeakerLayoutFor(on, deviceChannels);
        static int noted = -1;  // the device channels last logged about
        if (layout != s_layout)
        {
            const SDL_AudioSpec spec{ SDL_AUDIO_F32, int(apu::ChannelsOf(layout)), int(SAMPLE_RATE) };
            if (!SDL_SetAudioStreamFormat(s_stream, &spec, nullptr))
            {
                static bool logged = false;  // it's tried again every second: say so once
                if (!std::exchange(logged, true))
                    fprintf(stderr, "[audio] 7.1 Speakers: couldn't switch the output to %s: %s (staying %s)\n",
                        apu::SpeakerLayoutName(layout), SDL_GetError(), apu::SpeakerLayoutName(s_layout));
                return;
            }
            s_layout = layout;
            s_channels.store(apu::ChannelsOf(layout), std::memory_order_relaxed);
            noted = deviceChannels;
            if (layout == apu::SpeakerLayout::Wide71)
                LogWide();
            else
                fprintf(stderr, "[audio] 7.1 Speakers: 5.1 output as before (%s, the device has %d channels)\n",
                    on ? "on" : "off", deviceChannels);
            return;
        }
        // Once per device: 7.1 in use; an 8-channel output with the setting
        // off plays the surrounds on one of its two pairs (SDL's 5.1 to 7.1),
        // and the log says where to change that; with it on but no 7.1
        // device, why nothing changed. Off on any other device: nothing.
        if (deviceChannels != noted)
        {
            noted = deviceChannels;
            if (layout == apu::SpeakerLayout::Wide71)
                LogWide();
            else if (!on && deviceChannels == 8)
                fprintf(stderr, "[audio] the output is 7.1 (8 channels): the game's 5.1 surround channels play on its back "
                    "pair only; Settings > Audio > 7.1 Speakers plays them on the side pair too\n");
            else if (on && layout == apu::SpeakerLayout::Game51)
                fprintf(stderr, "[audio] 7.1 Speakers is on, but the output has %d channels, not 8: 5.1 as before\n",
                    deviceChannels);
        }
    }

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
        s_deviceChannels.store(deviceSpec.channels, std::memory_order_relaxed);
        FollowLayout(deviceSpec.channels);
        if (deviceFrames > 0 && deviceSpec.freq > 0)
            deviceFrames = int(int64_t(deviceFrames) * SAMPLE_RATE / deviceSpec.freq);
        if (!opening && deviceFrames == s_deviceFrames)
            return;
        s_deviceFrames = deviceFrames;
        s_targetFrames = std::max(4, (2 * deviceFrames + int(SAMPLES_PER_FRAME) - 1) / int(SAMPLES_PER_FRAME));
        fprintf(stderr, "[audio] %s: %s, %s float at %u Hz (device %d ch, %s at %d Hz; device buffer %d samples, queue target %.1f ms)\n",
            opening ? "output" : "output now", SDL_GetAudioDeviceName(device), apu::SpeakerLayoutName(s_layout), SAMPLE_RATE, deviceSpec.channels,
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
        // NFSMW_TEST_AUDIO_DEVICE_CHANNELS=<n> (tests): the default output
        // device opened at n channels first, and held open: as a system whose
        // output has n channels (Windows opens a 7.1 receiver's device at 8,
        // whatever the game asks for). With SDL's disk driver
        // (SDL_AUDIO_DRIVER=disk, SDL_AUDIO_DISK_OUTPUT_FILE), a 7.1 output to
        // check without 7.1 speakers: what the device gets is in the file.
        if (const char* v = std::getenv("NFSMW_TEST_AUDIO_DEVICE_CHANNELS"); v && std::atoi(v) > 0)
        {
            const SDL_AudioSpec hold{ SDL_AUDIO_F32, std::atoi(v), int(SAMPLE_RATE) };
            if (SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &hold))
                fprintf(stderr, "[test] NFSMW_TEST_AUDIO_DEVICE_CHANNELS: the output device held open at %d channels\n", hold.channels);
            else
                fprintf(stderr, "[test] NFSMW_TEST_AUDIO_DEVICE_CHANNELS: couldn't open the output device: %s\n", SDL_GetError());
        }
        // 7.1 Speakers on and a default device with 8 channels: ask for 7.1
        // (the stream's format, and what the device is opened with where the
        // system lets the app choose). Otherwise 5.1, as before.
        apu::SpeakerLayout layout = apu::SpeakerLayout::Game51;
        if (settings::GetBool(settings::Id::Speakers71))
        {
            SDL_AudioSpec deviceSpec{};
            if (SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &deviceSpec, nullptr))
                layout = apu::SpeakerLayoutFor(true, deviceSpec.channels);
        }
        SDL_AudioSpec spec{ SDL_AUDIO_F32, int(apu::ChannelsOf(layout)), int(SAMPLE_RATE) };
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
            s_layout = layout;
            s_channels.store(apu::ChannelsOf(layout), std::memory_order_relaxed);
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
        // Windows (from rafidwayne's port, PR #39): MMCSS for this thread, and
        // its ticks on a high-resolution waitable timer (platform::SleepUntil):
        // sleep_until there wakes on the system tick, 1 ms only while
        // timeBeginPeriod(1) is honoured, which Windows 11 stops guaranteeing
        // while the window is occluded or minimized; at 15.6 ms every tick is
        // late and the catch-up bursts play silence.
        if (const char* priority = platform::RaiseAudioThreadPriority())
            fprintf(stderr, "[audio] pacing: a high-resolution timer, %s\n", priority);
        // Ticks that woke a whole interval or more late (a catch-up burst
        // follows), the worst, logged at most every 10 s.
        uint32_t lateTicks = 0;
        double worstLateMs = 0;
        auto lateLogged = steady_clock::now();
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
        // NFSMW_TEST_AUDIO_DEVICE_STALL=<s>[+<secs>] (tests): the output
        // device stops taking audio <s> seconds after this thread starts
        // (paused, as a device that stalls or that another app holds), for
        // good or for <secs>.
        double stallAt = -1, stallFor = 0;
        if (const char* v = std::getenv("NFSMW_TEST_AUDIO_DEVICE_STALL"))
        {
            char* end = nullptr;
            stallAt = std::strtod(v, &end);
            if (end && *end == '+')
                stallFor = std::strtod(end + 1, nullptr);
        }
        int stallState = 0;  // 0 before, 1 stalled, 2 over
        const auto started = steady_clock::now();
        while (s_running)
        {
            platform::SleepUntil(duration_cast<nanoseconds>(next.time_since_epoch()).count());
            {
                const auto woke = steady_clock::now();
                const double lateMs = duration<double, std::milli>(woke - next).count();
                if (lateMs >= duration<double, std::milli>(INTERVAL).count())
                {
                    lateTicks++;
                    worstLateMs = std::max(worstLateMs, lateMs);
                }
                if (lateTicks && woke - lateLogged >= seconds(10))
                {
                    fprintf(stderr, "[audio] pacing: %u ticks woke an interval or more late in %.0f s (worst %.1f ms)\n", lateTicks,
                        duration<double>(woke - lateLogged).count(), worstLateMs);
                    lateTicks = 0;
                    worstLateMs = 0;
                    lateLogged = woke;
                }
            }
            if (stallAt >= 0 && stallState < 2 && s_stream)
            {
                const double t = duration<double>(steady_clock::now() - started).count();
                if (stallState == 0 && t >= stallAt)
                {
                    stallState = 1;
                    fprintf(stderr, "[test] NFSMW_TEST_AUDIO_DEVICE_STALL: the output device stops taking audio (%.1f s)\n", t);
                    std::lock_guard lock(s_streamMutex);
                    SDL_PauseAudioStreamDevice(s_stream);
                }
                else if (stallState == 1 && stallFor > 0 && t >= stallAt + stallFor)
                {
                    stallState = 2;
                    fprintf(stderr, "[test] NFSMW_TEST_AUDIO_DEVICE_STALL: the output device takes audio again (%.1f s)\n", t);
                    std::lock_guard lock(s_streamMutex);
                    if (!guesttime::Suspended())
                        SDL_ResumeAudioStreamDevice(s_stream);
                }
            }
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
                // queue drifts from its target, never stopping the game's
                // sound (audio_cadence.h).
                int queued = std::max(0, SDL_GetAudioStreamQueued(s_stream)) / FrameBytes();
                frames = s_cadence.Frames(queued, s_targetFrames);
                if (s_cadence.Stalled() && !s_deviceStalledNs.load(std::memory_order_relaxed))
                {
                    s_deviceStalledNs.store(SteadyNs(), std::memory_order_relaxed);
                    fprintf(stderr, "[audio] the output device has taken no audio for %.1f s (%.0f ms queued): the game's sound "
                        "carries on, unheard, until it does\n", apu::AudioCadence::StallTicks(s_targetFrames) * apu::kFrameNs / 1e9,
                        1000.0 * queued * SAMPLES_PER_FRAME / SAMPLE_RATE);
                }
                if (s_cadence.Resumed())
                {
                    // What was queued before the stall is stale.
                    SDL_ClearAudioStream(s_stream);
                    const int64_t since = s_deviceStalledNs.exchange(0, std::memory_order_relaxed);
                    fprintf(stderr, "[audio] the output device takes audio again after %.1f s (%llu frames dropped in all)\n",
                        since ? double(SteadyNs() - since) / 1e9 : 0.0, (unsigned long long)s_dropped.load());
                }
            }
            for (int i = 0; i < frames && !guesttime::Suspended(); i++)
            {
                RenderFrame(ctx);
                s_rendered.fetch_add(1, std::memory_order_relaxed);
                s_lastRenderNs.store(SteadyNs(), std::memory_order_relaxed);
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

    std::string DescribeState()
    {
        const int64_t now = SteadyNs();
        auto ago = [now](int64_t t) { return t ? std::format("{:.1f} s ago", double(now - t) / 1e9) : std::string("never"); };
        std::string text = std::format("Render callbacks: {} (the last {}); frames from the game: {}; the game's mix last had sound {}.\n",
            s_rendered.load(), ago(s_lastRenderNs.load()), s_framesSubmitted.load(), ago(s_lastSoundNs.load()));
        if (!s_stream)
            return text + "Output device: none (muted by NFSMW_MUTE, or no device).\n";
        const int64_t stalled = s_deviceStalledNs.load();
        text += std::format("Output device: {:.1f} ms queued (target {:.1f} ms); {}; {} frames dropped in all.\n", QueuedMs(),
            1000.0 * s_targetFrames * SAMPLES_PER_FRAME / SAMPLE_RATE,
            stalled ? std::format("not taking audio for {:.1f} s", double(now - stalled) / 1e9) : std::string("taking audio"),
            s_dropped.load());
        return text;
    }

    double QueuedMs()
    {
        std::lock_guard lock(s_streamMutex);
        if (!s_stream)
            return 0.0;
        int queued = std::max(0, SDL_GetAudioStreamQueued(s_stream));
        return 1000.0 * queued / (s_channels.load(std::memory_order_relaxed) * sizeof(float) * SAMPLE_RATE);
    }

    int OutputChannels()
    {
        return s_deviceChannels.load(std::memory_order_relaxed);
    }

    bool SurroundOnBothPairs()
    {
        return s_channels.load(std::memory_order_relaxed) == 8;
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
    // When the game's mix last had sound (the hang report), and a line once it
    // has been silent for 10 s in a row (its sound stopped, or a silent
    // screen) and when it sounds again: a player's log shows when it dropped.
    if (samples)
    {
        static uint64_t silentFrames = 0;
        static double silentFrom = 0;
        const auto* words = static_cast<const uint32_t*>(samples);
        bool silent = true;
        for (uint32_t i = 0; i < SAMPLES_PER_FRAME * CHANNELS && silent; i++)
            silent = (words[i] & 0xFFFFFF7Fu) == 0;  // +0.0 or -0.0, big-endian
        constexpr uint64_t kSilentLog = 10 * SAMPLE_RATE / SAMPLES_PER_FRAME;
        if (!silent)
        {
            s_lastSoundNs.store(SteadyNs(), std::memory_order_relaxed);
            if (silentFrames >= kSilentLog)
                fprintf(stderr, "[audio] the game's mix sounds again after %.1f s of silence\n",
                    double(silentFrames) * SAMPLES_PER_FRAME / SAMPLE_RATE);
            silentFrames = 0;
        }
        else
        {
            if (silentFrames == 0)
                silentFrom = report::SessionSeconds();
            if (++silentFrames == kSilentLog)
                fprintf(stderr, "[audio] the game's mix has been silent for 10 s (since %.1f s)\n", silentFrom);
        }
    }
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
            // 7.1 Speakers, turned on or off in the menu: from this frame.
            FollowLayout(s_deviceChannels.load(std::memory_order_relaxed));
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
    // The frame as the stream takes it: 5.1 as the game mixed it, or 7.1
    // with the surrounds on both pairs (apu/speaker_layout.h).
    float out[SAMPLES_PER_FRAME * apu::kMaxChannels];
    const uint32_t outFloats = SAMPLES_PER_FRAME * apu::ChannelsOf(s_layout);
    apu::InterleaveFrame(static_cast<const uint32_t*>(samples), SAMPLES_PER_FRAME, s_layout, out);
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
    // A device that isn't taking audio (audio_cadence.h): the frame is
    // dropped, and the game's sound carries on, unheard.
    if (s_cadence.Stalled())
    {
        s_dropped.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    SDL_PutAudioStreamData(s_stream, out, int(outFloats * sizeof(float)));

    // NFSMW_AUDIO_DUMP=<file>: the final mix as raw interleaved float (48 kHz)
    // as the stream takes it: 6 channels, or 8 while 7.1 Speakers is in use
    // (a switch mid-dump changes it from there); from NFSMW_AUDIO_DUMP_FROM
    // seconds for NFSMW_AUDIO_DUMP_SECONDS (default 30).
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
                fwrite(out, outFloats * sizeof(float), 1, dump);
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
        for (uint32_t k = 0; k < outFloats; k++)
        {
            const float v = out[k];
            sumSquares += double(v) * v;
            peak = std::max(peak, std::fabs(v));
        }
        if (++frames == 375)
        {
            fprintf(stderr, "[audio] level: peak %.3f, rms %.4f\n", peak,
                std::sqrt(sumSquares / (375.0 * outFloats)));
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
