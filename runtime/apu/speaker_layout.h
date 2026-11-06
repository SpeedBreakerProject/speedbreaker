// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// What the game's 5.1 mix is written as for the output device (apu/audio.cpp),
// header only so that tests/speaker_layout_test.cpp runs it.
//
// The game mixes six channels, as on the Xbox 360: front left and right,
// centre, LFE and two surrounds (XAudioSubmitRenderDriverFrame: 256 samples
// x 6 channels, planar big-endian float). They go to SDL as 5.1, whose last
// two channels SDL calls the back pair (FL FR FC LFE BL BR). On a 7.1
// device (FL FR FC LFE BL BR SL SR) SDL puts them on the back pair and leaves
// the side pair silent (SDL_Convert51To71); a receiver may name that back pair
// "rear" or "surround back". GitHub issue #91: Igor4276's 7.1 receiver over
// HDMI played the front, centre, sub and rear speakers, and the sides never.
//
// Settings > Audio > 7.1 Speakers (off by default) writes the frame as 7.1
// instead when the device has 8 channels: front, centre and LFE as they are,
// and the two surround channels on BOTH the back and the side pair, each at
// kSurroundShare (-3 dB). Nothing new is mixed or positioned: it is the same
// two channels, so every speaker plays. -3 dB keeps the surrounds' total
// power what one pair at full level gave (2 x 0.5 = 1): the balance against
// the front stays as the game mixed it. At full level on both pairs they'd
// come out 3 dB louder than mixed (up to 6 dB at low frequencies, where the
// two pairs add in phase); at half (-6 dB), 3 dB quieter in the room.
//
// Off, or on an output that isn't 7.1, the frame is written as 5.1 by the
// same loop as before (Game51): the same bits, and SDL converts for the device
// (to stereo, or 5.1 to 7.1 as above) exactly as it did.
#pragma once
#include <cstdint>
#include <cstring>

namespace apu
{
    enum class SpeakerLayout : uint8_t
    {
        Game51,  // the game's six channels as SDL's 5.1: FL FR FC LFE BL BR
        Wide71,  // SDL's 7.1, the surrounds on both pairs: FL FR FC LFE BL BR SL SR
    };

    inline constexpr uint32_t kGameChannels = 6;  // FL FR FC LFE SL SR, the game's mix
    inline constexpr uint32_t kMaxChannels = 8;
    // Each pair's share of the surround channels in Wide71: 1/sqrt(2), -3 dB.
    inline constexpr float kSurroundShare = 0.70710678f;

    // The layout for this output: Wide71 only with the setting on and an
    // 8-channel (7.1) device; 5.1 as before otherwise (stereo, 5.1, 6.1, no
    // device, or a device whose format isn't known: 0).
    constexpr SpeakerLayout SpeakerLayoutFor(bool speakers71, int deviceChannels)
    {
        return speakers71 && deviceChannels == 8 ? SpeakerLayout::Wide71 : SpeakerLayout::Game51;
    }

    constexpr uint32_t ChannelsOf(SpeakerLayout layout)
    {
        return layout == SpeakerLayout::Wide71 ? 8u : kGameChannels;
    }

    constexpr const char* SpeakerLayoutName(SpeakerLayout layout)
    {
        return layout == SpeakerLayout::Wide71 ? "7.1" : "5.1";
    }

    // One of the game's frames (`samples` samples x 6 channels, planar,
    // big-endian float) as interleaved host floats for SDL in `layout`:
    // samples x ChannelsOf(layout) floats in `out`.
    inline void InterleaveFrame(const uint32_t* planarBigEndian, uint32_t samples, SpeakerLayout layout, float* out)
    {
        auto sample = [&](uint32_t c, uint32_t i)
        {
            uint32_t bits = planarBigEndian[c * samples + i];
            bits = (bits >> 24) | ((bits >> 8) & 0x0000FF00u) | ((bits << 8) & 0x00FF0000u) | (bits << 24);
            float v;
            std::memcpy(&v, &bits, 4);
            return v;
        };
        if (layout == SpeakerLayout::Game51)
        {
            for (uint32_t c = 0; c < kGameChannels; c++)
                for (uint32_t i = 0; i < samples; i++)
                    out[i * kGameChannels + c] = sample(c, i);
            return;
        }
        for (uint32_t i = 0; i < samples; i++)
        {
            float* o = out + i * 8;
            o[0] = sample(0, i);  // FL
            o[1] = sample(1, i);  // FR
            o[2] = sample(2, i);  // FC
            o[3] = sample(3, i);  // LFE
            const float left = sample(4, i) * kSurroundShare, right = sample(5, i) * kSurroundShare;
            o[4] = left;   // BL
            o[5] = right;  // BR
            o[6] = left;   // SL
            o[7] = right;  // SR
        }
    }
}
