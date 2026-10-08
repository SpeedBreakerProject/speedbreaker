// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// xboxkrnl video exports (Vd*). Semantics follow Xenia's xboxkrnl_video.cc
// (BSD): mode queries report 1280x720 widescreen HD at 60 Hz; VdSwap writes
// the front buffer's texture fetch constant and an XE_SWAP packet into the
// 64-dword slot the game reserved in its ring buffer.
#include <stdafx.h>
#include <kernel/function.h>
#include <kernel/vmem.h>
#include <report/report.h>
#include "command_processor.h"

void VdQueryVideoMode(XVIDEO_MODE* mode)
{
    memset(mode, 0, sizeof(XVIDEO_MODE));
    mode->DisplayWidth = 1280;
    mode->DisplayHeight = 720;
    mode->IsInterlaced = 0;
    mode->IsWidescreen = 1;
    mode->IsHighDefinition = 1;
    mode->RefreshRate = 0x42700000;  // 60.0f
    mode->VideoStandard = 1;         // NTSC
    mode->Unknown4A = 0x4A;
    mode->Unknown01 = 0x01;
}

uint32_t VdQueryVideoFlags()
{
    return 1 | 2;  // widescreen, width >= 1024
}

void VdGetCurrentDisplayGamma(be<uint32_t>* type, be<float>* power)
{
    *type = 2;        // Xenia's default kernel_display_gamma_type
    *power = 2.22222233f;
}

void VdGetCurrentDisplayInformation(uint8_t* info)
{
    // X_DISPLAY_INFO (0x58 bytes), filled as Xenia does for 1280x720.
    memset(info, 0, 0x58);
    auto u16 = [&](uint32_t off, uint16_t v) { *reinterpret_cast<be<uint16_t>*>(info + off) = v; };
    auto u32 = [&](uint32_t off, uint32_t v) { *reinterpret_cast<be<uint32_t>*>(info + off) = v; };
    u16(0x00, 1280); u16(0x02, 720);         // front buffer
    u32(0x08 + 0x08, 1280); u32(0x08 + 0x0C, 720);  // scaler source rect x2, y2
    u32(0x08 + 0x10, 1280); u32(0x08 + 0x14, 720);  // scaled output
    u32(0x08 + 0x18, 1);                     // vertical filter type
    u32(0x08 + 0x28, 1);                     // horizontal filter type
    u16(0x40, 320); u16(0x42, 180); u16(0x44, 320); u16(0x46, 180);  // overscan
    u16(0x48, 1280); u16(0x4A, 720);
    u32(0x4C, 0x42700000);                   // 60.0f
    u16(0x56, 1280);
}

uint32_t VdSetDisplayMode(uint32_t flags) { return 0; }
uint32_t VdInitializeEngines(uint32_t unk0, uint32_t callback, uint32_t arg, uint32_t pfp, uint32_t me) { return 1; }
void VdShutdownEngines() {}
uint32_t VdGetGraphicsAsicID() { return 0x11; }
uint32_t VdEnableDisableClockGating(uint32_t enabled) { return 0; }
uint32_t VdIsHSIOTrainingSucceeded() { return 1; }
uint32_t VdRetrainEDRAMWorker(uint32_t unk0) { return 0; }
uint32_t VdRetrainEDRAM(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f) { return 0; }
uint32_t VdInitializeEDRAM() { return 0; }
void VdSetSystemCommandBufferGpuIdentifierAddress(uint32_t address) {}
uint32_t VdCallGraphicsNotificationRoutines(uint32_t unk0, void* args) { return 0; }

void VdSetGraphicsInterruptCallback(uint32_t callback, uint32_t userData)
{
    gpu::SetInterruptCallback(callback, userData);
}

void VdInitializeRingBuffer(uint32_t ptr, int32_t sizeLog2)
{
    gpu::InitializeRingBuffer(ptr, uint32_t(sizeLog2));
}

void VdEnableRingBufferRPtrWriteBack(uint32_t ptr, int32_t blockSizeLog2)
{
    gpu::EnableReadPointerWriteBack(ptr, uint32_t(blockSizeLog2));
}

void VdGetSystemCommandBuffer(be<uint32_t>* p0, be<uint32_t>* p1)
{
    memset(p0, 0, 0x94);
    *p0 = 0xBEEF0000;
    *p1 = 0xBEEF0001;
}

uint32_t VdQuerySystemCommandBuffer(be<uint32_t>* p0, be<uint32_t>* p1)
{
    VdGetSystemCommandBuffer(p0, p1);
    return 0;
}

uint32_t VdSetSystemCommandBuffer(uint32_t unk) { return 0; }

uint32_t VdInitializeScalerCommandBuffer(uint32_t sourceXY, uint32_t sourceWH, uint32_t outputXY, uint32_t outputWH,
    uint32_t frontBufferWH, uint32_t verticalFilter, void* verticalParams, uint32_t horizontalFilter,
    void* horizontalParams, void* unk9, be<uint32_t>* dest, uint32_t destCount)
{
    for (uint32_t i = 0; i < destCount; i++)
        dest[i] = 0x80000000;  // type-2 filler packets
    return destCount;
}

uint32_t VdPersistDisplay(uint32_t unk0, be<uint32_t>* out)
{
    if (out)
        *out = vmem::Physical().Alloc(64, 32, vmem::X_MEM_RESERVE | vmem::X_MEM_COMMIT, 0x01, false);
    return 1;
}

void VdSwap(be<uint32_t>* buffer, be<uint32_t>* fetch, void* unk2, void* unk3, void* unk4,
    be<uint32_t>* frontBuffer, be<uint32_t>* textureFormat, be<uint32_t>* colorSpace,
    be<uint32_t>* width, be<uint32_t>* height)
{
    // NFSMW_TEST_CRASH: a crash with the game's render loop on the stack.
    if (report::g_testCrash.load(std::memory_order_relaxed)) [[unlikely]]
        report::TestCrash();
    // The front buffer's texture fetch constant (6 dwords), with its base
    // address rewritten to a physical page number.
    uint32_t words[6];
    for (int i = 0; i < 6; i++)
        words[i] = fetch[i];
    uint32_t frontVirtual = (words[1] >> 12) << 12;  // dword_1: base_address in bits 12-31
    uint32_t frontPhysical = frontVirtual & 0x1FFFFFFF;
    words[1] = (words[1] & 0xFFF) | frontPhysical;

    memset(buffer, 0, 64 * 4);
    uint32_t o = 0;
    buffer[o++] = (0u << 30) | ((6 - 1) << 16) | 0x4800;  // type 0: SHADER_CONSTANT_FETCH_00_0, 6 regs
    for (int i = 0; i < 6; i++)
        buffer[o++] = words[i];
    buffer[o++] = (3u << 30) | ((4 - 1) << 16) | (0x64 << 8);  // type 3: XE_SWAP, 4 dwords
    buffer[o++] = 0x53574150;  // 'SWAP'
    buffer[o++] = frontPhysical;
    buffer[o++] = *width;
    buffer[o++] = *height;
    while (o < 64)
        buffer[o++] = 2u << 30;  // type-2 filler
}

GUEST_FUNCTION_HOOK(__imp__VdQueryVideoMode, VdQueryVideoMode);
GUEST_FUNCTION_HOOK(__imp__XGetVideoMode, VdQueryVideoMode);
GUEST_FUNCTION_HOOK(__imp__VdQueryVideoFlags, VdQueryVideoFlags);
GUEST_FUNCTION_HOOK(__imp__VdGetCurrentDisplayGamma, VdGetCurrentDisplayGamma);
GUEST_FUNCTION_HOOK(__imp__VdGetCurrentDisplayInformation, VdGetCurrentDisplayInformation);
GUEST_FUNCTION_HOOK(__imp__VdSetDisplayMode, VdSetDisplayMode);
GUEST_FUNCTION_HOOK(__imp__VdInitializeEngines, VdInitializeEngines);
GUEST_FUNCTION_HOOK(__imp__VdShutdownEngines, VdShutdownEngines);
GUEST_FUNCTION_HOOK(__imp__VdGetGraphicsAsicID, VdGetGraphicsAsicID);
GUEST_FUNCTION_HOOK(__imp__VdEnableDisableClockGating, VdEnableDisableClockGating);
GUEST_FUNCTION_HOOK(__imp__VdIsHSIOTrainingSucceeded, VdIsHSIOTrainingSucceeded);
GUEST_FUNCTION_HOOK(__imp__VdRetrainEDRAMWorker, VdRetrainEDRAMWorker);
GUEST_FUNCTION_HOOK(__imp__VdRetrainEDRAM, VdRetrainEDRAM);
GUEST_FUNCTION_HOOK(__imp__VdInitializeEDRAM, VdInitializeEDRAM);
GUEST_FUNCTION_HOOK(__imp__VdSetSystemCommandBufferGpuIdentifierAddress, VdSetSystemCommandBufferGpuIdentifierAddress);
GUEST_FUNCTION_HOOK(__imp__VdCallGraphicsNotificationRoutines, VdCallGraphicsNotificationRoutines);
GUEST_FUNCTION_HOOK(__imp__VdSetGraphicsInterruptCallback, VdSetGraphicsInterruptCallback);
GUEST_FUNCTION_HOOK(__imp__VdInitializeRingBuffer, VdInitializeRingBuffer);
GUEST_FUNCTION_HOOK(__imp__VdEnableRingBufferRPtrWriteBack, VdEnableRingBufferRPtrWriteBack);
GUEST_FUNCTION_HOOK(__imp__VdGetSystemCommandBuffer, VdGetSystemCommandBuffer);
GUEST_FUNCTION_HOOK(__imp__VdQuerySystemCommandBuffer, VdQuerySystemCommandBuffer);
GUEST_FUNCTION_HOOK(__imp__VdSetSystemCommandBuffer, VdSetSystemCommandBuffer);
GUEST_FUNCTION_HOOK(__imp__VdInitializeScalerCommandBuffer, VdInitializeScalerCommandBuffer);
GUEST_FUNCTION_HOOK(__imp__VdPersistDisplay, VdPersistDisplay);
GUEST_FUNCTION_HOOK(__imp__VdSwap, VdSwap);
