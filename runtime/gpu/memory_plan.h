// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Where the renderer (gpu/renderer.cpp) puts the two big allocations the CPU
// writes: guest physical memory as the GPU sees it (512 MB: imported without
// a copy, or a copy the write watch keeps current) and the upload ring
// (192 MB: draw constants, converted indices, texture staging). A plan is a
// list of places to try in turn; the renderer goes on to the next when the
// driver refuses one, so a refused allocation is never fatal. Vulkan-free
// (the flags are Vulkan's values): tests/memory_plan_test.cpp checks it alone
// against the memory of the devices we know.
//
// Chosen by capability, not by device type alone. "CPU-mapped VRAM" (a
// device-local, host-visible memory type) is all of VRAM with resizable BAR,
// but only a 256 MB window without it (NVIDIA before Ampere, and any GPU with
// resizable BAR off in the firmware). v0.1.0 put a discrete GPU's 512 MB copy
// there whatever its size, and the driver's refusal (out of device memory)
// aborted the game at startup (issue #2: a GTX 1080 Ti and an RTX 4070 on
// Linux). Now a place is planned first only when its heap holds what we put
// there with room to spare: at most half the heap (kHeapShareDivisor; the
// rest is for the driver, the game's textures and render targets, and other
// programs). Otherwise, on a discrete GPU: guest memory imported (the GPU
// reads system memory over PCIe without a copy: VK_EXT_external_memory_host,
// which NVIDIA's driver has), then a copy in system memory, uncached
// (write-combined) first; the ring in cached system memory, then uncached.
// Every list ends with the places that didn't fit, as a last try.
//
// Devices that fit keep exactly what v0.1.0 chose (the tests pin each one):
// the Steam Machine (discrete RDNA3, resizable BAR: copy and ring in VRAM),
// the Steam Deck (integrated: import, which RADV refuses for the guest's
// shared mapping, then a copy in uncached system memory; ring in VRAM), the
// Steam Frame and other unified-memory devices (a copy in their one
// host-visible device memory), and Apple's (imported).
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace gpu::memplan
{
    // VkMemoryPropertyFlagBits.
    constexpr uint32_t kDeviceLocal = 0x1, kHostVisible = 0x2, kHostCoherent = 0x4, kHostCached = 0x8;
    constexpr uint32_t kMapped = kHostVisible | kHostCoherent;
    // Never planned: lazily allocated, protected, and AMD's device-coherent
    // and device-uncached types (slower; they come after the plain ones).
    constexpr uint32_t kSpecial = 0x10 | 0x20 | 0x40 | 0x80;
    constexpr uint32_t kNoType = UINT32_MAX;

    constexpr uint64_t kGuestBytes = 512ull << 20;
    constexpr uint64_t kRingBytes = 192ull << 20;
    // What we plan into a heap: at most this share of it (1/2).
    constexpr uint64_t kHeapShareDivisor = 2;

    // VkDriverId values the plan knows.
    constexpr uint32_t kDriverIntelMesa = 6;  // VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA (ANV, Linux)

    enum class Gpu : uint8_t { kDiscrete, kIntegrated, kOther };
    // kVram: device-local and host-visible ("CPU-mapped VRAM"; on a unified
    // memory device, its one host-visible memory). kSystem: host-visible, not
    // device-local, not cached (write-combined system memory). kCached:
    // host-visible and cached; not device-local for guest memory, any for
    // the ring (NFSMW_RING_MEMORY=cached chose so before).
    enum class Place : uint8_t { kImport, kVram, kSystem, kCached };

    struct Type
    {
        uint32_t flags = 0, heap = 0;
    };
    struct Memory
    {
        std::vector<Type> types;
        std::vector<uint64_t> heaps;  // bytes
    };

    struct Candidate
    {
        Place place;
        uint32_t type;  // kNoType for kImport (the import finds its own)
        bool fits;      // its heap holds it (kHeapShareDivisor); always true for kImport
    };

    struct Plan
    {
        std::vector<Candidate> order;  // try in turn
        std::string why;               // the reason for the order, for the log
    };

    // The environment's overrides (unset or unknown: kAuto).
    //   NFSMW_SHARED_MEMORY=import|shadow: import first / never import.
    //   NFSMW_SHADOW_MEMORY=vram|gtt: the copy's first place.
    //   NFSMW_RING_MEMORY=vram|cached|system: the ring's first place.
    // An override is tried first whatever its heap's size, and the default
    // order follows it.
    struct Overrides
    {
        enum class Shared : uint8_t { kAuto, kImport, kShadow } shared = Shared::kAuto;
        Place copy = Place::kImport;  // kImport: none (kVram or kSystem when set)
        Place ring = Place::kImport;  // kImport: none
    };

    inline bool Is(const char* v, const char* s)
    {
        return v && std::string(v) == s;
    }

    inline Overrides ParseOverrides(const char* shared, const char* shadow, const char* ring)
    {
        Overrides o;
        o.shared = Is(shared, "import") ? Overrides::Shared::kImport : Is(shared, "shadow") ? Overrides::Shared::kShadow : Overrides::Shared::kAuto;
        o.copy = Is(shadow, "vram") ? Place::kVram : Is(shadow, "gtt") ? Place::kSystem : Place::kImport;
        o.ring = Is(ring, "vram") ? Place::kVram : Is(ring, "cached") ? Place::kCached : Is(ring, "system") ? Place::kSystem : Place::kImport;
        return o;
    }

    // Host-memory import that the driver offers but that can't work here:
    // nullptr when it can, else why not (for the log). Intel's Linux driver
    // (ANV) imports host memory as an i915 (or xe) userptr object. Each
    // protection change on those pages (the write watch's mprotect, many a
    // frame) invalidates it, and the next submission pins all its pages
    // again for writing; any page the write watch holds read-only or
    // inaccessible then fails that (EFAULT), the kernel refuses the
    // submission, and ANV reports the device lost: issue #2's Arrow Lake
    // laptop, VK_ERROR_DEVICE_LOST in vkQueueSubmit 0.8 s after the import,
    // with ANV's dump of the refused batch (the 512 MB "host-ptr" object at
    // the guest's 0xA0000000 window) just before. NVIDIA's driver pins the
    // pages once at import and MoltenVK wraps them in a Metal buffer, so
    // protection changes don't touch the GPU's mapping there.
    inline const char* ImportRefusal(uint32_t driverId)
    {
        if (driverId == kDriverIntelMesa)
            return "Intel's Linux driver can't keep imported memory mapped while the write watch protects its pages (issue #2)";
        return nullptr;
    }

    // NFSMW_TEST_BAR_HEAP_MB (a test hook): every heap holding a CPU-mapped
    // VRAM type reads as `bytes`, as on a GPU without resizable BAR.
    inline void ShrinkMappedVram(Memory& m, uint64_t bytes)
    {
        for (const Type& t : m.types)
            if ((t.flags & (kDeviceLocal | kHostVisible)) == (kDeviceLocal | kHostVisible) && t.heap < m.heaps.size())
                m.heaps[t.heap] = bytes;
    }

    // The first memory type among `bits` for `place` (kNoType: none).
    inline uint32_t FindType(const Memory& m, uint32_t bits, Place place, bool ring)
    {
        auto find = [&](auto&& ok) {
            for (uint32_t i = 0; i < m.types.size() && i < 32; i++)
                if ((bits & (1u << i)) && !(m.types[i].flags & kSpecial) && (m.types[i].flags & kMapped) == kMapped && ok(m.types[i].flags))
                    return i;
            return kNoType;
        };
        switch (place)
        {
        case Place::kVram:
            return find([](uint32_t f) { return (f & kDeviceLocal) != 0; });
        case Place::kSystem:
            return find([](uint32_t f) { return !(f & (kDeviceLocal | kHostCached)); });
        case Place::kCached:
        {
            uint32_t t = find([](uint32_t f) { return (f & kHostCached) && !(f & kDeviceLocal); });
            return t != kNoType || !ring ? t : find([](uint32_t f) { return (f & kHostCached) != 0; });
        }
        case Place::kImport:
            break;
        }
        return kNoType;
    }

    inline uint64_t HeapOf(const Memory& m, uint32_t type)
    {
        return type < m.types.size() && m.types[type].heap < m.heaps.size() ? m.heaps[m.types[type].heap] : 0;
    }

    // `bytes` more in `type`'s heap, beside `also` already there, within kHeapShareDivisor.
    inline bool Fits(const Memory& m, uint32_t type, uint64_t bytes, uint64_t also = 0)
    {
        return type != kNoType && also + bytes <= HeapOf(m, type) / kHeapShareDivisor;
    }

    inline std::string Size(uint64_t bytes)
    {
        char s[32];
        if (bytes >= (10ull << 30))
            snprintf(s, sizeof(s), "%.0f GB", double(bytes) / double(1ull << 30));
        else if (bytes >= (1ull << 30))
            snprintf(s, sizeof(s), "%.2f GB", double(bytes) / double(1ull << 30));
        else
            snprintf(s, sizeof(s), "%llu MB", (unsigned long long)(bytes >> 20));
        return s;
    }

    // For NFSMW_TEST_ALLOC_FAIL and the log.
    inline const char* Name(Place place, bool ring)
    {
        switch (place)
        {
        case Place::kImport: return "import";
        case Place::kVram: return ring ? "ring-vram" : "copy-vram";
        case Place::kSystem: return ring ? "ring-system" : "copy-system";
        case Place::kCached: return ring ? "ring-cached" : "copy-cached";
        }
        return "?";
    }

    namespace detail
    {
        // `places` in order, each once, those with a type (or kImport).
        inline void Add(Plan& p, const Memory& m, uint32_t bits, const std::vector<Place>& places, bool ring, uint64_t bytes, uint64_t also)
        {
            for (Place place : places)
            {
                bool seen = false;
                for (const Candidate& c : p.order)
                    seen |= c.place == place;
                if (seen)
                    continue;
                if (place == Place::kImport)
                {
                    p.order.push_back({ place, kNoType, true });
                    continue;
                }
                uint32_t type = FindType(m, bits, place, ring);
                if (type != kNoType)
                    p.order.push_back({ place, type, Fits(m, type, bytes, place == Place::kVram ? also : 0) });
            }
        }

        inline std::string HeapText(const Memory& m, uint32_t type)
        {
            return "heap " + std::to_string(m.types[type].heap) + ", " + Size(HeapOf(m, type));
        }
    }

    struct GuestInput
    {
        Gpu gpu = Gpu::kOther;
        bool importable = false;            // VK_EXT_external_memory_host
        const char* importRefusal = nullptr;  // ImportRefusal()
        uint32_t bits = ~0u;                // the copy buffer's memoryTypeBits
        Overrides overrides;
    };

    // Guest physical memory.
    inline Plan PlanGuestMemory(const Memory& m, const GuestInput& in)
    {
        Plan p;
        const Overrides& o = in.overrides;
        uint32_t vram = FindType(m, in.bits, Place::kVram, false);
        bool vramFits = Fits(m, vram, kGuestBytes);
        bool import = in.importable && o.shared != Overrides::Shared::kShadow &&
            (!in.importRefusal || o.shared == Overrides::Shared::kImport);

        // The copy's places, best first.
        std::vector<Place> copy;
        switch (in.gpu)
        {
        case Gpu::kDiscrete:
            copy = vramFits ? std::vector<Place>{ Place::kVram, Place::kSystem, Place::kCached }
                            : std::vector<Place>{ Place::kSystem, Place::kCached, Place::kVram };
            break;
        case Gpu::kIntegrated:
            // Its "VRAM" is a carve-out of the same RAM, 1 GB on a Deck, and
            // the copy there with the textures and targets didn't fit: at
            // every world load the kernel moved ~500 MB of it out to GTT,
            // and the GPU reads GTT as fast. So system memory (write-combined
            // GTT on amdgpu, uncached first), and the device's host-visible
            // memory where that is all there is (unified memory: the Steam
            // Frame, Intel).
            copy = { Place::kSystem, Place::kCached, Place::kVram };
            break;
        case Gpu::kOther:
            copy = { Place::kVram, Place::kSystem, Place::kCached };
            break;
        }
        if (o.copy != Place::kImport)
            copy.insert(copy.begin(), o.copy);

        // Import: first, except on a discrete GPU whose own memory takes the
        // copy (VRAM it holds, or the place NFSMW_SHADOW_MEMORY names): the
        // GPU reads its VRAM faster than system memory over PCIe.
        std::vector<Place> places;
        bool copyFirst = in.gpu == Gpu::kDiscrete && o.shared != Overrides::Shared::kImport && (vramFits || o.copy != Place::kImport);
        if (import && !copyFirst)
            places.push_back(Place::kImport);
        places.push_back(copy.front());
        if (import && copyFirst)
            places.push_back(Place::kImport);
        places.insert(places.end(), copy.begin() + 1, copy.end());
        detail::Add(p, m, in.bits, places, false, kGuestBytes, 0);

        // Why.
        std::string why;
        switch (in.gpu)
        {
        case Gpu::kDiscrete:
            if (vram == kNoType)
                why = "a discrete GPU without CPU-mapped VRAM";
            else if (vramFits)
                why = "a discrete GPU whose CPU-mapped VRAM (" + detail::HeapText(m, vram) + ") holds the 512 MB copy: resizable BAR";
            else
                why = "a discrete GPU whose CPU-mapped VRAM (" + detail::HeapText(m, vram) +
                    ") is too small for the 512 MB copy: no resizable BAR";
            break;
        case Gpu::kIntegrated:
            why = "an integrated GPU (system memory)";
            break;
        case Gpu::kOther:
            why = "a GPU neither discrete nor integrated";
            break;
        }
        if (o.shared == Overrides::Shared::kImport)
            why += in.importable ? "; NFSMW_SHARED_MEMORY=import" : "; NFSMW_SHARED_MEMORY=import, but the driver can't import host memory";
        else if (o.shared == Overrides::Shared::kShadow)
            why += "; NFSMW_SHARED_MEMORY=shadow";
        else if (!in.importable && (in.gpu != Gpu::kDiscrete || !vramFits))
            why += "; the driver can't import host memory";
        else if (in.importRefusal)
            why += std::string("; not imported: ") + in.importRefusal;
        if (o.copy == Place::kVram)
            why += "; NFSMW_SHADOW_MEMORY=vram";
        else if (o.copy == Place::kSystem)
            why += "; NFSMW_SHADOW_MEMORY=gtt";
        p.why = why;
        return p;
    }

    // The upload ring. `guestType`: the memory type guest memory's copy went
    // to (kNoType: imported), whose 512 MB count against the ring's heap.
    inline Plan PlanRing(const Memory& m, uint32_t bits, uint32_t guestType, Place override = Place::kImport)
    {
        Plan p;
        uint32_t vram = FindType(m, bits, Place::kVram, true);
        uint64_t also = guestType != kNoType && vram != kNoType && guestType < m.types.size() &&
            m.types[guestType].heap == m.types[vram].heap ? kGuestBytes : 0;
        bool vramFits = Fits(m, vram, kRingBytes, also);
        std::vector<Place> places = vramFits ? std::vector<Place>{ Place::kVram, Place::kCached, Place::kSystem }
                                             : std::vector<Place>{ Place::kCached, Place::kSystem, Place::kVram };
        if (override != Place::kImport)
            places.insert(places.begin(), override);
        detail::Add(p, m, bits, places, true, kRingBytes, also);
        if (vram == kNoType)
            p.why = "no CPU-mapped VRAM";
        else if (vramFits)
            p.why = "CPU-mapped VRAM (" + detail::HeapText(m, vram) + ") holds it";
        else
            p.why = "CPU-mapped VRAM (" + detail::HeapText(m, vram) + ") is too small for the 192 MB ring" +
                (also ? " beside the copy" : "");
        if (override == Place::kVram)
            p.why += "; NFSMW_RING_MEMORY=vram";
        else if (override == Place::kCached)
            p.why += "; NFSMW_RING_MEMORY=cached";
        else if (override == Place::kSystem)
            p.why += "; NFSMW_RING_MEMORY=system";
        return p;
    }
}
