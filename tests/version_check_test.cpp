// Tests for user/version_check.h: the session log's [version] line, when the
// "What's new" note shows, and how a CHANGELOG.md entry is laid out. From the
// repo root:
//   clang++ -std=c++20 -O2 -Iruntime tests/version_check_test.cpp -o build/version_check_test
//   build/version_check_test
#include <user/version_check.h>

#include <cstdio>
#include <string>

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main()
{
    using version::Change;

    // Parse and Compare.
    std::array<uint32_t, 3> v;
    CHECK(version::Parse("0.1.0", v) && v == (std::array<uint32_t, 3>{ 0, 1, 0 }), "0.1.0");
    CHECK(version::Parse("v1.20.3", v) && v == (std::array<uint32_t, 3>{ 1, 20, 3 }), "v1.20.3");
    for (const char* bad : { "", "1", "1.2", "1.2.3.4", "1..2", ".1.2", "1.2.", "1.2.x", "406a23d", "v", "1.2.3-dirty", "99999999.0.0" })
        CHECK(!version::Parse(bad, v), "'%s' isn't a version", bad);
    CHECK(version::Compare("0.1.0", "0.1.0") == 0, "equal");
    CHECK(version::Compare("0.2.0", "0.1.9") == 1, "minor beats patch");
    CHECK(version::Compare("0.10.0", "0.9.0") == 1, "numbers, not text");
    CHECK(version::Compare("1.0.0", "0.99.99") == 1, "major");
    CHECK(version::Compare("0.1.0", "garbage") == -1 && version::Compare("garbage", "0.1.0") == 1, "unparsable after parsable");

    // Classify and Describe: the log's line.
    CHECK(version::Classify("", false, "0.1.0") == Change::FirstRun, "no settings.toml: first run");
    CHECK(version::Classify("", true, "0.1.0") == Change::Unversioned, "settings without a version: an earlier build");
    CHECK(version::Classify("0.1.0", true, "0.1.0") == Change::Same, "same");
    CHECK(version::Classify("0.1.0", true, "0.2.0") == Change::Updated, "updated");
    CHECK(version::Classify("0.2.0", true, "0.1.0") == Change::Older, "went back");
    CHECK(version::Describe(Change::FirstRun, "", "0.1.0") == "first run (v0.1.0)", "%s", version::Describe(Change::FirstRun, "", "0.1.0").c_str());
    CHECK(version::Describe(Change::Updated, "0.1.0", "0.2.0") == "updated from v0.1.0 to v0.2.0", "%s",
        version::Describe(Change::Updated, "0.1.0", "0.2.0").c_str());
    CHECK(version::Describe(Change::Older, "0.2.0", "0.1.0") == "went back from v0.2.0 to v0.1.0", "older");
    CHECK(version::Describe(Change::Same, "0.1.0", "0.1.0") == "v0.1.0, the same as the last run", "same");
    CHECK(version::Describe(Change::Unversioned, "", "0.1.0").starts_with("updated to v0.1.0 from a build before"), "unversioned");

    // When the note shows: once, after an update; never for a new player.
    const char* entry = "- One thing.";
    CHECK(version::ShowWhatsNew(Change::Updated, entry), "updated");
    CHECK(version::ShowWhatsNew(Change::Unversioned, entry), "from a build before versions");
    CHECK(!version::ShowWhatsNew(Change::FirstRun, entry), "not on a new player's first run");
    CHECK(!version::ShowWhatsNew(Change::Same, entry), "not twice");
    CHECK(!version::ShowWhatsNew(Change::Older, entry), "not after going back");
    CHECK(!version::ShowWhatsNew(Change::Updated, " \n\t\n"), "not without an entry");

    // The note's lines.
    auto lines = version::ParseNote(
        "The first public release.\nIt runs your own copy.\n\n### Downloads\n- **Steam Deck**: the `x86_64` file\n  (and Linux PCs).\n"
        "* Mac\n\n\nLast paragraph.\r\n");
    using K = version::NoteLine::Kind;
    CHECK(lines.size() == 7, "%zu lines", lines.size());
    if (lines.size() == 7)
    {
        CHECK(lines[0].kind == K::Text && lines[0].text == "The first public release. It runs your own copy.", "paragraph joined: '%s'",
            lines[0].text.c_str());
        CHECK(lines[1].kind == K::Gap, "gap");
        CHECK(lines[2].kind == K::Heading && lines[2].text == "Downloads", "heading: '%s'", lines[2].text.c_str());
        CHECK(lines[3].kind == K::Bullet && lines[3].text == "Steam Deck: the x86_64 file (and Linux PCs).", "bullet, marks dropped: '%s'",
            lines[3].text.c_str());
        CHECK(lines[4].kind == K::Bullet && lines[4].text == "Mac", "* bullet");
        CHECK(lines[5].kind == K::Gap, "one gap for two blank lines");
        CHECK(lines[6].kind == K::Text && lines[6].text == "Last paragraph.", "CR dropped: '%s'", lines[6].text.c_str());
    }
    CHECK(version::ParseNote("").empty() && version::ParseNote("\n\n").empty(), "nothing");

    printf("%s (%d failures)\n", g_failures ? "FAILED" : "passed", g_failures);
    return g_failures ? 1 : 0;
}
