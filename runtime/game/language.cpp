// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Game Language: lets the USA executable run with a PAL disc's data (its NFS
// and Movies folders), in that disc's language.
//
// EA built one executable per SKU. The USA one (MWRelease) and Spain's
// (MWEuropeSpanRelease, default.xex SHA-256 684ceef9...) share the same game
// code and the same .data; matched function by function, the only
// differences that touch a PAL disc's files are two constants:
//
//   - The boot (sub_823B4740) sets the game's language with
//     `li r3, <language>; bl sub_822BCBC8` at 0x823B48D8: 0 (English) in the
//     USA build, 4 (Spanish) in Spain's. That index picks the text
//     (LANGUAGES\<NAME>.BIN), the speech banks and the movies' language, so
//     with Spanish data and English asked for the speech is silent.
//   - The movie name (sub_82282E80) is "%sMOVIES\%s%s%s%s": prefix, name,
//     "_<language>", the TV system, ".wmv". The USA build passes "_ntsc"
//     (0x82078AA8) in r9; Spain's passes "_pal", which the USA build also
//     holds, unused, at 0x82078AB0. PAL discs carry only _pal movies.
//
// (The rest differs only online: SKU codes, region "NA"/"EU", date format.)
//
// Two overrides of the generated, weak functions (no ppc/ regeneration):
//   - sub_822BCBC8, SetCurrentLanguage(r3): on the boot's call (ctx.lr =
//     kBootCall) r3 becomes the setting's language. The debug language
//     cycling's calls are left alone.
//   - sub_82282A58, the movie name's snprintf: on sub_82282E80's call
//     (ctx.lr = kMovieNameCall) r9 becomes "_pal" for any language but
//     English.
//
// settings::Id::GameLanguage ([game] language in settings.toml,
// NFSMW_LANGUAGE=spanish for one run); read at boot, so a change applies at
// the next launch.
#include <stdafx.h>

#include <kernel/function.h>
#include <user/settings.h>

extern "C" PPC_FUNC(__imp__sub_822BCBC8);
extern "C" PPC_FUNC(__imp__sub_82282A58);

namespace
{
    constexpr uint32_t kBootCall = 0x823B48E0;       // after the boot's bl sub_822BCBC8
    constexpr uint32_t kMovieNameCall = 0x82282F00;  // after sub_82282E80's bl sub_82282A58
    constexpr uint32_t kNtscSuffix = 0x82078AA8;     // "_ntsc"
    constexpr uint32_t kPalSuffix = 0x82078AB0;      // "_pal"

    int32_t Language()
    {
        return settings::GetInt(settings::Id::GameLanguage);
    }
}

PPC_FUNC(sub_822BCBC8)
{
    if (static_cast<uint32_t>(ctx.lr) == kBootCall)
    {
        const int32_t language = Language();
        if (language != ctx.r3.s32)
        {
            fprintf(stderr, "[language] the game's language: %d (the executable's: %d)\n", language, ctx.r3.s32);
            ctx.r3.s64 = language;
        }
    }
    __imp__sub_822BCBC8(ctx, base);
}

PPC_FUNC(sub_82282A58)
{
    if (static_cast<uint32_t>(ctx.lr) == kMovieNameCall && ctx.r9.u32 == kNtscSuffix &&
        Language() != int32_t(settings::GameLanguage::English))
        ctx.r9.u64 = kPalSuffix;
    __imp__sub_82282A58(ctx, base);
}
