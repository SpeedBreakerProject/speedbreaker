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

#include <sys/utsname.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

extern char** environ;

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
            else if (access("/run/.containerenv", F_OK) == 0 || access("/.dockerenv", F_OK) == 0)
                parts.push_back("in a container");
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
            for (char** e = environ; e && *e; e++)
                if (strncmp(*e, "NFSMW_", 6) == 0)
                    vars.push_back(*e);
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
        {
            std::lock_guard lock(s_mutex);
            for (const auto& [key, value] : s_items)
                text += key + ": " + value + "\n";
        }
        text += "Session: " + SessionText() + "\n";
        text += "Environment: " + EnvironmentText() + "\n";
        text += "Data: " + GetUserPath().string() + " (logs in logs/), caches: " + GetCachePath().string() + "\n";
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
