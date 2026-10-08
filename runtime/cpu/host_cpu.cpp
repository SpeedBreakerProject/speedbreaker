// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
#include <stdafx.h>
#include "host_cpu.h"

#ifdef __linux__
#include <sched.h>
#include <sys/prctl.h>
#endif

namespace hostcpu
{
#ifdef __linux__
    namespace
    {
        // Set by ReserveFastCore before any other thread exists, read-only after.
        bool s_reserving = false;
        cpu_set_t s_reserved, s_shared;

        uint64_t ReadCpuValue(int cpu, const char* file)
        {
            char path[96];
            snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/%s", cpu, file);
            FILE* f = fopen(path, "r");
            if (!f)
                return 0;
            unsigned long long value = 0;
            if (fscanf(f, "%llu", &value) != 1)
                value = 0;
            fclose(f);
            return value;
        }

        std::string CpuList(const cpu_set_t& set)
        {
            std::string list;
            for (int cpu = 0; cpu < CPU_SETSIZE; cpu++)
                if (CPU_ISSET(cpu, &set))
                    list += (list.empty() ? "" : ",") + std::to_string(cpu);
            return list;
        }
    }
#endif

    void ReserveFastCore()
    {
#ifdef __linux__
        // NFSMW_CP_OWN_CORE=0: never; =fast: only on CPUs with fast and slow
        // cores (the rule until 2026-09-29, when alike cores got one too).
        const char* mode = std::getenv("NFSMW_CP_OWN_CORE");
        if (mode && mode[0] == '0')
            return;
        bool fastOnly = mode && strcmp(mode, "fast") == 0;
        cpu_set_t allowed;
        if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0)
            return;
        // Each CPU's top speed: cpuinfo_max_freq (Intel's hybrid cores), or
        // where that is the same on every core, as amd-pstate reports it, the
        // CPPC highest performance (196 on two of the Steam Machine's cores,
        // 144 on the other four; 255 on each of the Deck's). A file that reads
        // 0 everywhere isn't there: the other one tells.
        struct Cpu { int cpu; uint64_t speed, core, capacity; };
        std::vector<Cpu> cpus, read;
        for (const char* file : { "cpufreq/cpuinfo_max_freq", "acpi_cppc/highest_perf" })
        {
            read.clear();
            for (int cpu = 0; cpu < CPU_SETSIZE; cpu++)
                if (CPU_ISSET(cpu, &allowed))
                    read.push_back({ cpu, ReadCpuValue(cpu, file),
                        ReadCpuValue(cpu, "topology/physical_package_id") << 32 | ReadCpuValue(cpu, "topology/core_id"),
                        ReadCpuValue(cpu, "cpu_capacity") });
            if (std::all_of(read.begin(), read.end(), [](const Cpu& c) { return c.speed == 0; }))
                continue;
            cpus = read;
            if (std::any_of(cpus.begin(), cpus.end(), [&](const Cpu& c) { return c.speed != cpus[0].speed; }))
                break;
        }
        if (cpus.empty())
            return;
        auto [low, high] = std::minmax_element(cpus.begin(), cpus.end(), [](const Cpu& a, const Cpu& b) { return a.speed < b.speed; });
        if (low->speed == 0)
            return;
        std::vector<uint64_t> cores, fastCores;
        for (const Cpu& c : cpus)
        {
            if (std::find(cores.begin(), cores.end(), c.core) == cores.end())
                cores.push_back(c.core);
            if (c.speed * 100 >= high->speed * 95 && std::find(fastCores.begin(), fastCores.end(), c.core) == fastCores.end())
                fastCores.push_back(c.core);
        }
        // A real split into fast and slow cores (the Steam Machine's slow
        // ones top out at 73%) needs at least two fast physical cores, so the
        // game's own threads keep one. Cores alike (a desktop Ryzen's
        // preferred-core ranking differs by a few percent, the Deck's not at
        // all) need at least four, so the other threads keep three: the
        // whole game uses up to 2.3 cores in the fly-in, the command
        // processor one of them. On the Deck its busy time fell 2-6%, with
        // fewer late frames. The reserved core is the fast one
        // with the most capacity (ARM: the kernel's cpu_capacity, which
        // weighs a core's size too; the Steam Frame's Cortex-X4, cpu 7, tops
        // out at 3.05 GHz to its A720s' 3.15 but does 20% more), then the
        // last (cpu 0's core stays shared; x86 has no cpu_capacity).
        bool split = low->speed * 100 <= high->speed * 85;
        if (split ? cpus.size() < 6 || fastCores.size() < 2 : fastOnly || cores.size() < 4)
            return;
        uint64_t core = 0, capacity = 0;
        for (const Cpu& c : cpus)
            if (std::find(fastCores.begin(), fastCores.end(), c.core) != fastCores.end() &&
                (c.capacity > capacity || (c.capacity == capacity && c.core > core)))
            {
                core = c.core;
                capacity = c.capacity;
            }
        CPU_ZERO(&s_reserved);
        s_shared = allowed;
        for (const Cpu& c : cpus)
            if (c.core == core)
            {
                CPU_SET(c.cpu, &s_reserved);
                CPU_CLR(c.cpu, &s_shared);
            }
        if (sched_setaffinity(0, sizeof(s_shared), &s_shared) != 0)
            return;
        s_reserving = true;
        fprintf(stderr, "[cpu] command processor: cpus %s (a %score of its own); other threads: cpus %s\n",
            CpuList(s_reserved).c_str(), split ? "fast " : "", CpuList(s_shared).c_str());
#endif
    }

    void UseReservedCore(const char* who)
    {
#ifdef __linux__
        if (s_reserving && sched_setaffinity(0, sizeof(s_reserved), &s_reserved) != 0)
            fprintf(stderr, "[cpu] %s: could not move to its reserved core\n", who);
#else
        (void)who;
#endif
    }

    void LeaveReservedCore()
    {
#ifdef __linux__
        if (s_reserving)
            sched_setaffinity(0, sizeof(s_shared), &s_shared);
#endif
    }

    void TightenTimerSlack()
    {
#ifdef __linux__
        thread_local bool done = false;
        if (!done)
        {
            prctl(PR_SET_TIMERSLACK, 1000UL, 0, 0, 0);
            done = true;
        }
#endif
    }
}
