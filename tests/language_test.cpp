// Tests for Settings > Game > Language (runtime/game/language.h, and the
// option in user/settings.cpp): the boot's language, the movie suffix and the
// speech record game/language.cpp hooks, the game's table, what the installed files give a
// language, and the option itself. Links user/settings.cpp (and toml++ from
// XenonRecomp's vendored copy); no game files. From the repo root:
//   clang++ -std=c++20 -O2 -Iruntime -Itools/XenonRecomp/thirdparty/tomlplusplus/include \
//     tests/language_test.cpp runtime/user/settings.cpp -o build/language_test && build/language_test
#include <game/language.h>
#include <user/settings.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <process.h>  // _getpid
#include <stdlib.h>   // _putenv_s
#else
#include <unistd.h>  // mkdtemp
#endif
#ifdef __APPLE__
#include <TargetConditionals.h>  // TARGET_OS_IOS: the row is hidden there
#endif

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

using namespace game::language;

namespace
{
    // The game's language table (0x828FF000 in the USA default.xex, 10
    // records of 20 bytes: index, name, LANGUAGES\<NAME>.BIN, ...), copied
    // from the image in its order.
    struct GameEntry { int32_t index; const char* name; };
    constexpr GameEntry kGameTable[] = {
        { 0, "ENGLISH" }, { 1, "FRENCH" }, { 2, "GERMAN" }, { 3, "ITALIAN" }, { 4, "SPANISH" },
        { 5, "DUTCH" }, { 6, "SWEDISH" }, { 12, "POLISH" }, { 13, "FINNISH" }, { 7, "DANISH" },
    };

    // Callers of SetCurrentLanguage (the boot, the debug cycling's, the
    // unused English tail call's) and of the movie sprintf (the movie name,
    // two of the other 145), plus nonsense.
    constexpr uint32_t kCallers[] = { kBootCall, 0x822BC738, 0x822BCD08, kMovieNameCall, 0x82282A5C, 0x8230F1C4, 0, 0xFFFFFFFF };

    void PutHash(std::vector<uint8_t>& zdir, uint32_t h)
    {
        uint8_t e[24] = {};
        e[0] = uint8_t(h);
        e[1] = uint8_t(h >> 8);
        e[2] = uint8_t(h >> 16);
        e[3] = uint8_t(h >> 24);
        zdir.insert(zdir.end(), e, e + 24);
    }

    // An archive index as the USA disc's: every language's text, English's
    // police radio only.
    std::vector<uint8_t> UsaIndex()
    {
        std::vector<uint8_t> zdir;
        PutHash(zdir, ArchiveHash("SOUND\\SPEECH\\COPSPEECH_EN.BIG"));
        for (const Language& l : kLanguages)
            PutHash(zdir, ArchiveHash(std::string("LANGUAGES\\") + l.disc + ".BIN"));
        PutHash(zdir, 0x12345678);
        return zdir;
    }

    // The USA disc's Movies/ (33 files, all <name>_english_ntsc.wmv).
    std::vector<std::string> UsaMovies()
    {
        std::vector<std::string> m;
        for (const char* n : { "attract_movie", "bounty_tutorial", "drag_tutorial", "eahd_bumper", "ealogo", "psa", "pursuit_tutorial",
                 "speedtrap_tutorial", "storyfmv_bla134", "storyfmv_bus12", "storyfmv_cro06_coh06a", "storyfmv_her136", "storyfmv_pin11",
                 "storyfmv_rac01", "storyfmv_rap30", "storyfmv_raz08", "storyfmv_saf25", "tollbooth_tutorial" })
            m.push_back(std::string(n) + "_english_ntsc.wmv");
        for (int i = 1; i <= 15; i++)
        {
            char n[64];
            snprintf(n, sizeof(n), "blacklist_%02d_english_ntsc.wmv", i);
            m.push_back(n);
        }
        return m;
    }

    const Language& L(int32_t id)
    {
        return *Find(id);
    }

    std::string ReadAll(const std::filesystem::path& file)
    {
        std::ifstream f(file);
        std::stringstream s;
        s << f.rdbuf();
        return s.str();
    }

    void TestTable()
    {
        CHECK(std::size(kLanguages) == 10, "ten languages, %zu", std::size(kLanguages));
        CHECK(kLanguages[0].id == kEnglish && std::string_view(kLanguages[0].token) == "english", "English first (the fallback's)");
        std::set<int32_t> ids;
        for (const GameEntry& g : kGameTable)
        {
            const Language* l = Find(g.index);
            CHECK(l != nullptr, "the game's %d (%s) is offered", g.index, g.name);
            if (!l)
                continue;
            CHECK(std::string_view(l->disc) == g.name, "%d: %s, the game's %s", g.index, l->disc, g.name);
            std::string token = g.name, name = g.name, speech;
            for (char& c : token)
                c = char(std::tolower(uint8_t(c)));
            for (size_t i = 1; i < name.size(); i++)
                name[i] = char(std::tolower(uint8_t(name[i])));
            // The game's speech switch (sub_821B82E8): own banks for 1-4.
            speech = g.index >= 1 && g.index <= 4 ? token.substr(0, 2) : "en";
            CHECK(token == l->token, "%d: token %s", g.index, l->token);
            CHECK(name == l->name, "%d: name %s", g.index, l->name);
            CHECK(speech == l->speech, "%d: speech code %s", g.index, l->speech);
            ids.insert(l->id);
        }
        CHECK(ids == std::set<int32_t>({ 0, 1, 2, 3, 4, 5, 6, 7, 12, 13 }), "the ten values 0-7, 12, 13");
        for (int32_t id : { -1, 8, 9, 10, 11, 14, 100 })
            CHECK(Find(id) == nullptr, "%d is not a language", id);
    }

    void TestBoot()
    {
        // English, the default: the executable's r3 for every caller.
        for (uint32_t lr : kCallers)
            for (int32_t r3 : { 0, 1, 4, 13, -1 })
                CHECK(BootLanguage(lr, r3, kEnglish) == r3, "English leaves lr %08X r3 %d alone", lr, r3);
        // Another language: only the boot's call changes.
        for (const Language& l : kLanguages)
            for (uint32_t lr : kCallers)
                for (int32_t r3 : { 0, 4 })
                {
                    const int32_t want = lr == kBootCall && l.id != kEnglish ? l.id : r3;
                    CHECK(BootLanguage(lr, r3, l.id) == want, "%s at lr %08X r3 %d: %d", l.token, lr, r3, BootLanguage(lr, r3, l.id));
                }
    }

    void TestMovieSuffix()
    {
        constexpr uint32_t kOther = 0x82078AC4;  // another string the sprintf gets in r9
        for (uint32_t lr : kCallers)
            for (uint32_t r9 : { kNtscSuffix, kPalSuffix, kOther, 0u })
            {
                CHECK(MovieSuffix(lr, r9, false) == r9, "no PAL movies: lr %08X r9 %08X unchanged", lr, r9);
                const uint32_t want = lr == kMovieNameCall && r9 == kNtscSuffix ? kPalSuffix : r9;
                CHECK(MovieSuffix(lr, r9, true) == want, "PAL movies: lr %08X r9 %08X -> %08X", lr, r9, MovieSuffix(lr, r9, true));
            }
    }

    void TestSpeechIndex()
    {
        // sub_821B82E8's returns from its lookups (one per case, English's
        // last), callers elsewhere, and the edges of its range.
        const uint32_t lrs[] = { 0x821B83C8, 0x821B8428, 0x821B8488, 0x821B84E8, 0x821B8548, 0x821B85E4, kSpeechPickBegin,
            kSpeechPickEnd - 4, kSpeechPickEnd, kSpeechPickBegin - 4, 0x821BD4D0, 0x8248B2A8, kBootCall, 0 };
        for (uint32_t lr : lrs)
            for (uint32_t r4 : { kSpeechCollection, 0x4166FA80u, 0u })
                for (uint32_t r5 : { 0u, 1u, 2u, 3u, 4u, 5u })
                {
                    CHECK(SpeechIndex(lr, r4, r5, false) == r5, "no fallback: lr %08X r4 %08X r5 %u unchanged", lr, r4, r5);
                    const bool inside = lr >= kSpeechPickBegin && lr < kSpeechPickEnd && r4 == kSpeechCollection;
                    CHECK(SpeechIndex(lr, r4, r5, true) == (inside ? kEnglishSpeech : r5), "fallback: lr %08X r4 %08X r5 %u", lr, r4, r5);
                }
    }

    void TestArchiveHash()
    {
        // Keys in the USA disc's NFS/ZDIR.BIN, for paths as the game asks.
        CHECK(ArchiveHash("D:\\Sound\\Speech\\copspeech_en.big") == 0xF9F10F64, "%08X", ArchiveHash("D:\\Sound\\Speech\\copspeech_en.big"));
        CHECK(ArchiveHash("D:\\Sound\\Speech\\copspeech_en.idx") == 0xF9F12C97, "%08X", ArchiveHash("D:\\Sound\\Speech\\copspeech_en.idx"));
        CHECK(ArchiveHash("LANGUAGES\\SPANISH.BIN") == 0x983729EF, "%08X", ArchiveHash("LANGUAGES\\SPANISH.BIN"));
        CHECK(ArchiveHash("D:\\LANGUAGES\\ENGLISH.BIN") == 0x27771A63, "%08X", ArchiveHash("D:\\LANGUAGES\\ENGLISH.BIN"));
        CHECK(ArchiveHash("sound/streams/nisaudio.big") == 0x059CAA5B, "%08X", ArchiveHash("sound/streams/nisaudio.big"));
        std::vector<uint8_t> zdir = UsaIndex();
        CHECK(InArchive(zdir, "D:\\Sound\\Speech\\CopSpeech_EN.big"), "any case");
        CHECK(!InArchive(zdir, "D:\\Sound\\Speech\\copspeech_sp.big"), "no Spanish bank");
        std::vector<uint8_t> cut;
        PutHash(cut, ArchiveHash("LANGUAGES\\POLISH.BIN"));
        cut.resize(23);
        CHECK(!InArchive(cut, "LANGUAGES\\POLISH.BIN"), "a cut-off entry is not read");
        CHECK(!InArchive({}, "LANGUAGES\\ENGLISH.BIN"), "no index: nothing");
    }

    void TestFiles()
    {
        const std::vector<uint8_t> usa = UsaIndex();
        const std::vector<std::string> usaMovies = UsaMovies();
        CHECK(usaMovies.size() == 33, "the USA disc's 33 movies");

        // The USA disc's files: English is the disc's own, all of it.
        Files en = Scan(L(kEnglish), usaMovies, usa);
        CHECK(en.text && !en.pal && en.ownMovies && en.speech && !en.speechToEnglish, "English on the USA disc");
        CHECK(Note(L(kEnglish), en) == "Menus, race text, movies and speech in English.", "%s", Note(L(kEnglish), en).c_str());
        CHECK(LogLine(L(kEnglish), en) == "[language] English (english): text yes; movies English (_ntsc); speech English (copspeech_en present)",
            "%s", LogLine(L(kEnglish), en).c_str());
        // ...and every other language: its text, the English movies (_ntsc
        // kept), English speech: the game's own for five of them, the
        // fallback for French, German, Italian and Spanish.
        for (const Language& l : kLanguages)
        {
            if (l.id == kEnglish)
                continue;
            Files f = Scan(l, usaMovies, usa);
            const bool own = std::string_view(l.speech) != "en";
            CHECK(f.text && !f.pal && !f.ownMovies && f.englishMovies && f.englishSpeech, "%s on the USA disc", l.token);
            CHECK(f.speech == !own && f.speechToEnglish == own, "%s: speech %d, to English %d", l.token, f.speech, f.speechToEnglish);
            CHECK(MoviesIn(l, f) == "English" && SpeechIn(l, f) == "English", "%s: movies and speech in English", l.token);
            CHECK(MovieSuffix(kMovieNameCall, kNtscSuffix, f.pal) == kNtscSuffix, "%s keeps _ntsc", l.token);
            CHECK(Note(l, f) == std::string("Menus and race text in ") + l.name + ". Movies and speech in English.", "%s", Note(l, f).c_str());
        }
        Files es = Scan(L(4), usaMovies, usa);
        CHECK(LogLine(L(4), es) == "[language] Spanish (spanish): text yes; movies English (_ntsc); speech English (copspeech_sp absent: English's instead)",
            "%s", LogLine(L(4), es).c_str());
        Files nl = Scan(L(5), usaMovies, usa);
        CHECK(LogLine(L(5), nl) == "[language] Dutch (dutch): text yes; movies English (_ntsc); speech English (copspeech_en present)",
            "%s", LogLine(L(5), nl).c_str());
        // English never falls back (its lookups stay the game's), whatever
        // the files.
        for (const auto& movies : { usaMovies, std::vector<std::string> {} })
            for (const auto& index : { usa, std::vector<uint8_t> {} })
                CHECK(!Scan(L(kEnglish), movies, index).speechToEnglish, "English: no fallback");
        // No English bank either: nothing to fall back to.
        std::vector<uint8_t> noSpeech;
        PutHash(noSpeech, ArchiveHash("LANGUAGES\\SPANISH.BIN"));
        Files mute = Scan(L(4), usaMovies, noSpeech);
        CHECK(!mute.speech && !mute.speechToEnglish && SpeechIn(L(4), mute).empty(), "no bank at all");
        CHECK(Note(L(4), mute) == "Menus and race text in Spanish. Movies in English. No speech (the installed files have none for it).",
            "%s", Note(L(4), mute).c_str());

        // A language's PAL movies, alone (any case): _pal, its own movies.
        std::vector<std::string> pal = { "ealogo_spanish_pal.wmv", "ATTRACT_MOVIE_SPANISH_PAL.WMV", "Psa_Spanish_Pal.Wmv" };
        Files esPal = Scan(L(4), pal, usa);
        CHECK(esPal.pal && esPal.ownMovies && !esPal.englishMovies, "Spanish PAL movies");
        CHECK(MoviesIn(L(4), esPal) == "Spanish", "in Spanish");
        CHECK(MovieSuffix(kMovieNameCall, kNtscSuffix, esPal.pal) == kPalSuffix, "-> _pal");
        Files dePal = Scan(L(2), pal, usa);
        CHECK(!dePal.pal && !dePal.ownMovies && !dePal.englishMovies, "German with only Spanish movies: _ntsc, no movies");
        CHECK(Note(L(2), dePal).find("No movies") != std::string::npos, "%s", Note(L(2), dePal).c_str());

        // Both (the USA disc reinstalled over PAL movies): the language that
        // has PAL movies takes them; English keeps the USA disc's.
        std::vector<std::string> both = usaMovies;
        both.insert(both.end(), pal.begin(), pal.end());
        CHECK(Scan(L(4), both, usa).pal, "Spanish PAL movies beside the USA disc's: _pal");
        CHECK(!Scan(L(kEnglish), both, usa).pal, "English: the USA disc's _ntsc");
        CHECK(!Scan(L(1), both, usa).pal && Scan(L(1), both, usa).englishMovies, "French: _ntsc, English movies");
        // English PAL movies only (a European English disc's): _pal for English.
        std::vector<std::string> ukPal = { "ealogo_english_pal.wmv" };
        CHECK(Scan(L(kEnglish), ukPal, usa).pal && Scan(L(kEnglish), ukPal, usa).ownMovies, "English PAL movies alone: _pal");
        CHECK(Scan(L(1), ukPal, usa).pal == false && Scan(L(1), ukPal, usa).englishMovies == false, "French: _ntsc, the fallback has none");
        // ...but beside the USA disc's own English movies, English keeps them.
        std::vector<std::string> enBoth = usaMovies;
        enBoth.push_back("ealogo_english_pal.wmv");
        CHECK(!Scan(L(kEnglish), enBoth, usa).pal, "English NTSC and PAL movies: the USA disc's _ntsc");
        // Names that only look like it.
        std::vector<std::string> odd = { "_spanish_pal.wmv", "ealogo_spanish_pal.wmv.part", "ealogo_spanish_pal.bik", "ealogo_xspanish_pal.wmv" };
        CHECK(!Scan(L(4), odd, usa).pal, "no real Spanish PAL movie there");

        // A language's speech bank in the index: its police radio.
        std::vector<uint8_t> withEs = usa;
        PutHash(withEs, ArchiveHash("SOUND\\SPEECH\\COPSPEECH_SP.BIG"));
        Files esAll = Scan(L(4), pal, withEs);
        CHECK(esAll.speech && !esAll.speechToEnglish, "Spanish speech, no fallback");
        CHECK(Note(L(4), esAll) == "Menus, race text, movies and speech in Spanish.", "%s", Note(L(4), esAll).c_str());
        // Nothing found (an unreadable folder): said, not guessed.
        Files none = Scan(L(4), {}, {});
        CHECK(!none.text && !none.pal && !none.ownMovies && !none.englishMovies && !none.speech, "no files");
        CHECK(Note(L(4), none) == "The installed files have no Spanish text. No movies (the installed files have none for it). No speech (the installed files have none for it).",
            "%s", Note(L(4), none).c_str());
    }

    void TestOption(const std::filesystem::path& dir)
    {
        using namespace settings;
        const Option& o = Info(Id::GameLanguage);
        CHECK(std::string_view(o.table) == "game" && std::string_view(o.key) == "language", "[game] language");
        CHECK(std::string_view(o.label) == "Language", "label %s", o.label);
        CHECK(o.category == Category::Game && std::string_view(CategoryName(o.category)) == "Game", "Settings > Game");
        CHECK(o.type == Type::Enum && o.apply == Apply::Restart, "an enum, at the next launch");
        CHECK(std::string_view(o.env) == "NFSMW_LANGUAGE", "env %s", o.env);
        CHECK(o.defaultValue == double(kEnglish), "English by default");
#if defined(__APPLE__) && TARGET_OS_IOS
        CHECK(o.hidden, "not shown on iPhone and iPad");
#else
        CHECK(!o.hidden, "shown");
#endif
        CHECK(std::string_view(o.description).find("next launch") == std::string_view::npos, "said once, by the file's env note");
        // v0.2.0: the European (PAL) Spanish disc is supported, and named.
        CHECK(std::string_view(o.description).find("European (PAL) Spanish disc has Spanish movies and speech") != std::string_view::npos,
            "the PAL disc in the description");
        CHECK(o.entries.size() == std::size(kLanguages), "%zu entries", o.entries.size());
        for (size_t i = 0; i < o.entries.size() && i < std::size(kLanguages); i++)
        {
            CHECK(o.entries[i].value == kLanguages[i].id, "entry %zu value", i);
            CHECK(std::string_view(o.entries[i].token) == kLanguages[i].token, "entry %zu token", i);
            CHECK(std::string_view(o.entries[i].label) == kLanguages[i].name, "entry %zu: \"%s\"", i, o.entries[i].label);
        }

        // The file: a name in any case. The first Load fixes this run's
        // language (a Restart option), as at launch.
        const std::filesystem::path file = dir / "settings.toml";
        std::ofstream(file) << "[game]\nlanguage = \"SPANISH\"\n";
        CHECK(Load(file), "load");
        CHECK(GetInt(Id::GameLanguage) == 4 && GetStoredValue(Id::GameLanguage) == 4, "\"SPANISH\": Spanish, running %d",
            GetInt(Id::GameLanguage));
        CHECK(FormatValue(Id::GameLanguage, 4) == "Spanish", "%s", FormatValue(Id::GameLanguage, 4).c_str());
        // The menu: German waits for the next launch, and is what's saved.
        CHECK(Set(Id::GameLanguage, 2) == SetResult::Deferred, "deferred");
        CHECK(GetInt(Id::GameLanguage) == 4 && GetStoredValue(Id::GameLanguage) == 2 && RestartPending(Id::GameLanguage),
            "this run stays Spanish, German saved");
        CHECK(Save(file), "save");
        const std::string text = ReadAll(file);
        CHECK(text.find("[game]") != std::string::npos && text.find("language = \"german\"") != std::string::npos, "%s", text.c_str());
        CHECK(text.find("(NFSMW_LANGUAGE; applies at the next launch)") != std::string::npos, "the env note");
        const size_t row = text.find("# Language:"), value = text.find("language =", row);
        const std::string block = row == std::string::npos || value == std::string::npos ? "" : text.substr(row, value - row);
        size_t times = 0;
        for (size_t at = block.find("next launch"); at != std::string::npos; at = block.find("next launch", at + 1))
            times++;
        CHECK(times == 1, "the Language row says \"next launch\" once, not %zu times:\n%s", times, block.c_str());
        // Not a language: numbers and unknown words are refused.
        CHECK(Set(Id::GameLanguage, 8) == SetResult::Invalid, "8 is no language");
        CHECK(Set(Id::GameLanguage, 12) != SetResult::Invalid, "12 (Polish) is");
        for (const char* bad : { "4", "\"4\"", "\"klingon\"", "\"pal\"" })
        {
            std::ofstream(file) << "[game]\nlanguage = " << bad << "\n";
            Load(file);
            const double stored = GetStoredValue(Id::GameLanguage);
            CHECK(stored == double(kEnglish), "%s: refused, the default stored (%g)", bad, stored);
            CHECK(GetInt(Id::GameLanguage) == 4, "%s: this run keeps Spanish", bad);
        }
    }
}

namespace
{
    // The European (PAL, Spanish) disc: every language's text, Spanish
    // speech only (COPSPEECH_SP), and its 33 movies as <name>_spanish_pal.wmv.
    void TestPalDisc()
    {
        std::vector<uint8_t> index;
        PutHash(index, ArchiveHash("SOUND\\SPEECH\\COPSPEECH_SP.BIG"));
        for (const Language& l : kLanguages)
            PutHash(index, ArchiveHash(std::string("LANGUAGES\\") + l.disc + ".BIN"));
        std::vector<std::string> movies;
        for (const std::string& m : UsaMovies())
            movies.push_back(m.substr(0, m.size() - std::string("_english_ntsc.wmv").size()) + "_spanish_pal.wmv");

        // Spanish, the disc's own: all of it, nothing redirected.
        Files es = Scan(L(4), movies, index, true);
        CHECK(es.text && es.pal && es.ownMovies && es.speech && !es.movieRedirect && !es.speechRedirect, "Spanish on the PAL disc");
        CHECK(Note(L(4), es) == "Menus, race text, movies and speech in Spanish.", "%s", Note(L(4), es).c_str());
        CHECK(LogLine(L(4), es) == "[language] Spanish (spanish): text yes; movies Spanish (_pal); speech Spanish (copspeech_sp present)",
            "%s", LogLine(L(4), es).c_str());
        // Every other language: its text, Spanish movies and speech (no
        // English ones on this disc for the game's fallback to find).
        for (const Language& l : kLanguages)
        {
            if (l.id == 4)
                continue;
            Files f = Scan(l, movies, index, true);
            CHECK(f.text && !f.ownMovies && !f.englishMovies && f.movieRedirect && f.movieRedirect->id == 4, "%s: Spanish movies", l.token);
            CHECK(!f.speech && !f.speechToEnglish && f.speechRedirect && f.speechRedirect->id == 4, "%s: Spanish speech", l.token);
            CHECK(Note(l, f) == std::string("Menus and race text in ") + l.name + ". Movies and speech in Spanish.", "%s", Note(l, f).c_str());
            CHECK(SpeechIndexTo(kSpeechPickBegin + 0xE0, kSpeechCollection, 0, f.speechRedirect->id) == 4, "%s: record 4", l.token);
        }
        CHECK(LogLine(L(kEnglish), Scan(L(kEnglish), movies, index, true)) == "[language] English (english): text yes; movies Spanish (_pal); "
            "speech Spanish (copspeech_en absent: Spanish's instead)", "%s", LogLine(L(kEnglish), Scan(L(kEnglish), movies, index, true)).c_str());
        // The USA executable's rules don't redirect (its disc has English's).
        Files usaRules = Scan(L(kEnglish), movies, index);
        CHECK(!usaRules.movieRedirect && !usaRules.speechRedirect, "no redirects for the USA build");

        // The boot: the PAL build passes 4; the chosen language applies, English too.
        for (uint32_t lr : kCallers)
            for (const Language& l : kLanguages)
            {
                CHECK(BootLanguageFor(lr, 4, l.id, true) == (lr == kBootCall ? l.id : 4), "PAL boot %s lr %08X", l.token, lr);
                CHECK(BootLanguageFor(lr, 0, l.id, false) == BootLanguage(lr, 0, l.id), "USA boot unchanged %s", l.token);
            }
        // SpeechIndexTo(-1) never changes anything; (0) is SpeechIndex's fallback.
        for (uint32_t r5 : { 0u, 1u, 4u })
        {
            CHECK(SpeechIndexTo(kSpeechPickBegin, kSpeechCollection, r5, -1) == r5, "no redirect");
            CHECK(SpeechIndexTo(kSpeechPickBegin, kSpeechCollection, r5, 0) == SpeechIndex(kSpeechPickBegin, kSpeechCollection, r5, true), "= SpeechIndex");
        }
    }
}

int main()
{
    TestTable();
    TestBoot();
    TestMovieSuffix();
    TestSpeechIndex();
    TestArchiveHash();
    TestFiles();
    TestPalDisc();

#ifdef _WIN32
    // Windows has no mkdtemp (settings_keys_test's way): a folder named
    // after this process, made new; and no unsetenv.
    const std::filesystem::path scratch = std::filesystem::temp_directory_path() / ("language_test-" + std::to_string(_getpid()));
    std::error_code scratchError;
    std::filesystem::remove_all(scratch, scratchError);
    const bool made = std::filesystem::create_directory(scratch, scratchError);
    CHECK(made, "a scratch folder");
    if (made)
    {
        _putenv_s("NFSMW_LANGUAGE", "");
        TestOption(scratch);
        std::filesystem::remove_all(scratch, scratchError);
    }
#else
    char tmpl[] = "/tmp/language_test.XXXXXX";
    const char* dir = mkdtemp(tmpl);
    CHECK(dir != nullptr, "a scratch folder");
    if (dir)
    {
        unsetenv("NFSMW_LANGUAGE");
        TestOption(dir);
        std::filesystem::remove_all(dir);
    }
#endif

    if (g_failures)
    {
        printf("language_test: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("language_test: all passed\n");
    return 0;
}
