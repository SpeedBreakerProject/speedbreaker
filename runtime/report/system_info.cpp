// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See report.h.
//
// The system summary at the top of every log and in every report: enough to
// tell a tester's machine from another without asking them anything. The
// host facts are read here; the GPU, the display and the game install come
// from the modules that know them (SetSystemItem).
#include <stdafx.h>
#include "report.h"
#include "internal.h"

#include <user/paths.h>
#include <user/settings.h>

#include <fstream>
#include <set>
#include <sstream>

#ifdef _WIN32
#include <intrin.h>
#else
#include <sys/utsname.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

#ifndef _WIN32
extern char** environ;
#endif

namespace report
{
    namespace
    {
        std::mutex s_mutex;
        std::vector<std::pair<std::string, std::string>> s_items;  // in the order first set

        // The summary as last logged, for the crash handler (which can't build it).
        char s_logged[24 * 1024];
        std::atomic<bool> s_loggedReady{ false };

        std::string ReadFile(const char* path, size_t limit = 1 << 20)
        {
            std::ifstream in(path, std::ios::binary);
            std::string text;
            if (!in)
                return text;
            text.resize(limit);
            in.read(text.data(), std::streamsize(limit));
            text.resize(size_t(in.gcount()));
            return text;
        }

        std::string Trim(std::string s)
        {
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t'))
                s.pop_back();
            size_t start = s.find_first_not_of(" \t");
            return start == std::string::npos ? std::string() : s.substr(start);
        }

        // KEY=value or KEY="value" from an os-release file.
        std::string OsReleaseValue(const std::string& text, const char* key)
        {
            std::istringstream in(text);
            std::string line;
            std::string prefix = std::string(key) + "=";
            while (std::getline(in, line))
                if (line.starts_with(prefix))
                {
                    std::string v = Trim(line.substr(prefix.size()));
                    if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front())
                        v = v.substr(1, v.size() - 2);
                    return v;
                }
            return {};
        }

        std::string OsReleaseName(const std::string& text)
        {
            std::string name = OsReleaseValue(text, "PRETTY_NAME");
            if (name.empty())
                name = OsReleaseValue(text, "NAME");
            std::string version = OsReleaseValue(text, "VERSION_ID");
            std::string build = OsReleaseValue(text, "BUILD_ID");
            if (!version.empty() && name.find(version) == std::string::npos)
                name += " " + version;
            if (!build.empty())
                name += " (build " + build + ")";
            return name;
        }

#ifdef __APPLE__
        std::string Sysctl(const char* name)
        {
            char value[256];
            size_t size = sizeof(value);
            if (sysctlbyname(name, value, &size, nullptr, 0) != 0 || size == 0)
                return {};
            return Trim(std::string(value, strnlen(value, size)));
        }

        int64_t SysctlInt(const char* name)
        {
            int64_t value = 0;
            size_t size = sizeof(value);
            if (sysctlbyname(name, &value, &size, nullptr, 0) != 0)
                return -1;
            if (size == sizeof(int32_t))
            {
                int32_t v32;
                memcpy(&v32, &value, sizeof(v32));
                return v32;
            }
            return value;
        }
#endif

#ifdef _WIN32
        std::string Utf8(const wchar_t* wide)
        {
            int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
            if (n <= 1)
                return {};
            std::string out(size_t(n - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), n, nullptr, nullptr);
            return out;
        }

        std::string RegistryString(const wchar_t* key, const wchar_t* value)
        {
            wchar_t buffer[256];
            DWORD size = sizeof(buffer);
            if (RegGetValueW(HKEY_LOCAL_MACHINE, key, value, RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS)
                return {};
            return Trim(Utf8(buffer));
        }

        DWORD RegistryNumber(const wchar_t* key, const wchar_t* value)
        {
            DWORD number = 0, size = sizeof(number);
            if (RegGetValueW(HKEY_LOCAL_MACHINE, key, value, RRF_RT_REG_DWORD, nullptr, &number, &size) != ERROR_SUCCESS)
                return 0;
            return number;
        }

        // Windows 10 and 11 both report 10.0: the build tells them apart.
        std::string OsText()
        {
            RTL_OSVERSIONINFOW version{};
            version.dwOSVersionInfoSize = sizeof(version);
            using RtlGetVersionFn = LONG(WINAPI*)(RTL_OSVERSIONINFOW*);
            if (auto get = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")))
                get(&version);
            const wchar_t* key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
            std::string display = RegistryString(key, L"DisplayVersion");
            std::string edition = RegistryString(key, L"EditionID");
            return std::format("Windows {}{}{}{} (build {}.{}), x64", version.dwBuildNumber >= 22000 ? 11 : 10,
                edition.empty() ? "" : " ", edition, display.empty() ? "" : " " + display, version.dwBuildNumber,
                RegistryNumber(key, L"UBR"));
        }

        std::string MachineText()
        {
            // Lenovo keeps the model's name in SystemVersion ("ThinkPad T14
            // Gen 1") and its type number in SystemProductName.
            const wchar_t* key = L"HARDWARE\\DESCRIPTION\\System\\BIOS";
            std::string text = Trim(RegistryString(key, L"SystemManufacturer") + " " + RegistryString(key, L"SystemProductName"));
            std::string version = RegistryString(key, L"SystemVersion");
            if (!version.empty() && version != "None" && text.find(version) == std::string::npos)
                text += " (" + version + ")";
            return text;
        }

        std::string CpuText()
        {
            int r[4];
            char brand[49] = "";
            __cpuid(r, int(0x80000000));
            if (unsigned(r[0]) >= 0x80000004u)
                for (int i = 0; i < 3; i++)
                {
                    __cpuid(r, int(0x80000002 + i));
                    memcpy(brand + 16 * i, r, 16);
                }
            DWORD bytes = 0;
            GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes);
            std::vector<uint8_t> buffer(bytes);
            size_t cores = 0;
            if (bytes && GetLogicalProcessorInformationEx(RelationProcessorCore,
                    reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data()), &bytes))
                for (DWORD at = 0; at < bytes;)
                {
                    auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + at);
                    cores++;
                    at += info->Size;
                }
            std::string model = Trim(brand);
            return std::format("{}, {} cores / {} threads", model.empty() ? "unknown CPU" : model, cores, std::thread::hardware_concurrency());
        }

        std::string MemoryText()
        {
            constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
            MEMORYSTATUSEX m{};
            m.dwLength = sizeof(m);
            GlobalMemoryStatusEx(&m);
            return std::format("{:.1f} GB ({:.1f} GB available at startup), commit limit {:.1f} GB", double(m.ullTotalPhys) / kGiB,
                double(m.ullAvailPhys) / kGiB, double(m.ullTotalPageFile) / kGiB);
        }

        // Mains or battery, and Windows' power mode (Settings > System >
        // Power): on the ThinkPad, Best performance ran a race 25% faster
        // than Balanced, on the same settings.
        std::string PowerText()
        {
            std::string text = "power source unknown";
            SYSTEM_POWER_STATUS status{};
            if (GetSystemPowerStatus(&status))
            {
                text = status.ACLineStatus == 1 ? "on AC power" : status.ACLineStatus == 0 ? "on battery" : "power source unknown";
                if (status.BatteryFlag != 128 && status.BatteryFlag != 255 && status.BatteryLifePercent <= 100)
                    text += std::format(", battery {}%", status.BatteryLifePercent);
                if (status.SystemStatusFlag == 1)
                    text += ", battery saver on";
            }
            // powrprof's PowerGetEffectiveOverlayScheme, looked up so that a
            // Windows without it still starts.
            using OverlayFn = DWORD(WINAPI*)(GUID*);
            if (HMODULE powrprof = LoadLibraryExW(L"powrprof.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
            {
                GUID overlay{};
                auto get = reinterpret_cast<OverlayFn>(GetProcAddress(powrprof, "PowerGetEffectiveOverlayScheme"));
                if (get && get(&overlay) == ERROR_SUCCESS)
                {
                    std::string id = std::format("{:08x}-{:04x}-{:04x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}", overlay.Data1,
                        overlay.Data2, overlay.Data3, overlay.Data4[0], overlay.Data4[1], overlay.Data4[2], overlay.Data4[3],
                        overlay.Data4[4], overlay.Data4[5], overlay.Data4[6], overlay.Data4[7]);
                    const char* mode = id == "00000000-0000-0000-0000-000000000000"   ? "Balanced"
                                       : id == "ded574b5-45a0-4f42-8737-46345c09c238" ? "Best performance"
                                       : id == "961cc777-2547-4f9d-8174-7d86181b8a7a" ? "Best power efficiency"
                                                                                        : nullptr;
                    text += mode ? std::format("; power mode {}", mode) : std::format("; power mode overlay {}", id);
                }
                FreeLibrary(powrprof);
            }
            return text;
        }
#else
        std::string OsText()
        {
            struct utsname u{};
            uname(&u);
            std::string kernel = std::format("{} {} {}", u.sysname, u.release, u.machine);
#if defined(__APPLE__) && TARGET_OS_IOS
            return std::format("{} {} ({}), {}", Sysctl("hw.machine").starts_with("iPad") ? "iPadOS" : "iOS",
                Sysctl("kern.osproductversion"), Sysctl("kern.osversion"), kernel);
#elif defined(__APPLE__)
            return std::format("macOS {} ({}), {}", Sysctl("kern.osproductversion"), Sysctl("kern.osversion"), kernel);
#else
            std::string os = OsReleaseName(ReadFile("/etc/os-release"));
            // A distrobox/toolbox container shows the host's root at /run/host:
            // the host (SteamOS) is what the tester knows, the container what
            // the game was built in.
            std::string host = OsReleaseName(ReadFile("/run/host/etc/os-release"));
            if (!host.empty() && host != os)
                return std::format("{}, {}; in a container: {}", host, kernel, os.empty() ? "Linux" : os);
            return std::format("{}, {}", os.empty() ? "Linux" : os, kernel);
#endif
        }

        std::string MachineText()
        {
#if defined(__APPLE__) && TARGET_OS_IOS
            return Sysctl("hw.machine");  // e.g. iPad14,6 (hw.model is the board name)
#elif defined(__APPLE__)
            return Sysctl("hw.model");
#else
            std::string vendor = Trim(ReadFile("/sys/class/dmi/id/sys_vendor", 256));
            std::string product = Trim(ReadFile("/sys/class/dmi/id/product_name", 256));
            std::string text = Trim(vendor + " " + product);
            // Valve's code names.
            if (vendor == "Valve" && product == "Jupiter")
                text += " (Steam Deck LCD)";
            else if (vendor == "Valve" && product == "Galileo")
                text += " (Steam Deck OLED)";
            else if (vendor == "Valve" && product == "Fremont")
                text += " (Steam Machine)";
            return text;
#endif
        }

        std::string CpuText()
        {
            unsigned logical = std::thread::hardware_concurrency();
#ifdef __APPLE__
            std::string model = Sysctl("machdep.cpu.brand_string");
            int64_t perf = SysctlInt("hw.perflevel0.physicalcpu"), eff = SysctlInt("hw.perflevel1.physicalcpu");
            if (perf > 0 && eff > 0)
                return std::format("{}, {} performance + {} efficiency cores", model, perf, eff);
            return std::format("{}, {} cores / {} threads", model, SysctlInt("hw.physicalcpu"), logical);
#else
            std::string info = ReadFile("/proc/cpuinfo", 4 << 20);
            std::string model;
            std::set<std::pair<std::string, std::string>> cores;
            std::string physical;
            std::istringstream in(info);
            std::string line;
            while (std::getline(in, line))
            {
                size_t colon = line.find(':');
                if (colon == std::string::npos)
                    continue;
                std::string key = Trim(line.substr(0, colon)), value = Trim(line.substr(colon + 1));
                if (key == "model name" && model.empty())
                    model = value;
                else if (key == "physical id")
                    physical = value;
                else if (key == "core id")
                    cores.insert({ physical, value });
            }
            if (cores.empty())
                return std::format("{}, {} threads", model.empty() ? "unknown CPU" : model, logical);
            return std::format("{}, {} cores / {} threads", model.empty() ? "unknown CPU" : model, cores.size(), logical);
#endif
        }

        std::string MemoryText()
        {
            constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
#ifdef __APPLE__
            return std::format("{:.1f} GB", double(SysctlInt("hw.memsize")) / kGiB);
#else
            std::string info = ReadFile("/proc/meminfo");
            auto field = [&](const char* key) -> double {
                size_t at = info.find(std::string(key) + ":");
                return at == std::string::npos ? -1.0 : std::atof(info.c_str() + at + strlen(key) + 1) * 1024.0;
            };
            double total = field("MemTotal"), available = field("MemAvailable"), swap = field("SwapTotal");
            return std::format("{:.1f} GB ({:.1f} GB available at startup), swap {:.1f} GB", total / kGiB, available / kGiB,
                std::max(swap, 0.0) / kGiB);
#endif
        }
#endif

        // How it was launched: Game Mode, from Steam, in a container.
        std::string SessionText()
        {
            std::vector<std::string> parts;
            auto env = [](const char* name) -> std::string {
                const char* v = std::getenv(name);
                return v ? v : "";
            };
            // What the environment says; whether the game went with Game
            // Mode (NFSMW_GAME_MODE can say otherwise) is on the Display line.
            if (!env("GAMESCOPE_WAYLAND_DISPLAY").empty())
                parts.push_back("under gamescope (GAMESCOPE_WAYLAND_DISPLAY)");
            if (!env("XDG_CURRENT_DESKTOP").empty())
                parts.push_back("XDG_CURRENT_DESKTOP=" + env("XDG_CURRENT_DESKTOP"));
            if (!env("XDG_SESSION_TYPE").empty())
                parts.push_back(env("XDG_SESSION_TYPE") + " session");
            if (!env("SteamGameId").empty() || !env("SteamAppId").empty())
                parts.push_back("launched from Steam");
            if (env("SteamDeck") == "1")
                parts.push_back("SteamDeck=1");
            if (!env("CONTAINER_ID").empty())
                parts.push_back("container " + env("CONTAINER_ID"));
#ifndef _WIN32
            else if (access("/run/.containerenv", F_OK) == 0 || access("/.dockerenv", F_OK) == 0)
                parts.push_back("in a container");
#else
            DWORD sessionId = 0;
            if (ProcessIdToSessionId(GetCurrentProcessId(), &sessionId) && sessionId == 0)
                parts.push_back("session 0 (no desktop: a service or SSH)");
            if (GetSystemMetrics(SM_REMOTESESSION))
                parts.push_back("Remote Desktop");
#endif
            if (!env("NFSMW_LIFELINE_PID").empty())
                parts.push_back("scripts/steamos/speedbreaker.sh");
            std::string text;
            for (const std::string& p : parts)
                text += (text.empty() ? "" : "; ") + p;
            return text.empty() ? "desktop" : text;
        }

        std::string EnvironmentText()
        {
            std::vector<std::string> vars;
#ifdef _WIN32
            if (wchar_t* block = GetEnvironmentStringsW())
            {
                for (const wchar_t* e = block; *e; e += wcslen(e) + 1)
                    if (wcsncmp(e, L"NFSMW_", 6) == 0)
                        vars.push_back(Utf8(e));
                FreeEnvironmentStringsW(block);
            }
#else
            for (char** e = environ; e && *e; e++)
                if (strncmp(*e, "NFSMW_", 6) == 0)
                    vars.push_back(*e);
#endif
            std::sort(vars.begin(), vars.end());
            std::string text;
            for (const std::string& v : vars)
                text += (text.empty() ? "" : " ") + v;
            return text.empty() ? "(no NFSMW_* variables)" : text;
        }

        std::string SettingsText()
        {
            std::string text = std::format("Settings ({}):\n", settings::DefaultPath().string());
            for (const settings::Option& o : settings::Options())
            {
                text += std::format("  {}.{} = {}", o.table, o.key, settings::FormatValue(o.id, settings::GetValue(o.id)));
                if (const char* env = settings::EnvOverride(o.id))
                    text += std::format("  (from {}; saved: {})", env, settings::FormatValue(o.id, settings::GetStoredValue(o.id)));
                else if (settings::RestartPending(o.id))
                    text += std::format("  (saved: {}, from the next launch)", settings::FormatValue(o.id, settings::GetStoredValue(o.id)));
                text += '\n';
            }
            std::string error = settings::LastError();
            if (!error.empty())
                text += "  settings.toml: " + error + "\n";
            return text;
        }
    }

    void SetSystemItem(const char* key, std::string value)
    {
        std::lock_guard lock(s_mutex);
        for (auto& item : s_items)
            if (item.first == key)
            {
                item.second = std::move(value);
                return;
            }
        s_items.emplace_back(key, std::move(value));
    }

    std::string SystemSummary()
    {
        std::string text;
        text += "Build: SpeedBreaker " + std::string(BuildString()) + "\n";
        text += "OS: " + OsText() + "\n";
        if (std::string machine = MachineText(); !machine.empty())
            text += "Machine: " + machine + "\n";
        text += "CPU: " + CpuText() + "\n";
        text += "RAM: " + MemoryText() + "\n";
#ifdef _WIN32
        text += "Power: " + PowerText() + "\n";
#endif
        {
            std::lock_guard lock(s_mutex);
            for (const auto& [key, value] : s_items)
                text += key + ": " + value + "\n";
        }
        text += "Session: " + SessionText() + "\n";
        text += "Environment: " + EnvironmentText() + "\n";
        text += "Data: " + GetUserPath().string() + " (logs in logs/" + (portable::Current().Active() ? "; portable mode" : "")
            + "), caches: " + GetCachePath().string() + "\n";
        text += SettingsText();
        return text;
    }

    void LogSystemSummary()
    {
        std::string text = SystemSummary();
        std::istringstream in(text);
        std::string line;
        std::string logged;
        while (std::getline(in, line))
            logged += "[system] " + line + "\n";
        fputs(logged.c_str(), stderr);
        if (!s_loggedReady.load(std::memory_order_acquire))
        {
            snprintf(s_logged, sizeof(s_logged), "%s", text.c_str());
            s_loggedReady.store(true, std::memory_order_release);
        }
    }

    namespace detail
    {
        const char* LoggedSystemSummary()
        {
            return s_loggedReady.load(std::memory_order_acquire) ? s_logged : "";
        }
    }
}
