// report::detail::ReplaceFolders (runtime/report/scrub.h), what ScrubHome
// does to the text of a bug report before it leaves the machine. Header
// only. From the repo root:
//   clang++ -std=c++20 -O2 -Iruntime tests/scrub_test.cpp -o build/scrub_test
//   build/scrub_test
// - POSIX (HOME as "~"): whole folders only, as since 2026-09-28: not
//   /home/deck2, not the tail of /mnt/home/deck; the longest folder first
//   (HOME and its real path).
// - Windows (the user's folder as %USERPROFILE%): the same rule, letters in
//   either case, \ and / alike, a backslash escaped (\\) too, the 8.3 name
//   as a folder of its own; a folder never matches inside a longer name.
#include <report/scrub.h>

#include <cstdio>
#include <string>

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

using report::detail::FolderName;
using report::detail::ReplaceFolders;

static void Expect(const std::string& text, const std::vector<FolderName>& folders, bool windows, const std::string& want)
{
    std::string got = ReplaceFolders(text, folders, windows);
    CHECK(got == want, "\n    in:   %s\n    got:  %s\n    want: %s", text.c_str(), got.c_str(), want.c_str());
}

int main()
{
    printf("1. POSIX: HOME as ~, whole folders only\n");
    const std::vector<FolderName> deck = { { "/home/deck", "~" } };
    Expect("[paths] data /home/deck/.local/share/speedbreaker", deck, false, "[paths] data ~/.local/share/speedbreaker");
    Expect("/home/deck", deck, false, "~");
    Expect("cd /home/deck; ls /home/deck2 /mnt/home/deck /home/deck-old", deck, false,
        "cd ~; ls /home/deck2 /mnt/home/deck /home/deck-old");
    Expect("/HOME/DECK/x", deck, false, "/HOME/DECK/x");  // byte for byte
    // HOME through a symlink and its real path: the longer one first.
    const std::vector<FolderName> both = { { "/home/deck", "~" }, { "/var/home/deck", "~" } };
    Expect("/var/home/deck/x and /home/deck/y", both, false, "~/x and ~/y");
    const std::vector<FolderName> mac = { { "/Users/player", "~" } };
    Expect("'/Users/player/Library/Application Support/SpeedBreaker' \"/Users/player\"", mac, false,
        "'~/Library/Application Support/SpeedBreaker' \"~\"");

    printf("2. Windows: the user's folder as %%USERPROFILE%%\n");
    const std::vector<FolderName> user = { { "C:\\Users\\Jose", "%USERPROFILE%" } };
    Expect("[log] session 1: C:\\Users\\Jose\\AppData\\Local\\SpeedBreaker\\logs\\a.log", user, true,
        "[log] session 1: %USERPROFILE%\\AppData\\Local\\SpeedBreaker\\logs\\a.log");
    Expect("c:\\users\\jose\\OneDrive\\Desktop\\x.zip", user, true, "%USERPROFILE%\\OneDrive\\Desktop\\x.zip");
    Expect("C:/Users/Jose/AppData/Local", user, true, "%USERPROFILE%/AppData/Local");
    Expect("path = \"C:\\\\Users\\\\Jose\\\\AppData\"", user, true, "path = \"%USERPROFILE%\\\\AppData\"");
    Expect("C:\\Users\\Jose", user, true, "%USERPROFILE%");
    Expect("\\\\?\\C:\\Users\\Jose\\x", user, true, "\\\\?\\%USERPROFILE%\\x");
    Expect("C:\\Users\\Josefina\\x C:\\Users\\Jose-2 D:\\C:\\Users\\Jose", user, true,
        "C:\\Users\\Josefina\\x C:\\Users\\Jose-2 D:\\%USERPROFILE%");
    Expect("XC:\\Users\\Jose\\x", user, true, "XC:\\Users\\Jose\\x");  // the tail of a longer name
    Expect("C:\\Users\\Jos", user, true, "C:\\Users\\Jos");
    Expect("C:\\Users\\Jose\\x and C:\\Users\\Jose\\y", user, true, "%USERPROFILE%\\x and %USERPROFILE%\\y");
    // The 8.3 name next to the long one.
    const std::vector<FolderName> shortToo = { { "C:\\Users\\Jose Maria", "%USERPROFILE%" }, { "C:\\Users\\JOSEMA~1", "%USERPROFILE%" } };
    Expect("TEMP=C:\\Users\\JOSEMA~1\\AppData\\Local\\Temp; data C:\\Users\\Jose Maria\\AppData", shortToo, true,
        "TEMP=%USERPROFILE%\\AppData\\Local\\Temp; data %USERPROFILE%\\AppData");
    // UTF-8 names are bytes like any other.
    const std::vector<FolderName> utf8 = { { "C:\\Users\\Jos\xC3\xA9", "%USERPROFILE%" } };
    Expect("C:\\Users\\Jos\xC3\xA9\\Desktop", utf8, true, "%USERPROFILE%\\Desktop");
    // Nothing to replace, an empty folder: the text as it was.
    Expect("no paths here", user, true, "no paths here");
    Expect("C:\\Users\\Jose\\x", { { "", "%USERPROFILE%" } }, true, "C:\\Users\\Jose\\x");

    printf(g_failures ? "%d failure(s)\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
