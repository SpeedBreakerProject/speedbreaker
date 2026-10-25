// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The PCIe link a discrete GPU sits on, for the logs and bug reports. On a
// GPU without resizable BAR, guest memory and the upload ring stay in system
// memory (gpu/memory_plan.h) and the GPU reads every vertex, constant and
// texture load, and writes every resolve, across this link. Issue #40: an
// RTX 5060 Ti (a PCIe x8 card) in a Ryzen 5600G system (PCIe 3.0 only),
// 246 MB BAR window, took 20-25 ms of GPU time a frame at 1x where a Steam
// Machine (resizable BAR: the copy in VRAM) takes 3-4; its report could not
// say what the link was. Linux reads it from sysfs (VK_EXT_pci_bus_info gives
// the address); elsewhere, or when sysfs has nothing, the link is unknown.
// Vulkan-free and header only: tests/pcie_link_test.cpp checks it against a
// sysfs-shaped folder.
#pragma once
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>

namespace gpu::pcie
{
    struct Link
    {
        double gts = 0;        // current speed, GT/s (0: unknown); a GPU at rest drops it to save power
        int width = 0;         // current lanes (0: unknown)
        double maxGts = 0;     // the GPU's best
        int maxWidth = 0;
        double slotGts = 0;    // the port above it (the slot, or a bridge): its best
        int slotWidth = 0;
        bool Known() const { return maxGts > 0 && maxWidth > 0; }
        // What the link can run at under load: the slower of the GPU and its slot.
        double BestGts() const { return slotGts > 0 && slotGts < maxGts ? slotGts : maxGts; }
        int BestWidth() const { return slotWidth > 0 && slotWidth < maxWidth ? slotWidth : maxWidth; }
    };

    // "8.0 GT/s PCIe" (or "8 GT/s", "2.5 GT/s"): 8.0. 0 when it isn't one.
    inline double ParseSpeed(const std::string& s)
    {
        double v = 0;
        if (std::sscanf(s.c_str(), "%lf", &v) != 1 || v <= 0 || v > 1000 || s.find("GT/s") == std::string::npos)
            return 0;
        return v;
    }

    // "8", "16" (lanes); 0 when it isn't one. ("x8" too, for safety.)
    inline int ParseWidth(const std::string& s)
    {
        int v = 0;
        const char* p = s.c_str();
        while (*p == ' ' || *p == 'x' || *p == 'X')
            p++;
        if (std::sscanf(p, "%d", &v) != 1 || v <= 0 || v > 64)
            return 0;
        return v;
    }

    // The generation for a speed: 2.5 GT/s is PCIe 1.0 ... 64 GT/s is 6.0.
    inline int Generation(double gts)
    {
        static const double kSpeeds[] = { 2.5, 5, 8, 16, 32, 64 };
        for (int i = 0; i < 6; i++)
            if (gts > kSpeeds[i] * 0.9 && gts < kSpeeds[i] * 1.1)
                return i + 1;
        return 0;
    }

    inline std::string Name(double gts, int width)
    {
        char s[48];
        if (int gen = Generation(gts))
            std::snprintf(s, sizeof(s), "PCIe %d.0 x%d", gen, width);
        else
            std::snprintf(s, sizeof(s), "PCIe %.1f GT/s x%d", gts, width);
        return s;
    }

    // "PCIe 3.0 x8 at most (the GPU PCIe 5.0 x8, its slot PCIe 3.0 x16; now
    // PCIe 1.0 x8)": the best the link can run at, what limits it, and what it
    // reads now. Empty when unknown.
    inline std::string Describe(const Link& l)
    {
        if (!l.Known())
            return {};
        std::string s = Name(l.BestGts(), l.BestWidth()) + " at most (the GPU " + Name(l.maxGts, l.maxWidth);
        if (l.slotGts > 0 && l.slotWidth > 0)
            s += ", its slot " + Name(l.slotGts, l.slotWidth);
        if (l.gts > 0 && l.width > 0)
            s += "; now " + Name(l.gts, l.width);
        return s + ")";
    }

    // The sysfs folder of a PCI device: /sys/bus/pci/devices/0000:01:00.0.
    inline std::string SysfsDir(uint32_t domain, uint32_t bus, uint32_t device, uint32_t function, const char* root = "/sys/bus/pci/devices")
    {
        char s[64];
        std::snprintf(s, sizeof(s), "/%04x:%02x:%02x.%x", domain & 0xFFFF, bus & 0xFF, device & 0x1F, function & 0x7);
        return root + std::string(s);
    }

    inline std::string ReadLine(const std::string& path)
    {
        std::ifstream f(path);
        std::string s;
        if (f)
            std::getline(f, s);
        return s;
    }

    // The link from a device's sysfs folder (zeros where it can't be read).
    // `dir`/.. is the port above it: the kernel resolves ".." after following
    // /sys/bus/pci/devices' link into /sys/devices.
    inline Link Read(const std::string& dir)
    {
        Link l;
        l.gts = ParseSpeed(ReadLine(dir + "/current_link_speed"));
        l.width = ParseWidth(ReadLine(dir + "/current_link_width"));
        l.maxGts = ParseSpeed(ReadLine(dir + "/max_link_speed"));
        l.maxWidth = ParseWidth(ReadLine(dir + "/max_link_width"));
        l.slotGts = ParseSpeed(ReadLine(dir + "/../max_link_speed"));
        l.slotWidth = ParseWidth(ReadLine(dir + "/../max_link_width"));
        return l;
    }
}
