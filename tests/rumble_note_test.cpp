// SpeedBreaker tests. GPL-3.0-or-later (see COPYING).
//
// The one-time "no rumble over USB" note for an Xbox controller on a Mac
// (hid/rumble_note.h): when it shows (macOS, an Xbox pad by vendor or name,
// Apple's GameController route, not wireless, Vibration on, not dismissed)
// and when it doesn't (a DualSense wired, an Xbox pad over Bluetooth, other
// platforms); and the dismissal in settings.toml ([app] xbox_usb_rumble_note,
// user/settings.cpp as the game links it), in a scratch folder (the argument).
//
// Build by hand (from the repo root):
//   c++ -std=c++20 -O2 -UNDEBUG -I runtime tests/rumble_note_test.cpp runtime/user/settings.cpp \
//     -o build/rumble_note_test
//   build/rumble_note_test build/rumble_note_scratch
#include <hid/rumble_note.h>
#include <user/settings.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace
{
    int s_failures = 0;

#define CHECK(cond, ...)                                                    \
    do                                                                      \
    {                                                                       \
        if (!(cond))                                                        \
        {                                                                   \
            s_failures++;                                                   \
            fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
            fprintf(stderr, __VA_ARGS__);                                   \
            fprintf(stderr, "\n");                                          \
        }                                                                   \
    } while (0)

    using hid::RumbleNote;
    using Platform = RumbleNote::Platform;
    using Connection = RumbleNote::Connection;

    // A wired Xbox Series pad on a Mac as SDL logs it: GameController, connection unknown.
    RumbleNote::Inputs XboxUsbOnMac()
    {
        RumbleNote::Inputs in;
        in.platform = Platform::Mac;
        in.driver = 'm';
        in.connection = Connection::Unknown;
        in.vendor = 0x045E;
        in.name = "Xbox Series X Controller";
        return in;
    }

    void Expect(const char* what, const RumbleNote::Inputs& in, bool show)
    {
        const RumbleNote::Decision d = RumbleNote::Decide(in);
        printf("  %-52s %s (%s)\n", what, d.show ? "NOTE" : "no note", d.reason);
        CHECK(d.show == show, "%s: %s", what, d.reason);
    }

    void WhenItShows()
    {
        printf("when the note shows:\n");
        Expect("Xbox by USB on a Mac (connection unknown)", XboxUsbOnMac(), true);
        auto in = XboxUsbOnMac();
        in.connection = Connection::Wired;
        Expect("Xbox by USB on a Mac (reported wired)", in, true);
        in = XboxUsbOnMac();
        in.vendor = 0;
        Expect("Xbox by name only (vendor unknown)", in, true);
        in = XboxUsbOnMac();
        in.vendor = 0x24C6;  // PowerA
        in.name = "PowerA XBOX Series X Controller";
        Expect("licensed Xbox pad (\"XBOX\" in the name)", in, true);
        in = XboxUsbOnMac();
        in.name = "Controller";
        Expect("Microsoft vendor, plain name", in, true);

        in = XboxUsbOnMac();
        in.shownBefore = true;
        Expect("Xbox by USB on a Mac, dismissed before", in, false);
        in = XboxUsbOnMac();
        in.vibration = false;
        Expect("Xbox by USB on a Mac, Vibration off", in, false);

        // Over Bluetooth the same pad goes through HIDAPI (and rumbles); with
        // GameController reporting wireless, still none.
        in = XboxUsbOnMac();
        in.driver = 'h';
        in.connection = Connection::Wireless;
        in.name = "Xbox Wireless Controller";
        Expect("Xbox over Bluetooth (HIDAPI)", in, false);
        in.driver = 'm';
        Expect("Xbox over Bluetooth (GameController, wireless)", in, false);
        in = XboxUsbOnMac();
        in.driver = 'h';
        in.connection = Connection::Wired;
        Expect("Xbox by USB through HIDAPI (rumbles)", in, false);

        // A DualSense rumbles either way.
        in = XboxUsbOnMac();
        in.vendor = 0x054C;
        in.name = "DualSense Wireless Controller";
        Expect("DualSense by USB on a Mac", in, false);
        in.connection = Connection::Wired;
        in.driver = 'h';
        Expect("DualSense by USB on a Mac (HIDAPI)", in, false);

        // Other platforms: never.
        in = XboxUsbOnMac();
        in.platform = Platform::Other;
        Expect("Xbox by USB, not macOS", in, false);
    }

    std::string Read(const std::filesystem::path& file)
    {
        std::ifstream f(file);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    // Once: the first decision shows it, the dismissal is recorded, and every
    // later one (this run, a new load, after a save) doesn't.
    void Once(const std::filesystem::path& scratch)
    {
        printf("shown once (settings.toml):\n");
        std::error_code ec;
        std::filesystem::remove_all(scratch, ec);
        std::filesystem::create_directories(scratch, ec);
        const std::filesystem::path file = scratch / "settings.toml";
        std::ofstream(file) << "[app]\nlast_version = \"0.1.8\"\nrefresh_notices = \"90\"\n";
        CHECK(settings::Load(file), "load");
        auto in = XboxUsbOnMac();
        in.shownBefore = settings::RumbleNoteShown();
        CHECK(RumbleNote::Decide(in).show, "first time");
        CHECK(settings::RecordRumbleNote(), "record");
        in.shownBefore = settings::RumbleNoteShown();
        CHECK(!RumbleNote::Decide(in).show, "again this run");
        std::string text = Read(file);
        CHECK(text.find("xbox_usb_rumble_note = true") != std::string::npos && text.find("refresh_notices = \"90\"") != std::string::npos &&
                text.find("last_version = \"0.1.8\"") != std::string::npos,
            "file after the record:\n%s", text.c_str());
        CHECK(settings::Load(file) && settings::RumbleNoteShown(), "reload");
        CHECK(settings::RecordVersion("0.1.9") && settings::RecordRefreshNotice(144) && settings::Save(), "version, refresh notice, save");
        text = Read(file);
        CHECK(text.find("xbox_usb_rumble_note = true") != std::string::npos && text.find("refresh_notices = \"90,144\"") != std::string::npos,
            "after a save:\n%s", text.c_str());
        CHECK(settings::Load(file) && settings::RumbleNoteShown(), "reload after a save");
        // Without the key (a new player, or a hand edit): not shown before, and nothing written.
        std::ofstream(file) << "[app]\nlast_version = \"0.1.8\"\n";
        CHECK(settings::Load(file) && !settings::RumbleNoteShown() && settings::RecordVersion("0.1.9"), "plain");
        text = Read(file);
        CHECK(text.find("xbox_usb_rumble_note") == std::string::npos, "the key appeared:\n%s", text.c_str());
        std::ofstream(file) << "[app]\nxbox_usb_rumble_note = \"yes\"\n";
        CHECK(settings::Load(file) && !settings::RumbleNoteShown(), "not a bool");
        printf("  shown, recorded, not shown again, kept by a save: %s\n", s_failures ? "see failures" : "ok");
        std::filesystem::remove_all(scratch, ec);
    }
}

int main(int argc, char** argv)
{
    const std::filesystem::path scratch = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::temp_directory_path() / "rumble_note_test";
    WhenItShows();
    Once(scratch);
    printf(s_failures ? "\n%d FAILED\n" : "\nall passed\n", s_failures);
    return s_failures ? 1 : 0;
}
