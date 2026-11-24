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

    // A heading needs 1 to 6 '#'s and a space: "#90" at the start of a wrapped
    // line is the paragraph's (the v0.2.4 CHANGELOG's orange "#90" heading).
    lines = version::ParseNote("## Fixes\n- Car shadows, reported in issue\n  #90, look sharper.\n#91 too.\n\n####### Seven\n#\n");
    CHECK(lines.size() == 5, "%zu lines", lines.size());
    if (lines.size() == 5)
    {
        CHECK(lines[0].kind == K::Heading && lines[0].text == "Fixes", "## heading: '%s'", lines[0].text.c_str());
        // (Each number stays with the word before it: a no-break space there, below.)
        CHECK(lines[1].kind == K::Bullet && lines[1].text == "Car shadows, reported in issue\xC2\xA0#90, look sharper.\xC2\xA0#91 too.",
            "#90 and #91 continue the bullet: '%s'", lines[1].text.c_str());
        CHECK(lines[2].kind == K::Gap, "gap");
        CHECK(lines[3].kind == K::Text && lines[3].text == "####### Seven", "seven #s are text: '%s'", lines[3].text.c_str());
        CHECK(lines[4].kind == K::Heading && lines[4].text.empty(), "a bare # is an empty heading");
    }
    CHECK(version::HeadingMark("### Downloads") == 3 && version::HeadingMark("#\tTab") == 1 && version::HeadingMark("######") == 6,
        "heading marks");
    CHECK(version::HeadingMark("#90") == 0 && version::HeadingMark("#") == 1 && version::HeadingMark("") == 0 &&
            version::HeadingMark("####### x") == 0,
        "not heading marks");

    // Where a line must not break (v0.2.6's What's new split these on every
    // device: the Linux save path after "~/.", "Auto" / "(3600x1600)",
    // "about 1" / "ms", "9" / "to 21 %", "issue" / "#90)"): a no-break space
    // in a `code` span, between a number and its unit, through a range with
    // a unit, before an issue's number, and between a word and a parenthesis
    // that closes in the word it opens.
    const std::string nb = "\xC2\xA0";
    lines = version::ParseNote(
        "- The save folder\n"
        "  is `~/Library/Application Support/SpeedBreaker/save` on a Mac, `%LOCALAPPDATA%\\SpeedBreaker\\save`\n"
        "  on Windows, and `~/.local/share/speedbreaker/save` on Linux and the Steam Deck.\n"
        "- The row shows \"Auto (3600x1600)\". It drops to 30 when hot (\"serious\"), takes about 1\n"
        "  ms a frame off, about 1 to 1.5 ms of GPU time, 190 MB less, 57 fps, 9 to 21 % less (13 to 42 %), 14 days.\n"
        "- Not these: an M1 Pro, a 3600x1600 map, v0.2.5 did, 4 to 12 of them, down to 30 when hot, 2x or 4 times, to 5 of 9, the GPU\n"
        "  (reported by two), C# 7, `a\n"
        "  b` across lines.\n");
    CHECK(lines.size() == 3, "%zu lines", lines.size());
    if (lines.size() == 3)
    {
        const std::string& a = lines[0].text;
        CHECK(a.find("~/Library/Application" + nb + "Support/SpeedBreaker/save on") != std::string::npos, "a code span keeps its space: '%s'",
            a.c_str());
        CHECK(a.find(" ~/.local/share/speedbreaker/save ") != std::string::npos && a.find("%LOCALAPPDATA%\\SpeedBreaker\\save on") != std::string::npos,
            "paths as written: '%s'", a.c_str());
        const std::string& b = lines[1].text;
        for (const std::string& kept : { "\"Auto" + nb + "(3600x1600)\".", "hot" + nb + "(\"serious\")", "about 1" + nb + "ms a frame",
                 "about 1" + nb + "to" + nb + "1.5" + nb + "ms of", "190" + nb + "MB less", "57" + nb + "fps,",
                 " 9" + nb + "to" + nb + "21" + nb + "% less (13" + nb + "to" + nb + "42" + nb + "%),", "14" + nb + "days." })
            CHECK(b.find(kept) != std::string::npos, "kept together: '%s' in '%s'", kept.c_str(), b.c_str());
        CHECK(lines[2].text == "Not these: an M1 Pro, a 3600x1600 map, v0.2.5 did, 4 to 12 of them, down to 30 when hot, 2x or 4 times, to 5 of 9, "
                               "the GPU (reported by two), C# 7, a" + nb + "b across lines.",
            "no other space held: '%s'", lines[2].text.c_str());
    }
    CHECK(version::IsNumber("1.5") && version::IsNumber("7,800") && version::IsNumber("(13") && !version::IsNumber("3600x1600") &&
            !version::IsNumber("v0.2.5") && !version::IsNumber("1.") && !version::IsNumber(""),
        "numbers");
    CHECK(version::IsUnit("ms,") && version::IsUnit("%).") && version::IsUnit("fps") && !version::IsUnit("times") && !version::IsUnit("map"),
        "units");
    CHECK(version::IsIssueNumber("#87).") && version::IsIssueNumber("#101,") && version::IsIssueNumber("(#81)") && !version::IsIssueNumber("#") &&
            !version::IsIssueNumber("#a") && !version::IsIssueNumber("C#") && !version::IsIssueNumber("7") && !version::IsIssueNumber(""),
        "issue numbers");

    // WrapNote, one unit of width a character: at every width the note can
    // take, every line fits, no text is lost, no line ends at a held space
    // (each row's end is a plain space of the text), and those things stay
    // whole on one line; ImGui's own wrap broke the path after "~/.".
    auto chars = [](std::string_view text) {
        float n = 0;
        for (unsigned char c : text)
            n += (c & 0xC0) != 0x80;
        return n;
    };
    auto plain = [&](std::string text) {
        for (size_t at; (at = text.find(nb)) != std::string::npos;)
            text.replace(at, nb.size(), " ");
        return text;
    };
    auto split = [](const std::string& text) {
        std::vector<std::string> out;
        size_t start = 0;
        for (size_t at; (at = text.find('\n', start)) != std::string::npos; start = at + 1)
            out.push_back(text.substr(start, at - start));
        out.push_back(text.substr(start));
        return out;
    };
    if (lines.size() == 3)
    {
        const std::string text = lines[0].text + " " + lines[1].text;
        std::vector<bool> heldAt;  // by byte of plain(text): a no-break space there
        for (size_t i = 0; i < text.size(); i++)
        {
            const bool held = text.compare(i, nb.size(), nb) == 0;
            heldAt.push_back(held);
            if (held)
                i += nb.size() - 1;
        }
        int widthsChecked = 0, heldBreaks = 0;
        for (int width = 48; width <= 140; width++)
        {
            const std::string wrapped = version::WrapNote(text, float(width), chars);
            const std::vector<std::string> rows = split(wrapped);
            std::string joined;
            bool fit = true, ending = false;
            for (const std::string& row : rows)
            {
                if (!joined.empty())
                {
                    heldBreaks += joined.size() < heldAt.size() && heldAt[joined.size()];
                    joined += " ";
                }
                joined += row;
                fit = fit && chars(row) <= float(width);
                ending = ending || row.ends_with("~/.") || row.ends_with("\"Auto") || row.ends_with(" 1.5") || row.ends_with(" ");
            }
            auto onOneRow = [&](const std::string& part) {
                for (const std::string& row : rows)
                    if (row.find(part) != std::string::npos)
                        return true;
                return false;
            };
            CHECK(fit, "width %d: a row too wide:\n%s", width, wrapped.c_str());
            CHECK(joined == plain(text), "width %d: text lost or changed:\n%s", width, wrapped.c_str());
            CHECK(!ending, "width %d: a break inside what stays together:\n%s", width, wrapped.c_str());
            for (const char* part : { "~/.local/share/speedbreaker/save", "~/Library/Application Support/SpeedBreaker/save", "\"Auto (3600x1600)\"",
                     "1 ms", "1 to 1.5 ms", "190 MB", "9 to 21 %", "(13 to 42 %)", "hot (\"serious\")" })
                CHECK(onOneRow(part), "width %d: '%s' split:\n%s", width, part, wrapped.c_str());
            widthsChecked++;
        }
        CHECK(widthsChecked == 93, "%d widths", widthsChecked);
        CHECK(heldBreaks == 0, "%d rows ended at a held space", heldBreaks);
    }
    // v0.2.6's own entry, as its What's new showed it on an iPhone and a Mac:
    // "less CPU and 9" / "to 21 %", "M1 Pro Mac 4" / "to 12 %", "resets the" /
    // "GPU (reported with ..." (a line that had room for "GPU") and "(GitHub
    // issue" / "#90).".
    lines = version::ParseNote(
        "- In a race on an iPhone 18 Pro Max that's 6 to 16 %\n"
        "  less CPU and 9 to 21 % less CPU energy (13 to 42 % less in the menus), and on an M1 Pro Mac 4 to\n"
        "  12 % less CPU power, at the same 60 fps.\n"
        "- When the graphics driver stops answering and the system resets the GPU (reported with Intel\n"
        "  graphics on Windows: GitHub issues #57, #99 and #101, from Astrax2005, AhmadAbd9843 and\n"
        "  swifty-tekno), the game now closes within a few seconds and writes a crash report.\n"
        "- On an iPhone, at 16:9 and on ultrawide screens, the menus' graffiti tags now start at the top\n"
        "  edge of the screen, as they do on a 16:10 Mac since v0.2.3 (GitHub issue #87).\n"
        "- It gives back nearly all the detail across the picture that v0.2.2's fix for shadows missing at\n"
        "  the sides (#81) took away, and on an M1 Pro MacBook Pro it takes 1.5 to 1.7 ms less GPU time a\n"
        "  frame in a race than 2x, and about 190 MB less memory. The row shows \"Auto (3600x1600)\". Other\n"
        "  graphics cards keep 2x, and Steam Deck and Steam Frame keep 1x (GitHub issue #90).\n"
        "- It's off by default: on Intel graphics it costs about 0.5 to 1.5 ms of GPU time a frame.\n");
    CHECK(lines.size() == 5, "%zu lines", lines.size());
    if (lines.size() == 5)
    {
        // A range and its unit, "to" and all.
        CHECK(lines[0].text == "In a race on an iPhone 18 Pro Max that's 6" + nb + "to" + nb + "16" + nb + "% less CPU and 9" + nb + "to" + nb + "21" + nb +
                                   "% less CPU energy (13" + nb + "to" + nb + "42" + nb + "% less in the menus), and on an M1 Pro Mac 4" + nb + "to" + nb +
                                   "12" + nb + "% less CPU power, at the same 60" + nb + "fps.",
            "ranges: '%s'", lines[0].text.c_str());
        CHECK(lines[4].text == "It's off by default: on Intel graphics it costs about 0.5" + nb + "to" + nb + "1.5" + nb + "ms of GPU time a frame.",
            "a range in ms: '%s'", lines[4].text.c_str());
        // A parenthesis that goes on past the word it opens: a plain space
        // before it. An issue's number: with the word before it.
        CHECK(lines[1].text == "When the graphics driver stops answering and the system resets the GPU (reported with Intel graphics on Windows: "
                               "GitHub issues" + nb + "#57," + nb + "#99 and" + nb + "#101, from Astrax2005, AhmadAbd9843 and swifty-tekno), the game "
                               "now closes within a few seconds and writes a crash report.",
            "a long parenthesis, issue numbers: '%s'", lines[1].text.c_str());
        CHECK(lines[2].text.ends_with("since v0.2.3 (GitHub issue" + nb + "#87).") && lines[2].text.find(nb) == lines[2].text.size() - 7,
            "issue #87: '%s'", lines[2].text.c_str());
        // The parenthesis that closes in its word still stays with the word
        // before it.
        CHECK(lines[3].text == "It gives back nearly all the detail across the picture that v0.2.2's fix for shadows missing at the sides" + nb +
                                   "(#81) took away, and on an M1 Pro MacBook Pro it takes 1.5" + nb + "to" + nb + "1.7" + nb + "ms less GPU time a frame "
                                   "in a race than 2x, and about 190" + nb + "MB less memory. The row shows \"Auto" + nb + "(3600x1600)\". Other graphics "
                                   "cards keep 2x, and Steam Deck and Steam Frame keep 1x (GitHub issue" + nb + "#90).",
            "short parentheses, issue #90: '%s'", lines[3].text.c_str());

        int rowsChecked = 0;
        for (const version::NoteLine& line : lines)
        {
            const std::string whole = plain(line.text);
            std::string solid = whole;  // without its spaces
            std::erase(solid, ' ');
            for (int width = 1; width <= 140; width++)
            {
                const std::string wrapped = version::WrapNote(line.text, float(width), chars);
                const std::vector<std::string> rows = split(wrapped);
                // Every row fits and no text is lost, down to one character
                // a row (a group wider than the row breaks where it must).
                std::string joined, glued;
                bool fit = true;
                for (const std::string& row : rows)
                {
                    joined += (joined.empty() ? "" : " ") + row;
                    glued += row;
                    fit = fit && chars(row) <= float(width) && !row.empty() && row.front() != ' ' && row.back() != ' ';
                    rowsChecked++;
                }
                std::erase(glued, ' ');
                CHECK(fit, "width %d: a row too wide, empty, or with a space at an end:\n%s", width, wrapped.c_str());
                CHECK(glued == solid, "width %d: text lost:\n%s", width, wrapped.c_str());
                if (width < 48)
                    continue;
                // At the widths the note takes: breaks at the text's spaces
                // only, and each group whole on one row.
                CHECK(joined == whole, "width %d: text changed:\n%s", width, wrapped.c_str());
                for (const char* part : { "6 to 16 %", "9 to 21 %", "(13 to 42 %", "4 to 12 %", "60 fps.", "0.5 to 1.5 ms", "1.5 to 1.7 ms", "190 MB",
                         "issues #57, #99", "and #101,", "issue #87).", "issue #90).", "sides (#81)", "\"Auto (3600x1600)\"." })
                {
                    if (whole.find(part) == std::string::npos)
                        continue;
                    bool oneRow = false;
                    for (const std::string& row : rows)
                        oneRow = oneRow || row.find(part) != std::string::npos;
                    CHECK(oneRow, "width %d: '%s' split:\n%s", width, part, wrapped.c_str());
                }
            }
        }
        CHECK(rowsChecked > 5 * 140, "%d rows", rowsChecked);
        // "GPU" goes on the first row as soon as it fits there (70 wide), and
        // "(reported" when it does (80); held together, the row ended at
        // "resets the" up to 79.
        for (int width = 66; width <= 80; width++)
        {
            const std::string first = split(version::WrapNote(lines[1].text, float(width), chars))[0];
            const char* end = width < 70 ? "resets the" : width < 80 ? "resets the GPU" : "resets the GPU (reported";
            CHECK(first.ends_with(end), "width %d: the first row ends '%s'", width, first.c_str());
        }
    }
    // A group wider than the row: it breaks at its own spaces, then wherever
    // it must, and nothing is lost.
    CHECK(version::WrapNote("9" + nb + "to" + nb + "21" + nb + "% less", 6, chars) == "9 to\n21 %\nless", "a range wider than the row: '%s'",
        version::WrapNote("9" + nb + "to" + nb + "21" + nb + "% less", 6, chars).c_str());
    CHECK(version::WrapNote("0.5" + nb + "to" + nb + "1.5" + nb + "ms of GPU", 9, chars) == "0.5 to\n1.5 ms of\nGPU", "a range in ms wider than the row: '%s'",
        version::WrapNote("0.5" + nb + "to" + nb + "1.5" + nb + "ms of GPU", 9, chars).c_str());
    CHECK(version::WrapNote("issue" + nb + "#87) now", 4, chars) == "issu\ne\n#87)\nnow", "an issue's number wider than the row: '%s'",
        version::WrapNote("issue" + nb + "#87) now", 4, chars).c_str());

    CHECK(version::WrapNote("one two three", 100, chars) == "one two three", "fits: no break");
    CHECK(version::WrapNote("one two three", 7, chars) == "one two\nthree", "at a space: '%s'", version::WrapNote("one two three", 7, chars).c_str());
    CHECK(version::WrapNote("one  two", 4, chars) == "one\ntwo", "the spaces at a break go");
    CHECK(version::WrapNote("aaa" + nb + "bbb" + nb + "ccc ddd", 7, chars) == "aaa bbb\nccc ddd",
        "a held run wider than the line breaks at its own spaces: '%s'", version::WrapNote("aaa" + nb + "bbb" + nb + "ccc ddd", 7, chars).c_str());
    CHECK(version::WrapNote("abcdefghij", 4, chars) == "abcd\nefgh\nij", "a word wider than the line: cut, nothing lost");
    CHECK(version::WrapNote("\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9", 2, chars) == "\xC3\xA9\xC3\xA9\n\xC3\xA9\xC3\xA9\n\xC3\xA9",
        "cut between whole characters");
    CHECK(version::WrapNote("abc", 0.5f, chars) == "a\nb\nc", "at least one character a row");
    CHECK(version::WrapNote("", 10, chars).empty(), "nothing");

    printf("%s (%d failures)\n", g_failures ? "FAILED" : "passed", g_failures);
    return g_failures ? 1 : 0;
}
