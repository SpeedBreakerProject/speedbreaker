// Tests for procstat (runtime/report/proc_stat.h): Linux's /proc/<pid>/task/<tid>/stat as the [cpu] line reads it
// (NFSMW_CPU_LOG on SteamOS, item #71): the thread's name, its state and its user and system time. Sample texts on
// any system; on Linux also this test's own thread, against its comm file and a stretch of spinning. From the repo
// root:
//   clang++ -std=c++20 -O2 -Iruntime tests/proc_stat_test.cpp -o build/proc_stat_test
//   build/proc_stat_test
#include <report/proc_stat.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#ifdef __linux__
#include <sys/syscall.h>
#include <unistd.h>
#endif

using namespace procstat;

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void Samples()
{
    // A Steam Deck thread's line (the shape of SteamOS 3.9's kernel 7.2; all 52 fields).
    const char* cp = "48211 (nfsmw-cp) S 48190 48190 48190 0 -1 4194624 1234 0 0 0 5821 390 0 0 20 0 45 0 987654 "
                     "6442450944 123456 18446744073709551615 1 1 0 0 0 0 0 4096 17663 0 0 0 -1 6 0 0 0 0 0 0 0 0 0 0 0 0 0\n";
    TaskStat t;
    CHECK(ParseTaskStat(cp, t), "the CP's line");
    CHECK(t.name == "nfsmw-cp", "%s", t.name.c_str());
    CHECK(t.state == 'S', "%c", t.state);
    CHECK(t.utime == 5821 && t.stime == 390, "%llu %llu", (unsigned long long)t.utime, (unsigned long long)t.stime);

    // A guest thread, running, the fields' end without a newline.
    TaskStat g;
    CHECK(ParseTaskStat("48230 (g2-8284FE40) R 48190 48190 48190 0 -1 4194368 0 0 0 0 123456789012 7 0 0 20 0", g), "g2");
    CHECK(g.name == "g2-8284FE40" && g.state == 'R' && g.utime == 123456789012ull && g.stime == 7, "%s %c %llu %llu",
        g.name.c_str(), g.state, (unsigned long long)g.utime, (unsigned long long)g.stime);

    // A name may hold spaces, '(' and ')': it runs to the LAST ')'.
    TaskStat s;
    CHECK(ParseTaskStat("77 (a ) (b c) D 1 1 1 0 -1 0 0 0 0 0 10 20 0 0 20 0 1 0 5\n", s), "a name with ') ('");
    CHECK(s.name == "a ) (b c" && s.state == 'D' && s.utime == 10 && s.stime == 20, "%s %c %llu %llu", s.name.c_str(), s.state,
        (unsigned long long)s.utime, (unsigned long long)s.stime);
    TaskStat p;
    CHECK(ParseTaskStat("88 ((sd-pam)) S 1 1 1 0 -1 0 0 0 0 0 3 4 0 0", p) && p.name == "(sd-pam)" && p.utime == 3 && p.stime == 4,
        "%s", p.name.c_str());
    TaskStat e;
    CHECK(ParseTaskStat("90 () S 1 1 1 0 -1 0 0 0 0 0 0 0", e) && e.name.empty() && e.utime == 0 && e.stime == 0, "an empty name");

    // Not a stat text: false, and the result is left as it was.
    TaskStat keep;
    keep.name = "kept";
    keep.utime = 5;
    CHECK(!ParseTaskStat("", keep), "empty");
    CHECK(!ParseTaskStat("48211 nfsmw-cp S 1 2 3", keep), "no parentheses");
    CHECK(!ParseTaskStat("48211 (nfsmw-cp S 1 2 3 4 5 6 7 8 9 10 11 12", keep), "no ')'");
    CHECK(!ParseTaskStat("48211 )nfsmw-cp( S 1 1 1 0 -1 0 0 0 0 0 1 2", keep), "')' before '('");
    CHECK(!ParseTaskStat("48211 (nfsmw-cp) S 48190 48190 48190 0 -1 4194624 1234 0 0 0 5821", keep), "ends before stime");
    CHECK(!ParseTaskStat("48211 (nfsmw-cp) S 48190 48190 48190 0 -1 4194624 1234 0 0 0 58x1 390 0", keep), "utime not a number");
    CHECK(!ParseTaskStat("48211 (nfsmw-cp) S 48190 48190 48190 0 -1 4194624 1234 0 0 0 -5 390 0", keep), "a negative utime");
    CHECK(!ParseTaskStat("48211 (nfsmw-cp) SS 48190 48190 48190 0 -1 4194624 1234 0 0 0 5821 390", keep), "a state of two letters");
    CHECK(!ParseTaskStat("48211 (nfsmw-cp) S 48190 48190\n48190 0 -1 4194624 1234 0 0 0 5821 390", keep), "a newline before stime");
    CHECK(keep.name == "kept" && keep.utime == 5, "changed by a failed parse: %s %llu", keep.name.c_str(), (unsigned long long)keep.utime);
}

#ifdef __linux__
static std::string ReadFile(const std::string& path)
{
    std::ifstream f(path);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

// This thread's own line, as the game reads its threads': the name its comm file holds, and time that grows while it
// spins.
static void OwnThread()
{
    const long tid = syscall(SYS_gettid);
    const std::string dir = "/proc/self/task/" + std::to_string(tid);
    std::string comm = ReadFile(dir + "/comm");
    while (!comm.empty() && comm.back() == '\n')
        comm.pop_back();
    TaskStat before;
    CHECK(ParseTaskStat(ReadFile(dir + "/stat"), before), "this thread's stat");
    CHECK(before.name == comm, "name %s, comm %s", before.name.c_str(), comm.c_str());
    CHECK(before.state == 'R', "%c (the reading thread runs)", before.state);
    const long tck = sysconf(_SC_CLK_TCK);
    volatile uint64_t x = 1;
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(300))
        x = x * 6364136223846793005ull + 1442695040888963407ull;
    TaskStat after;
    CHECK(ParseTaskStat(ReadFile(dir + "/stat"), after), "this thread's stat again");
    const uint64_t ticks = (after.utime + after.stime) - (before.utime + before.stime);
    // 300 ms of spinning: at least 10 ticks of 10 ms at the usual 100 a second (some of it may wait for a CPU).
    CHECK(tck > 0 && ticks * 1000 / uint64_t(tck) >= 100 && ticks * 1000 / uint64_t(tck) <= 1000, "%llu ticks at %ld a second",
        (unsigned long long)ticks, tck);
    printf("  this thread: %s, %llu ticks over 300 ms of spinning (%ld a second)\n", after.name.c_str(), (unsigned long long)ticks, tck);
}
#endif

int main()
{
    Samples();
#ifdef __linux__
    OwnThread();
#endif
    if (g_failures)
    {
        printf("%d FAILED\n", g_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
