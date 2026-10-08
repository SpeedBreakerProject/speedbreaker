// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
// See renderer.h. State translation follows Xenia (BSD): draw_util.cc and
// vulkan/vulkan_pipeline_cache.cc.
#include <stdafx.h>
#include "renderer.h"
#include <cpu/guest_thread.h>
#include <cpu/host_cpu.h>
#include <cpu/guest_time.h>
#include <debug/timeline.h>
#include <unordered_set>
#include "block_ranges.h"
#include "shader_translator.h"
#include "zpd_report.h"
#include "shared_memory_glsl.h"
#include "xenos/registers.h"

#include <kernel/vmem.h>
#include <kernel/write_watch.h>
#include <video/presenter.h>
#include <video/picture_fit.h>
#include <user/paths.h>
#include <user/settings.h>
#include <game/ultrawide.h>

#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>

#include <array>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#include <memory>
#include <unordered_map>

namespace gpu::renderer
{
    namespace
    {
        using namespace xe::gpu;

        const video::VulkanContext* s_vk = nullptr;
        VkDevice s_dev = VK_NULL_HANDLE;
        const uint32_t* s_regs = nullptr;
        PFN_vkCmdPushDescriptorSetKHR s_pushDescriptorSet = nullptr;
        Stats s_stats{};
        // NFSMW_GPU_EXP bitmask, for performance experiments: 1 skip draw
        // calls, 2 skip resolve copies, 4 skip texture loads, 8 bind 1x1
        // placeholders instead of textures.
        const uint32_t s_exp = [] { const char* v = std::getenv("NFSMW_GPU_EXP"); return v ? uint32_t(std::atoi(v)) : 0u; }();
        // Fewer render passes and texture reloads (NFSMW_PASS_MERGE,
        // NFSMW_CLEAR_FLUSH_RECT, NFSMW_DIRECT_CUBE, NFSMW_FUSE_PARTIAL): on
        // by default where passes cost most, the tile GPUs: under MoltenVK
        // (iPad race frame: 46 -> 20 passes; heavy scenes ~4 ms less GPU
        // time) and Turnip on arm64 Linux (Steam Frame: 37 -> 21 passes,
        // -0.3 ms GPU a busy frame); off elsewhere until measured there.
        // Lighter passes (NFSMW_UNTILED_AREA, NFSMW_PASS_BARRIER): Apple only.
        // On the Frame the smaller area cost +0.3 ms (Turnip chooses tiled
        // or direct rendering by the area) and the barrier changed nothing.
        // Each variable overrides.
#if defined(__APPLE__) || (defined(__linux__) && defined(__aarch64__))
        constexpr bool kFewerPasses = true;
#else
        constexpr bool kFewerPasses = false;
#endif
#ifdef __APPLE__
        constexpr bool kLighterPasses = true;
#else
        constexpr bool kLighterPasses = false;
#endif

        // Where a frame's time went, for hitch reports (renderer::HitchReport).
        struct HitchTimes
        {
            double shaderMs = 0, pipelineMs = 0, textureMs = 0, shadowMs = 0, imageMs = 0, submitMs = 0;
            uint32_t shaders = 0, pipelines = 0, textures = 0, images = 0, scaledTextures = 0;
            uint64_t textureBytes = 0;
        };
        HitchTimes s_hitch;
        struct HitchTimer
        {
            double& total;
            std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
            explicit HitchTimer(double& t) : total(t) {}
            ~HitchTimer() { total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); }
        };

        float RegF(uint32_t index)
        {
            float f;
            memcpy(&f, &s_regs[index], 4);
            return f;
        }

        template<typename T>
        T Reg()
        {
            T t;
            t.value = s_regs[T::register_index];
            return t;
        }

        template<typename T>
        T RegAt(uint32_t index)
        {
            T t;
            t.value = s_regs[index];
            return t;
        }

        void Check(VkResult r, const char* what)
        {
            if (r != VK_SUCCESS)
            {
                fprintf(stderr, "[renderer] %s failed: %d\n", what, int(r));
                abort();
            }
        }

        // Log a message the first time `key` is seen.
        void LogOnce(uint64_t key, const char* fmt, ...)
        {
            static std::unordered_map<uint64_t, bool> seen;
            if (seen[key])
                return;
            seen[key] = true;
            va_list args;
            va_start(args, fmt);
            fprintf(stderr, "[renderer] ");
            vfprintf(stderr, fmt, args);
            fprintf(stderr, "\n");
            va_end(args);
        }

        uint32_t GpuSwap(uint32_t v, uint32_t endian)
        {
            switch (endian & 3)
            {
            case 1: return ((v << 8) & 0xFF00FF00) | ((v >> 8) & 0x00FF00FF);
            case 2: return __builtin_bswap32(v);
            case 3: return (v >> 16) | (v << 16);
            default: return v;
            }
        }

        // ---------------------------------------------------------------
        // Memory

        VkPhysicalDeviceMemoryProperties s_memProps;
        bool s_memoryBudget = false;  // VK_EXT_memory_budget (the [perf] line's device memory)

        uint32_t FindMemoryType(uint32_t bits, VkMemoryPropertyFlags want)
        {
            for (uint32_t i = 0; i < s_memProps.memoryTypeCount; i++)
                if ((bits & (1u << i)) && (s_memProps.memoryTypes[i].propertyFlags & want) == want)
                    return i;
            for (uint32_t i = 0; i < s_memProps.memoryTypeCount; i++)
                if (bits & (1u << i))
                    return i;
            return 0;
        }

        // `prefer` when some type has it (e.g. device-local and host-visible:
        // VRAM the CPU can write, with resizable BAR), else `want`.
        uint32_t FindMemoryTypePreferring(uint32_t bits, VkMemoryPropertyFlags prefer, VkMemoryPropertyFlags want)
        {
            for (uint32_t i = 0; i < s_memProps.memoryTypeCount; i++)
                if ((bits & (1u << i)) && (s_memProps.memoryTypes[i].propertyFlags & prefer) == prefer)
                    return i;
            return FindMemoryType(bits, want);
        }

        // Guest physical memory (512 MB), imported without a copy.
        constexpr VkDeviceSize kSharedSize = 512ull << 20;
        VkBuffer s_shared = VK_NULL_HANDLE;
        VkDeviceMemory s_sharedMemory = VK_NULL_HANDLE;
        uint8_t* s_sharedHost = nullptr;
        // Devices that bind less of a storage buffer than guest memory
        // (maxStorageBufferRange: 128 MB on Turnip, the Steam Frame's Adreno,
        // where everything above the first 128 MB read as zeros: a black
        // picture) get it as four 128 MB parts: binding 0 and
        // kSharedPartBinding.. in every layout that has it, SPLIT_MEMORY in
        // the shaders (gpu/shared_memory_glsl.h). Elsewhere (RADV, MoltenVK:
        // 4 GB) one binding, and the shaders compile as before.
        // NFSMW_SPLIT_MEMORY=1|0 forces the choice.
        constexpr VkDeviceSize kSharedPart = 128ull << 20;
        constexpr uint32_t kSharedPartBinding = 13;
        bool s_splitMemory = false;
        VkDescriptorBufferInfo s_sharedInfo[4]{};  // the binding(s): [0] alone, or the four parts

        void SetSharedInfo()
        {
            for (uint32_t k = 0; k < 4; k++)
                s_sharedInfo[k] = s_splitMemory ? VkDescriptorBufferInfo{ s_shared, k * kSharedPart, kSharedPart }
                                                : VkDescriptorBufferInfo{ s_shared, 0, kSharedSize };
        }

        // A layout's bindings, plus (split) guest memory's other three
        // parts: copies of its binding `shared`.
        std::vector<VkDescriptorSetLayoutBinding> WithSharedParts(const VkDescriptorSetLayoutBinding* b, uint32_t count,
            uint32_t shared = 0)
        {
            std::vector<VkDescriptorSetLayoutBinding> out(b, b + count);
            for (uint32_t k = 0; s_splitMemory && k < 3; k++)
            {
                out.push_back(b[shared]);
                out.back().binding = kSharedPartBinding + k;
            }
            return out;
        }

        // Appends the writes for guest memory's other three parts (split
        // only) after `count` writes; returns the new count. `w` has room
        // for 3 more.
        uint32_t AddSharedParts(VkWriteDescriptorSet* w, uint32_t count, VkDescriptorSet set = VK_NULL_HANDLE)
        {
            for (uint32_t k = 1; s_splitMemory && k < 4; k++)
                w[count++] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, kSharedPartBinding + k - 1, 0, 1,
                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &s_sharedInfo[k], nullptr };
            return count;
        }

        // A stand-in storage buffer for a binding a shader won't read this
        // time (no scaled memory): guest memory, as much as one binding takes.
        VkDescriptorBufferInfo DummyStorage(VkBuffer buffer)
        {
            return { buffer ? buffer : s_shared, 0, buffer || !s_splitMemory ? VK_WHOLE_SIZE : kSharedPart };
        }
        // Discrete GPUs, and drivers that can't import the guest's memory
        // (amdgpu imports only anonymous memory, and guest physical memory is
        // a shared mapping, mirrored three times): the GPU gets its own copy
        // (VRAM the CPU writes through resizable BAR when there is one), kept
        // current page by page from the write watch (writewatch::SyncShadow).
        // NFSMW_SHARED_MEMORY=import|shadow overrides the choice.
        bool s_shadowMode = false;
        bool s_shadowInVram = false;  // (the "ready" line)
        std::unordered_map<uint64_t, uint64_t> s_shadowSynced;  // range -> writewatch::Current() at its last sync

        uint32_t LoadPhysical(uint32_t address)
        {
            uint32_t v;
            memcpy(&v, s_sharedHost + (address & 0x1FFFFFFC), 4);
            return v;  // host-order load of the guest's bytes (as the GPU reads them)
        }

        // Upload ring: draw constants, converted indices and texture staging.
        // One slot per in-flight submission, so a slot is reused only after
        // the GPU finished with it.
        constexpr uint32_t kSlots = 12;
        constexpr VkDeviceSize kSlotSize = 16ull << 20;
        constexpr VkDeviceSize kRingSize = kSlots * kSlotSize;
        VkBuffer s_ring = VK_NULL_HANDLE;
        VkDeviceMemory s_ringMemory = VK_NULL_HANDLE;
        uint8_t* s_ringHost = nullptr;
        VkDeviceSize s_ringOffset = 0;

        // XenosConstants (shader_translator.cpp): c[512], fetch[48], bools[2],
        // loops[8], drawInfo[4], each a vec4/uvec4.
        constexpr uint32_t kConstantsSize = (512 + 48 + 2 + 8 + 4) * 16;

        // Image memory. Images are bound at offsets into blocks of device
        // memory (64 MB; gpu/block_ranges.h keeps each block's free ranges),
        // one list per memory type, instead of an allocation each. On the
        // Deck vkAllocateMemory was most of a new texture's cost (40-220 us
        // in scripted runs, 380 us while the kernel was moving memory out of
        // VRAM, about 1 ms after a long free roam, where frames that streamed
        // textures spent a median 9.5 ms making images); a range in a block
        // is a few us. A block is allocated ahead of need, off the command
        // processor: the nfsmw-memory thread keeps an empty spare of each
        // memory type in use (0.1-0.2 ms a block there, unless the kernel has
        // memory to move), so the command processor takes the spare when a
        // block fills and the thread makes the next. Without a spare ready (the first block, or a
        // burst that outran the thread) the command processor allocates the
        // block itself, counted in the [perf] line. An image bigger than half
        // a block, one the driver wants in an allocation of its own
        // (VkMemoryDedicatedRequirements) and any image when a block can't be
        // had keep their own allocation, as before. A block whose images all
        // went is a spare too, freed by the thread after a few seconds unused
        // beside another. Images are destroyed only with the GPU idle
        // (DestroyImage), so a range can be handed out again at once.
        // NFSMW_IMAGE_POOL=0: an allocation per image (the old path);
        // NFSMW_IMAGE_BLOCK_MB: the block size (16-1024).
        struct ImageBlock
        {
            VkDeviceMemory memory = VK_NULL_HANDLE;
            uint32_t type = 0;
            gpu::BlockRanges ranges;
        };

        struct Image
        {
            VkImage image = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;  // its own, or its block's
            VkImageView view = VK_NULL_HANDLE;
            VkDeviceSize bytes = 0;  // its allocation or range (the [perf] line's memory)
            ImageBlock* block = nullptr;  // bound at `offset` in this block, if any
            VkDeviceSize offset = 0;
        };

        const bool s_imagePool = [] { const char* v = std::getenv("NFSMW_IMAGE_POOL"); return !v || v[0] != '0'; }();
        const VkDeviceSize s_blockSize = [] {
            const char* v = std::getenv("NFSMW_IMAGE_BLOCK_MB");
            return VkDeviceSize(v ? std::clamp(std::atoi(v), 16, 1024) : 64) << 20;
        }();
        std::vector<std::unique_ptr<ImageBlock>> s_blocks;  // in use (command processor)
        // Empty blocks by memory type, the newest last, under s_spareMutex:
        // the one the nfsmw-memory thread keeps ready, and blocks whose images
        // all went. Those are kept kSpareKeep (a settings change drops
        // textures or targets and makes them again at once) and then freed
        // down to one.
        struct Spare
        {
            VkDeviceMemory memory;
            std::chrono::steady_clock::time_point since;
        };
        constexpr auto kSpareKeep = std::chrono::seconds(5);
        std::mutex s_spareMutex;
        std::condition_variable s_spareCv;
        std::vector<Spare> s_spares[VK_MAX_MEMORY_TYPES];
        uint32_t s_spareWanted = 0, s_spareFailed = 0;  // memory type bits
        // Totals for the [perf] line (command processor): images made and
        // the time it took, vkAllocateMemory calls for them (their own or a
        // block's) and the time those took, blocks taken from the spares,
        // blocks the spares didn't cover; under s_spareMutex, the memory
        // thread's: blocks it made and their time, blocks freed.
        struct ImageTotals
        {
            uint64_t images = 0, imageNs = 0, allocations = 0, allocationNs = 0, fromSpare = 0, blocksHere = 0;
            uint64_t spareBlocks = 0, spareNs = 0, blocksFreed = 0;
        };
        ImageTotals s_imageTotals;

        uint64_t SinceNs(std::chrono::steady_clock::time_point start)
        {
            return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
        }

        VkDeviceMemory AllocateBlock(uint32_t type)
        {
            VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            mai.allocationSize = s_blockSize;
            mai.memoryTypeIndex = type;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            return vkAllocateMemory(s_dev, &mai, nullptr, &memory) == VK_SUCCESS ? memory : VK_NULL_HANDLE;
        }

        // nfsmw-memory: makes a spare when one is wanted, and frees spares
        // beyond one once they have been empty kSpareKeep.
        void MemoryThread()
        {
            SetHostThreadName("nfsmw-memory");
            hostcpu::LeaveReservedCore();  // started by the command processor
            std::unique_lock lock(s_spareMutex);
            while (true)
            {
                auto now = std::chrono::steady_clock::now();
                auto next = now + std::chrono::hours(1);
                bool worked = false;
                for (uint32_t type = 0; type < VK_MAX_MEMORY_TYPES; type++)
                {
                    std::vector<Spare>& spares = s_spares[type];
                    if (spares.size() > 1 && now - spares.front().since >= kSpareKeep)
                    {
                        VkDeviceMemory memory = spares.front().memory;
                        spares.erase(spares.begin());
                        s_imageTotals.blocksFreed++;
                        lock.unlock();
                        vkFreeMemory(s_dev, memory, nullptr);
                        lock.lock();
                        worked = true;
                        break;
                    }
                    if (spares.size() > 1)
                        next = std::min(next, spares.front().since + kSpareKeep);
                    uint32_t bit = 1u << type;
                    if ((s_spareWanted & bit) && !(s_spareFailed & bit) && spares.empty())
                    {
                        lock.unlock();
                        auto start = std::chrono::steady_clock::now();
                        VkDeviceMemory memory = AllocateBlock(type);
                        uint64_t ns = SinceNs(start);
                        lock.lock();
                        s_spareWanted &= ~bit;
                        if (!memory)
                        {
                            // Left to the command processor, which tries again
                            // (and falls back to an allocation per image).
                            s_spareFailed |= bit;
                            fprintf(stderr, "[renderer] image memory: no spare block of memory type %u (%llu MB)\n", type,
                                (unsigned long long)(s_blockSize >> 20));
                        }
                        else
                        {
                            s_imageTotals.spareBlocks++;
                            s_imageTotals.spareNs += ns;
                            s_spares[type].push_back({ memory, std::chrono::steady_clock::now() });
                        }
                        worked = true;
                        break;
                    }
                    if (s_spareWanted & bit)
                        s_spareWanted &= ~bit;  // (a spare came back meanwhile, or the thread can't make one)
                }
                if (!worked)
                    s_spareCv.wait_until(lock, next);
            }
        }

        // A block of `type` for a new image: a spare (the newest; the thread
        // makes the next when it was the last), or one allocated here.
        ImageBlock* NewBlock(uint32_t type)
        {
            VkDeviceMemory memory = VK_NULL_HANDLE;
            {
                std::lock_guard lock(s_spareMutex);
                std::vector<Spare>& spares = s_spares[type];
                if (!spares.empty())
                {
                    memory = spares.back().memory;
                    spares.pop_back();
                }
                if (spares.empty())
                    s_spareWanted |= 1u << type;
            }
            s_spareCv.notify_one();
            if (memory)
                s_imageTotals.fromSpare++;
            else
            {
                auto start = std::chrono::steady_clock::now();
                memory = AllocateBlock(type);
                s_imageTotals.allocations++;
                s_imageTotals.allocationNs += SinceNs(start);
                if (!memory)
                    return nullptr;
                s_imageTotals.blocksHere++;
                std::lock_guard lock(s_spareMutex);
                s_spareFailed &= ~(1u << type);
            }
            s_blocks.push_back(std::make_unique<ImageBlock>(ImageBlock{ memory, type, gpu::BlockRanges(s_blockSize) }));
            return s_blocks.back().get();
        }

        // Binds `img` into a block of `type`; false when no block can be had.
        bool BindToBlock(Image& img, const VkMemoryRequirements& req, uint32_t type)
        {
            ImageBlock* block = nullptr;
            VkDeviceSize offset = gpu::BlockRanges::kNone;
            for (auto& b : s_blocks)
                if (b->type == type && (offset = b->ranges.Allocate(req.size, req.alignment)) != gpu::BlockRanges::kNone)
                {
                    block = b.get();
                    break;
                }
            if (!block)
            {
                block = NewBlock(type);
                if (!block || (offset = block->ranges.Allocate(req.size, req.alignment)) == gpu::BlockRanges::kNone)
                    return false;
            }
            img.block = block;
            img.offset = offset;
            img.memory = block->memory;
            return true;
        }

        void DestroyImage(Image& img);

        // `imageNext`, `viewNext`: chained to the image's and its view's create infos.
        // `fallible`: an image or view the device turns down returns an empty
        // Image (nothing kept) instead of aborting.
        Image CreateImage(VkImageType type, VkFormat format, VkExtent3D extent, uint32_t layers,
            VkImageUsageFlags usage, VkImageViewType viewType, VkImageAspectFlags aspect, VkImageCreateFlags flags = 0,
            uint32_t levels = 1, const void* imageNext = nullptr, const void* viewNext = nullptr, bool fallible = false)
        {
            HitchTimer hitchTimer(s_hitch.imageMs);
            s_hitch.images++;
            auto start = std::chrono::steady_clock::now();
            Image img;
            VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
            ici.pNext = imageNext;
            ici.flags = flags;
            ici.imageType = type;
            ici.format = format;
            ici.extent = extent;
            ici.mipLevels = levels;
            ici.arrayLayers = layers;
            ici.samples = VK_SAMPLE_COUNT_1_BIT;
            ici.tiling = VK_IMAGE_TILING_OPTIMAL;  // (image memory blocks hold nothing linear)
            ici.usage = usage;
            ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VkResult result = vkCreateImage(s_dev, &ici, nullptr, &img.image);
            if (fallible && result != VK_SUCCESS)
                return {};
            Check(result, "vkCreateImage");
            VkMemoryDedicatedRequirements dedicated{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS };
            VkMemoryRequirements2 req2{ VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &dedicated };
            if (s_imagePool)
            {
                VkImageMemoryRequirementsInfo2 info{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, nullptr, img.image };
                vkGetImageMemoryRequirements2(s_dev, &info, &req2);
            }
            else
                vkGetImageMemoryRequirements(s_dev, img.image, &req2.memoryRequirements);
            const VkMemoryRequirements& req = req2.memoryRequirements;
            uint32_t memoryType = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            img.bytes = req.size;
            if (!s_imagePool || dedicated.prefersDedicatedAllocation || dedicated.requiresDedicatedAllocation ||
                req.size > s_blockSize / 2 || !BindToBlock(img, req, memoryType))
            {
                VkMemoryDedicatedAllocateInfo dai{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, nullptr, img.image };
                VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
                mai.pNext = dedicated.requiresDedicatedAllocation ? &dai : nullptr;
                mai.allocationSize = req.size;
                mai.memoryTypeIndex = memoryType;
                auto allocationStart = std::chrono::steady_clock::now();
                Check(vkAllocateMemory(s_dev, &mai, nullptr, &img.memory), "vkAllocateMemory(image)");
                s_imageTotals.allocations++;
                s_imageTotals.allocationNs += SinceNs(allocationStart);
            }
            Check(vkBindImageMemory(s_dev, img.image, img.memory, img.offset), "vkBindImageMemory");
            VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            vci.pNext = viewNext;
            vci.image = img.image;
            vci.viewType = viewType;
            vci.format = format;
            vci.subresourceRange = { aspect, 0, levels, 0, layers };
            result = vkCreateImageView(s_dev, &vci, nullptr, &img.view);
            if (fallible && result != VK_SUCCESS)
            {
                img.view = VK_NULL_HANDLE;
                DestroyImage(img);
                return {};
            }
            Check(result, "vkCreateImageView");
            s_imageTotals.images++;
            s_imageTotals.imageNs += SinceNs(start);
            return img;
        }

        // Nothing the GPU may still run can use it (see ApplyLiveSettings):
        // its memory, or its range of a block, is free for reuse at once.
        void DestroyImage(Image& img)
        {
            if (img.view)
                vkDestroyImageView(s_dev, img.view, nullptr);
            if (img.image)
                vkDestroyImage(s_dev, img.image, nullptr);
            if (ImageBlock* block = img.block)
            {
                if (!block->ranges.Free(img.offset, img.bytes))
                {
                    fprintf(stderr, "[renderer] image memory: %llu bytes at %llu aren't in use in their block\n",
                        (unsigned long long)img.bytes, (unsigned long long)img.offset);
                    abort();
                }
                if (block->ranges.Empty())
                {
                    // A spare now: the memory thread frees it if it stays
                    // unused beside another.
                    Spare spare{ block->memory, std::chrono::steady_clock::now() };
                    uint32_t type = block->type;
                    std::erase_if(s_blocks, [block](const std::unique_ptr<ImageBlock>& b) { return b.get() == block; });
                    {
                        std::lock_guard lock(s_spareMutex);
                        s_spares[type].push_back(spare);
                    }
                    s_spareCv.notify_one();
                }
            }
            else if (img.memory)
                vkFreeMemory(s_dev, img.memory, nullptr);
            img = {};
        }

        // ---------------------------------------------------------------
        // Command recording

        // Submissions. Work is recorded into the current slot's command
        // buffer and submitted without waiting; CPU-visible effects of the
        // command stream (fences, the read pointer) are attached to the
        // submission they follow and applied by the completion thread once
        // the GPU is done, as the real GPU would write them.
        // NFSMW_OCCLUSION=1 (ZpassDone): a counting segment is one precise
        // occlusion query, begun before a draw while a query bracket is open
        // and ended at its pass's end, at any ZPD or when its weight changes;
        // an END report is stored once the submission holding it completes,
        // with the samples of segments [s0, s1).
        struct ZpdSegment { uint64_t id; uint32_t query, num, den; };
        struct ZpdEnd { uint32_t address; uint64_t s0, s1, frame; std::chrono::steady_clock::time_point queued; };
        struct Submission
        {
            VkCommandBuffer cmd = VK_NULL_HANDLE;
            VkFence fence = VK_NULL_HANDLE;
            bool inFlight = false;
            // A write whose `event` is set runs as soon as the GPU passes that
            // point of the command buffer (vkCmdSetEvent after the work it
            // follows); the rest run when the whole submission completes.
            struct Write { std::function<void()> fn; VkEvent event = VK_NULL_HANDLE; std::chrono::steady_clock::time_point queued = std::chrono::steady_clock::now(); };
            std::vector<Write> writes;
            std::vector<VkEvent> events;  // returned to the pool on completion
            uint64_t number = 0;           // s_submissionCounter when submitted (write-watch guard)
            size_t unsignaled = 0;         // first write not yet covered by an event
            std::chrono::steady_clock::time_point submitted;
            // NFSMW_OCCLUSION=1 (else always empty): the segments recorded
            // here, harvested at completion, then the END reports, in order.
            std::vector<ZpdSegment> zpdSegments;
            std::vector<ZpdEnd> zpdEnds;
        };
        // NFSMW_FENCE_EVENTS=0 disables mid-buffer fence publication.
        const bool s_fenceEvents = [] { const char* v = std::getenv("NFSMW_FENCE_EVENTS"); return !v || v[0] != '0'; }();
        std::vector<VkEvent> s_eventPool;  // under s_submitMutex
        // NFSMW_LOG_LATENCY: queue -> publish delay of deferred writes, by
        // how they were published (0 event, 1 completion). Under s_submitMutex.
        double s_writeDelayUs[2] = {};
        uint32_t s_writeCount[2] = {};
        void RunWrite(Submission::Write& write, int how)
        {
            s_writeDelayUs[how] += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - write.queued).count();
            s_writeCount[how]++;
            if (how)
                timeline::Mark(timeline::WriteAtComplete);
            write.fn();
        }
        // GPU busy time: sum over submissions of the time from max(submit,
        // previous completion) to completion (completion-thread wake-up).
        std::atomic<uint64_t> s_gpuBusyUs{ 0 };
        // True GPU execution time: timestamps at each command buffer's start
        // and end (2 queries per slot).
        VkQueryPool s_timestamps = VK_NULL_HANDLE;  // NFSMW_GPU_TIMING=1 (each costs a blit encoder on MoltenVK)
        double s_timestampPeriod = 1.0;

        // NFSMW_PASS_PROFILE=<frame>: time every render pass of one frame and
        // print them (targets, area, draws, GPU us). 256 passes max.
        const int s_profileFrame = [] { const char* v = std::getenv("NFSMW_PASS_PROFILE"); return v ? std::atoi(v) : -1; }();
        VkQueryPool s_passQueries = VK_NULL_HANDLE;
        // Per profiled pass, also what was recorded in the gap before it:
        // resolves, texture uploads, submissions, guest memory synced to the
        // GPU copy (KB).
        struct PassInfo { std::string desc; uint32_t draws = 0; uint32_t slot, query; uint32_t resolves = 0, uploads = 0, submits = 0; uint64_t shadowKB = 0; };
        struct GapCounters { uint64_t resolves = 0, uploads = 0, submits = 0, shadowBytes = 0; } s_gapLast;
        std::vector<PassInfo> s_passInfos;
        bool s_profiling = false;
        uint32_t s_passQueryNext = 0;
        std::atomic<uint64_t> s_gpuExecNs{ 0 };
        std::chrono::steady_clock::time_point s_lastCompletion;
        VkCommandPool s_commandPool = VK_NULL_HANDLE;
        Submission s_submissions[kSlots];
        uint32_t s_slot = 0;
        uint64_t s_submissionCounter = 1;       // bumps on every submit
        VkDeviceSize s_lastConstantsOffset = 0;
        uint64_t s_lastConstantsSubmission = 0; // submission holding s_lastConstantsOffset
        const void* s_lastConstLayoutVs = nullptr;  // the layout of that upload (see Draw)
        const void* s_lastConstLayoutPs = nullptr;
        VkCommandBuffer s_cmd = VK_NULL_HANDLE;
        bool s_recording = false;
        std::chrono::steady_clock::time_point s_recordStart;
        std::mutex s_submitMutex;
        std::condition_variable s_submitCv;
        std::deque<uint32_t> s_inFlight;
        // NFSMW_LOG_LATENCY: time with nothing in flight (under s_submitMutex).
        std::chrono::steady_clock::time_point s_emptySince = std::chrono::steady_clock::now();
        double s_emptyUs = 0;

        // Occlusion queries counted on the GPU (NFSMW_OCCLUSION=1; the
        // presenter decides: VulkanContext::occlusionCounting). Command
        // processor thread, but for the results ring (completion thread) and
        // the store statistics (under s_submitMutex). See ZpassDone.
        bool s_zpdOn = false;                  // constant after Initialize
        constexpr uint32_t kZpdQueries = 256;  // per slot
        VkQueryPool s_zpdPool[kSlots] = {};
        uint32_t s_zpdUsed[kSlots] = {};       // used by the slot's recording (reset at its next Begin)
        int32_t s_zpdQuery = -1;               // the active segment's query in s_zpdPool[s_slot], or -1
        uint32_t s_zpdNum = 0, s_zpdDen = 0;   // its weight (guest samples per host sample, num / den)
        uint64_t s_zpdSegNext = 1;             // segment ids
        zpd::BracketBook s_zpdOpen;
        std::unique_ptr<zpd::ResultRing> s_zpdResults;
        // The [zpd] line's counts: the command processor's, and the stores'
        // (under s_submitMutex; the delays in ms, CP -> store).
        struct ZpdCounts { uint64_t zpds, begins, ends, segments, draws, unmatched, dropped, replaced, unknown, overflow, clamped, unclamped; };
        ZpdCounts s_zpdCounts{};
        struct ZpdStores { uint64_t stored, nonZero, lost; double delaySum, delayMax; };
        ZpdStores s_zpdStores{};
        std::atomic<uint64_t> s_zpdUnavailable{ 0 };
        // The latest END count per report address (count, frame), for
        // captures (LogOcclusionCounts, any thread).
        std::mutex s_zpdLatestMutex;
        std::map<uint32_t, std::pair<uint32_t, uint64_t>> s_zpdLatest;
        // NFSMW_LOG_ZPD=<lines>: each BEGIN, counted draw, segment and END,
        // that many lines at most (any thread).
        const int64_t s_zpdLogLines = [] { const char* v = std::getenv("NFSMW_LOG_ZPD"); return v ? int64_t(std::atoll(v)) : int64_t(0); }();
        std::atomic<int64_t> s_zpdLogged{ 0 };
        __attribute__((format(printf, 1, 2))) void ZpdLog(const char* fmt, ...)
        {
            if (s_zpdLogged.load(std::memory_order_relaxed) >= s_zpdLogLines || s_zpdLogged.fetch_add(1) >= s_zpdLogLines)
                return;
            char line[512];
            va_list args;
            va_start(args, fmt);
            vsnprintf(line, sizeof(line), fmt, args);
            va_end(args);
            fprintf(stderr, "[zpd] %s\n", line);
        }
        bool ZpdLogging() { return s_zpdLogged.load(std::memory_order_relaxed) < s_zpdLogLines; }

        // Ends the active segment. Inside its pass: one is begun only right
        // before a draw, and every pass end ends it first (EndPass).
        void ZpdEndSegment()
        {
            if (s_zpdQuery < 0)
                return;
            vkCmdEndQuery(s_cmd, s_zpdPool[s_slot], uint32_t(s_zpdQuery));
            s_zpdQuery = -1;
        }

        void EndPass();
        // Bumped when the command buffer's graphics state is unknown (a new
        // command buffer; compute work, which shares the push constants).
        uint64_t s_stateEpoch = 1;

        // Write-watch GPU guard for the submission being recorded, once per
        // range and submission (the same vertex buffer serves many draws).
        // The ranges guarded so far are a direct-mapped table stamped with
        // the submission: no clearing, no allocation (a range that a
        // collision evicted is guarded again, which changes nothing).
        // NFSMW_WATCH_RANGES=0 (write_watch.cpp): a hash set, as before.
        const bool s_guardTable = [] { const char* v = std::getenv("NFSMW_WATCH_RANGES"); return !v || v[0] != '0'; }();
        struct GuardedRange { uint64_t key, submission; };
        std::unique_ptr<GuardedRange[]> s_guardedRanges;  // 4096
        std::unordered_set<uint64_t> s_guarded;
        uint64_t s_guardedSubmission = 0;
        void GuardGpu(uint32_t base, uint32_t size, bool write, const char* tag, bool untile = false)
        {
            if ((!g_eagerWrites && !s_shadowMode) || size == 0)
                return;
            // (A texture's untile is keyed apart from a plain read of the same
            // range, or it would lose its flag: see writewatch::GpuRead.)
            uint64_t key = uint64_t(base) << 32 | (size ^ (write ? 0x80000000u : untile ? 0x40000000u : 0));
            if (s_guardTable)
            {
                if (!s_guardedRanges)
                    s_guardedRanges.reset(new GuardedRange[4096]());
                GuardedRange& slot = s_guardedRanges[(key * 0x9E3779B97F4A7C15ull) >> 52];
                if (slot.key == key && slot.submission == s_submissionCounter)
                    return;
                slot = { key, s_submissionCounter };
            }
            else
            {
                if (s_guardedSubmission != s_submissionCounter)
                {
                    s_guarded.clear();
                    s_guardedSubmission = s_submissionCounter;
                }
                if (!s_guarded.insert(key).second)
                    return;
            }
            if (write)
                writewatch::GpuWrite(base, size, s_submissionCounter, tag);
            else
                writewatch::GpuRead(base, size, s_submissionCounter, tag, untile);
        }

        // Outside a render pass: an event after everything recorded so far,
        // covering the current submission's writes that have none yet.
        void SignalWrites()
        {
            if (!s_fenceEvents || !s_recording)
                return;
            Submission& sub = s_submissions[s_slot];
            VkEvent event;
            {
                std::lock_guard lock(s_submitMutex);
                if (sub.unsignaled == sub.writes.size())
                    return;
                if (s_eventPool.empty())
                {
                    VkEventCreateInfo eci{ VK_STRUCTURE_TYPE_EVENT_CREATE_INFO };
                    Check(vkCreateEvent(s_dev, &eci, nullptr, &event), "vkCreateEvent");
                }
                else
                {
                    event = s_eventPool.back();
                    s_eventPool.pop_back();
                }
                for (size_t i = sub.unsignaled; i < sub.writes.size(); i++)
                    sub.writes[i].event = event;
                sub.unsignaled = sub.writes.size();
                sub.events.push_back(event);
            }
            vkCmdSetEvent(s_cmd, event, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        }

        void Begin()
        {
            if (s_recording)
                return;
            Submission& sub = s_submissions[s_slot];
            {
                CpWaitTimer wait(CP_WAIT_SLOT);
                std::unique_lock lock(s_submitMutex);
                s_submitCv.wait(lock, [&] { return !sub.inFlight; });
            }
            vkResetFences(s_dev, 1, &sub.fence);
            vkResetCommandBuffer(sub.cmd, 0);
            s_cmd = sub.cmd;
            VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            Check(vkBeginCommandBuffer(s_cmd, &bi), "vkBeginCommandBuffer");
            s_recording = true;
            s_stateEpoch++;
            s_recordStart = std::chrono::steady_clock::now();
            if (s_timestamps)
            {
                vkCmdResetQueryPool(s_cmd, s_timestamps, s_slot * 2, 2);
                vkCmdWriteTimestamp(s_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s_timestamps, s_slot * 2);
            }
            // Occlusion queries: the slot's last submission has retired, its
            // results harvested; reset the queries it used (a host reset is a
            // memset on MoltenVK; else a command, outside any pass here: for
            // devices before Vulkan 1.2, which requires host reset, so not
            // exercised on MoltenVK, RADV or Turnip).
            if (s_zpdOn && s_zpdUsed[s_slot])
            {
                if (s_vk->resetQueryPool)
                    s_vk->resetQueryPool(s_dev, s_zpdPool[s_slot], 0, s_zpdUsed[s_slot]);
                else
                    vkCmdResetQueryPool(s_cmd, s_zpdPool[s_slot], 0, s_zpdUsed[s_slot]);
                s_zpdUsed[s_slot] = 0;
            }
            s_ringOffset = 0;
        }

        // Submits per call site (NFSMW_LOG_LATENCY): 1 ring full, 2 Submit, 3 SubmitIfIdle,
        // 4 SubmitIfDue, 5 Flush, 6 QueueWrite on an idle GPU, 7 present, 8 after a draw
        // to a starving GPU, 9 after a resolve or upload to one (SubmitIfStarving).
        uint32_t s_submitWhy[10] = {};
        const uint32_t s_logTextureUse = [] { const char* v = std::getenv("NFSMW_LOG_TEXTURE_USE"); return v ? uint32_t(strtoul(v, nullptr, 16)) : 0u; }();
        bool s_drawUsesLoggedTexture = false;
        bool s_drawScaled = false;  // the current draw renders into scaled targets (internal resolution)
        bool s_logDrawsNow = false;
        std::string s_loggedTextureFetch;
        uint32_t s_drawsSinceCheck = 0;
        bool s_submitAfterDraw = false;  // mid-batch submit to a starving GPU (eager writes)
        double s_queueLockUs = 0, s_queueSubmitUs = 0;  // waiting for the queue lock, in vkQueueSubmit

        void SubmitRecorded(int why = 0)
        {
            if (!s_recording)
                return;
            s_submitWhy[why]++;
            timeline::Mark(timeline::SubmitBegin, why);
            EndPass();
            if (s_timestamps)
                vkCmdWriteTimestamp(s_cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s_timestamps, s_slot * 2 + 1);
            Check(vkEndCommandBuffer(s_cmd), "vkEndCommandBuffer");
            Submission& sub = s_submissions[s_slot];
            sub.number = s_submissionCounter;
            VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
            si.commandBufferCount = 1;
            si.pCommandBuffers = &s_cmd;
            {
                auto t0 = std::chrono::steady_clock::now();
                std::unique_lock lock(*s_vk->queueMutex);
                auto tl = std::chrono::steady_clock::now();
                // The presenter's gate holds GPU work while the app is
                // inactive (iOS). It closes only after guesttime::Suspend()
                // and opens before Resume(), so a hold here is always the
                // suspension's time, not this submit's or the queue lock's.
                if (guesttime::Suspended())
                {
                    CpWaitTimer held(CP_WAIT_SUSPENDED);
                    video::WaitForeground(lock);
                }
                else
                    video::WaitForeground(lock);
                auto t1 = std::chrono::steady_clock::now();
                Check(vkQueueSubmit(s_vk->queue, 1, &si, sub.fence), "vkQueueSubmit");
                auto t2 = std::chrono::steady_clock::now();
                s_queueLockUs += std::chrono::duration<double, std::micro>(tl - t0).count();
                s_queueSubmitUs += std::chrono::duration<double, std::micro>(t2 - t1).count();
                s_hitch.submitMs += std::chrono::duration<double, std::milli>((t2 - t0) - (t1 - tl)).count();
            }
            {
                std::lock_guard lock(s_submitMutex);
                sub.inFlight = true;
                sub.submitted = std::chrono::steady_clock::now();
                if (s_inFlight.empty())
                    s_emptyUs += std::chrono::duration<double, std::micro>(sub.submitted - s_emptySince).count();
                s_inFlight.push_back(s_slot);
                timeline::Mark(timeline::Submit, s_slot | std::min<uint32_t>(uint32_t(sub.writes.size()), 0xFFFF) << 8 | uint32_t(why) << 24);
            }
            s_submitCv.notify_all();
            s_slot = (s_slot + 1) % kSlots;
            s_recording = false;
            s_submissionCounter++;
            s_stats.submits++;
            writewatch::Rearm();
        }

        // Completion thread, `sub` (in `slot`) done: its segments' counts into
        // the ring, in guest samples. Never waits: the fence has signalled
        // (MoltenVK makes queries available before that), and WAIT_BIT could
        // hang elsewhere on a query never ended; one not available counts 0.
        void ZpdHarvest(uint32_t slot, const Submission& sub)
        {
            static uint64_t results[kZpdQueries * 2];  // value, availability
            uint32_t n = 0;
            for (const ZpdSegment& s : sub.zpdSegments)
                n = std::max(n, s.query + 1);
            memset(results, 0, size_t(n) * 16);
            VkResult r = vkGetQueryPoolResults(s_dev, s_zpdPool[slot], 0, n, size_t(n) * 16, results, 16,
                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
            const bool read = r == VK_SUCCESS || r == VK_NOT_READY;
            for (const ZpdSegment& s : sub.zpdSegments)
            {
                const bool available = read && results[s.query * 2 + 1] != 0;
                uint64_t raw = available ? results[s.query * 2] : 0;
                if (!available)
                    s_zpdUnavailable++;
                uint64_t guest = zpd::Normalize(raw, s.num, s.den);
                s_zpdResults->Put(s.id, guest, raw);
                ZpdLog("segment %llu: %s%llu host samples x %u/%u = %llu", (unsigned long long)s.id, available ? "" : "not available, ",
                    (unsigned long long)raw, s.num, s.den, (unsigned long long)guest);
            }
        }

        // An END report from its segments' counts, under s_submitMutex (the
        // completion thread; the command processor when nothing is in
        // flight). Stored as the GPU's stores are, around the write watch's
        // guard (opened once for its 8 dwords): only the CPU reads these
        // bytes (GetData), never the GPU, so imported and shadow memory
        // alike, and nothing guards them (GetData polls and takes "not
        // ready").
        void ZpdStore(const ZpdEnd& end)
        {
            uint32_t lost = 0;
            uint64_t raw = 0;
            // A runaway count stays one the game rejects (zpd::EndCount).
            const uint32_t count = zpd::EndCount(s_zpdResults->Sum(end.s0, end.s1, lost, &raw));
            uint8_t* host = s_sharedHost + end.address;
            writewatch::GpuThreadWrite(host, 32, [host, count] {
                zpd::StoreEnd(count, [host](uint32_t offset, uint32_t value) { memcpy(host + offset, &value, 4); });
            });
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - end.queued).count();
            s_zpdStores.stored++;
            s_zpdStores.nonZero += count != 0;
            s_zpdStores.lost += lost;
            s_zpdStores.delaySum += ms;
            s_zpdStores.delayMax = std::max(s_zpdStores.delayMax, ms);
            {
                std::lock_guard lock(s_zpdLatestMutex);
                if (s_zpdLatest.size() >= 64 && !s_zpdLatest.contains(end.address))
                    s_zpdLatest.clear();
                s_zpdLatest[end.address] = { count, end.frame };
            }
            ZpdLog("f%llu END %08X: segments %llu..%llu, %llu host samples: %u stored %.2f ms after its ZPD%s",
                (unsigned long long)end.frame, end.address, (unsigned long long)end.s0, (unsigned long long)end.s1,
                (unsigned long long)raw, count, ms, lost ? std::format(" ({} segments lost)", lost).c_str() : "");
        }

        void CompletionThread()
        {
#ifdef __APPLE__
            pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);  // see the command processor's Worker
#endif
            writewatch::t_gpuThread = true;
            SetHostThreadName("nfsmw-gpu-done");
            hostcpu::LeaveReservedCore();  // started by the command processor
            while (true)
            {
                uint32_t slot;
                {
                    std::unique_lock lock(s_submitMutex);
                    s_submitCv.wait(lock, [] { return !s_inFlight.empty(); });
                    slot = s_inFlight.front();
                }
                Submission& sub = s_submissions[slot];
                // Publish writes as the GPU passes their events; block on the
                // fence once none is left to watch.
                size_t done = 0;
                while (true)
                {
                    bool finished = vkGetFenceStatus(s_dev, sub.fence) == VK_SUCCESS;
                    bool watching = false;
                    {
                        std::lock_guard lock(s_submitMutex);
                        while (!finished && done < sub.writes.size() && sub.writes[done].event &&
                            vkGetEventStatus(s_dev, sub.writes[done].event) == VK_EVENT_SET)
                            { timeline::Mark(timeline::WriteByEvent); RunWrite(sub.writes[done++], 0); }
                        watching = done < sub.writes.size() && sub.writes[done].event;
                    }
                    if (finished)
                        break;
                    if (watching)
                        std::this_thread::sleep_for(std::chrono::microseconds(30));
                    else
                    {
                        // Nothing mid-buffer to watch: wait for the fence, but
                        // wake now and then in case a write arrives with an event.
                        VkResult r = vkWaitForFences(s_dev, 1, &sub.fence, VK_TRUE, 1'000'000);
                        if (r == VK_SUCCESS)
                            break;
                    }
                }
                auto now = std::chrono::steady_clock::now();
                {
                    // NFSMW_LOG_LATENCY=1: submit -> completion-wake latency.
                    static const bool logLatency = std::getenv("NFSMW_LOG_LATENCY") != nullptr;
                    static double sumUs = 0, maxUs = 0;
                    static uint32_t n = 0;
                    static auto window = now;
                    if (logLatency)
                    {
                        double us = std::chrono::duration<double, std::micro>(now - sub.submitted).count();
                        sumUs += us;
                        maxUs = std::max(maxUs, us);
                        n++;
                        if (now - window > std::chrono::seconds(1))
                        {
                            double empty;
                            char writes[160];
                            {
                                std::lock_guard lock(s_submitMutex);
                                empty = s_emptyUs;
                                s_emptyUs = 0;
                                snprintf(writes, sizeof(writes), " | writes: %u by event (avg %.0f us), %u at completion (avg %.0f us)",
                                    s_writeCount[0], s_writeCount[0] ? s_writeDelayUs[0] / s_writeCount[0] : 0.0,
                                    s_writeCount[1], s_writeCount[1] ? s_writeDelayUs[1] / s_writeCount[1] : 0.0);
                                s_writeDelayUs[0] = s_writeDelayUs[1] = 0;
                                s_writeCount[0] = s_writeCount[1] = 0;
                            }
                            double windowUs = std::chrono::duration<double, std::micro>(now - window).count();
                            fprintf(stderr, "[latency] %u submissions/s, submit->done avg %.0f us, max %.0f us, queue empty %.0f%% | by site ring %u submit %u ifidle %u ifdue %u flush %u write %u present %u draw %u copies %u%s | queue lock %.1f ms/s, vkQueueSubmit %.1f ms/s\n",
                                n, sumUs / n, maxUs, 100.0 * empty / windowUs, s_submitWhy[1], s_submitWhy[2], s_submitWhy[3],
                                s_submitWhy[4], s_submitWhy[5], s_submitWhy[6], s_submitWhy[7], s_submitWhy[8], s_submitWhy[9], writes,
                                s_queueLockUs / 1000, s_queueSubmitUs / 1000);
                            s_queueLockUs = s_queueSubmitUs = 0;
                            for (auto& w : s_submitWhy)
                                w = 0;
                            sumUs = maxUs = 0;
                            n = 0;
                            window = now;
                        }
                    }
                }
                auto start = std::max(sub.submitted, s_lastCompletion);
                if (now > start)
                    s_gpuBusyUs += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now - start).count());
                s_lastCompletion = now;
                if (s_timestamps)
                {
                    uint64_t ts[2];
                    if (vkGetQueryPoolResults(s_dev, s_timestamps, slot * 2, 2, sizeof(ts), ts, sizeof(uint64_t),
                            VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ts[1] > ts[0])
                        s_gpuExecNs += uint64_t(double(ts[1] - ts[0]) * s_timestampPeriod);
                }
                if (!sub.zpdSegments.empty())
                    ZpdHarvest(slot, sub);
                {
                    // Writes run under the lock so QueueWrite can't append to
                    // a submission that is being retired.
                    std::lock_guard lock(s_submitMutex);
                    for (; done < sub.writes.size(); done++)
                        RunWrite(sub.writes[done], 1);
                    sub.writes.clear();
                    // Occlusion query reports, in stream order: a query's
                    // last tile, which makes it ready, is stored last.
                    for (const ZpdEnd& end : sub.zpdEnds)
                        ZpdStore(end);
                    sub.zpdEnds.clear();
                    sub.zpdSegments.clear();
                    sub.unsignaled = 0;
                    for (VkEvent event : sub.events)
                    {
                        vkResetEvent(s_dev, event);
                        s_eventPool.push_back(event);
                    }
                    sub.events.clear();
                    writewatch::SetCompleted(sub.number);
                    s_inFlight.pop_front();
                    timeline::Mark(timeline::Complete, slot);
                    if (s_inFlight.empty())
                        s_emptySince = std::chrono::steady_clock::now();
                    sub.inFlight = false;
                }
                s_submitCv.notify_all();
            }
        }

        // Until submission `number` completed (submitting it if it is still
        // being recorded).
        void WaitForSubmission(uint64_t number)
        {
            s_stats.shadowWaits++;
            if (number >= s_submissionCounter)
                SubmitRecorded(9);
            CpWaitTimer wait(CP_WAIT_SHADOW);
            std::unique_lock lock(s_submitMutex);
            s_submitCv.wait(lock, [&] { return s_inFlight.empty() || s_submissions[s_inFlight.front()].number > number; });
        }

        // Shadow mode: bring the GPU's copy of [base, base + size) up to date
        // before recording work that reads it.
        void EnsureShadow(uint32_t base, uint32_t size)
        {
            if (!s_shadowMode || size == 0)
                return;
            base &= 0x1FFFFFFF;
            size = std::min<uint32_t>(size, uint32_t(kSharedSize) - base);
            uint64_t key = uint64_t(base) << 32 | size;
            auto it = s_shadowSynced.find(key);
            if (it != s_shadowSynced.end() && !writewatch::RangeWrittenSince(base, size, it->second))
                return;
            uint64_t sequence = writewatch::Current();
            uint64_t since = it != s_shadowSynced.end() ? it->second : 0;
            HitchTimer hitchTimer(s_hitch.shadowMs);
            while (uint64_t busy = writewatch::SyncShadow(base, size, since))
                WaitForSubmission(busy);
            if (s_shadowSynced.size() > 65536)
                s_shadowSynced.clear();
            s_shadowSynced[key] = sequence;
        }

        // NFSMW_PASS_BARRIER=0|1|2|3 (default 2 with kLighterPasses, else 0): narrower
        // barriers between passes. On MoltenVK a barrier outside a pass is a
        // Metal fence wait per stage (vertex, fragment, compute, copy) of the
        // encoders after it on those before it; a full one makes the next
        // pass's vertex stage wait for everything before, so no pass's vertex
        // work overlaps the last pass's fragments or the resolve between them.
        // 1: a pass ends with its attachment writes (and its shaders' reads)
        // before fragment, compute and transfer work only: no vertex shader
        // reads an attachment (targets reach draws only through resolves).
        // Every pass is followed by a resolve now, so alone it overlaps
        // nothing (a control). 2: a resolve's writes, too, go before those
        // stages only, and a draw whose vertex shader may read what one wrote
        // (vertex or index data in its range, the cached texture it stored
        // into) gets a full barrier first (VertexBarrierIfNeeded). Vertex
        // stages then run ahead through a chain of passes and resolves (the
        // cube faces'). 3: 2 with full pass ends: a pass's vertex stage
        // overlaps only the resolve before it. Texture loads keep full
        // barriers (rare since the write watch's subpages; the ring they
        // stage through is a Metal-tracked index buffer anyway).
        // An M4 Max (race, alternating windows): at 2 and 3 all but one
        // post-resolve vertex stage start early, and the renderer's GPU time
        // drops 0.22-0.34 ms (12-16%); 1 changes nothing. For the iPad to
        // decide.
        // NFSMW_PASS_BARRIER_ALT=1 (A/B): that level in every other 120-frame
        // [perf] window, off in the rest: one run, the same content in both
        // arms. Each window's [barrier] line names the level it ran at.
        // Default 2 with kLighterPasses (Apple: iPad Pro M2 GPU-bound at 2x,
        // same-run windows: executing -0.5 ms), else 0.
        const uint32_t s_passBarrierLevel = [] { const char* v = std::getenv("NFSMW_PASS_BARRIER"); return v ? uint32_t(std::clamp(std::atoi(v), 0, 3)) : kLighterPasses ? 2u : 0u; }();
        const bool s_passBarrierAlt = [] { const char* v = std::getenv("NFSMW_PASS_BARRIER_ALT"); return v && v[0] == '1'; }() && s_passBarrierLevel;
        uint32_t s_passBarrier = s_passBarrierAlt ? 0u : s_passBarrierLevel;
        // Levels 2-3: guest memory resolves wrote since the last full barrier,
        // sorted and merged [base, end) (no vertex stage waits for them yet),
        // their hull for the common miss, and the cached textures (by guest
        // base) they stored into.
        std::vector<std::pair<uint32_t, uint32_t>> s_vertexPending;
        uint32_t s_vertexPendingLo = UINT32_MAX, s_vertexPendingHi = 0;
        std::vector<uint32_t> s_vertexPendingTextures;
        // Pass ends narrowed since the last full barrier. MoltenVK names each
        // stage's fences from a ring of 63 (MVKCommandEncoder::setBarrier):
        // a pass's vertex wait left on an old fragment fence must never come
        // round to the one that pass updates itself, so every 48th is full.
        uint32_t s_narrowPassEnds = 0;
        // The [barrier] line (OnSwap): narrowed pass ends and resolves, full
        // ones forced by the cap, vertex barriers (by reason: vertex data,
        // indices, a texture) and those that ended an open pass.
        struct BarrierCounts { uint64_t passEnds, resolves, capped, vertex[3], splits; };
        BarrierCounts s_barrierCounts{};

        // Everything before -> everything after. Outside rendering only.
        void FullBarrier()
        {
            VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            vkCmdPipelineBarrier(s_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                0, 1, &mb, 0, nullptr, 0, nullptr);
            s_vertexPending.clear();
            s_vertexPendingLo = UINT32_MAX;
            s_vertexPendingHi = 0;
            s_vertexPendingTextures.clear();
            s_narrowPassEnds = 0;
        }

        // `src` stages' work (and, in Vulkan, everything logically earlier)
        // -> every stage after but the vertex ones: on MoltenVK nothing waits
        // before a vertex stage. Outside rendering only.
        void NarrowBarrier(VkPipelineStageFlags src, VkAccessFlags srcAccess)
        {
            VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            mb.srcAccessMask = srcAccess;
            mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            vkCmdPipelineBarrier(s_cmd, src,
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 1, &mb, 0, nullptr, 0, nullptr);
        }

        // At a pass's end. Its attachment writes go before what reads or
        // writes those images next (passes' loads, clears and stores;
        // resolves), its shaders' reads before compute and transfer writes
        // (resolves, texture loads). The vertex stage is named in the source
        // although Vulkan implies it: MoltenVK maps stages one to one, and
        // without it a resolve overwriting what this pass's vertex shader
        // read would wait only for its fragment stage.
        void PassEndBarrier()
        {
            if (s_passBarrier < 1 || s_passBarrier == 3)
                return FullBarrier();
            if (s_narrowPassEnds >= 48)
            {
                s_barrierCounts.capped++;
                return FullBarrier();
            }
            s_narrowPassEnds++;
            s_barrierCounts.passEnds++;
            NarrowBarrier(VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        }

        // After a 1x resolve's dispatch, which wrote guest memory [base, base
        // + size) and, unless `texture` is 0, the texels of the cached texture
        // at that guest base. Levels 2-3 leave the vertex stages out and keep
        // both for VertexBarrierIfNeeded until a full barrier (the front
        // buffer's, once a frame, at least).
        void ResolveBarrier(uint32_t base, uint32_t size, uint32_t texture)
        {
            if (s_passBarrier < 2)
                return FullBarrier();
            s_barrierCounts.resolves++;
            NarrowBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
            if (texture && std::find(s_vertexPendingTextures.begin(), s_vertexPendingTextures.end(), texture) == s_vertexPendingTextures.end())
                s_vertexPendingTextures.push_back(texture);
            if (!size)
                return;
            uint32_t end = base + size;
            s_vertexPendingLo = std::min(s_vertexPendingLo, base);
            s_vertexPendingHi = std::max(s_vertexPendingHi, end);
            // Into the sorted list, merged with what it overlaps or touches.
            auto it = std::lower_bound(s_vertexPending.begin(), s_vertexPending.end(), base,
                [](const std::pair<uint32_t, uint32_t>& r, uint32_t b) { return r.second < b; });
            auto last = it;
            while (last != s_vertexPending.end() && last->first <= end)
            {
                base = std::min(base, last->first);
                end = std::max(end, last->second);
                ++last;
            }
            it = s_vertexPending.erase(it, last);
            s_vertexPending.insert(it, { base, end });
        }

        // As FullBarrier, when the only work since the last one is a compute
        // dispatch: its shader writes before everything after.
        void ComputeBarrier()
        {
            VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            vkCmdPipelineBarrier(s_cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                0, 1, &mb, 0, nullptr, 0, nullptr);
        }

        void ToGeneral(VkImage image, VkImageAspectFlags aspect, uint32_t layers)
        {
            VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            b.srcAccessMask = 0;
            b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = image;
            b.subresourceRange = { aspect, 0, VK_REMAINING_MIP_LEVELS, 0, layers };
            vkCmdPipelineBarrier(s_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                0, 0, nullptr, 0, nullptr, 1, &b);
        }

        // After a resolve or a texture upload, as every 128 draws: submit to a
        // starving GPU (fewer than two submissions in flight) a recording at
        // least NFSMW_STARVE_SUBMIT_US old. Around the shadow maps and the
        // mirror there are few draws but long GPU work, and on the Steam Frame
        // the GPU sat idle beside them. NFSMW_STARVE_AT_COPIES=0 turns it off.
        void SubmitIfStarving()
        {
            static const bool on = [] { const char* v = std::getenv("NFSMW_STARVE_AT_COPIES"); return !v || v[0] != '0'; }();
            static const int64_t starveUs = [] { const char* v = std::getenv("NFSMW_STARVE_SUBMIT_US"); return v ? int64_t(std::atoll(v)) : 1000; }();
            if (!on || !g_eagerWrites || !s_recording)  // (called between passes)
                return;
            if (std::chrono::steady_clock::now() - s_recordStart < std::chrono::microseconds(starveUs))
                return;
            {
                std::lock_guard lock(s_submitMutex);
                if (s_inFlight.size() >= 2)
                    return;
            }
            SubmitRecorded(9);
        }

        // Space in the current submission's ring slot (may submit and move
        // on to the next slot). Returns ~0 if `size` can never fit.
        VkDeviceSize RingAllocate(VkDeviceSize size)
        {
            if (size > kSlotSize)
                return ~VkDeviceSize(0);
            Begin();
            VkDeviceSize offset = (s_ringOffset + 255) & ~VkDeviceSize(255);
            if (offset + size > kSlotSize)
            {
                SubmitRecorded(1);
                Begin();
                offset = 0;
            }
            s_ringOffset = offset + size;
            return VkDeviceSize(s_slot) * kSlotSize + offset;
        }

        // ---------------------------------------------------------------
        // Shaders

        // A translated shader. Its SPIR-V is compiled when a pipeline first
        // needs it, on a pipeline worker (compileMutex guards the modules).
        struct Shader
        {
            ShaderStage stage;
            uint64_t hash;
            TranslatedShader info;         // immutable once loaded
            std::mutex compileMutex;
            bool vertexCompiled = false;
            VkShaderModule module = VK_NULL_HANDLE;  // VS
            // PS variants by (colour outputs, interpolator inputs, alpha test).
            std::unordered_map<uint32_t, VkShaderModule> variants;
        };
        std::unordered_map<uint64_t, std::unique_ptr<Shader>> s_shaders;
        Shader* s_vs = nullptr;
        Shader* s_ps = nullptr;

        // IM_LOAD shaders by where they are (LoadShader). A dense frame loads
        // thousands, the same few hundred from the same addresses, and
        // finding each by a hash of all its dwords (FNV-1a, a latency-bound
        // multiply chain) was 2-2.7% of the command processor (Deck
        // profile). An entry keeps the words it was made from and is used
        // only while memory holds exactly those words: a compare, many times
        // cheaper than the hash, and the shader the hash would find.
        // NFSMW_SHADER_CACHE=0 turns it off; NFSMW_CP_CACHE_VERIFY=1 also
        // hashes on every hit and logs any difference.
        struct ShaderCacheEntry
        {
            const uint32_t* words = nullptr;  // where they were loaded from (the key)
            uint32_t dwords = 0, type = 0;
            Shader* shader = nullptr;
            std::vector<uint32_t> copy;  // the words, as loaded (guest order)
        };
        struct ShaderCache
        {
            ShaderCacheEntry entries[1024];
            bool on = false, verify = false;
            uint64_t loads = 0, hits = 0, comparedBytes = 0, mismatches = 0;
        };
        ShaderCache* const s_shaderCache = [] {
            auto* c = new ShaderCache();
            const char* v = std::getenv("NFSMW_SHADER_CACHE");
            c->on = (g_cpOpt & CP_OPT_SHADER_CACHE) && !(v && v[0] == '0');
            c->verify = std::getenv("NFSMW_CP_CACHE_VERIFY") != nullptr;
            return c;
        }();

        // Compiled shaders are cached on disk (GetCachePath()/spirv), keyed by
        // their exact source: new shaders compile once (10-20 ms each on the
        // Steam Machine, a hitch whenever new content appears), then load.
        // NFSMW_SHADER_CACHE=0 disables it.
        const bool s_shaderCacheEnabled = [] { const char* v = std::getenv("NFSMW_SHADER_CACHE"); return !v || v[0] != '0'; }();

        std::filesystem::path ShaderCacheFile(const std::string& source, glslang_stage_t stage, const std::string& preamble)
        {
            uint64_t h = 0xCBF29CE484222325ull;
            auto mix = [&](const void* data, size_t size) {
                for (size_t i = 0; i < size; i++)
                    h = (h ^ static_cast<const uint8_t*>(data)[i]) * 0x100000001B3ull;
            };
            const uint32_t format = 1;  // bump when the compile options below change
            mix(&format, sizeof(format));
            mix(&stage, sizeof(stage));
            mix(preamble.data(), preamble.size());
            mix("\0", 1);
            mix(source.data(), source.size());
            return GetCachePath() / "spirv" / std::format("{:016x}.spv", h);
        }

        std::vector<uint32_t> CompileGlslUncached(const std::string& source, glslang_stage_t stage, std::string& log,
            const std::string& preamble);
        std::atomic<uint64_t> s_shaderCacheHits{ 0 };

        std::vector<uint32_t> CompileGlsl(const std::string& source, glslang_stage_t stage, std::string& log,
            const std::string& shaderPreamble = {})
        {
            const std::string preamble = s_splitMemory ? "#define SPLIT_MEMORY\n" + shaderPreamble : shaderPreamble;
            if (!s_shaderCacheEnabled)
                return CompileGlslUncached(source, stage, log, preamble);
            std::filesystem::path file = ShaderCacheFile(source, stage, preamble);
            if (FILE* f = fopen(file.c_str(), "rb"))
            {
                std::vector<uint32_t> spirv;
                fseek(f, 0, SEEK_END);
                long size = ftell(f);
                fseek(f, 0, SEEK_SET);
                if (size >= 20 && size % 4 == 0)
                {
                    spirv.resize(size_t(size) / 4);
                    if (fread(spirv.data(), 4, spirv.size(), f) != spirv.size() || spirv[0] != 0x07230203u)
                        spirv.clear();
                }
                fclose(f);
                if (!spirv.empty())
                {
                    s_shaderCacheHits.fetch_add(1, std::memory_order_relaxed);
                    return spirv;
                }
            }
            std::vector<uint32_t> spirv = CompileGlslUncached(source, stage, log, preamble);
            if (!spirv.empty())
            {
                std::error_code ec;
                std::filesystem::create_directories(file.parent_path(), ec);
                // Unique per thread: two workers may compile the same source.
                std::filesystem::path temp = file;
                temp += std::format(".{}.tmp", std::hash<std::thread::id>{}(std::this_thread::get_id()));
                if (FILE* f = fopen(temp.c_str(), "wb"))
                {
                    bool ok = fwrite(spirv.data(), 4, spirv.size(), f) == spirv.size();
                    ok = fclose(f) == 0 && ok;
                    if (ok)
                        std::filesystem::rename(temp, file, ec);
                    else
                        std::filesystem::remove(temp, ec);
                }
            }
            return spirv;
        }

        std::vector<uint32_t> CompileGlslUncached(const std::string& source, glslang_stage_t stage, std::string& log,
            const std::string& preamble)
        {
            glslang_input_t input{};
            input.language = GLSLANG_SOURCE_GLSL;
            input.stage = stage;
            input.client = GLSLANG_CLIENT_VULKAN;
            input.client_version = GLSLANG_TARGET_VULKAN_1_2;
            input.target_language = GLSLANG_TARGET_SPV;
            input.target_language_version = GLSLANG_TARGET_SPV_1_5;
            input.code = source.c_str();
            input.default_version = 460;
            input.default_profile = GLSLANG_NO_PROFILE;
            input.messages = glslang_messages_t(GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT);
            input.resource = glslang_default_resource();

            std::vector<uint32_t> spirv;
            glslang_shader_t* shader = glslang_shader_create(&input);
            if (!preamble.empty())
                glslang_shader_set_preamble(shader, preamble.c_str());
            if (!glslang_shader_preprocess(shader, &input) || !glslang_shader_parse(shader, &input))
            {
                log = glslang_shader_get_info_log(shader);
                glslang_shader_delete(shader);
                return spirv;
            }
            glslang_program_t* program = glslang_program_create();
            glslang_program_add_shader(program, shader);
            if (!glslang_program_link(program, int(GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT)))
            {
                log = glslang_program_get_info_log(program);
            }
            else
            {
                glslang_program_SPIRV_generate(program, stage);
                spirv.resize(glslang_program_SPIRV_get_size(program));
                glslang_program_SPIRV_get(program, spirv.data());
            }
            glslang_program_delete(program);
            glslang_shader_delete(shader);
            return spirv;
        }

        // Vulkan pipeline cache, persisted as GetCachePath()/pipelines.bin (the
        // driver validates the header: another GPU or driver version starts
        // empty). Saved every 600 frames when new pipelines appeared, and at exit.
        VkPipelineCache s_pipelineCache = VK_NULL_HANDLE;
        uint32_t s_pipelinesAtSave = 0;

        void CreatePipelineCache()
        {
            std::vector<uint8_t> data;
            if (s_shaderCacheEnabled)
                if (FILE* f = fopen((GetCachePath() / "pipelines.bin").c_str(), "rb"))
                {
                    fseek(f, 0, SEEK_END);
                    long size = ftell(f);
                    fseek(f, 0, SEEK_SET);
                    if (size > 0)
                    {
                        data.resize(size_t(size));
                        if (fread(data.data(), 1, data.size(), f) != data.size())
                            data.clear();
                    }
                    fclose(f);
                }
            VkPipelineCacheCreateInfo pcci{ VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
            pcci.initialDataSize = data.size();
            pcci.pInitialData = data.empty() ? nullptr : data.data();
            if (vkCreatePipelineCache(s_dev, &pcci, nullptr, &s_pipelineCache) != VK_SUCCESS)
            {
                pcci.initialDataSize = 0;
                pcci.pInitialData = nullptr;
                Check(vkCreatePipelineCache(s_dev, &pcci, nullptr, &s_pipelineCache), "vkCreatePipelineCache");
            }
            fprintf(stderr, "[renderer] pipeline cache: %zu KB loaded\n", data.size() / 1024);
        }

        // Saves are serialized (the background saver and the one at exit).
        std::mutex s_pipelineSaveMutex;

        void SavePipelineCache()
        {
            if (!s_pipelineCache || !s_shaderCacheEnabled)
                return;
            std::lock_guard saveLock(s_pipelineSaveMutex);
            // Workers may add pipelines between the two calls (VK_INCOMPLETE).
            size_t size = 0;
            std::vector<uint8_t> data;
            for (int attempt = 0;; attempt++)
            {
                if (vkGetPipelineCacheData(s_dev, s_pipelineCache, &size, nullptr) != VK_SUCCESS || size == 0)
                    return;
                data.resize(size);
                VkResult r = vkGetPipelineCacheData(s_dev, s_pipelineCache, &size, data.data());
                if (r == VK_SUCCESS)
                    break;
                if (r != VK_INCOMPLETE || attempt == 3)
                    return;
            }
            std::error_code ec;
            std::filesystem::create_directories(GetCachePath(), ec);
            std::filesystem::path file = GetCachePath() / "pipelines.bin", temp = file;
            temp += ".tmp";
            if (FILE* f = fopen(temp.c_str(), "wb"))
            {
                bool ok = fwrite(data.data(), 1, size, f) == size;
                ok = fclose(f) == 0 && ok;
                if (ok)
                    std::filesystem::rename(temp, file, ec);
                else
                    std::filesystem::remove(temp, ec);
            }
        }

        VkShaderModule CreateModule(const std::vector<uint32_t>& spirv)
        {
            VkShaderModuleCreateInfo smci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
            smci.codeSize = spirv.size() * 4;
            smci.pCode = spirv.data();
            VkShaderModule module;
            Check(vkCreateShaderModule(s_dev, &smci, nullptr, &module), "vkCreateShaderModule");
            return module;
        }

        // A pixel shader variant: colour outputs only for bound attachments
        // (Metal rejects others), inputs only for interpolators the vertex
        // shader writes, and the alpha test compiled in only when enabled.
        VkShaderModule VertexModule(Shader& vs)
        {
            std::lock_guard lock(vs.compileMutex);
            if (!vs.vertexCompiled)
            {
                vs.vertexCompiled = true;
                std::string log;
                auto spirv = CompileGlsl(vs.info.glsl, GLSLANG_STAGE_VERTEX, log);
                if (spirv.empty())
                    fprintf(stderr, "[renderer] vs_%016llx: GLSL compile failed:\n%s\n", (unsigned long long)vs.hash, log.c_str());
                else
                    vs.module = CreateModule(spirv);
            }
            return vs.module;
        }

        VkShaderModule PixelVariant(Shader& ps, uint32_t outputs, uint32_t inputs, bool alphaTest)
        {
            uint32_t key = (outputs & 0xF) | ((inputs & 0xFFFF) << 4) | (alphaTest ? 1u << 20 : 0);
            std::lock_guard lock(ps.compileMutex);
            auto it = ps.variants.find(key);
            if (it != ps.variants.end())
                return it->second;
            std::string preamble;
            for (int i = 0; i < 4; i++)
                if (outputs & (1u << i))
                    preamble += std::format("#define OUT{}\n", i);
            for (int i = 0; i < 16; i++)
                if (inputs & (1u << i))
                    preamble += std::format("#define IN{}\n", i);
            if (alphaTest)
                preamble += "#define ALPHA_TEST\n";
            std::string log;
            auto spirv = CompileGlsl(ps.info.glsl, GLSLANG_STAGE_FRAGMENT, log, preamble);
            VkShaderModule module = VK_NULL_HANDLE;
            if (spirv.empty())
                fprintf(stderr, "[renderer] ps_%016llx (variant %x): GLSL compile failed:\n%s\n",
                    (unsigned long long)ps.hash, key, log.c_str());
            else
                module = CreateModule(spirv);
            ps.variants[key] = module;
            return module;
        }

        // ---------------------------------------------------------------
        // Layouts: set 0 = shared memory + constants (one static set),
        // set 1 = the textures a shader pair uses (push descriptors).

        VkDescriptorSetLayout s_set0Layout = VK_NULL_HANDLE;
        VkDescriptorPool s_descriptorPool = VK_NULL_HANDLE;
        VkDescriptorSet s_set0 = VK_NULL_HANDLE;
        VkSampler s_sampler = VK_NULL_HANDLE;
        Image s_dummy[3];  // 2D, 3D, cube: until the texture cache (M4)
        Image s_dummyStorage;  // R32_UINT 1x1: a resolve's texture binding when it stores into none

        struct DrawConstants
        {
            uint32_t vsConstBase, psConstBase, indexAddress, indexInfo, indexOffset, alphaTest;
            float alphaRef;
            uint32_t flags;
            float posScale[4];
            uint32_t vfRedirect[4][4];  // guest start, size, ring offset, 0 (vertex data snapshots)
        };
        static_assert(sizeof(DrawConstants) == 112);

        struct TextureLayout
        {
            VkDescriptorSetLayout set = VK_NULL_HANDLE;
            VkPipelineLayout pipeline = VK_NULL_HANDLE;
            std::array<uint32_t, 3> vsMasks{}, psMasks{};
        };
        std::map<std::array<uint32_t, 6>, std::unique_ptr<TextureLayout>> s_textureLayouts;

        const TextureLayout& GetTextureLayoutUncached(const Shader& vs, const Shader& ps);

        const TextureLayout& GetTextureLayout(const Shader& vs, const Shader& ps)
        {
            static const Shader *lastVs = nullptr, *lastPs = nullptr;
            static const TextureLayout* last = nullptr;
            if (&vs == lastVs && &ps == lastPs)
                return *last;
            lastVs = &vs;
            lastPs = &ps;
            last = &GetTextureLayoutUncached(vs, ps);
            return *last;
        }

        const TextureLayout& GetTextureLayoutUncached(const Shader& vs, const Shader& ps)
        {
            std::array<uint32_t, 3> v = { vs.info.texture2DMask, vs.info.texture3DMask, vs.info.textureCubeMask };
            std::array<uint32_t, 3> p = { ps.info.texture2DMask, ps.info.texture3DMask, ps.info.textureCubeMask };
            std::array<uint32_t, 6> key = { v[0], v[1], v[2], p[0], p[1], p[2] };
            auto& slot = s_textureLayouts[key];
            if (slot)
                return *slot;
            slot = std::make_unique<TextureLayout>();
            slot->vsMasks = v;
            slot->psMasks = p;
            std::vector<VkDescriptorSetLayoutBinding> bindings;
            for (uint32_t d = 0; d < 3; d++)
                for (uint32_t n = 0; n < 32; n++)
                {
                    VkShaderStageFlags stages = ((v[d] >> n) & 1 ? VK_SHADER_STAGE_VERTEX_BIT : 0) |
                        ((p[d] >> n) & 1 ? VK_SHADER_STAGE_FRAGMENT_BIT : 0);
                    if (stages)
                        bindings.push_back({ d * 32 + n, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, stages, nullptr });
                }
            VkDescriptorSetLayoutCreateInfo dslci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            dslci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
            dslci.bindingCount = uint32_t(bindings.size());
            dslci.pBindings = bindings.data();
            Check(vkCreateDescriptorSetLayout(s_dev, &dslci, nullptr, &slot->set), "vkCreateDescriptorSetLayout(textures)");
            VkDescriptorSetLayout sets[] = { s_set0Layout, slot->set };
            VkPushConstantRange range{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(DrawConstants) };
            VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            plci.setLayoutCount = 2;
            plci.pSetLayouts = sets;
            plci.pushConstantRangeCount = 1;
            plci.pPushConstantRanges = &range;
            Check(vkCreatePipelineLayout(s_dev, &plci, nullptr, &slot->pipeline), "vkCreatePipelineLayout");
            return *slot;
        }

        // ---------------------------------------------------------------
        // Render targets (EDRAM)

        struct RenderTarget
        {
            uint32_t base, format, pitch;
            bool depth;
            bool wide;                         // side-by-side tiles drawn untiled (see GetTarget)
            uint32_t width, height;            // host pixels (guest x scale)
            uint32_t guestWidth, guestHeight;  // guest pixels
            uint32_t scaleX = 1, scaleY = 1;
            VkFormat vkFormat;
            bool blendable;
            Image image;
            VkImageView sampleView = VK_NULL_HANDLE;  // depth aspect only, for resolves
            // NFSMW_UNTILED_AREA: in the last frame untiled draws drew it (until
            // the next one's first), whether one was cut short of the
            // attachments' common size, what those cut draws drew (their host
            // scissors' union; one cut to nothing adds nothing) and that
            // common size; and whether a resolve has read past them (its
            // draws then keep the whole target).
            uint64_t areaFrame = ~0ull;
            VkRect2D cutDrawn{};
            VkExtent2D cutReach{};
            bool cut = false;
            bool wholeArea = false;
        };
        std::unordered_map<uint64_t, std::unique_ptr<RenderTarget>> s_targets;

        VkFormat ColorFormat(uint32_t format)
        {
            switch (xenos::ColorRenderTargetFormat(format))
            {
            case xenos::ColorRenderTargetFormat::k_8_8_8_8:
            case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: return VK_FORMAT_R8G8B8A8_UNORM;
            case xenos::ColorRenderTargetFormat::k_2_10_10_10:
            case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
            case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
            case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
            case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
            case xenos::ColorRenderTargetFormat::k_16_16: return VK_FORMAT_R16G16_SNORM;
            case xenos::ColorRenderTargetFormat::k_16_16_16_16: return VK_FORMAT_R16G16B16A16_SNORM;
            case xenos::ColorRenderTargetFormat::k_16_16_FLOAT: return VK_FORMAT_R16G16_SFLOAT;
            case xenos::ColorRenderTargetFormat::k_32_FLOAT: return VK_FORMAT_R32_SFLOAT;
            case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
            default: return VK_FORMAT_R8G8B8A8_UNORM;
            }
        }

        bool Is64bpp(uint32_t format)
        {
            switch (xenos::ColorRenderTargetFormat(format))
            {
            case xenos::ColorRenderTargetFormat::k_16_16_16_16:
            case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
            case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
            case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: return true;
            default: return false;
            }
        }

        // Untiled rendering (see TileShift); NFSMW_TILING=1 renders per tile.
        const bool s_untiled = [] { const char* v = std::getenv("NFSMW_TILING"); return !(v && v[0] == '1'); }();
        // NFSMW_UNTILED_AREA=1 (default with kLighterPasses): an untiled draw with clipping on
        // is scissored to its guest viewport, not the whole EDRAM-sized
        // target, so its pass's area (what a tile GPU loads and stores:
        // MoltenVK's render target runs from (0,0) to the area's far corner)
        // is the frame: the scene's 1280x720, not 1280x1024, and a cube
        // face's 256x256, not 1280x896. ~130 MB a race frame less (modelled:
        // the area's far corner x the attachments' bytes, loaded and stored);
        // an M4 Max measured -0.09 ms, a phone is unmeasured. The console
        // never draws past the viewport there (its window scissor is the
        // tile inside it); the host clipped only to the target, so the road
        // below the frame and the neighbouring faces' geometry were shaded
        // too. Clipping off, or x and y not both scaled and offset: the whole
        // target, as before. Where cutting would change more than the area,
        // the target falls back to the whole target from then on
        // (RenderTarget::wholeArea), logged: a resolve reading past what its
        // cut draws drew (rows the host used to overdraw; see Resolve), a
        // draw that would restart its pass for its area (no untiled draw
        // did), two cuts whose union isn't exact.
        // =alt (measurement only): on in every other 120-frame [perf] window,
        // a paired A/B within one run.
        // NFSMW_LOG_UNTILED_AREA=1: counts with each [perf] line (draws cut,
        // pass restarts, the passes' area and bytes), and each distinct cut
        // and each shader pair keeping the whole target, once.
        // On by default with kLighterPasses (Apple: iPad Pro M2 GPU-bound at
        // 2x, same-run windows: executing -1.0 ms), else off; =0 turns it off.
        const uint32_t s_untiledArea = [] {
            const char* v = std::getenv("NFSMW_UNTILED_AREA");
            return !v ? (kLighterPasses ? 1u : 0u) : v[0] == '1' ? 1u : strcmp(v, "alt") == 0 ? 2u : 0u;
        }();
        const bool s_logUntiledArea = [] { const char* v = std::getenv("NFSMW_LOG_UNTILED_AREA"); return v && v[0] == '1'; }();
        struct UntiledAreaCounts
        {
            uint64_t cut = 0, whole = 0, clipOff = 0, noViewport = 0, wholeArea = 0, empty = 0, restarts = 0;
            uint64_t passes = 0, passPixels = 0, passBytes = 0;                // every pass: its area, its attachments' bytes there
            uint64_t untiledPasses = 0, untiledPixels = 0, untiledBytes = 0;  // passes on wide or MSAA-tiled targets
            uint64_t resolvesPast = 0;
        } s_ua;

        // Internal resolution. Render targets of the scene's surface pitches
        // (NFSMW_SCALE_PITCH, default 640-1280: the scene, its side-by-side
        // tiles, the road reflection, the final composite) are created at
        // guest size x (sx, sy) host pixels; the rest (the 1600x1600 shadow
        // map, the 256x256 cube faces, the bloom and luminance chain from
        // 320x180 down) stay 1x. The bloom blurs step in texels: at scale
        // they cover a fraction of their area and the game's haze thins (the
        // colour grade visibly changed with 320 scaled), and they are blurry
        // by design anyway. Draws keep the guest's viewport transform in guest pixels
        // and scale only the host viewport and scissor; resolves average each
        // sx x sy block down into guest memory (depth takes one sample), so
        // the game and the texture cache see 1x data as before.
        // NFSMW_RENDER_SCALE=<n> or <sx>x<sy> (1-4; default 1).
        // The setting applies live (ApplyLiveSettings): every scaled target,
        // the scaled resolve memory and the scaled textures are made again at
        // the new scale between two frames, and Auto follows the picture's
        // size (the window, fullscreen, the aspect ratio setting).
        uint32_t s_renderScaleX = 1, s_renderScaleY = 1;
        uint32_t s_scalePitchMin = 640, s_scalePitchMax = 1280;
        uint32_t s_scaleBudget = 6;  // Auto's most host pixels per guest pixel
        bool s_scaleFixed = false;   // NFSMW_RENDER_SCALE: no live changes

        // What the Internal Resolution setting asks for with the picture
        // covering `regionW` x `regionH` of the swapchain.
        void WantedRenderScale(int32_t setting, uint32_t regionW, uint32_t regionH, uint32_t& sx, uint32_t& sy)
        {
            if (setting > 0)
            {
                sx = sy = uint32_t(std::clamp(setting, 1, 3));
                return;
            }
            // Auto: enough host pixels to cover the screen area the frame
            // fills (video/picture_fit: 3440x1440 filled: 3x2; a 1440p 16:9
            // box: 2x2; a Steam Deck's 1280x800 filled with Vert+: 1x1),
            // within a budget of sx x sy host pixels per guest pixel: 6 on a
            // discrete GPU, 4 on an integrated one (the M1 Pro holds a race
            // at 2x2), 9 on a Mac with an M3 or later Max or Ultra.
            video::AutoRenderScale(regionW, regionH, s_scaleBudget, sx, sy);
        }

        std::string ScaleReason(int32_t setting, uint32_t regionW, uint32_t regionH)
        {
            if (s_scaleFixed)
                return "NFSMW_RENDER_SCALE";
            if (setting > 0)
                return "setting";
            return std::format("auto for a {}x{} picture", regionW, regionH);
        }

        void ReadRenderScale(const VkPhysicalDeviceProperties& props)
        {
            // Settings > Video > Internal Resolution; NFSMW_RENDER_SCALE
            // (<n> or <sx>x<sy>, developer override) wins.
            s_scaleBudget = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 6 : 4;
#if defined(__APPLE__) && TARGET_OS_IOS
            // iPhones and iPads hold a race at 60 fps at 1x only. Scripted
            // drive, GPU time a frame: iPhone 15 Pro Max (A17 Pro) 15 ms at
            // 1x, 36-40 at 2x; iPad Pro M2 20 ms at 2x (43-50 fps).
            s_scaleBudget = 1;
#elif defined(__APPLE__)
            // Apple GPUs are all integrated, but a recent Max or Ultra has
            // the headroom for 3x3 (4K). Scripted race, GPU time a frame, M4
            // Max: 5-6.6 ms at 2x2, 9-11 ms at 3x3, 60 fps locked.
            // An M1 or M2 Max has about half that GPU (its 3x3 would be ~20
            // ms): from the M3 on only. MoltenVK names the device after the
            // Metal one ("Apple M4 Max").
            std::string_view name = props.deviceName;
            if (name.starts_with("Apple M") && (name.ends_with(" Max") || name.ends_with(" Ultra")))
            {
                unsigned generation = 0;
                for (char c : name.substr(7))
                {
                    if (c < '0' || c > '9')
                        break;
                    generation = generation * 10 + unsigned(c - '0');
                }
                if (generation >= 3)
                    s_scaleBudget = 9;
            }
#endif
            int32_t setting = settings::GetInt(settings::Id::InternalResolution);
            uint32_t regionW = 0, regionH = 0;
            if (const char* v = std::getenv("NFSMW_RENDER_SCALE"))
            {
                unsigned x = 1, y = 0;
                if (sscanf(v, "%ux%u", &x, &y) < 2)
                    y = x;
                s_renderScaleX = std::clamp(x, 1u, 4u);
                s_renderScaleY = std::clamp(y, 1u, 4u);
                s_scaleFixed = true;
            }
            else
            {
                video::FrameRegion(regionW, regionH);
                WantedRenderScale(setting, regionW, regionH, s_renderScaleX, s_renderScaleY);
            }
            if (const char* v = std::getenv("NFSMW_SCALE_PITCH"))
                sscanf(v, "%u-%u", &s_scalePitchMin, &s_scalePitchMax);
            fprintf(stderr, "[renderer] internal resolution %ux%u (%ux%u, %s; surface pitches %u-%u)\n", s_renderScaleX, s_renderScaleY,
                1280 * s_renderScaleX, 720 * s_renderScaleY, ScaleReason(setting, regionW, regionH).c_str(), s_scalePitchMin, s_scalePitchMax);
        }

        bool ScaledPitch(uint32_t pitch)
        {
            return pitch >= s_scalePitchMin && pitch <= s_scalePitchMax;
        }

        // Untiled rendering draws tile 0 over the whole frame (TileShift).
        // With tiles side by side (NFSMW switches to two 640-wide tiles in
        // some scenes) the frame is wider than the surface pitch: those
        // targets get the full 720p width.
        constexpr uint32_t kUntiledWidth = 1280;

        // ~5 lookups a draw: a small direct-mapped cache before the map
        // (targets are freed only when the internal resolution changes,
        // which clears it).
        struct RecentTarget { uint64_t key = ~0ull; RenderTarget* target = nullptr; };
        RecentTarget s_recentTargets[16];

        // A new target, at the current scale if its pitch is a scaled one,
        // cleared to zero (new EDRAM contents).
        std::unique_ptr<RenderTarget> CreateTarget(uint32_t base, uint32_t format, bool depth, uint32_t pitch, bool wide)
        {
            // Height: whatever fits in EDRAM (2048 tiles of 80x16 32-bit
            // samples) from this base, capped at the 2048-line maximum.
            uint32_t tilesPerRow = std::max(1u, (pitch + 79) / 80) * (!depth && Is64bpp(format) ? 2 : 1);
            uint32_t rows = std::clamp((2048u - std::min(base, 2047u)) / tilesPerRow * 16, 16u, 2048u);

            auto rt = std::make_unique<RenderTarget>();
            rt->base = base;
            rt->format = format;
            rt->pitch = pitch;
            rt->depth = depth;
            rt->wide = wide;
            rt->guestWidth = wide ? kUntiledWidth : pitch;
            rt->guestHeight = rows;
            if (ScaledPitch(pitch))
            {
                rt->scaleX = s_renderScaleX;
                rt->scaleY = s_renderScaleY;
            }
            rt->width = rt->guestWidth * rt->scaleX;
            rt->height = rt->guestHeight * rt->scaleY;
            rt->vkFormat = depth ? VK_FORMAT_D32_SFLOAT_S8_UINT : ColorFormat(format);
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(s_vk->physical, rt->vkFormat, &fp);
            rt->blendable = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) != 0;
            VkImageAspectFlags aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
            VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
            rt->image = CreateImage(VK_IMAGE_TYPE_2D, rt->vkFormat, { rt->width, rt->height, 1 }, 1, usage,
                VK_IMAGE_VIEW_TYPE_2D, aspect);
            if (depth)
            {
                VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
                vci.image = rt->image.image;
                vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
                vci.format = rt->vkFormat;
                vci.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
                Check(vkCreateImageView(s_dev, &vci, nullptr, &rt->sampleView), "vkCreateImageView(depth)");
            }
            else
            {
                rt->sampleView = rt->image.view;
            }

            // New EDRAM contents: zero.
            EndPass();
            Begin();
            ToGeneral(rt->image.image, aspect, 1);
            VkImageSubresourceRange range{ aspect, 0, 1, 0, 1 };
            if (depth)
            {
                VkClearDepthStencilValue v{ 0.0f, 0 };
                vkCmdClearDepthStencilImage(s_cmd, rt->image.image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &range);
            }
            else
            {
                VkClearColorValue v{};
                vkCmdClearColorImage(s_cmd, rt->image.image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &range);
            }
            FullBarrier();

            fprintf(stderr, "[renderer] EDRAM target: base %u %s format %u pitch %u -> %ux%u%s\n",
                base, depth ? "depth" : "color", format, pitch, rt->guestWidth, rt->guestHeight,
                rt->scaleX * rt->scaleY > 1 ? std::format(" (host {}x{})", rt->width, rt->height).c_str() : "");
            return rt;
        }

        void DestroyTarget(RenderTarget& rt)
        {
            if (rt.sampleView && rt.sampleView != rt.image.view)
                vkDestroyImageView(s_dev, rt.sampleView, nullptr);
            rt.sampleView = VK_NULL_HANDLE;
            DestroyImage(rt.image);
        }

        RenderTarget* GetTarget(uint32_t base, uint32_t format, bool depth, uint32_t pitch)
        {
            bool wide = s_untiled && Reg<reg::RB_SURFACE_INFO>().msaa_samples != xenos::MsaaSamples::k1X &&
                Reg<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable && pitch < kUntiledWidth;
            uint64_t key = uint64_t(base) | (uint64_t(format) << 12) | (uint64_t(depth) << 16) | (uint64_t(pitch) << 17) |
                (uint64_t(wide) << 31);
            RecentTarget& hit = s_recentTargets[(key ^ (key >> 12) ^ (key >> 17)) & 15];
            if (hit.key == key)
                return hit.target;
            auto& slot = s_targets[key];
            if (!slot)
                slot = CreateTarget(base, format, depth, pitch, wide);
            hit = { key, slot.get() };
            return slot.get();
        }

        // Current dynamic rendering pass.
        RenderTarget* s_passColor[4] = {};
        RenderTarget* s_passDepth = nullptr;
        bool s_passActive = false;
        VkExtent2D s_passExtent{};  // the attachments' common size
        VkRect2D s_passArea{};      // what this pass loads and stores (tile GPUs move only this)
        // NFSMW_PASS_MERGE bitmask (Draw; default 3 with kFewerPasses, else
        // 0), fewer passes around the sun: 1 skip draws that write nothing (its
        // occlusion quad), 2 draw into the open pass a draw whose targets are
        // some of its (its glow). In every view with the sun in it (the
        // mirror, up to three cube faces, the scene) the pair split the
        // view's pass in three: 10 of the 46 passes of an iPad race frame's
        // profile. Expected ~0.4-0.5 ms of its GPU time (bit 1 ~0.2, bit 2
        // ~0.15-0.25: the glow and flare draws still run).
        const uint32_t s_passMerge = [] { const char* v = std::getenv("NFSMW_PASS_MERGE"); return v ? uint32_t(std::atoi(v)) : kFewerPasses ? 3u : 0u; }();

        void EndPass()
        {
            if (!s_passActive)
                return;
            ZpdEndSegment();  // a segment never outlives its pass
            s_vk->cmdEndRendering(s_cmd);
            s_passActive = false;
            if (s_profiling && !s_passInfos.empty() && s_passInfos.back().query + 1 < s_passQueryNext)
                vkCmdWriteTimestamp(s_cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s_passQueries, s_passInfos.back().query + 1);
            PassEndBarrier();
            SignalWrites();
        }

        bool Contains(const VkRect2D& outer, const VkRect2D& inner)
        {
            return inner.offset.x >= outer.offset.x && inner.offset.y >= outer.offset.y &&
                inner.offset.x + int32_t(inner.extent.width) <= outer.offset.x + int32_t(outer.extent.width) &&
                inner.offset.y + int32_t(inner.extent.height) <= outer.offset.y + int32_t(outer.extent.height);
        }

        VkRect2D Union(const VkRect2D& a, const VkRect2D& b)
        {
            int32_t x0 = std::min(a.offset.x, b.offset.x), y0 = std::min(a.offset.y, b.offset.y);
            int32_t x1 = std::max(a.offset.x + int32_t(a.extent.width), b.offset.x + int32_t(b.extent.width));
            int32_t y1 = std::max(a.offset.y + int32_t(a.extent.height), b.offset.y + int32_t(b.extent.height));
            return { { x0, y0 }, { uint32_t(x1 - x0), uint32_t(y1 - y0) } };
        }

        bool Overlaps(const VkRect2D& a, const VkRect2D& b)
        {
            return a.offset.x < b.offset.x + int32_t(b.extent.width) && b.offset.x < a.offset.x + int32_t(a.extent.width) &&
                a.offset.y < b.offset.y + int32_t(b.extent.height) && b.offset.y < a.offset.y + int32_t(a.extent.height);
        }

        // Stacked or side by side with a whole edge in common: their union is
        // exactly the two.
        bool Adjoins(const VkRect2D& a, const VkRect2D& b)
        {
            int32_t ax1 = a.offset.x + int32_t(a.extent.width), ay1 = a.offset.y + int32_t(a.extent.height);
            int32_t bx1 = b.offset.x + int32_t(b.extent.width), by1 = b.offset.y + int32_t(b.extent.height);
            if (a.offset.x == b.offset.x && ax1 == bx1)
                return ay1 == b.offset.y || by1 == a.offset.y;
            if (a.offset.y == b.offset.y && ay1 == by1)
                return ax1 == b.offset.x || bx1 == a.offset.x;
            return false;
        }

        // `area` (in target pixels) is what the next draw or clear touches; a
        // pass on the same targets is kept while it covers that, else it is
        // restarted over the union.
        // Resolve clears are deferred into the next pass on their target (a
        // pass of their own would load and store the attachments on a tile
        // GPU): a load-op clear when they cover the render area, else an
        // in-pass clear.
        // Untiled rendering of predicated tiling. At 720p with 4x MSAA the
        // game renders the scene once per EDRAM tile, 3 tiles of 256 rows
        // (window offsets 0, -256, -512), and resolves each tile. The host
        // has no EDRAM limit (and renders at 1x), so tile 0's draws cover the
        // whole target, later tiles' draws are skipped, and each tile's
        // resolve and clear read the target at screen coordinates (EDRAM
        // rectangle minus the window offset). NFSMW_TILING=1 keeps the
        // per-tile rendering.

        // The window offset to undo for the current MSAA surface, or 0,0.
        std::pair<int32_t, int32_t> TileShift()
        {
            if (!s_untiled || Reg<reg::RB_SURFACE_INFO>().msaa_samples == xenos::MsaaSamples::k1X ||
                !Reg<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable)
                return { 0, 0 };
            auto wo = Reg<reg::PA_SC_WINDOW_OFFSET>();
            return { -wo.window_x_offset, -wo.window_y_offset };
        }

        struct PendingClear
        {
            RenderTarget* rt;
            VkRect2D rect;
            VkClearValue value;
        };
        std::vector<PendingClear> s_pendingClears;
        // NFSMW_CLEAR_FLUSH_RECT=1 (default kFewerPasses; 0: a resolve runs
        // every pending clear of its source): a resolve runs only the pending clears of
        // what it reads (FlushPendingClears), and a clear adjoining its
        // target's newest pending one of the same value extends it
        // (ClearRect). The 4 empty passes a race frame spent on the scene's
        // tile bands, ~0.2-0.3 ms of the iPad's GPU time. The clears kept
        // wait for the next pass on their target, which must hold them
        // (BeginPass logs one that reaches past its pass).
        const bool s_clearFlushRect = [] { const char* v = std::getenv("NFSMW_CLEAR_FLUSH_RECT"); return v ? v[0] == '1' : kFewerPasses; }();

        void BeginPass(RenderTarget* const color[4], RenderTarget* depth, VkRect2D area)
        {
            bool sameTargets = s_passActive && depth == s_passDepth && std::equal(color, color + 4, s_passColor);
            if (sameTargets && Contains(s_passArea, area))
                return;
            if (sameTargets)
            {
                area = Union(area, s_passArea);
                if (s_logUntiledArea)
                    s_ua.restarts++;
            }
            auto attached = [&](RenderTarget* rt) { return rt == depth || std::find(color, color + 4, rt) != color + 4; };
            for (const PendingClear& pc : s_pendingClears)
                if (attached(pc.rt))
                    area = Union(area, pc.rect);
            EndPass();
            Begin();
            uint32_t w = UINT32_MAX, h = UINT32_MAX;
            VkRenderingAttachmentInfoKHR colors[4]{};
            for (int i = 0; i < 4; i++)
            {
                s_passColor[i] = color[i];
                colors[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
                colors[i].imageView = color[i] ? color[i]->image.view : VK_NULL_HANDLE;
                colors[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                colors[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                if (color[i])
                {
                    w = std::min(w, color[i]->width);
                    h = std::min(h, color[i]->height);
                }
            }
            s_passDepth = depth;
            VkRenderingAttachmentInfoKHR ds{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR };
            if (depth)
            {
                ds.imageView = depth->image.view;
                ds.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                ds.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                ds.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                w = std::min(w, depth->width);
                h = std::min(h, depth->height);
            }
            s_passExtent = { w, h };
            // Clamp to the attachments.
            int32_t x0 = std::clamp(area.offset.x, 0, int32_t(w)), y0 = std::clamp(area.offset.y, 0, int32_t(h));
            int32_t x1 = std::clamp(area.offset.x + int32_t(area.extent.width), x0, int32_t(w));
            int32_t y1 = std::clamp(area.offset.y + int32_t(area.extent.height), y0, int32_t(h));
            s_passArea = { { x0, y0 }, { uint32_t(x1 - x0), uint32_t(y1 - y0) } };
            if (s_logUntiledArea)
            {
                // What Metal loads and stores: MoltenVK's render target runs
                // from (0,0) to the area's far corner. D32S8: 4 + 1 bytes.
                uint64_t px = uint64_t(x1) * uint64_t(y1), bpp = depth ? 5 : 0;
                bool untiled = depth && depth->wide;
                for (int i = 0; i < 4; i++)
                    if (color[i])
                    {
                        bpp += Is64bpp(color[i]->format) ? 8 : 4;
                        untiled = untiled || color[i]->wide;
                    }
                untiled = untiled || (s_untiled && Reg<reg::RB_SURFACE_INFO>().msaa_samples != xenos::MsaaSamples::k1X &&
                    Reg<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable);
                s_ua.passes++;
                s_ua.passPixels += px;
                s_ua.passBytes += 2 * px * bpp;
                if (untiled)
                {
                    s_ua.untiledPasses++;
                    s_ua.untiledPixels += px;
                    s_ua.untiledBytes += 2 * px * bpp;
                }
            }

            // Pending clears of these targets: whole-area ones become load ops.
            std::vector<PendingClear> inPass;
            for (auto it = s_pendingClears.begin(); it != s_pendingClears.end();)
            {
                if (!attached(it->rt))
                {
                    ++it;
                    continue;
                }
                bool whole = Contains(it->rect, s_passArea);
                // One reaching past the attachments' common size (a target
                // larger than another attached) is consumed here, but the
                // part beyond this pass is never cleared; one not covering
                // the pass is also a vkCmdClearAttachments outside the render
                // area (invalid usage). A deferred clear relies on its
                // target's next pass holding it, and NFSMW_CLEAR_FLUSH_RECT
                // defers more of them: with it, logged once per target.
                if (s_clearFlushRect && !Contains(s_passArea, it->rect))
                    LogOnce((0x27ull << 56) | (uint64_t(it->rt->base) << 24) | (uint64_t(it->rt->pitch) << 1) | uint64_t(it->rt->depth),
                        "pending clear %ux%u@%d,%d of target b%u p%u%s reaches past its pass (area %ux%u@%d,%d): the rest is not cleared%s",
                        it->rect.extent.width, it->rect.extent.height, it->rect.offset.x, it->rect.offset.y, it->rt->base, it->rt->pitch,
                        it->rt->depth ? " (depth)" : "", s_passArea.extent.width, s_passArea.extent.height, s_passArea.offset.x,
                        s_passArea.offset.y, whole ? "" : " (cleared in the pass: a vkCmdClearAttachments rect outside its render area)");
                VkRenderingAttachmentInfoKHR* att = nullptr;
                if (it->rt == depth)
                    att = &ds;
                else
                    for (int i = 0; i < 4; i++)
                        if (color[i] == it->rt)
                            att = &colors[i];
                if (whole && att)
                {
                    // Supersedes any older partial clear of this target.
                    std::erase_if(inPass, [&](const PendingClear& pc) { return pc.rt == it->rt; });
                    att->loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                    att->clearValue = it->value;
                }
                else
                {
                    inPass.push_back(*it);
                }
                it = s_pendingClears.erase(it);
            }
            VkRenderingInfoKHR ri{ VK_STRUCTURE_TYPE_RENDERING_INFO_KHR };
            ri.renderArea = s_passArea;
            ri.layerCount = 1;
            ri.colorAttachmentCount = 4;
            ri.pColorAttachments = colors;
            ri.pDepthAttachment = depth ? &ds : nullptr;
            ri.pStencilAttachment = depth ? &ds : nullptr;
            if (s_profiling && s_passQueryNext + 2 <= 512)
            {
                char desc[256];
                int n = snprintf(desc, sizeof(desc), "area %ux%u@%d,%d", s_passArea.extent.width, s_passArea.extent.height,
                    s_passArea.offset.x, s_passArea.offset.y);
                for (int i = 0; i < 4; i++)
                    if (color[i])
                        n += snprintf(desc + n, sizeof(desc) - n, " C%d[b%u f%u p%u]", i, color[i]->base, color[i]->format, color[i]->pitch);
                if (depth)
                    n += snprintf(desc + n, sizeof(desc) - n, " D[b%u p%u]", depth->base, depth->pitch);
                vkCmdResetQueryPool(s_cmd, s_passQueries, s_passQueryNext, 2);
                vkCmdWriteTimestamp(s_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s_passQueries, s_passQueryNext);
                GapCounters now{ s_stats.resolves, s_stats.uploads, s_stats.submits, writewatch::GetGuardStats().uploadBytes };
                s_passInfos.push_back({ desc, 0, s_slot, s_passQueryNext, uint32_t(now.resolves - s_gapLast.resolves),
                    uint32_t(now.uploads - s_gapLast.uploads), uint32_t(now.submits - s_gapLast.submits), (now.shadowBytes - s_gapLast.shadowBytes) / 1024 });
                s_gapLast = now;
                s_passQueryNext += 2;
            }
            s_vk->cmdBeginRendering(s_cmd, &ri);
            s_passActive = true;
            s_stats.passes++;
            for (const PendingClear& pc : inPass)
            {
                VkClearAttachment ca{};
                ca.clearValue = pc.value;
                if (pc.rt == depth)
                    ca.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
                else
                {
                    ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    for (uint32_t i = 0; i < 4; i++)
                        if (color[i] == pc.rt)
                            ca.colorAttachment = i;
                }
                VkClearRect cr{ pc.rect, 0, 1 };
                vkCmdClearAttachments(s_cmd, 1, &ca, 1, &cr);
            }
        }

        // Before something reads `rt` outside a pass (a resolve): run its
        // pending clears. Given what it reads (`read`, target pixels; see
        // s_clearFlushRect), only those overlapping that, and older ones
        // overlapping those (they must land first); the rest stay pending for
        // the next pass on `rt`. Each tile resolve of the untiled scene reads
        // the band after the last tile's clear: four passes of their own a
        // frame, each a Metal render encoder and a FullBarrier.
        void FlushPendingClears(RenderTarget* rt, const VkRect2D* read = nullptr)
        {
            std::vector<PendingClear> kept;
            if (read)
            {
                // Newest first, so `run` (the read, then each clear that runs
                // now) holds every newer clear an older one must land under.
                std::vector<VkRect2D> run{ *read };
                for (size_t i = s_pendingClears.size(); i-- > 0;)
                {
                    if (s_pendingClears[i].rt != rt)
                        continue;
                    const VkRect2D rect = s_pendingClears[i].rect;
                    if (std::any_of(run.begin(), run.end(), [&](const VkRect2D& o) { return Overlaps(o, rect); }))
                        run.push_back(rect);
                    else
                    {
                        kept.insert(kept.begin(), s_pendingClears[i]);
                        s_pendingClears.erase(s_pendingClears.begin() + i);
                    }
                }
            }
            for (const PendingClear& pc : s_pendingClears)
                if (pc.rt == rt)
                {
                    RenderTarget* none[4] = {};
                    RenderTarget* color[4] = { rt };
                    BeginPass(rt->depth ? none : color, rt->depth ? rt : nullptr, pc.rect);
                    EndPass();
                    break;
                }
            // (In their order: the pass took every other clear of `rt`.)
            s_pendingClears.insert(s_pendingClears.end(), kept.begin(), kept.end());
        }

        // Levels 2-3 (see s_passBarrier): before a draw whose vertex shader may
        // read what a resolve wrote since the last full barrier (its vertex
        // data, the indices it fetches itself: `indexBytes` from `indexBase`,
        // a texture of `vsMasks` a resolve stored into), that barrier.
        // Mid-pass it ends the pass. Guest memory is no Metal-tracked binding
        // of a draw, so this is its only ordering.
        void VertexBarrierIfNeeded(const std::array<uint32_t, 3>& vsMasks, uint32_t indexBase, uint32_t indexBytes)
        {
            if (s_vertexPendingLo >= s_vertexPendingHi && s_vertexPendingTextures.empty())
                return;
            auto pending = [](uint32_t base, uint32_t size) {
                base &= 0x1FFFFFFF;
                uint32_t end = base + size;
                if (size == 0 || base >= s_vertexPendingHi || end <= s_vertexPendingLo)
                    return false;
                auto it = std::upper_bound(s_vertexPending.begin(), s_vertexPending.end(), base,
                    [](uint32_t b, const std::pair<uint32_t, uint32_t>& r) { return b < r.second; });
                return it != s_vertexPending.end() && it->first < end;
            };
            int why = -1;
            // (A texture's cache entry and image are keyed by its guest base.)
            for (uint32_t d = 0; d < 3 && why < 0 && !s_vertexPendingTextures.empty(); d++)
                for (uint32_t bits = vsMasks[d]; bits && why < 0; bits &= bits - 1)
                {
                    xenos::xe_gpu_texture_fetch_t f;
                    memcpy(&f, &s_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + uint32_t(__builtin_ctz(bits)) * 6], sizeof(f));
                    if (std::find(s_vertexPendingTextures.begin(), s_vertexPendingTextures.end(), (f.base_address << 12) & 0x1FFFFFFF) !=
                        s_vertexPendingTextures.end())
                        why = 2;
                }
            for (uint32_t word = 0; word < 3 && why < 0; word++)
                for (uint32_t bits = s_vs->info.vertexFetchConstants[word]; bits && why < 0; bits &= bits - 1)
                {
                    const uint32_t* vf = &s_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + (word * 32 + uint32_t(__builtin_ctz(bits))) * 2];
                    if (pending(vf[0] & 0x1FFFFFFC, ((vf[1] >> 2) & 0xFFFFFF) * 4))
                        why = 0;
                }
            if (why < 0 && pending(indexBase, indexBytes))
                why = 1;
            if (why < 0)
                return;
            static const char* const kWhy[3] = { "vertex data", "indices", "a texture" };
            LogOnce((0x2Full << 56) | (s_vs->hash & 0xFFFFFFFFFFFFFCull) | uint64_t(why),
                "vertex barrier (NFSMW_PASS_BARRIER): vs_%016llx reads %s a resolve wrote since the last full barrier%s",
                (unsigned long long)s_vs->hash, kWhy[why], s_passActive ? " (its pass is split)" : "");
            s_barrierCounts.vertex[why]++;
            s_barrierCounts.splits += s_passActive;
            EndPass();
            Begin();
            FullBarrier();
        }

        // ---------------------------------------------------------------
        // Pipelines

        struct PipelineKey
        {
            uint64_t vs, ps;
            uint32_t topology, restart;
            uint32_t colorFormats[4];
            uint32_t depthFormat;
            uint32_t blend[4];       // RB_BLENDCONTROL, or ~0u when blending is off
            uint32_t colorMask;      // 4 bits per target
            uint32_t depthControl;   // RB_DEPTHCONTROL (0 without a depth target, or one attached for the pass only)
            uint32_t raster;         // cull | front CW << 2 | depth clamp << 3 | depth bias << 4
            uint32_t pad;

            bool operator==(const PipelineKey& o) const { return memcmp(this, &o, sizeof(*this)) == 0; }
        };
        static_assert(sizeof(PipelineKey) == 80);

        struct PipelineKeyHash
        {
            size_t operator()(const PipelineKey& k) const
            {
                uint64_t words[sizeof(k) / 8];
                memcpy(words, &k, sizeof(k));
                uint64_t h = 0xCBF29CE484222325ull;
                for (uint64_t w : words)
                    h = (h ^ w) * 0x100000001B3ull ^ (h >> 29);
                return size_t(h);
            }
        };
        std::unordered_map<PipelineKey, VkPipeline, PipelineKeyHash> s_pipelines;

        VkBlendFactor BlendFactor(uint32_t f)
        {
            switch (f)
            {
            case 0: return VK_BLEND_FACTOR_ZERO;
            case 1: return VK_BLEND_FACTOR_ONE;
            case 4: return VK_BLEND_FACTOR_SRC_COLOR;
            case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
            case 6: return VK_BLEND_FACTOR_SRC_ALPHA;
            case 7: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            case 8: return VK_BLEND_FACTOR_DST_COLOR;
            case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
            case 10: return VK_BLEND_FACTOR_DST_ALPHA;
            case 11: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
            case 12: return VK_BLEND_FACTOR_CONSTANT_COLOR;
            case 13: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
            case 14: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
            case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
            case 16: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
            default: return VK_BLEND_FACTOR_ONE;
            }
        }

        VkBlendOp BlendOp(uint32_t op)
        {
            switch (op)
            {
            case 1: return VK_BLEND_OP_SUBTRACT;
            case 2: return VK_BLEND_OP_MIN;
            case 3: return VK_BLEND_OP_MAX;
            case 4: return VK_BLEND_OP_REVERSE_SUBTRACT;
            default: return VK_BLEND_OP_ADD;
            }
        }

        // Compiles the pipeline for `key` (and the SPIR-V it needs). Runs on a
        // pipeline worker: touches only its arguments, the shaders' own
        // locked modules and the (internally synchronized) pipeline cache.
        VkPipeline BuildPipeline(const PipelineKey& key, VkPipelineLayout layout, Shader& vs, Shader& ps)
        {
            VkShaderModule vsModule = VertexModule(vs);
            if (!vsModule)
                return VK_NULL_HANDLE;
            VkPipelineShaderStageCreateInfo stages[2]{};
            stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,
                vsModule, "main", nullptr };
            uint32_t outputs = 0;
            for (int i = 0; i < 4; i++)
                if (key.colorFormats[i] != 0)
                    outputs |= 1u << i;
            uint32_t psRegs = std::min(16u, ps.info.registerCount);
            uint32_t inputs = vs.info.interpolatorMask & ((1u << psRegs) - 1);
            VkShaderModule psModule = PixelVariant(ps, outputs, inputs, (key.raster >> 5) & 1);
            if (!psModule)
                return VK_NULL_HANDLE;
            stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
                psModule, "main", nullptr };

            VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
            VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
            ia.topology = VkPrimitiveTopology(key.topology);
            ia.primitiveRestartEnable = key.restart;
            VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
            vp.viewportCount = 1;
            vp.scissorCount = 1;

            VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
            rs.depthClampEnable = (key.raster >> 3) & 1;
            rs.polygonMode = VK_POLYGON_MODE_FILL;
            rs.cullMode = key.raster & 3;
            rs.frontFace = (key.raster >> 2) & 1 ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
            rs.depthBiasEnable = (key.raster >> 4) & 1;
            rs.lineWidth = 1.0f;

            VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
            ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

            VkPipelineDepthStencilStateCreateInfo dss{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
            if (key.depthFormat != 0)
            {
                reg::RB_DEPTHCONTROL dc{};
                dc.value = key.depthControl;
                dss.depthTestEnable = dc.z_enable;
                dss.depthWriteEnable = dc.z_enable && dc.z_write_enable;
                dss.depthCompareOp = VkCompareOp(dc.zfunc);
                dss.stencilTestEnable = dc.stencil_enable;
                dss.front = { VkStencilOp(dc.stencilfail), VkStencilOp(dc.stencilzpass), VkStencilOp(dc.stencilzfail),
                    VkCompareOp(dc.stencilfunc), 0xFF, 0xFF, 0 };
                if (dc.backface_enable)
                    dss.back = { VkStencilOp(dc.stencilfail_bf), VkStencilOp(dc.stencilzpass_bf), VkStencilOp(dc.stencilzfail_bf),
                        VkCompareOp(dc.stencilfunc_bf), 0xFF, 0xFF, 0 };
                else
                    dss.back = dss.front;
            }

            VkPipelineColorBlendAttachmentState att[4]{};
            VkFormat formats[4];
            for (int i = 0; i < 4; i++)
            {
                formats[i] = VkFormat(key.colorFormats[i]);
                if (key.colorFormats[i] == 0)
                    continue;
                att[i].colorWriteMask = (key.colorMask >> (4 * i)) & 0xF;
                if (key.blend[i] != ~0u)
                {
                    reg::RB_BLENDCONTROL bc{};
                    bc.value = key.blend[i];
                    att[i].blendEnable = VK_TRUE;
                    att[i].srcColorBlendFactor = BlendFactor(uint32_t(bc.color_srcblend));
                    att[i].dstColorBlendFactor = BlendFactor(uint32_t(bc.color_destblend));
                    att[i].colorBlendOp = BlendOp(uint32_t(bc.color_comb_fcn));
                    att[i].srcAlphaBlendFactor = BlendFactor(uint32_t(bc.alpha_srcblend));
                    att[i].dstAlphaBlendFactor = BlendFactor(uint32_t(bc.alpha_destblend));
                    att[i].alphaBlendOp = BlendOp(uint32_t(bc.alpha_comb_fcn));
                }
            }
            VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
            cb.attachmentCount = 4;
            cb.pAttachments = att;

            VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                VK_DYNAMIC_STATE_DEPTH_BIAS };
            VkPipelineDynamicStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
            ds.dynamicStateCount = uint32_t(std::size(dyn));
            ds.pDynamicStates = dyn;

            VkPipelineRenderingCreateInfoKHR rci{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR };
            rci.colorAttachmentCount = 4;
            rci.pColorAttachmentFormats = formats;
            rci.depthAttachmentFormat = VkFormat(key.depthFormat);
            rci.stencilAttachmentFormat = VkFormat(key.depthFormat);
            // NFSMW_SHADING_RATE (an experiment, video/presenter.h): every draw
            // shades once per w x h pixels.
            VkPipelineFragmentShadingRateStateCreateInfoKHR rate{ VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR };
            if (s_vk->shadingRateW)
            {
                rate.fragmentSize = { s_vk->shadingRateW, s_vk->shadingRateH };
                rate.combinerOps[0] = rate.combinerOps[1] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
                rci.pNext = &rate;
            }

            VkGraphicsPipelineCreateInfo gpci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
            gpci.pNext = &rci;
            gpci.stageCount = 2;
            gpci.pStages = stages;
            gpci.pVertexInputState = &vi;
            gpci.pInputAssemblyState = &ia;
            gpci.pViewportState = &vp;
            gpci.pRasterizationState = &rs;
            gpci.pMultisampleState = &ms;
            gpci.pDepthStencilState = &dss;
            gpci.pColorBlendState = &cb;
            gpci.pDynamicState = &ds;
            gpci.layout = layout;
            VkPipeline pipeline = VK_NULL_HANDLE;
            VkResult r = vkCreateGraphicsPipelines(s_dev, s_pipelineCache, 1, &gpci, nullptr, &pipeline);
            if (r != VK_SUCCESS)
            {
                fprintf(stderr, "[renderer] pipeline vs %016llx ps %016llx failed: %d\n",
                    (unsigned long long)key.vs, (unsigned long long)key.ps, int(r));
                pipeline = VK_NULL_HANDLE;
            }
            return pipeline;
        }

        // Pipelines are compiled on worker threads. A new one costs 20-80 ms
        // on RADV (it was 100-460 ms before the vertex shader's single call
        // site), and a command processor waiting on it stalls the game: a
        // traffic car streaming in mid-race froze it for 2 s. The command
        // processor waits briefly (NFSMW_PIPELINE_WAIT_US a frame, default
        // 3000: disk cache hits make it), then skips the draw until it is
        // ready, as Xenia does. For 2 s after a loading screen (a frame of
        // under 64 draws) it waits in full, so scene setup, including
        // anything the game renders once, is drawn exactly.
        // NFSMW_PIPELINE_THREADS=<n> (default: a third of the cores, at most
        // 4; 0 compiles on the command processor). Settings > Advanced >
        // Asynchronous Shaders off (NFSMW_ASYNC_PIPELINES=0): always wait.
        // NFSMW_PIPELINE_LOG=1 logs each pipeline's timing.
        struct PendingPipeline
        {
            std::mutex mutex;
            std::condition_variable cv;
            bool done = false;
            VkPipeline pipeline = VK_NULL_HANDLE;
            std::chrono::steady_clock::time_point queued = std::chrono::steady_clock::now();
            double createMs = 0;
            uint64_t queuedFrame = 0;
            bool outlasted = false;  // outlasted a frame's wait budget: a real compile, not worth waiting for again
        };
        std::unordered_map<PipelineKey, std::shared_ptr<PendingPipeline>, PipelineKeyHash> s_pendingPipelines;
        const uint32_t s_pipelineThreads = [] {
            if (const char* v = std::getenv("NFSMW_PIPELINE_THREADS"))
                return uint32_t(std::atoi(v));
            return std::clamp(std::thread::hardware_concurrency() / 3, 1u, 4u);
        }();
        const int64_t s_pipelineWaitUs = [] { const char* v = std::getenv("NFSMW_PIPELINE_WAIT_US"); return v ? int64_t(std::atoll(v)) : 3000; }();
        const bool s_logPipelines = std::getenv("NFSMW_PIPELINE_LOG") != nullptr;
        uint64_t s_frame = 0, s_drawsAtSwap = 0, s_waitPipelinesUntil = 120;  // boot: wait in full
        uint32_t s_pipelineSkips = 0;         // draws skipped this frame, their pipeline not ready
        int64_t s_pipelineWaitLeftUs = 0;     // this frame's wait budget left

        std::mutex s_compileMutex;
        std::condition_variable s_compileCv;
        std::deque<std::function<void()>> s_compileQueue;
        uint32_t s_compileActive = 0;         // jobs running (under s_compileMutex)
        bool s_compileStopping = false;

        void CompileWorker()
        {
            SetHostThreadName("nfsmw-pipelines");
            hostcpu::LeaveReservedCore();  // started by the command processor
            while (true)
            {
                std::function<void()> job;
                {
                    std::unique_lock lock(s_compileMutex);
                    s_compileCv.wait(lock, [] { return s_compileStopping || !s_compileQueue.empty(); });
                    if (s_compileStopping)
                        return;
                    job = std::move(s_compileQueue.front());
                    s_compileQueue.pop_front();
                    s_compileActive++;
                }
                job();
                {
                    std::lock_guard lock(s_compileMutex);
                    s_compileActive--;
                }
                s_compileCv.notify_all();
            }
        }

        // The pipeline for `key`; VK_NULL_HANDLE with `pending` set while a
        // worker still compiles it (the caller skips the draw).
        VkPipeline GetPipeline(const PipelineKey& key, VkPipelineLayout layout, bool& pending)
        {
            pending = false;
            static PipelineKey lastKey{};
            static VkPipeline lastPipeline = VK_NULL_HANDLE;
            if (lastPipeline != VK_NULL_HANDLE && key == lastKey)
                return lastPipeline;
            auto it = s_pipelines.find(key);
            if (it != s_pipelines.end())
            {
                lastKey = key;
                lastPipeline = it->second;
                return it->second;
            }
            std::shared_ptr<PendingPipeline> job;
            if (auto p = s_pendingPipelines.find(key); p != s_pendingPipelines.end())
                job = p->second;
            else
            {
                s_hitch.pipelines++;
                job = std::make_shared<PendingPipeline>();
                job->queuedFrame = s_frame;
                s_pendingPipelines[key] = job;
                Shader* vs = s_shaders[key.vs].get();
                Shader* ps = s_shaders[key.ps].get();
                auto build = [job, key, layout, vs, ps] {
                    auto start = std::chrono::steady_clock::now();
                    VkPipeline pipeline = BuildPipeline(key, layout, *vs, *ps);
                    {
                        std::lock_guard lock(job->mutex);
                        job->pipeline = pipeline;
                        job->createMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                        job->done = true;
                    }
                    job->cv.notify_all();
                };
                if (s_pipelineThreads == 0)
                {
                    HitchTimer hitchTimer(s_hitch.pipelineMs);
                    build();
                }
                else
                {
                    static std::once_flag started;
                    std::call_once(started, [] {
                        for (uint32_t i = 0; i < s_pipelineThreads; i++)
                            std::thread(CompileWorker).detach();
                    });
                    {
                        std::lock_guard lock(s_compileMutex);
                        s_compileQueue.push_back(std::move(build));
                    }
                    s_compileCv.notify_all();
                }
            }
            {
                HitchTimer hitchTimer(s_hitch.pipelineMs);
                CpWaitTimer wait(CP_WAIT_PIPELINE);
                std::unique_lock lock(job->mutex);
                // Waits share one budget a frame (cache hits make it); once it
                // is spent, draws just check. A per-draw wait made a pipeline
                // used by 13 draws cost 47 ms a frame while it compiled.
                if (!settings::GetBool(settings::Id::AsyncShaders) || s_frame < s_waitPipelinesUntil)
                    job->cv.wait(lock, [&] { return job->done; });
                else
                {
                    // The budget catches compiles that finish within a few
                    // milliseconds (cache hits); one that outlasted it once is
                    // only checked afterwards, not waited for every frame
                    // (that cost 3 ms a frame for as long as it compiled).
                    auto start = std::chrono::steady_clock::now();
                    int64_t budget = job->outlasted ? 0 : std::max<int64_t>(s_pipelineWaitLeftUs, 0);
                    bool ready = job->cv.wait_for(lock, std::chrono::microseconds(budget), [&] { return job->done; });
                    s_pipelineWaitLeftUs -= std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
                    if (!ready && budget > 0)
                        job->outlasted = true;
                    if (!ready)
                    {
                        pending = true;
                        s_pipelineSkips++;
                        return VK_NULL_HANDLE;
                    }
                }
            }
            if (s_logPipelines)
                fprintf(stderr, "[pipeline] vs %016llx ps %016llx: %.1f ms to compile, ready %.1f ms and %llu frames after the first draw\n",
                    (unsigned long long)key.vs, (unsigned long long)key.ps, job->createMs,
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - job->queued).count(),
                    (unsigned long long)(s_frame - job->queuedFrame));
            s_stats.pipelines++;
            s_pipelines[key] = job->pipeline;
            s_pendingPipelines.erase(key);
            return job->pipeline;
        }

        // ---------------------------------------------------------------
        // Scissor (Xenia's draw_util GetScissor)

        struct Rect { int32_t x0, y0, x1, y1; };

        Rect GuestScissor()
        {
            auto tl = Reg<reg::PA_SC_WINDOW_SCISSOR_TL>();
            auto br = Reg<reg::PA_SC_WINDOW_SCISSOR_BR>();
            Rect r{ int32_t(tl.tl_x), int32_t(tl.tl_y), int32_t(br.br_x), int32_t(br.br_y) };
            if (!tl.window_offset_disable)
            {
                auto wo = Reg<reg::PA_SC_WINDOW_OFFSET>();
                r.x0 = std::max(r.x0 + wo.window_x_offset, 0);
                r.y0 = std::max(r.y0 + wo.window_y_offset, 0);
                r.x1 = std::max(r.x1 + wo.window_x_offset, 0);
                r.y1 = std::max(r.y1 + wo.window_y_offset, 0);
            }
            auto stl = Reg<reg::PA_SC_SCREEN_SCISSOR_TL>();
            auto sbr = Reg<reg::PA_SC_SCREEN_SCISSOR_BR>();
            r.x0 = std::max(r.x0, int32_t(stl.tl_x));
            r.y0 = std::max(r.y0, int32_t(stl.tl_y));
            r.x1 = std::min(r.x1, int32_t(sbr.br_x));
            r.y1 = std::min(r.y1, int32_t(sbr.br_y));
            r.x1 = std::max(r.x1, r.x0);
            r.y1 = std::max(r.y1, r.y0);
            return r;
        }

        // NFSMW_UNTILED_AREA on for this frame (=alt: in odd 120-frame windows).
        bool UntiledAreaOn()
        {
            return s_untiledArea == 1 || (s_untiledArea == 2 && s_frame / 120 % 2 == 1);
        }

        // Where an untiled draw into `targets` (4 colour, then depth; any
        // null) may draw, in guest pixels of their common w x h (the caller
        // clamps to it): tile 0 draws the whole frame, so the whole target.
        // With NFSMW_UNTILED_AREA, one clipped to its viewport (x and y
        // scaled and offset, clipping on) only the pixels whose centres lie
        // inside the viewport (a counted draw's rule, in Draw). No scissor:
        // they are tile 0's, and the rows and columns past tile 0 are drawn
        // by tile 0 alone (the window scissor is the tile: for side-by-side
        // tiles only the left one's columns, and the right half went black
        // when they were drawn to it; D3D leaves the screen scissor
        // 8192x8192). `why`: 0 cut to the viewport, 1 clipping off, 2 no
        // viewport transform (or garbage in it), 3 a target that fell back
        // to the whole target (wholeArea), 4 off.
        Rect UntiledScissor(RenderTarget* const targets[5], int32_t w, int32_t h, int* why = nullptr)
        {
            Rect r{ 0, 0, w, h };
            int reason = 4;
            if (UntiledAreaOn())
            {
                float ax = std::fabs(RegF(XE_GPU_REG_PA_CL_VPORT_XSCALE)), ox = RegF(XE_GPU_REG_PA_CL_VPORT_XOFFSET);
                float ay = std::fabs(RegF(XE_GPU_REG_PA_CL_VPORT_YSCALE)), oy = RegF(XE_GPU_REG_PA_CL_VPORT_YOFFSET);
                if (Reg<reg::PA_CL_CLIP_CNTL>().clip_disable)
                    reason = 1;
                else if ((Reg<reg::PA_CL_VTE_CNTL>().value & 0xF) != 0xF || !std::isfinite(ax + ox + ay + oy))
                    reason = 2;
                else if (std::any_of(targets, targets + 5, [](RenderTarget* rt) { return rt && rt->wholeArea; }))
                    reason = 3;
                else
                    reason = 0;
                if (reason == 0)
                {
                    // As the host viewport: the window offset (0 for tile 0)
                    // and D3D's integer pixel centres. |scale|: y is flipped.
                    if (Reg<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable)
                    {
                        auto wo = Reg<reg::PA_SC_WINDOW_OFFSET>();
                        ox += float(wo.window_x_offset);
                        oy += float(wo.window_y_offset);
                    }
                    if (Reg<reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero)
                    {
                        ox += 0.5f;
                        oy += 0.5f;
                    }
                    auto edge = [](float v, int32_t hi) { return int32_t(std::fmax(0.0f, std::fmin(std::ceil(v - 0.5f), float(hi)))); };
                    r = { edge(ox - ax, w), edge(oy - ay, h), edge(ox + ax, w), edge(oy + ay, h) };
                }
            }
            if (why)
                *why = reason;
            return r;
        }

        // ---------------------------------------------------------------
        // Indices

        // Index `i` of the current draw, as the vertex shader's fetchIndex.
        struct IndexSource
        {
            bool indexed;
            uint32_t base;       // physical
            bool is32;
            uint32_t endian;

            uint32_t Get(uint32_t i) const
            {
                if (!indexed)
                    return i;
                if (is32)
                    return GpuSwap(LoadPhysical(base + i * 4), endian) & 0xFFFFFF;
                uint32_t addr = base + i * 2;
                uint32_t w = GpuSwap(LoadPhysical(addr & ~3u), endian);
                bool firstInHigh = endian == 2 || endian == 3;
                bool high = ((addr >> 1) & 1) != firstInHigh;
                return high ? (w >> 16) : (w & 0xFFFF);
            }
        };

        // Get() for a whole draw, written straight into the ring: `out16`
        // keeps 16-bit indices (the pipeline's restart index is then 0xFFFF,
        // so only when restart is off or the guest's reset index is 0xFFFF);
        // otherwise 32-bit, with the reset index mapped to ~0 when `restart`.
        // For 16-bit indices, 8in16 and 8in32 swap each index's bytes; none
        // and 16in32 keep them (Get() picks the other half of the swapped word).
        void ConvertIndices(uint8_t* dst, const IndexSource& src, uint32_t count, bool out16, bool restart, uint32_t resetIndex)
        {
            if (!src.is32)
            {
                const uint8_t* p = s_sharedHost + (src.base & 0x1FFFFFFE);
                bool swap = src.endian == 1 || src.endian == 2;
                if (out16)
                {
                    if (!swap)
                    {
                        memcpy(dst, p, size_t(count) * 2);
                        return;
                    }
                    for (uint32_t i = 0; i < count; i++)
                    {
                        uint16_t v;
                        memcpy(&v, p + size_t(i) * 2, 2);
                        v = __builtin_bswap16(v);
                        memcpy(dst + size_t(i) * 2, &v, 2);
                    }
                    return;
                }
                for (uint32_t i = 0; i < count; i++)
                {
                    uint16_t v;
                    memcpy(&v, p + size_t(i) * 2, 2);
                    uint32_t x = swap ? __builtin_bswap16(v) : v;
                    x = restart && x == resetIndex ? 0xFFFFFFFFu : x;
                    memcpy(dst + size_t(i) * 4, &x, 4);
                }
                return;
            }
            const uint8_t* p = s_sharedHost + (src.base & 0x1FFFFFFC);
            for (uint32_t i = 0; i < count; i++)
            {
                uint32_t w;
                memcpy(&w, p + size_t(i) * 4, 4);
                uint32_t v = GpuSwap(w, src.endian) & 0xFFFFFF;
                v = restart && v == resetIndex ? 0xFFFFFFFFu : v;
                memcpy(dst + size_t(i) * 4, &v, 4);
            }
        }


        // ---------------------------------------------------------------
        // Textures: 2D, cube and 3D, with the guest's mip levels (2D, cube).
        // Validity comes from write watches (kernel/write_watch.h): a
        // texture reloads when any page it covers was written (a CPU store,
        // a file read or a resolve) since it was last loaded.

        struct TextureFormatInfo
        {
            VkFormat vk;
            uint32_t bpbLog2;   // guest bytes per block
            uint32_t block;     // block width/height in texels
            uint32_t convert;   // 0 copy; 1 565, 2 1555, 3 4444 -> RGBA8; 4 24_8 -> R32F
        };

        bool GetTextureFormat(uint32_t f, TextureFormatInfo& out)
        {
            switch (f)
            {
            case 2: out = { VK_FORMAT_R8_UNORM, 0, 1, 0 }; return true;
            case 3: out = { VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 2 }; return true;
            case 4: out = { VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1 }; return true;
            case 6: case 14: out = { VK_FORMAT_R8G8B8A8_UNORM, 2, 1, 0 }; return true;
            case 7: out = { VK_FORMAT_A2B10G10R10_UNORM_PACK32, 2, 1, 0 }; return true;
            case 10: out = { VK_FORMAT_R8G8_UNORM, 1, 1, 0 }; return true;
            case 15: out = { VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 3 }; return true;
            case 18: out = { VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 3, 4, 0 }; return true;
            case 19: out = { VK_FORMAT_BC2_UNORM_BLOCK, 4, 4, 0 }; return true;
            case 20: out = { VK_FORMAT_BC3_UNORM_BLOCK, 4, 4, 0 }; return true;
            case 22: case 23: out = { VK_FORMAT_R32_SFLOAT, 2, 1, 4 }; return true;
            case 24: out = { VK_FORMAT_R16_UNORM, 1, 1, 0 }; return true;
            case 25: out = { VK_FORMAT_R16G16_UNORM, 2, 1, 0 }; return true;
            case 26: out = { VK_FORMAT_R16G16B16A16_UNORM, 3, 1, 0 }; return true;
            case 30: out = { VK_FORMAT_R16_SFLOAT, 1, 1, 0 }; return true;
            case 31: out = { VK_FORMAT_R16G16_SFLOAT, 2, 1, 0 }; return true;
            case 32: out = { VK_FORMAT_R16G16B16A16_SFLOAT, 3, 1, 0 }; return true;
            case 36: out = { VK_FORMAT_R32_SFLOAT, 2, 1, 0 }; return true;
            case 37: out = { VK_FORMAT_R32G32_SFLOAT, 3, 1, 0 }; return true;
            case 38: out = { VK_FORMAT_R32G32B32A32_SFLOAT, 4, 1, 0 }; return true;
            case 49: out = { VK_FORMAT_BC5_UNORM_BLOCK, 4, 4, 0 }; return true;
            default: return false;
            }
        }

        // Stored as resolves write it: one 32-bit word a texel, unconverted
        // (the formats a scaled texture can have).
        bool ScaledFormat(const TextureFormatInfo& fi)
        {
            return fi.bpbLog2 == 2 && fi.convert == 0 && fi.block == 1;
        }

        uint32_t Tiled2D(uint32_t x, uint32_t y, uint32_t pitchAligned, uint32_t bppLog2)
        {
            uint32_t outer = (((y >> 5) * (pitchAligned >> 5)) + (x >> 5)) << 6;
            uint32_t inner = (((y >> 1) & 7) << 3) | (x & 7);
            uint32_t oi = (outer | inner) << bppLog2;
            uint32_t bank = (y >> 4) & 1;
            uint32_t pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1);
            return ((y & 1) << 4) | (pipe << 6) | (bank << 11) | (oi & 0xF) | (((oi >> 4) & 1) << 5) |
                (((oi >> 5) & 7) << 8) | ((oi >> 8) << 12);
        }

        uint32_t Log2Ceil(uint32_t v)
        {
            return v <= 1 ? 0 : 32 - __builtin_clz(v - 1);
        }

        // Mip levels (Xenia's texture_util GetGuestTextureLayout and
        // GetPackedMipOffset, for 2D and cube textures). Levels 1.. are
        // stored one after another from mip_address, each (per face) with a
        // pitch and height from the base size rounded up to a power of two,
        // shifted to the level and aligned to 32x32 blocks (linear rows to
        // 256 bytes), faces 4 KB-aligned. With packed mips, the levels from
        // the one whose short side is 16 texels (packedLevel) share that
        // level's storage, each at an offset in its 32x32 tile; when the
        // base itself is that small (packedLevel 0), the mips' shared tile
        // is at mip_address.
        struct MipLayout
        {
            uint32_t width, height, block, bpbLog2, layers;
            bool tiled, packed;
            uint32_t packedLevel;

            uint32_t StorageLevel(uint32_t level) const { return packed ? std::min(level, packedLevel) : level; }
            uint32_t PitchBlocks(uint32_t storage) const  // 32-aligned
            {
                uint32_t texels = std::max(std::bit_ceil(width) >> storage, 1u);
                return (((texels + block - 1) / block) + 31) & ~31u;
            }
            uint32_t RowsBlocks(uint32_t storage) const  // 32-aligned
            {
                uint32_t texels = std::max(std::bit_ceil(height) >> storage, 1u);
                return (((texels + block - 1) / block) + 31) & ~31u;
            }
            uint32_t RowBytes(uint32_t storage) const  // linear
            {
                return ((PitchBlocks(storage) << bpbLog2) + 255) & ~255u;
            }
            uint32_t SliceStride(uint32_t storage) const
            {
                uint32_t bytes = tiled ? (PitchBlocks(storage) * RowsBlocks(storage)) << bpbLog2 : RowBytes(storage) * RowsBlocks(storage);
                return (bytes + 4095) & ~4095u;
            }
            uint32_t Offset(uint32_t storage) const  // from mip_address
            {
                uint32_t offset = 0;
                for (uint32_t s = 1; s < storage; s++)
                    offset += SliceStride(s) * layers;
                return offset;
            }
            // The level's position in its storage, in blocks.
            void PackedOffset(uint32_t level, uint32_t& x, uint32_t& y) const
            {
                x = y = 0;
                if (!packed || level < packedLevel)
                    return;
                uint32_t log2w = Log2Ceil(width), log2h = Log2Ceil(height);
                uint32_t log2Size = std::min(log2w, log2h);
                uint32_t base = log2Size > 4 ? log2Size - 4 : 0;
                uint32_t sub = level - base;
                if (sub < 3)
                {
                    if (log2w > log2h)
                        y = 16 >> sub;
                    else
                        x = 16 >> sub;
                }
                else if (log2w > log2h)
                    x = (1u << (log2w - base)) >> (sub - 2);
                else
                    y = (1u << (log2h - base)) >> (sub - 2);
                x /= block;
                y /= block;
            }
        };

        // Texture loading runs on the GPU: a compute shader untiles and
        // converts from guest memory into the ring, then a buffer->image copy.
        // Ordering against resolves is then just a barrier (no CPU wait).
        // ---------------------------------------------------------------
        // Scaled resolve memory (internal resolution above 1x)
        //
        // A resolve from a scaled render target writes each guest pixel's
        // sx x sy host samples here, besides the 1x average into guest memory
        // (which the CPU, 1x draws and the auto-exposure readback keep
        // using). Storage is per 4 KB guest page: a slot of 4096 x s bytes
        // (s = sx x sy) in one pool buffer, found through a page table (a GPU
        // buffer and its CPU copy), as MoltenVK has no sparse buffers. The
        // samples of the 32-bit texel at guest byte address A are at
        //   slot(A >> 12) x 4096 x s + (A & 4095) x s + (j x sx + i) x 4.
        // A page's scaled copy is current while the last write to the page
        // was a scaled resolve: its write sequence is still the one recorded
        // right after that resolve (a CPU write, a 1x resolve or any other
        // GPU write moves it on). Draws into scaled targets then sample
        // scaled textures made from it, and the front buffer is presented at
        // host resolution. NFSMW_SCALED_RESOLVE=0 keeps everything 1x (the
        // targets are still scaled: supersampling). NFSMW_SCALED_MB sizes the
        // pool (default 512). Scaled textures are stored into straight from
        // the pool through an R32_UINT view; NFSMW_SCALED_DIRECT=0 (or a
        // device that can't) loads them through a scratch buffer and a copy.
        // Scaled resolves write the pool coalesced (see the resolve shader;
        // not on Apple GPUs); NFSMW_SCALED_COALESCE=0 has each thread store
        // its own samples, =1 coalesces on any GPU.
        constexpr uint32_t kGuestPages = uint32_t(kSharedSize >> 12);
        struct ScaledMemory
        {
            bool tried = false, enabled = false, direct = false, coalesce = false;
            uint32_t sx = 1, sy = 1, s = 1, slotWords = 0, slots = 0;
            VkBuffer pool = VK_NULL_HANDLE, table = VK_NULL_HANDLE, scratch = VK_NULL_HANDLE;
            VkDeviceMemory poolMemory = VK_NULL_HANDLE, tableMemory = VK_NULL_HANDLE, scratchMemory = VK_NULL_HANDLE;
            VkDeviceSize scratchSize = 0;  // also the largest scaled texture (no buffer when direct)
            VkDeviceSize poolBytes = 0, tableBytes = 0, scratchBytes = 0;  // allocated (the [perf] line's memory)
            std::vector<uint32_t> pageSlot;  // slot + 1; 0: none
            std::vector<uint64_t> pageSeq;   // write sequence after the page's last scaled resolve; 0: none or stale
            std::vector<uint32_t> slotPage;  // page + 1; 0: free
            std::vector<uint8_t> slotRef;    // clock bit (used since the hand last passed)
            uint32_t clock = 0;
            std::vector<uint32_t> dirty;     // pages whose table entry changed
            uint64_t allocations = 0, evictions = 0, textures = 0, fallbacks = 0;
            uint64_t loads = 0, untiledBytes = 0, resolvedBytes = 0;  // for the [perf] line
            uint32_t hostPageGuestPages = 1;  // guest pages per host page (write sequences are per host page)
        } s_scaled;

        bool EnsureScaledMemory();
        void FillFreshSlots(const std::vector<std::pair<uint32_t, uint32_t>>& fresh);

        // The page's slot, allocated (clock replacement) if it has none.
        uint32_t ScaledSlot(uint32_t page, bool& fresh)
        {
            fresh = false;
            uint32_t& entry = s_scaled.pageSlot[page];
            if (entry)
            {
                s_scaled.slotRef[entry - 1] = 1;
                return entry - 1;
            }
            uint32_t slot;
            for (;;)
            {
                slot = s_scaled.clock;
                s_scaled.clock = (slot + 1) % s_scaled.slots;
                if (!s_scaled.slotPage[slot])
                    break;
                if (s_scaled.slotRef[slot])
                {
                    s_scaled.slotRef[slot] = 0;
                    continue;
                }
                // Evict: that page falls back to its 1x copy. (Command
                // buffers are ordered by full barriers, so earlier work is
                // done with the slot before the table changes.)
                uint32_t old = s_scaled.slotPage[slot] - 1;
                s_scaled.pageSlot[old] = 0;
                s_scaled.pageSeq[old] = 0;
                s_scaled.dirty.push_back(old);
                s_scaled.evictions++;
                break;
            }
            s_scaled.slotPage[slot] = page + 1;
            s_scaled.slotRef[slot] = 1;
            entry = slot + 1;
            s_scaled.dirty.push_back(page);
            s_scaled.allocations++;
            fresh = true;
            return slot;
        }

        // Upload changed page-table entries (in the command stream, so the
        // GPU sees them in order).
        void FlushScaledTable()
        {
            auto& dirty = s_scaled.dirty;
            if (dirty.empty())
                return;
            std::sort(dirty.begin(), dirty.end());
            dirty.erase(std::unique(dirty.begin(), dirty.end()), dirty.end());
            EndPass();
            Begin();
            for (size_t i = 0; i < dirty.size();)
            {
                size_t j = i + 1;
                while (j < dirty.size() && dirty[j] == dirty[j - 1] + 1 && j - i < 16384)
                    j++;
                vkCmdUpdateBuffer(s_cmd, s_scaled.table, VkDeviceSize(dirty[i]) * 4, VkDeviceSize(j - i) * 4,
                    &s_scaled.pageSlot[dirty[i]]);
                i = j;
            }
            dirty.clear();
            FullBarrier();
        }

        // The page's scaled copy is current (see above).
        bool ScaledPageValid(uint32_t page)
        {
            uint64_t seq = s_scaled.pageSeq[page];
            return seq != 0 && s_scaled.pageSlot[page] != 0 && writewatch::WriteSequence(page << 12, 4096) == seq;
        }

        // Every page of [base, base + size) has a current scaled copy.
        bool ScaledRangeValid(uint32_t base, uint32_t size)
        {
            if (!s_scaled.enabled || size == 0)
                return false;
            uint32_t first = base >> 12, last = std::min<uint32_t>((base + size - 1) >> 12, kGuestPages - 1);
            for (uint32_t page = first; page <= last; page++)
                if (!ScaledPageValid(page))
                    return false;
            for (uint32_t page = first; page <= last; page++)
                s_scaled.slotRef[s_scaled.pageSlot[page] - 1] = 1;
            return true;
        }

        const char* kUntileGlsl = R"(#version 460
layout(local_size_x = 8, local_size_y = 8) in;
layout(std430, set = 0, binding = 0) readonly buffer SharedMemory { uint g_mem[]; };)" GLSL_SHARED_MEMORY_READ R"(
#ifdef TO_IMAGE
// 1x textures of 32-bit texels, stored into the image through an R32_UINT view
// (no staging copy): x is the texel, as the staging row's word index.
layout(set = 0, binding = 1, r32ui) writeonly uniform uimage2D o_image;
#define STORE(x, v) imageStore(o_image, ivec2(int(x), int(gy)), uvec4((v), 0u, 0u, 0u))
#else
layout(std430, set = 0, binding = 1) writeonly buffer Staging { uint o_mem[]; };
#define STORE(x, v) o_mem[row + (x)] = (v)
#endif
layout(push_constant) uniform P
{
    uint srcBase, tiled, pitch, bpbLog2;   // pitch: tiled = blocks (32-aligned), linear = bytes
    uint offX, offY, width, height;        // blocks
    uint endian, convert, dstWord, dstRowWords;
    uint z, heightAligned;                 // tiled == 2: 3D slice and aligned height (blocks)
} p;

uint tiledCombine(uint oi, uint bank, uint pipe, uint yLsb)
{
    return (yLsb << 4) | (pipe << 6) | (bank << 11) | (oi & 0xFu) | (((oi >> 4) & 1u) << 5) |
        (((oi >> 5) & 7u) << 8) | ((oi >> 8) << 12);
}

uint tiled3D(uint x, uint y, uint z, uint pitch, uint height, uint bppLog2)
{
    uint outer = ((((z >> 2) * (height >> 4) + (y >> 4)) * (pitch >> 5)) + (x >> 5)) << 7;
    uint inner = ((z & 3u) << 5) | (((y >> 1) & 3u) << 3) | (x & 7u);
    uint bank = ((y >> 3) ^ (z >> 2)) & 1u;
    uint pipe = ((x >> 3) & 3u) ^ (bank << 1);
    return tiledCombine((outer | inner) << bppLog2, bank, pipe, y & 1u);
}

uint tiled2D(uint x, uint y, uint pitch, uint bppLog2)
{
    uint outer = (((y >> 5) * (pitch >> 5)) + (x >> 5)) << 6;
    uint inner = (((y >> 1) & 7u) << 3) | (x & 7u);
    uint oi = (outer | inner) << bppLog2;
    uint bank = (y >> 4) & 1u;
    uint pipe = ((x >> 3) & 3u) ^ (((y >> 3) & 1u) << 1);
    return ((y & 1u) << 4) | (pipe << 6) | (bank << 11) | (oi & 0xFu) | (((oi >> 4) & 1u) << 5) |
        (((oi >> 5) & 7u) << 8) | ((oi >> 8) << 12);
}

uint swapWord(uint v, uint e)
{
    if (e == 1u) return ((v << 8) & 0xFF00FF00u) | ((v >> 8) & 0x00FF00FFu);
    if (e == 2u) return (v << 24) | ((v << 8) & 0x00FF0000u) | ((v >> 8) & 0x0000FF00u) | (v >> 24);
    if (e == 3u) return (v >> 16) | (v << 16);
    return v;
}

uint addr(uint bx, uint by)
{
    uint x = bx + p.offX, y = by + p.offY;
    if (p.tiled == 2u)
        return p.srcBase + tiled3D(x, y, p.z, p.pitch, p.heightAligned, p.bpbLog2);
    return p.srcBase + (p.tiled != 0u ? tiled2D(x, y, p.pitch, p.bpbLog2) : y * p.pitch + (x << p.bpbLog2));
}

uint word(uint a) { return swapWord(memLoad((a & 0x1FFFFFFFu) >> 2), p.endian); }

uint element(uint bx, uint by)
{
    if (bx >= p.width) return 0u;
    uint a = addr(bx, by);
    uint w = word(a & ~3u);
    return p.bpbLog2 == 1u ? (w >> ((a & 2u) * 8u)) & 0xFFFFu : (w >> ((a & 3u) * 8u)) & 0xFFu;
}

uint expand(uint v, uint bits) { uint m = (1u << bits) - 1u; return (v * 255u + m / 2u) / m; }

void main()
{
    uint gx = gl_GlobalInvocationID.x, gy = gl_GlobalInvocationID.y;
    if (gy >= p.height) return;
    uint row = p.dstWord + gy * p.dstRowWords;
    if (p.convert != 0u)
    {
        if (gx >= p.width) return;
        uint o;
        if (p.convert == 4u)
        {
            o = floatBitsToUint(float(word(addr(gx, gy)) >> 8) / 16777215.0);
        }
        else
        {
            uint v = element(gx, gy);
            uint r, g, b, a;
            if (p.convert == 1u) { r = expand(v & 31u, 5u); g = expand((v >> 5) & 63u, 6u); b = expand((v >> 11) & 31u, 5u); a = 255u; }
            else if (p.convert == 2u) { r = expand(v & 31u, 5u); g = expand((v >> 5) & 31u, 5u); b = expand((v >> 10) & 31u, 5u); a = (v >> 15) != 0u ? 255u : 0u; }
            else { r = expand(v & 15u, 4u); g = expand((v >> 4) & 15u, 4u); b = expand((v >> 8) & 15u, 4u); a = expand(v >> 12, 4u); }
            o = r | (g << 8) | (b << 16) | (a << 24);
        }
        STORE(gx, o);
        return;
    }
    if (p.bpbLog2 >= 2u)
    {
        if (gx >= p.width) return;
        uint words = 1u << (p.bpbLog2 - 2u);
        uint a = addr(gx, gy);
        for (uint k = 0u; k < words; k++)
            STORE(gx * words + k, word(a + k * 4u));
        return;
    }
    uint perWord = 4u >> p.bpbLog2;
    uint bx = gx * perWord;
    if (bx >= p.width) return;
    uint o = 0u;
    for (uint k = 0u; k < perWord; k++)
        o |= element(bx + k, gy) << (k * (8u << p.bpbLog2));
    STORE(gx, o);
}
)";

        VkPipeline s_untilePipeline = VK_NULL_HANDLE;
        VkPipelineLayout s_untileLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout s_untileSetLayout = VK_NULL_HANDLE;
        // 1x textures stored directly (TO_IMAGE): 2D (or, NFSMW_DIRECT_CUBE, a
        // cube, a face at a time), one level, uncompressed, 32-bit texels; the
        // untile writes the image, with no staging slice and no copy after it
        // (on the Steam Frame a 1600x1600 shadow map's upload was ~0.7 ms,
        // about half of it the copy). NFSMW_UNTILE_DIRECT=0 keeps the staging
        // path.
        VkPipeline s_untileStorePipeline = VK_NULL_HANDLE;
        VkPipelineLayout s_untileStoreLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout s_untileStoreSetLayout = VK_NULL_HANDLE;
        const bool s_untileDirect = [] { const char* v = std::getenv("NFSMW_UNTILE_DIRECT"); return !v || v[0] != '0'; }();
        // NFSMW_DIRECT_CUBE=1 (default kFewerPasses; 0: cubes take the staging path): an
        // un-mipped cube is a direct texture too, each face stored through an
        // R32_UINT 2D view of its layer (Texture::storeViews), so a resolve
        // into a face stores into it as well (Resolve). The race's
        // environment cube (256x256, faces resolved every frame) was staged
        // again at its first use in the scene, on every platform: six
        // untiles, six copies and a split render pass a frame (two in heavy
        // scenes). A device that turns the image or a view down keeps the
        // staging path for that format.
        const bool s_directCube = [] { const char* v = std::getenv("NFSMW_DIRECT_CUBE"); return v ? v[0] == '1' : kFewerPasses; }();
        // fi.vk -> a storable R32_UINT view is allowed (2D; cube compatible, with six layers)
        std::unordered_map<VkFormat, bool> s_directFormats, s_directCubeFormats;

        bool DirectFormat(VkFormat format, bool cube = false)
        {
            auto [it, fresh] = (cube ? s_directCubeFormats : s_directFormats).try_emplace(format, false);
            if (fresh)
            {
                VkImageFormatProperties props;
                it->second = vkGetPhysicalDeviceImageFormatProperties(s_vk->physical, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT | (cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0),
                    &props) == VK_SUCCESS && (!cube || props.maxArrayLayers >= 6);
            }
            return it->second;
        }

        struct UntileConstants
        {
            uint32_t srcBase, tiled, pitch, bpbLog2;
            uint32_t offX, offY, width, height;
            uint32_t endian, convert, dstWord, dstRowWords;
            uint32_t z, heightAligned;
        };

        struct Texture
        {
            Image image;
            VkFormat format;
            uint32_t width, height;
            uint32_t guestStart, guestSize;
            uint64_t loadedSequence = 0;  // writewatch::Current() when last loaded
            uint64_t checkedEpoch = ~0ull;  // writewatch::WriteEpoch() at the last validity check
            bool uploaded = false;
            bool cube = false;
            bool volume = false;
            uint32_t layers = 1, depth = 1;
            uint32_t levels = 1;
            uint32_t mipStart = 0, mipSize = 0;  // the mip levels' guest memory (size 0: none)
            bool scaled = false;  // host resolution, from the scaled resolve memory
            VkImageView storeView = VK_NULL_HANDLE;  // scaled: R32_UINT, stored into by the untile (if the device can)
            // The same, a 2D view of each layer: [0] is storeView, and a
            // direct cube (NFSMW_DIRECT_CUBE) has one per face.
            VkImageView storeViews[6] = {};
            std::unordered_map<uint32_t, VkImageView> views;  // by swizzle
            // How the untile reads a direct 1x texture (a resolve can store
            // into it the same way, see Resolve); sliceStride: a cube's guest
            // bytes from one face to the next.
            bool tiled = false;
            uint32_t pitch = 0, bpbLog2 = 0, convert = 0, endian = 0, offX = 0, offY = 0, sliceStride = 0;
            // NFSMW_TEXTURE_VERIFY: a hash of each 4 KB guest page of its
            // base, then its mips, as loaded (empty: not checked), and the
            // frame it was last checked in.
            std::vector<uint64_t> verifyHashes;
            uint64_t verifyFrame = 0;
        };
        void LoadScaledTexture(Texture& t, uint32_t base, uint32_t pitchBlocks, uint32_t endian);
        bool ScaledTextureValid(uint32_t base, uint32_t size);
        std::unordered_map<uint64_t, std::unique_ptr<Texture>> s_textures;
        // Direct 1x 2D textures by guest address (the latest made there), for
        // resolves that store into them too (DestroyTexture drops its entry).
        std::unordered_map<uint32_t, Texture*> s_directByBase;
        // Resolves store into such a texture too when it is exactly their
        // surface (see Resolve); NFSMW_RESOLVE_TO_TEXTURE=0 turns it off.
        const bool s_resolveToTexture = [] { const char* v = std::getenv("NFSMW_RESOLVE_TO_TEXTURE"); return !v || v[0] != '0'; }();
        // NFSMW_FUSE_PARTIAL=1 (default kFewerPasses; 0: only a resolve
        // covering the whole texture stores into it): a resolve into whole 32x32 tiles
        // inside a direct texture (or a cube's face) stores there too, at
        // those tiles' texels (Resolve). The scene's colour and depth
        // (1280x720) are each resolved in three strips, a predicated tile
        // each, and were untiled again between passes every frame (3.7 MB
        // each), on every platform.
        const bool s_fusePartial = [] { const char* v = std::getenv("NFSMW_FUSE_PARTIAL"); return v ? v[0] == '1' : kFewerPasses; }();
        // Direct textures by their first guest byte (the latest made there),
        // for resolves into part of one (Resolve): every one with
        // NFSMW_FUSE_PARTIAL, the cubes with NFSMW_DIRECT_CUBE alone; and the
        // largest one's guest size, how far before an address one holding
        // it can start.
        std::map<uint32_t, Texture*> s_directByStart;
        uint32_t s_maxDirectSize = 0;

        // NFSMW_LOG_UPLOADS=<first>[,<last>]: in those frames (s_frame, as
        // NFSMW_PASS_PROFILE numbers them), a line per texture (re)load: its
        // path, whether it ended a render pass, and for a reload the guest
        // pages written since the last one with what last wrote each
        // (write_watch keeps that while this is set); a line per resolve
        // (stored into its cached texture or not, and why); a summary at the
        // end of each frame. Null when off.
        struct UploadLog
        {
            uint64_t first = 0, last = 0;
            bool now = false;  // s_frame is logged
            struct Resolve { uint64_t frame; const char* unfused; };  // unfused: null when it stored into the texture
            std::unordered_map<uint32_t, Resolve> resolves;  // by destination, the latest (every frame)
            uint32_t uploads = 0, endedPasses = 0, resolvesNow = 0, fusedNow = 0;
            uint64_t bytes = 0;
            std::map<std::string, uint32_t> byPath, byWriters;
        };
        UploadLog* const s_uploadLog = []() -> UploadLog* {
            const char* v = std::getenv("NFSMW_LOG_UPLOADS");
            if (!v)
                return nullptr;
            auto* log = new UploadLog();
            log->first = log->last = std::strtoull(v, nullptr, 10);
            if (const char* comma = std::strchr(v, ','))
                log->last = std::strtoull(comma + 1, nullptr, 10);
            return log;
        }();

        // Whether the latest resolve to `dest` stored into its cached texture.
        std::string ResolveNote(uint32_t dest)
        {
            auto it = s_uploadLog->resolves.find(dest);
            if (it == s_uploadLog->resolves.end())
                return "?";
            return it->second.unfused ? std::string("unfused: ") + it->second.unfused : "fused";
        }

        // The guest pages of `t` written since its load, and what last wrote
        // them (the first 4, then counts by kind, also added to `kinds`).
        std::string DescribeWrites(const Texture& t, std::set<std::string>& kinds)
        {
            auto pageCount = [](uint32_t b, uint32_t n) { return n ? ((b + n - 1) >> 12) - (b >> 12) + 1 : 0u; };
            auto inTexture = [&](uint32_t a) { return a - t.guestStart < t.guestSize || (t.mipSize && a - t.mipStart < t.mipSize); };
            std::vector<uint32_t> pages(pageCount(t.guestStart, t.guestSize) + pageCount(t.mipStart, t.mipSize));
            uint32_t n = writewatch::WrittenPages(t.guestStart, t.guestSize, t.loadedSequence, pages.data(), pageCount(t.guestStart, t.guestSize));
            uint32_t mipN = t.mipSize ? writewatch::WrittenPages(t.mipStart, t.mipSize, t.loadedSequence, pages.data() + n,
                pageCount(t.mipStart, t.mipSize)) : 0;
            std::string text = std::format("dirty {}/{} pages, mips {}/{}:", n, pageCount(t.guestStart, t.guestSize), mipN,
                pageCount(t.mipStart, t.mipSize));
            std::map<std::string, uint32_t> counts;
            for (uint32_t i = 0; i < n + mipN; i++)
            {
                writewatch::LastWrite w = writewatch::LastWriter(pages[i]);
                // (NFSMW_WATCH_SUBPAGES: compared, a store its host page's
                // snapshot found there; unverified, marked unseen as the
                // snapshot closed without a compare, at the fault that opened it.)
                std::string kind = w.kind == writewatch::Writer::kCpuFault ? (inTexture(w.tag) ? "cpu-fault" : "cpu-fault-nbr")
                    : w.kind == writewatch::Writer::kResolve ? "resolve" : w.kind == writewatch::Writer::kHostWrite ? "host-write"
                    : w.kind == writewatch::Writer::kGpuStore ? "gpu-store" : w.kind == writewatch::Writer::kRange ? "range"
                    : w.kind == writewatch::Writer::kCompared ? "compared" : w.kind == writewatch::Writer::kUnverified ? "unverified" : "none";
                counts[kind]++;
                kinds.insert(kind);
                if (i < 4)
                    text += std::format(" [{:08X} {}{} -{}f]", pages[i], kind,
                        w.kind == writewatch::Writer::kResolve ? std::format(" dest {:08X} {}", w.tag, ResolveNote(w.tag))
                        : w.kind != writewatch::Writer::kNone ? std::format(" at {:08X}{}", w.tag, w.pc ? std::format(" pc +{:X}", w.pc) : "")
                        : std::string(), (uint32_t(s_frame) - w.frame) & 0x0FFFFFFF);
            }
            text += " |";
            for (auto& [kind, count] : counts)
                text += std::format(" {} {}", kind, count);
            return text;
        }

        // Before (re)loading `t` (its loadedSequence is still the last load's).
        void LogUpload(const Texture& t, uint32_t format, bool tiled)
        {
            UploadLog& log = *s_uploadLog;
            const char* path = t.scaled ? "scaled" : t.storeView ? "direct" : "staging";
            std::set<std::string> writers;
            std::string line = std::format("[upload] frame {} base {:08X} size {} mip {:08X}/{} {}x{} {}/{} levels {} fmt {}/vk{} tiled {} {} path {} {} reason {}",
                s_frame, t.guestStart, t.guestSize, t.mipStart, t.mipSize, t.width, t.height, t.depth, t.layers, t.levels, format,
                int(t.format), int(tiled), t.cube ? "cube" : t.volume ? "volume" : "2d", path, s_passActive ? "ends-pass" : "between-passes",
                t.uploaded ? "written " + DescribeWrites(t, writers) : "first");
            if (!t.uploaded)
                writers.insert("first");
            auto it = log.resolves.find(t.guestStart);
            line += it == log.resolves.end() ? std::string(" | resolve-dest no")
                : std::format(" | resolve-dest -{}f {}", s_frame - it->second.frame, ResolveNote(t.guestStart));
            fprintf(stderr, "%s\n", line.c_str());
            std::string key;
            for (const std::string& w : writers)
                key += (key.empty() ? "" : "+") + w;
            log.byWriters[key]++;
            log.byPath[path]++;
            log.uploads++;
            log.endedPasses += s_passActive;
            log.bytes += t.guestSize + t.mipSize;
        }

        // NFSMW_TEXTURE_VERIFY=1 (debug; as NFSMW_CP_CACHE_VERIFY): that the
        // write watch sees every change to a texture's memory, which
        // NFSMW_WATCH_SUBPAGES partly finds by comparing instead of faulting.
        // A 1x texture's guest pages are hashed at its load (after Watch
        // protects them), and again at its first use each frame that finds
        // it still valid; a change with no write recorded since the load is
        // logged ([texverify], the first 100). Not checked: scaled textures,
        // pages a GPU write leaves unreadable (resolve destinations), and a
        // texture a resolve stored into (the GPU changes its memory later).
        const bool s_textureVerify = [] { const char* v = std::getenv("NFSMW_TEXTURE_VERIFY"); return v && v[0] == '1'; }();
        uint64_t s_textureMisses = 0;

        // A hash of each 4 KB guest page of `t`'s base, then its mips (the
        // part in the range), as they are now.
        void HashTexturePages(const Texture& t, std::vector<uint64_t>& hashes)
        {
            hashes.clear();
            for (auto [start, size] : { std::pair{ t.guestStart, t.guestSize }, std::pair{ t.mipStart, t.mipSize } })
                for (uint64_t page = start & ~0xFFFull; page < uint64_t(start) + size; page += 0x1000)
                {
                    uint64_t from = std::max<uint64_t>(page, start), to = std::min<uint64_t>(page + 0x1000, uint64_t(start) + size);
                    const uint8_t* p = s_sharedHost + from;
                    size_t n = size_t(to - from), i = 0;
                    // Four lanes: one multiply chain would be latency-bound.
                    uint64_t h[4] = { n, 1, 2, 3 };
                    for (; i + 32 <= n; i += 32)
                        for (int lane = 0; lane < 4; lane++)
                        {
                            uint64_t w;
                            memcpy(&w, p + i + lane * 8, 8);
                            h[lane] = (h[lane] ^ w) * 0x9E3779B97F4A7C15ull;
                            h[lane] ^= h[lane] >> 32;
                        }
                    for (; i < n; i++)
                        h[0] = (h[0] ^ p[i]) * 0x100000001B3ull;
                    hashes.push_back(h[0] ^ std::rotl(h[1], 16) ^ std::rotl(h[2], 32) ^ std::rotl(h[3], 48));
                }
        }

        // NFSMW_TEXTURE_VERIFY: after a load's Watch, what its pages hold (a
        // store from now on faults, or is compared).
        void HashLoadedTexture(Texture& t)
        {
            // Only the GPU guard (NFSMW_EAGER_WRITES, the default, or shadow
            // mode) leaves a resolve's destination unreadable until the CPU
            // next touches it: without it, one would be hashed before the
            // resolve lands, and each later check would report it.
            bool guarded = g_eagerWrites || s_shadowMode;
            LogOnce(0x2900, "%s", guarded ? "NFSMW_TEXTURE_VERIFY: texture pages hashed at each load, checked at the first use each frame"
                : "NFSMW_TEXTURE_VERIFY needs the GPU guard (NFSMW_EAGER_WRITES=0 turns it off): nothing checked");
            t.verifyFrame = s_frame;
            if (!guarded || t.scaled || !writewatch::CpuReadable(t.guestStart, t.guestSize) || !writewatch::CpuReadable(t.mipStart, t.mipSize))
                t.verifyHashes.clear();
            else
                HashTexturePages(t, t.verifyHashes);
        }

        // NFSMW_TEXTURE_VERIFY: a use found `t` valid, deciding at WriteEpoch()
        // `epoch`. Its pages must hold what they did at its load.
        void VerifyTexture(Texture& t, uint64_t epoch)
        {
            if (t.verifyHashes.empty() || t.verifyFrame == s_frame)
                return;
            t.verifyFrame = s_frame;
            if (!writewatch::CpuReadable(t.guestStart, t.guestSize) || !writewatch::CpuReadable(t.mipStart, t.mipSize))
                return;
            std::vector<uint64_t> now;
            HashTexturePages(t, now);
            if (now == t.verifyHashes)
                return;
            // A write recorded since that use decided (another thread, just
            // now) moved the epoch: the next use loads it again.
            if (writewatch::WriteEpoch() != epoch && (writewatch::WrittenSince(t.guestStart, t.guestSize, t.loadedSequence) ||
                    (t.mipSize && writewatch::WrittenSince(t.mipStart, t.mipSize, t.loadedSequence))))
                return;
            if (s_textureMisses++ < 100)
            {
                // The changed pages' addresses, in HashTexturePages' order.
                uint32_t basePages = t.guestSize ? ((t.guestStart + t.guestSize - 1) >> 12) - (t.guestStart >> 12) + 1 : 0;
                std::string pages;
                uint32_t changed = 0;
                for (size_t i = 0; i < now.size(); i++)
                    if (now[i] != t.verifyHashes[i] && changed++ < 4)
                        pages += std::format(" {:08X}", i < basePages ? ((t.guestStart >> 12) + uint32_t(i)) << 12
                            : ((t.mipStart >> 12) + uint32_t(i - basePages)) << 12);
                fprintf(stderr, "[texverify] frame %llu texture %08X+%u mips %08X+%u %ux%u%s vk%d: %u of %zu pages changed since its load "
                    "with no write recorded:%s%s\n", (unsigned long long)s_frame, t.guestStart, t.guestSize, t.mipStart, t.mipSize, t.width,
                    t.height, t.cube ? " cube" : t.volume ? " volume" : "", int(t.format), changed, now.size(), pages.c_str(),
                    changed > 4 ? " ..." : "");
            }
            t.verifyHashes = std::move(now);  // (each change reported once)
        }

        std::unordered_map<uint64_t, VkSampler> s_samplers;
        bool s_loadMips = true;  // Settings > Video > Mipmaps, as applied (ApplyLiveSettings)

        // Anisotropic filtering on linearly filtered textures: 16x on a
        // discrete GPU or an M-series iPad, 4x on other integrated ones (16x
        // cost the M1 Pro ~4 ms a frame in races at 2x2). Settings > Video (NFSMW_ANISOTROPY=<1..16>)
        // changes it live; "game" uses the game's own setting.
        float s_maxAnisotropy = 1.0f;  // device limit (1: no anisotropic filtering)
        int s_autoAnisotropy = 16;      // "Auto": set in CreateSharedMemory

        // `maxLevel`: the texture's last mip level (0: base only).
        VkSampler GetSampler(const xenos::xe_gpu_texture_fetch_t& f, uint32_t maxLevel)
        {
            // The setting is part of the key: a change makes new samplers,
            // while command buffers in flight keep theirs (never destroyed).
            int anisotropy = settings::GetInt(settings::Id::Anisotropy);
            if (anisotropy == settings::kAnisotropyAuto)
                anisotropy = s_autoAnisotropy;
            uint64_t key = uint64_t(f.clamp_x) | (uint64_t(f.clamp_y) << 3) | (uint64_t(f.clamp_z) << 6) |
                (uint64_t(f.mag_filter) << 9) | (uint64_t(f.min_filter) << 11) | (uint64_t(f.border_color) << 13) |
                (uint64_t(f.mip_filter) << 15) | (uint64_t(f.aniso_filter) << 17) | (uint64_t(f.mip_min_level) << 20) |
                (uint64_t(maxLevel) << 24) | (uint64_t(uint8_t(anisotropy)) << 32);
            auto& slot = s_samplers[key];
            if (slot)
                return slot;
            auto address = [](xenos::ClampMode m) {
                switch (m)
                {
                case xenos::ClampMode::kRepeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
                case xenos::ClampMode::kMirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
                case xenos::ClampMode::kClampToBorder:
                case xenos::ClampMode::kMirrorClampToBorder: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
                default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                }
            };
            VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
            sci.magFilter = f.mag_filter == xenos::TextureFilter::kPoint ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
            sci.minFilter = f.min_filter == xenos::TextureFilter::kPoint ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
            sci.addressModeU = address(f.clamp_x);
            sci.addressModeV = address(f.clamp_y);
            sci.addressModeW = address(f.clamp_z);
            // Mips as the fetch constant says: point or linear between
            // levels, or the base level only; its level range and bias.
            sci.mipmapMode = f.mip_filter == xenos::TextureFilter::kLinear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
            bool baseOnly = maxLevel == 0 || f.mip_filter == xenos::TextureFilter::kBaseMap;
            sci.minLod = baseOnly ? 0.0f : float(std::min<uint32_t>(f.mip_min_level, maxLevel));
            sci.maxLod = baseOnly ? 0.0f : float(maxLevel);
            // (The fetch constant's LOD bias is applied in the shader, as
            // Xenia does: MoltenVK has no sampler LOD bias.)
            // Anisotropy (the game's: 1, 2, 4, 8 or 16:1, unless overridden)
            // for linear minification.
            static const float gameAniso[8] = { 1, 1, 2, 4, 8, 16, 1, 1 };
            float aniso = anisotropy > 0 ? float(anisotropy) : gameAniso[uint32_t(f.aniso_filter) & 7];
            aniso = std::min(aniso, s_maxAnisotropy);
            if (aniso > 1.0f && sci.minFilter == VK_FILTER_LINEAR)
            {
                sci.anisotropyEnable = VK_TRUE;
                sci.maxAnisotropy = aniso;
            }
            sci.borderColor = f.border_color == xenos::BorderColor::k_ABGR_White
                ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
            Check(vkCreateSampler(s_dev, &sci, nullptr, &slot), "vkCreateSampler");
            return slot;
        }

        VkImageView TextureView(Texture& t, uint32_t swizzle)
        {
            auto& view = t.views[swizzle];
            if (view)
                return view;
            auto comp = [](uint32_t s) {
                switch (s & 7)
                {
                case 0: return VK_COMPONENT_SWIZZLE_R;
                case 1: return VK_COMPONENT_SWIZZLE_G;
                case 2: return VK_COMPONENT_SWIZZLE_B;
                case 3: return VK_COMPONENT_SWIZZLE_A;
                case 4: return VK_COMPONENT_SWIZZLE_ZERO;
                case 5: return VK_COMPONENT_SWIZZLE_ONE;
                default: return VK_COMPONENT_SWIZZLE_IDENTITY;
                }
            };
            // Sampled only (a storable scaled texture's format need not support storage).
            VkImageViewUsageCreateInfo usage{ VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, nullptr, VK_IMAGE_USAGE_SAMPLED_BIT };
            VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            vci.pNext = t.storeView ? &usage : nullptr;
            vci.image = t.image.image;
            vci.viewType = t.cube ? VK_IMAGE_VIEW_TYPE_CUBE : t.volume ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
            vci.format = t.format;
            vci.components = { comp(swizzle), comp(swizzle >> 3), comp(swizzle >> 6), comp(swizzle >> 9) };
            vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, t.cube ? 6u : 1u };
            Check(vkCreateImageView(s_dev, &vci, nullptr, &view), "vkCreateImageView(texture)");
            return view;
        }

        // The view for texture fetch constant `slot` sampled as 2D or as a
        // cube, or the placeholder. Base level only; each cube face is a
        // tiled 2D slice, 4 KB-aligned (Xenia's GetGuestTextureLayout).
        enum class TexDim { k2D, k3D, kCube };

        // What a lookup's result depends on beyond the fetch constant (for
        // the lookup cache below). `texture`: null when it isn't kept (a
        // placeholder, a scaled texture).
        struct TextureLookup
        {
            Texture* texture = nullptr;
            uint32_t base = 0, size = 0, mipBase = 0, mipSize = 0;
            bool scaledCandidate = false;  // a draw into scaled targets may take the scaled path
        };

        // The full lookup (GetTexture below keeps its results).
        VkImageView LookupTexture(uint32_t slot, TexDim dim, VkSampler& sampler, TextureLookup* out)
        {
            const bool cube = dim == TexDim::kCube, volume = dim == TexDim::k3D;
            xenos::xe_gpu_texture_fetch_t f;
            memcpy(&f, &s_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + slot * 6], sizeof(f));
            sampler = s_sampler;
            const VkImageView dummy = s_dummy[cube ? 2 : volume ? 1 : 0].view;
            if (f.type != xenos::FetchConstantType::kTexture)
                return dummy;
            bool dimensionOk = cube ? f.dimension == xenos::DataDimension::kCube
                : volume ? f.dimension == xenos::DataDimension::k3D
                : (f.dimension == xenos::DataDimension::k2DOrStacked || f.dimension == xenos::DataDimension::k1D);
            if (!dimensionOk)
            {
                LogOnce(0x2000 + uint32_t(f.dimension) + uint32_t(dim) * 8, "texture dimension %u sampled as dimension %u",
                    uint32_t(f.dimension), uint32_t(dim));
                return dummy;
            }
            uint32_t layers = cube ? 6 : 1;
            TextureFormatInfo fi;
            uint32_t format = uint32_t(f.format);
            if (!GetTextureFormat(format, fi))
            {
                LogOnce(0x3000 + format, "texture format %u not implemented", format);
                return dummy;
            }
            if (f.base_address == 0)
            {
                LogOnce(0x2100, "texture without a base level (mips only)");
                return dummy;
            }
            uint32_t width = (f.dimension == xenos::DataDimension::k1D ? f.size_1d.width
                : volume ? f.size_3d.width : f.size_2d.width) + 1;
            uint32_t height = f.dimension == xenos::DataDimension::k1D ? 1 : (volume ? f.size_3d.height : f.size_2d.height) + 1;
            uint32_t depth = volume ? f.size_3d.depth + 1 : 1;

            // Guest layout (Xenia's texture_util).
            uint32_t bw = (width + fi.block - 1) / fi.block, bh = (height + fi.block - 1) / fi.block;
            uint32_t offX = 0, offY = 0;
            uint32_t log2w = Log2Ceil(width), log2h = Log2Ceil(height);
            if (f.packed_mips && std::min(log2w, log2h) <= 4)
            {
                // The base level itself sits in the packed-mip tile.
                if (log2w > log2h)
                    offY = 16 / fi.block;
                else
                    offX = 16 / fi.block;
            }
            uint32_t pitchTexels = f.pitch ? f.pitch << 5 : width;
            uint32_t pitchBlocks = (pitchTexels + fi.block - 1) / fi.block;
            uint32_t base = (f.base_address << 12) & 0x1FFFFFFF;
            if (base == s_logTextureUse)
            {
                s_drawUsesLoggedTexture = true;
                s_loggedTextureFetch = std::format("slot {} dim {} fetch {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}", slot, int(dim),
                    s_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + slot * 6], s_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + slot * 6 + 1],
                    s_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + slot * 6 + 2], s_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + slot * 6 + 3],
                    s_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + slot * 6 + 4], s_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + slot * 6 + 5]);
            }
            uint32_t pitchAligned = 0, rowBytes = 0, size;
            if (f.tiled)
            {
                pitchAligned = (std::max(pitchBlocks, bw + offX) + 31) & ~31u;
                uint32_t heightAligned = (bh + offY + 31) & ~31u;
                size = (pitchAligned * heightAligned) << fi.bpbLog2;
            }
            else
            {
                rowBytes = ((pitchBlocks << fi.bpbLog2) + 255) & ~255u;
                size = rowBytes * (bh + offY);
            }
            // Faces after the first follow at the 4 KB-aligned slice stride.
            uint32_t sliceStride = f.tiled ? (((pitchAligned * ((bh + offY + 31) & ~31u)) << fi.bpbLog2) + 4095) & ~4095u
                                           : ((rowBytes * ((bh + offY + 31) & ~31u)) + 4095) & ~4095u;
            size += sliceStride * (layers - 1);
            uint32_t heightAligned3D = (bh + 31) & ~31u;
            if (volume)
            {
                // Tiled: one Tiled3D volume (depth in groups of 4); linear:
                // slices at the 2D slice stride.
                size = f.tiled ? (pitchAligned * heightAligned3D * ((depth + 3) & ~3u)) << fi.bpbLog2
                               : sliceStride * depth;
            }
            size = std::min(size, uint32_t(kSharedSize) - base);

            // Mip levels (2D and cube): up to mip_max_level, when the texture
            // has a mip address. Settings > Video > Mipmaps off
            // (NFSMW_TEXTURE_MIPS=0) loads the base only; a change drops the
            // cache (ApplyLiveSettings), so every texture is loaded again.
            const bool loadMips = s_loadMips;
            uint32_t maxLevel = 0, mipBase = 0, mipSize = 0;
            MipLayout mips{ width, height, fi.block, fi.bpbLog2, layers, f.tiled != 0, f.packed_mips != 0, 0 };
            if (loadMips && !volume && !f.stacked && f.dimension != xenos::DataDimension::k1D && f.mip_address != 0)
            {
                uint32_t sizeMaxLevel = 31 - uint32_t(__builtin_clz(std::max(width, height)));
                maxLevel = std::min<uint32_t>(f.mip_max_level, sizeMaxLevel);
                if (maxLevel > 0)
                {
                    uint32_t log2Short = Log2Ceil(std::min(width, height));
                    mips.packedLevel = log2Short > 4 ? log2Short - 4 : 0;
                    mipBase = (f.mip_address << 12) & 0x1FFFFFFF;
                    uint32_t lastStorage = mips.StorageLevel(maxLevel);
                    mipSize = mips.Offset(lastStorage) + mips.SliceStride(lastStorage) * layers;
                    mipSize = std::min(mipSize, uint32_t(kSharedSize) - mipBase);
                }
            }
            // Drawn into scaled targets: a texture that scaled resolves wrote
            // (and nothing since) is sampled at host resolution, base level
            // only (resolved surfaces have no mips).
            // (Mipmapped textures keep their levels on the 1x path.)
            bool scaledCandidate = !cube && !volume && f.tiled && layers == 1 && !f.stacked &&
                ScaledFormat(fi) && offX == 0 && offY == 0 && maxLevel == 0 &&
                uint64_t(width) * height * s_scaled.s * 4 <= s_scaled.scratchSize;
            bool scaled = s_drawScaled && s_scaled.enabled && scaledCandidate && ScaledTextureValid(base, size);
            sampler = GetSampler(f, maxLevel);
            if (out)
                *out = { nullptr, base, size, mipBase, mipSize, scaledCandidate };

            uint64_t key = scaled ? 0x5CA1ED5CA1EDull : 0xCBF29CE484222325ull;
            for (uint64_t v : { uint64_t(base), uint64_t(format), uint64_t(width), uint64_t(height), uint64_t(f.tiled),
                     uint64_t(pitchTexels), uint64_t(f.endianness), uint64_t(f.packed_mips), uint64_t(layers), uint64_t(depth),
                     uint64_t(mipBase), uint64_t(maxLevel) })
                key = (key ^ v) * 0x100000001B3ull;
            auto& slotRef = s_textures[key];
            if (!slotRef)
            {
                slotRef = std::make_unique<Texture>();
                Texture& t = *slotRef;
                t.format = fi.vk;
                t.width = width;
                t.height = height;
                t.guestStart = base;
                t.guestSize = size;
                t.cube = cube;
                t.volume = volume;
                t.layers = layers;
                t.depth = depth;
                t.levels = maxLevel + 1;
                t.mipStart = mipBase;
                t.mipSize = mipSize;
                t.scaled = scaled;
                if (scaled)
                {
                    t.width = width * s_scaled.sx;
                    t.height = height * s_scaled.sy;
                    s_scaled.textures++;
                }
                // A 1x texture the untile can store directly (see
                // s_untileStorePipeline): 2D, or a cube (NFSMW_DIRECT_CUBE).
                bool direct1x = !scaled && s_untileDirect && !volume && maxLevel == 0 && fi.block == 1 &&
                    (fi.convert != 0 || fi.bpbLog2 == 2) && (cube ? s_directCube && layers == 6 && !f.stacked : layers == 1) &&
                    DirectFormat(fi.vk, cube);
                if ((scaled && s_scaled.direct) || direct1x)
                {
                    // The untile stores its words through an R32_UINT view
                    // (every scaled format is 32 bits a texel, as is each
                    // direct 1x one), a 2D one of each layer (a cube's faces).
                    VkFormat formats[2] = { fi.vk, VK_FORMAT_R32_UINT };
                    VkImageFormatListCreateInfo list{ VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, nullptr, 2, formats };
                    VkImageViewUsageCreateInfo sampled{ VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, nullptr, VK_IMAGE_USAGE_SAMPLED_BIT };
                    // A cube's image and views are untried on some drivers
                    // (MoltenVK: 2D R32_UINT views of a cube's layers): one
                    // turned down is staged instead, below.
                    t.image = CreateImage(VK_IMAGE_TYPE_2D, fi.vk, { t.width, t.height, 1 }, layers,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | (direct1x ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0),
                        cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT | (cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0),
                        1, &list, &sampled, cube);
                    VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
                    vci.image = t.image.image;
                    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
                    vci.format = VK_FORMAT_R32_UINT;
                    bool made = t.image.image != VK_NULL_HANDLE;
                    for (uint32_t layer = 0; layer < layers && made; layer++)
                    {
                        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1 };
                        VkResult result = vkCreateImageView(s_dev, &vci, nullptr, &t.storeViews[layer]);
                        if (!cube)
                            Check(result, "vkCreateImageView(scaled store)");
                        if (result != VK_SUCCESS)
                        {
                            t.storeViews[layer] = VK_NULL_HANDLE;
                            made = false;
                        }
                    }
                    t.storeView = t.storeViews[0];
                    if (!made)
                    {
                        fprintf(stderr, "[renderer] cube %ux%u format %d can't be stored directly (image or view refused): staged\n",
                            width, height, int(fi.vk));
                        for (VkImageView& view : t.storeViews)
                        {
                            if (view)
                                vkDestroyImageView(s_dev, view, nullptr);
                            view = VK_NULL_HANDLE;
                        }
                        t.storeView = VK_NULL_HANDLE;
                        if (t.image.image)
                            DestroyImage(t.image);
                        s_directCubeFormats[fi.vk] = false;  // (once a format: DirectFormat says no from now on)
                    }
                    else if (direct1x)
                    {
                        t.tiled = f.tiled != 0;
                        t.pitch = f.tiled ? pitchAligned : rowBytes;
                        t.bpbLog2 = fi.bpbLog2;
                        t.convert = fi.convert;
                        t.endian = uint32_t(f.endianness);
                        t.offX = offX;
                        t.offY = offY;
                        t.sliceStride = sliceStride;
                        // (A resolve into a cube takes the face rule, see Resolve.)
                        if (!cube)
                            s_directByBase[base] = &t;
                        if (s_fusePartial || cube)
                        {
                            s_directByStart[base] = &t;
                            s_maxDirectSize = std::max(s_maxDirectSize, t.guestSize);
                        }
                    }
                }
                if (!t.image.image)
                    t.image = CreateImage(volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D, fi.vk, { t.width, t.height, depth }, layers,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        cube ? VK_IMAGE_VIEW_TYPE_CUBE : volume ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT,
                        cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0, t.levels);
            }
            Texture& t = *slotRef;
            {
                // No page written anywhere since the last check: still valid
                // (a draw binds ~7,500 textures a frame; writes are rare).
                uint64_t epoch = writewatch::WriteEpoch();
                bool scan = !(g_cpOpt & CP_OPT_TEXTURE_EPOCH) || epoch != t.checkedEpoch;
                t.checkedEpoch = epoch;
                if (!t.uploaded || (scan && (writewatch::WrittenSince(base, size, t.loadedSequence) ||
                        (mipSize && writewatch::WrittenSince(mipBase, mipSize, t.loadedSequence)))))
                {
                    if (s_uploadLog && s_uploadLog->now)
                        LogUpload(t, format, f.tiled != 0);
                    // Snapshot before recording: a store after this faults and
                    // bumps the sequence; the GPU load itself runs later and
                    // sees anything written before. (NFSMW_WATCH_SUBPAGES: the
                    // snapshots open on its pages are compared first, else the
                    // compare Watch makes below would mark its own pages newer
                    // than this load, and it would load again.)
                    writewatch::Settle(base, size);
                    writewatch::Settle(mipBase, mipSize);
                    t.loadedSequence = writewatch::Current();
                    HitchTimer hitchTimer(s_hitch.textureMs);
                    s_hitch.textures++;
                    if (t.scaled)
                    {
                        s_hitch.scaledTextures++;
                        LoadScaledTexture(t, base, pitchAligned, uint32_t(f.endianness));
                        t.uploaded = true;
                        writewatch::Watch(base, size);  // as the 1x path: a CPU write faults and retires it
                        return TextureView(t, f.swizzle);
                    }
                    s_hitch.textureBytes += size + mipSize;
                    EnsureShadow(base, size);
                    if (mipSize)
                        EnsureShadow(mipBase, mipSize);
                    uint32_t outBpbLog2 = fi.convert == 0 ? fi.bpbLog2 : 2;
                    uint32_t rowWords, rowTexels;
                    if (fi.convert != 0 || fi.bpbLog2 >= 2)
                    {
                        rowWords = (bw << outBpbLog2) / 4;
                        rowTexels = bw * fi.block;
                    }
                    else
                    {
                        uint32_t perWord = 4 >> fi.bpbLog2;
                        rowWords = (bw + perWord - 1) / perWord;
                        rowTexels = rowWords * perWord;
                    }
                    for (uint32_t slice = 0; slice < layers * depth; slice++)
                    {
                        if (t.storeView)  // stored directly (one 2D slice, or a cube's face: see s_untileStorePipeline)
                        {
                            EndPass();
                            Begin();
                            UntileConstants uc{ base + slice * sliceStride, f.tiled ? 1u : 0u, f.tiled ? pitchAligned : rowBytes, fi.bpbLog2,
                                offX, offY, bw, bh, uint32_t(f.endianness), fi.convert, 0, 0, 0, 0 };
                            VkDescriptorImageInfo ii{ VK_NULL_HANDLE, t.storeViews[slice], VK_IMAGE_LAYOUT_GENERAL };
                            VkWriteDescriptorSet w[5] = {
                                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &s_sharedInfo[0], nullptr },
                                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii, nullptr, nullptr },
                            };
                            if (!t.uploaded && slice == 0)
                                ToGeneral(t.image.image, VK_IMAGE_ASPECT_COLOR_BIT, layers);
                            s_stateEpoch++;
                            vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_untileStorePipeline);
                            s_pushDescriptorSet(s_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_untileStoreLayout, 0, AddSharedParts(w, 2), w);
                            vkCmdPushConstants(s_cmd, s_untileStoreLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uc), &uc);
                            if (!(s_exp & 4))
                                vkCmdDispatch(s_cmd, (bw + 7) / 8, (bh + 7) / 8, 1);
                            FullBarrier();
                            SubmitIfStarving();
                            continue;
                        }
                        VkDeviceSize staging = RingAllocate(VkDeviceSize(rowWords) * 4 * bh);
                        if (staging == ~VkDeviceSize(0))
                        {
                            LogOnce(0x2200, "texture %ux%u format %u is too large for a staging slot", width, height, format);
                            return dummy;
                        }
                        EndPass();
                        Begin();
                        uint32_t layer = volume ? 0 : slice, zSlice = volume ? slice : 0;
                        uint32_t sliceBase = base + (volume ? (f.tiled ? 0 : zSlice * sliceStride) : layer * sliceStride);
                        UntileConstants uc{ sliceBase, f.tiled ? 1u : 0u, f.tiled ? pitchAligned : rowBytes, fi.bpbLog2,
                            offX, offY, bw, bh, uint32_t(f.endianness), fi.convert, uint32_t(staging / 4), rowWords,
                            zSlice, heightAligned3D };
                        if (volume && f.tiled)
                            uc.tiled = 2;
                        VkDescriptorBufferInfo ring{ s_ring, 0, kRingSize };
                        VkWriteDescriptorSet w[5] = {
                            { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &s_sharedInfo[0], nullptr },
                            { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &ring, nullptr },
                        };
                        s_stateEpoch++;
                        vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_untilePipeline);
                        s_pushDescriptorSet(s_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_untileLayout, 0, AddSharedParts(w, 2), w);
                        vkCmdPushConstants(s_cmd, s_untileLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uc), &uc);
                        uint32_t groupsX = fi.convert != 0 || fi.bpbLog2 >= 2 ? bw : rowWords;
                        if (!(s_exp & 4))
                            vkCmdDispatch(s_cmd, (groupsX + 7) / 8, (bh + 7) / 8, 1);
                        FullBarrier();
                        if (!t.uploaded && slice == 0)
                            ToGeneral(t.image.image, VK_IMAGE_ASPECT_COLOR_BIT, layers);
                        VkBufferImageCopy region{};
                        region.bufferOffset = staging;
                        region.bufferRowLength = rowTexels;
                        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1 };
                        region.imageOffset = { 0, 0, int32_t(zSlice) };
                        region.imageExtent = { width, height, 1 };
                        if (!(s_exp & 16))  // NFSMW_GPU_EXP bit 16: no staging copy (upload cost experiment)
                            vkCmdCopyBufferToImage(s_cmd, s_ring, t.image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
                        FullBarrier();
                        SubmitIfStarving();
                    }
                    // Mip levels 1..maxLevel, each face from its storage.
                    for (uint32_t level = 1; level <= maxLevel; level++)
                    {
                        uint32_t lw = std::max(width >> level, 1u), lh = std::max(height >> level, 1u);
                        uint32_t lbw = (lw + fi.block - 1) / fi.block, lbh = (lh + fi.block - 1) / fi.block;
                        uint32_t storage = mips.StorageLevel(level);
                        uint32_t levelBase = mipBase + mips.Offset(storage);
                        uint32_t levelOffX, levelOffY;
                        mips.PackedOffset(level, levelOffX, levelOffY);
                        uint32_t levelRowWords, levelRowTexels;
                        if (fi.convert != 0 || fi.bpbLog2 >= 2)
                        {
                            levelRowWords = (lbw << outBpbLog2) / 4;
                            levelRowTexels = lbw * fi.block;
                        }
                        else
                        {
                            uint32_t perWord = 4 >> fi.bpbLog2;
                            levelRowWords = (lbw + perWord - 1) / perWord;
                            levelRowTexels = levelRowWords * perWord;
                        }
                        for (uint32_t layer = 0; layer < layers; layer++)
                        {
                            VkDeviceSize staging = RingAllocate(VkDeviceSize(levelRowWords) * 4 * lbh);
                            if (staging == ~VkDeviceSize(0))
                                break;
                            EndPass();
                            Begin();
                            UntileConstants uc{ levelBase + layer * mips.SliceStride(storage), f.tiled ? 1u : 0u,
                                f.tiled ? mips.PitchBlocks(storage) : mips.RowBytes(storage), fi.bpbLog2,
                                levelOffX, levelOffY, lbw, lbh, uint32_t(f.endianness), fi.convert, uint32_t(staging / 4), levelRowWords,
                                0, 0 };
                            VkDescriptorBufferInfo ring{ s_ring, 0, kRingSize };
                            VkWriteDescriptorSet w[5] = {
                                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &s_sharedInfo[0], nullptr },
                                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &ring, nullptr },
                            };
                            s_stateEpoch++;
                            vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_untilePipeline);
                            s_pushDescriptorSet(s_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_untileLayout, 0, AddSharedParts(w, 2), w);
                            vkCmdPushConstants(s_cmd, s_untileLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uc), &uc);
                            uint32_t groupsX = fi.convert != 0 || fi.bpbLog2 >= 2 ? lbw : levelRowWords;
                            vkCmdDispatch(s_cmd, (groupsX + 7) / 8, (lbh + 7) / 8, 1);
                            FullBarrier();
                            VkBufferImageCopy region{};
                            region.bufferOffset = staging;
                            region.bufferRowLength = levelRowTexels;
                            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1 };
                            region.imageExtent = { lw, lh, 1 };
                            if (!(s_exp & 16))  // NFSMW_GPU_EXP bit 16: no staging copy (upload cost experiment)
                                vkCmdCopyBufferToImage(s_cmd, s_ring, t.image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
                            FullBarrier();
                        }
                    }
                    t.uploaded = true;
                    writewatch::Watch(base, size);
                    GuardGpu(base, size, false, "texture", true);  // the untile runs later
                    if (mipSize)
                    {
                        writewatch::Watch(mipBase, mipSize);
                        GuardGpu(mipBase, mipSize, false, "texture mips", true);
                    }
                    if (s_textureVerify)
                        HashLoadedTexture(t);
                    s_stats.uploads++;
                }
                else if (s_textureVerify)
                {
                    VerifyTexture(t, epoch);
                }
            }
            if (out && !t.scaled)
                out->texture = &t;
            return TextureView(t, f.swizzle);
        }

        // Texture lookups kept per fetch slot and dimension. Dense scenes
        // look up 5,000-10,000 textures a frame, nearly all with a fetch
        // constant seen very recently, and the full lookup (layout, three
        // hash maps, the sampler) was ~16% of the command processor (Deck
        // profile). An entry is a lookup's result, used again while the
        // fetch constant's 6 dwords are the same and nothing else it depends
        // on moved: the texture cache (DropTextures), any setting
        // (anisotropy is in the sampler) and the scaled resolve memory bump
        // `generation`; for a texture the scaled path could take, whether
        // the draw renders into scaled targets is part of the key. Validity
        // is the full lookup's: unchanged while no page anywhere was written
        // (WriteEpoch), else checked in place (the texture's write
        // sequences, and whether a scaled copy became current) instead of
        // repeating the whole lookup. Each slot keeps its last lookup; a
        // table by the fetch constant's hash keeps textures that alternate
        // in a slot (about a sixth of the lookups in the career intro).
        // Placeholders and scaled textures aren't kept. NFSMW_TEXTURE_CACHE=0
        // turns it off; NFSMW_CP_CACHE_VERIFY=1 also does the full lookup on
        // every hit and logs any difference (it then returns the full
        // lookup's result).
        struct TextureCacheEntry
        {
            uint32_t fetch[6] = {};
            uint32_t generation = 0;  // TextureCache::generation when made; 0: empty
            uint8_t dim = 0;
            bool scaledCandidate = false, drawScaled = false;
            uint64_t epoch = 0;  // WriteEpoch() it was last valid at
            Texture* texture = nullptr;
            VkImageView view = VK_NULL_HANDLE;
            VkSampler sampler = VK_NULL_HANDLE;
            uint32_t base = 0, size = 0, mipBase = 0, mipSize = 0;
        };
        struct TextureCache
        {
            TextureCacheEntry slots[3][32];
            TextureCacheEntry recent[1024];
            uint32_t generation = 1;
            uint64_t settingsGeneration = 0;
            bool on = false, verify = false;
            uint64_t hits = 0, recentHits = 0, revalidated = 0, misses = 0, stale = 0, uncached = 0, mismatches = 0, raced = 0;
        };
        // On the heap (as the shader cache): the renderer's globals move by a pointer, not by the tables (4K aliasing, see NOTES).
        TextureCache* const s_textureCache = [] {
            auto* c = new TextureCache();
            const char* v = std::getenv("NFSMW_TEXTURE_CACHE");
            // (Built on the epoch rule: without it the full lookup rescans every time.)
            c->on = (g_cpOpt & CP_OPT_TEXTURE_CACHE) && (g_cpOpt & CP_OPT_TEXTURE_EPOCH) && !(v && v[0] == '0') && s_logTextureUse == 0;
            c->verify = std::getenv("NFSMW_CP_CACHE_VERIFY") != nullptr;
            return c;
        }();

        enum class TextureEntryState { kOtherKey, kStale, kValid };

        // Whether entry `e` holds this lookup's result now (see above).
        // `checked`: the texture's checkedEpoch before this looked.
        TextureEntryState CheckTextureEntry(TextureCacheEntry& e, TexDim dim, const uint32_t* fetch, bool drawScaled, uint64_t epoch,
            uint64_t& checked)
        {
            TextureCache& c = *s_textureCache;
            if (e.generation != c.generation || e.dim != uint8_t(dim) || memcmp(e.fetch, fetch, sizeof(e.fetch)) != 0 ||
                (e.scaledCandidate && e.drawScaled != drawScaled))
                return TextureEntryState::kOtherKey;
            Texture& t = *e.texture;
            checked = t.checkedEpoch;
            if (e.epoch == epoch)
                return TextureEntryState::kValid;
            // Some page was written since: the full lookup's checks, in its
            // order (its scaled copy first).
            bool valid = !(e.scaledCandidate && drawScaled && ScaledTextureValid(e.base, e.size)) &&
                (checked == epoch || (t.uploaded && writewatch::WriteSequence(e.base, e.size) <= t.loadedSequence &&
                    (!e.mipSize || writewatch::WriteSequence(e.mipBase, e.mipSize) <= t.loadedSequence)));
            if (!valid)
            {
                c.stale++;
                return TextureEntryState::kStale;
            }
            t.checkedEpoch = epoch;
            e.epoch = epoch;
            c.revalidated++;
            return TextureEntryState::kValid;
        }

        TextureCacheEntry& RecentTextureEntry(const uint32_t* fetch, TexDim dim)
        {
            uint64_t a, b, d;
            memcpy(&a, fetch, 8);
            memcpy(&b, fetch + 2, 8);
            memcpy(&d, fetch + 4, 8);
            uint64_t h = (a * 0x9E3779B97F4A7C15ull) ^ (b * 0xC2B2AE3D27D4EB4Full) ^ (d * 0x165667B19E3779F9ull) ^ uint64_t(dim);
            return s_textureCache->recent[(h * 0x9E3779B97F4A7C15ull) >> 54];
        }

        VkImageView GetTexture(uint32_t slot, TexDim dim, VkSampler& sampler)
        {
            TextureCache& c = *s_textureCache;
            if (!c.on)
                return LookupTexture(slot, dim, sampler, nullptr);
            const uint32_t* fetch = &s_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + slot * 6];
            TextureCacheEntry& e = c.slots[uint32_t(dim)][slot & 31];
            const bool drawScaled = s_drawScaled && s_scaled.enabled;
            const uint64_t epoch = writewatch::WriteEpoch();
            uint64_t checked = 0;
            TextureEntryState state = CheckTextureEntry(e, dim, fetch, drawScaled, epoch, checked);
            if (state == TextureEntryState::kOtherKey)
            {
                TextureCacheEntry& r = RecentTextureEntry(fetch, dim);
                state = CheckTextureEntry(r, dim, fetch, drawScaled, epoch, checked);
                if (state == TextureEntryState::kValid)
                {
                    e = r;
                    c.recentHits++;
                }
            }
            if (state == TextureEntryState::kValid)
            {
                c.hits++;
                if (s_textureVerify)
                    VerifyTexture(*e.texture, epoch);
                if (!c.verify)
                {
                    sampler = e.sampler;
                    return e.view;
                }
                // The full lookup as if this hit hadn't happened: a reload
                // it does is one the hit would have missed, unless a page
                // was written during the check (another thread): the full
                // lookup then saw memory the hit, a moment earlier, didn't.
                e.texture->checkedEpoch = checked;
                uint32_t loads = s_hitch.textures;
                TextureLookup lookup;
                VkImageView view = LookupTexture(slot, dim, sampler, &lookup);
                bool reloaded = s_hitch.textures != loads;
                if (reloaded && writewatch::WriteEpoch() != epoch)
                {
                    c.raced++;
                    reloaded = false;
                }
                if (view != e.view || sampler != e.sampler || lookup.texture != e.texture || reloaded)
                {
                    if (c.mismatches++ < 50)
                        fprintf(stderr, "[cpverify] texture slot %u dim %u: cached view %p sampler %p texture %p, full lookup %p %p %p%s "
                            "(fetch %08X %08X %08X %08X %08X %08X, epoch %llu, entry epoch %llu, scaled %d/%d)\n",
                            slot, uint32_t(dim), (void*)e.view, (void*)e.sampler, (void*)e.texture, (void*)view, (void*)sampler,
                            (void*)lookup.texture, reloaded ? ", reloaded" : "", fetch[0], fetch[1], fetch[2], fetch[3],
                            fetch[4], fetch[5], (unsigned long long)epoch, (unsigned long long)e.epoch, int(e.scaledCandidate), int(drawScaled));
                    e.generation = 0;
                    RecentTextureEntry(fetch, dim).generation = 0;
                }
                return view;
            }
            c.misses++;
            TextureLookup lookup;
            VkImageView view = LookupTexture(slot, dim, sampler, &lookup);
            if (!lookup.texture)
            {
                c.uncached++;
                e.generation = 0;
                return view;
            }
            memcpy(e.fetch, fetch, sizeof(e.fetch));
            e.generation = c.generation;
            e.dim = uint8_t(dim);
            e.scaledCandidate = lookup.scaledCandidate;
            e.drawScaled = drawScaled;
            e.epoch = epoch;  // read before the lookup: a write meanwhile is checked next time
            e.texture = lookup.texture;
            e.view = view;
            e.sampler = sampler;
            e.base = lookup.base;
            e.size = lookup.size;
            e.mipBase = lookup.mipBase;
            e.mipSize = lookup.mipSize;
            RecentTextureEntry(fetch, dim) = e;
            return view;
        }

        // ---------------------------------------------------------------
        // Resolves

        VkPipeline s_resolvePipeline = VK_NULL_HANDLE;
        VkPipeline s_resolveCoalescedPipeline = VK_NULL_HANDLE;  // scaled resolves (see COALESCED in the shader)
        VkPipelineLayout s_resolveLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout s_resolveSetLayout = VK_NULL_HANDLE;

        struct ResolveConstants
        {
            int32_t originX, originY;
            uint32_t width, height;
            uint32_t destBase, destPitch;
            uint32_t format, endian;
            uint32_t flags;  // bit 0: swap red/blue; bit 1: depth source
            float expScale;
            uint32_t srcWidth, srcHeight;
            int32_t srcOriginX, srcOriginY;  // host pixels
            uint32_t scaleX, scaleY;         // host pixels per guest pixel in the source
            uint32_t scaledWrite, slotWords; // also write the samples to the scaled memory
            // Also store into a cached direct texture (binding 4), as its
            // untile would have from the words (see Resolve); texX, texY:
            // where the destination's texel 0,0 is in it (NFSMW_FUSE_PARTIAL,
            // NFSMW_DIRECT_CUBE; pushed only with either, see s_resolvePush).
            uint32_t texWrite, texEndian, texConvert, texWidth, texHeight;
            uint32_t texX, texY;
        };
        // The constants pushed to the resolve shader: without NFSMW_FUSE_PARTIAL
        // and NFSMW_DIRECT_CUBE, those before texX, as before them, and the
        // shader is built without TEX_OFFSET (the same code and push).
        const uint32_t s_resolvePush = s_fusePartial || s_directCube ? sizeof(ResolveConstants) : offsetof(ResolveConstants, texX);

        const char* kResolveGlsl = R"(#version 460
#extension GL_EXT_samplerless_texture_functions : require
layout(local_size_x = 8, local_size_y = 8) in;
layout(std430, set = 0, binding = 0) buffer SharedMemory { uint g_mem[]; };)" GLSL_SHARED_MEMORY_WRITE R"(
layout(set = 0, binding = 1) uniform texture2D src;
layout(std430, set = 0, binding = 2) buffer ScaledPool { uint s_pool[]; };
layout(std430, set = 0, binding = 3) readonly buffer ScaledTable { uint s_table[]; };
layout(set = 0, binding = 4, r32ui) writeonly uniform uimage2D t_image;
layout(push_constant) uniform P
{
    ivec2 origin;
    uvec2 size;
    uint destBase, destPitch, format, endian, flags;
    float expScale;
    uvec2 srcSize;
    ivec2 srcOrigin;    // where the rectangle is in the host target (see untiled rendering), host pixels
    uvec2 scale;        // host pixels per guest pixel (internal resolution)
    uint scaledWrite, slotWords;
    uint texWrite, texEndian, texConvert, texWidth, texHeight;  // also into a cached texture (binding 4)
#ifdef TEX_OFFSET
    uint texX, texY;    // the destination's texel 0,0 in it (whole tiles)
#endif
} p;

uint tiled2D(uint x, uint y, uint pitch, uint bppLog2)
{
    uint outer = (((y >> 5) * (pitch >> 5)) + (x >> 5)) << 6;
    uint inner = (((y >> 1) & 7u) << 3) | (x & 7u);
    uint oi = (outer | inner) << bppLog2;
    uint bank = (y >> 4) & 1u;
    uint pipe = ((x >> 3) & 3u) ^ (((y >> 3) & 1u) << 1);
    return ((y & 1u) << 4) | (pipe << 6) | (bank << 11) | (oi & 0xFu) | (((oi >> 4) & 1u) << 5) |
        (((oi >> 5) & 7u) << 8) | ((oi >> 8) << 12);
}

uint swapWord(uint v, uint e)
{
    if (e == 1u) return ((v << 8) & 0xFF00FF00u) | ((v >> 8) & 0x00FF00FFu);
    if (e == 2u || e >= 4u) return (v << 24) | ((v << 8) & 0x00FF0000u) | ((v >> 8) & 0x0000FF00u) | (v >> 24);
    if (e == 3u) return (v >> 16) | (v << 16);
    return v;
}

uint bppLog2(uint f)
{
    if (f == 2u) return 0u;                                  // 8
    if (f == 3u || f == 4u || f == 10u || f == 15u || f == 24u || f == 30u) return 1u;
    if (f == 26u || f == 32u || f == 34u || f == 37u) return 3u;
    return 2u;
}

vec4 px(uint x, uint y)
{
    // Guest pixel (x, y) of the rectangle: its sx x sy block of host pixels,
    // averaged (colour) or its first sample (depth).
    ivec2 c = clamp(p.srcOrigin + ivec2(x * p.scale.x, y * p.scale.y), ivec2(0), ivec2(p.srcSize) - 1);
    vec4 v = texelFetch(src, c, 0);
    if ((p.flags & 2u) != 0u) return v;
    if (p.scale.x * p.scale.y > 1u)
    {
        for (uint j = 0u; j < p.scale.y; j++)
            for (uint i = 0u; i < p.scale.x; i++)
                if (i + j > 0u)
                    v += texelFetch(src, clamp(c + ivec2(i, j), ivec2(0), ivec2(p.srcSize) - 1), 0);
        v /= float(p.scale.x * p.scale.y);
    }
    v *= p.expScale;
    if ((p.flags & 1u) != 0u) v = v.bgra;
    return v;
}

uint unorm(float v, float m) { return uint(clamp(v, 0.0, 1.0) * m + 0.5); }

uint pack32(vec4 c)
{
    switch (p.format)
    {
    case 6u: case 14u: return packUnorm4x8(c);
    case 7u: return unorm(c.r, 1023.0) | (unorm(c.g, 1023.0) << 10) | (unorm(c.b, 1023.0) << 20) | (unorm(c.a, 3.0) << 30);
    case 22u: case 23u: return unorm(c.r, 16777215.0) << 8;
    case 25u: return packSnorm2x16(c.rg);
    case 31u: return packHalf2x16(c.rg);
    case 33u: case 36u: return floatBitsToUint(c.r);
    default: return packUnorm4x8(c);
    }
}

uint pack16(vec4 c)
{
    switch (p.format)
    {
    case 3u: return unorm(c.r, 31.0) | (unorm(c.g, 31.0) << 5) | (unorm(c.b, 31.0) << 10) | (unorm(c.a, 1.0) << 15);
    case 4u: return unorm(c.r, 31.0) | (unorm(c.g, 63.0) << 5) | (unorm(c.b, 31.0) << 11);
    case 15u: return unorm(c.r, 15.0) | (unorm(c.g, 15.0) << 4) | (unorm(c.b, 15.0) << 8) | (unorm(c.a, 15.0) << 12);
    case 10u: return unorm(c.r, 255.0) | (unorm(c.g, 255.0) << 8);
    case 30u: return packHalf2x16(vec2(c.r, 0.0)) & 0xFFFFu;
    default: return unorm(c.r, 65535.0);
    }
}

#ifdef COALESCED
// A scaled resolve's samples stored coalesced: a group is 32x2 pixels, its
// threads in the order of their words in the 32-bit tiled layout (64
// consecutive words when aligned), and the samples go to the pool through
// shared memory so that each store writes consecutive pool words. Each
// thread storing its own pixel's samples writes words `s` apart across the
// group, several times the memory transactions: most of a scaled resolve's
// cost. (A pipeline of its own: the shared memory would slow the others.)
shared uint g_samples[64 * 16];  // by pixel, then sample (scale up to 4x4)
shared uint g_offsets[64];       // each pixel's samples in the pool; ~0u: none
#endif

void main()
{
    uint bl = bppLog2(p.format);
    uint perWord = bl >= 2u ? 1u : (4u >> bl);
#ifdef COALESCED
    uint l = gl_LocalInvocationIndex;
    uvec2 id = gl_WorkGroupID.xy * uvec2(32u, 2u) + uvec2((l & 3u) | ((l >> 3u) << 2u), (l >> 2u) & 1u);
#else
    uvec2 id = gl_GlobalInvocationID.xy;
#endif
    uint gx = id.x * perWord, gy = id.y;
    bool inside = gx < p.size.x && gy < p.size.y;
#ifndef COALESCED
    if (!inside) return;  // (a coalesced write needs every thread at its barrier)
#endif
    uint x = uint(p.origin.x) + gx, y = uint(p.origin.y) + gy;
    uint word = ((p.destBase + tiled2D(x, y, p.destPitch, bl)) & 0x1FFFFFFFu) >> 2;
    uint w;
    if (bl == 2u && p.scaledWrite != 0u)
    {
        // Each host sample into the page's scaled slot (see ScaledMemory).
        uint a = word << 2;
        uint slot = inside ? s_table[a >> 12] : 0u;
        uint s = p.scale.x * p.scale.y;
        uint o = (slot - 1u) * p.slotWords + ((a & 4095u) >> 2) * s;
        if (slot != 0u)
        {
            ivec2 c0 = p.srcOrigin + ivec2(gx * p.scale.x, gy * p.scale.y);
            for (uint j = 0u; j < p.scale.y; j++)
                for (uint i = 0u; i < p.scale.x; i++)
                {
                    vec4 v = texelFetch(src, clamp(c0 + ivec2(i, j), ivec2(0), ivec2(p.srcSize) - 1), 0);
                    if ((p.flags & 2u) == 0u)
                    {
                        v *= p.expScale;
                        if ((p.flags & 1u) != 0u) v = v.bgra;
                    }
#ifdef COALESCED
                    g_samples[l * s + j * p.scale.x + i] = swapWord(pack32(v), p.endian);
#else
                    s_pool[o + j * p.scale.x + i] = swapWord(pack32(v), p.endian);
#endif
                }
        }
#ifdef COALESCED
        g_offsets[l] = slot != 0u ? o : ~0u;
        barrier();
        for (uint q = l; q < 64u * s; q += 64u)
        {
            uint po = g_offsets[q / s];
            if (po != ~0u)
                s_pool[po + q % s] = g_samples[q];
        }
#endif
    }
    if (!inside) return;
    if (bl == 3u)
    {
        vec4 c = px(gx, gy);
        uint w0, w1;
        if (p.format == 26u) { w0 = packSnorm2x16(c.rg); w1 = packSnorm2x16(c.ba); }
        else if (p.format == 32u) { w0 = packHalf2x16(c.rg); w1 = packHalf2x16(c.ba); }
        else { w0 = floatBitsToUint(c.r); w1 = floatBitsToUint(c.g); }
        if (p.endian == 4u) { uint t = w0; w0 = swapWord(w1, 2u); w1 = swapWord(t, 2u); }
        else { w0 = swapWord(w0, p.endian); w1 = swapWord(w1, p.endian); }
        memStore(word, w0);
        memStore(word + 1u, w1);
        return;
    }
    if (bl == 2u) w = pack32(px(gx, gy));
    else if (bl == 1u) w = pack16(px(gx, gy)) | (pack16(px(gx + 1u, gy)) << 16);
    else w = unorm(px(gx, gy).r, 255.0) | (unorm(px(gx + 1u, gy).r, 255.0) << 8) |
        (unorm(px(gx + 2u, gy).r, 255.0) << 16) | (unorm(px(gx + 3u, gy).r, 255.0) << 24);
    uint m = swapWord(w, p.endian);
    memStore(word, m);
    // A cached direct 1x texture of this surface: its texel as the untile
    // would read the word (its endianness, D24 -> float). Or (TEX_OFFSET) one
    // holding it whole tiles in: 32-bit texels make a 4 KB tile, and
    // tiled2D's address is that tile's index in bits 12 and up, the rest from
    // x & 31 and y & 31 only, so the destination's x, y is the texture's
    // x + texX, y + texY.
#ifdef TEX_OFFSET
    if (p.texWrite != 0u && bl == 2u && x + p.texX < p.texWidth && y + p.texY < p.texHeight)
#else
    if (p.texWrite != 0u && bl == 2u && x < p.texWidth && y < p.texHeight)
#endif
    {
        uint v = swapWord(m, p.texEndian);
        uint o = p.texConvert == 4u ? floatBitsToUint(float(v >> 8) / 16777215.0) : v;
#ifdef TEX_OFFSET
        imageStore(t_image, ivec2(int(x + p.texX), int(y + p.texY)), uvec4(o, 0u, 0u, 0u));
#else
        imageStore(t_image, ivec2(int(x), int(y)), uvec4(o, 0u, 0u, 0u));
#endif
    }
}
)";

        uint32_t ResolveBppLog2(uint32_t f)
        {
            if (f == 2) return 0;
            if (f == 3 || f == 4 || f == 10 || f == 15 || f == 24 || f == 30) return 1;
            if (f == 26 || f == 32 || f == 34 || f == 37) return 3;
            return 2;
        }

        // The guest bytes a resolve of `r` writes, as offsets from the
        // destination base: the 4 KB blocks of the 32x32 tiles the rectangle
        // touches (Tiled2D puts the tile index, shifted by the bytes per
        // pixel, in address bits 12 and up). Not every row of the pitch down
        // to a 32-row boundary: at world entry NFSMW resolves the right
        // 640x720 half of a 1280x720 texture through a destination base 20
        // tiles into it, so whole rows from there ran 80 KB past the
        // texture, into Direct3D's command ring right after it. In shadow
        // mode (discrete GPUs) the pages a resolve marks are read back whole
        // from the GPU copy, which put old bytes over the game's newest
        // commands: a malformed indirect buffer, a hang.
        // NFSMW_RESOLVE_EXTENT=0 marks whole rows again.
        std::pair<uint32_t, uint32_t> ResolveExtent(const Rect& r, uint32_t pitchAligned, uint32_t bppLog2)
        {
            static const bool tiles = [] { const char* v = std::getenv("NFSMW_RESOLVE_EXTENT"); return !v || v[0] != '0'; }();
            if (!tiles)
                return { 0, (pitchAligned * ((uint32_t(r.y1) + 31) & ~31u)) << bppLog2 };
            uint32_t tilesPerRow = pitchAligned >> 5;
            uint32_t first = (uint32_t(r.y0) >> 5) * tilesPerRow + (uint32_t(r.x0) >> 5);
            uint32_t last = (uint32_t(r.y1 - 1) >> 5) * tilesPerRow + (uint32_t(r.x1 - 1) >> 5);
            return { ((first << (6 + bppLog2)) >> 8) << 12, (((((last + 1) << (6 + bppLog2)) - 1) >> 8) + 1) << 12 };
        }

        bool ResolveFormatSupported(uint32_t f)
        {
            switch (f)
            {
            case 2: case 3: case 4: case 6: case 7: case 10: case 14: case 15: case 22: case 23: case 24:
            case 25: case 26: case 30: case 31: case 32: case 33: case 34: case 36: case 37:
                return true;
            default:
                return false;
            }
        }

        int32_t ToFixed16p8(float f)
        {
            float v = std::clamp(f, -8388608.0f / 256.0f, 8388607.0f / 256.0f) * 256.0f;
            return int32_t(v + (v < 0 ? -0.5f : 0.5f));
        }

        // Clear a rectangle of a target (resolve with clear enabled).
        void ClearRect(RenderTarget* rt, const Rect& r)
        {
            VkClearAttachment ca{};
            if (rt->depth)
            {
                uint32_t v = s_regs[XE_GPU_REG_RB_DEPTH_CLEAR];
                ca.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
                float depth;
                if (xenos::DepthRenderTargetFormat(rt->format) == xenos::DepthRenderTargetFormat::kD24FS8)
                {
                    // 20e4 float: 4-bit exponent (bias 15), 20-bit mantissa.
                    uint32_t f24 = v >> 8;
                    uint32_t e = f24 >> 20, m = f24 & 0xFFFFF;
                    depth = e == 0 ? std::ldexp(float(m), -34) : std::ldexp(1.0f + float(m) / 1048576.0f, int(e) - 15);
                }
                else
                {
                    depth = float(v >> 8) / 16777215.0f;
                }
                ca.clearValue.depthStencil = { std::clamp(depth, 0.0f, 1.0f), v & 0xFF };
            }
            else
            {
                uint32_t lo = s_regs[XE_GPU_REG_RB_COLOR_CLEAR];
                uint32_t hi = s_regs[XE_GPU_REG_RB_COLOR_CLEAR_LO];
                ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                ca.colorAttachment = 0;
                float* c = ca.clearValue.color.float32;
                auto half = [](uint32_t h) {
                    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1F, m = h & 0x3FF;
                    float v = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? 65504.0f : std::ldexp(1.0f + m / 1024.0f, int(e) - 15);
                    return s ? -v : v;
                };
                switch (rt->vkFormat)
                {
                case VK_FORMAT_R8G8B8A8_UNORM:
                    for (int i = 0; i < 4; i++)
                        c[i] = float((lo >> (8 * i)) & 0xFF) / 255.0f;
                    break;
                case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
                    c[0] = float(lo & 0x3FF) / 1023.0f;
                    c[1] = float((lo >> 10) & 0x3FF) / 1023.0f;
                    c[2] = float((lo >> 20) & 0x3FF) / 1023.0f;
                    c[3] = float(lo >> 30) / 3.0f;
                    break;
                case VK_FORMAT_R16G16_SFLOAT:
                    c[0] = half(lo & 0xFFFF);
                    c[1] = half(lo >> 16);
                    break;
                case VK_FORMAT_R16G16B16A16_SFLOAT:
                    c[0] = half(lo & 0xFFFF);
                    c[1] = half(lo >> 16);
                    c[2] = half(hi & 0xFFFF);
                    c[3] = half(hi >> 16);
                    break;
                case VK_FORMAT_R32_SFLOAT:
                    memcpy(&c[0], &lo, 4);
                    break;
                case VK_FORMAT_R32G32_SFLOAT:
                    memcpy(&c[0], &lo, 4);
                    memcpy(&c[1], &hi, 4);
                    break;
                default:
                    for (int i = 0; i < 4; i++)
                        c[i] = 0.0f;
                    break;
                }
            }
            // `r` is in guest pixels; the clear covers its host pixels.
            int32_t x0 = std::clamp(r.x0, 0, int32_t(rt->guestWidth)) * int32_t(rt->scaleX);
            int32_t y0 = std::clamp(r.y0, 0, int32_t(rt->guestHeight)) * int32_t(rt->scaleY);
            int32_t x1 = std::clamp(r.x1, 0, int32_t(rt->guestWidth)) * int32_t(rt->scaleX);
            int32_t y1 = std::clamp(r.y1, 0, int32_t(rt->guestHeight)) * int32_t(rt->scaleY);
            x1 = std::max(x1, x0);
            y1 = std::max(y1, y0);
            if (x1 <= x0 || y1 <= y0)
                return;
            // Deferred (see s_pendingClears); a pass open on the target now
            // must end so the clear lands before its next use.
            if (s_passActive && (s_passDepth == rt || std::find(s_passColor, s_passColor + 4, rt) != s_passColor + 4))
                EndPass();
            VkRect2D rect{ { x0, y0 }, { uint32_t(x1 - x0), uint32_t(y1 - y0) } };
            // NFSMW_CLEAR_FLUSH_RECT: tile bands left pending by the resolves
            // between them become one clear (a load-op clear when it covers the
            // next pass). Only with the target's newest pending clear, so no
            // clear of the target lies between the two (values compared as
            // bytes: `ca` is zeroed, so the bytes a format leaves unused agree).
            if (s_clearFlushRect)
                for (size_t i = s_pendingClears.size(); i-- > 0;)
                    if (s_pendingClears[i].rt == rt)
                    {
                        if (!memcmp(&s_pendingClears[i].value, &ca.clearValue, sizeof(VkClearValue)) &&
                            Adjoins(s_pendingClears[i].rect, rect))
                        {
                            rect = Union(s_pendingClears[i].rect, rect);
                            s_pendingClears.erase(s_pendingClears.begin() + i);
                        }
                        break;
                    }
            // A newer clear covering an older one replaces it.
            std::erase_if(s_pendingClears, [&](const PendingClear& pc) { return pc.rt == rt && Contains(rect, pc.rect); });
            s_pendingClears.push_back({ rt, rect, ca.clearValue });
        }

        void Resolve()
        {
            s_stats.resolves++;
            auto copyControl = Reg<reg::RB_COPY_CONTROL>();
            auto surfaceInfo = Reg<reg::RB_SURFACE_INFO>();
            uint32_t pitch = surfaceInfo.surface_pitch;
            if (pitch == 0)
                return;

            // The rectangle comes from the three vertices in vertex fetch 0
            // (Direct3D 9 writes them with the CPU).
            uint32_t fetch0 = s_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0];
            uint32_t fetch1 = s_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_1];
            uint32_t vertexAddress = fetch0 & 0x1FFFFFFC;
            uint32_t vertexEndian = fetch1 & 3;
            if ((fetch0 & 3) != 3 || ((fetch1 >> 2) & 0xFFFFFF) != 6)
            {
                LogOnce(0x100, "resolve with unexpected vertex fetch constant %08X %08X", fetch0, fetch1);
                return;
            }
            float halfPixel = Reg<reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero ? 0.5f : 0.0f;
            int32_t v[6];
            for (int i = 0; i < 6; i++)
            {
                uint32_t bits = GpuSwap(LoadPhysical(vertexAddress + i * 4), vertexEndian);
                float f;
                memcpy(&f, &bits, 4);
                v[i] = ToFixed16p8(f + halfPixel);
            }
            Rect r;
            r.x0 = (std::min({ v[0], v[2], v[4] }) + 127) >> 8;
            r.y0 = (std::min({ v[1], v[3], v[5] }) + 127) >> 8;
            r.x1 = (std::max({ v[0], v[2], v[4] }) + 127) >> 8;
            r.y1 = (std::max({ v[1], v[3], v[5] }) + 127) >> 8;
            if (Reg<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable)
            {
                auto wo = Reg<reg::PA_SC_WINDOW_OFFSET>();
                r.x0 += wo.window_x_offset;
                r.x1 += wo.window_x_offset;
                r.y0 += wo.window_y_offset;
                r.y1 += wo.window_y_offset;
            }
            Rect sc = GuestScissor();
            r.x0 = std::clamp(r.x0, sc.x0, sc.x1);
            r.x1 = std::clamp(r.x1, sc.x0, sc.x1);
            r.y0 = std::clamp(r.y0, sc.y0, sc.y1);
            r.y1 = std::clamp(r.y1, sc.y0, sc.y1);
            r.x0 &= ~7;
            r.y0 &= ~7;
            r.x1 = (r.x1 + 7) & ~7;
            r.y1 = (r.y1 + 7) & ~7;
            r.x1 = std::min(r.x1, int32_t(pitch & ~7u));
            r.x0 = std::min(r.x0, r.x1);
            if (r.x1 <= r.x0 || r.y1 <= r.y0)
                return;

            bool depthSource = copyControl.copy_src_select >= 4;
            // NFSMW_LOG_RESOLVES=1: describe the first resolves in detail.
            // NFSMW_LOG_RESOLVES_PITCH=<p>: only resolves from surfaces of that pitch.
            static const bool logResolves = std::getenv("NFSMW_LOG_RESOLVES") != nullptr;
            static const int logPitch = [] { const char* v = std::getenv("NFSMW_LOG_RESOLVES_PITCH"); return v ? std::atoi(v) : -1; }();
            static uint32_t logged = 0;
            if ((logResolves || (logPitch >= 0 && pitch == uint32_t(logPitch))) && logged < 200)
            {
                logged++;
                auto wo = Reg<reg::PA_SC_WINDOW_OFFSET>();
                fprintf(stderr, "[resolve] rect (%d,%d)-(%d,%d) wo (%d,%d) woEn %u src %u pitch %u msaa %u | dest %08X pitch %u h %u info %08X "
                    "(fmt %u endian %u swap %u bias %d) | ctl %08X color %08X depth %08X clear c%08X d%08X\n",
                    r.x0, r.y0, r.x1, r.y1, wo.window_x_offset, wo.window_y_offset,
                    uint32_t(Reg<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable), copyControl.copy_src_select, pitch,
                    uint32_t(surfaceInfo.msaa_samples), s_regs[XE_GPU_REG_RB_COPY_DEST_BASE],
                    Reg<reg::RB_COPY_DEST_PITCH>().copy_dest_pitch, Reg<reg::RB_COPY_DEST_PITCH>().copy_dest_height,
                    s_regs[XE_GPU_REG_RB_COPY_DEST_INFO], uint32_t(Reg<reg::RB_COPY_DEST_INFO>().copy_dest_format),
                    uint32_t(Reg<reg::RB_COPY_DEST_INFO>().copy_dest_endian), uint32_t(Reg<reg::RB_COPY_DEST_INFO>().copy_dest_swap),
                    int(Reg<reg::RB_COPY_DEST_INFO>().copy_dest_exp_bias), copyControl.value, s_regs[XE_GPU_REG_RB_COLOR_INFO],
                    s_regs[XE_GPU_REG_RB_DEPTH_INFO], s_regs[XE_GPU_REG_RB_COLOR_CLEAR], s_regs[XE_GPU_REG_RB_DEPTH_CLEAR]);
                if (logPitch >= 0)
                    for (int i = 0; i < 6; i++)
                    {
                        uint32_t bits = GpuSwap(LoadPhysical(vertexAddress + i * 4), vertexEndian);
                        float f;
                        memcpy(&f, &bits, 4);
                        fprintf(stderr, "[resolve]   v%d.%c = %.1f\n", i / 2, "xy"[i & 1], f);
                    }
            }
            RenderTarget* source;
            if (depthSource)
            {
                auto di = Reg<reg::RB_DEPTH_INFO>();
                source = GetTarget(di.depth_base, uint32_t(di.depth_format), true, pitch);
            }
            else
            {
                static const uint32_t infoRegs[4] = { XE_GPU_REG_RB_COLOR_INFO, XE_GPU_REG_RB_COLOR1_INFO,
                    XE_GPU_REG_RB_COLOR2_INFO, XE_GPU_REG_RB_COLOR3_INFO };
                auto ci = RegAt<reg::RB_COLOR_INFO>(infoRegs[copyControl.copy_src_select & 3]);
                source = GetTarget(ci.color_base, uint32_t(ci.color_format), false, pitch);
            }

            // Copy.
            auto destInfo = Reg<reg::RB_COPY_DEST_INFO>();
            auto destPitch = Reg<reg::RB_COPY_DEST_PITCH>();
            uint32_t destBase = s_regs[XE_GPU_REG_RB_COPY_DEST_BASE];
            uint32_t format = depthSource
                ? (xenos::DepthRenderTargetFormat(Reg<reg::RB_DEPTH_INFO>().depth_format) == xenos::DepthRenderTargetFormat::kD24FS8 ? 23u : 22u)
                : uint32_t(destInfo.copy_dest_format);
            bool copy = copyControl.copy_command == xenos::CopyCommand::kRaw ||
                copyControl.copy_command == xenos::CopyCommand::kConvert;
            if (copy && destBase != 0)
            {
                auto [extentStart, extentEnd] = ResolveExtent(r, (destPitch.copy_dest_pitch + 31) & ~31u, ResolveBppLog2(format));
                uint32_t rangeBase = (destBase & 0x1FFFFFFF) + extentStart, rangeBytes = extentEnd - extentStart;
                // Shadow mode: pages the resolve covers only in part keep
                // the CPU's latest data around it.
                EnsureShadow(rangeBase, rangeBytes);
                if (destInfo.copy_dest_array)
                    LogOnce(0x200, "resolve to a 3D/array destination is not implemented (copying slice 0)");
                if (!ResolveFormatSupported(format))
                {
                    LogOnce(0x300 + format, "resolve to texture format %u not implemented", format);
                }
                else
                {
                    EndPass();
                    Begin();
                    // NFSMW_UNTILED_AREA: a resolve reading, inside what the
                    // untiled draws cut to their viewports could reach (their
                    // attachments' common size), past what they drew in the
                    // last frame untiled draws drew its source (a resolve
                    // after the swap reads that frame's), reads pixels the
                    // host used to overdraw (and the console never drew).
                    // None seen in menus, loading, pre-race or races; should
                    // one appear, that target's draws keep the whole target
                    // from then on, so it differs from before for this one
                    // frame. Blind: a cut inside another (their union is the
                    // larger). Needless (costing only the saving): a band only
                    // a clear covered, a target drawn since by non-untiled draws.
                    if (s_untiledArea && source->cut && !source->wholeArea)
                    {
                        auto [shiftX, shiftY] = TileShift();
                        int32_t x0 = std::max((r.x0 + shiftX) * int32_t(source->scaleX), 0);
                        int32_t y0 = std::max((r.y0 + shiftY) * int32_t(source->scaleY), 0);
                        int32_t x1 = std::min({ (r.x1 + shiftX) * int32_t(source->scaleX), int32_t(source->width), int32_t(source->cutReach.width) });
                        int32_t y1 = std::min({ (r.y1 + shiftY) * int32_t(source->scaleY), int32_t(source->height), int32_t(source->cutReach.height) });
                        VkRect2D read{ { x0, y0 }, { uint32_t(std::max(x1 - x0, 0)), uint32_t(std::max(y1 - y0, 0)) } };
                        if (read.extent.width && read.extent.height && !Contains(source->cutDrawn, read))
                        {
                            source->wholeArea = true;
                            s_ua.resolvesPast++;
                            LogOnce((0x31ull << 56) | (uint64_t(source->base) << 24) | (uint64_t(source->pitch) << 1) | uint64_t(source->depth),
                                "untiled area: frame %llu resolve of target b%u p%u%s reads %ux%u@%d,%d, past what its cut draws drew (%ux%u@%d,%d "
                                "in frame %llu; dest %08X): its draws keep the whole target from now on",
                                (unsigned long long)s_frame, source->base, source->pitch, source->depth ? " (depth)" : "", read.extent.width,
                                read.extent.height, read.offset.x, read.offset.y, source->cutDrawn.extent.width, source->cutDrawn.extent.height,
                                source->cutDrawn.offset.x, source->cutDrawn.offset.y, (unsigned long long)source->areaFrame, destBase);
                        }
                    }
                    if (s_clearFlushRect)
                    {
                        // What the shader reads (px() and the scaled path:
                        // size x scale host pixels from rc.srcOrigin, each
                        // axis clamped to the target), so the clears it
                        // doesn't see stay pending.
                        auto [shiftX, shiftY] = TileShift();
                        auto texel = [](int32_t v, uint32_t size) { return std::clamp(v, 0, int32_t(size) - 1); };
                        int32_t x0 = texel((r.x0 + shiftX) * int32_t(source->scaleX), source->width);
                        int32_t y0 = texel((r.y0 + shiftY) * int32_t(source->scaleY), source->height);
                        int32_t x1 = texel((r.x1 + shiftX) * int32_t(source->scaleX) - 1, source->width) + 1;
                        int32_t y1 = texel((r.y1 + shiftY) * int32_t(source->scaleY) - 1, source->height) + 1;
                        VkRect2D read{ { x0, y0 }, { uint32_t(x1 - x0), uint32_t(y1 - y0) } };
                        FlushPendingClears(source, &read);
                    }
                    else
                    {
                        FlushPendingClears(source);
                    }
                    EndPass();
                    Begin();
                    ResolveConstants rc{};
                    rc.originX = r.x0;
                    rc.originY = r.y0;
                    rc.width = uint32_t(r.x1 - r.x0);
                    rc.height = uint32_t(r.y1 - r.y0);
                    rc.destBase = destBase & 0x1FFFFFFF;
                    rc.destPitch = (destPitch.copy_dest_pitch + 31) & ~31u;
                    rc.format = format;
                    rc.endian = uint32_t(destInfo.copy_dest_endian);
                    rc.flags = (destInfo.copy_dest_swap ? 1u : 0u) | (depthSource ? 2u : 0u);
                    rc.expScale = std::ldexp(1.0f, destInfo.copy_dest_exp_bias);
                    rc.srcWidth = source->width;
                    rc.srcHeight = source->height;
                    auto [shiftX, shiftY] = TileShift();
                    rc.srcOriginX = (r.x0 + shiftX) * int32_t(source->scaleX);
                    rc.srcOriginY = (r.y0 + shiftY) * int32_t(source->scaleY);
                    rc.scaleX = source->scaleX;
                    rc.scaleY = source->scaleY;
                    uint32_t bl = ResolveBppLog2(format);
                    uint32_t firstPage = rangeBase >> 12;
                    uint32_t lastPage = std::min<uint32_t>((rangeBase + rangeBytes - 1) >> 12, kGuestPages - 1);
                    // A 32-bit resolve from a scaled target keeps its samples.
                    bool scaledWrite = source->scaleX * source->scaleY > 1 && bl == 2 && !destInfo.copy_dest_array &&
                        EnsureScaledMemory() && source->scaleX == s_scaled.sx && source->scaleY == s_scaled.sy;
                    if (scaledWrite)
                    {
                        std::vector<std::pair<uint32_t, uint32_t>> fresh;
                        for (uint32_t page = firstPage; page <= lastPage; page++)
                        {
                            // A slot whose copy went stale (a CPU write, a 1x
                            // resolve) starts over from the 1x data too: the
                            // resolve may not cover all of it.
                            bool stale = s_scaled.pageSlot[page] != 0 && !ScaledPageValid(page);
                            bool isFresh;
                            uint32_t slot = ScaledSlot(page, isFresh);
                            if (isFresh || stale)
                                fresh.push_back({ page, slot });
                        }
                        FlushScaledTable();
                        FillFreshSlots(fresh);
                        rc.scaledWrite = 1;
                        rc.slotWords = s_scaled.slotWords;
                        s_scaled.resolvedBytes += uint64_t(rc.width) * rc.height * s_scaled.s * 4;
                    }

                    // A cached direct 1x texture of exactly this surface (its
                    // address, tiling, pitch and 32-bit texels), current until
                    // now, whose memory this resolve rewrites: the resolve
                    // stores its texels too, and the texture stays current. The
                    // Steam Frame uploaded each 1600x1600 shadow map again after
                    // its resolve (~0.4 ms, twice a frame).
                    // Else (NFSMW_FUSE_PARTIAL, NFSMW_DIRECT_CUBE) one holding
                    // the destination whole 32x32 tiles in, and all this resolve
                    // writes (a strip of the scene, a cube's face): the same,
                    // at those tiles' texels (see the shader). Its other texels
                    // must still be what it loaded: checked again after
                    // MarkWritten, below.
                    Texture* fused = nullptr;
                    Texture* direct = nullptr;
                    bool partial = false;
                    uint32_t layer = 0, texX = 0, texY = 0;  // partial: the face, and the destination's texel 0,0 in it
                    // Why not (NFSMW_LOG_UPLOADS).
                    const char* unfused = !s_resolveToTexture ? "off" : scaledWrite ? "scaled resolve" : bl != 2 ? "not 32-bit"
                        : "no direct texture there";
                    // Whether `t` can take this resolve's words as its texels at all.
                    auto unfit = [&](const Texture& t) -> const char* {
                        return !(t.uploaded && t.storeView && !t.scaled) ? "texture not loaded"
                            : !t.tiled || t.bpbLog2 != 2 || (t.convert != 0 && t.convert != 4) ? "texture layout"
                            : t.pitch != rc.destPitch ? "pitch differs"
                            : t.offX != 0 || t.offY != 0 ? "texture offset"
                            : nullptr;
                    };
                    if (!scaledWrite && bl == 2 && s_resolveToTexture)
                        if (auto it = s_directByBase.find(rc.destBase); it != s_directByBase.end())
                        {
                            Texture& t = *(direct = it->second);
                            unfused = unfit(t);
                            if (!unfused)
                                unfused = !(t.guestStart >= rangeBase && uint64_t(t.guestStart) + t.guestSize <= uint64_t(rangeBase) + rangeBytes)
                                    ? "texture beyond the resolve's range"
                                    : writewatch::WrittenSince(t.guestStart, t.guestSize, t.loadedSequence) ? "texture written since its load"
                                    : nullptr;
                            if (!unfused)
                                fused = &t;
                        }
                    if (!fused && !scaledWrite && bl == 2 && s_resolveToTexture && (s_fusePartial || s_directCube))
                    {
                        // Each direct texture holding the destination, the
                        // nearest start first, until one takes it: textures
                        // are kept (dead ones too) until the cache is dropped,
                        // so an older one may start inside a larger one. Why
                        // not: the nearest's reason, or the nearest's that
                        // could take its words at all (unfit).
                        Texture* why = nullptr;
                        bool whyFits = false;
                        for (auto it = s_directByStart.upper_bound(rc.destBase); !fused && it != s_directByStart.begin();)
                        {
                            if (rc.destBase - (--it)->first >= s_maxDirectSize)
                                break;  // none starting earlier reaches it
                            Texture& t = *it->second;
                            if (rc.destBase - t.guestStart >= t.guestSize)
                                continue;
                            // Its face holding the destination (a cube's: its
                            // layers are the guest's faces in order, as the
                            // staging path copies them), and the tile there:
                            // 32-bit texels make a tile 4 KB, so a destination
                            // whole tiles in is the texture's texels from
                            // (tile % tiles a row, tile / tiles a row) x 32,
                            // as the untile reads them.
                            uint32_t face = t.cube ? (rc.destBase - t.guestStart) / t.sliceStride : 0;
                            uint32_t sliceStart = t.guestStart + face * t.sliceStride;
                            uint64_t sliceEnd = uint64_t(t.guestStart) + t.guestSize;
                            if (t.cube)
                                sliceEnd = std::min<uint64_t>(sliceEnd, uint64_t(sliceStart) + t.sliceStride);
                            uint32_t rel = rc.destBase - sliceStart, tilesPerRow = std::max(t.pitch >> 5, 1u);
                            uint32_t x = (rel >> 12) % tilesPerRow * 32, y = (rel >> 12) / tilesPerRow * 32;
                            const char* reason = unfit(t);
                            bool fits = !reason;
                            if (fits)
                                reason = face >= t.layers ? "texture layout"
                                    : rel % 4096 != 0 ? "not whole tiles into the texture"
                                    : (rel != 0 || !t.cube) && !s_fusePartial ? "partial fusion off"
                                    : x + uint32_t(r.x1) > rc.destPitch ? "beyond the texture's columns"
                                    : !(rangeBase >= sliceStart && uint64_t(rangeBase) + rangeBytes <= sliceEnd)
                                    ? (t.cube ? "beyond the face" : "beyond the texture")
                                    : writewatch::WrittenSince(t.guestStart, t.guestSize, t.loadedSequence) ? "texture written since its load"
                                    : nullptr;
                            if (!reason)
                            {
                                fused = direct = &t;
                                unfused = nullptr;
                                partial = true;
                                layer = face;
                                texX = x;
                                texY = y;
                            }
                            else if (!why || (fits && !whyFits))
                            {
                                why = direct = &t;
                                whyFits = fits;
                                unfused = reason;
                            }
                        }
                    }
                    if (s_uploadLog)
                    {
                        UploadLog& log = *s_uploadLog;
                        log.resolves[rc.destBase] = { s_frame, unfused };
                        if (log.now)
                        {
                            log.resolvesNow++;
                            log.fusedNow += fused != nullptr;
                            std::string part = !partial ? std::string()
                                : fused->cube ? std::format(" (face {}, texels from {},{})", layer, texX, texY)
                                : std::format(" (texels from {},{})", texX, texY);
                            fprintf(stderr, "[upload] frame %llu resolve dest %08X range %08X+%u rect %ux%u fmt %u pitch %u %s %s%s",
                                (unsigned long long)s_frame, rc.destBase, rangeBase, rangeBytes, rc.width, rc.height, format, rc.destPitch,
                                depthSource ? "depth" : "color", fused ? "fused" : "unfused: ", fused ? part.c_str() : unfused);
                            std::set<std::string> kinds;
                            if (direct)
                                fprintf(stderr, " | texture %08X+%u %ux%u pitch %u%s%s", direct->guestStart, direct->guestSize, direct->width,
                                    direct->height, direct->pitch, direct->uploaded ? " " : "",
                                    direct->uploaded ? DescribeWrites(*direct, kinds).c_str() : "");
                            fprintf(stderr, "\n");
                        }
                    }
                    if (fused)
                    {
                        rc.texWrite = 1;
                        rc.texEndian = fused->endian;
                        rc.texConvert = fused->convert;
                        rc.texWidth = fused->width;
                        rc.texHeight = fused->height;
                        rc.texX = texX;
                        rc.texY = texY;
                        LogOnce(0x2400, "resolves store into cached textures too (the first: %ux%u at %08X)", fused->width,
                            fused->height, rc.destBase);
                        if (partial)
                            LogOnce(0x2800, "resolves store into part of cached textures too (the first: %ux%u%s at %08X, texels from %u,%u)",
                                fused->width, fused->height, fused->cube ? " cube" : "", rc.destBase, texX, texY);
                    }

                    VkDescriptorImageInfo ii{ VK_NULL_HANDLE, source->sampleView, VK_IMAGE_LAYOUT_GENERAL };
                    VkDescriptorImageInfo ti{ VK_NULL_HANDLE, fused ? fused->storeViews[layer] : s_dummyStorage.view, VK_IMAGE_LAYOUT_GENERAL };
                    VkDescriptorBufferInfo pool = DummyStorage(s_scaled.pool);
                    VkDescriptorBufferInfo table = DummyStorage(s_scaled.table);
                    VkWriteDescriptorSet w[8]{};
                    w[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 0, 0, 1,
                        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &s_sharedInfo[0], nullptr };
                    w[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 1, 0, 1,
                        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &ii, nullptr, nullptr };
                    w[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 2, 0, 1,
                        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &pool, nullptr };
                    w[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 3, 0, 1,
                        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &table, nullptr };
                    w[4] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 4, 0, 1,
                        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ti, nullptr, nullptr };
                    s_stateEpoch++;
                    bool coalesced = scaledWrite && s_scaled.coalesce;  // 32x2 pixels a group
                    vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, coalesced ? s_resolveCoalescedPipeline : s_resolvePipeline);
                    s_pushDescriptorSet(s_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_resolveLayout, 0, AddSharedParts(w, 5), w);
                    vkCmdPushConstants(s_cmd, s_resolveLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, s_resolvePush, &rc);
                    uint32_t perWord = bl >= 2 ? 1 : (4 >> bl);
                    if (!(s_exp & 2))
                        vkCmdDispatch(s_cmd, coalesced ? (rc.width + 31) / 32 : (rc.width / perWord + 7) / 8,
                            coalesced ? (rc.height + 1) / 2 : (rc.height + 7) / 8, 1);
                    if (scaledWrite)
                        FullBarrier();  // (above 1x: as before)
                    else
                        ResolveBarrier(rangeBase, rangeBytes, fused ? fused->guestStart : 0);
                    SubmitIfStarving();
                    // Write sequences are per host page: guest pages of the
                    // edge host pages outside this resolve whose scaled copy
                    // was current stay current (this write isn't theirs).
                    std::vector<uint32_t> siblings;
                    if (s_scaled.enabled && s_scaled.hostPageGuestPages > 1)
                    {
                        uint32_t g = s_scaled.hostPageGuestPages;
                        for (uint32_t edge : { firstPage, lastPage })
                            for (uint32_t q = edge / g * g; q < edge / g * g + g && q < kGuestPages; q++)
                                if ((q < firstPage || q > lastPage) && ScaledPageValid(q))
                                    siblings.push_back(q);
                    }
                    uint64_t written = writewatch::MarkWritten(rangeBase, rangeBytes, writewatch::Writer::kResolve, rc.destBase);
                    // The fused texture holds this write: its pages' sequence
                    // now, and any write after it is newer (0: per-page
                    // sequences, NFSMW_WATCH_RANGES=0, so it is loaded again).
                    // A partial one's pages outside this range keep theirs,
                    // which the check above found no newer than its load: a
                    // CPU store there since (a fault racing this) is older
                    // than `written`, and would never be loaded. Then it stays
                    // as it was (its range is newer now: it loads again). The
                    // exact rule's texture is all in the range. Quiesce first:
                    // a fault that took its sequence before `written` may not
                    // have stored it on its pages yet.
                    uint32_t rangeEnd = rangeBase + rangeBytes;
                    if (partial)
                        writewatch::Quiesce();
                    bool keep = !partial || (!writewatch::WrittenSince(fused->guestStart, rangeBase - fused->guestStart, fused->loadedSequence) &&
                        !writewatch::WrittenSince(rangeEnd, fused->guestStart + fused->guestSize - rangeEnd, fused->loadedSequence));
                    if (fused && written && keep)
                    {
                        fused->loadedSequence = written;
                        fused->verifyHashes.clear();  // (NFSMW_TEXTURE_VERIFY: its memory is the GPU's to change now)
                    }
                    if (fused && !keep && s_uploadLog)
                    {
                        s_uploadLog->resolves[rc.destBase].unfused = "texture written outside it meanwhile";
                        if (s_uploadLog->now)
                        {
                            s_uploadLog->fusedNow--;
                            fprintf(stderr, "[upload] frame %llu resolve dest %08X: stored into %08X, but the texture was written outside "
                                "it meanwhile: loaded again\n", (unsigned long long)s_frame, rc.destBase, fused->guestStart);
                        }
                    }
                    // CPU writes must fault to move the sequence on (and so
                    // retire the scaled copy), whatever else protects the pages.
                    if (scaledWrite)
                        writewatch::Watch(rangeBase, rangeBytes);
                    if (s_scaled.enabled)
                    {
                        for (uint32_t page = firstPage; page <= lastPage; page++)
                            s_scaled.pageSeq[page] = scaledWrite ? writewatch::WriteSequence(page << 12, 4096) : 0;
                        for (uint32_t q : siblings)
                            s_scaled.pageSeq[q] = writewatch::WriteSequence(q << 12, 4096);
                    }
                    GuardGpu(rangeBase, rangeBytes, true, "resolve");
                    writewatch::MarkGpuOwned(rangeBase, rangeBytes);
                }
            }

            // Clears (in host target coordinates, see TileShift).
            {
                auto [shiftX, shiftY] = TileShift();
                r.x0 += shiftX;
                r.x1 += shiftX;
                r.y0 += shiftY;
                r.y1 += shiftY;
            }
            if (copyControl.color_clear_enable && !depthSource)
                ClearRect(source, r);
            if (copyControl.depth_clear_enable)
            {
                auto di = Reg<reg::RB_DEPTH_INFO>();
                ClearRect(GetTarget(di.depth_base, uint32_t(di.depth_format), true, pitch), r);
            }
            if (copyControl.color_clear_enable && depthSource)
                LogOnce(0x400, "color clear requested with a depth resolve source");
        }

        void CreateComputePipeline(const char* glsl, const VkDescriptorSetLayoutBinding* bindings, uint32_t bindingCount,
            uint32_t pushSize, VkDescriptorSetLayout& setLayout, VkPipelineLayout& layout, VkPipeline& pipeline,
            const char* preamble = "")
        {
            std::string log;
            auto spirv = CompileGlsl(glsl, GLSLANG_STAGE_COMPUTE, log, preamble);
            if (spirv.empty())
            {
                fprintf(stderr, "[renderer] compute shader failed:\n%s\n", log.c_str());
                abort();
            }
            VkDescriptorSetLayoutCreateInfo dslci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            dslci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
            dslci.bindingCount = bindingCount;
            dslci.pBindings = bindings;
            Check(vkCreateDescriptorSetLayout(s_dev, &dslci, nullptr, &setLayout), "compute set layout");
            VkPushConstantRange range{ VK_SHADER_STAGE_COMPUTE_BIT, 0, pushSize };
            VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            plci.setLayoutCount = 1;
            plci.pSetLayouts = &setLayout;
            plci.pushConstantRangeCount = 1;
            plci.pPushConstantRanges = &range;
            Check(vkCreatePipelineLayout(s_dev, &plci, nullptr, &layout), "compute pipeline layout");
            VkShaderModule module = CreateModule(spirv);
            VkComputePipelineCreateInfo cpci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
            cpci.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT,
                module, "main", nullptr };
            cpci.layout = layout;
            Check(vkCreateComputePipelines(s_dev, s_pipelineCache, 1, &cpci, nullptr, &pipeline), "compute pipeline");
        }

        // Scaled memory's GPU side (see ScaledMemory): made on the first
        // scaled resolve.
        const char* kScaledFillGlsl = R"(#version 460
layout(local_size_x = 64) in;
layout(std430, set = 0, binding = 0) readonly buffer SharedMemory { uint g_mem[]; };)" GLSL_SHARED_MEMORY_READ R"(
layout(std430, set = 0, binding = 1) buffer ScaledPool { uint s_pool[]; };
layout(std430, set = 0, binding = 2) readonly buffer List { uint l_pairs[]; };
layout(push_constant) uniform P { uint count, s, slotWords, listWord; } p;
// A new slot starts as its page's 1x data, each word repeated s times: a
// resolve covering the page only in part leaves no garbage around it.
void main()
{
    uint n = gl_GlobalInvocationID.x;
    uint pair = n >> 10, k = n & 1023u;
    if (pair >= p.count) return;
    uint page = l_pairs[p.listWord + pair * 2u], slot = l_pairs[p.listWord + pair * 2u + 1u];
    uint v = memLoad(page * 1024u + k);
    uint o = slot * p.slotWords + k * p.s;
    for (uint i = 0u; i < p.s; i++)
        s_pool[o + i] = v;
}
)";
        VkPipeline s_scaledFillPipeline = VK_NULL_HANDLE;
        VkPipelineLayout s_scaledFillLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout s_scaledFillSetLayout = VK_NULL_HANDLE;

        // A scaled texture's texels from the scaled memory (the 32-bit tiled
        // layout of the guest texture, each texel's samples): stored into the
        // texture through its R32_UINT view, or (TO_BUFFER) linear into the
        // scratch buffer for a copy.
        const char* kScaledUntileGlsl = R"(#version 460
layout(local_size_x = 8, local_size_y = 8) in;
layout(std430, set = 0, binding = 0) readonly buffer ScaledPool { uint s_pool[]; };
layout(std430, set = 0, binding = 1) readonly buffer ScaledTable { uint s_table[]; };
#ifdef TO_BUFFER
layout(std430, set = 0, binding = 2) writeonly buffer Out { uint o_mem[]; };
#else
layout(set = 0, binding = 2, r32ui) writeonly uniform uimage2D o_image;
#endif
layout(push_constant) uniform P { uint base, pitch, width, height, sx, sy, endian, slotWords; } p;

uint tiled2D(uint x, uint y, uint pitch, uint bppLog2)
{
    uint outer = (((y >> 5) * (pitch >> 5)) + (x >> 5)) << 6;
    uint inner = (((y >> 1) & 7u) << 3) | (x & 7u);
    uint oi = (outer | inner) << bppLog2;
    uint bank = (y >> 4) & 1u;
    uint pipe = ((x >> 3) & 3u) ^ (((y >> 3) & 1u) << 1);
    return ((y & 1u) << 4) | (pipe << 6) | (bank << 11) | (oi & 0xFu) | (((oi >> 4) & 1u) << 5) |
        (((oi >> 5) & 7u) << 8) | ((oi >> 8) << 12);
}

uint swapWord(uint v, uint e)
{
    if (e == 1u) return ((v << 8) & 0xFF00FF00u) | ((v >> 8) & 0x00FF00FFu);
    if (e == 2u) return (v << 24) | ((v << 8) & 0x00FF0000u) | ((v >> 8) & 0x0000FF00u) | (v >> 24);
    if (e == 3u) return (v >> 16) | (v << 16);
    return v;
}

void main()
{
    uvec2 h = gl_GlobalInvocationID.xy;
    uint w = p.width * p.sx;
    if (h.x >= w || h.y >= p.height * p.sy) return;
    uint gx = h.x / p.sx, gy = h.y / p.sy;
    uint a = (p.base + tiled2D(gx, gy, p.pitch, 2u)) & 0x1FFFFFFFu;
    uint slot = s_table[a >> 12];
    uint v = 0u;
    if (slot != 0u)
        v = s_pool[(slot - 1u) * p.slotWords + ((a & 4095u) >> 2) * (p.sx * p.sy) + (h.y - gy * p.sy) * p.sx + (h.x - gx * p.sx)];
#ifdef TO_BUFFER
    o_mem[h.y * w + h.x] = swapWord(v, p.endian);
#else
    imageStore(o_image, ivec2(h), uvec4(swapWord(v, p.endian)));
#endif
}
)";
        VkPipeline s_scaledUntilePipeline = VK_NULL_HANDLE;  // TO_BUFFER
        VkPipelineLayout s_scaledUntileLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout s_scaledUntileSetLayout = VK_NULL_HANDLE;
        VkPipeline s_scaledStorePipeline = VK_NULL_HANDLE;  // into the image
        VkPipelineLayout s_scaledStoreLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout s_scaledStoreSetLayout = VK_NULL_HANDLE;

        bool CreateDeviceBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer, VkDeviceMemory& memory, const char* what,
            VkDeviceSize* allocated = nullptr)
        {
            VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bci.size = size;
            bci.usage = usage;
            if (vkCreateBuffer(s_dev, &bci, nullptr, &buffer) != VK_SUCCESS)
            {
                fprintf(stderr, "[renderer] %s: vkCreateBuffer failed\n", what);
                return false;
            }
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(s_dev, buffer, &req);
            VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            mai.allocationSize = req.size;
            mai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (vkAllocateMemory(s_dev, &mai, nullptr, &memory) != VK_SUCCESS)
            {
                fprintf(stderr, "[renderer] %s: vkAllocateMemory (%llu MB) failed\n", what, (unsigned long long)(size >> 20));
                vkDestroyBuffer(s_dev, buffer, nullptr);
                buffer = VK_NULL_HANDLE;
                return false;
            }
            vkBindBufferMemory(s_dev, buffer, memory, 0);
            if (allocated)
                *allocated = req.size;
            return true;
        }

        // Scaled-texture checks by range, redone only when something was
        // written since (the write epoch moved).
        struct ScaledCheck
        {
            uint64_t epoch = ~0ull;
            bool valid = false;
        };
        std::unordered_map<uint64_t, ScaledCheck> s_scaledChecks;

        // The pool, page table and scratch buffer (the caller has the GPU idle).
        void FreeScaledBuffers()
        {
            for (auto [buffer, memory] : { std::pair{ &s_scaled.pool, &s_scaled.poolMemory }, std::pair{ &s_scaled.table, &s_scaled.tableMemory },
                     std::pair{ &s_scaled.scratch, &s_scaled.scratchMemory } })
            {
                if (*buffer)
                    vkDestroyBuffer(s_dev, *buffer, nullptr);
                if (*memory)
                    vkFreeMemory(s_dev, *memory, nullptr);
                *buffer = VK_NULL_HANDLE;
                *memory = VK_NULL_HANDLE;
            }
            s_scaled.poolBytes = s_scaled.tableBytes = s_scaled.scratchBytes = 0;
        }

        // Back to "not made yet" (a new internal resolution; the GPU idle):
        // the next scaled resolve makes it again at the new scale. Every page
        // falls back to its 1x copy until a scaled resolve writes it again.
        // `keepBuffers` (the new scale is above 1x too): the pool and the
        // page table stay allocated, as their sizes don't depend on the
        // scale; only the slots are laid out again.
        void ResetScaledMemory(bool keepBuffers)
        {
            if (!keepBuffers)
                FreeScaledBuffers();
            s_scaled.tried = s_scaled.enabled = false;
            s_scaled.sx = s_scaled.sy = s_scaled.s = 1;
            s_scaled.slotWords = s_scaled.slots = 0;
            s_scaled.scratchSize = 0;
            std::vector<uint32_t>().swap(s_scaled.pageSlot);
            std::vector<uint64_t>().swap(s_scaled.pageSeq);
            std::vector<uint32_t>().swap(s_scaled.slotPage);
            std::vector<uint8_t>().swap(s_scaled.slotRef);
            s_scaled.dirty.clear();
            s_scaled.clock = 0;
            s_scaledChecks.clear();
            s_textureCache->generation++;  // lookups decided with the old scale
        }

        bool EnsureScaledMemory()
        {
            if (s_scaled.tried)
                return s_scaled.enabled;
            s_scaled.tried = true;
            if (const char* v = std::getenv("NFSMW_SCALED_RESOLVE"); v && v[0] == '0')
                return false;
            auto start = std::chrono::steady_clock::now();
            s_scaled.sx = s_renderScaleX;
            s_scaled.sy = s_renderScaleY;
            s_scaled.s = s_scaled.sx * s_scaled.sy;
            if (s_scaled.s <= 1)
                return false;
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(s_vk->physical, &props);
            uint64_t megabytes = 512;
            if (const char* v = std::getenv("NFSMW_SCALED_MB"))
                megabytes = std::clamp<uint64_t>(std::strtoull(v, nullptr, 10), 16, 4096);
            // The whole budget, whatever the scale: kept across live changes
            // of the internal resolution (ResetScaledMemory). Bound whole, so
            // within the device's storage buffer range (NFSMW_SCALED_MB=4096
            // is past a 4 GB - 1 limit).
            VkDeviceSize poolSize = std::min<VkDeviceSize>(VkDeviceSize(megabytes) << 20, props.limits.maxStorageBufferRange) & ~VkDeviceSize(0xFFFF);
            s_scaled.slotWords = 1024 * s_scaled.s;
            s_scaled.slots = uint32_t(std::min<uint64_t>(poolSize / (4096ull * s_scaled.s), kGuestPages));
            // Scaled textures up to 1280x1024 (at scale). The untile stores
            // into them through an R32_UINT view when the device can for every
            // scaled format (a mutable-format image with extended usage: the
            // format itself needn't be storable); otherwise they load through
            // a scratch buffer that size (the ring's slots are too small) and a
            // copy, one more full write and read of the texture.
            s_scaled.scratchSize = VkDeviceSize(1280) * 1024 * 4 * s_scaled.s;
            {
                // (The image's format list is Vulkan 1.2.)
                const char* v = std::getenv("NFSMW_SCALED_DIRECT");
                VkFormatProperties fp;
                vkGetPhysicalDeviceFormatProperties(s_vk->physical, VK_FORMAT_R32_UINT, &fp);
                s_scaled.direct = !(v && v[0] == '0') && props.apiVersion >= VK_API_VERSION_1_2 &&
                    (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
                for (uint32_t f = 0; f < 64 && s_scaled.direct; f++)
                {
                    TextureFormatInfo fi;
                    VkImageFormatProperties ifp;
                    if (GetTextureFormat(f, fi) && ScaledFormat(fi))
                        s_scaled.direct = vkGetPhysicalDeviceImageFormatProperties(s_vk->physical, fi.vk, VK_IMAGE_TYPE_2D,
                            VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                            VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT, &ifp) == VK_SUCCESS;
                }
            }
            // Coalesced resolves: 2.8 -> 1.1 ms of scaled resolves a frame on
            // the Steam Machine (RADV), but 2.6-3.4 -> 4.1-4.4 ms on an M1 Pro
            // (MoltenVK), so not on Apple GPUs unless NFSMW_SCALED_COALESCE=1.
            const char* coalesce = std::getenv("NFSMW_SCALED_COALESCE");
            s_scaled.coalesce = coalesce ? coalesce[0] != '0' : props.vendorID != 0x106B;
            // A scratch buffer kept from a smaller scale is too small.
            if (s_scaled.scratch && s_scaled.scratchBytes < s_scaled.scratchSize)
            {
                vkDestroyBuffer(s_dev, s_scaled.scratch, nullptr);
                vkFreeMemory(s_dev, s_scaled.scratchMemory, nullptr);
                s_scaled.scratch = VK_NULL_HANDLE;
                s_scaled.scratchMemory = VK_NULL_HANDLE;
                s_scaled.scratchBytes = 0;
            }
            if ((!s_scaled.pool && !CreateDeviceBuffer(poolSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, s_scaled.pool, s_scaled.poolMemory,
                     "scaled pool", &s_scaled.poolBytes)) ||
                (!s_scaled.table && !CreateDeviceBuffer(VkDeviceSize(kGuestPages) * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    s_scaled.table, s_scaled.tableMemory, "scaled page table", &s_scaled.tableBytes)) ||
                (!s_scaled.direct && !s_scaled.scratch && !CreateDeviceBuffer(s_scaled.scratchSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    s_scaled.scratch, s_scaled.scratchMemory, "scaled scratch", &s_scaled.scratchBytes)))
            {
                fprintf(stderr, "[renderer] scaled resolve memory unavailable: textures stay 1x\n");
                FreeScaledBuffers();
                return false;
            }
            s_scaled.pageSlot.assign(kGuestPages, 0);
            s_scaled.pageSeq.assign(kGuestPages, 0);
            s_scaled.slotPage.assign(s_scaled.slots, 0);
            s_scaled.slotRef.assign(s_scaled.slots, 0);
            long hostPage = sysconf(_SC_PAGESIZE);
            s_scaled.hostPageGuestPages = hostPage > 4096 ? uint32_t(hostPage / 4096) : 1;
            // The pipelines take the scale in their push constants: made
            // once, kept when the memory is made again at another scale.
            VkDescriptorSetLayoutBinding b[3] = {
                { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            };
            if (!s_scaledFillPipeline)
            {
                auto bindings = WithSharedParts(b, 3);
                CreateComputePipeline(kScaledFillGlsl, bindings.data(), uint32_t(bindings.size()), 4 * sizeof(uint32_t),
                    s_scaledFillSetLayout, s_scaledFillLayout, s_scaledFillPipeline);
            }
            if (s_scaled.direct)
            {
                b[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                if (!s_scaledStorePipeline)
                    CreateComputePipeline(kScaledUntileGlsl, b, 3, 8 * sizeof(uint32_t), s_scaledStoreSetLayout, s_scaledStoreLayout,
                        s_scaledStorePipeline);
            }
            else if (!s_scaledUntilePipeline)
                CreateComputePipeline(kScaledUntileGlsl, b, 3, 8 * sizeof(uint32_t), s_scaledUntileSetLayout, s_scaledUntileLayout,
                    s_scaledUntilePipeline, "#define TO_BUFFER\n");
            EndPass();
            Begin();
            vkCmdFillBuffer(s_cmd, s_scaled.table, 0, VK_WHOLE_SIZE, 0);
            FullBarrier();
            s_scaled.enabled = true;
            s_textureCache->generation++;  // lookups decided without the scaled path
            fprintf(stderr, "[renderer] scaled resolve memory: %u slots of %u KB (%llu MB), scale %ux%u, %s writes, textures %s (%.1f ms)\n",
                s_scaled.slots, 4 * s_scaled.s, (unsigned long long)(poolSize >> 20), s_scaled.sx, s_scaled.sy,
                s_scaled.coalesce ? "coalesced" : "per-thread", s_scaled.direct ? "stored directly" : "through a scratch buffer",
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            return true;
        }

        bool ScaledTextureValid(uint32_t base, uint32_t size)
        {
            uint64_t epoch = writewatch::WriteEpoch();
            ScaledCheck& check = s_scaledChecks[(uint64_t(base) << 32) | size];
            if (check.epoch != epoch)
            {
                check.epoch = epoch;
                check.valid = ScaledRangeValid(base, size);
                if (!check.valid)
                    s_scaled.fallbacks++;
            }
            return check.valid;
        }

        void LoadScaledTexture(Texture& t, uint32_t base, uint32_t pitchBlocks, uint32_t endian)
        {
            uint32_t guestW = t.width / s_scaled.sx, guestH = t.height / s_scaled.sy;
            VkDeviceSize bytes = VkDeviceSize(t.width) * t.height * 4;
            if (bytes > s_scaled.scratchSize)
            {
                LogOnce(0x2300, "scaled texture %ux%u is larger than the scratch buffer", t.width, t.height);
                return;
            }
            EndPass();
            Begin();
            s_scaled.loads++;
            s_scaled.untiledBytes += bytes;
            VkDescriptorBufferInfo bi[3] = { { s_scaled.pool, 0, VK_WHOLE_SIZE }, { s_scaled.table, 0, VK_WHOLE_SIZE },
                { s_scaled.scratch, 0, VK_WHOLE_SIZE } };
            VkWriteDescriptorSet w[3];
            for (uint32_t k = 0; k < 3; k++)
                w[k] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, k, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    nullptr, &bi[k], nullptr };
            struct { uint32_t base, pitch, width, height, sx, sy, endian, slotWords; } pc = { base, pitchBlocks, guestW, guestH,
                s_scaled.sx, s_scaled.sy, endian, s_scaled.slotWords };
            s_stateEpoch++;
            if (t.storeView)
            {
                // Straight into the image. Everything before is behind a full
                // barrier already (as every pass and dispatch is; with
                // NFSMW_PASS_BARRIER a pass's narrower one still holds compute
                // work), including the draws that sampled the last load and
                // the resolves that wrote the pool, so only the stores need
                // ordering after.
                if (!t.uploaded)
                    ToGeneral(t.image.image, VK_IMAGE_ASPECT_COLOR_BIT, 1);
                VkDescriptorImageInfo ii{ VK_NULL_HANDLE, t.storeView, VK_IMAGE_LAYOUT_GENERAL };
                w[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                    &ii, nullptr, nullptr };
                vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_scaledStorePipeline);
                s_pushDescriptorSet(s_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_scaledStoreLayout, 0, 3, w);
                vkCmdPushConstants(s_cmd, s_scaledStoreLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
                vkCmdDispatch(s_cmd, (t.width + 7) / 8, (t.height + 7) / 8, 1);
                ComputeBarrier();
                return;
            }
            vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_scaledUntilePipeline);
            s_pushDescriptorSet(s_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_scaledUntileLayout, 0, 3, w);
            vkCmdPushConstants(s_cmd, s_scaledUntileLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
            vkCmdDispatch(s_cmd, (t.width + 7) / 8, (t.height + 7) / 8, 1);
            FullBarrier();
            if (!t.uploaded)
                ToGeneral(t.image.image, VK_IMAGE_ASPECT_COLOR_BIT, 1);
            VkBufferImageCopy region{};
            region.bufferRowLength = t.width;
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.imageExtent = { t.width, t.height, 1 };
            vkCmdCopyBufferToImage(s_cmd, s_scaled.scratch, t.image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
            FullBarrier();
        }

        void FillFreshSlots(const std::vector<std::pair<uint32_t, uint32_t>>& fresh)
        {
            for (size_t first = 0; first < fresh.size(); first += 4096)
            {
                uint32_t count = uint32_t(std::min<size_t>(4096, fresh.size() - first));
                VkDeviceSize list = RingAllocate(VkDeviceSize(count) * 8);
                if (list == ~VkDeviceSize(0))
                    return;
                uint32_t* out = reinterpret_cast<uint32_t*>(s_ringHost + list);
                for (uint32_t i = 0; i < count; i++)
                {
                    out[i * 2] = fresh[first + i].first;
                    out[i * 2 + 1] = fresh[first + i].second;
                    EnsureShadow(fresh[first + i].first << 12, 4096);
                }
                EndPass();
                Begin();
                VkDescriptorBufferInfo bi[3] = { s_sharedInfo[0], { s_scaled.pool, 0, VK_WHOLE_SIZE }, { s_ring, 0, kRingSize } };
                VkWriteDescriptorSet w[6];
                for (uint32_t k = 0; k < 3; k++)
                    w[k] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, k, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                        nullptr, &bi[k], nullptr };
                struct { uint32_t count, s, slotWords, listWord; } pc = { count, s_scaled.s, s_scaled.slotWords, uint32_t(list / 4) };
                s_stateEpoch++;
                vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_scaledFillPipeline);
                s_pushDescriptorSet(s_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_scaledFillLayout, 0, AddSharedParts(w, 3), w);
                vkCmdPushConstants(s_cmd, s_scaledFillLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
                vkCmdDispatch(s_cmd, (count * 1024 + 63) / 64, 1, 1);
                FullBarrier();
            }
        }

        // Front buffer -> RGBA8 image on the GPU (the presenter blits it), so a
        // swap needs neither a CPU wait nor a CPU untile.
        const char* kFrontGlsl = R"(#version 460
layout(local_size_x = 8, local_size_y = 8) in;
layout(std430, set = 0, binding = 0) readonly buffer SharedMemory { uint g_mem[]; };)" GLSL_SHARED_MEMORY_READ R"(
layout(set = 0, binding = 1, rgba8) writeonly uniform image2D outImage;
layout(std430, set = 0, binding = 2) readonly buffer ScaledPool { uint s_pool[]; };
layout(std430, set = 0, binding = 3) readonly buffer ScaledTable { uint s_table[]; };
// width, height: the output (host pixels when scaled: from the scaled memory).
layout(push_constant) uniform P { uint base, pitch, tiled, endian, format, swizzle, width, height, sx, sy, slotWords, scaled; } p;

uint tiled2D(uint x, uint y, uint pitch, uint bppLog2)
{
    uint outer = (((y >> 5) * (pitch >> 5)) + (x >> 5)) << 6;
    uint inner = (((y >> 1) & 7u) << 3) | (x & 7u);
    uint oi = (outer | inner) << bppLog2;
    uint bank = (y >> 4) & 1u;
    uint pipe = ((x >> 3) & 3u) ^ (((y >> 3) & 1u) << 1);
    return ((y & 1u) << 4) | (pipe << 6) | (bank << 11) | (oi & 0xFu) | (((oi >> 4) & 1u) << 5) |
        (((oi >> 5) & 7u) << 8) | ((oi >> 8) << 12);
}

void main()
{
    uvec2 g = gl_GlobalInvocationID.xy;
    if (g.x >= p.width || g.y >= p.height) return;
    uint v;
    if (p.scaled != 0u)
    {
        uint gx = g.x / p.sx, gy = g.y / p.sy;
        uint a = (p.base + tiled2D(gx, gy, p.pitch, 2u)) & 0x1FFFFFFFu;
        uint slot = s_table[a >> 12];
        v = slot != 0u ? s_pool[(slot - 1u) * p.slotWords + ((a & 4095u) >> 2) * (p.sx * p.sy) + (g.y - gy * p.sy) * p.sx + (g.x - gx * p.sx)] : 0u;
    }
    else
    {
        uint addr = p.base + (p.tiled != 0u ? tiled2D(g.x, g.y, p.pitch, 2u) : (g.y * p.pitch + g.x) * 4u);
        v = memLoad((addr & 0x1FFFFFFFu) >> 2);
    }
    if (p.endian == 1u) v = ((v << 8) & 0xFF00FF00u) | ((v >> 8) & 0x00FF00FFu);
    else if (p.endian == 2u) v = (v << 24) | ((v << 8) & 0x00FF0000u) | ((v >> 8) & 0x0000FF00u) | (v >> 24);
    else if (p.endian == 3u) v = (v >> 16) | (v << 16);
    float c[6];
    if (p.format == 7u || p.format == 54u)
    {
        c[0] = float(v & 0x3FFu) / 1023.0; c[1] = float((v >> 10) & 0x3FFu) / 1023.0;
        c[2] = float((v >> 20) & 0x3FFu) / 1023.0; c[3] = float(v >> 30) / 3.0;
    }
    else
    {
        for (int i = 0; i < 4; i++) c[i] = float((v >> (8 * i)) & 0xFFu) / 255.0;
    }
    c[4] = 0.0; c[5] = 1.0;
    vec3 rgb = vec3(c[min(p.swizzle & 7u, 5u)], c[min((p.swizzle >> 3) & 7u, 5u)], c[min((p.swizzle >> 6) & 7u, 5u)]);
    imageStore(outImage, ivec2(g), vec4(rgb, 1.0));
}
)";

        VkPipeline s_frontPipeline = VK_NULL_HANDLE;
        VkPipelineLayout s_frontLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout s_frontSetLayout = VK_NULL_HANDLE;
        // The front buffer as presented: 1x, and at host resolution when the
        // scaled resolve memory has it (two images: no stall when the game
        // switches between them, e.g. around movies).
        Image s_front, s_frontScaled;
        uint32_t s_frontWidth = 0, s_frontHeight = 0, s_frontScaledWidth = 0, s_frontScaledHeight = 0;

        // A front image the presenter may still show or be reading: taken
        // back from it, then destroyed once the queue is idle.
        void DestroyFrontImage(Image& front, uint32_t& width, uint32_t& height)
        {
            Flush();
            if (front.image)
                video::ReleaseFrontImage(front.image);
            video::WaitGpuIdle();
            DestroyImage(front);
            width = height = 0;
        }

        void CreateUntilePipeline()
        {
            VkDescriptorSetLayoutBinding b[2] = {
                { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            };
            auto bindings = WithSharedParts(b, 2);
            CreateComputePipeline(kUntileGlsl, bindings.data(), uint32_t(bindings.size()), sizeof(UntileConstants), s_untileSetLayout,
                s_untileLayout, s_untilePipeline);
            b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings = WithSharedParts(b, 2);
            CreateComputePipeline(kUntileGlsl, bindings.data(), uint32_t(bindings.size()), sizeof(UntileConstants), s_untileStoreSetLayout,
                s_untileStoreLayout, s_untileStorePipeline, "#define TO_IMAGE\n");
        }

        void CreateResolvePipeline()
        {
            VkDescriptorSetLayoutBinding b[5] = {
                { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                { 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },  // scaled pool
                { 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },  // scaled page table
                { 4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },   // a cached texture (or a dummy)
            };
            auto bindings = WithSharedParts(b, 5);
            VkDescriptorSetLayoutCreateInfo dslci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            dslci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
            dslci.bindingCount = uint32_t(bindings.size());
            dslci.pBindings = bindings.data();
            Check(vkCreateDescriptorSetLayout(s_dev, &dslci, nullptr, &s_resolveSetLayout), "resolve set layout");
            VkPushConstantRange range{ VK_SHADER_STAGE_COMPUTE_BIT, 0, s_resolvePush };
            VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            plci.setLayoutCount = 1;
            plci.pSetLayouts = &s_resolveSetLayout;
            plci.pushConstantRangeCount = 1;
            plci.pPushConstantRanges = &range;
            Check(vkCreatePipelineLayout(s_dev, &plci, nullptr, &s_resolveLayout), "resolve pipeline layout");
            for (bool coalesced : { false, true })
            {
                std::string log;
                std::string preamble = coalesced ? "#define COALESCED\n" : "";
                if (s_resolvePush != offsetof(ResolveConstants, texX))
                    preamble += "#define TEX_OFFSET\n";
                auto spirv = CompileGlsl(kResolveGlsl, GLSLANG_STAGE_COMPUTE, log, preamble);
                if (spirv.empty())
                {
                    fprintf(stderr, "[renderer] resolve shader failed:\n%s\n", log.c_str());
                    abort();
                }
                VkShaderModule module = CreateModule(spirv);
                VkComputePipelineCreateInfo cpci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
                cpci.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT,
                    module, "main", nullptr };
                cpci.layout = s_resolveLayout;
                Check(vkCreateComputePipelines(s_dev, s_pipelineCache, 1, &cpci, nullptr,
                    coalesced ? &s_resolveCoalescedPipeline : &s_resolvePipeline), "resolve pipeline");
            }
        }

        // ---------------------------------------------------------------

        // Guest physical memory imported without a copy (unified memory).
        bool ImportSharedMemory()
        {
            if (!s_vk->getMemoryHostPointerProperties)  // (no host-memory import)
                return false;
            VkMemoryHostPointerPropertiesEXT hpp{ VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
            if (s_vk->getMemoryHostPointerProperties(s_dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                    s_sharedHost, &hpp) != VK_SUCCESS)
                return false;

            VkExternalMemoryBufferCreateInfo embci{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO };
            embci.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
            VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bci.pNext = &embci;
            bci.size = kSharedSize;
            bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            Check(vkCreateBuffer(s_dev, &bci, nullptr, &s_shared), "vkCreateBuffer(shared)");
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(s_dev, s_shared, &req);

            VkImportMemoryHostPointerInfoEXT import{ VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT };
            import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
            import.pHostPointer = s_sharedHost;
            VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            mai.pNext = &import;
            mai.allocationSize = kSharedSize;
            mai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits & hpp.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (VkResult r = vkAllocateMemory(s_dev, &mai, nullptr, &s_sharedMemory); r != VK_SUCCESS)
            {
                fprintf(stderr, "[renderer] importing guest memory failed (%d)\n", int(r));
                vkDestroyBuffer(s_dev, s_shared, nullptr);
                s_shared = VK_NULL_HANDLE;
                return false;
            }
            Check(vkBindBufferMemory(s_dev, s_shared, s_sharedMemory, 0), "vkBindBufferMemory(shared)");
            return true;
        }

        void CreateShadowSharedMemory()
        {
            VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bci.size = kSharedSize;
            bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            Check(vkCreateBuffer(s_dev, &bci, nullptr, &s_shared), "vkCreateBuffer(shadow)");
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(s_dev, s_shared, &req);
            VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            mai.allocationSize = req.size;
            // An integrated GPU's (the Deck's) copy goes in system memory:
            // host-visible and not device-local (write-combined GTT on amdgpu,
            // uncached first). Its "VRAM" is a carve-out of the same RAM, 1 GB
            // on a Deck, and the copy there with the textures and targets
            // didn't fit: at every world load the kernel moved ~500 MB of it
            // out to GTT, and the GPU reads GTT as fast. A discrete GPU keeps
            // it in VRAM the CPU writes through resizable BAR when there is
            // some. NFSMW_SHADOW_MEMORY=vram|gtt overrides.
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(s_vk->physical, &props);
            const char* where = std::getenv("NFSMW_SHADOW_MEMORY");
            bool system = where ? strcmp(where, "gtt") == 0 : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
            const VkMemoryPropertyFlags mapped = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            mai.memoryTypeIndex = UINT32_MAX;
            for (int pass = 0; system && pass < 2 && mai.memoryTypeIndex == UINT32_MAX; pass++)
                for (uint32_t i = 0; i < s_memProps.memoryTypeCount; i++)
                {
                    VkMemoryPropertyFlags f = s_memProps.memoryTypes[i].propertyFlags;
                    if ((req.memoryTypeBits & (1u << i)) && (f & mapped) == mapped && !(f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
                        (pass == 1 || !(f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)))
                    {
                        mai.memoryTypeIndex = i;
                        break;
                    }
                }
            if (mai.memoryTypeIndex == UINT32_MAX)  // (all device-local: unified memory)
                mai.memoryTypeIndex = FindMemoryTypePreferring(req.memoryTypeBits,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, mapped);
            Check(vkAllocateMemory(s_dev, &mai, nullptr, &s_sharedMemory), "vkAllocateMemory(shadow)");
            Check(vkBindBufferMemory(s_dev, s_shared, s_sharedMemory, 0), "vkBindBufferMemory(shadow)");
            void* p;
            Check(vkMapMemory(s_dev, s_sharedMemory, 0, VK_WHOLE_SIZE, 0, &p), "vkMapMemory(shadow)");
            // Write every page of the mapping once, before anything uses it.
            // The first CPU write to a page of mapped VRAM faults it into the
            // page tables; left to the first sync, a burst of new textures
            // paid it mid-frame (on the Steam Machine, copying 7.9 MB of
            // never-copied pages took 3.9 ms instead of 2.9, a scene load's
            // 44 MB 22 ms instead of 14-17), while all 512 MB in order take
            // ~100 ms here. The zeros are never read: SyncShadow copies a page
            // before its first use. NFSMW_SHADOW_PREFAULT=0 leaves the faults
            // to the first sync.
            static const bool prefault = [] { const char* v = std::getenv("NFSMW_SHADOW_PREFAULT"); return !v || v[0] != '0'; }();
            auto prefaultStart = std::chrono::steady_clock::now();
            if (prefault)
                for (VkDeviceSize offset = 0; offset < kSharedSize; offset += 4096)
                    static_cast<volatile uint8_t*>(p)[offset] = 0;
            double prefaultMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - prefaultStart).count();
            writewatch::EnableShadow(static_cast<uint8_t*>(p));
            s_shadowMode = true;
            s_shadowInVram = (s_memProps.memoryTypes[mai.memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
            fprintf(stderr, "[renderer] guest memory: GPU copy in %s (memory type %u, flags %X), updated from the write watch%s\n",
                s_shadowInVram ? "VRAM (CPU-mapped)" : "host memory", mai.memoryTypeIndex, s_memProps.memoryTypes[mai.memoryTypeIndex].propertyFlags,
                prefault ? std::format(" (pages touched in {:.0f} ms)", prefaultMs).c_str() : "");
        }

        void CreateSharedMemory()
        {
            s_sharedHost = static_cast<uint8_t*>(g_memory.Translate(vmem::Physical().Base()));
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(s_vk->physical, &props);
            VkPhysicalDeviceFeatures features;
            vkGetPhysicalDeviceFeatures(s_vk->physical, &features);  // the presenter enables anisotropy when supported
            s_maxAnisotropy = features.samplerAnisotropy ? props.limits.maxSamplerAnisotropy : 1.0f;
            s_autoAnisotropy = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 16 : 4;
#if defined(__APPLE__) && TARGET_OS_IOS
            // An M-series iPad renders at 1x (ReadRenderScale), where 16x
            // cost nothing measurable (iPad Pro M2, scripted race: 60 fps in
            // the race and in the heavy windows, as at 4x). The A-series
            // GPUs of iPhones are unmeasured: 4x there.
            if (std::string_view(props.deviceName).starts_with("Apple M"))
                s_autoAnisotropy = 16;
#endif
            {
                int anisotropy = settings::GetInt(settings::Id::Anisotropy);
                if (anisotropy == settings::kAnisotropyAuto)
                    anisotropy = s_autoAnisotropy;
                fprintf(stderr, "[renderer] anisotropic filtering: %s\n",
                    anisotropy < 0 ? "the game's" : std::format("{}x", std::min(float(anisotropy), s_maxAnisotropy)).c_str());
            }

            const char* mode = std::getenv("NFSMW_SHARED_MEMORY");
            bool shadow = mode ? strcmp(mode, "shadow") == 0 : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
            if (!shadow && ImportSharedMemory())
            {
                fprintf(stderr, "[renderer] guest memory: imported (zero-copy)\n");
                return;
            }
            CreateShadowSharedMemory();
        }

        void CreateRing()
        {
            VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bci.size = kRingSize;
            bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            Check(vkCreateBuffer(s_dev, &bci, nullptr, &s_ring), "vkCreateBuffer(ring)");
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(s_dev, s_ring, &req);
            VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            mai.allocationSize = req.size;
            // Device-local when the host can map it (resizable BAR), so the
            // GPU reads constants and indices from VRAM. NFSMW_RING_MEMORY=cached
            // prefers cached system memory instead: the CPU's ~40 MB of writes
            // a frame then aren't write-combined PCIe stores (A/B experiment).
            static const bool cached = [] { const char* v = std::getenv("NFSMW_RING_MEMORY"); return v && std::string_view(v) == "cached"; }();
            mai.memoryTypeIndex = FindMemoryTypePreferring(req.memoryTypeBits,
                cached ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT
                       : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            fprintf(stderr, "[renderer] upload ring: memory type %u (flags %X)\n", mai.memoryTypeIndex,
                s_memProps.memoryTypes[mai.memoryTypeIndex].propertyFlags);
            Check(vkAllocateMemory(s_dev, &mai, nullptr, &s_ringMemory), "vkAllocateMemory(ring)");
            vkBindBufferMemory(s_dev, s_ring, s_ringMemory, 0);
            void* p;
            Check(vkMapMemory(s_dev, s_ringMemory, 0, VK_WHOLE_SIZE, 0, &p), "vkMapMemory(ring)");
            s_ringHost = static_cast<uint8_t*>(p);
        }

        void CreateSet0()
        {
            VkDescriptorSetLayoutBinding b[3] = {
                { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
                { 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
                { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
            };
            auto bindings = WithSharedParts(b, 3);
            VkDescriptorSetLayoutCreateInfo dslci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            dslci.bindingCount = uint32_t(bindings.size());
            dslci.pBindings = bindings.data();
            Check(vkCreateDescriptorSetLayout(s_dev, &dslci, nullptr, &s_set0Layout), "set 0 layout");
            VkDescriptorPoolSize sizes[2] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, s_splitMemory ? 5u : 2u },
                { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1 } };
            VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            dpci.maxSets = 1;
            dpci.poolSizeCount = 2;
            dpci.pPoolSizes = sizes;
            Check(vkCreateDescriptorPool(s_dev, &dpci, nullptr, &s_descriptorPool), "descriptor pool");
            VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            dsai.descriptorPool = s_descriptorPool;
            dsai.descriptorSetCount = 1;
            dsai.pSetLayouts = &s_set0Layout;
            Check(vkAllocateDescriptorSets(s_dev, &dsai, &s_set0), "set 0");
            VkDescriptorBufferInfo constants{ s_ring, 0, kConstantsSize };
            VkDescriptorBufferInfo ring{ s_ring, 0, kRingSize };
            VkWriteDescriptorSet w[6] = {
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, s_set0, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &s_sharedInfo[0], nullptr },
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, s_set0, 1, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, nullptr, &constants, nullptr },
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, s_set0, 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &ring, nullptr },
            };
            vkUpdateDescriptorSets(s_dev, AddSharedParts(w, 3, s_set0), w, 0, nullptr);
        }

        void CreateSamplerAndDummies()
        {
            VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
            sci.magFilter = VK_FILTER_LINEAR;
            sci.minFilter = VK_FILTER_LINEAR;
            sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            sci.maxLod = VK_LOD_CLAMP_NONE;
            Check(vkCreateSampler(s_dev, &sci, nullptr, &s_sampler), "vkCreateSampler");

            VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            s_dummy[0] = CreateImage(VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, { 1, 1, 1 }, 1, usage,
                VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT);
            s_dummy[1] = CreateImage(VK_IMAGE_TYPE_3D, VK_FORMAT_R8G8B8A8_UNORM, { 1, 1, 1 }, 1, usage,
                VK_IMAGE_VIEW_TYPE_3D, VK_IMAGE_ASPECT_COLOR_BIT);
            s_dummy[2] = CreateImage(VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, { 1, 1, 1 }, 6, usage,
                VK_IMAGE_VIEW_TYPE_CUBE, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT);
            s_dummyStorage = CreateImage(VK_IMAGE_TYPE_2D, VK_FORMAT_R32_UINT, { 1, 1, 1 }, 1, VK_IMAGE_USAGE_STORAGE_BIT,
                VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT);
            Begin();
            ToGeneral(s_dummyStorage.image, VK_IMAGE_ASPECT_COLOR_BIT, 1);
            VkClearColorValue white{ { 1.0f, 1.0f, 1.0f, 1.0f } };
            for (int i = 0; i < 3; i++)
            {
                uint32_t layers = i == 2 ? 6 : 1;
                ToGeneral(s_dummy[i].image, VK_IMAGE_ASPECT_COLOR_BIT, layers);
                VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers };
                vkCmdClearColorImage(s_cmd, s_dummy[i].image, VK_IMAGE_LAYOUT_GENERAL, &white, 1, &range);
            }
            FullBarrier();
        }
    }

    namespace
    {
        // ---------------------------------------------------------------
        // Live settings. Internal Resolution and Mipmaps change between two
        // frames (OnSwap, on this thread), with the GPU idle (Flush: command
        // buffers in flight are the only other users of what is replaced).
        // What was built for the old value is made again from what the guest
        // has:
        //   - Internal Resolution: the render targets of the scaled pitches
        //     at the new scale (their EDRAM contents are lost; the game clears
        //     and redraws them every frame, and resolve clears still waiting
        //     move over), the scaled resolve memory (every page back to its
        //     1x copy until a scaled resolve writes it again) and the scaled
        //     textures. The front images follow at the next swap
        //     (PresentFrontBuffer replaces one of a new size), and the
        //     presenter's AA image with them.
        //   - Mipmaps: the whole texture cache, each texture loaded again
        //     from guest memory when a draw next uses it.
        // Auto follows the picture's size (the window, fullscreen, Aspect
        // Ratio) once it has held for kAutoSettleFrames, so a window being
        // dragged doesn't rebuild at every step; a change of the setting
        // itself applies at the next swap.
        uint64_t s_liveGeneration = ~0ull;
        int32_t s_resolutionSetting = 0;  // Internal Resolution as applied
        constexpr uint32_t kAutoSettleFrames = 30;
        uint32_t s_autoWantX = 0, s_autoWantY = 0, s_autoSeen = 0;

        void DestroyTexture(Texture& t)
        {
            if (auto it = s_directByBase.find(t.guestStart); it != s_directByBase.end() && it->second == &t)
                s_directByBase.erase(it);
            if (auto it = s_directByStart.find(t.guestStart); it != s_directByStart.end() && it->second == &t)
                s_directByStart.erase(it);
            for (auto& [swizzle, view] : t.views)
                vkDestroyImageView(s_dev, view, nullptr);
            t.views.clear();
            for (uint32_t layer = 1; layer < 6; layer++)  // ([0] is storeView)
                if (t.storeViews[layer])
                    vkDestroyImageView(s_dev, t.storeViews[layer], nullptr);
            std::fill(std::begin(t.storeViews), std::end(t.storeViews), VK_NULL_HANDLE);
            if (t.storeView)
                vkDestroyImageView(s_dev, t.storeView, nullptr);
            t.storeView = VK_NULL_HANDLE;
            DestroyImage(t.image);
        }

        // Drops the cached textures `which` picks (the GPU idle); returns how many.
        template <typename Which>
        size_t DropTextures(Which which)
        {
            s_textureCache->generation++;
            size_t dropped = 0;
            for (auto it = s_textures.begin(); it != s_textures.end();)
            {
                if (!which(*it->second))
                {
                    ++it;
                    continue;
                }
                DestroyTexture(*it->second);
                it = s_textures.erase(it);
                dropped++;
            }
            return dropped;
        }

        void ApplyRenderScale(uint32_t sx, uint32_t sy, const std::string& how)
        {
            auto start = std::chrono::steady_clock::now();
            uint32_t oldX = s_renderScaleX, oldY = s_renderScaleY;
            Flush();
            s_renderScaleX = sx;
            s_renderScaleY = sy;
            size_t targets = 0;
            for (auto& [key, slot] : s_targets)
            {
                RenderTarget& old = *slot;  // (keyed as before: the key has no scale)
                if (!ScaledPitch(old.pitch) || (old.scaleX == sx && old.scaleY == sy))
                    continue;
                // Clears still waiting for their target's next pass, in guest pixels.
                std::vector<PendingClear> moved;
                std::erase_if(s_pendingClears, [&](const PendingClear& pc) {
                    if (pc.rt != &old)
                        return false;
                    moved.push_back({ nullptr,
                        { { pc.rect.offset.x / int32_t(old.scaleX), pc.rect.offset.y / int32_t(old.scaleY) },
                          { pc.rect.extent.width / old.scaleX, pc.rect.extent.height / old.scaleY } },
                        pc.value });
                    return true;
                });
                uint32_t base = old.base, format = old.format, pitch = old.pitch;
                bool depth = old.depth, wide = old.wide;
                DestroyTarget(old);
                slot = CreateTarget(base, format, depth, pitch, wide);
                for (PendingClear& pc : moved)
                {
                    pc.rt = slot.get();
                    pc.rect = { { pc.rect.offset.x * int32_t(sx), pc.rect.offset.y * int32_t(sy) },
                        { pc.rect.extent.width * sx, pc.rect.extent.height * sy } };
                    s_pendingClears.push_back(pc);
                }
                targets++;
            }
            for (RecentTarget& recent : s_recentTargets)
                recent = {};
            size_t textures = DropTextures([](const Texture& t) { return t.scaled; });
            ResetScaledMemory(sx * sy > 1);
            fprintf(stderr, "[renderer] internal resolution %ux%u -> %ux%u (%ux%u, %s): %zu targets and %zu scaled textures replaced in %.1f ms\n",
                oldX, oldY, sx, sy, 1280 * sx, 720 * sy, how.c_str(), targets, textures,
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        }

        void ApplyMipmaps(bool mips)
        {
            auto start = std::chrono::steady_clock::now();
            Flush();
            s_loadMips = mips;
            size_t textures = DropTextures([](const Texture&) { return true; });
            fprintf(stderr, "[renderer] mipmaps %s: %zu textures dropped in %.1f ms (loaded again as they are drawn)\n",
                mips ? "on" : "off", textures, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        }

        void ApplyLiveSettings()
        {
            uint64_t generation = settings::Generation();
            if (generation != s_liveGeneration)
            {
                s_liveGeneration = generation;
                bool mips = settings::GetBool(settings::Id::Mipmaps);
                if (mips != s_loadMips)
                    ApplyMipmaps(mips);
            }
            if (s_scaleFixed)
                return;
            int32_t setting = settings::GetInt(settings::Id::InternalResolution);
            uint32_t regionW = 0, regionH = 0;
            if (setting <= 0)
            {
                video::FrameRegion(regionW, regionH);
                if (regionW == 0 || regionH == 0)
                    return;  // no picture to size it for: keep the scale
            }
            uint32_t sx, sy;
            WantedRenderScale(setting, regionW, regionH, sx, sy);
            if (sx == s_renderScaleX && sy == s_renderScaleY)
            {
                s_resolutionSetting = setting;
                s_autoSeen = 0;
                return;
            }
            // Only the picture's size moved (Auto): once it has settled.
            if (setting == s_resolutionSetting)
            {
                if (sx != s_autoWantX || sy != s_autoWantY)
                {
                    s_autoWantX = sx;
                    s_autoWantY = sy;
                    s_autoSeen = 0;
                }
                if (++s_autoSeen < kAutoSettleFrames)
                    return;
            }
            s_resolutionSetting = setting;
            s_autoSeen = 0;
            ApplyRenderScale(sx, sy, ScaleReason(setting, regionW, regionH));
        }

        // ---------------------------------------------------------------
        // Occlusion queries counted on the GPU (NFSMW_OCCLUSION=1; see
        // ZpassDone).

        // Before a counted draw, inside its pass: the active segment while
        // the weight is the same, else a new one. None when the slot's
        // queries are used up (the draw isn't counted).
        void ZpdBeginSegment(uint32_t num, uint32_t den)
        {
            if (s_zpdQuery >= 0 && (num != s_zpdNum || den != s_zpdDen))
                ZpdEndSegment();
            if (s_zpdQuery >= 0)
                return;
            if (s_zpdUsed[s_slot] >= kZpdQueries)
            {
                s_zpdCounts.overflow++;
                LogOnce((0x2Aull << 56) | 2, "occlusion query: %u segments in one command buffer: draws past them aren't counted", kZpdQueries);
                return;
            }
            uint32_t q = s_zpdUsed[s_slot]++;
            vkCmdBeginQuery(s_cmd, s_zpdPool[s_slot], q, VK_QUERY_CONTROL_PRECISE_BIT);
            s_zpdQuery = int32_t(q);
            s_zpdNum = num;
            s_zpdDen = den;
            s_submissions[s_slot].zpdSegments.push_back({ s_zpdSegNext++, q, num, den });
            s_zpdCounts.segments++;
        }

        // At each swap. The game's BEGIN, quad and END follow each other in
        // one view, so a bracket still open came from an earlier frame: it
        // is dropped (an open bracket counts every draw and turns off
        // NFSMW_PASS_MERGE bit 1). Every 120 frames, before the [perf] line,
        // the [zpd] line.
        void ZpdOnSwap(uint64_t frame)
        {
            zpd::Bracket first;
            if (uint32_t dropped = s_zpdOpen.DropBefore(frame, &first))
            {
                s_zpdCounts.dropped += dropped;
                LogOnce((0x2Bull << 56) | first.beginAddress, "occlusion query: BEGIN %08X (frame %llu) still open at the swap: dropped (%u in all)",
                    first.beginAddress, (unsigned long long)first.frame, dropped);
                ZpdLog("f%llu swap: %u brackets dropped (the first: BEGIN %08X)", (unsigned long long)frame, dropped, first.beginAddress);
            }
            if (frame % 120 != 0)
                return;
            static ZpdCounts last{};
            static ZpdStores lastStores{};
            ZpdStores stores;
            {
                std::lock_guard lock(s_submitMutex);
                stores = s_zpdStores;
                s_zpdStores.delayMax = 0;
            }
            const ZpdCounts& c = s_zpdCounts;
            uint64_t stored = stores.stored - lastStores.stored;
            fprintf(stderr, "[zpd] per frame: %.1f ZPDs, %.1f ENDs (%.1f with samples), %.1f counted draws in %.1f segments"
                " | CP -> store %.2f ms avg, %.2f ms max | totals: %llu unmatched ENDs, %llu dropped and %llu replaced brackets,"
                " %llu other reports, %llu not available, %llu over the queries, %llu lost, %llu outside the viewport, %llu not clamped\n",
                double(c.zpds - last.zpds) / 120, double(stored) / 120, double(stores.nonZero - lastStores.nonZero) / 120,
                double(c.draws - last.draws) / 120, double(c.segments - last.segments) / 120,
                stored ? (stores.delaySum - lastStores.delaySum) / double(stored) : 0.0, stores.delayMax,
                (unsigned long long)c.unmatched, (unsigned long long)c.dropped, (unsigned long long)c.replaced,
                (unsigned long long)c.unknown, (unsigned long long)s_zpdUnavailable.load(), (unsigned long long)c.overflow,
                (unsigned long long)stores.lost, (unsigned long long)c.clamped, (unsigned long long)c.unclamped);
            last = c;
            lastStores = stores;
        }
    }

    bool Initialize(const uint32_t* regs)
    {
        s_vk = video::GetVulkan();
        if (!s_vk)
            return false;
        s_dev = s_vk->device;
        s_regs = regs;
        {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(s_vk->physical, &props);
            ReadRenderScale(props);
            const char* split = std::getenv("NFSMW_SPLIT_MEMORY");
            s_splitMemory = split ? split[0] == '1' : props.limits.maxStorageBufferRange < kSharedSize;
            if (s_splitMemory)
                fprintf(stderr, "[renderer] guest memory bound as 4 parts of %llu MB (storage buffers bind up to %u MB here)\n",
                    (unsigned long long)(kSharedPart >> 20), props.limits.maxStorageBufferRange >> 20);
            s_resolutionSetting = settings::GetInt(settings::Id::InternalResolution);
            s_loadMips = settings::GetBool(settings::Id::Mipmaps);
            s_liveGeneration = settings::Generation();
            uint32_t count = 0;
            vkEnumerateDeviceExtensionProperties(s_vk->physical, nullptr, &count, nullptr);
            std::vector<VkExtensionProperties> extensions(count);
            vkEnumerateDeviceExtensionProperties(s_vk->physical, nullptr, &count, extensions.data());
            for (const VkExtensionProperties& e : extensions)
                s_memoryBudget |= strcmp(e.extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) == 0;
        }
        vkGetPhysicalDeviceMemoryProperties(s_vk->physical, &s_memProps);
        s_pushDescriptorSet = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(
            vkGetDeviceProcAddr(s_dev, "vkCmdPushDescriptorSetKHR"));
        if (!s_pushDescriptorSet)
        {
            fprintf(stderr, "[renderer] VK_KHR_push_descriptor is missing; rendering disabled\n");
            s_vk = nullptr;
            return false;
        }
        glslang_initialize_process();
        CreatePipelineCache();

        VkCommandPoolCreateInfo cpci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cpci.queueFamilyIndex = s_vk->queueFamily;
        Check(vkCreateCommandPool(s_dev, &cpci, nullptr, &s_commandPool), "vkCreateCommandPool");
        for (Submission& sub : s_submissions)
        {
            VkCommandBufferAllocateInfo cbai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            cbai.commandPool = s_commandPool;
            cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cbai.commandBufferCount = 1;
            Check(vkAllocateCommandBuffers(s_dev, &cbai, &sub.cmd), "vkAllocateCommandBuffers");
            VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            Check(vkCreateFence(s_dev, &fci, nullptr, &sub.fence), "vkCreateFence");
        }
        {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(s_vk->physical, &props);
            s_timestampPeriod = props.limits.timestampPeriod;
            VkQueryPoolCreateInfo qpci{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
            qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qpci.queryCount = kSlots * 2;
            if (const char* v = std::getenv("NFSMW_GPU_TIMING"); v && v[0] == '1')
                if (vkCreateQueryPool(s_dev, &qpci, nullptr, &s_timestamps) != VK_SUCCESS)
                    s_timestamps = VK_NULL_HANDLE;
            if (s_profileFrame >= 0)
            {
                qpci.queryCount = 512;
                Check(vkCreateQueryPool(s_dev, &qpci, nullptr, &s_passQueries), "pass query pool");
            }
        }
        // NFSMW_OCCLUSION=1: a pool of occlusion queries per slot, so a reset
        // never touches queries a submission in flight uses. Each is reset
        // before its first use: here with host reset, else by the slot's
        // first Begin.
        if (s_vk->occlusionCounting)
        {
            VkQueryPoolCreateInfo qpci{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
            qpci.queryType = VK_QUERY_TYPE_OCCLUSION;
            qpci.queryCount = kZpdQueries;
            s_zpdOn = true;
            for (uint32_t k = 0; k < kSlots && s_zpdOn; k++)
            {
                s_zpdOn = vkCreateQueryPool(s_dev, &qpci, nullptr, &s_zpdPool[k]) == VK_SUCCESS;
                if (!s_zpdOn)
                    s_zpdPool[k] = VK_NULL_HANDLE;
                else if (s_vk->resetQueryPool)
                    s_vk->resetQueryPool(s_dev, s_zpdPool[k], 0, kZpdQueries);
                else
                    s_zpdUsed[k] = kZpdQueries;
            }
            if (s_zpdOn)
                s_zpdResults = std::make_unique<zpd::ResultRing>();
            else
            {
                for (VkQueryPool& pool : s_zpdPool)
                    if (pool)
                        vkDestroyQueryPool(s_dev, pool, nullptr);
                std::fill(std::begin(s_zpdPool), std::end(s_zpdPool), VK_NULL_HANDLE);
                std::fill(std::begin(s_zpdUsed), std::end(s_zpdUsed), 0u);
            }
            fprintf(stderr, s_zpdOn ? "[renderer] occlusion queries counted: %u per command buffer, %u command buffers\n"
                                    : "[renderer] occlusion query pools (%u x %u) couldn't be made: queries report zero samples\n",
                kZpdQueries, kSlots);
        }
        if (s_passBarrierLevel)
            fprintf(stderr, "[renderer] NFSMW_PASS_BARRIER=%u: %s leave the vertex stages out of their barriers%s\n", s_passBarrierLevel,
                s_passBarrierLevel == 1 ? "pass ends" : s_passBarrierLevel == 2 ? "pass ends and 1x resolves" : "1x resolves",
                s_passBarrierAlt ? " (every other [perf] window: NFSMW_PASS_BARRIER_ALT)" : "");
        if (s_untiledArea)
            fprintf(stderr, "[renderer] NFSMW_UNTILED_AREA%s: untiled passes load and store their draws' guest viewport, not the whole target\n",
                s_untiledArea == 2 ? "=alt (every other [perf] window)" : "");
        std::thread(CompletionThread).detach();
        if (s_imagePool)
            std::thread(MemoryThread).detach();

        CreateSharedMemory();
        SetSharedInfo();
        CreateRing();
        CreateSet0();
        CreateSamplerAndDummies();
        CreateResolvePipeline();
        CreateUntilePipeline();
        {
            VkDescriptorSetLayoutBinding b[4] = {
                { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                { 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            };
            auto bindings = WithSharedParts(b, 4);
            CreateComputePipeline(kFrontGlsl, bindings.data(), uint32_t(bindings.size()), 12 * sizeof(uint32_t), s_frontSetLayout,
                s_frontLayout, s_frontPipeline);
        }
        Flush();
        fprintf(stderr, "[renderer] ready: guest physical memory %s (%llu MB)\n",
            !s_shadowMode ? "imported" : s_shadowInVram ? "copied for the GPU, in VRAM" : "copied for the GPU, in host memory",
            (unsigned long long)(kSharedSize >> 20));
        return true;
    }

    bool Enabled()
    {
        return s_vk != nullptr;
    }

    void LoadShader(uint32_t type, const uint32_t* words, uint32_t dwords)
    {
        if (!s_vk || dwords == 0 || type > 1)
            return;
        auto hash = [type](const uint32_t* w, uint32_t n) {
            uint64_t h = 0xCBF29CE484222325ull ^ type;
            for (uint32_t i = 0; i < n; i++)
                h = (h ^ w[i]) * 0x100000001B3ull;
            return h;
        };
        ShaderCache& c = *s_shaderCache;
        ShaderCacheEntry* e = nullptr;
        if (c.on)
        {
            c.loads++;
            e = &c.entries[(uint64_t(reinterpret_cast<uintptr_t>(words)) * 0x9E3779B97F4A7C15ull) >> 54];
            if (e->words == words && e->dwords == dwords && e->type == type)
            {
                c.comparedBytes += size_t(dwords) * 4;
                if (memcmp(e->copy.data(), words, size_t(dwords) * 4) == 0)
                {
                    c.hits++;
                    if (c.verify)
                    {
                        auto it = s_shaders.find(hash(words, dwords));
                        Shader* full = it == s_shaders.end() ? nullptr : it->second.get();
                        if (full != e->shader && c.mismatches++ < 50)
                            fprintf(stderr, "[cpverify] shader at %p (%u dwords, type %u): cached %016llx, hash finds %016llx\n",
                                (const void*)words, dwords, type, (unsigned long long)e->shader->hash,
                                (unsigned long long)(full ? full->hash : 0));
                    }
                    (type == 0 ? s_vs : s_ps) = e->shader;
                    return;
                }
            }
            // Hashed from the copy, so the entry's words and shader agree
            // even if the guest writes them meanwhile.
            e->words = words;
            e->dwords = dwords;
            e->type = type;
            e->copy.assign(words, words + dwords);
            words = e->copy.data();
        }
        uint64_t h = hash(words, dwords);
        auto& slot = s_shaders[h];
        if (!slot)
        {
            HitchTimer hitchTimer(s_hitch.shaderMs);
            s_hitch.shaders++;
            slot = std::make_unique<Shader>();
            slot->stage = type == 0 ? ShaderStage::Vertex : ShaderStage::Pixel;
            slot->hash = h;
            std::vector<uint32_t> ucode(dwords);
            for (uint32_t i = 0; i < dwords; i++)
                ucode[i] = __builtin_bswap32(words[i]);
            slot->info = TranslateShader(slot->stage, ucode.data(), dwords);
            if (!slot->info.ok)
                fprintf(stderr, "[renderer] %s_%016llx: translation failed: %s\n", type == 0 ? "vs" : "ps",
                    (unsigned long long)h, slot->info.error.c_str());
            // Compiled to SPIR-V with its first pipeline, on a pipeline worker.
        }
        if (e)
            e->shader = slot.get();
        (type == 0 ? s_vs : s_ps) = slot.get();
    }

    void Draw(uint32_t initiatorValue, uint32_t dmaBase, uint32_t dmaSizeValue)
    {
        if (!s_vk)
            return;
        reg::VGT_DRAW_INITIATOR initiator;
        initiator.value = initiatorValue;
        auto edramMode = Reg<reg::RB_MODECONTROL>().edram_mode;
        if (edramMode == xenos::EdramMode::kCopy)
        {
            Resolve();
            return;
        }
        if (edramMode != xenos::EdramMode::kColorDepth && edramMode != xenos::EdramMode::kDepthOnly)
            return;
        s_stats.draws++;
        auto skip = [&](uint64_t key, const char* why) {
            s_stats.skipped++;
            LogOnce(key, "draw skipped: %s", why);
        };
        if (!s_vs || !s_ps)
            return skip(1, "no shaders loaded");
        if (!s_vs->info.ok || !s_ps->info.ok)
            return skip(2, "a shader failed to translate");
        uint32_t count = initiator.num_indices;
        if (count == 0)
            return;
        if (initiator.source_select == xenos::SourceSelect::kImmediate)
            return skip(3, "immediate indices");
        auto viz = Reg<reg::PA_SC_VIZ_QUERY>();
        if (viz.viz_query_ena && viz.kill_pix_post_hi_z)
            return;

        auto surface = Reg<reg::RB_SURFACE_INFO>();
        uint32_t pitch = surface.surface_pitch;
        if (pitch == 0)
            return skip(4, "zero surface pitch");
        auto [tileShiftX, tileShiftY] = TileShift();
        if (tileShiftX != 0 || tileShiftY != 0)
        {
            s_stats.tileSkips++;
            return;  // drawn with tile 0 (untiled rendering)
        }
        const bool untiledPass = s_untiled && surface.msaa_samples != xenos::MsaaSamples::k1X &&
            Reg<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable;

        // Render targets.
        RenderTarget* color[4] = {};
        auto colorMask = Reg<reg::RB_COLOR_MASK>();
        uint32_t writeMask = 0;
        if (edramMode == xenos::EdramMode::kColorDepth)
        {
            static const uint32_t infoRegs[4] = { XE_GPU_REG_RB_COLOR_INFO, XE_GPU_REG_RB_COLOR1_INFO,
                XE_GPU_REG_RB_COLOR2_INFO, XE_GPU_REG_RB_COLOR3_INFO };
            for (uint32_t i = 0; i < 4; i++)
            {
                uint32_t mask = (colorMask.value >> (4 * i)) & 0xF;
                if (!(s_ps->info.colorTargetMask & (1u << i)) || mask == 0)
                    continue;
                auto ci = RegAt<reg::RB_COLOR_INFO>(infoRegs[i]);
                color[i] = GetTarget(ci.color_base, uint32_t(ci.color_format), false, pitch);
                writeMask |= mask << (4 * i);
            }
        }
        auto depthControl = Reg<reg::RB_DEPTHCONTROL>();
        RenderTarget* depth = nullptr;
        if (depthControl.z_enable || depthControl.stencil_enable)
        {
            auto di = Reg<reg::RB_DEPTH_INFO>();
            depth = GetTarget(di.depth_base, uint32_t(di.depth_format), true, pitch);
        }
        if (!depth && writeMask == 0)
            return;  // nothing visible (memexport-only draws are not supported yet)
        // NFSMW_OCCLUSION=1, an occlusion query bracket open: this draw is
        // counted (ZpassDone).
        const bool zpdCount = s_zpdOn && !s_zpdOpen.Empty();
        // NFSMW_PASS_MERGE bit 1: nor is a depth-tested draw that writes no
        // colour, depth or stencil (the sun's occlusion quad: LEQUAL, no
        // writes), unless a query counts it: then it is what the query
        // counts, and bit 2 draws it in the view's open pass (depth tested
        // there, its colour masked). Otherwise the command processor answers
        // the queries (EVENT_WRITE_ZPD stores zero samples, VIZ_QUERY sets
        // its bit), so the draw changes nothing; its pass of its own does.
        if ((s_passMerge & 1) && !zpdCount && writeMask == 0 && !(depthControl.z_enable && depthControl.z_write_enable) &&
            !depthControl.stencil_enable)
        {
            LogOnce(s_vs->hash ^ (s_ps->hash << 1) ^ 0x2600, "pass merge: vs_%016llx ps_%016llx writes nothing (depth control %08X): skipped",
                (unsigned long long)s_vs->hash, (unsigned long long)s_ps->hash, depthControl.value);
            return;
        }
        // NFSMW_PASS_MERGE bit 2: a draw whose targets are some of the open
        // pass's (the sun's glow, colour only and additive, right after the
        // view's colour and depth draws) is drawn in that pass, with the
        // others attached but untouched: their colour writes masked, depth
        // and stencil tests off (the pipeline must have the pass's formats).
        // On its own it was a pass that loaded and stored its target (an
        // untiled one whole, 1280x2048) behind a full barrier, and the
        // view's pass started again after it. Only when no pixel anything
        // reads is lost: its area with its own targets is its area with the
        // pass's (the same pixels; untiled with NFSMW_UNTILED_AREA, both are
        // its viewport), or, untiled (that area is then the whole
        // target, and the pass's is shorter: the depth target's), with
        // clipping on and its viewport, the frame, inside the pass's
        // targets. What it loses then lies outside its viewport (the host
        // clips to the target, not the viewport) and at or beyond the pass's
        // extent: right only while nothing resolves rows at or beyond the
        // pass's extent (the frame's resolves read inside it). With clipping
        // off a draw may mean to reach past its viewport: it keeps its pass.
        uint32_t adoptedColor = 0;  // colour targets attached for the pass only
        bool adoptedDepth = false;
        if ((s_passMerge & 2) && s_passActive && (!depth || depth == s_passDepth))
        {
            bool subset = true, same = depth == s_passDepth;
            for (int i = 0; i < 4; i++)
            {
                subset = subset && (!color[i] || color[i] == s_passColor[i]);
                same = same && color[i] == s_passColor[i];
            }
            // As the scissor below (keep the two alike), in guest pixels,
            // with these targets (the pass's area is never larger: its
            // targets include these).
            auto areaWith = [&](RenderTarget* const* targets) {
                int32_t w = INT32_MAX, h = INT32_MAX;
                for (int i = 0; i < 5; i++)
                    if (targets[i])
                    {
                        w = std::min(w, int32_t(targets[i]->guestWidth));
                        h = std::min(h, int32_t(targets[i]->guestHeight));
                    }
                Rect sc = untiledPass ? UntiledScissor(targets, w, h) : GuestScissor();
                int32_t x0 = std::clamp(sc.x0, 0, w), y0 = std::clamp(sc.y0, 0, h);
                return Rect{ x0, y0, std::clamp(sc.x1, x0, w), std::clamp(sc.y1, y0, h) };
            };
            bool keeps = false;
            if (subset && !same)
            {
                RenderTarget* const own[5] = { color[0], color[1], color[2], color[3], depth };
                RenderTarget* const pass[5] = { s_passColor[0], s_passColor[1], s_passColor[2], s_passColor[3], s_passDepth };
                Rect a = areaWith(own), b = areaWith(pass);
                keeps = a.x0 == b.x0 && a.y0 == b.y0 && a.x1 == b.x1 && a.y1 == b.y1;
                // (The viewport only where x and y are scaled and offset.)
                auto vte = Reg<reg::PA_CL_VTE_CNTL>();
                if (!keeps && untiledPass && (vte.value & 0xF) == 0xF && !Reg<reg::PA_CL_CLIP_CNTL>().clip_disable)
                {
                    float sx = std::fabs(RegF(XE_GPU_REG_PA_CL_VPORT_XSCALE)), ox = RegF(XE_GPU_REG_PA_CL_VPORT_XOFFSET);
                    float sy = std::fabs(RegF(XE_GPU_REG_PA_CL_VPORT_YSCALE)), oy = RegF(XE_GPU_REG_PA_CL_VPORT_YOFFSET);
                    keeps = ox - sx >= float(b.x0) && ox + sx <= float(b.x1) && oy - sy >= float(b.y0) && oy + sy <= float(b.y1);
                }
            }
            if (keeps)
            {
                for (int i = 0; i < 4; i++)
                    if (!color[i] && s_passColor[i])
                    {
                        color[i] = s_passColor[i];
                        adoptedColor |= 1u << i;
                    }
                adoptedDepth = !depth && s_passDepth;
                depth = s_passDepth;
                LogOnce(s_vs->hash ^ (s_ps->hash << 1) ^ 0x2601, "pass merge: vs_%016llx ps_%016llx drawn in the open pass (%s attached)",
                    (unsigned long long)s_vs->hash, (unsigned long long)s_ps->hash,
                    adoptedDepth ? (adoptedColor ? "colour and depth" : "depth") : "colour");
            }
        }
        // Before the textures are looked up: a draw into scaled targets
        // samples scaled textures (every attachment shares the pitch, so the scale).
        s_drawScaled = false;
        for (RenderTarget* rt : { color[0], color[1], color[2], color[3], depth })
            if (rt && rt->scaleX * rt->scaleY > 1)
                s_drawScaled = true;

        // NFSMW_LOG_TILES=1: register state of the first draws under MSAA (predicated tiling).
        static const bool logTiles = std::getenv("NFSMW_LOG_TILES") != nullptr;
        if (logTiles && surface.msaa_samples != xenos::MsaaSamples::k1X)
        {
            static uint32_t logged = 0;
            static uint32_t lastKey = 0;
            auto wo = Reg<reg::PA_SC_WINDOW_OFFSET>();
            auto wtl = Reg<reg::PA_SC_WINDOW_SCISSOR_TL>();
            auto wbr = Reg<reg::PA_SC_WINDOW_SCISSOR_BR>();
            auto stl = Reg<reg::PA_SC_SCREEN_SCISSOR_TL>();
            auto sbr = Reg<reg::PA_SC_SCREEN_SCISSOR_BR>();
            uint32_t key = wo.value ^ (wtl.value * 3) ^ (wbr.value * 7) ^ (stl.value * 11) ^ (sbr.value * 13);
            if (key != lastKey && logged < 60)
            {
                logged++;
                lastKey = key;
                fprintf(stderr, "[tiles] draw %llu: msaa %u pitch %u wo (%d,%d) woEn %u | window scissor (%u,%u)-(%u,%u) offDis %u | screen scissor (%d,%d)-(%d,%d) | color %08X depth %08X vp y %.1f+%.1f\n",
                    (unsigned long long)s_stats.draws, uint32_t(surface.msaa_samples), pitch, wo.window_x_offset, wo.window_y_offset,
                    uint32_t(Reg<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable), wtl.tl_x, wtl.tl_y, wbr.br_x, wbr.br_y,
                    wtl.window_offset_disable, stl.tl_x, stl.tl_y, sbr.br_x, sbr.br_y, s_regs[XE_GPU_REG_RB_COLOR_INFO],
                    s_regs[XE_GPU_REG_RB_DEPTH_INFO], RegF(XE_GPU_REG_PA_CL_VPORT_YOFFSET), RegF(XE_GPU_REG_PA_CL_VPORT_YSCALE));
            }
        }

        // Primitive assembly.
        auto modeCntl = Reg<reg::PA_SU_SC_MODE_CNTL>();
        auto dmaSize = RegAt<reg::VGT_DMA_SIZE>(0);
        dmaSize.value = dmaSizeValue;
        IndexSource src{ initiator.source_select == xenos::SourceSelect::kDMA, dmaBase & 0x1FFFFFFF,
            initiator.index_size == xenos::IndexFormat::kInt32, uint32_t(dmaSize.swap_mode) };
        bool restartEnabled = modeCntl.multi_prim_ib_ena && src.indexed;
        uint32_t resetIndex = s_regs[XE_GPU_REG_VGT_MULTI_PRIM_IB_RESET_INDX] & 0xFFFFFF;

        DrawConstants pc{};
        VkPrimitiveTopology topology;
        bool restart = false;
        bool polygonal = true;
        std::vector<uint32_t> indices;  // converted/expanded (empty: plain vkCmdDraw)
        bool useIndices = false;
        bool directIndices = false;     // converted by ConvertIndices into the ring instead
        uint32_t vertexCount = count;
        auto convertAll = [&](bool allowRestart) {
            if ((g_cpOpt & CP_OPT_FAST_INDICES) &&
                uint64_t(src.base & 0x1FFFFFFF) + uint64_t(count) * (src.is32 ? 4 : 2) <= kSharedSize)
            {
                directIndices = true;
                useIndices = true;
                return;
            }
            indices.resize(count);
            for (uint32_t i = 0; i < count; i++)
            {
                uint32_t v = src.Get(i);
                indices[i] = allowRestart && v == resetIndex ? 0xFFFFFFFFu : v;
            }
            useIndices = true;
        };
        // Splits the draw at reset indices and hands each run to `emit`.
        auto forEachRun = [&](auto&& emit) {
            std::vector<uint32_t> run;
            for (uint32_t i = 0; i < count; i++)
            {
                uint32_t v = src.Get(i);
                if (restartEnabled && v == resetIndex)
                {
                    emit(run);
                    run.clear();
                    continue;
                }
                run.push_back(v);
            }
            emit(run);
            useIndices = true;
        };

        switch (initiator.prim_type)
        {
        case xenos::PrimitiveType::kPointList:
            topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
            polygonal = false;
            if (src.indexed) convertAll(false);
            break;
        case xenos::PrimitiveType::kLineList:
            topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
            polygonal = false;
            if (src.indexed) convertAll(false);
            break;
        case xenos::PrimitiveType::kLineStrip:
            topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
            polygonal = false;
            restart = restartEnabled;
            if (src.indexed) convertAll(restart);
            break;
        case xenos::PrimitiveType::kTriangleList:
            topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            if (src.indexed) convertAll(false);
            break;
        case xenos::PrimitiveType::kTriangleStrip:
            topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
            restart = restartEnabled;
            if (src.indexed) convertAll(restart);
            break;
        case xenos::PrimitiveType::kTriangleFan:
        case xenos::PrimitiveType::kPolygon:
            topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            forEachRun([&](const std::vector<uint32_t>& r) {
                for (size_t k = 2; k < r.size(); k++)
                    indices.insert(indices.end(), { r[0], r[k - 1], r[k] });
            });
            break;
        case xenos::PrimitiveType::kQuadList:
            topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            forEachRun([&](const std::vector<uint32_t>& r) {
                for (size_t k = 0; k + 3 < r.size(); k += 4)
                    indices.insert(indices.end(), { r[k], r[k + 1], r[k + 2], r[k], r[k + 2], r[k + 3] });
            });
            break;
        case xenos::PrimitiveType::kQuadStrip:
            topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            forEachRun([&](const std::vector<uint32_t>& r) {
                for (size_t k = 0; k + 3 < r.size(); k += 2)
                    indices.insert(indices.end(), { r[k], r[k + 1], r[k + 3], r[k], r[k + 3], r[k + 2] });
            });
            break;
        case xenos::PrimitiveType::kLineLoop:
            topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
            polygonal = false;
            forEachRun([&](const std::vector<uint32_t>& r) {
                for (size_t k = 0; k + 1 < r.size(); k++)
                    indices.insert(indices.end(), { r[k], r[k + 1] });
                if (r.size() > 2)
                    indices.insert(indices.end(), { r.back(), r[0] });
            });
            break;
        case xenos::PrimitiveType::kRectangleList:
            // Expanded by the vertex shader (4th corner = v1 + v2 - v0).
            topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            vertexCount = count / 3 * 6;
            pc.indexInfo = 1u << 3;
            if (src.indexed)
            {
                pc.indexAddress = src.base;
                pc.indexInfo |= (src.is32 ? 1u : 0u) | (src.endian << 1);
            }
            break;
        default:
            s_stats.skipped++;
            LogOnce(0x1000 + uint32_t(initiator.prim_type), "primitive type %u not supported",
                uint32_t(initiator.prim_type));
            return;
        }
        if (useIndices)
            vertexCount = directIndices ? count : uint32_t(indices.size());
        // 16-bit indices when 0xFFFF means what the pipeline says: restart
        // with the guest's reset index 0xFFFF, or no restart and no 0xFFFF
        // index (Metal can't turn primitive restart off).
        bool indices16 = false;
        if (directIndices && !src.is32)
        {
            if (restart)
                indices16 = resetIndex == 0xFFFF;
            else
            {
                const uint16_t* p = reinterpret_cast<const uint16_t*>(s_sharedHost + (src.base & 0x1FFFFFFE));
                indices16 = std::find(p, p + count, uint16_t(0xFFFF)) == p + count;  // byte order doesn't matter
            }
        }
        const VkDeviceSize indexBytes = directIndices ? VkDeviceSize(count) * (indices16 ? 2 : 4) : indices.size() * 4;
        if (vertexCount == 0)
            return;

        // Pipeline state.
        const TextureLayout& layout = GetTextureLayout(*s_vs, *s_ps);
        PipelineKey key{};
        key.vs = s_vs->hash;
        key.ps = s_ps->hash;
        key.topology = topology;
        key.restart = restart;
        static const uint32_t blendRegs[4] = { XE_GPU_REG_RB_BLENDCONTROL0, XE_GPU_REG_RB_BLENDCONTROL1,
            XE_GPU_REG_RB_BLENDCONTROL2, XE_GPU_REG_RB_BLENDCONTROL3 };
        for (int i = 0; i < 4; i++)
        {
            key.blend[i] = ~0u;
            if (!color[i])
                continue;
            key.colorFormats[i] = color[i]->vkFormat;
            uint32_t bc = s_regs[blendRegs[i]] & 0x1FFF1FFF;
            // (One, Zero, Add) for both colour and alpha is "blending off".
            if (bc != 0x00010001 && color[i]->blendable && !(adoptedColor & (1u << i)))
                key.blend[i] = bc;
        }
        key.colorMask = writeMask;  // (none for adopted targets)
        if (depth)
        {
            key.depthFormat = depth->vkFormat;
            key.depthControl = adoptedDepth ? 0u : depthControl.value;  // adopted: no depth or stencil test or write
        }
        auto clipCntl = Reg<reg::PA_CL_CLIP_CNTL>();
        bool depthBias = polygonal && (modeCntl.poly_offset_front_enable || modeCntl.poly_offset_para_enable);
        uint32_t cull = 0;
        if (polygonal && initiator.prim_type != xenos::PrimitiveType::kRectangleList)
            cull = (modeCntl.cull_front ? VK_CULL_MODE_FRONT_BIT : 0) | (modeCntl.cull_back ? VK_CULL_MODE_BACK_BIT : 0);
        auto colorControlKey = Reg<reg::RB_COLORCONTROL>();
        bool alphaTest = colorControlKey.alpha_test_enable && colorControlKey.alpha_func != xenos::CompareFunction::kAlways;
        key.raster = cull | (modeCntl.face ? 4u : 0u) | (clipCntl.clip_disable ? 8u : 0u) | (depthBias ? 16u : 0u) |
            (alphaTest ? 32u : 0u);

        bool pipelinePending;
        VkPipeline pipeline = GetPipeline(key, layout.pipeline, pipelinePending);
        if (pipelinePending)
            return;  // still compiling (counted in OnSwap's log)
        if (pipeline == VK_NULL_HANDLE)
            return skip(5, "pipeline creation failed");

        // Textures (may upload through the ring and so flush: before the
        // constants are allocated).
        if (s_textureCache->on && settings::Generation() != s_textureCache->settingsGeneration)
        {
            s_textureCache->settingsGeneration = settings::Generation();  // the anisotropy setting, in the samplers
            s_textureCache->generation++;
        }
        VkDescriptorImageInfo textureInfos[96];
        for (uint32_t d = 0; d < 3; d++)
            for (uint32_t n = 0; n < 32; n++)
            {
                if (!(((layout.vsMasks[d] | layout.psMasks[d]) >> n) & 1))
                    continue;
                VkDescriptorImageInfo& ii = textureInfos[d * 32 + n];
                ii = { s_sampler, s_dummy[d].view, VK_IMAGE_LAYOUT_GENERAL };
                if (!(s_exp & 8))  // NFSMW_GPU_EXP bit 8: placeholders only (texture cost experiment)
                    ii.imageView = GetTexture(n, d == 0 ? TexDim::k2D : d == 1 ? TexDim::k3D : TexDim::kCube, ii.sampler);
            }

        // Vertex data the guest may reuse before the GPU runs this draw (its
        // fences are published early, see g_eagerWrites): small ranges are
        // copied into the ring now and the shader reads the copy (vmem);
        // larger ones (static meshes, the triple-buffered dynamic buffers)
        // are guarded by the write watch. A range the GPU itself writes
        // first (a resolve into vertex memory) is never copied from the CPU.
        // Vertex data snapshots, copied into the ring with this draw's
        // constants and indices (one allocation: a submit between two would
        // leave the snapshot in the previous submission's slot).
        struct SnapshotRange { uint32_t base, size; };
        SnapshotRange copies[4];
        uint32_t copyCount = 0;
        VkDeviceSize copyBytes = 0;
        if (g_eagerWrites || s_shadowMode)
        {
            constexpr uint32_t kSnapshotMax = 64 * 1024;
            using Range = SnapshotRange;
            for (const Shader* shader : { s_vs, s_ps })
                for (uint32_t word = 0; word < 3; word++)
                    for (uint32_t bits = shader->info.vertexFetchConstants[word]; bits; bits &= bits - 1)
                    {
                        uint32_t constant = word * 32 + uint32_t(__builtin_ctz(bits));
                        const uint32_t* vf = &s_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + constant * 2];
                        Range r{ vf[0] & 0x1FFFFFFC, ((vf[1] >> 2) & 0xFFFFFF) * 4 };
                        r.size = std::min<uint32_t>(r.size, uint32_t(kSharedSize) - r.base);
                        if (r.size == 0 || std::any_of(copies, copies + copyCount, [&](const Range& c) { return c.base == r.base && c.size == r.size; }))
                            continue;
                        if (g_eagerWrites && r.size <= kSnapshotMax && copyCount < 4 &&
                            !writewatch::GpuWritePending(r.base, r.size) && !writewatch::AnyGpuOwned(r.base, r.size))
                        {
                            copies[copyCount++] = r;
                            copyBytes += (r.size + 255) & ~255u;
                        }
                        else
                        {
                            EnsureShadow(r.base, r.size);
                            GuardGpu(r.base, r.size, false, "vertices");
                        }
                    }
            // Indices the shader fetches itself.
            if (pc.indexAddress)
            {
                EnsureShadow(pc.indexAddress, count << (src.is32 ? 2 : 1));
                GuardGpu(pc.indexAddress, count << (src.is32 ? 2 : 1), false, "indices");
            }
        }

        // NFSMW_LOG_TEXTURE_USE=<hex base>: dump the shaders of draws that
        // sample the texture at that address (once per shader pair).
        if (s_drawUsesLoggedTexture)
        {
            s_drawUsesLoggedTexture = false;
            static std::unordered_set<uint64_t> dumped;
            if (dumped.insert(s_vs->hash ^ (s_ps->hash << 1)).second)
            {
                std::string path = std::format("build/texuse_vs_{:016x}.glsl", s_vs->hash);
                if (FILE* f = fopen(path.c_str(), "w")) { fputs(s_vs->info.glsl.c_str(), f); fclose(f); }
                path = std::format("build/texuse_ps_{:016x}.glsl", s_ps->hash);
                if (FILE* f = fopen(path.c_str(), "w")) { fputs(s_ps->info.glsl.c_str(), f); fclose(f); }
                fprintf(stderr, "[texuse] %08X (%s) sampled by vs %016llx ps %016llx; vertex fetch constants:", s_logTextureUse, s_loggedTextureFetch.c_str(),
                    (unsigned long long)s_vs->hash, (unsigned long long)s_ps->hash);
                for (uint32_t word = 0; word < 3; word++)
                    for (uint32_t bits = s_vs->info.vertexFetchConstants[word]; bits; bits &= bits - 1)
                    {
                        uint32_t c = word * 32 + uint32_t(__builtin_ctz(bits));
                        const uint32_t* vf = &s_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + c * 2];
                        fprintf(stderr, " [%u: %08X %08X]", c, vf[0], vf[1]);
                        uint32_t words = std::min<uint32_t>((vf[1] >> 2) & 0xFFFFFF, 128);
                        for (uint32_t i = 0; i < words; i++)
                        {
                            uint32_t bits = GpuSwap(LoadPhysical((vf[0] & 0x1FFFFFFC) + i * 4), vf[1] & 3);
                            float fv;
                            memcpy(&fv, &bits, 4);
                            fprintf(stderr, "%s%.3f", i % 20 == 0 ? "\n[texuse]   v: " : " ", fv);
                        }
                    }
                fprintf(stderr, "\n");
            }
        }

        // Per-draw data in the ring (may submit when full, so before
        // recording). Constants are uploaded only when the game wrote any
        // constant register since the last upload (g_constantsDirty) or the
        // last copy is in an older submission's slot; one allocation holds
        // both, since a submit between two would recycle the first.
        VkDeviceSize constantsOffset = s_lastConstantsOffset;
        VkDeviceSize indexOffset = 0;
        // A shader with packed constants reads its used ones from slots
        // 0..N-1 of its stage's range, so the upload's layout is the shader's
        // (null: the full file). ~20 vec4s instead of 256 per stage: constants
        // were 9 KB an upload, ~2,500 uploads a frame into the BAR ring.
        const Shader* constLayoutVs = s_vs->info.constPacked ? s_vs : nullptr;
        const Shader* constLayoutPs = s_ps->info.constPacked ? s_ps : nullptr;
        while (true)
        {
            bool uploadConstants = g_constantsDirty.load(std::memory_order_relaxed) ||
                s_lastConstantsSubmission != s_submissionCounter || !s_recording ||
                constLayoutVs != s_lastConstLayoutVs || constLayoutPs != s_lastConstLayoutPs;
            VkDeviceSize need = (uploadConstants ? kConstantsSize : 0) + (useIndices ? 256 + indexBytes : 0) +
                (copyCount ? 256 + copyBytes : 0);
            if (need == 0)
                break;
            uint64_t before = s_submissionCounter;
            VkDeviceSize at = RingAllocate(need);
            if (!uploadConstants && s_submissionCounter != before)
                continue;  // submitted meanwhile: the cached constants are gone
            if (uploadConstants)
            {
                g_constantsDirty.store(false, std::memory_order_relaxed);
                constantsOffset = at;
                uint8_t* c = s_ringHost + at;
                auto copyStage = [&](const Shader* packed, uint32_t base) {
                    const uint32_t* src = &s_regs[XE_GPU_REG_SHADER_CONSTANT_000_X + base * 4];
                    if (!packed)
                    {
                        memcpy(c + base * 16, src, 256 * 16);
                        return;
                    }
                    uint32_t slot = 0;
                    for (uint32_t q = 0; q < 4; q++)
                        for (uint64_t bits = packed->info.constUsed[q]; bits; bits &= bits - 1, slot++)
                            memcpy(c + (base + slot) * 16, src + (q * 64 + uint32_t(__builtin_ctzll(bits))) * 4, 16);
                };
                copyStage(constLayoutVs, 0);    // pc.vsConstBase
                copyStage(constLayoutPs, 256);  // pc.psConstBase
                s_lastConstLayoutVs = constLayoutVs;
                s_lastConstLayoutPs = constLayoutPs;
                memcpy(c + 512 * 16, &s_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], 48 * 16);
                memcpy(c + 560 * 16, &s_regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031], 2 * 16);
                memcpy(c + 562 * 16, &s_regs[XE_GPU_REG_SHADER_CONSTANT_LOOP_00], 8 * 16);
                memset(c + 570 * 16, 0, 4 * 16);
                s_lastConstantsOffset = at;
                s_lastConstantsSubmission = s_submissionCounter;
                s_stats.constantUploads++;
            }
            if (useIndices)
            {
                indexOffset = (at + (uploadConstants ? kConstantsSize : 0) + 255) & ~VkDeviceSize(255);
                if (directIndices)
                {
                    ConvertIndices(s_ringHost + indexOffset, src, count, indices16, restart, resetIndex);
                    // NFSMW_CP_OPT_VERIFY=1: compare with the plain conversion.
                    static const bool verify = std::getenv("NFSMW_CP_OPT_VERIFY") != nullptr;
                    if (verify)
                    {
                        static uint64_t checked = 0, bad = 0;
                        for (uint32_t i = 0; i < count; i++)
                        {
                            uint32_t v = src.Get(i);
                            uint32_t want = restart && v == resetIndex ? 0xFFFFFFFFu : v, got;
                            if (indices16)
                            {
                                uint16_t g16;
                                memcpy(&g16, s_ringHost + indexOffset + size_t(i) * 2, 2);
                                got = restart && g16 == 0xFFFF ? 0xFFFFFFFFu : g16;
                            }
                            else
                                memcpy(&got, s_ringHost + indexOffset + size_t(i) * 4, 4);
                            if (got != want && bad++ < 10)
                                fprintf(stderr, "[cpverify] index mismatch at %u: %08X, want %08X (32-bit %d, endian %u, restart %d)\n",
                                    i, got, want, int(src.is32), src.endian, int(restart));
                        }
                        if (++checked % 100000 == 0)
                            fprintf(stderr, "[cpverify] %llu indexed draws checked, %llu mismatches\n", (unsigned long long)checked, (unsigned long long)bad);
                    }
                }
                else
                    memcpy(s_ringHost + indexOffset, indices.data(), indices.size() * 4);
            }
            if (copyCount)
            {
                VkDeviceSize snapshot = (at + (uploadConstants ? kConstantsSize : 0) + (useIndices ? 256 + indexBytes : 0) + 255) & ~VkDeviceSize(255);
                s_stats.snapshotBytes += copyBytes;
                for (uint32_t i = 0; i < copyCount; i++)
                {
                    memcpy(s_ringHost + snapshot, s_sharedHost + copies[i].base, copies[i].size);
                    pc.vfRedirect[i][0] = copies[i].base;
                    pc.vfRedirect[i][1] = copies[i].size;
                    pc.vfRedirect[i][2] = uint32_t(snapshot);
                    snapshot += (copies[i].size + 255) & ~255u;
                }
            }
            break;
        }

        // The attachments' common size (the pass extent) and this draw's
        // scissor, which is also the area the pass must cover (NFSMW_PASS_MERGE
        // repeats it before adopting the open pass's targets: areaWith).
        // Untiled, tile 0 draws the whole frame: the whole target, or with
        // NFSMW_UNTILED_AREA its viewport (UntiledScissor).
        // In guest pixels; the host viewport and scissor are x (scaleX, scaleY)
        // (every attachment of a pass shares the surface pitch, so its scale).
        uint32_t targetW = UINT32_MAX, targetH = UINT32_MAX, scaleX = 1, scaleY = 1;
        for (RenderTarget* rt : { color[0], color[1], color[2], color[3], depth })
            if (rt)
            {
                targetW = std::min(targetW, rt->guestWidth);
                targetH = std::min(targetH, rt->guestHeight);
                scaleX = rt->scaleX;
                scaleY = rt->scaleY;
            }
        float w = float(targetW), h = float(targetH);
        RenderTarget* const attached[5] = { color[0], color[1], color[2], color[3], depth };
        Rect sc = GuestScissor();
        int untiledWhy = 4;
        if (untiledPass)
            sc = UntiledScissor(attached, int32_t(w), int32_t(h), &untiledWhy);
        int32_t x0 = std::clamp(sc.x0, 0, int32_t(w)), y0 = std::clamp(sc.y0, 0, int32_t(h));
        int32_t x1 = std::clamp(sc.x1, x0, int32_t(w)), y1 = std::clamp(sc.y1, y0, int32_t(h));
        // NFSMW_UNTILED_AREA: a draw whose area its targets' open pass doesn't
        // cover (a whole-target draw after cut ones, another viewport) would
        // restart that pass over the union, a pass more and the union
        // reloaded, which no untiled draw did before (each one's area was the
        // whole target): those targets keep the whole target from now on, as
        // this draw does, so they restart this once. (Checked before a vertex
        // barrier may end the pass: then it gives way a draw early.)
        if (untiledPass && untiledWhy != 4 && x1 > x0 && y1 > y0 && s_passActive && depth == s_passDepth &&
            std::equal(color, color + 4, s_passColor) &&
            !Contains(s_passArea, { { x0 * int32_t(scaleX), y0 * int32_t(scaleY) }, { uint32_t(x1 - x0) * scaleX, uint32_t(y1 - y0) * scaleY } }))
        {
            uint64_t key = 0;
            for (RenderTarget* rt : attached)
                if (rt)
                {
                    rt->wholeArea = true;
                    key = key * 0x9E3779B1ull + ((uint64_t(rt->base) << 20) ^ (uint64_t(rt->pitch) << 1) ^ uint64_t(rt->depth));
                }
            LogOnce((0x33ull << 56) | (key & 0xFFFFFFFFFFFFFFull), "untiled area: frame %llu vs_%016llx ps_%016llx would restart its targets' "
                "pass (area %ux%u@%d,%d) for %dx%d@%d,%d: they keep the whole target from now on", (unsigned long long)s_frame,
                (unsigned long long)s_vs->hash, (unsigned long long)s_ps->hash, s_passArea.extent.width, s_passArea.extent.height,
                s_passArea.offset.x, s_passArea.offset.y, (x1 - x0) * int32_t(scaleX), (y1 - y0) * int32_t(scaleY), x0 * int32_t(scaleX),
                y0 * int32_t(scaleY));
            if (untiledWhy == 0)
            {
                x0 = y0 = 0;
                x1 = int32_t(w);
                y1 = int32_t(h);
                untiledWhy = 3;
            }
        }
        if (untiledPass && untiledWhy != 4 && s_logUntiledArea)
        {
            if (untiledWhy != 0)
                s_ua.whole++;
            else if (x1 <= x0 || y1 <= y0)
                s_ua.empty++;
            else
                s_ua.cut++;
            if (untiledWhy == 1)
                s_ua.clipOff++;
            else if (untiledWhy == 2)
                s_ua.noViewport++;
            else if (untiledWhy == 3)
                s_ua.wholeArea++;
            if (untiledWhy == 1 || untiledWhy == 2)
                LogOnce((0x30ull << 56) ^ s_vs->hash ^ (s_ps->hash << 1), "untiled area: vs_%016llx ps_%016llx keeps the whole target (%s; VTE %03X, clip control %08X)",
                    (unsigned long long)s_vs->hash, (unsigned long long)s_ps->hash, untiledWhy == 1 ? "clipping off" : "no viewport transform",
                    Reg<reg::PA_CL_VTE_CNTL>().value & 0xFFF, Reg<reg::PA_CL_CLIP_CNTL>().value);
            // Each distinct cut per surface (pitch, MSAA, window scissor),
            // once (the map looked up only when it differs from the last).
            if (untiledWhy == 0)
            {
                static uint64_t lastKey = 0;
                auto wtl = Reg<reg::PA_SC_WINDOW_SCISSOR_TL>();
                auto wbr = Reg<reg::PA_SC_WINDOW_SCISSOR_BR>();
                uint64_t key = (0x32ull << 56) ^ (uint64_t(pitch) << 40) ^ (uint64_t(surface.msaa_samples) << 38) ^ (uint64_t(wbr.br_x) << 50) ^
                    (uint64_t(x0) << 26) ^ (uint64_t(y0) << 14) ^ (uint64_t(x1) << 2) ^ (uint64_t(y1) * 0x9E3779B1ull);
                if (key != lastKey)
                    LogOnce(key, "untiled area: pitch %u msaa %u target %dx%d, window scissor (%u,%u)-(%u,%u): draws cut to (%d,%d)-(%d,%d)%s",
                        pitch, uint32_t(surface.msaa_samples), int32_t(w), int32_t(h), wtl.tl_x, wtl.tl_y, wbr.br_x, wbr.br_y, x0, y0, x1, y1,
                        x1 <= x0 || y1 <= y0 ? " (nothing: not drawn)" : "");
                lastKey = key;
            }
        }
        // NFSMW_UNTILED_AREA, for Resolve's check: the untiled draws cut short
        // of the attachments' common size (a smaller attachment cuts the
        // others as before), even to nothing, what they drew and that size.
        // Whole-target draws lose nothing, nor do the targets a draw adopts
        // (never written). Two cuts whose union isn't exact (disjoint
        // viewports: the gap would count as drawn) fall back to the whole
        // target from the next draw.
        if (untiledPass && s_untiledArea)
        {
            const bool cut = untiledWhy == 0 && (x0 > 0 || y0 > 0 || x1 < int32_t(w) || y1 < int32_t(h));
            const VkRect2D drew{ { x0 * int32_t(scaleX), y0 * int32_t(scaleY) }, { uint32_t(x1 - x0) * scaleX, uint32_t(y1 - y0) * scaleY } };
            for (int i = 0; i < 5; i++)
                if (RenderTarget* rt = attached[i]; rt && !(i < 4 ? (adoptedColor >> i) & 1 : adoptedDepth))
                {
                    if (rt->areaFrame != s_frame)
                    {
                        rt->areaFrame = s_frame;
                        rt->cut = false;
                    }
                    if (!cut)
                        continue;
                    if (!rt->cut)
                    {
                        rt->cutDrawn = {};
                        rt->cutReach = {};
                    }
                    rt->cut = true;
                    rt->cutReach = { std::max(rt->cutReach.width, uint32_t(w) * scaleX), std::max(rt->cutReach.height, uint32_t(h) * scaleY) };
                    if (x1 <= x0 || y1 <= y0)
                        continue;
                    const VkRect2D& was = rt->cutDrawn;
                    if (was.extent.width && !Contains(was, drew) && !Contains(drew, was) && !Adjoins(was, drew))
                    {
                        rt->wholeArea = true;
                        LogOnce((0x34ull << 56) | (uint64_t(rt->base) << 24) | (uint64_t(rt->pitch) << 1) | uint64_t(rt->depth),
                            "untiled area: frame %llu target b%u p%u%s cut to %ux%u@%d,%d and %ux%u@%d,%d: its draws keep the whole target from now on",
                            (unsigned long long)s_frame, rt->base, rt->pitch, rt->depth ? " (depth)" : "", was.extent.width, was.extent.height,
                            was.offset.x, was.offset.y, drew.extent.width, drew.extent.height, drew.offset.x, drew.offset.y);
                    }
                    rt->cutDrawn = was.extent.width ? Union(was, drew) : drew;
                }
        }
        if (x1 <= x0 || y1 <= y0)
            return;
        VkRect2D scissor{ { x0 * int32_t(scaleX), y0 * int32_t(scaleY) }, { uint32_t(x1 - x0) * scaleX, uint32_t(y1 - y0) * scaleY } };

        // Viewport: the guest transform is folded into the vertex shader
        // (pc.posScale) and the host viewport covers the whole target.
        auto vte = Reg<reg::PA_CL_VTE_CNTL>();
        float sx = vte.vport_x_scale_ena ? RegF(XE_GPU_REG_PA_CL_VPORT_XSCALE) : 1.0f;
        float sy = vte.vport_y_scale_ena ? RegF(XE_GPU_REG_PA_CL_VPORT_YSCALE) : 1.0f;
        float sz = vte.vport_z_scale_ena ? RegF(XE_GPU_REG_PA_CL_VPORT_ZSCALE) : 1.0f;
        float ox = vte.vport_x_offset_ena ? RegF(XE_GPU_REG_PA_CL_VPORT_XOFFSET) : 0.0f;
        float oy = vte.vport_y_offset_ena ? RegF(XE_GPU_REG_PA_CL_VPORT_YOFFSET) : 0.0f;
        float oz = vte.vport_z_offset_ena ? RegF(XE_GPU_REG_PA_CL_VPORT_ZOFFSET) : 0.0f;
        if (modeCntl.vtx_window_offset_enable)
        {
            auto wo = Reg<reg::PA_SC_WINDOW_OFFSET>();
            ox += float(wo.window_x_offset);
            oy += float(wo.window_y_offset);
        }
        if (Reg<reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero)
        {
            ox += 0.5f;
            oy += 0.5f;
        }
        // A counted draw stops at its viewport, as the console's does: the
        // host clips to the target, and a cube face's 256x256 viewport lies
        // in a 280-pitch target up to 896 rows tall, so a quad past the
        // face's edge would count samples the console never draws. A pixel
        // is in when its centre is. The edges are clamped to the scissor
        // while still floats (garbage in the viewport registers never
        // converts out of range; NaN lands on an edge). Wholly outside, the
        // draw is not drawn and begins no pass; else the pass is begun with
        // the unclamped area (pass decisions are those of any other draw).
        VkRect2D drawScissor = scissor;
        if (zpdCount)
        {
            if ((vte.value & 0xF) == 0xF && !clipCntl.clip_disable)  // (x and y scaled and offset)
            {
                const float fx = float(scaleX), fy = float(scaleY), ax = std::fabs(sx), ay = std::fabs(sy);
                const float vx0 = (ox - ax) * fx, vx1 = (ox + ax) * fx, vy0 = (oy - ay) * fy, vy1 = (oy + ay) * fy;
                const float lx = float(scissor.offset.x), hx = lx + float(scissor.extent.width);
                const float ly = float(scissor.offset.y), hy = ly + float(scissor.extent.height);
                auto edge = [](float v, float lo, float hi) { return int32_t(std::fmax(lo, std::fmin(std::ceil(v - 0.5f), hi))); };
                const int32_t cx0 = edge(vx0, lx, hx), cx1 = edge(vx1, lx, hx), cy0 = edge(vy0, ly, hy), cy1 = edge(vy1, ly, hy);
                if (cx1 <= cx0 || cy1 <= cy0)
                {
                    s_zpdCounts.clamped++;
                    ZpdLog("f%llu draw vs_%016llx ps_%016llx: nothing inside its viewport %.1f..%.1f x %.1f..%.1f (host px): not drawn",
                        (unsigned long long)s_frame, (unsigned long long)s_vs->hash, (unsigned long long)s_ps->hash, vx0, vx1, vy0, vy1);
                    return;
                }
                drawScissor = { { cx0, cy0 }, { uint32_t(cx1 - cx0), uint32_t(cy1 - cy0) } };
            }
            else
            {
                s_zpdCounts.unclamped++;
                LogOnce((0x2Aull << 56) | 1, "occlusion query: a counted draw without its viewport transform or with clipping off "
                    "(VTE %03X, clip control %08X): not clamped to its viewport", vte.value & 0xFFF, clipCntl.value);
            }
        }
        if (s_passBarrier >= 2)
            VertexBarrierIfNeeded(layout.vsMasks, pc.indexAddress, pc.indexAddress ? count << (src.is32 ? 2 : 1) : 0);
        BeginPass(color, depth, scissor);
        if (s_profiling && !s_passInfos.empty())
        {
            PassInfo& pi = s_passInfos.back();
            if (pi.draws++ < 3)
                pi.desc += std::format(" [vs_{:016x} ps_{:016x} n{}]", s_vs->hash, s_ps->hash, vertexCount);
        }
        scissor = drawScissor;
        pc.posScale[0] = 2.0f * sx / w;
        pc.posScale[1] = 2.0f * sy / h;
        pc.posScale[2] = 2.0f * ox / w - 1.0f;
        pc.posScale[3] = 2.0f * oy / h - 1.0f;
        pc.flags = 1u | (vte.vtx_w0_fmt ? 2u : 0u) | (vte.vtx_xy_fmt ? 4u : 0u) | (vte.vtx_z_fmt ? 8u : 0u);
        // Ultrawide (game/ultrawide): the 16:9 guest frame is stretched to the
        // screen and the front-end (HUD and menu) matrix is narrowed by s to
        // match. Full-screen front-end layers (fades to black, the dims behind
        // dialogs and menus, the loading screen's backdrop and band) are
        // authored ~29% past the 16:9 edges; narrowed by s (0.744 at 3440x1440)
        // they stopped ~1.8% short of each screen edge and the world showed
        // through. A front-end draw whose every primitive spans the whole 16:9
        // width keeps its 16:9 extent (still past both screen edges).
        // Vert+ (a screen narrower than 16:9) is the same in y: the front-end
        // matrix's y scale is narrowed by sv (0.75 on a 4:3 iPad), so a layer
        // that ended at the 16:9 frame's top and bottom ends at the 16:9
        // band's. One that spans the whole width (as above) and reaches both
        // band edges is stretched in y to the screen's top and bottom; the
        // loading band, short of them, keeps its height. Any other layer that
        // covers the whole band (a menu backdrop picture, made of one quad or
        // several) is scaled up in x and y alike about the centre until it
        // reaches the screen's top and bottom, its sides cropped ("cover"):
        // stretched in y alone it would be distorted by up to 1/sv. The rest
        // of the front end is clipped to the band (below).
        if (const float s = game::AspectScaleX(), sv = game::AspectScaleY(); s < 1.0f || sv < 1.0f)
        {
            if (const float feX = game::FrontEndScaleX(); feX > 0.0f)
            {
                // A front-end draw: c0..c3 hold the front-end matrix (x scale
                // as the hook left it, no rotation, no projection), and its
                // vertices are ePolys (32 bytes: x, y, z, colour, uv0, uv1;
                // written at 0x82445DC8) at vertex fetch constant 95.
                float c[16];
                memcpy(c, &s_regs[XE_GPU_REG_SHADER_CONSTANT_000_X], sizeof(c));
                const uint32_t* vf = &s_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 95 * 2];
                if (std::fabs(c[0] - feX) <= feX * 1e-5f && c[1] == 0.0f && c[2] == 0.0f &&
                    c[12] == 0.0f && c[13] == 0.0f && c[14] == 0.0f && c[15] == 1.0f && ((vf[0] & 3) == 3))
                {
                    // NFSMW_LOG_FRONTEND=1: each distinct full-width layer,
                    // and in Vert+ each draw that covers the band, sits at
                    // one of its edges or reaches past it (below), once, with
                    // its extents in the frame's clip space before any
                    // widening (to check the thresholds); 4096 lines at most.
                    static const bool logFrontEnd = std::getenv("NFSMW_LOG_FRONTEND") != nullptr;
                    // At most 32 lines per shader pair (an animated draw makes
                    // a new line each frame), so a long session still logs
                    // the screen it was played to.
                    auto logOnce = [](uint64_t vs, uint64_t ps, const std::string& line) {
                        static std::unordered_set<std::string> logged;
                        static std::unordered_map<std::string, uint32_t> perPair;
                        if (logged.size() >= 4096 || !logged.insert(line).second)
                            return;
                        uint32_t n = ++perPair[std::format("{:016x}{:016x}", vs, ps)];
                        if (n <= 32)
                            fprintf(stderr, "[frontend] %s\n", line.c_str());
                        else if (n == 33)
                            fprintf(stderr, "[frontend] vs_%016llx ps_%016llx: 32 lines, no more\n", (unsigned long long)vs, (unsigned long long)ps);
                    };
                    // c4..c7 the y column: 0, the y scale as the hook left it
                    // (its sign unverified), 0, the y translation.
                    const float feY = game::FrontEndScaleY();
                    // Vert+ acts on every draw it matches (Hor+ only on
                    // full-width layers): the vertex shader must read the
                    // matrix (registers c0..c3: the 16 floats above) and the
                    // ePolys (fetch constant 95), not just inherit them from
                    // the HUD drawn before it (the mirror's picture, a movie
                    // quad, a post-process pass).
                    const auto& vi = s_vs->info;
                    const bool feShader = ((vi.vertexFetchConstants[95 / 32] >> (95 % 32)) & 1) &&
                        (vi.constRelative || (vi.constUsed[0] & 0xFull) == 0xFull);
                    const bool tall = feShader && sv < 1.0f && feY > 0.0f && c[4] == 0.0f && c[6] == 0.0f &&
                        std::fabs(std::fabs(c[5]) - feY) <= feY * 1e-5f;
                    const bool textured = (s_ps->info.texture2DMask | s_ps->info.texture3DMask | s_ps->info.textureCubeMask) != 0;
                    // Quad, triangle and rectangle lists are measured: up to
                    // 16 quads to act on (layers are a few), more for the log
                    // (which says so). Hor+ stops at the first primitive short
                    // of the 16:9 width; Vert+ needs them all (the band's
                    // cover is their union).
                    const uint32_t prim = uint32_t(initiator.prim_type);
                    const uint32_t per = prim == 13 ? 4 : (prim == 4 || prim == 8) ? 3 : 0;
                    const bool small = count <= 64;
                    const bool scan = per && count % per == 0 && (small || (logFrontEnd && count <= 4096));
                    const bool measureY = tall || logFrontEnd;
                    bool valid = scan, fullWidth = scan, fullHeight = scan && tall;
                    float reach = 1e30f, reachY = 1e30f;  // the nearest screen edge any primitive reaches
                    float allLo = 1e30f, allHi = -1e30f, allLoY = 1e30f, allHiY = -1e30f;
                    // The primitives' areas inside the band (x +-1, s being 1
                    // in Vert+; y +-sv), in clip units: a quad's or rectangle's
                    // box, half a triangle's (a quad's half). The first eight
                    // primitives' extents for the log.
                    float bandArea = 0.0f, minPrimArea = 1e30f;
                    // The band this draw shows on a 16:9 frame: clip y of
                    // its translation's share outside the narrowing, +-sv.
                    const float bandMid = c[7] * (1.0f - sv), bandLo = bandMid - sv, bandHi = bandMid + sv;
                    bool outside = false;  // a primitive wholly above or below the band
                    uint32_t prims = 0;
                    std::string primList;
                    if (scan)
                    {
                        const uint32_t base = vf[0] & 0x1FFFFFFC, endian = vf[1] & 3;
                        const uint32_t words = (vf[1] >> 2) & 0xFFFFFF, first = s_regs[XE_GPU_REG_VGT_INDX_OFFSET];
                        for (uint32_t v0 = 0; v0 < count && valid && (fullWidth || measureY); v0 += per)
                        {
                            float lo = 1e30f, hi = -1e30f, loY = 1e30f, hiY = -1e30f;
                            for (uint32_t k = 0; k < per; k++)
                            {
                                uint32_t v = src.Get(v0 + k) + first;
                                if ((v + 1) * 8 > words)
                                {
                                    valid = false;
                                    break;
                                }
                                float p[3];
                                for (uint32_t j = 0; j < 3; j++)
                                {
                                    uint32_t b = GpuSwap(LoadPhysical(base + (v * 8 + j) * 4), endian);
                                    memcpy(&p[j], &b, 4);
                                }
                                float x = p[0] * c[0] + p[1] * c[1] + p[2] * c[2] + c[3];
                                lo = std::min(lo, x);
                                hi = std::max(hi, x);
                                if (measureY)
                                {
                                    float y = p[0] * c[4] + p[1] * c[5] + p[2] * c[6] + c[7];
                                    loY = std::min(loY, y);
                                    hiY = std::max(hiY, y);
                                }
                            }
                            if (!valid)
                                break;
                            // Only layers authored well past both 16:9 edges (the
                            // fades, dims and loading band at +-1.29): the title /
                            // Controls key art (+-1.105) keeps its shape.
                            fullWidth = fullWidth && lo <= -1.2f * s && hi >= 1.2f * s;
                            reach = std::min(reach, std::min(-lo, hi));
                            // Vert+: and down to both edges of the 16:9 band.
                            fullHeight = fullHeight && loY <= -0.99f * sv && hiY >= 0.99f * sv;
                            reachY = std::min(reachY, std::min(-loY, hiY));
                            allLo = std::min(allLo, lo);
                            allHi = std::max(allHi, hi);
                            allLoY = std::min(allLoY, loY);
                            allHiY = std::max(allHiY, hiY);
                            const float primArea = std::max(0.0f, std::min(hi, 1.0f) - std::max(lo, -1.0f)) *
                                std::max(0.0f, std::min(hiY, sv) - std::max(loY, -sv)) * (prim == 4 ? 0.5f : 1.0f);
                            bandArea += primArea;
                            minPrimArea = std::min(minPrimArea, primArea);
                            outside = outside || (measureY && (hiY < bandLo || loY > bandHi));
                            if (logFrontEnd && prims < 8)
                                primList += std::format(" [{:.3f}..{:.3f} {:.3f}..{:.3f}]", lo, hi, loY, hiY);
                            prims++;
                        }
                        fullWidth = fullWidth && valid;
                        fullHeight = fullHeight && valid;
                    }
                    // Saved before any widening: the band below is this
                    // draw's own transform of clip y = +-sv.
                    const float ps1 = pc.posScale[1], ps3 = pc.posScale[3];
                    float kX = 1.0f, kY = 1.0f, kCover = 1.0f;
                    if (s < 1.0f && small && fullWidth && reach < 1.0f)
                    {
                        // Just past the screen edges, not the full 1/s: a
                        // textured layer (the band's stripes) stays ~5% off.
                        kX = std::min(1.0f / s, 1.01f / reach);
                        pc.posScale[0] *= kX;
                        pc.posScale[2] *= kX;
                    }
                    // A flat full-screen layer (a fade or dim) is stretched in
                    // y alone; a textured one (a backdrop picture) is not, it
                    // would be distorted: it is a cover below.
                    const bool extended = sv < 1.0f && small && fullWidth && fullHeight && !textured;
                    if (extended && reachY < 1.0f)
                    {
                        // posScale.y alone: NDC y = y * posScale.y +
                        // posScale.w * w scales about posScale.w, the
                        // viewport's centre, also with a tile's window offset
                        // (NFSMW_TILING=1).
                        kY = std::min(1.0f / sv, 1.01f / reachY);
                        pc.posScale[1] *= kY;
                    }
                    // Cover: the draw's primitives together reach both band
                    // edges in x and y (within 1%) and fill 95% of its area
                    // (corner pieces, a frame's four sides or a list's rows
                    // don't). Scaled by up to 1/sv (just past the screen's
                    // top and bottom, as the fades), the same in x so it keeps
                    // its shape; flat ones too (nothing to distort, and they
                    // must not be clipped: the strips would show undimmed).
                    const bool coversBand = allLo <= -0.99f && allHi >= 0.99f && allLoY <= -0.99f * sv && allHiY >= 0.99f * sv;
                    // And nothing else in the batch: the primitives' areas add
                    // up to the band's (no overlap, no extra sprite drawn with
                    // the same texture) and each is a sixteenth of it at
                    // least (a backdrop in up to 4x4 tiles), so a dialog's
                    // pieces batched with a dim are never zoomed.
                    const bool cover = tall && small && valid && !extended && coversBand && bandArea >= 0.95f * 4.0f * sv &&
                        bandArea <= 1.05f * 4.0f * sv && minPrimArea >= 4.0f * sv / 16.0f;
                    if (const float reachAllY = std::min(-allLoY, allHiY); cover && reachAllY < 1.0f)
                    {
                        // posScale.x and .y alone: about the viewport's centre
                        // (clip 0,0), as above.
                        kCover = std::min(1.0f / sv, 1.01f / reachAllY);
                        pc.posScale[0] *= kCover;
                        pc.posScale[1] *= kCover;
                    }
                    // Vert+: everything else is clipped to the 16:9 band, the
                    // rows a 16:9 frame shows. Menus park elements just past
                    // the frame's top and bottom (clip y beyond +-1, never on
                    // a 16:9 screen), and the narrowed matrix brings them into
                    // the strips above and below the band (the autosave
                    // dialog's white box and warning triangle, top right).
                    // The band is clip y = +-sv through this draw's viewport
                    // transform, in the target's host pixels like the
                    // scissor: posScale covers the render scale (the host
                    // viewport is the target's size x scale) and a tile's
                    // window offset (NFSMW_TILING=1: posScale.w moves with
                    // it). A row is in it when its centre is, as for a quad
                    // that ends there (the edges snapped to 1/256 pixel, as
                    // the rasteriser snaps vertices, so float noise doesn't
                    // decide a tie): at 1x, 720 rows and sv 0.75, rows
                    // 90..629.
                    // A measured draw whose every primitive meets the band
                    // isn't clipped: a bar or gradient authored past the
                    // frame's edge (as the fades) reaches into the strips as
                    // before. One with a primitive wholly outside (parked)
                    // is, and so is one too big to measure (text batches).
                    const bool meetsBand = small && scan && valid && !outside;
                    bool clipped = false;
                    if (tall && !extended && !cover && !meetsBand && vte.vport_y_scale_ena && vte.vport_y_offset_ena)
                    {
                        const float hostH = h * float(scaleY);
                        const float n0 = bandLo * ps1 + ps3, n1 = bandHi * ps1 + ps3;  // NDC y of the band's edges
                        const float a = std::round((std::min(n0, n1) + 1.0f) * 0.5f * hostH * 256.0f) / 256.0f;
                        const float b = std::round((std::max(n0, n1) + 1.0f) * 0.5f * hostH * 256.0f) / 256.0f;
                        const int32_t top = std::max(scissor.offset.y, int32_t(std::ceil(a - 0.5f)));
                        const int32_t bottom = std::min(scissor.offset.y + int32_t(scissor.extent.height), int32_t(std::ceil(b - 0.5f)));
                        scissor.offset.y = top;
                        scissor.extent.height = uint32_t(std::max(bottom - top, 0));
                        clipped = true;
                    }
                    if (logFrontEnd && fullWidth)
                    {
                        logOnce(s_vs->hash, s_ps->hash, std::format("vs_{:016x} ps_{:016x} n{} x {:.3f}..{:.3f} y {:.3f}..{:.3f} c5 {:.4f} (front end y {:.4f}{}{}) "
                            "view scale x {:.4f} y {:.4f}: widened x{:.3f} y{:.3f}{}",
                            s_vs->hash, s_ps->hash, count, allLo, allHi, allLoY, allHiY, c[5], feY, tall ? "" : ", not matched",
                            textured ? ", textured" : "", s, sv, kX, kY, small ? "" : " (n > 64: not acted on)"));
                    }
                    if (logFrontEnd && sv < 1.0f)
                    {
                        // The union's share of the band's area, and the
                        // primitives' summed (overlaps count twice). Logged:
                        // a draw that covers the band, one of a tenth of it
                        // at either edge (a backdrop in tiles drawn one by
                        // one, a header or button strip), and what is clipped
                        // because it reaches past the band.
                        const float boxShare = std::max(0.0f, std::min(allHi, 1.0f) - std::max(allLo, -1.0f)) *
                            std::max(0.0f, std::min(allHiY, sv) - std::max(allLoY, -sv)) / (4.0f * sv);
                        const bool atPlus = allHiY >= 0.99f * sv, atMinus = allLoY <= -0.99f * sv;
                        const bool pastBand = allLoY < -1.001f * sv || allHiY > 1.001f * sv;
                        const std::string action = cover ? std::format("cover x{:.3f}", kCover)
                            : extended ? std::format("widened y{:.3f}", kY)
                            : clipped ? std::format("clipped to rows {}..{}", scissor.offset.y, scissor.offset.y + int32_t(scissor.extent.height))
                            : tall ? "left alone (no viewport y)" : "not matched, left alone";
                        if (!scan || !valid)
                        {
                            if (tall)
                                logOnce(s_vs->hash, s_ps->hash, std::format("band vs_{:016x} ps_{:016x} prim {} n{} (translation c3 {:.3f} c7 {:.3f}): not measured{}: {}",
                                    s_vs->hash, s_ps->hash, prim, count, c[3], c[7], textured ? ", textured" : "", action));
                        }
                        else if (boxShare >= 0.95f || (boxShare >= 0.1f && (atPlus || atMinus)) || (tall && pastBand))
                        {
                            // A cover's 2D textures (address, size): a picture
                            // loaded with the menu, or a copy of the frame
                            // (a resolve: then the cover would show a zoomed
                            // copy of the picture under it).
                            std::string textures;
                            for (uint32_t n = 0; n < 32; n++)
                                if (cover && (s_ps->info.texture2DMask & (1u << n)))
                                {
                                    xenos::xe_gpu_texture_fetch_t f;
                                    memcpy(&f, &s_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + n * 6], sizeof(f));
                                    textures += std::format(" t{} {:08X} {}x{}", n, (uint32_t(f.base_address) << 12) & 0x1FFFFFFFu,
                                        uint32_t(f.size_2d.width) + 1, uint32_t(f.size_2d.height) + 1);
                                }
                            logOnce(s_vs->hash, s_ps->hash, std::format("band vs_{:016x} ps_{:016x} prim {} n{} ({} primitives) x {:.3f}..{:.3f} y {:.3f}..{:.3f} "
                                "(band y +-{:.3f}; translation c3 {:.3f} c7 {:.3f}): box {:.0f}%, primitives {:.0f}%{}{}{}{}: {}{}{}{}",
                                s_vs->hash, s_ps->hash, prim, count, prims, allLo, allHi, allLoY, allHiY, sv, c[3], c[7], boxShare * 100.0f,
                                bandArea / (4.0f * sv) * 100.0f, textured ? ", textured" : "", atPlus ? ", at +y edge" : "",
                                atMinus ? ", at -y edge" : "", pastBand ? ", past it" : "", action, small ? "" : " (n > 64: not acted on)",
                                textures, primList));
                        }
                    }
                    // Nothing of the band inside the game's own scissor.
                    if (clipped && scissor.extent.height == 0)
                        return;
                }
            }
        }
        // NFSMW_LOG_DRAWS=<seconds>: every draw of one frame (target, viewport,
        // scissor, textures, shaders), consecutive duplicates folded.
        if (s_logDrawsNow)
        {
            std::string line = std::format("{}x{} vp s({:.1f},{:.1f}) o({:.1f},{:.1f}) sc({},{})-({},{}) vte{:03X} vs_{:016x} ps_{:016x} tex",
                targetW, targetH, sx, sy, ox, oy, x0, y0, x1, y1, vte.value & 0xFFF, s_vs->hash, s_ps->hash);
            for (uint32_t n = 0; n < 32; n++)
                if ((s_ps->info.texture2DMask | s_ps->info.texture3DMask | s_ps->info.textureCubeMask) & (1u << n))
                    line += std::format(" {}:{:08X}", n, (s_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + n * 6 + 1] & 0x1FFFF000u));
            static std::string last;
            static uint32_t repeats = 0;
            if (line == last)
                repeats++;
            else
            {
                if (repeats)
                    fprintf(stderr, "[draws]   (x%u more)\n", repeats);
                fprintf(stderr, "[draws] %s\n", line.c_str());
                last = line;
                repeats = 0;
            }
        }
        pc.vsConstBase = 0;
        pc.psConstBase = 256;
        pc.indexOffset = s_regs[XE_GPU_REG_VGT_INDX_OFFSET];
        auto colorControl = Reg<reg::RB_COLORCONTROL>();
        pc.alphaTest = colorControl.alpha_test_enable ? uint32_t(colorControl.alpha_func) : 7u;
        pc.alphaRef = RegF(XE_GPU_REG_RB_ALPHA_REF);

        float zmin = std::clamp(oz, 0.0f, 1.0f), zmax = std::clamp(oz + sz, 0.0f, 1.0f);
        VkViewport viewport{ 0.0f, 0.0f, w * float(scaleX), h * float(scaleY), zmin, zmax };

        // What this command buffer already has: commands that would set the
        // same value again are skipped (most draws change only a few). Set 0
        // stays bound and push constants stay valid across the texture
        // layouts (same set-0 layout and push-constant range); set 1 is
        // pushed again whenever the layout changes. Compute work and a new
        // command buffer reset it (s_stateEpoch).
        struct BoundState
        {
            uint64_t epoch = 0;
            VkPipeline pipeline = VK_NULL_HANDLE;
            VkViewport viewport{};
            VkRect2D scissor{};
            uint32_t stencil[6] = {};
            float blend[4] = {}, bias[3] = {};
            uint32_t setOffset = ~0u;
            VkPipelineLayout textureLayout = VK_NULL_HANDLE;
            uint32_t textureCount = ~0u;
            VkDescriptorImageInfo textures[96];
            DrawConstants pc{};
        };
        static BoundState bound;
        // The first draw after a reset sets everything: dynamic state must be
        // set before a draw uses it, whatever the values.
        const bool fresh = !(g_cpOpt & CP_OPT_SKIP_REDUNDANT) || bound.epoch != s_stateEpoch;
        if (fresh)
        {
            bound = BoundState{};
            bound.epoch = s_stateEpoch;
        }
        if (fresh || bound.pipeline != pipeline)
        {
            vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            bound.pipeline = pipeline;
        }
        if (fresh || memcmp(&bound.viewport, &viewport, sizeof(viewport)) != 0)
        {
            vkCmdSetViewport(s_cmd, 0, 1, &viewport);
            bound.viewport = viewport;
        }
        if (fresh || memcmp(&bound.scissor, &scissor, sizeof(scissor)) != 0)
        {
            vkCmdSetScissor(s_cmd, 0, 1, &scissor);
            bound.scissor = scissor;
        }
        auto front = Reg<reg::RB_STENCILREFMASK>();
        auto back = depthControl.backface_enable ? RegAt<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK_BF) : front;
        uint32_t stencil[6] = { front.stencilmask, back.stencilmask, front.stencilwritemask, back.stencilwritemask,
            front.stencilref, back.stencilref };
        if (fresh || memcmp(bound.stencil, stencil, sizeof(stencil)) != 0)
        {
            vkCmdSetStencilCompareMask(s_cmd, VK_STENCIL_FACE_FRONT_BIT, front.stencilmask);
            vkCmdSetStencilCompareMask(s_cmd, VK_STENCIL_FACE_BACK_BIT, back.stencilmask);
            vkCmdSetStencilWriteMask(s_cmd, VK_STENCIL_FACE_FRONT_BIT, front.stencilwritemask);
            vkCmdSetStencilWriteMask(s_cmd, VK_STENCIL_FACE_BACK_BIT, back.stencilwritemask);
            vkCmdSetStencilReference(s_cmd, VK_STENCIL_FACE_FRONT_BIT, front.stencilref);
            vkCmdSetStencilReference(s_cmd, VK_STENCIL_FACE_BACK_BIT, back.stencilref);
            memcpy(bound.stencil, stencil, sizeof(stencil));
        }
        float blendConstants[4] = { RegF(XE_GPU_REG_RB_BLEND_RED), RegF(XE_GPU_REG_RB_BLEND_GREEN),
            RegF(XE_GPU_REG_RB_BLEND_BLUE), RegF(XE_GPU_REG_RB_BLEND_ALPHA) };
        if (fresh || memcmp(bound.blend, blendConstants, sizeof(blendConstants)) != 0)
        {
            vkCmdSetBlendConstants(s_cmd, blendConstants);
            memcpy(bound.blend, blendConstants, sizeof(blendConstants));
        }
        float bias[3] = { 0.0f, 0.0f, 0.0f };
        if (depthBias)
        {
            bias[0] = RegF(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET) * float(1 << 24);
            // The slope factor is per host pixel: scaled with the resolution (Xenia).
            bias[2] = RegF(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE) / 16.0f * float(std::max(scaleX, scaleY));
        }
        if (fresh || memcmp(bound.bias, bias, sizeof(bias)) != 0)
        {
            vkCmdSetDepthBias(s_cmd, bias[0], bias[1], bias[2]);
            memcpy(bound.bias, bias, sizeof(bias));
        }

        uint32_t dynamicOffset = uint32_t(constantsOffset);
        if (fresh || bound.setOffset != dynamicOffset)
        {
            vkCmdBindDescriptorSets(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout.pipeline, 0, 1, &s_set0, 1, &dynamicOffset);
            bound.setOffset = dynamicOffset;
        }

        VkWriteDescriptorSet writes[96];
        uint32_t writeCount = 0;
        for (uint32_t d = 0; d < 3; d++)
            for (uint32_t n = 0; n < 32; n++)
            {
                if (!(((layout.vsMasks[d] | layout.psMasks[d]) >> n) & 1))
                    continue;
                VkWriteDescriptorSet& wds = writes[writeCount++];
                wds = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                wds.dstBinding = d * 32 + n;
                wds.descriptorCount = 1;
                wds.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                wds.pImageInfo = &textureInfos[d * 32 + n];
            }
        if (writeCount)
        {
            bool same = !fresh && bound.textureLayout == layout.pipeline && bound.textureCount == writeCount;
            for (uint32_t i = 0; same && i < writeCount; i++)  // (not memcmp: the struct has padding)
                same = bound.textures[i].sampler == writes[i].pImageInfo->sampler &&
                    bound.textures[i].imageView == writes[i].pImageInfo->imageView &&
                    bound.textures[i].imageLayout == writes[i].pImageInfo->imageLayout;
            if (!same)
            {
                s_pushDescriptorSet(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout.pipeline, 1, writeCount, writes);
                bound.textureLayout = layout.pipeline;
                bound.textureCount = writeCount;
                for (uint32_t i = 0; i < writeCount; i++)
                    bound.textures[i] = *writes[i].pImageInfo;
            }
        }
        if (fresh || memcmp(&bound.pc, &pc, sizeof(pc)) != 0)
        {
            vkCmdPushConstants(s_cmd, layout.pipeline, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
            bound.pc = pc;
        }

        // Guest memory this draw reads when the GPU runs it: vertex data, and
        // indices when the shader fetches them itself.


        if (g_eagerWrites && (++s_drawsSinceCheck & 127) == 0)
        {
            // Every 128 draws: submit when another thread waits on recorded
            // work (write-watch guard), or when the GPU is starving and the
            // recording is at least NFSMW_STARVE_SUBMIT_US old (default
            // 1000). Each submit costs the CP ~20 us on RADV plus a pass
            // restart; every 128 draws was ~25 submits a frame in races.
            static const int64_t starveUs = [] { const char* v = std::getenv("NFSMW_STARVE_SUBMIT_US"); return v ? int64_t(std::atoll(v)) : 1000; }();
            bool submit = writewatch::g_submitRequested.exchange(false);
            if (!submit && std::chrono::steady_clock::now() - s_recordStart >= std::chrono::microseconds(starveUs))
            {
                std::lock_guard lock(s_submitMutex);
                submit = s_inFlight.size() < 2;
            }
            if (submit)
            {
                // Record this draw first: submit right after it.
                s_submitAfterDraw = true;
            }
        }

        if (s_exp & 1)
            return;
        if (zpdCount)
        {
            // Weighted by the guest's samples per pixel (its MSAA; the host
            // renders 1) over host pixels per guest pixel (the render scale).
            const uint64_t segments = s_zpdSegNext;
            ZpdBeginSegment(1u << uint32_t(surface.msaa_samples), scaleX * scaleY);
            s_zpdCounts.draws++;
            if (ZpdLogging())
            {
                std::string targets = depth ? std::format("D[b{} p{}]", depth->base, depth->pitch) : "no depth";
                for (int i = 0; i < 4; i++)
                    if (color[i])
                        targets += std::format(" C{}[b{} p{}]{}", i, color[i]->base, color[i]->pitch, (adoptedColor >> i) & 1 ? " (pass's)" : "");
                ZpdLog("f%llu draw vs_%016llx ps_%016llx n%u: %s, x %u/%u, %s, scissor %ux%u@%d,%d (host px)",
                    (unsigned long long)s_frame, (unsigned long long)s_vs->hash, (unsigned long long)s_ps->hash, vertexCount,
                    s_zpdQuery < 0 ? "not counted (this command buffer's queries are used up)"
                        : std::format("{} {}", s_zpdSegNext != segments ? "new segment" : "segment", s_zpdSegNext - 1).c_str(),
                    1u << uint32_t(surface.msaa_samples), scaleX * scaleY, targets.c_str(), scissor.extent.width,
                    scissor.extent.height, scissor.offset.x, scissor.offset.y);
            }
        }
        if (useIndices)
        {
            vkCmdBindIndexBuffer(s_cmd, s_ring, indexOffset, indices16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(s_cmd, vertexCount, 1, 0, 0, 0);
        }
        else
        {
            vkCmdDraw(s_cmd, vertexCount, 1, 0, 0);
        }
        if (s_submitAfterDraw)
        {
            s_submitAfterDraw = false;
            SubmitRecorded(8);
        }
    }

    void Submit()
    {
        if (s_vk)
            SubmitRecorded(2);
    }

    void SubmitIfIdle()
    {
        if (!s_vk || !s_recording)
            return;
        size_t inFlight;
        {
            std::lock_guard lock(s_submitMutex);
            inFlight = s_inFlight.size();
        }
        if (inFlight < (g_eagerWrites ? 2u : 1u))
            SubmitRecorded(3);
    }

    void SubmitIfDue()
    {
        // With fences published mid-buffer (events), batching more work per
        // command buffer costs the game nothing while the GPU is busy anyway;
        // each Metal command buffer has a large fixed cost. Submit when the
        // queue is shallow, or when the recording has aged.
        // Eager writes: nothing waits on the recording, so keep the GPU fed.
        static const uint32_t depth = [] { const char* v = std::getenv("NFSMW_SUBMIT_DEPTH"); return v ? uint32_t(std::atoi(v)) : g_eagerWrites ? 2u : 1u; }();
        static const int64_t ageUs = [] { const char* v = std::getenv("NFSMW_SUBMIT_AGE_US"); return v ? int64_t(std::atoll(v)) : 2000; }();
        if (!s_vk || !s_recording)
            return;
        if (!s_fenceEvents)
        {
            SubmitRecorded(4);
            return;
        }
        size_t inFlight;
        {
            std::lock_guard lock(s_submitMutex);
            inFlight = s_inFlight.size();
        }
        if (inFlight < depth || std::chrono::steady_clock::now() - s_recordStart > std::chrono::microseconds(ageUs))
            SubmitRecorded(4);
    }

    void Flush()
    {
        if (!s_vk)
            return;
        SubmitRecorded(5);
        CpWaitTimer wait(CP_WAIT_FLUSH);
        std::unique_lock lock(s_submitMutex);
        s_submitCv.wait(lock, [] { return s_inFlight.empty(); });
    }

    void QueueWrite(std::function<void()> write)
    {
        if (!s_vk || g_eagerWrites)
        {
            write();
            return;
        }
        // Attach to the newest submission: the one being recorded (it then
        // becomes visible when that completes, a little later than strictly
        // needed, which is always safe for fences), else the last in flight.
        std::unique_lock lock(s_submitMutex);
        if (s_recording)
        {
            s_submissions[s_slot].writes.push_back({ std::move(write) });
            bool inPass = s_passActive;
            // With the GPU idle nothing would carry this write until the next
            // submit: send the work now so whoever waits on it isn't stalled.
            bool idle = s_inFlight.empty();
            lock.unlock();
            if (idle)
                SubmitRecorded(6);
            else if (!inPass)
                SignalWrites();  // in a pass, the pass end signals it
            return;
        }
        if (s_inFlight.empty())
        {
            lock.unlock();
            write();
            return;
        }
        s_submissions[s_inFlight.back()].writes.push_back({ std::move(write) });
    }

    // Occlusion queries, counted (NFSMW_OCCLUSION=1). The game brackets its
    // sun quads with them and turns the share of samples that passed into
    // the glare's strength (gpu/zpd_report.h has the reports). A bracket
    // runs from its BEGIN ZPD to its END ZPD; the draws in it are counted
    // in segments, each a precise occlusion query begun right before a
    // draw inside its pass (no pass is split for a query) and ended at the
    // pass's end, at the next ZPD or when the weight changes, so a segment
    // belongs to every bracket open while it ran (they may overlap; Vulkan
    // allows one active query). The END's report is stored when the newest
    // submission completes (every segment's count is in by then: the
    // completion thread harvests in submission order, never waiting), in
    // stream order, so a predicated-tiling query's last tile, which makes
    // it ready, lands last. Tile 0 holds the whole count (untiled
    // rendering: later tiles' draws are skipped, their brackets count 0),
    // as GetData sums the tiles. A result lands a frame or two after its
    // draw; the game polls without waiting and keeps its value meanwhile,
    // as on the console. Nothing here waits.
    bool ZpassDone(uint32_t address)
    {
        if (!s_zpdOn)
            return false;
        address &= 0x1FFFFFFF;
        if (address > kSharedSize - 32)
            return false;
        // Every ZPD ends the segment (inside the open pass: legal), so no
        // segment spans a bracket's edge.
        ZpdEndSegment();
        s_zpdCounts.zpds++;
        switch (zpd::Classify(address))
        {
        case zpd::Kind::Begin:
        {
            zpd::Bracket old;
            auto opened = s_zpdOpen.Open(address, s_zpdSegNext, s_frame, &old);
            s_zpdCounts.begins++;
            if (opened == zpd::BracketBook::Opened::Replaced)
            {
                s_zpdCounts.replaced++;
                LogOnce((0x2Cull << 56) | address, "occlusion query: BEGIN %08X again before its END (open since frame %llu): replaced",
                    address, (unsigned long long)old.frame);
            }
            else if (opened == zpd::BracketBook::Opened::DroppedOldest)
            {
                s_zpdCounts.dropped++;
                LogOnce((0x2Aull << 56) | 3, "occlusion query: more than %u brackets open: the oldest (BEGIN %08X, frame %llu) dropped",
                    zpd::BracketBook::kMax, old.beginAddress, (unsigned long long)old.frame);
            }
            ZpdLog("f%llu BEGIN %08X: segments from %llu, %u open", (unsigned long long)s_frame, address,
                (unsigned long long)s_zpdSegNext, s_zpdOpen.Size());
            return false;  // its zeros are stored as without counting
        }
        case zpd::Kind::End:
        {
            ZpdEnd end{ address, s_zpdSegNext, s_zpdSegNext, s_frame, std::chrono::steady_clock::now() };
            zpd::Bracket closed;
            if (s_zpdOpen.Close(address, &closed))
                end.s0 = closed.s0;
            else
            {
                s_zpdCounts.unmatched++;
                LogOnce((0x2Dull << 56) | address, "occlusion query: END %08X without its BEGIN: zero samples", address);
            }
            s_zpdCounts.ends++;
            ZpdLog("f%llu END %08X queued: segments %llu..%llu", (unsigned long long)s_frame, address,
                (unsigned long long)end.s0, (unsigned long long)end.s1);
            // With the newest submission, as QueueWrite: the one being
            // recorded, else the last in flight, else now (nothing in
            // flight: every count is in, harvested before its submission
            // left s_inFlight under this lock).
            std::lock_guard lock(s_submitMutex);
            if (s_recording)
                s_submissions[s_slot].zpdEnds.push_back(end);
            else if (!s_inFlight.empty())
                s_submissions[s_inFlight.back()].zpdEnds.push_back(end);
            else
                ZpdStore(end);
            return true;
        }
        default:
            s_zpdCounts.unknown++;
            LogOnce((0x2Eull << 56) | address, "occlusion query: report at %08X is neither a BEGIN (+0x20) nor an END (+0) of a 64-byte block: zero samples",
                address);
            return false;
        }
    }

    void LogOcclusionCounts(const char* what)
    {
        if (!s_zpdOn)  // (also when the device counts but the pools couldn't be made)
            return;
        std::string line;
        {
            std::lock_guard lock(s_zpdLatestMutex);
            for (const auto& [address, latest] : s_zpdLatest)
                line += std::format(" {:08X} {} (frame {})", address, latest.first, latest.second);
        }
        fprintf(stderr, "[zpd] %s: latest END counts:%s\n", what, line.empty() ? " none yet" : line.c_str());
    }

    bool PresentFrontBuffer(const uint32_t fetch[6], uint32_t width, uint32_t height)
    {
        if (!s_vk || width == 0 || height == 0)
            return false;
        // NFSMW_SHOW_TEXTURE=<hex base>,<width>,<height>: present that tiled
        // 32-bit guest texture instead of the front buffer (debugging).
        static const std::array<uint32_t, 3> showTexture = [] {
            std::array<uint32_t, 3> t{};
            if (const char* v = std::getenv("NFSMW_SHOW_TEXTURE"))
                sscanf(v, "%x,%u,%u", &t[0], &t[1], &t[2]);
            return t;
        }();
        uint32_t debugFetch[6];
        if (showTexture[0])
        {
            memcpy(debugFetch, fetch, sizeof(debugFetch));
            debugFetch[0] = (debugFetch[0] & ~(0x1FFu << 22)) | (((showTexture[1] + 31) / 32) << 22) | 0x80000000u;
            debugFetch[1] = (debugFetch[1] & 0xFFFu) | (showTexture[0] & 0x1FFFF000u);
            fetch = debugFetch;
            width = showTexture[1];
            height = showTexture[2];
        }
        uint32_t pitch = ((fetch[0] >> 22) & 0x1FF) << 5;
        struct { uint32_t base, pitch, tiled, endian, format, swizzle, width, height, sx, sy, slotWords, scaled; } pc = {
            fetch[1] & 0x1FFFF000u, pitch ? pitch : (width + 31) & ~31u, (fetch[0] >> 31) & 1, (fetch[1] >> 6) & 3,
            fetch[1] & 0x3F, (fetch[3] >> 1) & 0xFFF, width, height, 1, 1, 0, 0 };
        uint32_t frontBytes = pc.pitch * ((height + 31) & ~31u) * 4;
        // At host resolution when a scaled resolve wrote it (the game's frames;
        // movies and loading screens drawn at 1x use the 1x image).
        bool scaled = pc.tiled && !showTexture[0] && s_scaled.enabled && ScaledRangeValid(pc.base, frontBytes);
        uint32_t outW = scaled ? width * s_scaled.sx : width, outH = scaled ? height * s_scaled.sy : height;
        Image& front = scaled ? s_frontScaled : s_front;
        uint32_t& frontW = scaled ? s_frontScaledWidth : s_frontWidth;
        uint32_t& frontH = scaled ? s_frontScaledHeight : s_frontHeight;
        if (outW != frontW || outH != frontH)
        {
            // A new size: wait for any use of the old image before replacing it.
            DestroyFrontImage(front, frontW, frontH);
            front = CreateImage(VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, { outW, outH, 1 }, 1,
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,  // sampled: presenter post-processing
                VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT);
            Begin();
            ToGeneral(front.image, VK_IMAGE_ASPECT_COLOR_BIT, 1);
            frontW = outW;
            frontH = outH;
            fprintf(stderr, "[renderer] front image %ux%u (%s)\n", outW, outH, scaled ? "host resolution" : "1x");
        }
        if (scaled)
        {
            pc.width = outW;
            pc.height = outH;
            pc.sx = s_scaled.sx;
            pc.sy = s_scaled.sy;
            pc.slotWords = s_scaled.slotWords;
            pc.scaled = 1;
            s_scaled.untiledBytes += uint64_t(outW) * outH * 4;
        }
        else
            EnsureShadow(pc.base, frontBytes);
        EndPass();
        Begin();
        FullBarrier();  // after earlier work, including the presenter's last read
        VkDescriptorImageInfo ii{ VK_NULL_HANDLE, front.view, VK_IMAGE_LAYOUT_GENERAL };
        VkDescriptorBufferInfo pool = DummyStorage(s_scaled.pool);
        VkDescriptorBufferInfo table = DummyStorage(s_scaled.table);
        VkWriteDescriptorSet w[7] = {
            { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &s_sharedInfo[0], nullptr },
            { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii, nullptr, nullptr },
            { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &pool, nullptr },
            { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 3, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &table, nullptr },
        };
        s_stateEpoch++;
        vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_frontPipeline);
        s_pushDescriptorSet(s_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_frontLayout, 0, AddSharedParts(w, 4), w);
        vkCmdPushConstants(s_cmd, s_frontLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(s_cmd, (outW + 7) / 8, (outH + 7) / 8, 1);
        GuardGpu(pc.base, frontBytes, false, "front buffer");
        FullBarrier();
        SubmitRecorded(7);  // the presenter's blit comes later in queue order
        video::SubmitFrontImage(front.image, outW, outH, width, height);
        // Back at 1x (a live change): the host-resolution image is no longer
        // needed. The presenter has this frame to show instead.
        if (!scaled && s_frontScaled.image && s_renderScaleX * s_renderScaleY == 1)
            DestroyFrontImage(s_frontScaled, s_frontScaledWidth, s_frontScaledHeight);
        if (g_eagerWrites)
        {
            // With fences published early nothing else keeps the GPU from
            // falling behind: allow one frame in flight, as the console's
            // GPU never lags more. The ring then fills and the game waits.
            static uint64_t previousSwap = 0;
            uint64_t thisSwap = s_submissionCounter - 1;
            if (previousSwap)
            {
                CpWaitTimer wait(CP_WAIT_THROTTLE);
                std::unique_lock lock(s_submitMutex);
                s_submitCv.wait(lock, [] { return s_inFlight.empty() || s_submissions[s_inFlight.front()].number > previousSwap; });
            }
            previousSwap = thisSwap;
        }

        // NFSMW_CHECK_FRONT=<n>: at the n-th GPU present, read the image back
        // into build/front_check.ppm (verifies this path without a screen).
        // NFSMW_CHECK_FRONT_SEC=<s>: from s seconds after launch, 6 captures
        // 20 presents apart, into build/front_check_<k>.ppm.
        static const int checkAt = [] { const char* v = std::getenv("NFSMW_CHECK_FRONT"); return v ? std::atoi(v) : -1; }();
        static const double checkSec = [] { const char* v = std::getenv("NFSMW_CHECK_FRONT_SEC"); return v ? std::atof(v) : -1.0; }();
        static const auto launch = std::chrono::steady_clock::now();
        static int presents = 0, seriesStart = -1, seriesTaken = 0;
        presents++;
        std::string checkPath;
        if (presents == checkAt)
            checkPath = "build/front_check.ppm";
        else if (checkSec >= 0 && seriesTaken < 6)
        {
            if (seriesStart < 0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - launch).count() >= checkSec)
                seriesStart = presents;
            if (seriesStart >= 0 && (presents - seriesStart) % 20 == 0)
                checkPath = "build/front_check_" + std::to_string(seriesTaken++) + ".ppm";
        }
        if (!checkPath.empty())
            width = outW, height = outH;  // what was presented
        VkDeviceSize at = checkPath.empty() ? 0 : RingAllocate(VkDeviceSize(width) * height * 4);
        if (at == ~VkDeviceSize(0))
        {
            LogOnce(0x2400, "front buffer capture %ux%u doesn't fit a staging slot", width, height);
            checkPath.clear();
        }
        if (!checkPath.empty())
        {
            Begin();
            FullBarrier();
            VkBufferImageCopy region{};
            region.bufferOffset = at;
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.imageExtent = { width, height, 1 };
            vkCmdCopyImageToBuffer(s_cmd, front.image, VK_IMAGE_LAYOUT_GENERAL, s_ring, 1, &region);
            Flush();
            if (FILE* f = fopen(checkPath.c_str(), "wb"))
            {
                // One bulk copy out of the ring first: on a discrete GPU it
                // is uncached VRAM, where per-pixel reads took ~20 s a frame.
                std::vector<uint8_t> rgba(s_ringHost + at, s_ringHost + at + size_t(width) * height * 4), rgb(size_t(width) * height * 3);
                for (size_t i = 0; i < size_t(width) * height; i++)
                    memcpy(&rgb[i * 3], &rgba[i * 4], 3);
                fprintf(f, "P6\n%u %u\n255\n", width, height);
                fwrite(rgb.data(), 1, rgb.size(), f);
                fclose(f);
                fprintf(stderr, "[renderer] front buffer check written\n");
                LogOcclusionCounts(checkPath.c_str());  // NFSMW_OCCLUSION=1: the counts behind it
            }
        }
        return true;
    }

    void OnSwap(uint64_t frame)
    {
        if (s_vk)
            ApplyLiveSettings();
        if (s_uploadLog)
        {
            UploadLog& log = *s_uploadLog;
            if (log.now)  // s_frame just ended
            {
                std::string paths, writers;
                for (auto& [k, n] : log.byPath)
                    paths += std::format(" {} {}", k, n);
                for (auto& [k, n] : log.byWriters)
                    writers += std::format(" {} {},", k, n);
                fprintf(stderr, "[upload] frame %llu summary: %u uploads (%llu KB, %u ending a pass) | paths:%s | writers:%s | resolves %u (%u fused)\n",
                    (unsigned long long)s_frame, log.uploads, (unsigned long long)(log.bytes / 1024), log.endedPasses, paths.c_str(),
                    writers.c_str(), log.resolvesNow, log.fusedNow);
                log.uploads = log.endedPasses = log.resolvesNow = log.fusedNow = 0;
                log.bytes = 0;
                log.byPath.clear();
                log.byWriters.clear();
            }
            log.now = frame >= log.first && frame <= log.last;
            writewatch::SetWriterFrame(frame);
        }
        s_frame = frame;
        if (s_zpdOn)
            ZpdOnSwap(frame);
        // NFSMW_PASS_BARRIER: per frame of the window ending here, with the
        // level it ran at, before its [perf] line. Vertex barriers cost a
        // pass's overlap (a split: a pass too).
        if (s_passBarrierLevel && frame % 120 == 0)
        {
            static BarrierCounts last{};
            const BarrierCounts& c = s_barrierCounts;
            fprintf(stderr, "[barrier] level %u per frame: %.1f pass ends and %.1f resolves narrowed, %.2f full at the cap | vertex barriers %.2f"
                " (vertex data %.2f, indices %.2f, a texture %.2f), %.2f splitting a pass\n", s_passBarrier,
                double(c.passEnds - last.passEnds) / 120, double(c.resolves - last.resolves) / 120, double(c.capped - last.capped) / 120,
                double(c.vertex[0] + c.vertex[1] + c.vertex[2] - last.vertex[0] - last.vertex[1] - last.vertex[2]) / 120,
                double(c.vertex[0] - last.vertex[0]) / 120, double(c.vertex[1] - last.vertex[1]) / 120,
                double(c.vertex[2] - last.vertex[2]) / 120, double(c.splits - last.splits) / 120);
            last = c;
        }
        // NFSMW_PASS_BARRIER_ALT: the next window's level. The swap's front
        // buffer has just put a full barrier behind everything recorded; a
        // CPU present's Flush (frame dumps) only idles the GPU, so what a
        // resolve left for the vertex stages gets one here before level 0.
        if (s_passBarrierAlt && frame % 120 == 0)
        {
            s_passBarrier = (frame / 120) % 2 ? s_passBarrierLevel : 0u;
            if (!s_passBarrier && (s_vertexPendingLo < s_vertexPendingHi || !s_vertexPendingTextures.empty()))
            {
                Begin();
                FullBarrier();
            }
            fprintf(stderr, "[barrier] alt: frames %llu-%llu run with level %u\n", (unsigned long long)(frame + 1),
                (unsigned long long)(frame + 120), s_passBarrier);
        }
        // NFSMW_LOG_UNTILED_AREA=1: with each [perf] line, whether the toggle
        // was on in its window (=alt: every other one) and what it did.
        if (s_logUntiledArea && frame % 120 == 0)
        {
            static UntiledAreaCounts last;
            const UntiledAreaCounts& n = s_ua;
            auto per = [](uint64_t now, uint64_t before) { return double(now - before) / 120.0; };
            const bool on = s_untiledArea == 1 || (s_untiledArea == 2 && (frame - 1) / 120 % 2 == 1);
            fprintf(stderr, "[untiled-area] %s per frame: %.1f untiled draws cut to their viewport, %.1f whole target (%.1f clipping off, "
                "%.1f no viewport transform, %.1f read past), %.1f nothing inside; %.2f pass restarts | %.1f passes, %.2f Mpx, %.1f MB loaded+stored; "
                "untiled %.1f passes, %.2f Mpx, %.1f MB | %llu resolves read past (total)\n",
                on ? "on" : "off", per(n.cut, last.cut), per(n.whole, last.whole), per(n.clipOff, last.clipOff), per(n.noViewport, last.noViewport),
                per(n.wholeArea, last.wholeArea), per(n.empty, last.empty), per(n.restarts, last.restarts), per(n.passes, last.passes),
                per(n.passPixels, last.passPixels) / 1e6, per(n.passBytes, last.passBytes) / 1e6, per(n.untiledPasses, last.untiledPasses),
                per(n.untiledPixels, last.untiledPixels) / 1e6, per(n.untiledBytes, last.untiledBytes) / 1e6, (unsigned long long)n.resolvesPast);
            last = n;
        }
        // NFSMW_LOG_CP_CACHES=1 (or NFSMW_CP_CACHE_VERIFY): the command
        // processor's lookup caches, with each [perf] line. (A separate
        // check from NFSMW_CP_OPT_VERIFY, whose index check reads the ring
        // back: uncached VRAM on the Deck and the Steam Machine, seconds a
        // frame.)
        static const bool logCaches = std::getenv("NFSMW_LOG_CP_CACHES") != nullptr || s_textureCache->verify;
        if (logCaches && frame % 120 == 0)
        {
            const TextureCache& t = *s_textureCache;
            const ShaderCache& sh = *s_shaderCache;
            const uint64_t now[9] = { t.hits, t.misses, t.revalidated, t.stale, t.uncached, t.recentHits, sh.loads, sh.hits,
                sh.comparedBytes };
            static uint64_t last[9] = {};
            uint64_t d[9];
            for (int i = 0; i < 9; i++)
                d[i] = now[i] - last[i];
            std::copy(std::begin(now), std::end(now), last);
            auto pct = [](uint64_t part, uint64_t whole) { return whole ? 100.0 * double(part) / double(whole) : 0.0; };
            fprintf(stderr, "[cpcache] textures: %.0f lookups/frame, %.1f%% kept (%.1f%% of them checked after a write, %.1f%% by hash), "
                "%.1f%% stale, %.1f%% not keepable | shaders: %.0f loads/frame, %.1f%% found by address (%.0f KB compared) | "
                "mismatches: %llu textures, %llu shaders (%llu texture checks raced a write)\n",
                double(d[0] + d[1]) / 120, pct(d[0], d[0] + d[1]), pct(d[2], d[0]), pct(d[5], d[0]), pct(d[3], d[0] + d[1]),
                pct(d[4], d[0] + d[1]), double(d[6]) / 120, pct(d[7], d[6]), double(d[8]) / 1024 / 120,
                (unsigned long long)t.mismatches, (unsigned long long)sh.mismatches, (unsigned long long)t.raced);
        }
        s_pipelineWaitLeftUs = s_pipelineWaitUs;
        // A loading screen (or movie): wait for pipelines in full for 2 s.
        if (s_stats.draws - s_drawsAtSwap < 64)
            s_waitPipelinesUntil = frame + 120;
        s_drawsAtSwap = s_stats.draws;
        if (s_pipelineSkips)
        {
            static uint32_t logged = 0;
            if (logged++ < 300)
                fprintf(stderr, "[pipelines] frame %llu: %u draws skipped, %zu pipelines compiling\n",
                    (unsigned long long)frame, s_pipelineSkips, s_pendingPipelines.size());
            s_stats.pipelineSkips += s_pipelineSkips;
            s_pipelineSkips = 0;
        }
        // New pipelines are saved every 10 s, on a thread of its own: getting a
        // large cache's data and writing it took ~70 ms, a visible stutter
        // when it ran here (the pipeline cache is internally synchronized).
        if (s_vk && frame % 600 == 0 && s_stats.pipelines != s_pipelinesAtSave)
        {
            s_pipelinesAtSave = uint32_t(s_stats.pipelines);
            static std::atomic<bool> saving{ false };
            if (!saving.exchange(true))
                std::thread([] {
                    SetHostThreadName("nfsmw-cachesave");
                    hostcpu::LeaveReservedCore();
                    SavePipelineCache();
                    saving = false;
                }).detach();
        }
        // NFSMW_LOG_DRAWS=<seconds>: the first frame after that time.
        static const double logDrawsSec = [] { const char* v = std::getenv("NFSMW_LOG_DRAWS"); return v ? std::atof(v) : -1.0; }();
        static const auto launch = std::chrono::steady_clock::now();
        static bool loggedDraws = false;
        s_logDrawsNow = false;
        if (logDrawsSec >= 0 && !loggedDraws &&
            std::chrono::duration<double>(std::chrono::steady_clock::now() - launch).count() >= logDrawsSec)
        {
            loggedDraws = true;
            s_logDrawsNow = true;
            fprintf(stderr, "[draws] frame %llu\n", (unsigned long long)frame);
        }
        if (!s_vk || s_profileFrame < 0)
            return;
        if (frame == uint64_t(s_profileFrame))
        {
            s_profiling = true;
            s_passInfos.clear();
            s_gapLast = { s_stats.resolves, s_stats.uploads, s_stats.submits, writewatch::GetGuardStats().uploadBytes };
            s_passQueryNext = 0;
            return;
        }
        if (!s_profiling)
            return;
        s_profiling = false;
        Flush();  // presentation no longer waits: make the results ready
        // Per pass, also the GPU time since the previous pass ended: the
        // resolves, untiles, copies and barriers between them (and idle time
        // where a submission waited).
        double total = 0, between = 0;
        uint64_t lastEnd = 0;
        fprintf(stderr, "[profile] frame %d: %zu passes\n", s_profileFrame, s_passInfos.size());
        for (const PassInfo& pi : s_passInfos)
        {
            uint64_t ts[2] = {};
            vkGetQueryPoolResults(s_dev, s_passQueries, pi.query, 2, sizeof(ts), ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
            double us = ts[1] > ts[0] ? double(ts[1] - ts[0]) * s_timestampPeriod / 1000.0 : 0.0;
            double gap = lastEnd && ts[0] > lastEnd ? double(ts[0] - lastEnd) * s_timestampPeriod / 1000.0 : 0.0;
            total += us;
            between += gap;
            if (ts[1] > ts[0])
                lastEnd = ts[1];
            fprintf(stderr, "[profile] %8.1f us  %4u draws  (+%7.1f us before: %u resolves, %u uploads, %u submits, %llu KB synced)  %s\n",
                us, pi.draws, gap, pi.resolves, pi.uploads, pi.submits, (unsigned long long)pi.shadowKB, pi.desc.c_str());
        }
        fprintf(stderr, "[profile] total %.1f us in passes, %.1f us between them\n", total, between);
    }

    std::vector<uint32_t> CompileShader(const std::string& glsl, int stage, std::string& log)
    {
        static std::once_flag initialized;
        std::call_once(initialized, [] { glslang_initialize_process(); });
        glslang_stage_t stages[3] = { GLSLANG_STAGE_VERTEX, GLSLANG_STAGE_FRAGMENT, GLSLANG_STAGE_COMPUTE };
        return CompileGlsl(glsl, stages[std::clamp(stage, 0, 2)], log);
    }

    void Shutdown()
    {
        if (!s_vk)
            return;
        {
            // Let running compiles finish (their pipelines go into the
            // cache being saved); drop the queued ones.
            std::unique_lock lock(s_compileMutex);
            s_compileQueue.clear();
            s_compileCv.wait_for(lock, std::chrono::seconds(2), [] { return s_compileActive == 0; });
            s_compileStopping = true;
        }
        s_compileCv.notify_all();
        SavePipelineCache();
    }

    void HitchReport(uint64_t frame, double frameMs, uint32_t vblanks)
    {
        // A frame is late when it took NFSMW_HITCH_LATE_MS (default 3; 0 off)
        // longer than the median of the last 63, so a 20 ms frame among 16.7
        // ms ones is, unless it continues a cadence (as long as the frame
        // before, a whole number of vblanks: 30 fps in movies and parts of
        // the front end); or when it took NFSMW_HITCH_MS (default 50) or
        // more. At most 8 are logged per 120-frame [perf] window (its "late"
        // count has them all), with where the command processor's time went
        // and what the game's threads were doing meanwhile. A fixed 50 ms
        // threshold hid the 19-25 ms frames that stutter while driving.
        static const double lateMs = [] { const char* v = std::getenv("NFSMW_HITCH_LATE_MS"); return v ? std::atof(v) : 3.0; }();
        static const double threshold = [] { const char* v = std::getenv("NFSMW_HITCH_MS"); return v ? std::atof(v) : 50.0; }();
        static std::array<double, 63> recent{};
        static uint32_t recentCount = 0;
        double median = 0, previous = recentCount ? recent[(recentCount - 1) % recent.size()] : 0;
        if (recentCount >= 15)
        {
            std::array<double, 63> sorted = recent;
            uint32_t n = std::min<uint32_t>(recentCount, uint32_t(sorted.size()));
            std::nth_element(sorted.begin(), sorted.begin() + n / 2, sorted.begin() + n);
            median = sorted[n / 2];
        }
        recent[recentCount++ % recent.size()] = frameMs;

        struct Totals
        {
            uint64_t busyNs, waitNs[CP_WAIT_KINDS], draws, tileSkips, resolves, submits, snapshotBytes, gpuExecNs, gameWaitNs, gamePresentNs,
                allocations, allocationNs;
            writewatch::GuardStats guard;
            uint64_t cpuNs = 0;
            long preemptions = 0;
        };
        static Totals last{};
        Totals now{ g_cpBusyNs, {}, s_stats.draws, s_stats.tileSkips, s_stats.resolves, s_stats.submits, s_stats.snapshotBytes,
            s_gpuExecNs.load(), timeline::g_gameWaitNs.load(), timeline::g_gamePresentNs.load(), s_imageTotals.allocations,
            s_imageTotals.allocationNs, writewatch::GetGuardStats() };
        std::copy(std::begin(g_cpWaitKindNs), std::end(g_cpWaitKindNs), now.waitNs);
        // This (the command processor's) thread's CPU time, and on Linux the
        // times the scheduler took its core away: busy time the thread wasn't
        // on a core is time it was descheduled, not work.
        struct timespec cpu{};
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu);
        now.cpuNs = uint64_t(cpu.tv_sec) * 1000000000ull + uint64_t(cpu.tv_nsec);
#ifdef RUSAGE_THREAD
        struct rusage usage{};
        getrusage(RUSAGE_THREAD, &usage);
        now.preemptions = usage.ru_nivcsw;
#endif
        double gameFrameMs = double(timeline::g_gameLongestFrameNs.exchange(0)) / 1e6;
        double vblanksLong = std::round(frameMs / 16.667);
        bool cadence = std::abs(frameMs - previous) < 1 && vblanksLong >= 2 && std::abs(frameMs - vblanksLong * 16.667) < 1;
        bool late = frameMs >= threshold || (lateMs > 0 && median > 0 && frameMs > median + lateMs && !cadence);
        static uint64_t window = 0;
        static uint32_t logged = 0;
        if ((frame + 119) / 120 != window)  // the [perf] line at frame % 120 == 0 closes a window
        {
            window = (frame + 119) / 120;
            logged = 0;
        }
        if (late)
            s_stats.lateFrames++;
        if (late && logged++ < 8)
        {
            auto ms = [](uint64_t ns) { return double(ns) / 1e6; };
            double waits[CP_WAIT_KINDS], waitMs = 0;
            for (uint32_t k = 0; k < CP_WAIT_KINDS; k++)
                waitMs += waits[k] = ms(now.waitNs[k] - last.waitNs[k]);
            // The frame's time is guest time, which stood still while the
            // game was suspended: that wait is shown apart, outside the
            // frame's waits and idle time.
            waitMs -= waits[CP_WAIT_SUSPENDED];
            char suspended[48] = "";
            if (waits[CP_WAIT_SUSPENDED] > 0)
                snprintf(suspended, sizeof(suspended), ", suspended %.1f ms", waits[CP_WAIT_SUSPENDED]);
            double busyMs = ms(now.busyNs - last.busyNs);
            const writewatch::GuardStats& g = now.guard;
            const writewatch::GuardStats& lg = last.guard;
            // idle: the ring was empty (the command processor waited for the game).
            char preempted[32] = "";
#ifdef RUSAGE_THREAD
            snprintf(preempted, sizeof(preempted), ", preempted %ld times", now.preemptions - last.preemptions);
#endif
            // Textures' time includes their shadow sync; mprotect counts all
            // threads (the guard's fault handler too).
            fprintf(stderr, "[hitch] frame %llu took %.1f ms (median %.1f, %u vblanks) | command processor: busy %.1f ms, "
                "waits %.1f ms (throttle %.1f, regmem %.1f, shadow %.1f, flush %.1f, pipeline %.1f, slot %.1f)%s, idle %.1f ms, "
                "on a core %.1f ms%s | "
                "shadow sync %.1f ms (%.0f KB, %.0f KB of it new; copying %.1f ms), textures %u (%.1f ms, %.0f KB, %u scaled), images created %u (%.1f ms), "
                "image memory allocations %llu (%.1f ms), shaders %u (%.1f ms), pipelines %u (%.1f ms), submits %llu (%.1f ms), draws %llu (%llu later tiles), resolves %llu, "
                "snapshots %.0f KB | guard: %llu faults, %llu stalls (%.1f ms), %llu mprotect (%.1f ms), %.0f KB read back, "
                "%.0f KB file reads | "
                "game: longest frame %.1f ms, present %.1f ms, D3D waits %.1f ms | GPU executing %.1f ms\n",
                (unsigned long long)frame, frameMs, median, vblanks, busyMs, waitMs, waits[CP_WAIT_THROTTLE], waits[CP_WAIT_REGMEM],
                waits[CP_WAIT_SHADOW], waits[CP_WAIT_FLUSH], waits[CP_WAIT_PIPELINE], waits[CP_WAIT_SLOT], suspended,
                std::max(0.0, frameMs - busyMs - waitMs), ms(now.cpuNs - last.cpuNs), preempted, s_hitch.shadowMs, double(g.uploadBytes - lg.uploadBytes) / 1024,
                double(g.firstUploadBytes - lg.firstUploadBytes) / 1024, ms(g.uploadNs - lg.uploadNs), s_hitch.textures, s_hitch.textureMs, double(s_hitch.textureBytes) / 1024,
                s_hitch.scaledTextures, s_hitch.images, s_hitch.imageMs, (unsigned long long)(now.allocations - last.allocations),
                ms(now.allocationNs - last.allocationNs), s_hitch.shaders, s_hitch.shaderMs, s_hitch.pipelines,
                s_hitch.pipelineMs, (unsigned long long)(now.submits - last.submits), s_hitch.submitMs,
                (unsigned long long)(now.draws - last.draws), (unsigned long long)(now.tileSkips - last.tileSkips),
                (unsigned long long)(now.resolves - last.resolves), double(now.snapshotBytes - last.snapshotBytes) / 1024,
                (unsigned long long)(g.faults - lg.faults), (unsigned long long)(g.stalls - lg.stalls), double(g.stallUs - lg.stallUs) / 1000,
                (unsigned long long)(g.protects - lg.protects), ms(g.protectNs - lg.protectNs), double(g.readbackBytes - lg.readbackBytes) / 1024, double(g.hostWriteBytes - lg.hostWriteBytes) / 1024,
                gameFrameMs, ms(now.gamePresentNs - last.gamePresentNs), ms(now.gameWaitNs - last.gameWaitNs),
                ms(now.gpuExecNs - last.gpuExecNs));
        }
        last = now;
        s_hitch = {};
    }

    namespace
    {
        std::mutex s_perfMutex;
        PerfNumbers s_perfNumbers;
    }

    PerfNumbers GetPerfNumbers()
    {
        std::lock_guard lock(s_perfMutex);
        return s_perfNumbers;
    }

    void PublishPerfNumbers(const PerfNumbers& numbers)
    {
        std::lock_guard lock(s_perfMutex);
        s_perfNumbers = numbers;
    }

    MemoryStats GetMemoryStats()
    {
        MemoryStats m{};
        for (const auto& [key, rt] : s_targets)
        {
            m.targets += rt->image.bytes;
            m.targetCount++;
        }
        for (const auto& [key, t] : s_textures)
        {
            m.textures += t->image.bytes;
            m.textureCount++;
        }
        m.scaled = s_scaled.poolBytes + s_scaled.tableBytes + s_scaled.scratchBytes;
        m.front = s_front.bytes + s_frontScaled.bytes;
        auto own = [&m](const Image& image) {
            if (image.memory && !image.block)
            {
                m.own++;
                m.ownBytes += image.bytes;
            }
        };
        for (const auto& [key, rt] : s_targets)
            own(rt->image);
        for (const auto& [key, t] : s_textures)
            own(t->image);
        for (const Image* image : { &s_front, &s_frontScaled, &s_dummy[0], &s_dummy[1], &s_dummy[2], &s_dummyStorage })
            own(*image);
        for (const auto& block : s_blocks)
        {
            m.blocks++;
            m.blockBytes += block->ranges.Size();
            m.rangeBytes += block->ranges.Used();
            m.ranges += block->ranges.Ranges();
        }
        {
            std::lock_guard lock(s_spareMutex);
            for (const std::vector<Spare>& spares : s_spares)
                m.spares += uint32_t(spares.size());
            m.spareBlocks = s_imageTotals.spareBlocks;
            m.spareNs = s_imageTotals.spareNs;
            m.blocksFreed = s_imageTotals.blocksFreed;
        }
        m.spareBytes = m.spares * s_blockSize;
        m.images = s_imageTotals.images;
        m.imageNs = s_imageTotals.imageNs;
        m.allocations = s_imageTotals.allocations;
        m.allocationNs = s_imageTotals.allocationNs;
        m.fromSpare = s_imageTotals.fromSpare;
        m.blocksHere = s_imageTotals.blocksHere;
        if (s_vk && s_memoryBudget)
        {
            VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT };
            VkPhysicalDeviceMemoryProperties2 props{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &budget };
            vkGetPhysicalDeviceMemoryProperties2(s_vk->physical, &props);
            for (uint32_t h = 0; h < props.memoryProperties.memoryHeapCount; h++)
                if (props.memoryProperties.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                {
                    m.deviceUsed += budget.heapUsage[h];
                    m.deviceBudget += budget.heapBudget[h];
                }
        }
        return m;
    }

    ScaledStats GetScaledStats()
    {
        return { s_scaled.loads, s_scaled.untiledBytes, s_scaled.resolvedBytes, s_scaled.allocations, s_scaled.evictions,
            s_scaled.fallbacks };
    }

    Stats GetStats()
    {
        s_stats.shaderCacheHits = s_shaderCacheHits.load(std::memory_order_relaxed);
        s_stats.gpuBusyUs = s_gpuBusyUs.load();
        s_stats.gpuExecUs = s_gpuExecNs.load() / 1000;
        return s_stats;
    }
}
