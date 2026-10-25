// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Settings > Game > Language: the game's text in any of its ten languages,
// from the USA disc's own files. From wendersonrodrigues's GitHub pull
// request #26, which found the two constants below.
//
// EA built one executable per SKU. The USA one (MWRelease) and Spain's
// (MWEuropeSpanRelease, default.xex SHA-256 684ceef9...) share the same game
// code and the same .data; matched function by function, the only
// differences that touch the disc's files are two constants:
//
//   - The boot (sub_823B4740) sets the game's language with
//     `li r3, <language>; bl sub_822BCBC8` at 0x823B48D8: 0 (English) in the
//     USA build, 4 (Spanish) in Spain's. That index picks the text
//     (LANGUAGES\<NAME>.BIN, which the USA disc's archives hold for all ten
//     languages), the police radio's speech bank
//     (Sound\Speech\copspeech_<xx>.big for French, German, Italian, Spanish:
//     the USA disc has only English's) and the movies' language.
//   - The movie name (sub_82282E80) is "%sMOVIES\%s%s%s%s": prefix, name,
//     "_<language>", the TV system, ".wmv". The USA build passes "_ntsc"
//     (0x82078AA8) in r9; Spain's passes "_pal", which the USA build also
//     holds, unused, at 0x82078AB0. When that language's movie is missing,
//     the game tries English's with the same suffix.
//
// (The rest differs only online: SKU codes, region "NA"/"EU", date format.)
//
// Two overrides of the generated, weak functions (no ppc/ regeneration);
// game/language.h has the decisions:
//   - sub_822BCBC8, SetCurrentLanguage(r3): on the boot's call r3 becomes
//     the setting's language (BootLanguage). The debug language cycling's
//     calls are left alone.
//   - sub_82282A58, the movie name's sprintf: on sub_82282E80's call r9
//     becomes "_pal" only when Movies/ holds the chosen language's PAL
//     movies (MovieSuffix), so with the USA disc's files the game's own
//     fallback plays the English movies.
//   - sub_82148818, the attribute lookup: sub_821B82E8's speech record
//     becomes English's when the chosen language's bank is missing
//     (SpeechIndex), so the police radio and the in-game scenes' voices play
//     in English instead of not at all.
//
// settings::Id::GameLanguage ([game] language in settings.toml,
// NFSMW_LANGUAGE=spanish for one run); read at boot, so a change applies at
// the next launch. English, the default, opens the same files as before.
#include <stdafx.h>
#include "language.h"

#include <kernel/function.h>
#include <user/paths.h>
#include <user/settings.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

extern "C" PPC_FUNC(__imp__sub_822BCBC8);
extern "C" PPC_FUNC(__imp__sub_82282A58);
extern "C" PPC_FUNC(__imp__sub_82148818);

namespace
{
    using namespace game::language;

    int32_t Chosen()
    {
        return settings::GetInt(settings::Id::GameLanguage);
    }

    // `name` in `dir`, in any case (the disc's folders are "Movies" and
    // "NFS"; an install on a case-sensitive file system keeps the disc's).
    std::filesystem::path Child(const std::filesystem::path& dir, std::string_view name)
    {
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(dir, ec))
        {
            const std::string n = e.path().filename().string();
            if (n.size() == name.size() && EndsWithNoCase(n, name))
                return e.path();
        }
        return {};
    }

    struct Installed
    {
        std::array<Files, std::size(kLanguages)> files;
    };

    // Read once: Movies/'s names and NFS/ZDIR.BIN (33 KB), never the game's
    // own file opens.
    const Installed& InstalledFiles()
    {
        static const Installed installed = [] {
            Installed out;
            const std::filesystem::path game = GetGamePath();
            std::vector<std::string> movies;
            std::error_code ec;
            if (std::filesystem::path dir = Child(game, "movies"); !dir.empty())
                for (const auto& e : std::filesystem::directory_iterator(dir, ec))
                    movies.push_back(e.path().filename().string());
            std::vector<uint8_t> zdir;
            if (std::filesystem::path nfs = Child(game, "nfs"); !nfs.empty())
                if (std::filesystem::path index = Child(nfs, "zdir.bin"); !index.empty())
                {
                    std::ifstream f(index, std::ios::binary);
                    zdir.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
                }
            for (size_t i = 0; i < std::size(kLanguages); i++)
                out.files[i] = Scan(kLanguages[i], movies, zdir);
            return out;
        }();
        return installed;
    }

    const Files* FilesOf(int32_t id)
    {
        for (size_t i = 0; i < std::size(kLanguages); i++)
            if (kLanguages[i].id == id)
                return &InstalledFiles().files[i];
        return nullptr;
    }
}

namespace game::language
{
    std::string MenuNote(int32_t id)
    {
        const Language* l = Find(id);
        const Files* f = FilesOf(id);
        return l && f ? Note(*l, *f) : std::string();
    }
}

PPC_FUNC(sub_822BCBC8)
{
    if (static_cast<uint32_t>(ctx.lr) == kBootCall)
    {
        const int32_t chosen = Chosen();
        if (const Language* l = Find(chosen))
            fprintf(stderr, "%s\n", LogLine(*l, *FilesOf(chosen)).c_str());
        if (const int32_t r3 = BootLanguage(kBootCall, ctx.r3.s32, chosen); r3 != ctx.r3.s32)
            ctx.r3.s64 = r3;
    }
    __imp__sub_822BCBC8(ctx, base);
}

PPC_FUNC(sub_82282A58)
{
    if (static_cast<uint32_t>(ctx.lr) == kMovieNameCall)
    {
        const Files* f = FilesOf(Chosen());
        if (const uint32_t r9 = MovieSuffix(kMovieNameCall, ctx.r9.u32, f && f->pal); r9 != ctx.r9.u32)
            ctx.r9.u64 = r9;
    }
    __imp__sub_82282A58(ctx, base);
}

// The attribute lookup (852 callers): only sub_821B82E8's speech-record
// lookups change, and only when the chosen language's bank is missing.
PPC_FUNC(sub_82148818)
{
    const uint32_t lr = static_cast<uint32_t>(ctx.lr);
    if (lr >= kSpeechPickBegin && lr < kSpeechPickEnd)
    {
        const Files* f = FilesOf(Chosen());
        if (const uint32_t r5 = SpeechIndex(lr, ctx.r4.u32, ctx.r5.u32, f && f->speechToEnglish); r5 != ctx.r5.u32)
            ctx.r5.u64 = r5;
    }
    __imp__sub_82148818(ctx, base);
}
