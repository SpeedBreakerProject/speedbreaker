// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// speaker_layout_test: Settings > Audio > 7.1 Speakers (apu/speaker_layout.h,
// GitHub issue #91), what the game's 5.1 frame is written as for SDL.
//   1. The layout: 7.1 only with the setting on and an 8-channel device;
//      5.1 for stereo, 5.1, 6.1, no device, and whenever the setting is off.
//   2. Off (Game51), the frame is bit for bit what audio.cpp's loop wrote
//      up to v0.2.4 (a copy of it here), for every kind of float: noise,
//      NaN, infinities, denormals, -0.
//   3. On (Wide71), each channel lands where SDL's 7.1 order wants it
//      (FL FR FC LFE BL BR SL SR): the front, centre and LFE bit for bit,
//      the left surround on BL and SL, the right on BR and SR, equal on
//      both pairs and kSurroundShare of the input.
//   4. The trim keeps the surrounds' total power: 2 x share^2 = 1 (-3 dB).
//   5. Silence stays silence, and the frame sizes are 6 and 8 channels.
// Build by hand: c++ -std=c++20 -O2 -I runtime tests/speaker_layout_test.cpp -o /tmp/speaker_layout_test
#include <apu/speaker_layout.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace
{
    int s_failed = 0;

    void Check(bool ok, const std::string& what)
    {
        if (!ok)
        {
            s_failed++;
            fprintf(stderr, "FAIL: %s\n", what.c_str());
        }
    }

    constexpr uint32_t kSamples = 256;

    uint32_t Bits(float v)
    {
        uint32_t b;
        std::memcpy(&b, &v, 4);
        return b;
    }

    float FromBits(uint32_t b)
    {
        float v;
        std::memcpy(&v, &b, 4);
        return v;
    }

    // A big-endian word as the guest wrote it, byte by byte (independent of
    // the header's shifts).
    uint32_t BigEndian(uint32_t host)
    {
        const uint8_t bytes[4] = { uint8_t(host >> 24), uint8_t(host >> 16), uint8_t(host >> 8), uint8_t(host) };
        uint32_t w;
        std::memcpy(&w, bytes, 4);  // little-endian host: the bytes as they sit in guest memory
        return w;
    }

    // audio.cpp's frame loop up to v0.2.4, as it was (ByteSwap there is a
    // plain 32-bit byte swap).
    void OldLoop(const uint32_t* in, float* out)
    {
        constexpr uint32_t CHANNELS = 6;
        for (uint32_t c = 0; c < CHANNELS; c++)
            for (uint32_t i = 0; i < kSamples; i++)
            {
                const uint32_t w = in[c * kSamples + i];
                uint32_t bits = ((w & 0xFFu) << 24) | ((w & 0xFF00u) << 8) | ((w >> 8) & 0xFF00u) | (w >> 24);
                float v;
                memcpy(&v, &bits, 4);
                out[i * CHANNELS + c] = v;
            }
    }

    // A frame of the game's (planar, big-endian) from host-order bit patterns.
    std::vector<uint32_t> Frame(const std::vector<uint32_t>& hostBits)
    {
        std::vector<uint32_t> f(hostBits.size());
        for (size_t k = 0; k < f.size(); k++)
            f[k] = BigEndian(hostBits[k]);
        return f;
    }

    std::vector<uint32_t> Patterns(uint32_t seed)
    {
        std::mt19937 rng(seed);
        std::uniform_real_distribution<float> audio(-1.0f, 1.0f);
        const uint32_t specials[] = { 0x00000000u, 0x80000000u, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0xFFC00001u,
                                      0x7F800001u, 0x00000001u, 0x807FFFFFu, 0x3F800000u, 0xBF800000u, 0x7F7FFFFFu };
        std::vector<uint32_t> v(kSamples * apu::kGameChannels);
        for (size_t k = 0; k < v.size(); k++)
        {
            switch (rng() % 4)
            {
            case 0: v[k] = specials[rng() % (sizeof(specials) / sizeof(specials[0]))]; break;
            case 1: v[k] = uint32_t(rng()); break;  // any bits at all
            default: v[k] = Bits(audio(rng)); break;
            }
        }
        return v;
    }

    void Layouts()
    {
        using apu::SpeakerLayout;
        for (int channels : { -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 16 })
        {
            Check(apu::SpeakerLayoutFor(false, channels) == SpeakerLayout::Game51,
                "off: 5.1 for a " + std::to_string(channels) + "-channel device");
            Check(apu::SpeakerLayoutFor(true, channels) == (channels == 8 ? SpeakerLayout::Wide71 : SpeakerLayout::Game51),
                "on: 7.1 only for an 8-channel device (" + std::to_string(channels) + ")");
        }
        Check(apu::ChannelsOf(SpeakerLayout::Game51) == 6 && apu::ChannelsOf(SpeakerLayout::Wide71) == 8, "6 and 8 channels");
        Check(apu::kGameChannels == 6 && apu::kMaxChannels == 8, "the game's 6, at most 8");
        Check(std::string(apu::SpeakerLayoutName(SpeakerLayout::Game51)) == "5.1" &&
                  std::string(apu::SpeakerLayoutName(SpeakerLayout::Wide71)) == "7.1", "the names the log uses");
    }

    void OffIsAsBefore()
    {
        for (uint32_t seed = 1; seed <= 200; seed++)
        {
            const std::vector<uint32_t> in = Frame(Patterns(seed));
            std::vector<float> before(kSamples * 6), now(kSamples * apu::kMaxChannels, FromBits(0xDEADBEEFu));
            OldLoop(in.data(), before.data());
            apu::InterleaveFrame(in.data(), kSamples, apu::SpeakerLayout::Game51, now.data());
            Check(std::memcmp(before.data(), now.data(), before.size() * sizeof(float)) == 0,
                "off: the frame is bit for bit the old loop's (seed " + std::to_string(seed) + ")");
            bool untouched = true;
            for (size_t k = before.size(); k < now.size(); k++)
                untouched = untouched && Bits(now[k]) == 0xDEADBEEFu;
            Check(untouched, "off: nothing written past 6 channels (seed " + std::to_string(seed) + ")");
        }
    }

    void OnPlacesEachChannel()
    {
        // Each channel its own value and sample index: a swapped pair, or a
        // channel read from the wrong plane, can't go unseen.
        std::vector<uint32_t> host(kSamples * 6);
        for (uint32_t c = 0; c < 6; c++)
            for (uint32_t i = 0; i < kSamples; i++)
                host[c * kSamples + i] = Bits(float(c + 1) * 0.1f + float(i) * 1e-4f);
        const std::vector<uint32_t> in = Frame(host);
        std::vector<float> out(kSamples * 8);
        apu::InterleaveFrame(in.data(), kSamples, apu::SpeakerLayout::Wide71, out.data());
        auto game = [&](uint32_t c, uint32_t i) { return FromBits(host[c * kSamples + i]); };
        bool front = true, pairs = true, share = true;
        for (uint32_t i = 0; i < kSamples; i++)
        {
            const float* o = &out[i * 8];
            for (uint32_t c = 0; c < 4; c++)  // FL FR FC LFE
                front = front && Bits(o[c]) == Bits(game(c, i));
            pairs = pairs && Bits(o[4]) == Bits(o[6]) && Bits(o[5]) == Bits(o[7]);  // BL = SL, BR = SR
            share = share && o[4] == game(4, i) * apu::kSurroundShare && o[5] == game(5, i) * apu::kSurroundShare;
        }
        Check(front, "on: FL FR FC LFE bit for bit in SDL's first four");
        Check(pairs, "on: the sides are the backs, bit for bit");
        Check(share, "on: the back/side pair is the game's left/right surround at kSurroundShare");
        // The left surround's value never on the right and the other way round.
        Check(std::fabs(out[4] - 0.5f * apu::kSurroundShare) < 1e-6f && std::fabs(out[5] - 0.6f * apu::kSurroundShare) < 1e-6f,
            "on: left surround on the left (BL, SL), right on the right (BR, SR)");

        // And with every kind of float, against the old loop's 5.1.
        for (uint32_t seed = 1; seed <= 200; seed++)
        {
            const std::vector<uint32_t> noise = Frame(Patterns(seed));
            std::vector<float> five(kSamples * 6), seven(kSamples * 8);
            OldLoop(noise.data(), five.data());
            apu::InterleaveFrame(noise.data(), kSamples, apu::SpeakerLayout::Wide71, seven.data());
            bool ok = true;
            for (uint32_t i = 0; i < kSamples && ok; i++)
            {
                for (uint32_t c = 0; c < 4; c++)
                    ok = ok && Bits(seven[i * 8 + c]) == Bits(five[i * 6 + c]);
                const float l = five[i * 6 + 4] * apu::kSurroundShare, r = five[i * 6 + 5] * apu::kSurroundShare;
                ok = ok && Bits(seven[i * 8 + 4]) == Bits(l) && Bits(seven[i * 8 + 6]) == Bits(l);
                ok = ok && Bits(seven[i * 8 + 5]) == Bits(r) && Bits(seven[i * 8 + 7]) == Bits(r);
            }
            Check(ok, "on: 7.1 from the same frame as 5.1 (seed " + std::to_string(seed) + ")");
        }
    }

    void Trim()
    {
        const double share = apu::kSurroundShare;
        Check(std::fabs(2.0 * share * share - 1.0) < 1e-6, "the two pairs' power adds up to one pair at full level");
        const double db = 20.0 * std::log10(share);
        Check(db > -3.02 && db < -3.00, "the trim is -3 dB per pair (" + std::to_string(db) + ")");
    }

    void Silence()
    {
        std::vector<uint32_t> in(kSamples * 6, 0);
        std::vector<float> out(kSamples * 8, 1.0f);
        apu::InterleaveFrame(in.data(), kSamples, apu::SpeakerLayout::Wide71, out.data());
        bool silent = true;
        for (float v : out)
            silent = silent && v == 0.0f;
        Check(silent, "on: a silent frame is a silent 7.1 frame");
    }
}

int main()
{
    Layouts();
    OffIsAsBefore();
    OnPlacesEachChannel();
    Trim();
    Silence();
    if (s_failed)
    {
        fprintf(stderr, "speaker_layout_test: %d check(s) failed\n", s_failed);
        return 1;
    }
    printf("speaker_layout_test: all checks passed\n");
    return 0;
}
