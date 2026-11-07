// Tests for update/check.h: Check for Updates' logic. Versions (Semantic
// Versioning), release tags and the Windows beta order, the strict JSON
// reader for GitHub's release and list of releases, this device's file and
// whether a release is complete for it, the HTTP answer as curl prints it,
// the release notes as plain lines, the result of every outcome, and every
// row of update-now/DESIGN.md's table "Which version is newer". From the
// repo root:
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
  "assets": [ { "name": "SpeedBreaker-macos.dmg", "body": "no", "size": 123, "state": "uploaded",
                "uploader": { "name": "not the asset", "size": 9, "state": "x" }, "digest": "sha256:ab", "label": null },
              {}, { "name": "SpeedBreaker-macos.dmg.sha256", "size": 89, "state": "open", "digest": null } ],
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
    CHECK(r.assets.size() == 3, "three assets: %zu", r.assets.size());
    if (r.assets.size() == 3)
    {
        CHECK(r.assets[0].name == "SpeedBreaker-macos.dmg" && r.assets[0].size == 123 && r.assets[0].state == "uploaded" &&
                  r.assets[0].digest == "sha256:ab",
            "the first asset, not its uploader's members: %s %lld %s", r.assets[0].name.c_str(), (long long)r.assets[0].size,
            r.assets[0].state.c_str());
        CHECK(r.assets[1].name.empty() && r.assets[1].size == -1 && r.assets[1].state.empty(), "an empty asset");
        CHECK(r.assets[2].state == "open" && r.assets[2].digest.empty(), "a null digest, an open state");
    }

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
        // The assets: an array of objects, each member read of the right type, once.
        { "{\"assets\":{}}", "\"assets\" of the wrong type" },
        { "{\"assets\":null}", "\"assets\" of the wrong type" },
        { "{\"assets\":[],\"assets\":[]}", "a second \"assets\"" },
        { "{\"assets\":[null]}", "an asset that isn't an object" },
        { "{\"assets\":[{},[]]}", "an asset that isn't an object" },
        { "{\"assets\":[{}],}", "a missing member name" },
        { "{\"assets\":[{} {}]}", "a missing ',' or ']'" },
        { "{\"assets\":[{\"name\":5}]}", "an asset's \"name\" of the wrong type" },
        { "{\"assets\":[{\"name\":null}]}", "an asset's \"name\" of the wrong type" },
        { "{\"assets\":[{\"state\":true}]}", "an asset's \"state\" of the wrong type" },
        { "{\"assets\":[{\"size\":\"12\"}]}", "an asset's \"size\" of the wrong type" },
        { "{\"assets\":[{\"size\":-1}]}", "an asset's \"size\" of the wrong type" },
        { "{\"assets\":[{\"size\":1.5}]}", "an asset's \"size\" of the wrong type" },
        { "{\"assets\":[{\"size\":1e3}]}", "an asset's \"size\" of the wrong type" },
        { "{\"assets\":[{\"size\":012}]}", "a missing ',' or '}'" },
        { "{\"assets\":[{\"size\":99999999999999999999}]}", "an asset's \"size\" of the wrong type" },
        { "{\"assets\":[{\"digest\":7}]}", "an asset's \"digest\" of the wrong type" },
        { "{\"assets\":[{\"name\":\"a\",\"name\":\"b\"}]}", "an asset's second \"name\"" },
        { "{\"assets\":[{\"name\":\"a\"]}", "a missing ',' or '}'" },
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
    CHECK(r.log == "v0.1.2 is available (running v0.1.1): https://github.com/SpeedBreakerProject/speedbreaker/releases/tag/v0.1.2; "
                   "not complete for this device (no file for this device); a development build",
        "log: '%s'", r.log.c_str());
    CHECK(r.latestTag == "v0.1.2" && r.label == "v0.1.2" && !r.complete && r.why == NoUpdateNow::DevBuild, "tag, label, why");

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


// --- Release tags and the Windows order ---

static ReleaseTag Tag(const char* text)
{
    ReleaseTag t;
    if (!ParseReleaseTag(text, t))
        printf("  (test) '%s' isn't a tag\n", text);
    return t;
}

static int TagCmp(const char* a, const char* b)
{
    ReleaseTag x, y;
    if (!ParseReleaseTag(a, x) || !ParseReleaseTag(b, y))
        return 99;
    return CompareReleaseTags(x, y);
}

static void Tags()
{
    ReleaseTag t;
    CHECK(ParseReleaseTag("v0.2.4", t) && !t.windowsBeta && t.beta == 0 && FormatSemver(t.version) == "0.2.4", "v0.2.4");
    CHECK(ParseReleaseTag("v0.2.3-windows-beta.6", t) && t.windowsBeta && t.beta == 6 && FormatSemver(t.version) == "0.2.3",
        "v0.2.3-windows-beta.6 (the running Windows build)");
    CHECK(ParseReleaseTag("v10.0.0-windows-beta.123", t) && t.beta == 123, "big numbers");
    CHECK(FormatReleaseTag(Tag("v0.2.3-windows-beta.6")) == "v0.2.3-windows-beta.6" && FormatReleaseTag(Tag("v0.2.4")) == "v0.2.4", "format");
    CHECK(ReleaseLabel(Tag("v0.2.3-windows-beta.6")) == "v0.2.3 beta 6" && ReleaseLabel(Tag("v0.2.5")) == "v0.2.5",
        "labels: '%s'", ReleaseLabel(Tag("v0.2.3-windows-beta.6")).c_str());
    // Odd tags are never releases: the one that once existed, and its kin.
    for (const char* odd : { "v0.2.0-test-issue67", "0.2.4", "V0.2.4", "vv0.2.4", "v0.2", "v0.2.4.1", "v0.2.4-rc.1", "v0.2.4+5",
             "v0.2.4-windows-beta", "v0.2.4-windows-beta.", "v0.2.4-windows-beta.0", "v0.2.4-windows-beta.07", "v0.2.4-windows-beta.1.2",
             "v0.2.4-windows-beta.x", "v0.2.4-Windows-beta.1", "v0.2.4-windows-beta.1+x", "v0.2.4-linux-beta.1", "v01.2.4", "v0.2.4 ",
             " v0.2.4", "v0.2.4-windows-beta.-1", "", "v", "latest" })
        CHECK(!ParseReleaseTag(odd, t), "'%s' isn't a release tag", odd);

    // Version, then beta number; a version's release after all of its betas.
    const char* order[] = { "v0.2.2", "v0.2.3-windows-beta.5", "v0.2.3-windows-beta.6", "v0.2.3", "v0.2.4-windows-beta.2",
        "v0.2.4-windows-beta.7", "v0.2.4-windows-beta.10", "v0.2.4", "v0.2.5-windows-beta.1", "v0.10.0-windows-beta.1", "v1.0.0" };
    for (size_t i = 0; i + 1 < std::size(order); i++)
        for (size_t j = i + 1; j < std::size(order); j++)
        {
            CHECK(TagCmp(order[i], order[j]) == -1, "%s < %s", order[i], order[j]);
            CHECK(TagCmp(order[j], order[i]) == 1, "%s > %s", order[j], order[i]);
        }
    CHECK(TagCmp("v0.2.4-windows-beta.7", "v0.2.4-windows-beta.7") == 0 && TagCmp("v0.2.4", "v0.2.4") == 0, "equal");
    // Where plain Semantic Versioning goes wrong for the betas.
    CHECK(Cmp("0.2.4-windows-beta.10", "0.2.4-windows-beta.9") == 1, "SemVer compares the beta numbers as numbers too");
    CHECK(TagCmp("v0.2.4-windows-beta.10", "v0.2.4-windows-beta.9") == 1, "beta 10 after beta 9");
}

// --- This device's file, and complete releases ---

static const Device kMac{ Platform::MacOS, Arch::Arm64, "SpeedBreaker" };
static const Device kDeck{ Platform::Linux, Arch::X86_64, "SpeedBreaker" };
static const Device kFrame{ Platform::Linux, Arch::Arm64, "SpeedBreaker" };
static const Device kWindows{ Platform::Windows, Arch::X86_64, "SpeedBreaker" };
static const Device kPhone{ Platform::IOS, Arch::Arm64, "SpeedBreaker" };

static std::string Names(const std::vector<std::string>& names)
{
    std::string out;
    for (const std::string& n : names)
        out += (out.empty() ? "" : " ") + n;
    return out;
}

static void DeviceFiles()
{
    struct Row { Device device; const char* version; bool beta; const char* names; };
    const Device carbonMac{ Platform::MacOS, Arch::Arm64, "SpeedBreaker-Carbon" };
    const Device carbonFrame{ Platform::Linux, Arch::Arm64, "SpeedBreaker-Carbon" };
    const Device carbonWindows{ Platform::Windows, Arch::X86_64, "SpeedBreaker-Carbon" };
    const Device carbonPhone{ Platform::IOS, Arch::Arm64, "SpeedBreaker-Carbon" };
    const Row rows[] = {
        { kMac, "0.2.4", false, "SpeedBreaker-macos.dmg" },
        { kDeck, "0.2.4", false, "SpeedBreaker-steamos-x86_64.tar.xz" },
        { kFrame, "0.2.4", false, "SpeedBreaker-steamos-arm64.tar.xz" },
        { kWindows, "0.2.4", true, "SpeedBreaker-0.2.4-windows-x64-beta.zip" },
        { kWindows, "0.3.0", false, "SpeedBreaker-0.3.0-windows-x64.zip SpeedBreaker-0.3.0-windows-x64-beta.zip" },
        { kPhone, "0.2.4", false, "SpeedBreaker-0.2.4-ios.ipa" },
        { carbonMac, "0.1.3", false, "SpeedBreaker-Carbon-macos.dmg" },
        { carbonFrame, "0.1.3", false, "SpeedBreaker-Carbon-steamos-arm64.tar.xz" },
        { carbonWindows, "0.1.3", true, "SpeedBreaker-Carbon-0.1.3-windows-x64-beta.zip" },
        { carbonPhone, "0.1.3", false, "SpeedBreaker-Carbon-0.1.3-ios.ipa" },
        // Nothing is made for these.
        { { Platform::MacOS, Arch::X86_64, "SpeedBreaker" }, "0.2.4", false, "" },  // an Intel Mac (a build from source)
        { { Platform::Windows, Arch::Arm64, "SpeedBreaker" }, "0.2.4", true, "" },
        { { Platform::Linux, Arch::Other, "SpeedBreaker" }, "0.2.4", false, "" },
        { { Platform::Other, Arch::X86_64, "SpeedBreaker" }, "0.2.4", false, "" },
        // A Windows beta's pre-release holds no file of the others.
        { kMac, "0.2.4", true, "" },
        { kDeck, "0.2.4", true, "" },
        { kPhone, "0.2.4", true, "" },
    };
    for (const Row& row : rows)
    {
        const std::string got = Names(DeviceAssetNames(row.device, row.version, row.beta));
        CHECK(got == row.names, "platform %d arch %d %s%s: '%s', expected '%s'", int(row.device.platform), int(row.device.arch), row.version,
            row.beta ? " beta" : "", got.c_str(), row.names);
    }
    const Device here = ThisDevice();
#if defined(__APPLE__) && defined(__aarch64__)
    CHECK(here.platform == Platform::MacOS && here.arch == Arch::Arm64, "this Mac");
#elif defined(__linux__) && defined(__x86_64__)
    CHECK(here.platform == Platform::Linux && here.arch == Arch::X86_64, "this x86-64 Linux");
#elif defined(__linux__) && defined(__aarch64__)
    CHECK(here.platform == Platform::Linux && here.arch == Arch::Arm64, "this arm64 Linux");
#elif defined(_WIN32)
    CHECK(here.platform == Platform::Windows && here.arch == Arch::X86_64, "this Windows PC");
#endif
    CHECK(here.prefix == SB_ASSET_PREFIX, "this game's prefix: '%.*s'", int(here.prefix.size()), here.prefix.data());
    CHECK(UpdateListName(kMac) == "SpeedBreaker-update.json" && UpdateListName(carbonMac) == "SpeedBreaker-Carbon-update.json", "update lists");
}

static Asset A(const char* name, int64_t size = 1000, const char* state = "uploaded")
{
    return Asset{ name, state, size, "" };
}

static void Complete()
{
    const std::vector<Asset> full = { A("SpeedBreaker-macos.dmg", 32000596), A("SpeedBreaker-macos.dmg.sha256", 89),
        A("SpeedBreaker-steamos-x86_64.tar.xz"), A("SpeedBreaker-steamos-x86_64.tar.xz.sha256", 101), A("SpeedBreaker-0.2.4-ios.ipa"),
        A("SpeedBreaker-0.2.4-ios.ipa.sha256"), A("SpeedBreaker-0.2.4-third-party-source.tar.xz") };
    Completeness c = CheckComplete(full, kMac, "0.2.4", false);
    CHECK(c.complete && c.asset == "SpeedBreaker-macos.dmg" && c.missing.empty() && !c.updateList, "the Mac's: %s", c.missing.c_str());
    c = CheckComplete(full, kDeck, "0.2.4", false);
    CHECK(c.complete && c.asset == "SpeedBreaker-steamos-x86_64.tar.xz", "the Deck's");
    c = CheckComplete(full, kPhone, "0.2.4", false);
    CHECK(c.complete && c.asset == "SpeedBreaker-0.2.4-ios.ipa", "the iPhone's");
    // A release without the Steam Frame's build.
    c = CheckComplete(full, kFrame, "0.2.4", false);
    CHECK(!c.complete && c.asset.empty() && c.missing == "SpeedBreaker-steamos-arm64.tar.xz missing, SpeedBreaker-steamos-arm64.tar.xz.sha256 missing",
        "no Frame build: '%s'", c.missing.c_str());
    // Still being uploaded: the file's state is "open", or its .sha256 isn't there yet.
    c = CheckComplete({ A("SpeedBreaker-macos.dmg", 32000596, "open"), A("SpeedBreaker-macos.dmg.sha256", 89) }, kMac, "0.2.4", false);
    CHECK(!c.complete && c.asset == "SpeedBreaker-macos.dmg" && c.missing == "SpeedBreaker-macos.dmg \"open\"", "uploading: '%s'", c.missing.c_str());
    c = CheckComplete({ A("SpeedBreaker-macos.dmg", 32000596) }, kMac, "0.2.4", false);
    CHECK(!c.complete && c.missing == "SpeedBreaker-macos.dmg.sha256 missing", "no .sha256: '%s'", c.missing.c_str());
    c = CheckComplete({ A("SpeedBreaker-macos.dmg", 0), A("SpeedBreaker-macos.dmg.sha256", 89) }, kMac, "0.2.4", false);
    CHECK(!c.complete && c.missing == "SpeedBreaker-macos.dmg empty", "an empty file: '%s'", c.missing.c_str());
    c = CheckComplete({ A("SpeedBreaker-macos.dmg", -1), A("SpeedBreaker-macos.dmg.sha256", 89) }, kMac, "0.2.4", false);
    CHECK(!c.complete, "no size given");
    c = CheckComplete({ A("SpeedBreaker-macos.dmg"), A("SpeedBreaker-macos.dmg.sha256", 89, "") }, kMac, "0.2.4", false);
    CHECK(!c.complete && c.missing == "SpeedBreaker-macos.dmg.sha256 in no state", "no state: '%s'", c.missing.c_str());
    // Names are exact: the versioned copy, a case change or another game's file aren't the Mac's.
    c = CheckComplete({ A("SpeedBreaker-0.2.4-macos.dmg"), A("SpeedBreaker-0.2.4-macos.dmg.sha256"), A("speedbreaker-macos.dmg"),
                          A("SpeedBreaker-macos.dmg.sha256"), A("SpeedBreaker-Carbon-macos.dmg") },
        kMac, "0.2.4", false);
    CHECK(!c.complete && c.missing == "SpeedBreaker-macos.dmg missing", "exact names: '%s'", c.missing.c_str());
    // The iPhone's file carries the release's version, not the running one's.
    c = CheckComplete(full, kPhone, "0.2.5", false);
    CHECK(!c.complete, "the IPA of another version");
    // Windows: a beta's zip; a release may carry either name.
    c = CheckComplete({ A("SpeedBreaker-0.2.4-windows-x64-beta.zip"), A("SpeedBreaker-0.2.4-windows-x64-beta.zip.sha256") }, kWindows, "0.2.4", true);
    CHECK(c.complete && c.asset == "SpeedBreaker-0.2.4-windows-x64-beta.zip", "a beta's zip");
    c = CheckComplete({ A("SpeedBreaker-0.3.0-windows-x64-beta.zip"), A("SpeedBreaker-0.3.0-windows-x64-beta.zip.sha256") }, kWindows, "0.3.0", false);
    CHECK(c.complete && c.asset == "SpeedBreaker-0.3.0-windows-x64-beta.zip", "a release's beta-named zip");
    c = CheckComplete({ A("SpeedBreaker-0.3.0-windows-x64.zip"), A("SpeedBreaker-0.3.0-windows-x64.zip.sha256") }, kWindows, "0.3.0", false);
    CHECK(c.complete && c.asset == "SpeedBreaker-0.3.0-windows-x64.zip", "a release's zip");
    c = CheckComplete({ A("SpeedBreaker-0.3.0-windows-x64.zip") }, kWindows, "0.3.0", false);
    CHECK(!c.complete && c.missing == "SpeedBreaker-0.3.0-windows-x64.zip.sha256 missing", "its .sha256 belongs to the same name: '%s'",
        c.missing.c_str());
    c = CheckComplete({}, { Platform::MacOS, Arch::X86_64, "SpeedBreaker" }, "0.2.4", false);
    CHECK(!c.complete && c.missing == "no file for this device", "no file for an Intel Mac");

    // Phase 2's hook: with the signed update list required, it and its signature must be there too.
    std::vector<Asset> listed = full;
    c = CheckComplete(listed, kMac, "0.2.4", false, true);
    CHECK(!c.complete && c.missing == "SpeedBreaker-update.json missing, SpeedBreaker-update.json.minisig missing", "the list required: '%s'",
        c.missing.c_str());
    listed.push_back(A("SpeedBreaker-update.json", 2000));
    c = CheckComplete(listed, kMac, "0.2.4", false, true);
    CHECK(!c.complete && c.missing == "SpeedBreaker-update.json.minisig missing" && !c.updateList, "the signature too: '%s'", c.missing.c_str());
    listed.push_back(A("SpeedBreaker-update.json.minisig", 300));
    c = CheckComplete(listed, kMac, "0.2.4", false, true);
    CHECK(c.complete && c.updateList, "the list and its signature");
    CHECK(CheckComplete(listed, kMac, "0.2.4", false).updateList, "seen (for the log) when not required");
    CHECK(!kUpdateListRequired, "Phase 1: the list isn't required yet (Phase 2 turns this on with the release side)");
}

// --- Answers with assets, lists of releases, and the design's table ---

struct Rel
{
    const char* tag;
    bool prerelease = false;
    std::vector<const char*> files = {};  // each with its .sha256
    bool draft = false;
};

static std::string RelJson(const Rel& rel)
{
    std::string assets;
    for (const char* f : rel.files)
        for (std::string name : { std::string(f), std::string(f) + ".sha256" })
            assets += std::string(assets.empty() ? "" : ",") + R"({"name":")" + name +
                      R"(","state":"uploaded","size":4096,"digest":"sha256:00","uploader":{"login":"x"}})";
    // This game's repository (release_names.h), so the page is used as is in either game's build.
    return std::string(R"({"tag_name":")") + rel.tag + R"(","name":"SpeedBreaker )" + rel.tag +
           R"(","html_url":"https://github.com/)" SB_GITHUB_REPO R"(/releases/tag/)" + rel.tag +
           R"(","body":"## What's new\n- Things","draft":)" + (rel.draft ? "true" : "false") + R"(,"prerelease":)" +
           (rel.prerelease ? "true" : "false") + R"(,"assets":[)" + assets + "]}";
}

static std::string ListJson(const std::vector<Rel>& rels)
{
    std::string out = "[";
    for (const Rel& r : rels)
        out += (out.size() > 1 ? "," : "") + RelJson(r);
    return out + "]";
}

static Running Run(Device device, const char* version, const char* tag)
{
    Running r;
    r.device = device;
    r.version = version;
    r.tag = tag;
    r.devBuild = !*tag;
    return r;
}

static const char* kMacFiles[] = { "SpeedBreaker-macos.dmg", "SpeedBreaker-steamos-x86_64.tar.xz", "SpeedBreaker-steamos-arm64.tar.xz" };

static Rel Stable(const char* tag, bool windowsZip = false)
{
    Rel r{ tag };
    r.files = { kMacFiles[0], kMacFiles[1], kMacFiles[2] };
    if (windowsZip)
    {
        static std::string zip;  // one at a time in these tests
        zip = std::string("SpeedBreaker-") + (tag + 1) + "-windows-x64-beta.zip";
        r.files.push_back(zip.c_str());
    }
    return r;
}

static Rel Beta(const char* tag, const char* version)
{
    static std::string zips[8];
    static int next = 0;
    std::string& zip = zips[next++ % 8];
    zip = std::string("SpeedBreaker-") + version + "-windows-x64-beta.zip";
    Rel r{ tag, true };
    r.files = { zip.c_str() };
    return r;
}

static void DesignTable()
{
    const int64_t now = 1700000000;
    Result r;
    // Row 1: Windows beta 3 (v0.2.1-windows-beta.3, built before this feature) can't check at all: the
    // stub's answer. (Its code is gone; what it said was Transport::Unavailable.)
    {
        Fetched f;
        f.transport = Transport::Unavailable;
        f.detail = "Checking for updates isn't available on Windows yet. The releases page has every version.";
        r = Evaluate(f, Run(kWindows, "0.2.1", ""), now);
        CHECK(r.outcome == Outcome::Unavailable, "row 1: the stub can't check");
    }
    // Read every version one step later than the design's v0.2.2 examples: the running Windows build is
    // v0.2.3-windows-beta.6.
    const Running beta6 = Run(kWindows, "0.2.3", "v0.2.3-windows-beta.6");
    // Row 2: the latest stable v0.2.3 has no Windows zip: up to date, not a Windows release.
    r = Evaluate(Answer(200, ListJson({ Stable("v0.2.3"), Beta("v0.2.3-windows-beta.6", "0.2.3") })), beta6, now);
    CHECK(r.outcome == Outcome::UpToDate && r.headline == "You're up to date (v0.2.3 beta 6)", "row 2: '%s' / %s", r.headline.c_str(),
        r.log.c_str());
    CHECK(r.log == "up to date: running v0.2.3-windows-beta.6, the newest Windows release is v0.2.3-windows-beta.6 (1 of 2 not Windows releases: ignored)",
        "row 2 log: '%s'", r.log.c_str());
    r = Evaluate(Answer(200, ListJson({ Stable("v0.2.4") })), beta6, now);
    CHECK(r.outcome == Outcome::UpToDate, "row 2: a newer stable release without a Windows zip either: %s", r.log.c_str());
    // Row 3: the same version, a newer beta: a Windows fix.
    r = Evaluate(Answer(200, ListJson({ Stable("v0.2.3"), Beta("v0.2.3-windows-beta.7", "0.2.3"), Beta("v0.2.3-windows-beta.6", "0.2.3") })),
        beta6, now);
    CHECK(r.outcome == Outcome::Available && r.latestTag == "v0.2.3-windows-beta.7" && r.headline == "v0.2.3 beta 7 is available" &&
              r.message == "You have v0.2.3 beta 6." && r.complete && r.asset == "SpeedBreaker-0.2.3-windows-x64-beta.zip" &&
              r.why == NoUpdateNow::NotYet,
        "row 3: '%s' '%s' %s", r.headline.c_str(), r.message.c_str(), r.log.c_str());
    CHECK(r.url == "https://github.com/" SB_GITHUB_REPO "/releases/tag/v0.2.3-windows-beta.7" && r.label == "v0.2.3 beta 7",
        "row 3: its page '%s'", r.url.c_str());
    // Row 4: a newer version's beta.
    r = Evaluate(Answer(200, ListJson({ Beta("v0.2.4-windows-beta.7", "0.2.4"), Stable("v0.2.3"), Beta("v0.2.3-windows-beta.6", "0.2.3") })),
        beta6, now);
    CHECK(r.outcome == Outcome::Available && r.latestTag == "v0.2.4-windows-beta.7" && r.latest == "0.2.4", "row 4: %s", r.log.c_str());
    // Row 5: a stable release with a Windows zip (Windows out of beta), after the betas of its version.
    r = Evaluate(Answer(200, ListJson({ Stable("v0.2.4", true), Beta("v0.2.4-windows-beta.9", "0.2.4"), Beta("v0.2.3-windows-beta.6", "0.2.3") })),
        beta6, now);
    CHECK(r.outcome == Outcome::Available && r.latestTag == "v0.2.4" && r.label == "v0.2.4" && r.complete &&
              r.asset == "SpeedBreaker-0.2.4-windows-x64-beta.zip",
        "row 5: %s", r.log.c_str());
    // Row 6: a Mac sees nothing of a Windows pre-release ("latest" never returns one; checked anyway).
    const Running mac = Run(kMac, "0.2.3", "v0.2.3");
    Rel winOnly = Beta("v0.2.4-windows-beta.7", "0.2.4");
    r = Evaluate(Answer(200, RelJson(winOnly)), mac, now);
    CHECK(r.outcome == Outcome::UpToDate && r.latest.empty(), "row 6: %s", r.log.c_str());
    winOnly.prerelease = false;  // even published by mistake as a release
    r = Evaluate(Answer(200, RelJson(winOnly)), mac, now);
    CHECK(r.outcome == Outcome::UpToDate, "row 6, not marked pre-release: %s", r.log.c_str());
    // Row 7: Mac v0.2.3, latest v0.2.4: offered.
    r = Evaluate(Answer(200, RelJson(Stable("v0.2.4"))), mac, now);
    CHECK(r.outcome == Outcome::Available && r.latestTag == "v0.2.4" && r.complete && r.asset == "SpeedBreaker-macos.dmg" &&
              r.why == NoUpdateNow::NotYet && r.headline == "v0.2.4 is available" && r.message == "You have v0.2.3.",
        "row 7: %s", r.log.c_str());
    CHECK(r.log == "v0.2.4 is available (running v0.2.3): https://github.com/" SB_GITHUB_REPO "/releases/tag/v0.2.4; "
                   "complete for this device (SpeedBreaker-macos.dmg); no update list yet",
        "row 7 log: '%s'", r.log.c_str());
    // Row 8: SteamOS v0.2.4, the latest moved back to v0.2.3 by hand: up to date, never a downgrade.
    r = Evaluate(Answer(200, RelJson(Stable("v0.2.3"))), Run(kDeck, "0.2.4", "v0.2.4"), now);
    CHECK(r.outcome == Outcome::UpToDate && r.message == "The newest release is v0.2.3; this build is newer.", "row 8: %s", r.log.c_str());
    // Row 9: a development, test or dirty build (no tag): the link, no Update Now.
    r = Evaluate(Answer(200, RelJson(Stable("v0.2.4"))), Run(kMac, "0.2.3", ""), now);
    CHECK(r.outcome == Outcome::Available && r.why == NoUpdateNow::DevBuild &&
              r.whyLine == "This is a development build, so it doesn't update itself: get v0.2.4 from the download page.",
        "row 9: '%s'", r.whyLine.c_str());
    Running dirty = Run(kMac, "0.2.3", "v0.2.3");
    dirty.devBuild = true;  // tagged, but built with uncommitted changes
    r = Evaluate(Answer(200, RelJson(Stable("v0.2.4"))), dirty, now);
    CHECK(r.why == NoUpdateNow::DevBuild && r.log.ends_with("; a development build"), "row 9, dirty: %s", r.log.c_str());

    // "Complete for this device": a release without the Frame's build is still available, with the link only.
    Rel noFrame = Stable("v0.2.4");
    noFrame.files.pop_back();
    r = Evaluate(Answer(200, RelJson(noFrame)), Run(kFrame, "0.2.3", "v0.2.3"), now);
    CHECK(r.outcome == Outcome::Available && !r.complete && r.why == NoUpdateNow::Incomplete &&
              r.whyLine == "This device's download isn't in the release yet: see the download page.",
        "no Frame build: %s", r.log.c_str());
    r = Evaluate(Answer(200, RelJson(noFrame)), Run(kDeck, "0.2.3", "v0.2.3"), now);
    CHECK(r.complete, "the same release is complete for the Deck");
    // iPhone and iPad: a notice and a link.
    Rel withIpa = Stable("v0.2.4");
    withIpa.files.push_back("SpeedBreaker-0.2.4-ios.ipa");
    r = Evaluate(Answer(200, RelJson(withIpa)), Run(kPhone, "0.2.3", "v0.2.3"), now);
    CHECK(r.outcome == Outcome::Available && r.complete && r.why == NoUpdateNow::Phone &&
              r.whyLine == "Install it with the app you used to install " SB_APP_NAME ", over this one. Deleting the app deletes its saves.",
        "iPhone: '%s'", r.whyLine.c_str());
}

static void WindowsLists()
{
    const int64_t now = 1700000000;
    const Running beta6 = Run(kWindows, "0.2.3", "v0.2.3-windows-beta.6");
    Result r;
    // Never a downgrade, whatever order GitHub lists them in.
    r = Evaluate(Answer(200, ListJson({ Beta("v0.2.3-windows-beta.5", "0.2.3"), Beta("v0.2.2-windows-beta.5", "0.2.2"), Stable("v0.2.2", true) })),
        beta6, now);
    CHECK(r.outcome == Outcome::UpToDate && r.message == "The newest Windows release is v0.2.3 beta 5; this build is newer.", "older betas: '%s'",
        r.message.c_str());
    // The newest wins wherever it is in the list.
    r = Evaluate(Answer(200, ListJson({ Beta("v0.2.3-windows-beta.7", "0.2.3"), Beta("v0.2.3-windows-beta.10", "0.2.3"),
                                     Beta("v0.2.3-windows-beta.8", "0.2.3") })),
        beta6, now);
    CHECK(r.latestTag == "v0.2.3-windows-beta.10", "beta 10 over 8 and 7: %s", r.latestTag.c_str());
    // Ignored: odd tags, drafts, a beta not marked pre-release, a stable one marked pre-release.
    Rel odd = Beta("v0.2.0-test-issue67", "0.2.0");
    Rel draft = Beta("v0.2.9-windows-beta.1", "0.2.9");
    draft.draft = true;
    Rel unmarked = Beta("v0.2.8-windows-beta.1", "0.2.8");
    unmarked.prerelease = false;
    Rel marked = Stable("v0.2.7", true);
    marked.prerelease = true;
    Rel oddWin = Beta("v0.2.6-windows-beta.01", "0.2.6");
    Rel rc = Beta("v0.2.5-rc.1", "0.2.5");
    r = Evaluate(Answer(200, ListJson({ odd, draft, unmarked, marked, oddWin, rc, Beta("v0.2.3-windows-beta.6", "0.2.3") })), beta6, now);
    CHECK(r.outcome == Outcome::UpToDate && r.log.ends_with("(6 of 7 not Windows releases: ignored)"), "odd ones ignored: %s", r.log.c_str());
    // A newer beta without its zip is still offered, as the link only.
    Rel bare{ "v0.2.4-windows-beta.7", true };
    r = Evaluate(Answer(200, ListJson({ bare })), beta6, now);
    CHECK(r.outcome == Outcome::Available && !r.complete && r.why == NoUpdateNow::Incomplete, "a beta without its zip: %s", r.log.c_str());
    // An empty list, a list of nothing Windows.
    r = Evaluate(Answer(200, "[]"), beta6, now);
    CHECK(r.outcome == Outcome::UpToDate && r.log == "up to date: running v0.2.3-windows-beta.6; no Windows release among the 0 newest", "empty: %s",
        r.log.c_str());
    // Untagged builds: a development build (its version before every beta of it), NFSMW_TEST_VERSION's.
    r = Evaluate(Answer(200, ListJson({ Beta("v0.2.3-windows-beta.6", "0.2.3") })), Run(kWindows, "0.2.3", ""), now);
    CHECK(r.outcome == Outcome::Available && r.why == NoUpdateNow::DevBuild, "a dev build of 0.2.3 sees 0.2.3's beta 6 (link only): %s",
        r.log.c_str());
    r = Evaluate(Answer(200, ListJson({ Beta("v0.2.3-windows-beta.6", "0.2.3") })), Run(kWindows, "0.2.3-windows-beta.5", ""), now);
    CHECK(r.outcome == Outcome::Available, "NFSMW_TEST_VERSION=0.2.3-windows-beta.5");
    r = Evaluate(Answer(200, ListJson({ Beta("v0.2.3-windows-beta.6", "0.2.3") })), Run(kWindows, "0.2.3-windows-beta.6", ""), now);
    CHECK(r.outcome == Outcome::UpToDate, "NFSMW_TEST_VERSION=0.2.3-windows-beta.6");
    r = Evaluate(Answer(200, ListJson({ Beta("v0.2.3-windows-beta.6", "0.2.3") })), Run(kWindows, "0.2.3-rc.1", ""), now);
    CHECK(r.outcome == Outcome::Unreadable, "a version of no Windows shape: %s", r.log.c_str());
    // A list answer that isn't one.
    for (const std::string& bad : { std::string("{}"), RelJson(Stable("v0.2.4")), std::string("[1]"), std::string("[{}"), std::string("[{},]"),
             std::string("[] []"), std::string(""), std::string("[null]") })
    {
        r = Evaluate(Answer(200, bad), beta6, now);
        CHECK(r.outcome == Outcome::Unreadable && r.headline == "Couldn't check for updates", "'%s' isn't a list: %s", bad.c_str(), r.log.c_str());
    }
    // The list's failures are the latest's: a rate limit, a 404.
    r = Evaluate(Answer(403, R"({"message":"API rate limit exceeded"})", { { "x-ratelimit-remaining", "0" } }), beta6, now);
    CHECK(r.outcome == Outcome::RateLimited, "rate-limited list");
    r = Evaluate(Answer(404, R"({"message":"Not Found"})"), beta6, now);
    CHECK(r.outcome == Outcome::ServerError, "404 list");
}

static void Lists()
{
    std::vector<Release> list;
    std::string error;
    CHECK(ParseReleases("[]", list, error) && list.empty(), "an empty list");
    CHECK(ParseReleases(" [ " + std::string(kRelease) + " , {\"tag_name\":\"v0.0.1\"} ] ", list, error) && list.size() == 2 &&
              list[0].tag == "v0.1.2" && list[0].assets.size() == 3 && list[1].tag == "v0.0.1" && list[1].assets.empty(),
        "two releases: %s", error.c_str());
    struct Bad { const char* json; const char* why; };
    const Bad bad[] = {
        { "", "empty" },
        { "{}", "not a JSON array" },
        { "[1]", "a release that isn't an object" },
        { "[[]]", "a release that isn't an object" },
        { "[{}", "a missing ',' or ']'" },
        { "[{},]", "a release that isn't an object" },
        { "[{}] x", "more after the JSON" },
        { "[{\"tag_name\":1}]", "\"tag_name\" of the wrong type" },
        { "[{\"assets\":[{\"size\":\"x\"}]}]", "an asset's \"size\" of the wrong type" },
    };
    for (const Bad& b : bad)
    {
        error.clear();
        list = { Release{} };
        CHECK(!ParseReleases(b.json, list, error) && error.find(b.why) != std::string::npos && list.size() == 1, "%s: '%s', expected '%s'", b.json,
            error.c_str(), b.why);
    }
    // Real-size: ten releases with many assets and long notes, as Windows' answer is (295 KB measured).
    std::string big = "[";
    for (int i = 0; i < 10; i++)
    {
        std::string assets;
        for (int a = 0; a < 40; a++)
            assets += std::string(a ? "," : "") + R"({"url":"https://api.github.com/x","id":1,"node_id":"RA","name":"f)" + std::to_string(a) +
                      R"(","label":null,"uploader":{"login":"u","id":2,"site_admin":false},"content_type":"application/zip","state":"uploaded","size":123456,"digest":"sha256:)" +
                      std::string(64, 'a') + R"(","download_count":7,"created_at":"2026-11-05T00:00:00Z"})";
        big += std::string(i ? "," : "") + R"({"tag_name":"v0.2.)" + std::to_string(i) + R"(","prerelease":false,"draft":false,"body":")" +
               std::string(12000, 'n') + R"(","assets":[)" + assets + "]}";
    }
    big += "]";
    CHECK(big.size() > 250000 && big.size() < kMaxResponseBytes, "%zu bytes", big.size());
    CHECK(ParseReleases(big, list, error) && list.size() == 10 && list[9].assets.size() == 40 && list[9].assets[39].size == 123456,
        "a real-size list: %s", error.c_str());
}

int main()
{
    Versions();
    Json();
    Http();
    NotesCleanup();
    Outcomes();
    Tags();
    DeviceFiles();
    Complete();
    Lists();
    DesignTable();
    WindowsLists();
    printf("%s (%d failures)\n", g_failures ? "FAILED" : "passed", g_failures);
    return g_failures ? 1 : 0;
}
