// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Xenos command processor (PM4). Phase 5 step 1: a NULL GPU. It executes the
// packets that synchronise the CPU and GPU (register writes, waits, fences,
// memory writes, interrupts, swaps, indirect buffers, constants) and counts
// but skips draws. Packet semantics follow Xenia's gpu/command_processor.cc
// (BSD); its null backend works the same way.
//
// The game's stores to GPU registers land in guest memory at 0x7FC80000 +
// index*4 (we have no MMIO traps), so the write pointer is polled there and
// the register file is mirrored back into that page for the game's reads.
#pragma once
#include <cstdint>
#include <string>

namespace gpu
{
    constexpr uint32_t REGISTER_COUNT = 0x5003;
    constexpr uint32_t MMIO_BASE = 0x7FC80000;

    void InitializeRingBuffer(uint32_t guestAddress, uint32_t sizeLog2);
    void EnableReadPointerWriteBack(uint32_t guestAddress, uint32_t blockSizeLog2);
    void SetInterruptCallback(uint32_t callback, uint32_t userData);

    // Frames completed (XE_SWAP packets executed).
    uint64_t FrameCount();

    // For the hang watchdog (report/watchdog.cpp), any thread: since when
    // (steady clock, ns) the command processor has been polling one
    // WAIT_REG_MEM past its first check; 0 when it isn't.
    int64_t WaitingSince();
    // For a hang report: that wait (with the value it reads now), frame and
    // ring positions, and the last 64 packets.
    std::string DescribeState();
}
