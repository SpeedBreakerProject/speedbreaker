// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Settings > Game > Language: the decisions game/language.cpp hooks into the
// game, and what the installed files give each language. Header only, so
// tests/language_test.cpp checks it without the game.
//
// The USA executable fixes its language at boot (`li r3, 0` before the
// boot's only SetCurrentLanguage call), but the USA disc's archives hold the
// text of all ten languages in the game's table. Two decisions:
//   - BootLanguage: the boot's call gets the chosen language; any other
//     caller, and English everywhere, keep the executable's r3.
//   - MovieSuffix: the movie name's TV system (`_ntsc` in the USA build)
//     becomes `_pal` only when Movies/ holds that language's PAL movies and
//     none of its NTSC ones. Otherwise `_ntsc` stays, the game asks for
//     <name>_<LANGUAGE>_ntsc.wmv, and its own fallback plays the USA disc's
//     <name>_ENGLISH_ntsc.wmv: the movies never vanish.
//   - SpeechIndex: the game picks the police radio's speech record by
//     language (sub_821B82E8: French, German, Italian and Spanish have their
//     own, every other language English's). When the chosen language's bank
//     isn't in the installed files (the USA disc has only English's), the
//     record lookup is pointed at English's, as the movies fall back, instead
//     of the game loading nothing at all.
// Files says what the installed files have for a language (the text pack
// and the speech bank in NFS/ZDIR.BIN, its movies in Movies/), and Note /
// LogLine say it in words. Measured with the USA disc's files in Spanish and
// German (v0.1.9) without the fallback: the game asked for copspeech_sp /
// _ge, read no speech bank at all and never loaded the in-game scenes' audio
// (sound\streams\nisaudio.big): no police radio, no voices in those scenes.
#pragma once
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace game::language
{
    constexpr uint32_t kBootCall = 0x823B48E0;       // after the boot's bl sub_822BCBC8 (SetCurrentLanguage)
    constexpr uint32_t kMovieNameCall = 0x82282F00;  // after sub_82282E80's bl sub_82282A58 (the movie name)
    constexpr uint32_t kNtscSuffix = 0x82078AA8;     // "_ntsc"
    constexpr uint32_t kPalSuffix = 0x82078AB0;      // "_pal", unused by the USA build

    struct Language
    {
        int32_t id;          // the game's index (its table at 0x828FF000)
        const char* token;   // settings.toml / NFSMW_LANGUAGE
        const char* name;    // the menu's
        const char* disc;    // the table's name: LANGUAGES\<disc>.BIN, <movie>_<disc>_ntsc.wmv
        const char* speech;  // the speech bank the game asks for: Sound\Speech\copspeech_<speech>.big
    };

    // In the game's indices' order (its table lists Polish and Finnish before
    // Danish). Speech: the game's own choice (sub_821B82E8): French, German,
    // Italian and Spanish have their own bank (copspeech_ge and _sp seen in
    // its file opens; _fr and _it by the same pattern), the others English's.
    inline constexpr Language kLanguages[] = {
        { 0, "english", "English", "ENGLISH", "en" },
        { 1, "french", "French", "FRENCH", "fr" },
        { 2, "german", "German", "GERMAN", "ge" },
        { 3, "italian", "Italian", "ITALIAN", "it" },
        { 4, "spanish", "Spanish", "SPANISH", "sp" },
        { 5, "dutch", "Dutch", "DUTCH", "en" },
        { 6, "swedish", "Swedish", "SWEDISH", "en" },
        { 7, "danish", "Danish", "DANISH", "en" },
        { 12, "polish", "Polish", "POLISH", "en" },
        { 13, "finnish", "Finnish", "FINNISH", "en" },
    };
    constexpr int32_t kEnglish = 0;

    constexpr const Language* Find(int32_t id)
    {
        for (const Language& l : kLanguages)
            if (l.id == id)
                return &l;
        return nullptr;
    }

    // The speech record: sub_821B82E8 switches on the language (or a debug
    // override) and asks the attribute lookup sub_82148818 for collection
    // kSpeechCollection, record r5: 1 French, 2 German, 3 Italian, 4 Spanish,
    // 0 English (every other language). Its calls return inside the range.
    constexpr uint32_t kSpeechPickBegin = 0x821B82E8;
    constexpr uint32_t kSpeechPickEnd = 0x821B8628;
    constexpr uint32_t kSpeechCollection = 0xC243117C;
    constexpr uint32_t kEnglishSpeech = 0;

    // The record the lookup gets: English's when `toEnglish` (the chosen
    // language's bank is missing), only for sub_821B82E8's speech lookups.
    constexpr uint32_t SpeechIndex(uint32_t lr, uint32_t r4, uint32_t r5, bool toEnglish)
    {
        return toEnglish && lr >= kSpeechPickBegin && lr < kSpeechPickEnd && r4 == kSpeechCollection ? kEnglishSpeech : r5;
    }

    // The language SetCurrentLanguage gets: the chosen one at the boot's call
    // only. English (the default) never changes anything.
    constexpr int32_t BootLanguage(uint32_t lr, int32_t r3, int32_t chosen)
    {
        return lr == kBootCall && chosen != kEnglish ? chosen : r3;
    }

    // The movie name's TV-system argument (r9 of the name's sprintf).
    constexpr uint32_t MovieSuffix(uint32_t lr, uint32_t r9, bool palMovies)
    {
        return lr == kMovieNameCall && r9 == kNtscSuffix && palMovies ? kPalSuffix : r9;
    }

    // The archive index's key for a path (NFS/ZDIR.BIN: 24-byte entries, this
    // hash first, little-endian, sorted): EA's string hash (h = h * 33 + c,
    // from 0xFFFFFFFF) over the path without its drive, in upper case.
    inline uint32_t ArchiveHash(std::string_view path)
    {
        if (size_t colon = path.find(':'); colon != std::string_view::npos)
            path.remove_prefix(colon + 1);
        while (!path.empty() && (path.front() == '\\' || path.front() == '/'))
            path.remove_prefix(1);
        uint32_t h = 0xFFFFFFFFu;
        for (char c : path)
            h = h * 33u + uint8_t(std::toupper(uint8_t(c == '/' ? '\\' : c)));
        return h;
    }

    inline bool InArchive(const std::vector<uint8_t>& zdir, std::string_view path)
    {
        const uint32_t key = ArchiveHash(path);
        for (size_t at = 0; at + 24 <= zdir.size(); at += 24)
        {
            uint32_t h = uint32_t(zdir[at]) | uint32_t(zdir[at + 1]) << 8 | uint32_t(zdir[at + 2]) << 16 | uint32_t(zdir[at + 3]) << 24;
            if (h == key)
                return true;
        }
        return false;
    }

    inline bool EndsWithNoCase(std::string_view s, std::string_view end)
    {
        if (s.size() < end.size())
            return false;
        for (size_t i = 0; i < end.size(); i++)
            if (std::tolower(uint8_t(s[s.size() - end.size() + i])) != std::tolower(uint8_t(end[i])))
                return false;
        return true;
    }

    // Any of `movies` (file names in Movies/) is "<name>_<disc>_<tv>.wmv".
    inline bool HasMovies(const std::vector<std::string>& movies, const Language& l, std::string_view tv)
    {
        const std::string end = std::string("_") + l.disc + "_" + std::string(tv) + ".wmv";
        for (const std::string& m : movies)
            if (m.size() > end.size() && EndsWithNoCase(m, end))
                return true;
        return false;
    }

    // What the installed files give one language.
    struct Files
    {
        bool text = false;           // LANGUAGES\<disc>.BIN in the archive index
        bool pal = false;            // the movie suffix: its PAL movies, and none of its NTSC ones
        bool ownMovies = false;      // its own movies, with that suffix
        bool englishMovies = false;  // the game's fallback, English movies with that suffix
        bool speech = false;         // the bank the game asks for, in the archive index
        bool englishSpeech = false;  // English's
        bool speechToEnglish = false;  // the fallback: its own bank is missing, English's is there
    };

    inline Files Scan(const Language& l, const std::vector<std::string>& movies, const std::vector<uint8_t>& zdir)
    {
        Files f;
        f.text = InArchive(zdir, std::string("LANGUAGES\\") + l.disc + ".BIN");
        f.pal = HasMovies(movies, l, "pal") && !HasMovies(movies, l, "ntsc");
        const char* tv = f.pal ? "pal" : "ntsc";
        f.ownMovies = HasMovies(movies, l, tv);
        f.englishMovies = HasMovies(movies, kLanguages[0], tv);
        f.speech = InArchive(zdir, std::string("SOUND\\SPEECH\\COPSPEECH_") + l.speech + ".BIG");
        f.englishSpeech = InArchive(zdir, "SOUND\\SPEECH\\COPSPEECH_EN.BIG");
        f.speechToEnglish = std::string_view(l.speech) != "en" && !f.speech && f.englishSpeech;
        return f;
    }

    // Which language the movies play in ("" for none).
    inline std::string MoviesIn(const Language& l, const Files& f)
    {
        return f.ownMovies ? l.name : f.englishMovies ? kLanguages[0].name : "";
    }

    // Which language the speech (police radio, the in-game scenes' voices)
    // is in ("" for none): the bank the game asks for, or English's in its
    // place.
    inline std::string SpeechIn(const Language& l, const Files& f)
    {
        if (f.speech)
            return std::string_view(l.speech) == "en" ? kLanguages[0].name : l.name;
        return f.speechToEnglish ? kLanguages[0].name : "";
    }

    // The menu's line under the row, for the language chosen there.
    inline std::string Note(const Language& l, const Files& f)
    {
        const std::string name = l.name;
        const std::string movies = MoviesIn(l, f), speech = SpeechIn(l, f);
        if (f.text && movies == name && speech == name)
            return "Menus, race text, movies and speech in " + name + ".";
        std::string note = f.text ? "Menus and race text in " + name + "." : "The installed files have no " + name + " text.";
        if (!movies.empty() && movies == speech)
            return note + " Movies and speech in " + movies + ".";
        note += movies.empty() ? " No movies (the installed files have none for it)." : " Movies in " + movies + ".";
        note += speech.empty() ? " No speech (the installed files have none for it)." : " Speech in " + speech + ".";
        return note;
    }

    // The menu's note for the language `id` with the installed files (read
    // once; game/language.cpp). "" for an id not in the table.
    std::string MenuNote(int32_t id);

    // The log's line, once at boot.
    inline std::string LogLine(const Language& l, const Files& f)
    {
        const std::string movies = MoviesIn(l, f), speech = SpeechIn(l, f);
        return std::string("[language] ") + l.name + " (" + l.token + "): text " + (f.text ? "yes" : "none") + "; movies " +
            (movies.empty() ? std::string("none") : movies) + " (_" + (f.pal ? "pal" : "ntsc") + "); speech " +
            (speech.empty() ? std::string("none") : speech) + " (copspeech_" + l.speech +
            (f.speech ? " present" : f.speechToEnglish ? " absent: English's instead" : " absent") + ")";
    }
}
