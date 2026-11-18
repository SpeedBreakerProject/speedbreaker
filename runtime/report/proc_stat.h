// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Reading Linux's /proc/<pid>/task/<tid>/stat for the [cpu] line
// (report/cpu_use.h, NFSMW_CPU_LOG; item #71 on SteamOS): a thread's name and
// its user and system time. Header only, so tests/proc_stat_test.cpp reads
// sample texts with it on any system.
//
// The text is "<tid> (<comm>) <state> <ppid> ... <utime> <stime> ...": comm is
// the thread's name (15 bytes at most) and may hold spaces and ')' itself, so
// the name runs from the first '(' to the LAST ')', and the fields are counted
// from there (proc(5): utime is field 14, stime field 15, in clock ticks).
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace procstat
{
    struct TaskStat
    {
        std::string name;
        char state = 0;
        uint64_t utime = 0;  // clock ticks (sysconf(_SC_CLK_TCK) a second)
        uint64_t stime = 0;
    };

    // One stat text into `out`; false (and `out` unchanged) when it isn't one.
    inline bool ParseTaskStat(std::string_view text, TaskStat& out)
    {
        const size_t open = text.find('(');
        const size_t close = text.rfind(')');
        if (open == std::string_view::npos || close == std::string_view::npos || close < open)
            return false;
        TaskStat t;
        t.name.assign(text.substr(open + 1, close - open - 1));
        // Fields from 3 (state) on, separated by single spaces.
        size_t pos = close + 1;
        int field = 2;
        uint64_t value[2] = {};
        bool have[2] = {};
        while (pos < text.size() && field < 15)
        {
            while (pos < text.size() && text[pos] == ' ')
                pos++;
            if (pos >= text.size())
                break;
            size_t end = pos;
            while (end < text.size() && text[end] != ' ' && text[end] != '\n')
                end++;
            field++;
            std::string_view word = text.substr(pos, end - pos);
            if (field == 3)
            {
                if (word.size() != 1)
                    return false;
                t.state = word[0];
            }
            else if (field == 14 || field == 15)
            {
                if (word.empty())
                    return false;
                uint64_t v = 0;
                for (char c : word)
                {
                    if (c < '0' || c > '9')
                        return false;
                    v = v * 10 + uint64_t(c - '0');
                }
                value[field - 14] = v;
                have[field - 14] = true;
            }
            if (end < text.size() && text[end] == '\n')
                break;
            pos = end;
        }
        if (!have[0] || !have[1])
            return false;
        t.utime = value[0];
        t.stime = value[1];
        out = std::move(t);
        return true;
    }
}
