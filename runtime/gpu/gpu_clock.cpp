// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See gpu_clock.h.
#include <stdafx.h>
#include "gpu_clock.h"

namespace gpu::clock
{
    namespace
    {
        // trans_stat: a row per frequency, "[*] <Hz>: <transitions...> <ms at it>".
        std::map<uint64_t, uint64_t> ReadTimes(const std::filesystem::path& file)
        {
            std::map<uint64_t, uint64_t> times;
            FILE* f = fopen(file.c_str(), "r");
            if (!f)
                return times;
            char line[1024];
            while (fgets(line, sizeof(line), f))
            {
                const char* p = line;
                while (*p == ' ' || *p == '*')
                    p++;
                char* end;
                unsigned long long hz = strtoull(p, &end, 10);
                if (end == p || *end != ':')
                    continue;
                unsigned long long last = 0;
                for (p = end + 1;;)
                {
                    unsigned long long v = strtoull(p, &end, 10);
                    if (end == p)
                        break;
                    last = v;
                    p = end;
                }
                times[hz] = last;
            }
            fclose(f);
            return times;
        }

        std::filesystem::path FindTransStat()
        {
#ifdef __linux__
            std::error_code ec;
            for (std::filesystem::directory_iterator it("/sys/class/devfreq", ec), end; !ec && it != end; it.increment(ec))
            {
                std::string name = it->path().filename().string();
                if (name.find("gpu") != std::string::npos && std::filesystem::exists(it->path() / "trans_stat", ec))
                    return it->path() / "trans_stat";
            }
#endif
            return {};
        }
    }

    double AverageMHzSinceLast()
    {
        static const std::filesystem::path file = FindTransStat();
        static std::map<uint64_t, uint64_t> last;
        if (file.empty())
            return 0.0;
        std::map<uint64_t, uint64_t> now = ReadTimes(file);
        double weighted = 0.0, ms = 0.0;
        for (const auto& [hz, t] : now)
        {
            auto it = last.find(hz);
            if (it == last.end() || t < it->second)
                continue;
            weighted += double(hz) * double(t - it->second);
            ms += double(t - it->second);
        }
        bool first = last.empty();
        last = std::move(now);
        return first || ms <= 0.0 ? 0.0 : weighted / ms / 1e6;
    }
}
