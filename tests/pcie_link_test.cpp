// Tests for gpu::pcie (runtime/gpu/pcie_link.h): a discrete GPU's PCIe link
// read from a sysfs-shaped folder, and how the logs name it. From the repo root:
//   clang++ -std=c++20 -O2 -Iruntime tests/pcie_link_test.cpp -o build/pcie_link_test
//   build/pcie_link_test
// Issue #40's shape: an RTX 5060 Ti (PCIe 5.0 x8) in a Ryzen 5600G's PCIe
// 3.0 x16 slot, idling at 2.5 GT/s.
#include <gpu/pcie_link.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace gpu::pcie;

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void Put(const std::filesystem::path& p, const char* text)
{
    std::ofstream(p) << text;
}

static void Parsing()
{
    CHECK(ParseSpeed("8.0 GT/s PCIe") == 8.0, "8.0 GT/s PCIe");
    CHECK(ParseSpeed("2.5 GT/s PCIe") == 2.5, "2.5");
    CHECK(ParseSpeed("32.0 GT/s PCIe") == 32.0, "32");
    CHECK(ParseSpeed("8 GT/s") == 8.0, "older kernels' form");
    CHECK(ParseSpeed("Unknown") == 0 && ParseSpeed("") == 0 && ParseSpeed("8") == 0, "not a speed");
    CHECK(ParseWidth("8") == 8 && ParseWidth("16\n") == 16 && ParseWidth("x4") == 4, "widths");
    CHECK(ParseWidth("") == 0 && ParseWidth("0") == 0 && ParseWidth("255") == 0, "not a width");
    CHECK(Generation(2.5) == 1 && Generation(5) == 2 && Generation(8) == 3 && Generation(16) == 4 && Generation(32) == 5 &&
        Generation(64) == 6 && Generation(12) == 0, "generations");
    CHECK(Name(8, 8) == "PCIe 3.0 x8", "%s", Name(8, 8).c_str());
    CHECK(Name(12, 4) == "PCIe 12.0 GT/s x4", "%s", Name(12, 4).c_str());
    CHECK(SysfsDir(0, 1, 0, 0) == "/sys/bus/pci/devices/0000:01:00.0", "%s", SysfsDir(0, 1, 0, 0).c_str());
    CHECK(SysfsDir(0x10, 0xc1, 0x1f, 7, "/x") == "/x/0010:c1:1f.7", "%s", SysfsDir(0x10, 0xc1, 0x1f, 7, "/x").c_str());
}

static void Folder(const std::filesystem::path& root)
{
    // The slot (root port) and the GPU under it, as /sys/devices lays them out.
    // (Windows, where the test builds too: ':' can't be in a folder name, and
    // the reader doesn't look at the names.)
#ifdef _WIN32
    std::filesystem::path port = root / "0000-00-01.1", gpu = port / "0000-01-00.0";
#else
    std::filesystem::path port = root / "0000:00:01.1", gpu = port / "0000:01:00.0";
#endif
    std::filesystem::create_directories(gpu);
    Put(port / "max_link_speed", "8.0 GT/s PCIe\n");
    Put(port / "max_link_width", "16\n");
    Put(gpu / "max_link_speed", "32.0 GT/s PCIe\n");
    Put(gpu / "max_link_width", "8\n");
    Put(gpu / "current_link_speed", "2.5 GT/s PCIe\n");
    Put(gpu / "current_link_width", "8\n");
    Link l = Read(gpu.string());
    CHECK(l.Known() && l.BestGts() == 8.0 && l.BestWidth() == 8, "best %.1f x%d", l.BestGts(), l.BestWidth());
    std::string d = Describe(l);
    printf("issue #40's shape: %s\n", d.c_str());
    CHECK(d == "PCIe 3.0 x8 at most (the GPU PCIe 5.0 x8, its slot PCIe 3.0 x16; now PCIe 1.0 x8)", "%s", d.c_str());

    // A GPU at its best in a PCIe 4.0 x16 slot.
    Put(port / "max_link_speed", "16.0 GT/s PCIe\n");
    Put(gpu / "max_link_speed", "16.0 GT/s PCIe\n");
    Put(gpu / "max_link_width", "16\n");
    Put(gpu / "current_link_speed", "16.0 GT/s PCIe\n");
    Put(gpu / "current_link_width", "16\n");
    d = Describe(Read(gpu.string()));
    CHECK(d == "PCIe 4.0 x16 at most (the GPU PCIe 4.0 x16, its slot PCIe 4.0 x16; now PCIe 4.0 x16)", "%s", d.c_str());

    // No port files (a virtual bus), no current link: what is known.
    std::filesystem::remove(port / "max_link_speed");
    std::filesystem::remove(gpu / "current_link_speed");
    d = Describe(Read(gpu.string()));
    CHECK(d == "PCIe 4.0 x16 at most (the GPU PCIe 4.0 x16)", "%s", d.c_str());

    // Nothing there (not Linux, or no such device): empty, nothing logged.
    CHECK(Describe(Read((root / "missing").string())).empty(), "missing folder described");
}

int main()
{
    Parsing();
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("pcie_link_test_" + std::to_string(std::rand()) + std::to_string(uintptr_t(&g_failures) & 0xFFFF));
    std::filesystem::remove_all(root);
    Folder(root);
    std::filesystem::remove_all(root);
    if (g_failures)
    {
        printf("%d FAILED\n", g_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
