// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// XMA decoder contexts. The context layout and API semantics follow Xenia
// (xenia-canary: apu/xma_context_new.cc, kernel/xboxkrnl/xboxkrnl_audio_xma.cc).
//
// Decoding is frame by frame, like the hardware: from the bit offset in
// input_buffer_read_offset, each XMA frame (15-bit length prefix, possibly
// split across packets of its stream, which follow the packet skip counts
// and can continue into the other input buffer) is copied into a synthetic
// single-frame XMA packet for FFmpeg's XMA1 decoder, which keeps its MDCT
// overlap across frames. Each frame yields 512 samples (4 output blocks per
// channel) of big-endian s16 in the output ring. So:
//   - hardware loops jump at the exact frame (loop_start/loop_end bit
//     offsets) and subframe (loop_subframe_end/skip), with no decoder reset:
//     the overlap carries across the loop point as on the console;
//   - a read offset set in the middle of a packet (a seek) starts exactly there;
//   - the frame FFmpeg drops after a fresh start (its warm-up) is output as
//     silence in its own place, at the start of the stream.
// The previous packet-at-a-time decoder looped at packet granularity with a
// decoder flush, and put the missing first frame's silence in the middle of
// every sound: gameplay audio (SFX restarted all the time) was full of holes.
//
// A context decodes one pass per XMAEnableContext kick (Xenia's Work()): it
// fills the output ring as far as it has room, then waits for the next kick.
// Ring semantics: write == read with output_buffer_valid set means empty; a
// pass that fills the ring clears output_buffer_valid (full). A pass that
// starts with no input moves write_offset to read_offset and clears valid
// only if unread output remains: NFSMW uses write == read to detect a
// stalled voice, and reads write == read with valid clear as a FULL ring
// (clearing valid on an empty ring left its stream player waiting forever:
// the mid-race audio cut-out). Unlike Xenia, that fake full ring is undone
// when input arrives before the game has read past the real output: EA's
// player queues speech and music as sub-streams, and one queued after the
// last had drained would start with a ring of stale audio and lose its tail.
// Where more data can still come (the other input buffer isn't valid yet),
// a frame split across it waits for it, its length field included (Xenia
// drops such a frame); a frame that runs past data that can never come is
// skipped.
// NFSMW_XMA_SILENT=1 outputs zero PCM at the same rate.
#include <stdafx.h>
#include <kernel/function.h>
#include <kernel/vmem.h>
#include <cpu/guest_thread.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}

uint32_t MmGetPhysicalAddress(uint32_t address);  // imports_memory.cpp

namespace
{
    // Xenia's XMA_CONTEXT_DATA: 16 big-endian dwords, bitfields LSB first.
    struct XmaContextData
    {
        // DWORD 0
        uint32_t input_buffer_0_packet_count : 12;
        uint32_t loop_count : 8;
        uint32_t input_buffer_0_valid : 1;
        uint32_t input_buffer_1_valid : 1;
        uint32_t output_buffer_block_count : 5;
        uint32_t output_buffer_write_offset : 5;
        // DWORD 1
        uint32_t input_buffer_1_packet_count : 12;
        uint32_t loop_subframe_start : 2;
        uint32_t loop_subframe_end : 3;
        uint32_t loop_subframe_skip : 3;
        uint32_t subframe_decode_count : 4;
        uint32_t subframe_skip_count : 3;
        uint32_t sample_rate : 2;
        uint32_t is_stereo : 1;
        uint32_t unk_dword_1_c : 1;
        uint32_t output_buffer_valid : 1;
        // DWORD 2
        uint32_t input_buffer_read_offset : 26;
        uint32_t unk_dword_2 : 6;
        // DWORD 3
        uint32_t loop_start : 26;
        uint32_t unk_dword_3 : 6;
        // DWORD 4
        uint32_t loop_end : 26;
        uint32_t packet_metadata : 5;
        uint32_t current_buffer : 1;
        // DWORD 5-8
        uint32_t input_buffer_0_ptr;
        uint32_t input_buffer_1_ptr;
        uint32_t output_buffer_ptr;
        uint32_t work_buffer_ptr;
        // DWORD 9
        uint32_t output_buffer_read_offset : 5;
        uint32_t : 25;
        uint32_t stop_when_done : 1;
        uint32_t interrupt_when_done : 1;
        // DWORD 10-15
        uint32_t unk_dwords_10_15[6];

        static XmaContextData Load(const void* guest)
        {
            XmaContextData d;
            auto* src = static_cast<const uint32_t*>(guest);
            auto* dst = reinterpret_cast<uint32_t*>(&d);
            for (size_t i = 0; i < sizeof(d) / 4; i++)
                dst[i] = ByteSwap(src[i]);
            return d;
        }

        void Store(void* guest) const
        {
            auto* src = reinterpret_cast<const uint32_t*>(this);
            auto* dst = static_cast<uint32_t*>(guest);
            for (size_t i = 0; i < sizeof(*this) / 4; i++)
                dst[i] = ByteSwap(src[i]);
        }
    };
    static_assert(sizeof(XmaContextData) == 64);

    struct XmaLoopData
    {
        be<uint32_t> loopStart;
        be<uint32_t> loopEnd;
        uint8_t loopCount;
        uint8_t loopSubframeEnd;
        uint8_t loopSubframeSkip;
    };

    struct XmaContextInit
    {
        be<uint32_t> inputBuffer0Ptr;
        be<uint32_t> inputBuffer0PacketCount;
        be<uint32_t> inputBuffer1Ptr;
        be<uint32_t> inputBuffer1PacketCount;
        be<uint32_t> inputBufferReadOffset;
        be<uint32_t> outputBufferPtr;
        be<uint32_t> outputBufferBlockCount;
        be<uint32_t> workBuffer;
        be<uint32_t> subframeDecodeCount;
        be<uint32_t> channelCount;
        be<uint32_t> sampleRate;
        XmaLoopData loopData;
    };
    static_assert(sizeof(XmaContextInit) == 56);

    constexpr uint32_t CONTEXT_COUNT = 320;          // hardware contexts
    constexpr uint32_t OUTPUT_BLOCK_SIZE = 256;      // bytes per output block (a subframe of one channel)
    constexpr uint32_t PACKET_SIZE = 2048;
    constexpr uint32_t BITS_PER_PACKET = PACKET_SIZE * 8;
    constexpr uint32_t HEADER_BITS = 32;
    constexpr uint32_t PAYLOAD_BITS = BITS_PER_PACKET - HEADER_BITS;
    constexpr uint32_t LENGTH_BITS = 15;             // frame length prefix
    constexpr uint32_t SAMPLES_PER_FRAME = 512;
    constexpr uint32_t BLOCKS_PER_FRAME_CHANNEL = SAMPLES_PER_FRAME * 2 / OUTPUT_BLOCK_SIZE;  // 4
    constexpr auto TICK = std::chrono::milliseconds(5);

    struct HostState
    {
        bool allocated = false;
        AVCodecContext* decoder = nullptr;
        int decoderRate = 0, decoderChannels = 0;
        // The current frame's PCM (BE s16, interleaved) and the blocks of it
        // not yet in the output ring (Xenia's current_frame_remaining_subframes_).
        std::vector<uint8_t> frame;
        uint32_t frameBlocks = 0, frameBlocksLeft = 0;
        uint32_t frameLimitBlocks = 0;  // loop end: blocks of this frame to output (0 = all)
        bool loopStartSkipPending = false;
        // A starved pass faked a full ring: the real write offset, and the
        // read offset it moved the write offset to.
        bool starvedFake = false;
        uint32_t starvedRealWrite = 0, starvedRead = 0;
        uint64_t frames = 0;
    };

    // Xenia's XMA_CONTEXT_DATA / packet helpers.
    uint32_t FrameOffsetField(const uint8_t* packet)  // bits of the previous frame at the payload start
    {
        return (uint32_t(packet[0] & 3) << 13) | (uint32_t(packet[1]) << 5) | (packet[2] >> 3);
    }

    uint32_t SkipCount(const uint8_t* packet) { return packet[3]; }

    uint32_t Bit(const uint8_t* data, uint32_t pos) { return (data[pos >> 3] >> (7 - (pos & 7))) & 1u; }

    void PutBit(uint8_t* data, uint32_t pos, uint32_t v)
    {
        uint8_t mask = uint8_t(0x80 >> (pos & 7));
        data[pos >> 3] = uint8_t(v ? data[pos >> 3] | mask : data[pos >> 3] & ~mask);
    }

    const AVCodec* s_codec = nullptr;
    AVPacket* s_packet = nullptr;
    AVFrame* s_frame = nullptr;
    const bool s_silent = [] { const char* v = std::getenv("NFSMW_XMA_SILENT"); return v && v[0] == '1'; }();

    std::mutex s_mutex;  // serialises the API and the decoder, like register access

    // NFSMW_XMA_STATS=1: every 2 s, how the decode went (under s_mutex).
    struct XmaStats
    {
        uint64_t frames = 0, silentFrames = 0, loops = 0, ringFull = 0, starved = 0, unstarved = 0, stalls = 0, skipped = 0;
        // API calls in the window
        uint32_t creates = 0, releases = 0, inits = 0, enables = 0, disables = 0, inputValid = 0, outputValid = 0,
            blockWhileInUse = 0, seeks = 0, loopData = 0;
    };
    XmaStats s_stats;
    uint32_t s_contextArray = 0;  // guest address of the 320-context array
    HostState s_state[CONTEXT_COUNT];
    std::atomic<bool> s_logStarted{ false };

    // Host pointer of a physical address (0..512 MB).
    uint8_t* Physical(uint32_t physicalAddress)
    {
        return static_cast<uint8_t*>(g_memory.Translate(vmem::Physical().Base() | (physicalAddress & 0x1FFFFFFF)));
    }

    // A guest buffer pointer as the context holds it (physical, as Xenia's
    // GetPhysicalAddress: the 0xE0000000 window is offset by 4 KB).
    uint32_t ToPhysical(uint32_t guest)
    {
        return guest ? MmGetPhysicalAddress(guest) : 0;
    }

    int ContextIndex(const void* guestContext)
    {
        uint32_t addr = g_memory.MapVirtual(guestContext);
        if (s_contextArray == 0 || addr < s_contextArray || (addr - s_contextArray) % 64 != 0)
            return -1;
        uint32_t index = (addr - s_contextArray) / 64;
        return index < CONTEXT_COUNT ? int(index) : -1;
    }

    int SampleRate(uint32_t id)
    {
        static const int rates[4] = { 24000, 32000, 44100, 48000 };
        return rates[id & 3];
    }

    void ResetFrame(HostState& st)
    {
        st.frameBlocks = st.frameBlocksLeft = st.frameLimitBlocks = 0;
        st.loopStartSkipPending = false;
        st.starvedFake = false;
    }

    void FreeDecoder(HostState& st)
    {
        if (st.decoder)
            avcodec_free_context(&st.decoder);
        st.decoderRate = st.decoderChannels = 0;
        ResetFrame(st);
    }

    // A decoder for one XMA stream of `channels` (1 or 2) at `rate`. A new
    // (or flushed) decoder drops its first frame: that frame comes out as silence.
    bool EnsureDecoder(HostState& st, int rate, int channels)
    {
        if (st.decoder && st.decoderRate == rate && st.decoderChannels == channels)
            return true;
        if (st.decoder)
            avcodec_free_context(&st.decoder);
        st.decoderRate = st.decoderChannels = 0;
        if (!s_codec)
        {
            // XMA1 (packet header: 4-bit sequence number) or XMA2 (6-bit
            // frame count); the frame offset and skip fields are in the same
            // place, and the synthetic packets below work for both. NFSMW
            // (2005) uses XMA1; NFSMW_XMA=2 switches.
            const char* v = std::getenv("NFSMW_XMA");
            s_codec = avcodec_find_decoder(v && v[0] == '2' ? AV_CODEC_ID_XMA2 : AV_CODEC_ID_XMA1);
            s_packet = av_packet_alloc();
            s_frame = av_frame_alloc();
            if (!s_codec)
            {
                fprintf(stderr, "[xma] FFmpeg has no XMA decoder\n");
                return false;
            }
        }
        st.decoder = avcodec_alloc_context3(s_codec);
        st.decoder->sample_rate = rate;
        av_channel_layout_default(&st.decoder->ch_layout, channels);
        st.decoder->block_align = PACKET_SIZE;
        // Output every decoded sample, as the hardware does.
        st.decoder->flags2 |= AV_CODEC_FLAG2_SKIP_MANUAL;
        if (s_codec->id == AV_CODEC_ID_XMA2)
        {
            // XMA2WAVEFORMATEX extension: NumStreams = 1, ChannelMask 0.
            st.decoder->extradata = static_cast<uint8_t*>(av_mallocz(34 + AV_INPUT_BUFFER_PADDING_SIZE));
            st.decoder->extradata_size = 34;
            st.decoder->extradata[0] = 1;
        }
        else
        {
            // XMAWAVEFORMAT: 8-byte header (NumStreams at 4) + one 20-byte
            // stream entry (Channels at 17).
            st.decoder->extradata = static_cast<uint8_t*>(av_mallocz(28 + AV_INPUT_BUFFER_PADDING_SIZE));
            st.decoder->extradata_size = 28;
            st.decoder->extradata[4] = 1;
            st.decoder->extradata[8 + 17] = uint8_t(channels);
        }
        if (avcodec_open2(st.decoder, s_codec, nullptr) < 0)
        {
            fprintf(stderr, "[xma] avcodec_open2 failed (%d Hz, %d ch)\n", rate, channels);
            avcodec_free_context(&st.decoder);
            return false;
        }
        st.decoderRate = rate;
        st.decoderChannels = channels;
        return true;
    }

    int16_t ToS16(float v)
    {
        v = std::clamp(v, -1.0f, 1.0f);
        return int16_t(std::lrintf(v * 32767.0f));
    }

    // --- Input buffers -------------------------------------------------------

    bool BufferValid(const XmaContextData& c, uint32_t buffer)
    {
        return buffer ? c.input_buffer_1_valid : c.input_buffer_0_valid;
    }

    uint32_t BufferPtr(const XmaContextData& c, uint32_t buffer)
    {
        return buffer ? c.input_buffer_1_ptr : c.input_buffer_0_ptr;
    }

    uint32_t BufferPackets(const XmaContextData& c, uint32_t buffer)
    {
        return buffer ? c.input_buffer_1_packet_count : c.input_buffer_0_packet_count;
    }

    // Xenia's SwapInputBuffer: the current buffer is done.
    void SwapInputBuffer(XmaContextData& c)
    {
        if (c.current_buffer)
            c.input_buffer_1_valid = 0;
        else
            c.input_buffer_0_valid = 0;
        c.current_buffer ^= 1;
        c.input_buffer_read_offset = HEADER_BITS;
    }

    // Packet `index` counted from the current buffer's start, continuing into
    // the other buffer when past its end. nullptr if it isn't there; then
    // `pending` says whether it can still come (the other buffer isn't valid
    // yet) or never will (past the end of both buffers).
    const uint8_t* StreamPacket(const XmaContextData& c, uint32_t index, bool& pending)
    {
        uint32_t buffer = c.current_buffer;
        uint32_t count = BufferPackets(c, buffer);
        pending = false;
        if (index >= count)
        {
            index -= count;
            buffer ^= 1;
            if (!BufferValid(c, buffer))
            {
                pending = true;
                return nullptr;
            }
            if (index >= BufferPackets(c, buffer))
                return nullptr;
        }
        uint32_t ptr = BufferPtr(c, buffer);
        return ptr ? Physical(ptr + index * PACKET_SIZE) : nullptr;
    }

    // Move to the first frame starting at or after packet `index` of the
    // stream (following the skip counts of packets that only continue a
    // frame), swapping to the other buffer when this one ends.
    void GoToNextPacket(XmaContextData& c, uint32_t index)
    {
        uint32_t count = BufferPackets(c, c.current_buffer);
        while (index < count)
        {
            const uint8_t* packet = Physical(BufferPtr(c, c.current_buffer) + index * PACKET_SIZE);
            uint32_t field = FrameOffsetField(packet);
            if (field < PAYLOAD_BITS)
            {
                c.input_buffer_read_offset = index * BITS_PER_PACKET + HEADER_BITS + field;
                return;
            }
            uint32_t skip = SkipCount(packet);
            if (skip == 0xFF)
                skip = 0;
            index += skip + 1;
        }
        // Past this buffer: continue in the other one at the same stream
        // position (the skip count carries across).
        uint32_t carry = index - count;
        SwapInputBuffer(c);
        c.input_buffer_read_offset = carry * BITS_PER_PACKET + HEADER_BITS;  // snapped to its first frame when decoded
    }

    // Copies `bits` of the stream from bit `offset` of packet `index` into
    // `out` (payloads only, following skip counts). Returns the bits copied:
    // fewer when a packet isn't there (`pending` as StreamPacket's).
    uint32_t GatherBits(const XmaContextData& c, uint32_t index, uint32_t offset, uint32_t bits, uint8_t* out, bool& pending)
    {
        uint32_t copied = 0;
        pending = false;
        while (copied < bits)
        {
            const uint8_t* packet = StreamPacket(c, index, pending);
            if (!packet)
                break;
            for (; offset < BITS_PER_PACKET && copied < bits; offset++, copied++)
                PutBit(out, copied, Bit(packet, offset));
            uint32_t skip = SkipCount(packet);
            index += (skip == 0xFF ? 0 : skip) + 1;
            offset = HEADER_BITS;
        }
        return copied;
    }

    // --- Frames --------------------------------------------------------------

    // Decode the frame whose bits are in `frameBits` (length `length`) into
    // st.frame. Frames FFmpeg doesn't output (the first after a fresh start,
    // or a corrupt one) are silence.
    void DecodeFrameBits(HostState& st, const uint8_t* frameBits, uint32_t length, int channels)
    {
        st.frame.assign(size_t(SAMPLES_PER_FRAME) * channels * 2, 0);
        st.frames++;
        s_stats.frames++;
        bool got = false;
        if (!s_silent && st.decoder)
        {
            // One XMA packet holding only this frame: first frame at bit 0 of
            // the payload, its "another frame follows" bit cleared. A frame
            // longer than a payload continues in a second packet, whose
            // header gives the bits that belong to it. (Beyond two payloads,
            // 32704 bits, FFmpeg keeps only what fits: the frame loses its
            // last bits. NFSMW's longest frame is 6155 bits.)
            static uint8_t packets[2][PACKET_SIZE + AV_INPUT_BUFFER_PADDING_SIZE];
            uint32_t first = std::min(length, PAYLOAD_BITS);
            memset(packets[0], 0, PACKET_SIZE);
            for (uint32_t i = 0; i < first; i++)
                PutBit(packets[0], HEADER_BITS + i, Bit(frameBits, i));
            int count = 1;
            if (length > PAYLOAD_BITS)
            {
                uint32_t rest = length - PAYLOAD_BITS;
                memset(packets[1], 0, PACKET_SIZE);
                packets[1][0] = uint8_t(rest >> 13);
                packets[1][1] = uint8_t(rest >> 5);
                packets[1][2] = uint8_t(rest << 3);
                for (uint32_t i = 0; i < rest; i++)
                    PutBit(packets[1], HEADER_BITS + i, Bit(frameBits, PAYLOAD_BITS + i));
                count = 2;
            }
            uint32_t trailer = length - 1;  // the frame's last bit
            PutBit(trailer < PAYLOAD_BITS ? packets[0] : packets[1], HEADER_BITS + (trailer < PAYLOAD_BITS ? trailer : trailer - PAYLOAD_BITS), 0);
            for (int p = 0; p < count; p++)
            {
                av_packet_unref(s_packet);
                s_packet->data = packets[p];
                s_packet->size = PACKET_SIZE;
                if (avcodec_send_packet(st.decoder, s_packet) != 0)
                    continue;
                while (avcodec_receive_frame(st.decoder, s_frame) == 0)
                {
                    int n = std::min<int>(s_frame->nb_samples, SAMPLES_PER_FRAME);
                    int ch = std::min(channels, s_frame->ch_layout.nb_channels);
                    for (int i = 0; i < n; i++)
                        for (int c = 0; c < channels; c++)
                        {
                            const float* plane = reinterpret_cast<const float*>(s_frame->extended_data[std::min(c, ch - 1)]);
                            int16_t v = ToS16(plane[i]);
                            size_t at = (size_t(i) * channels + c) * 2;
                            st.frame[at] = uint8_t(uint16_t(v) >> 8);
                            st.frame[at + 1] = uint8_t(v);
                        }
                    got = n > 0;
                    av_frame_unref(s_frame);
                }
            }
        }
        if (!got)
            s_stats.silentFrames++;
        st.frameBlocks = st.frameBlocksLeft = BLOCKS_PER_FRAME_CHANNEL * channels;
    }

    // Xenia's Decode(): the next frame of the stream into st.frame, advancing
    // the read offset. False if nothing could be decoded (no input, or the
    // rest of a split frame hasn't arrived).
    bool DecodeFrame(HostState& st, XmaContextData& c, int channels)
    {
        bool loopEndFrame = false;
        for (int guard = 0; guard < 64; guard++)
        {
            if (!c.input_buffer_0_valid && !c.input_buffer_1_valid)
                return false;
            if (!BufferValid(c, c.current_buffer))
            {
                SwapInputBuffer(c);
                if (!BufferValid(c, c.current_buffer))
                    return false;
            }
            uint32_t count = BufferPackets(c, c.current_buffer);
            if (count == 0 || BufferPtr(c, c.current_buffer) == 0)
            {
                SwapInputBuffer(c);
                continue;
            }
            if (c.input_buffer_read_offset < HEADER_BITS)
                c.input_buffer_read_offset = HEADER_BITS;
            uint32_t index = c.input_buffer_read_offset / BITS_PER_PACKET;
            if (index >= count)
            {
                GoToNextPacket(c, index);
                continue;
            }

            const uint8_t* packet = Physical(BufferPtr(c, c.current_buffer) + index * PACKET_SIZE);
            uint32_t skip = SkipCount(packet);
            uint32_t nextIndex = index + (skip == 0xFF ? 0 : skip) + 1;
            uint32_t field = FrameOffsetField(packet);
            if (field >= PAYLOAD_BITS)
            {
                GoToNextPacket(c, nextIndex);  // no frame starts in this packet
                continue;
            }
            uint32_t offset = c.input_buffer_read_offset % BITS_PER_PACKET;
            if (offset < HEADER_BITS + field)
            {
                // In the tail of a frame from the previous packet: go to the
                // first frame that starts here.
                offset = HEADER_BITS + field;
                c.input_buffer_read_offset = index * BITS_PER_PACKET + offset;
            }

            // Hardware loop (Xenia's UpdateLoopStatus): at loop_end, back to
            // loop_start; that frame's output is limited to subframes
            // 0..loop_subframe_end, and loop_subframe_skip leading subframes
            // of it are dropped. (Checked at a frame start, as Xenia's read
            // offsets always are.)
            if (c.loop_count > 0 && !loopEndFrame &&
                c.input_buffer_read_offset == std::max(HEADER_BITS, uint32_t(c.loop_end)))
            {
                loopEndFrame = true;
                c.input_buffer_read_offset = std::max(HEADER_BITS, uint32_t(c.loop_start));
                st.loopStartSkipPending = true;
                if (c.loop_count != 255)
                    c.loop_count--;
                s_stats.loops++;
                continue;
            }

            uint8_t lengthBits[4] = {};
            bool pending = false;
            if (GatherBits(c, index, offset, LENGTH_BITS, lengthBits, pending) < LENGTH_BITS)
            {
                if (pending)
                {
                    // The length runs into the other buffer, not valid yet:
                    // wait for it, as for a split frame body.
                    s_stats.stalls++;
                    return false;
                }
                SwapInputBuffer(c);  // nothing more can follow: this buffer is done
                continue;
            }
            uint32_t length = (uint32_t(lengthBits[0]) << 7) | (lengthBits[1] >> 1);
            if (length <= LENGTH_BITS || length == 0x7FFF)
            {
                GoToNextPacket(c, nextIndex);  // end of the frames in this packet
                continue;
            }
            static uint8_t frameBits[(0x7FFF + 7) / 8 + 8];
            if (GatherBits(c, index, offset, length, frameBits, pending) < length)
            {
                if (pending)
                {
                    s_stats.stalls++;
                    return false;  // the rest of the frame comes with the other buffer
                }
                // It runs past data that can never come (corrupt, or with
                // 1-packet buffers a third packet): skip it.
                s_stats.skipped++;
                GoToNextPacket(c, nextIndex);
                continue;
            }
            uint32_t trailer = Bit(frameBits, length - 1);

            if (!s_silent)
                EnsureDecoder(st, SampleRate(c.sample_rate), channels);
            DecodeFrameBits(st, frameBits, length, channels);
            if (loopEndFrame)
                st.frameLimitBlocks = (uint32_t(c.loop_subframe_end) + 1) * channels;
            else
                st.frameLimitBlocks = 0;
            if (st.loopStartSkipPending)
            {
                uint32_t skipBlocks = uint32_t(c.loop_subframe_skip) * channels;
                if (skipBlocks < st.frameBlocksLeft)
                    st.frameBlocksLeft -= skipBlocks;
                st.loopStartSkipPending = false;
            }

            // Next frame: in this packet if the frame ends here and says
            // another follows, else the next packet of the stream.
            if (offset + length < BITS_PER_PACKET && trailer)
                c.input_buffer_read_offset = index * BITS_PER_PACKET + offset + length;
            else
                GoToNextPacket(c, nextIndex);
            return true;
        }
        return false;
    }

    // Xenia's StoreContextMerged: write back only what the decoder owns, so a
    // field the game wrote to the context meanwhile isn't lost.
    void StoreMerged(void* guest, const XmaContextData& data, const XmaContextData& initial)
    {
        XmaContextData fresh = XmaContextData::Load(guest);
        fresh.loop_count = data.loop_count;
        fresh.output_buffer_write_offset = data.output_buffer_write_offset;
        if (initial.input_buffer_0_valid && !data.input_buffer_0_valid)
            fresh.input_buffer_0_valid = 0;
        if (initial.input_buffer_1_valid && !data.input_buffer_1_valid)
            fresh.input_buffer_1_valid = 0;
        if (initial.output_buffer_valid && !data.output_buffer_valid)
            fresh.output_buffer_valid = 0;
        fresh.input_buffer_read_offset = data.input_buffer_read_offset;
        fresh.current_buffer = data.current_buffer;
        fresh.Store(guest);
    }

    // Xenia's Work(): the pass for an XMAEnableContext kick.
    void Service(uint32_t index)
    {
        HostState& st = s_state[index];
        void* guest = g_memory.Translate(s_contextArray + index * 64);
        XmaContextData c = XmaContextData::Load(guest);
        if (st.starvedFake && (c.input_buffer_0_valid || c.input_buffer_1_valid))
        {
            // New input after a starved pass faked a full ring. If the game
            // has read no further than the real output (it didn't use the
            // fake data), undo the fake: continue at the real write offset.
            const uint32_t n = c.output_buffer_block_count ? c.output_buffer_block_count : 32;
            uint32_t readSoFar = (c.output_buffer_read_offset + n - st.starvedRead) % n;
            uint32_t realSpan = (st.starvedRealWrite + n - st.starvedRead) % n;
            if (readSoFar <= realSpan)
            {
                c.output_buffer_write_offset = st.starvedRealWrite;
                c.output_buffer_valid = 1;
                c.Store(guest);
                s_stats.unstarved++;
            }
            st.starvedFake = false;
        }
        const XmaContextData initial = c;
        if (!c.output_buffer_valid || c.output_buffer_ptr == 0)
            return;

        const bool inputAtStart = c.input_buffer_0_valid || c.input_buffer_1_valid;
        const int channels = c.is_stereo ? 2 : 1;
        const uint32_t count = c.output_buffer_block_count ? c.output_buffer_block_count : 32;
        uint8_t* out = Physical(c.output_buffer_ptr);
        uint32_t w = c.output_buffer_write_offset % count, r = c.output_buffer_read_offset % count;
        uint32_t freeBlocks = w == r ? count : (r + count - w) % count;
        bool wrote = false;

        // Each frame yields at least one block unless a degenerate hardware
        // loop drops them all: bound the frames per pass.
        for (uint32_t frames = 0; frames < count + 8; frames++)
        {
            // 1. The current frame into the ring, block by block.
            while (st.frameBlocksLeft > 0 && freeBlocks > 0)
            {
                uint32_t done = st.frameBlocks - st.frameBlocksLeft;
                if (st.frameLimitBlocks && done >= st.frameLimitBlocks)
                {
                    st.frameBlocksLeft = 0;  // loop end: the rest of the frame isn't played
                    break;
                }
                memcpy(out + w * OUTPUT_BLOCK_SIZE, st.frame.data() + done * OUTPUT_BLOCK_SIZE, OUTPUT_BLOCK_SIZE);
                w = (w + 1) % count;
                freeBlocks--;
                st.frameBlocksLeft--;
                wrote = true;
            }
            if (st.frameBlocksLeft > 0 || freeBlocks == 0)
                break;  // ring full
            // 2. The next frame.
            if (!DecodeFrame(st, c, channels))
                break;
        }

        if (inputAtStart || wrote)
            c.output_buffer_write_offset = w;
        else if (c.output_buffer_write_offset != c.output_buffer_read_offset)
        {
            // Starved: NFSMW detects a stalled voice by write == read.
            if (!st.starvedFake)
            {
                st.starvedFake = true;
                st.starvedRealWrite = c.output_buffer_write_offset;
                st.starvedRead = c.output_buffer_read_offset;
            }
            c.output_buffer_write_offset = c.output_buffer_read_offset;
            c.output_buffer_valid = 0;
            s_stats.starved++;
        }
        if (wrote && freeBlocks == 0)
        {
            c.output_buffer_valid = 0;  // full: the game reads, then sets it valid again
            s_stats.ringFull++;
        }
        StoreMerged(guest, c, initial);
    }

    // NFSMW_XMA_LOG=1: every 2 s, the state of each allocated context.
    void LogContexts()
    {
        static const bool enabled = std::getenv("NFSMW_XMA_LOG") != nullptr;
        static auto next = std::chrono::steady_clock::now();
        if (!enabled || std::chrono::steady_clock::now() < next)
            return;
        next += std::chrono::seconds(2);
        for (uint32_t i = 0; i < CONTEXT_COUNT; i++)
        {
            const HostState& st = s_state[i];
            if (!st.allocated)
                continue;
            XmaContextData c = XmaContextData::Load(g_memory.Translate(s_contextArray + i * 64));
            if (!c.input_buffer_0_valid && !c.input_buffer_1_valid && !c.output_buffer_valid)
                continue;
            fprintf(stderr, "[xma] ctx %3u in0 %u(%u pk) in1 %u(%u pk) cur %u rd %u | out %s w %u r %u /%u | %s %dHz loop %u [%u,%u) | frames %llu\n",
                i, uint32_t(c.input_buffer_0_valid), uint32_t(c.input_buffer_0_packet_count),
                uint32_t(c.input_buffer_1_valid), uint32_t(c.input_buffer_1_packet_count), uint32_t(c.current_buffer),
                uint32_t(c.input_buffer_read_offset), c.output_buffer_valid ? "ok" : "--", uint32_t(c.output_buffer_write_offset),
                uint32_t(c.output_buffer_read_offset), uint32_t(c.output_buffer_block_count), c.is_stereo ? "st" : "mo",
                SampleRate(c.sample_rate), uint32_t(c.loop_count), uint32_t(c.loop_start), uint32_t(c.loop_end),
                (unsigned long long)st.frames);
        }
    }

    void LogStats()
    {
        static const bool enabled = std::getenv("NFSMW_XMA_STATS") != nullptr;
        static auto next = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        if (!enabled || std::chrono::steady_clock::now() < next)
            return;
        next += std::chrono::seconds(2);
        uint32_t allocated = 0, withInput = 0, outValid = 0, outPending = 0;
        for (uint32_t i = 0; i < CONTEXT_COUNT; i++)
        {
            if (!s_state[i].allocated)
                continue;
            allocated++;
            XmaContextData c = XmaContextData::Load(g_memory.Translate(s_contextArray + i * 64));
            withInput += c.input_buffer_0_valid || c.input_buffer_1_valid;
            outValid += c.output_buffer_valid;
            outPending += c.output_buffer_write_offset != c.output_buffer_read_offset;
        }
        fprintf(stderr, "[xma] stats: %llu frames (%llu silent, %llu skipped), %llu loops, %llu ring-full, %llu starved (%llu undone), %llu split-frame waits | "
            "contexts: %u allocated, %u with input, %u output valid, %u unread | calls: %u create %u release %u init %u enable %u disable "
            "%u input-valid %u output-valid %u block %u seek %u loop\n",
            (unsigned long long)s_stats.frames, (unsigned long long)s_stats.silentFrames, (unsigned long long)s_stats.skipped,
            (unsigned long long)s_stats.loops, (unsigned long long)s_stats.ringFull, (unsigned long long)s_stats.starved,
            (unsigned long long)s_stats.unstarved, (unsigned long long)s_stats.stalls,
            allocated, withInput, outValid, outPending, s_stats.creates, s_stats.releases, s_stats.inits, s_stats.enables,
            s_stats.disables, s_stats.inputValid, s_stats.outputValid, s_stats.blockWhileInUse, s_stats.seeks, s_stats.loopData);
        s_stats = {};
    }

    // NFSMW_XMA_LOG / NFSMW_XMA_STATS: the periodic logs. (Decoding needs no
    // thread: each kick is serviced in XMAEnableContext.)
    void LogThread()
    {
        while (true)
        {
            {
                std::lock_guard lock(s_mutex);
                LogContexts();
                LogStats();
            }
            std::this_thread::sleep_for(TICK);
        }
    }

    // NFSMW_XMA_LOG=1: API calls on contexts 0-3 (NFSMW_XMA_LOG_CTX=<n>: context n, "st": stereo ones).
    void LogCall(const char* name, void* context, uint32_t a = 0, uint32_t b = 0)
    {
        static const bool enabled = std::getenv("NFSMW_XMA_LOG") != nullptr;
        if (!enabled)
            return;
        int i = ContextIndex(context);
        static const char* filter = std::getenv("NFSMW_XMA_LOG_CTX");
        if (i < 0)
            return;
        XmaContextData c = XmaContextData::Load(context);
        if (filter && filter[0] == 's' ? !c.is_stereo : filter ? i != std::atoi(filter) : i > 3)
            return;
        fprintf(stderr, "[xma] t%u call %s ctx %d (%08X, %08X) | in0 %u in1 %u cur %u rd %u out %u w %u r %u\n",
            GuestThread::GetCurrentThreadId(), name, i, a, b,
            uint32_t(c.input_buffer_0_valid), uint32_t(c.input_buffer_1_valid), uint32_t(c.current_buffer),
            uint32_t(c.input_buffer_read_offset), uint32_t(c.output_buffer_valid), uint32_t(c.output_buffer_write_offset),
            uint32_t(c.output_buffer_read_offset));
    }

    // NFSMW_XMA_STATS=1: the first loops set up, to check how the game uses
    // loop_end (whether it is the offset of a frame of the stream).
    void LogLoop(const char* how, const XmaContextData& c)
    {
        static const bool enabled = std::getenv("NFSMW_XMA_STATS") != nullptr;
        static int logged = 0;
        if (!enabled || c.loop_count == 0 || logged >= 32)
            return;
        logged++;
        fprintf(stderr, "[xma] loop via %s: count %u start %u end %u subframe end %u skip %u | in0 %u pk, read %u\n", how,
            uint32_t(c.loop_count), uint32_t(c.loop_start), uint32_t(c.loop_end), uint32_t(c.loop_subframe_end),
            uint32_t(c.loop_subframe_skip), uint32_t(c.input_buffer_0_packet_count), uint32_t(c.input_buffer_read_offset));
    }

    template<typename F>
    uint32_t WithContext(void* context, F&& f)
    {
        std::lock_guard lock(s_mutex);
        XmaContextData c = XmaContextData::Load(context);
        uint32_t result = f(c);
        c.Store(context);
        return result;
    }
}

uint32_t XMACreateContext(be<uint32_t>* contextOut)
{
    std::lock_guard lock(s_mutex);
    s_stats.creates++;
    if (s_contextArray == 0)
    {
        s_contextArray = vmem::Physical().Alloc(CONTEXT_COUNT * 64, 0x1000,
            vmem::X_MEM_RESERVE | vmem::X_MEM_COMMIT, 0x04, true);
        if (s_contextArray == 0)
            return STATUS_NO_MEMORY;
    }
    for (uint32_t i = 0; i < CONTEXT_COUNT; i++)
    {
        if (!s_state[i].allocated)
        {
            FreeDecoder(s_state[i]);
            s_state[i].allocated = true;
            memset(g_memory.Translate(s_contextArray + i * 64), 0, 64);
            *contextOut = s_contextArray + i * 64;
            if ((std::getenv("NFSMW_XMA_LOG") || std::getenv("NFSMW_XMA_STATS")) && !s_logStarted.exchange(true))
                std::thread(LogThread).detach();
            return STATUS_SUCCESS;
        }
    }
    *contextOut = 0;
    return STATUS_NO_MEMORY;
}

uint32_t XMAReleaseContext(void* context)
{
    std::lock_guard lock(s_mutex);
    s_stats.releases++;
    int i = ContextIndex(context);
    if (i >= 0)
    {
        FreeDecoder(s_state[i]);
        s_state[i].allocated = false;
        memset(context, 0, 64);  // as Xenia's Release
    }
    return 0;
}

uint32_t XMAInitializeContext(void* context, XmaContextInit* init)
{
    std::lock_guard lock(s_mutex);
    s_stats.inits++;
    LogCall("XMAInitializeContext", context, uint32_t(init->inputBuffer0PacketCount), uint32_t(init->outputBufferBlockCount));
    XmaContextData c{};
    c.input_buffer_0_ptr = ToPhysical(init->inputBuffer0Ptr);
    c.input_buffer_0_packet_count = init->inputBuffer0PacketCount;
    c.input_buffer_1_ptr = ToPhysical(init->inputBuffer1Ptr);
    c.input_buffer_1_packet_count = init->inputBuffer1PacketCount;
    c.input_buffer_read_offset = init->inputBufferReadOffset;
    c.output_buffer_ptr = ToPhysical(init->outputBufferPtr);
    c.output_buffer_block_count = init->outputBufferBlockCount;
    c.subframe_decode_count = init->subframeDecodeCount;
    c.is_stereo = init->channelCount >= 1;  // Xenia: channel_count is 0 for mono
    c.sample_rate = init->sampleRate;
    c.loop_start = init->loopData.loopStart;
    c.loop_end = init->loopData.loopEnd;
    c.loop_count = init->loopData.loopCount;
    c.loop_subframe_end = init->loopData.loopSubframeEnd;
    c.loop_subframe_skip = init->loopData.loopSubframeSkip;
    c.Store(context);
    LogLoop("init", c);
    if (int i = ContextIndex(context); i >= 0)
    {
        // A new stream: no overlap from the last one, no undelivered frame.
        HostState& st = s_state[i];
        ResetFrame(st);
        if (st.decoder)
            avcodec_flush_buffers(st.decoder);
    }
    return 0;
}

uint32_t XMASetLoopData(void* context, XmaLoopData* loop)
{
    return WithContext(context, [&](XmaContextData& c) {
        s_stats.loopData++;
        c.loop_start = loop->loopStart;
        c.loop_end = loop->loopEnd;
        c.loop_count = loop->loopCount;
        c.loop_subframe_end = loop->loopSubframeEnd;
        c.loop_subframe_skip = loop->loopSubframeSkip;
        LogLoop("set", c);
        return 0u;
    });
}

uint32_t XMASetInputBufferReadOffset(void* context, uint32_t value)
{
    { std::lock_guard lock(s_mutex); LogCall("XMASetInputBufferReadOffset", context, value); }
    return WithContext(context, [&](XmaContextData& c) { s_stats.seeks++; c.input_buffer_read_offset = value; return 0u; });
}

uint32_t XMASetInputBuffer0(void* context, uint32_t buffer, uint32_t packetCount)
{
    { std::lock_guard lock(s_mutex); LogCall("XMASetInputBuffer0", context, buffer, packetCount); }
    return WithContext(context, [&](XmaContextData& c) {
        c.input_buffer_0_ptr = ToPhysical(buffer);
        c.input_buffer_0_packet_count = packetCount;
        return 0u;
    });
}

uint32_t XMASetInputBuffer1(void* context, uint32_t buffer, uint32_t packetCount)
{
    { std::lock_guard lock(s_mutex); LogCall("XMASetInputBuffer1", context, buffer, packetCount); }
    return WithContext(context, [&](XmaContextData& c) {
        c.input_buffer_1_ptr = ToPhysical(buffer);
        c.input_buffer_1_packet_count = packetCount;
        return 0u;
    });
}

uint32_t XMAIsInputBuffer0Valid(void* context)
{
    return WithContext(context, [](XmaContextData& c) { return uint32_t(c.input_buffer_0_valid); });
}

uint32_t XMAIsInputBuffer1Valid(void* context)
{
    return WithContext(context, [](XmaContextData& c) { return uint32_t(c.input_buffer_1_valid); });
}

uint32_t XMASetInputBuffer0Valid(void* context)
{
    { std::lock_guard lock(s_mutex); LogCall("XMASetInputBuffer0Valid", context); }
    return WithContext(context, [](XmaContextData& c) { s_stats.inputValid++; c.input_buffer_0_valid = 1; return 0u; });
}

uint32_t XMASetInputBuffer1Valid(void* context)
{
    { std::lock_guard lock(s_mutex); LogCall("XMASetInputBuffer1Valid", context); }
    return WithContext(context, [](XmaContextData& c) { s_stats.inputValid++; c.input_buffer_1_valid = 1; return 0u; });
}

uint32_t XMAIsOutputBufferValid(void* context)
{
    return WithContext(context, [](XmaContextData& c) { return uint32_t(c.output_buffer_valid); });
}

uint32_t XMASetOutputBufferValid(void* context)
{
    { std::lock_guard lock(s_mutex); LogCall("XMASetOutputBufferValid", context); }
    return WithContext(context, [](XmaContextData& c) { s_stats.outputValid++; c.output_buffer_valid = 1; return 0u; });
}

uint32_t XMAGetOutputBufferReadOffset(void* context)
{
    return WithContext(context, [](XmaContextData& c) { return uint32_t(c.output_buffer_read_offset); });
}

uint32_t XMASetOutputBufferReadOffset(void* context, uint32_t value)
{
    { std::lock_guard lock(s_mutex); LogCall("XMASetOutputBufferReadOffset", context, value); }
    return WithContext(context, [&](XmaContextData& c) { c.output_buffer_read_offset = value; return 0u; });
}

uint32_t XMAGetOutputBufferWriteOffset(void* context)
{
    // NFSMW_XMA_LOG: report contexts being polled hard (a player spinning).
    static const bool logPolls = std::getenv("NFSMW_XMA_LOG") != nullptr;
    if (logPolls)
    {
        static uint32_t polls[CONTEXT_COUNT];
        static auto windowStart = std::chrono::steady_clock::now();
        int i = ContextIndex(context);
        if (i >= 0)
            polls[i]++;
        auto now = std::chrono::steady_clock::now();
        if (now - windowStart > std::chrono::seconds(2))
        {
            for (uint32_t k = 0; k < CONTEXT_COUNT; k++)
                if (polls[k] > 20000)
                    fprintf(stderr, "[xma] hot poll: ctx %u polled %u times in 2 s\n", k, polls[k]);
            memset(polls, 0, sizeof(polls));
            windowStart = now;
        }
    }
    return WithContext(context, [](XmaContextData& c) { return uint32_t(c.output_buffer_write_offset); });
}

uint32_t XMAEnableContext(void* context)
{
    std::lock_guard lock(s_mutex);
    LogCall("XMAEnableContext", context);
    s_stats.enables++;
    // The hardware starts on the kick; the game's players poll for the result
    // right away, so the pass runs now.
    if (int i = ContextIndex(context); i >= 0)
        Service(uint32_t(i));
    return 0;
}

// Passes run inside XMAEnableContext, so there is nothing to stop; holding
// the decoder lock waits out a pass in progress, which is what `wait` asks.
uint32_t XMADisableContext(void* context, uint32_t wait)
{
    std::lock_guard lock(s_mutex);
    LogCall("XMADisableContext", context, wait);
    s_stats.disables++;
    return 0;
}

// "In use" means the decoder is working on the context. Our passes run under
// s_mutex, so acquiring it is enough (Xenia's version returns at once:
// work_buffer_ptr is never set). Waiting for the input buffers to drain, as
// before, hung forever: a disabled context is never decoded (XAudio disables,
// then blocks, when it requeues or seeks a playing voice).
uint32_t XMABlockWhileInUse(void* context)
{
    std::lock_guard lock(s_mutex);
    s_stats.blockWhileInUse++;
    LogCall("XMABlockWhileInUse", context);
    return 0;
}

GUEST_FUNCTION_HOOK(__imp__XMACreateContext, XMACreateContext);
GUEST_FUNCTION_HOOK(__imp__XMAReleaseContext, XMAReleaseContext);
GUEST_FUNCTION_HOOK(__imp__XMAInitializeContext, XMAInitializeContext);
GUEST_FUNCTION_HOOK(__imp__XMASetLoopData, XMASetLoopData);
GUEST_FUNCTION_HOOK(__imp__XMASetInputBufferReadOffset, XMASetInputBufferReadOffset);
GUEST_FUNCTION_HOOK(__imp__XMASetInputBuffer0, XMASetInputBuffer0);
GUEST_FUNCTION_HOOK(__imp__XMASetInputBuffer1, XMASetInputBuffer1);
GUEST_FUNCTION_HOOK(__imp__XMAIsInputBuffer0Valid, XMAIsInputBuffer0Valid);
GUEST_FUNCTION_HOOK(__imp__XMAIsInputBuffer1Valid, XMAIsInputBuffer1Valid);
GUEST_FUNCTION_HOOK(__imp__XMASetInputBuffer0Valid, XMASetInputBuffer0Valid);
GUEST_FUNCTION_HOOK(__imp__XMASetInputBuffer1Valid, XMASetInputBuffer1Valid);
GUEST_FUNCTION_HOOK(__imp__XMAIsOutputBufferValid, XMAIsOutputBufferValid);
GUEST_FUNCTION_HOOK(__imp__XMASetOutputBufferValid, XMASetOutputBufferValid);
GUEST_FUNCTION_HOOK(__imp__XMAGetOutputBufferReadOffset, XMAGetOutputBufferReadOffset);
GUEST_FUNCTION_HOOK(__imp__XMASetOutputBufferReadOffset, XMASetOutputBufferReadOffset);
GUEST_FUNCTION_HOOK(__imp__XMAGetOutputBufferWriteOffset, XMAGetOutputBufferWriteOffset);
GUEST_FUNCTION_HOOK(__imp__XMAEnableContext, XMAEnableContext);
GUEST_FUNCTION_HOOK(__imp__XMADisableContext, XMADisableContext);
GUEST_FUNCTION_HOOK(__imp__XMABlockWhileInUse, XMABlockWhileInUse);
