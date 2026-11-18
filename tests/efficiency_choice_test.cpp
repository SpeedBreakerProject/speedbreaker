// Tests for efficiency::Choose (runtime/cpu/efficiency_choice.h): which of item #71's waiting changes a run starts
// with. Without NFSMW_EFFICIENCY: all four on Apple; cpspin and lateguard on a Steam Deck (the DMI names
// video/steam_device.h reads), gpuwait and sleep0 off there; cpspin, sleep0 and lateguard on the Steam Machine (Valve
// "Fremont"), gpuwait off there; none on the Steam Frame, other Linux and the rest. With it: exactly what it names, on
// any machine ("0" none, "1"/"all" all). Then cpu/efficiency.cpp (the path in argv[1]) is read to check it takes its
// start state from Choose, the Deck from ThisSteamDevice and the Steam Machine from its DMI names. From the repo root:
//   clang++ -std=c++20 -O2 -Iruntime tests/efficiency_choice_test.cpp -o build/efficiency_choice_test
//   build/efficiency_choice_test runtime/cpu/efficiency.cpp
#include <cpu/efficiency_choice.h>
#include <video/steam_device.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using namespace efficiency;

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

// "cpspin+lateguard" style text of a choice, for messages and comparisons.
static std::string Text(const bool on[kChoiceCount])
{
    std::string t;
    for (int c = 0; c < kChoiceCount; c++)
        if (on[c])
            t += std::string(t.empty() ? "" : "+") + kChoiceNames[c];
    return t.empty() ? "none" : t;
}

static std::string Chosen(const char* setting, Machine m)
{
    bool on[kChoiceCount];
    for (bool& b : on)
        b = true;  // Choose must set every entry
    Choose(setting, m, on);
    return Text(on);
}

// The machine as cpu/efficiency.cpp decides it on Linux, from DMI names.
static Machine LinuxMachine(bool arm64, const char* vendor, const char* product)
{
    if (video::SteamDeviceFrom(arm64, vendor, product) == video::SteamDevice::Deck)
        return Machine::SteamDeck;
    return video::IsSteamMachineDmi(vendor, product) ? Machine::SteamMachine : Machine::Other;
}

static void Defaults()
{
    CHECK(Chosen(nullptr, Machine::Apple) == "cpspin+gpuwait+sleep0+lateguard", "%s", Chosen(nullptr, Machine::Apple).c_str());
    CHECK(Chosen(nullptr, Machine::SteamDeck) == "cpspin+lateguard", "%s", Chosen(nullptr, Machine::SteamDeck).c_str());
    CHECK(Chosen(nullptr, Machine::SteamMachine) == "cpspin+sleep0+lateguard", "%s", Chosen(nullptr, Machine::SteamMachine).c_str());
    CHECK(Chosen(nullptr, Machine::Other) == "none", "%s", Chosen(nullptr, Machine::Other).c_str());
    // The machines by their DMI names (the Steam Machine and the Deck as read on them, 2026-11-15).
    CHECK(Chosen(nullptr, LinuxMachine(false, "Valve", "Galileo")) == "cpspin+lateguard", "Deck OLED");
    CHECK(Chosen(nullptr, LinuxMachine(false, "Valve", "Jupiter")) == "cpspin+lateguard", "Deck LCD");
    CHECK(Chosen(nullptr, LinuxMachine(false, "Valve", "Fremont")) == "cpspin+sleep0+lateguard", "Steam Machine");
    CHECK(Chosen(nullptr, LinuxMachine(false, "Valve", "Fremont\n")) == "none", "the names as read, newline cut first");
    CHECK(Chosen(nullptr, LinuxMachine(false, "valve", "Fremont")) == "none", "the vendor exactly");
    CHECK(video::SteamDeviceFrom(false, "Valve", "Fremont") == video::SteamDevice::Other, "the Steam Machine stays SteamDevice::Other (the renderer's rules)");
    CHECK(Chosen(nullptr, LinuxMachine(true, "", "")) == "none", "Steam Frame (arm64 Linux)");
    CHECK(Chosen(nullptr, LinuxMachine(true, "Valve", "Galileo")) == "none", "arm64 Linux is the Frame whatever its names");
    CHECK(Chosen(nullptr, LinuxMachine(false, "LENOVO", "20QV")) == "none", "another Linux PC");
    CHECK(Chosen(nullptr, LinuxMachine(false, "", "")) == "none", "no DMI to read");
}

static void Overrides()
{
    const Machine all[] = { Machine::Apple, Machine::SteamDeck, Machine::SteamMachine, Machine::Other };
    for (Machine m : all)
    {
        CHECK(Chosen("0", m) == "none", "0 on machine %d: %s", int(m), Chosen("0", m).c_str());
        CHECK(Chosen("1", m) == "cpspin+gpuwait+sleep0+lateguard", "1 on machine %d", int(m));
        CHECK(Chosen("all", m) == "cpspin+gpuwait+sleep0+lateguard", "all on machine %d", int(m));
        CHECK(Chosen("gpuwait", m) == "gpuwait", "gpuwait on machine %d: %s", int(m), Chosen("gpuwait", m).c_str());
        CHECK(Chosen("sleep0,cpspin", m) == "cpspin+sleep0", "sleep0,cpspin on machine %d", int(m));
        CHECK(Chosen("cpspin,gpuwait,sleep0,lateguard", m) == "cpspin+gpuwait+sleep0+lateguard", "the four named on machine %d", int(m));
        CHECK(Chosen("", m) == "none", "set but empty on machine %d", int(m));
        CHECK(Chosen("cpspinx,gpu", m) == "none", "no exact name on machine %d", int(m));
    }
}

// cpu/efficiency.cpp uses this choice, with the Deck and the Steam Machine from video/steam_device.h.
static void Source(const char* path)
{
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string s = ss.str();
    CHECK(!s.empty(), "could not read %s", path);
    CHECK(s.find("Choose(std::getenv(\"NFSMW_EFFICIENCY\"), ThisMachine(), on);") != std::string::npos, "Init starts from Choose");
    CHECK(s.find("if (video::ThisSteamDevice() == video::SteamDevice::Deck)\n                return Machine::SteamDeck;") != std::string::npos,
        "the Deck by ThisSteamDevice");
    CHECK(s.find("video::IsSteamMachineDmi(video::ReadDmiLine(\"/sys/class/dmi/id/sys_vendor\"), video::ReadDmiLine(\"/sys/class/dmi/id/product_name\"))\n"
                 "                ? Machine::SteamMachine : Machine::Other;") != std::string::npos,
        "the Steam Machine by its DMI names");
    CHECK(s.find("#if defined(__APPLE__)\n            return Machine::Apple;") != std::string::npos, "Apple first");
    CHECK(s.find("#elif defined(__linux__) && !defined(__ANDROID__)") != std::string::npos, "Linux but not Android");
}

int main(int argc, char** argv)
{
    Defaults();
    Overrides();
    if (argc > 1)
        Source(argv[1]);
    else
    {
        g_failures++;
        printf("  FAIL: give the path of runtime/cpu/efficiency.cpp\n");
    }
    if (g_failures)
    {
        printf("%d FAILED\n", g_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
