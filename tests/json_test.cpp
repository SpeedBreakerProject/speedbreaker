// Tests for update/json.h: the strict reader under the update check,
// and its tree. From the repo root:
//   clang++ -std=c++20 -O2 -Iruntime tests/json_test.cpp
//     runtime/update/json.cpp -o build/json_test
//   build/json_test
#include <update/json.h>

#include <cstdio>
#include <string>

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; \
    printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

using namespace update::json;

static void Tree()
{
    Value j;
    std::string error;
    const char* text =
        " {\"a\": [1, -2.5e3, null, true, false,"
        " {\"b\":\"\\u00e9\\ud83d\\ude00\\n\"}]} \n";
    CHECK(Parse(text, j, error), "%s", error.c_str());
    const Value* a = j.Member("a", error);
    CHECK(j.kind == Kind::Object && a && a->kind == Kind::Array && a->values.size() == 6,
        "tree");
    if (a && a->values.size() == 6)
    {
        const Value* b = a->values[5].Member("b", error);
        CHECK(b && b->text == "\xc3\xa9\xf0\x9f\x98\x80\n", "escapes");
        CHECK(a->values[1].kind == Kind::Number && a->values[1].text == "-2.5e3"
            && a->values[2].kind == Kind::Null && a->values[3].kind == Kind::True,
            "kinds");
    }
    CHECK(!j.Member("nope", error) && error.empty(), "absent");
    CHECK(Parse(R"({"a":1,"a":2})", j, error) && !j.Member("a", error)
        && error == "a second \"a\"", "%s", error.c_str());
    CHECK(Parse("\"x\"", j, error) && j.kind == Kind::String && j.text == "x", "scalar");
}

static void Errors()
{
    struct Case
    {
        const char* text;
        const char* error;
    };
    const Case cases[] = {
        { "", "a missing value at byte 0" },
        { "{", "a missing member name at byte 1" },
        { "{} x", "more after the JSON at byte 3" },
        { "[1,]", "an unexpected ']' at byte 3" },
        { "{\"a\":01}", "a missing ',' or '}' at byte 6" },
        { "{\"a\" 1}", "a missing ':' at byte 5" },
        { "\"\\ud800\"", "an unpaired surrogate at byte 7" },
        { "\"\\udc00\"", "an unpaired surrogate at byte 7" },
        { "\"\\u12zz\"", "a bad \\u escape at byte 5" },
        { "\"\\u12\"", "a short \\u escape at byte 3" },
        { "\"\\x\"", "a bad escape at byte 2" },
        { "tru", "an unknown word at byte 0" },
        { "-", "a bad number at byte 1" },
        { "1.", "a bad number at byte 2" },
        { "\"a\xff\"", "invalid UTF-8 at byte 2" },
        { "\"\xc0\x80\"", "invalid UTF-8 at byte 1" },
        { "\"\n\"", "a control character in a string at byte 1" },
        { "\"abc", "an unterminated string at byte 4" },
        { "\x01", "an unexpected byte 0x01 at byte 0" },
    };
    for (const Case& c : cases)
    {
        Value j;
        std::string error;
        CHECK(!Parse(c.text, j, error) && error == c.error, "'%s': %s", c.text,
            error.c_str());
    }
    Value j;
    std::string error;
    CHECK(!Parse(std::string(70, '[') + std::string(70, ']'), j, error)
        && error.starts_with("nesting deeper than 64 levels"), "%s", error.c_str());
    CHECK(Parse(std::string(64, '[') + std::string(64, ']'), j, error),
        "%s", error.c_str());
    CHECK(!Parse(std::string(65, '[') + std::string(65, ']'), j, error),
        "65 levels");
}

// The streaming side, as update/check.cpp uses it.
static void Streaming()
{
    Reader r{ R"({"keep": "x", "skip": {"deep": [1, {"z": null}]}, "n": 42})" };
    std::string keep, number;
    bool ok = r.Members(1, true, [&](std::string& name) {
        r.Space();
        if (name == "keep")
            return r.String(&keep);
        Kind kind;
        if (name == "n")
            return r.Value(1, &kind, &number) && kind == Kind::Number;
        return r.Value(1, nullptr, nullptr);
    });
    CHECK(ok && r.i == r.s.size() && keep == "x" && number == "42", "%s",
        r.error.c_str());

    Reader names{ R"({"a": 1, "b": 2})" };
    int count = 0;
    ok = names.Members(1, false, [&](std::string& name) {
        count += name.empty();
        return names.Value(1, nullptr, nullptr);
    });
    CHECK(ok && count == 2, "names not kept");

    Reader items{ "[1, 2, 3]" };
    count = 0;
    ok = items.Items(1, [&] {
        count++;
        return items.Number();
    });
    CHECK(ok && count == 3, "items");

    Reader first{ "[1 2]" };
    CHECK(!first.Items(1, [&] { return first.Number(); })
        && first.error == "a missing ',' or ']' at byte 3", "%s", first.error.c_str());
    first.Fail("later");
    CHECK(first.error == "a missing ',' or ']' at byte 3", "first error sticks");
}

static void Utf8()
{
    uint32_t cp = 0;
    CHECK(DecodeUtf8("\xf0\x9f\x98\x80", 0, cp) == 4 && cp == 0x1F600, "4 bytes");
    CHECK(DecodeUtf8("\xed\xa0\x80", 0, cp) == 0, "surrogate");
    CHECK(DecodeUtf8("\xe0\x80\x80", 0, cp) == 0, "overlong");
    CHECK(DecodeUtf8("\xf4\x90\x80\x80", 0, cp) == 0, "past U+10FFFF");
    CHECK(DecodeUtf8("\xc3", 0, cp) == 0, "cut short");
    std::string out;
    AppendUtf8(0xE9, out);
    AppendUtf8(0x1F600, out);
    CHECK(out == "\xc3\xa9\xf0\x9f\x98\x80", "append");
}

int main()
{
    Tree();
    Errors();
    Streaming();
    Utf8();
    printf("%s (%d failures)\n", g_failures ? "FAILED" : "passed", g_failures);
    return g_failures ? 1 : 0;
}
