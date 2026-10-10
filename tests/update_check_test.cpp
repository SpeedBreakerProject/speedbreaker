// Tests for update/check.h: Check for Updates' logic. Versions (Semantic
// Versioning), the strict JSON reader for GitHub's release, the HTTP answer
// as curl prints it, the release notes as plain lines, and the result of
// every outcome. From the repo root:
//   clang++ -std=c++20 -O2 -Iruntime tests/update_check_test.cpp runtime/update/check.cpp -o build/update_check_test
//   build/update_check_test
#include <update/check.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

using namespace update;
using K = version::NoteLine::Kind;

static int Cmp(const char* a, const char* b)
{
    Semver x, y;
    if (!ParseSemver(a, x) || !ParseSemver(b, y))
        return 99;
    return CompareSemver(x, y);
}

static void Versions()
{
    Semver v;
    CHECK(ParseSemver("0.1.2", v) && v.major == 0 && v.minor == 1 && v.patch == 2 && v.pre.empty() && v.build.empty(), "0.1.2");
    CHECK(ParseSemver("v10.20.30", v) && v.major == 10 && v.minor == 20 && v.patch == 30, "v10.20.30");
    CHECK(ParseSemver("1.0.0-rc.1+build.5", v) && v.pre.size() == 2 && v.pre[0] == "rc" && v.pre[1] == "1" && v.build == "build.5",
        "pre-release and build");
    CHECK(ParseSemver("1.0.0-x-y.0", v) && v.pre.size() == 2 && v.pre[0] == "x-y", "hyphens in identifiers");
    CHECK(FormatSemver(v) == "1.0.0-x-y.0", "%s", FormatSemver(v).c_str());
    for (const char* bad : { "", "v", "1", "1.2", "1.2.3.4", "01.2.3", "1.02.3", "1.2.03", "1..3", "1.2.3-", "1.2.3+", "1.2.3-01",
             "1.2.3-a..b", "1.2.3-a_b", "1.2.3 ", " 1.2.3", "1.2.x", "-1.2.3", "1.2.3-\xC3\xA9", "1234567890123456.0.0", "vv1.2.3" })
        CHECK(!ParseSemver(bad, v), "'%s' isn't a version", bad);
    CHECK(ParseSemver("1.2.3-0", v) && ParseSemver("1.2.3+01", v), "a lone 0, and leading zeros in build metadata, are fine");

    CHECK(Cmp("0.1.1", "0.1.1") == 0, "equal");
    CHECK(Cmp("0.1.2", "0.1.1") == 1 && Cmp("0.1.1", "0.1.2") == -1, "patch");
    CHECK(Cmp("0.2.0", "0.1.9") == 1, "minor beats patch");
    CHECK(Cmp("1.0.0", "0.99.99") == 1, "major beats minor");
    CHECK(Cmp("0.10.0", "0.9.0") == 1, "numbers, not text");
    CHECK(Cmp("v0.1.0", "0.1.0") == 0, "the v doesn't count");
    CHECK(Cmp("1.0.0+a", "1.0.0+b") == 0, "build metadata doesn't count");
    // semver.org's own order.
    const char* order[] = { "1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta", "1.0.0-beta.2", "1.0.0-beta.11",
        "1.0.0-rc.1", "1.0.0", "1.0.1-0", "1.0.1" };
    for (size_t i = 0; i + 1 < std::size(order); i++)
    {
        CHECK(Cmp(order[i], order[i + 1]) == -1, "%s < %s", order[i], order[i + 1]);
        CHECK(Cmp(order[i + 1], order[i]) == 1, "%s > %s", order[i + 1], order[i]);
    }
    CHECK(Cmp("1.0.0-2", "1.0.0-10") == -1, "numeric identifiers compare as numbers");
    CHECK(Cmp("1.0.0-a10", "1.0.0-a9") == -1, "alphanumeric ones as text");
}

// A release as GitHub's API returns it (abridged), with members named like
// ours in nested objects that must not be read.
static const char kRelease[] = R"({
  "url": "https://api.github.com/repos/SpeedBreakerProject/speedbreaker/releases/1",
  "html_url": "https://github.com/SpeedBreakerProject/speedbreaker/releases/tag/v0.1.2",
  "id": 255474781, "size": -1.5e+3, "zero": 0, "frac": 0.25, "neg": -0,
  "author": { "login": "x", "name": "not the release", "tag_name": "v9.9.9", "site_admin": false },
  "tag_name": "v0.1.2",
  "target_commitish": "main",
  "name": "SpeedBreaker v0.1.2 \u2014 \"Rockport\" \ud83c\udfce",
  "draft": false,
  "prerelease": false,
  "assets": [ { "name": "SpeedBreaker-macos.dmg", "body": "no", "size": 123 }, [], {}, null, true ],
  "body": "## What's new\r\n\r\n- Caf\u00e9 \\ path\/x\ttab\n- r\u00e9sum\u00e9 \u20ac10 \uD83D\uDE00\nend",
  "reactions": {"+1": 3}
})";

static void Json()
{
    Release r;
    std::string error;
    CHECK(ParseRelease(kRelease, r, error), "the release: %s", error.c_str());
    CHECK(r.hasTag && r.tag == "v0.1.2", "tag: '%s' (not the nested one)", r.tag.c_str());
    CHECK(r.url == "https://github.com/SpeedBreakerProject/speedbreaker/releases/tag/v0.1.2", "html_url: '%s'", r.url.c_str());
    CHECK(r.name == "SpeedBreaker v0.1.2 \xE2\x80\x94 \"Rockport\" \xF0\x9F\x8F\x8E", "name, \\u escapes and a surrogate pair: '%s'",
        r.name.c_str());
    CHECK(r.body == "## What's new\r\n\r\n- Caf\xC3\xA9 \\ path/x\ttab\n- r\xC3\xA9sum\xC3\xA9 \xE2\x82\xAC" "10 \xF0\x9F\x98\x80\nend",
        "body: '%s'", r.body.c_str());
    CHECK(!r.draft && !r.prerelease, "flags");

    CHECK(ParseRelease(R"({"tag_name":"v1.0.0","name":null,"body":null,"draft":true,"prerelease":true})", r, error) && r.name.empty() &&
              r.body.empty() && r.draft && r.prerelease,
        "nulls and true flags: %s", error.c_str());
    error = "from an earlier answer";
    CHECK(ParseRelease("{}", r, error), "an error left from before doesn't fail a good answer");
    CHECK(ParseRelease("\xEF\xBB\xBF{}", r, error) == false, "a byte-order mark isn't JSON");
    CHECK(ParseRelease(" \n{ } \r\n", r, error) && !r.hasTag, "an empty object: no tag (%s)", error.c_str());
    CHECK(ParseRelease(R"({"body":"raw UTF-8: )" "\xC3\xA9\xE2\x82\xAC\xF0\x9F\x8F\x8E" R"("})", r, error) &&
              r.body == "raw UTF-8: \xC3\xA9\xE2\x82\xAC\xF0\x9F\x8F\x8E",
        "raw UTF-8 kept: %s", error.c_str());
    CHECK(ParseRelease(R"({"body":"\u0041\u00DF\u6771\uD801\uDC37"})", r, error) && r.body == "A\xC3\x9F\xE6\x9D\xB1\xF0\x90\x90\xB7",
        "upper-case hex, 1-4 byte encodings: '%s'", r.body.c_str());

    // Malformed answers: each is refused, and says why.
    struct Bad { const char* json; const char* why; };
    const Bad bad[] = {
        { "", "empty" },
        { "   ", "empty" },
        { "[]", "not a JSON object" },
        { "\"tag_name\"", "not a JSON object" },
        { "<html>rate limited</html>", "not a JSON object" },
        { "{", "a missing member name" },
        { "{\"tag_name\":\"v1.0.0\"", "a missing ',' or '}'" },
        { "{\"tag_name\":\"v1.0.0\",}", "a missing member name" },
        { "{\"tag_name\" \"v1.0.0\"}", "a missing ':'" },
        { "{\"tag_name\":\"v1.0.0}", "an unterminated string" },
        { "{\"tag_name\":\"v1.0.0\"} x", "more after the JSON" },
        { "{\"tag_name\":\"v1.0.0\"}{}", "more after the JSON" },
        { "{\"a\":[1,2,]}", "an unexpected ']'" },
        { "{\"a\":[1 2]}", "a missing ',' or ']'" },
        { "{\"a\":01}", "a missing ',' or '}'" },
        { "{\"a\":1.}", "a bad number" },
        { "{\"a\":.5}", "an unexpected '.'" },
        { "{\"a\":-}", "a bad number" },
        { "{\"a\":1e}", "a bad number" },
        { "{\"a\":tru}", "an unknown word" },
        { "{\"a\":nul}", "an unknown word" },
        { "{\"a\":'x'}", "an unexpected '''" },
        { "{\"body\":\"a\nb\"}", "a control character in a string" },
        { "{\"body\":\"tab\there\"}", "a control character in a string" },
        { "{\"body\":\"\\x41\"}", "a bad escape" },
        { "{\"body\":\"\\u12\"}", "a bad \\u escape" },
        { "{\"body\":\"\\u12G4\"}", "a bad \\u escape" },
        { "{\"body\":\"\\ud83c\"}", "an unpaired surrogate" },
        { "{\"body\":\"\\ud83cx\"}", "an unpaired surrogate" },
        { "{\"body\":\"\\ud83c\\u0041\"}", "an unpaired surrogate" },
        { "{\"body\":\"\\udfce\"}", "an unpaired surrogate" },
        { "{\"body\":\"\xC0\x80\"}", "invalid UTF-8" },          // an overlong NUL
        { "{\"body\":\"\xE0\x80\xAF\"}", "invalid UTF-8" },      // an overlong '/'
        { "{\"body\":\"\xED\xA0\x80\"}", "invalid UTF-8" },      // a surrogate in UTF-8
        { "{\"body\":\"\xF4\x90\x80\x80\"}", "invalid UTF-8" },  // past U+10FFFF
        { "{\"body\":\"\xF5\x80\x80\x80\"}", "invalid UTF-8" },
        { "{\"body\":\"\xC3\"}", "invalid UTF-8" },              // cut short
        { "{\"body\":\"\x80\"}", "invalid UTF-8" },
        { "{\"tag_name\":5}", "\"tag_name\" of the wrong type" },
        { "{\"tag_name\":null}", "\"tag_name\" of the wrong type" },
        { "{\"tag_name\":[\"v1.0.0\"]}", "\"tag_name\" of the wrong type" },
        { "{\"draft\":\"false\"}", "\"draft\" of the wrong type" },
        { "{\"prerelease\":0}", "\"prerelease\" of the wrong type" },
        { "{\"body\":{}}", "\"body\" of the wrong type" },
        { "{\"tag_name\":\"v1.0.0\",\"tag_name\":\"v9.0.0\"}", "a second \"tag_name\"" },
        { "{\"tag_name\":\"v1.0.0\",\"x\":{\"a\":", "a missing value" },
    };
    for (const Bad& b : bad)
    {
        error.clear();
        Release out;
        out.tag = "untouched";
        const bool ok = ParseRelease(b.json, out, error);
        CHECK(!ok, "accepted: %s", b.json);
        CHECK(error.find(b.why) != std::string::npos, "%s: '%s', expected '%s'", b.json, error.c_str(), b.why);
        CHECK(out.tag == "untouched", "a refused answer leaves the release alone: %s", b.json);
    }
    // Nesting: 64 levels are fine, 65 aren't (and a deep one can't overflow the stack).
    auto nested = [](int levels) {
        std::string json = "{\"a\":";
        for (int i = 1; i < levels; i++)
            json += i % 2 ? "[" : "{\"b\":";
        json += "0";
        for (int i = levels - 1; i >= 1; i--)
            json += i % 2 ? "]" : "}";
        return json + "}";
    };
    CHECK(ParseRelease(nested(64), r, error), "64 levels: %s", error.c_str());
    error.clear();
    CHECK(!ParseRelease(nested(65), r, error) && error.find("deeper than 64") != std::string::npos, "65 levels: '%s'", error.c_str());
    error.clear();
    CHECK(!ParseRelease(std::string(100000, '['), r, error), "100,000 open brackets");
    CHECK(error.find("not a JSON object") != std::string::npos, "%s", error.c_str());
    error.clear();
    CHECK(!ParseRelease("{\"a\":" + std::string(100000, '['), r, error) && error.find("deeper than 64") != std::string::npos,
        "100,000 nested arrays: '%s'", error.c_str());
}

static void Http()
{
    HttpResponse h;
    // curl --dump-header - on HTTP/2, a proxy's CONNECT first.
    std::string raw = "HTTP/1.1 200 Connection established\r\n\r\n"
                      "HTTP/2 403 \r\nContent-Type: application/json\r\nX-RateLimit-Remaining: 0\r\nx-ratelimit-reset:  1700000600 \r\n\r\n"
                      "{\"message\":\"API rate limit exceeded\"}";
    CHECK(ParseHttpResponse(raw, h), "curl's output");
    CHECK(h.status == 403, "the last block's status: %d", h.status);
    CHECK(Header(h, "x-ratelimit-remaining") == "0" && Header(h, "X-RATELIMIT-RESET") == "1700000600", "headers, any case, trimmed");
    CHECK(Header(h, "content-length").empty(), "an absent header");
    CHECK(h.body == "{\"message\":\"API rate limit exceeded\"}", "body: '%s'", h.body.c_str());
    CHECK(ParseHttpResponse("HTTP/1.1 200 OK\nA: b\n\n{}\n", h) && h.status == 200 && h.body == "{}\n" && Header(h, "a") == "b",
        "LF line ends (a hand-written canned file)");
    CHECK(ParseHttpResponse("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 204 No Content\r\n\r\n", h) && h.status == 204 && h.body.empty(),
        "an interim answer, then no body");
    for (const char* bad : { "", "{}", "HTTP/1.1 200 OK\r\nA: b\r\n", "HTTP/1.1 2000 OK\r\n\r\n", "HTTP/1.1 20 OK\r\n\r\n", "HTTP/1.1\r\n\r\n",
             "HTTP/1.1 abc\r\n\r\n", "HTTP/1.1 200 OK\r\nno colon\r\n\r\n", "HTTP/1.1 200 OK\r\n: empty name\r\n\r\n" })
        CHECK(!ParseHttpResponse(bad, h), "'%s' isn't an answer", bad);
}

static std::string Dump(const Notes& notes)
{
    std::string out;
    for (const auto& line : notes.lines)
    {
        out += line.kind == K::Heading ? "H:" : line.kind == K::Bullet ? "B:" : line.kind == K::Gap ? "~" : "T:";
        out += line.text;
        out += "|";
    }
    return out;
}

static void NotesCleanup()
{
    const char* md =
        "<!-- release-template: keep this -->\r\n"
        "## What's new in **v0.1.2**\r\n"
        "\r\n"
        "- The Steam Deck now *fills* its 16:10 screen ([#12](https://github.com/x/y/issues/12)).\r\n"
        "  It costs nothing: `kTallMinStretch` is __1.01__.\r\n"
        "* Sunlit roads ~~dark~~ fixed! ![shot](https://x/y.png)\r\n"
        "+ [ ] a task\n"
        "- [x] done task\n"
        "\n"
        "1. Unpack over `~/Games/SpeedBreaker`.\n"
        "2) Keep snake_case_names, 2 * 3 and a_b *as they are*.\n"
        "\n"
        "Setext heading\n"
        "==============\n"
        "> Quoted **text** &amp; an entity &#169; &#x41; &bogus; &\n"
        "Line with <b>tags</b> and a<br>break, <https://example.com/a>, and \\*escaped\\*.\n"
        "\n"
        "<details><summary>Checksums</summary>\n"
        "\n"
        "| File | SHA-256 |\n"
        "|---|:---:|\n"
        "| SpeedBreaker-macos.dmg | `abc123` |\n"
        "</details>\n"
        "\n"
        "```sh\n"
        "tar -xf *.tar.xz   # stays as **it** is\n"
        "```\n"
        "\n"
        "---\n"
        "[ref]: https://example.com/ref\n"
        "See [the page][ref]. Ctrl\x01" "chars\x7F gone, bidi \xE2\x80\xAE" "kept out\xE2\x80\xAC, \xC2\xA0nbsp.\n"
        "### Closing hashes ###\n"
        "#not a heading\n";
    Notes notes = CleanNotes(md);
    const std::string got = Dump(notes);
    const std::string want =
        "H:What's new in v0.1.2|~|"
        "B:The Steam Deck now fills its 16:10 screen (#12). It costs nothing: kTallMinStretch is 1.01.|"
        "B:Sunlit roads dark fixed! shot|"  // the image's alt text, as GitHub shows it when it can't load
        "B:a task|B:done task|~|"
        "T:1. Unpack over ~/Games/SpeedBreaker.|"
        "T:2. Keep snake_case_names, 2 * 3 and a_b as they are.|~|"
        "H:Setext heading|"
        "T:Quoted text & an entity \xC2\xA9 A &bogus; & Line with tags and a break, https://example.com/a, and *escaped*.|~|"
        "T:Checksums|~|"
        "T:File | SHA-256|T:SpeedBreaker-macos.dmg | abc123|~|"
        "T:tar -xf *.tar.xz # stays as **it** is|~|"
        "T:See the page. Ctrlchars gone, bidi kept out, nbsp.|"
        "H:Closing hashes|T:#not a heading|";
    CHECK(got == want, "\n    got  %s\n    want %s", got.c_str(), want.c_str());
    CHECK(!notes.truncated, "not truncated");

    // Caps: characters and lines, cut on a character boundary.
    std::string many;
    for (int i = 0; i < 400; i++)
        many += "- item " + std::to_string(i) + "\n";
    Notes capped = CleanNotes(many, 100000, 150);
    CHECK(capped.lines.size() == 150 && capped.truncated, "150 lines at most: %zu", capped.lines.size());
    std::string euros;
    for (int i = 0; i < 100; i++)
        euros += "\xE2\x82\xAC";  // 3 bytes each
    Notes cut = CleanNotes(euros, 10);
    CHECK(cut.truncated && cut.lines.size() == 1 && cut.lines[0].text == "\xE2\x82\xAC\xE2\x82\xAC...", "cut whole: '%s'",
        cut.lines.empty() ? "" : cut.lines[0].text.c_str());
    Notes words = CleanNotes("alpha beta gamma delta epsilon zeta eta theta", 30);
    CHECK(words.truncated && words.lines.size() == 1 && words.lines[0].text == "alpha beta gamma delta...", "cut at a space: '%s'",
        words.lines.empty() ? "" : words.lines[0].text.c_str());
    Notes total = CleanNotes("first line is here\n\nsecond line is longer than the rest", 30);
    CHECK(total.truncated && Dump(total) == "T:first line is here|~|T:second li...|", "a total, not a per-line cap: '%s'",
        Dump(total).c_str());
    Notes joined = CleanNotes("one two three\nfour five six", 18);
    CHECK(joined.truncated && Dump(joined) == "T:one two three f...|", "a joined line is capped too: '%s'", Dump(joined).c_str());
    CHECK(CleanNotes("").lines.empty() && CleanNotes("\n\n<!-- x -->\n---\n").lines.empty(), "nothing to show");
    CHECK(Dump(CleanNotes("a\n<!-- never closed\nb")) == "T:a|", "an unclosed comment hides the rest");
    CHECK(Dump(CleanNotes("```\nno end\n- not a bullet")) == "T:no end|T:- not a bullet|", "an unclosed fence: code to the end");
    std::string big(200000, 'x');
    Notes huge = CleanNotes(big);
    CHECK(huge.truncated && huge.lines.size() == 1 && huge.lines[0].text.size() <= 6000, "a 200 KB body: %zu bytes",
        huge.lines.empty() ? 0 : huge.lines[0].text.size());
    CHECK(Dump(CleanNotes(std::string("a\xFF\xC3 b\xC3\xA9"))) == "T:a b\xC3\xA9|", "stray bytes dropped");
    // Unclosed marks on one long line stay linear (each looks a bounded way
    // ahead): four times the text takes about four times as long, not sixteen.
    for (char mark : { '[', '<', '`', '&', '(', '*', '_' })
    {
        auto time = [mark](size_t size) {
            double best = 1e9;
            for (int run = 0; run < 3; run++)
            {
                const auto start = std::chrono::steady_clock::now();
                Notes marks = CleanNotes(std::string(size, mark));
                best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
                CHECK(marks.lines.size() <= 1, "%zu bytes of '%c': %zu lines", size, mark, marks.lines.size());
            }
            return best;
        };
        const double small = time(16384), large = time(65536);
        CHECK(large < 8 * small + 20 && large < 2000, "'%c': 16 KB %.1f ms, 64 KB %.1f ms", mark, small, large);
    }

    CHECK(CleanLine("SpeedBreaker **v0.1.2**\n\tfinal", 120) == "SpeedBreaker v0.1.2 final", "a name: '%s'",
        CleanLine("SpeedBreaker **v0.1.2**\n\tfinal", 120).c_str());
    CHECK(CleanLine("abcdefghij", 8) == "abcde...", "a capped name: '%s'", CleanLine("abcdefghij", 8).c_str());
}

static Fetched Answer(int status, std::string body, std::vector<std::pair<std::string, std::string>> headers = {})
{
    Fetched f;
    f.transport = Transport::Ok;
    f.response.status = status;
    f.response.body = std::move(body);
    f.response.headers = std::move(headers);
    return f;
}

static std::string ReleaseJson(const char* tag, const char* extra = "")
{
    return std::string(R"({"tag_name":")") + tag +
           R"(","name":"SpeedBreaker )" + tag + R"(","html_url":"https://github.com/SpeedBreakerProject/speedbreaker/releases/tag/)" + tag +
           R"(","body":"### Fixed\n- Shadows","draft":false,"prerelease":false)" + extra + "}";
}

static void Outcomes()
{
    const int64_t now = 1700000000;
    Result r = Evaluate(Answer(200, ReleaseJson("v0.1.1")), "0.1.1", now);
    CHECK(r.outcome == Outcome::UpToDate && r.headline == "You're up to date (v0.1.1)", "up to date: '%s'", r.headline.c_str());
    CHECK(r.latest == "0.1.1" && r.log == "up to date: running v0.1.1, the latest release is v0.1.1", "log: '%s'", r.log.c_str());

    r = Evaluate(Answer(200, ReleaseJson("v0.1.2")), "0.1.1", now);
    CHECK(r.outcome == Outcome::Available && r.headline == "v0.1.2 is available" && r.message == "You have v0.1.1.", "available: '%s' '%s'",
        r.headline.c_str(), r.message.c_str());
    CHECK(r.url == "https://github.com/SpeedBreakerProject/speedbreaker/releases/tag/v0.1.2" && r.name == "SpeedBreaker v0.1.2", "page, name");
    CHECK(Dump(r.notes) == "H:Fixed|B:Shadows|", "notes: %s", Dump(r.notes).c_str());
    CHECK(r.log == "v0.1.2 is available (running v0.1.1): https://github.com/SpeedBreakerProject/speedbreaker/releases/tag/v0.1.2", "log: '%s'",
        r.log.c_str());

    r = Evaluate(Answer(200, ReleaseJson("v0.1.0")), "0.0.9", now);
    CHECK(r.outcome == Outcome::Available && r.headline == "v0.1.0 is available", "NFSMW_TEST_VERSION's case: '%s'", r.headline.c_str());
    r = Evaluate(Answer(200, ReleaseJson("v0.10.0")), "0.9.0", now);
    CHECK(r.outcome == Outcome::Available, "0.10.0 is newer than 0.9.0");
    r = Evaluate(Answer(200, ReleaseJson("v0.1.0")), "0.1.1", now);
    CHECK(r.outcome == Outcome::UpToDate && r.message == "The newest release is v0.1.0; this build is newer.", "ahead: '%s'", r.message.c_str());
    r = Evaluate(Answer(200, ReleaseJson("v0.2.0")), "0.2.0-rc.1", now);
    CHECK(r.outcome == Outcome::Available, "a release after its pre-release");

    // Drafts and pre-releases are never offered.
    r = Evaluate(Answer(200, ReleaseJson("v0.2.0-beta.1")), "0.1.1", now);
    CHECK(r.outcome == Outcome::UpToDate && r.latest.empty() && r.log.find("pre-release (ignored)") != std::string::npos,
        "a pre-release tag: '%s'", r.log.c_str());
    r = Evaluate(Answer(200, R"({"tag_name":"v0.9.0","prerelease":true})"), "0.1.1", now);
    CHECK(r.outcome == Outcome::UpToDate, "prerelease: true");
    r = Evaluate(Answer(200, R"({"tag_name":"v0.9.0","draft":true})"), "0.1.1", now);
    CHECK(r.outcome == Outcome::UpToDate, "draft: true");

    // The download page: only this project's releases.
    r = Evaluate(Answer(200, R"({"tag_name":"v0.9.0","html_url":"https://evil.example/releases/"})"), "0.1.1", now);
    CHECK(r.url == kReleasesPage, "another site: '%s'", r.url.c_str());
    for (const char* bad : { "javascript:alert(1)", "https://github.com/SpeedBreakerProject/speedbreakerx/releases/tag/v1",
             "https://github.com/SpeedBreakerProject/speedbreaker/releases/", "https://github.com/SpeedBreakerProject/speedbreaker/releases/../../x",
             "https://github.com/SpeedBreakerProject/speedbreaker/releases/tag/v1 x", "https://github.com/SpeedBreakerProject/speedbreaker/releases/tag/\"x",
             "https://github.com/SpeedBreakerProject/speedbreaker/releases/tag/\xC3\xA9", "" })
        CHECK(SafePageUrl(bad) == kReleasesPage, "'%s' isn't used", bad);
    CHECK(SafePageUrl("https://github.com/SpeedBreakerProject/speedbreaker/releases/tag/v0.1.2") ==
              "https://github.com/SpeedBreakerProject/speedbreaker/releases/tag/v0.1.2",
        "the release's page is");

    // Rate limits: GitHub's 60 an hour.
    r = Evaluate(Answer(403, R"({"message":"API rate limit exceeded for 1.2.3.4."})",
                     { { "x-ratelimit-remaining", "0" }, { "x-ratelimit-reset", std::to_string(now + 1381) } }),
        "0.1.1", now);
    CHECK(r.outcome == Outcome::RateLimited && r.headline == "Couldn't check for updates", "rate-limited");
    CHECK(r.message == "GitHub's limit of 60 checks an hour is used up. Try again in 24 minutes.", "'%s'",
        r.message.c_str());
    CHECK(r.log == "check failed: rate-limited (HTTP 403, try again in 24 minutes)", "'%s'", r.log.c_str());
    r = Evaluate(Answer(403, "{}", { { "X-RateLimit-Remaining", "0" }, { "X-RateLimit-Reset", std::to_string(now + 60) } }), "0.1.1", now);
    CHECK(r.outcome == Outcome::RateLimited && r.message.ends_with("Try again in 1 minute."), "the header alone says so: '%s'", r.message.c_str());
    r = Evaluate(Answer(429, "", { { "retry-after", "60" } }), "0.1.1", now);
    CHECK(r.outcome == Outcome::RateLimited && r.message.ends_with("Try again in 1 minute."), "429: '%s'", r.message.c_str());
    r = Evaluate(Answer(403, R"({"message":"API rate limit exceeded"})", { { "x-ratelimit-reset", "garbage" } }), "0.1.1", now);
    CHECK(r.outcome == Outcome::RateLimited && r.message.ends_with("Try again later."), "no reset time: '%s'", r.message.c_str());
    r = Evaluate(Answer(403, R"({"message":"Forbidden"})", { { "x-ratelimit-remaining", "59" } }), "0.1.1", now);
    CHECK(r.outcome == Outcome::ServerError && r.message == "GitHub answered with an error (HTTP 403). Try again later.", "a 403 that isn't a limit");
    CHECK(r.log == "check failed: HTTP 403: Forbidden", "'%s'", r.log.c_str());
    r = Evaluate(Answer(404, R"({"message":"Not Found"})"), "0.1.1", now);
    CHECK(r.outcome == Outcome::ServerError && r.message.starts_with("GitHub found no release"), "404");
    r = Evaluate(Answer(502, "<html>Bad gateway</html>"), "0.1.1", now);
    CHECK(r.outcome == Outcome::ServerError && r.log == "check failed: HTTP 502", "502: '%s'", r.log.c_str());

    // Answers that can't be read.
    r = Evaluate(Answer(200, "{\"tag_name\": \"v0.1.2\""), "0.1.1", now);
    CHECK(r.outcome == Outcome::Unreadable && r.message == "GitHub's answer couldn't be read. Try again later.", "truncated JSON");
    CHECK(r.log.starts_with("check failed: unreadable answer: a missing ',' or '}' at byte"), "'%s'", r.log.c_str());
    r = Evaluate(Answer(200, "{}"), "0.1.1", now);
    CHECK(r.outcome == Outcome::Unreadable && r.log == "check failed: unreadable answer: no tag_name", "'%s'", r.log.c_str());
    r = Evaluate(Answer(200, R"({"tag_name":"latest\u0007"})"), "0.1.1", now);
    CHECK(r.outcome == Outcome::Unreadable && r.log == "check failed: unreadable answer: the tag \"latest\" isn't a version", "'%s'",
        r.log.c_str());
    r = Evaluate(Answer(200, ReleaseJson("v0.1.2")), "0.1.1-dirty+x y", now);
    CHECK(r.outcome == Outcome::Unreadable, "this build's own version doesn't parse");

    // The transport's failures: one line each.
    struct T { Transport t; Outcome o; const char* message; const char* log; };
    const T transports[] = {
        { Transport::Offline, Outcome::Offline, "No connection to GitHub. Check that this device is online.",
            "check failed: offline (curl exit 6)" },
        { Transport::Timeout, Outcome::Timeout, "GitHub didn't answer within 10 seconds. Try again later.", "check failed: timed out (curl exit 6)" },
        { Transport::Secure, Outcome::Offline, "Couldn't connect to GitHub securely. Check the date and time.",
            "check failed: secure connection failed (curl exit 6)" },
        { Transport::TooLarge, Outcome::Unreadable, "GitHub's answer was too large to read. Try again later.",
            "check failed: answer too large (curl exit 6)" },
        { Transport::Garbled, Outcome::Unreadable, "GitHub's answer couldn't be read. Try again later.", "check failed: unreadable answer (curl exit 6)" },
        { Transport::Unavailable, Outcome::Unavailable, "curl exit 6", "check failed: not available (curl exit 6)" },
        { Transport::Failed, Outcome::Offline, "The check couldn't be made (curl exit 6). Try again later.", "check failed: failed (curl exit 6)" },
    };
    for (const T& t : transports)
    {
        Fetched f;
        f.transport = t.t;
        f.detail = "curl exit 6";
        r = Evaluate(f, "0.1.1", now);
        CHECK(r.outcome == t.o && r.message == t.message && r.log == t.log && r.headline == "Couldn't check for updates", "transport %d: '%s' / '%s'",
            int(t.t), r.message.c_str(), r.log.c_str());
        CHECK(r.message.find('\n') == std::string::npos && r.url == kReleasesPage, "one line, the releases page");
    }
}

int main()
{
    Versions();
    Json();
    Http();
    NotesCleanup();
    Outcomes();
    printf("%s (%d failures)\n", g_failures ? "FAILED" : "passed", g_failures);
    return g_failures ? 1 : 0;
}
