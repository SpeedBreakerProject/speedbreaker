// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
#include <stdafx.h>
#include "module.h"
#include "arena.h"
#include "function.h"
#include "dispatcher.h"

#include <cpu/guest_time.h>
#include <xex.h>

void RtlInitializeCriticalSectionAndSpinCount(XRTL_CRITICAL_SECTION* cs, uint32_t spinCount);

namespace module
{
    namespace
    {
        uint32_t s_ldr = 0;
        uint32_t s_header = 0;
        uint32_t s_timeStamp = 0;
        std::vector<std::pair<std::string, uint32_t>> s_pendingData;  // exports seen before Initialize

        // X_TIME_STAMP_BUNDLE, refreshed every millisecond like the kernel's.
        // In guest time: it stands still while the game is suspended, and
        // the thread parks meanwhile.
        void TimeStampThread()
        {
            using namespace std::chrono;
            int64_t start = guesttime::NowNs();
            while (true)
            {
                guesttime::WaitWhileSuspended();
                auto* b = static_cast<be<uint64_t>*>(g_memory.Translate(s_timeStamp));
                int64_t sinceNs = guesttime::NowNs() - start;
                b[0] = uint64_t(sinceNs / 100);                      // interrupt time
                b[1] = uint64_t(dispatcher::GuestSystemTime());     // system time
                reinterpret_cast<be<uint32_t>*>(b)[4] = uint32_t(sinceNs / 1'000'000);
                std::this_thread::sleep_for(milliseconds(1));
            }
        }
    }

    void Initialize(const std::vector<uint8_t>& file, uint32_t imageBase, uint32_t imageSize, uint32_t entryPoint)
    {
        auto* header = reinterpret_cast<const Xex2Header*>(file.data());
        uint32_t headerSize = header->headerSize;
        s_header = arena::Alloc(headerSize, 16);
        memcpy(g_memory.Translate(s_header), file.data(), headerSize);

        s_ldr = arena::Alloc(0x64, 16);
        auto* ldr = static_cast<uint8_t*>(g_memory.Translate(s_ldr));
        auto u32 = [&](uint32_t off, uint32_t v) { *reinterpret_cast<be<uint32_t>*>(ldr + off) = v; };
        // List links point at themselves (a one-module list).
        for (uint32_t off : { 0x0u, 0x8u, 0x10u })
        {
            u32(off, s_ldr + off);
            u32(off + 4, s_ldr + off);
        }
        u32(0x18, 0);            // dll_base: GetProcAddress reads this (Xenia leaves 0)
        u32(0x1C, imageBase);    // image_base
        u32(0x20, imageSize);    // image_size
        u32(0x38, imageSize);    // full_image_size
        u32(0x3C, entryPoint);
        u32(0x58, s_header);     // xex_header_base

        for (auto& [name, addr] : s_pendingData)
            InitializeDataExport(name.c_str(), addr);
        s_pendingData.clear();
    }

    uint32_t ExecutableHandle() { return s_ldr; }
    uint32_t XexHeader() { return s_header; }

    bool InitializeDataExport(const char* name, uint32_t address)
    {
        auto* p = static_cast<uint8_t*>(g_memory.Translate(address));
        auto u32 = [&](uint32_t off, uint32_t v) { *reinterpret_cast<be<uint32_t>*>(p + off) = v; };
        std::string_view n = name;
        if (n == "XexExecutableModuleHandle")
        {
            // A variable holding the module handle.
            if (s_ldr == 0) { s_pendingData.emplace_back(name, address); return true; }
            u32(0, s_ldr);
        }
        else if (n == "ExLoadedCommandLine")
        {
            static constexpr char cmd[] = "\"default.xex\"";
            memcpy(p, cmd, sizeof(cmd));
        }
        else if (n == "KeTimeStampBundle")
        {
            s_timeStamp = address;
            std::thread(TimeStampThread).detach();
        }
        else if (n == "VdGpuClockInMHz")
        {
            u32(0, 500);
        }
        else if (n == "VdHSIOCalibrationLock")
        {
            RtlInitializeCriticalSectionAndSpinCount(reinterpret_cast<XRTL_CRITICAL_SECTION*>(p), 10000);
        }
        else if (n == "VdGlobalDevice" || n == "VdGlobalXamDevice" || n == "KeDebugMonitorData" ||
            n == "KeCertMonitorData" || n == "ExEventObjectType" || n == "ExSemaphoreObjectType" ||
            n == "ExThreadObjectType" || n == "ExTimerObjectType")
        {
            // Zero-initialised, as in Xenia (device pointers 0; object type
            // descriptors only compared by address).
        }
        else
        {
            return false;
        }
        return true;
    }
}

// ---- Module exports ---------------------------------------------------------

uint32_t XexGetModuleHandle(const char* name, be<uint32_t>* handle)
{
    // Only the executable itself exists.
    if (name == nullptr || strcasecmp(name, "default.xex") == 0)
    {
        *handle = module::ExecutableHandle();
        return STATUS_SUCCESS;
    }
    fprintf(stderr, "[module] XexGetModuleHandle(\"%s\"): no such module\n", name);
    *handle = 0;
    return 0xC0000225; // STATUS_NOT_FOUND
}

// Returns the optional header value (low byte 0/1: stored inline) or a guest
// pointer to its data inside the header copy; 0 if absent.
uint32_t RtlImageXexHeaderField(uint8_t* header, uint32_t key)
{
    auto* h = reinterpret_cast<const Xex2Header*>(header);
    auto* opt = reinterpret_cast<const Xex2OptHeader*>(h + 1);
    for (uint32_t i = 0; i < h->headerCount; i++)
    {
        if (opt[i].key != key)
            continue;
        if ((key & 0xFF) == 0 || (key & 0xFF) == 1)
            return opt[i].value;
        return g_memory.MapVirtual(header) + uint32_t(opt[i].offset);
    }
    return 0;
}

GUEST_FUNCTION_HOOK(__imp__XexGetModuleHandle, XexGetModuleHandle);
GUEST_FUNCTION_HOOK(__imp__RtlImageXexHeaderField, RtlImageXexHeaderField);
