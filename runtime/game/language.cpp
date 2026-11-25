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
// Overrides of the generated, weak functions (no ppc/ regeneration for these);
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
//   - sub_822BCE58, the string lookup: the Polish text fixes (see "Polish
//     text" below, with the MemcardLocalePolish midasm hook for the save
//     system text).
//
// settings::Id::GameLanguage ([game] language in settings.toml,
// NFSMW_LANGUAGE=spanish for one run); read at boot, so a change applies at
// the next launch. English, the default, opens the same files as before.
#include <stdafx.h>
#include <region/region.h>
#include "language.h"

#include <kernel/function.h>
#include <kernel/heap.h>
#include <kernel/memory.h>
#include <user/paths.h>
#include <user/settings.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <system_error>
#include <unordered_map>

extern "C" PPC_FUNC(__imp__sub_822BCBC8);
extern "C" PPC_FUNC(__imp__sub_82282A58);
extern "C" PPC_FUNC(__imp__sub_82148818);
extern "C" PPC_FUNC(__imp__sub_822BCE58);

namespace
{
    using namespace game::language;

    std::atomic<int32_t> s_launch{ -1 };
    std::atomic<int32_t> s_text{ kEnglish };  // the language SetCurrentLanguage last set

    int32_t Chosen()
    {
        const int32_t launch = s_launch.load(std::memory_order_relaxed);
        return launch >= 0 ? launch : settings::GetInt(settings::Id::GameLanguage);
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
                out.files[i] = Scan(kLanguages[i], movies, zdir, region::IsPal());
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
    void SetLaunchLanguage(int32_t id)
    {
        if (Find(id))
            s_launch.store(id, std::memory_order_relaxed);
    }

    std::string MenuNote(int32_t id)
    {
        const Language* l = Find(id);
        const Files* f = FilesOf(id);
        return l && f ? Note(*l, *f) : std::string();
    }
}

SB_HOOK(sub_822BCBC8)
{
    if (static_cast<uint32_t>(ctx.lr) == region::Addr(kBootCall))
    {
        const int32_t chosen = Chosen();
        if (const Language* l = Find(chosen))
            fprintf(stderr, "%s\n", LogLine(*l, *FilesOf(chosen)).c_str());
        if (const int32_t r3 = BootLanguageFor(kBootCall, ctx.r3.s32, chosen, region::IsPal()); r3 != ctx.r3.s32)
            ctx.r3.s64 = r3;
    }
    s_text.store(ctx.r3.s32, std::memory_order_relaxed);
    SB_ORIGINAL(sub_822BCBC8)(ctx, base);
}

SB_HOOK(sub_82282A58)
{
    if (static_cast<uint32_t>(ctx.lr) == region::Addr(kMovieNameCall))
    {
        const Files* f = FilesOf(Chosen());
        if (const uint32_t r9 = MovieSuffix(kMovieNameCall, ctx.r9.u32, f && f->pal); r9 != ctx.r9.u32)
            ctx.r9.u64 = r9;
        // r8: "_<LANGUAGE>" (a buffer on the caller's stack). A language
        // the disc has no movies for, nor English's (a PAL disc), gets the
        // disc's own (Files::movieRedirect).
        if (region::IsPal() && ctx.r8.u32)
        {
            char* name = reinterpret_cast<char*>(base + ctx.r8.u32);
            const size_t n = strnlen(name, 32);
            for (size_t i = 0; i < std::size(kLanguages); i++)
            {
                const Language& l = kLanguages[i];
                if (n == strlen(l.disc) + 1 && name[0] == '_' && EndsWithNoCase(std::string_view(name, n), l.disc))
                {
                    if (const Language* to = InstalledFiles().files[i].movieRedirect)
                    {
                        static std::atomic<int> said{ 0 };
                        if (said.fetch_add(1) < 4)
                            fprintf(stderr, "[language] movie %s: none in %s on this disc, %s's instead\n",
                                ctx.r7.u32 ? reinterpret_cast<const char*>(base + ctx.r7.u32) : "?", l.name, to->name);
                        snprintf(name, 32, "_%s", to->disc);
                    }
                    break;
                }
            }
        }
    }
    SB_ORIGINAL(sub_82282A58)(ctx, base);
}

// The attribute lookup (852 callers): only sub_821B82E8's speech-record
// lookups change, and only when the chosen language's bank is missing.
SB_HOOK(sub_82148818)
{
    const uint32_t lr = static_cast<uint32_t>(ctx.lr), begin = region::Addr(kSpeechPickBegin);
    if (lr >= begin && lr < region::Addr(kSpeechPickEnd))
    {
        const Files* f = FilesOf(Chosen());
        // The decision takes the USA return address (the same code).
        const int32_t record = !f ? -1 : f->speechToEnglish ? int32_t(kEnglishSpeech) : f->speechRedirect ? f->speechRedirect->id : -1;
        if (const uint32_t r5 = SpeechIndexTo(lr - begin + kSpeechPickBegin, ctx.r4.u32, ctx.r5.u32, record); r5 != ctx.r5.u32)
            ctx.r5.u64 = r5;
    }
    SB_ORIGINAL(sub_82148818)(ctx, base);
}

// Polish text. The disc memcard/LOCALE_POLISH.LOC holds "LANGUAGE NOT SUPPORTED"
// stub text instead of content, so the save system gets the one below instead; and
// six LANGUAGES\POLISH.BIN strings are adapted from English or trimmed to match it.
namespace
{
    constexpr int32_t kPolish = 12;

    // memcard/LOCALE_POLISH.LOC (UTF-16, no labels):
    // the save system texts by index, whole file.
    constexpr std::string_view kPolishLoc[] = {
        "2.03.22",
        "2.03.06",
        "Kontynuuj",
        "Kontynuuj bez zapisywania",
        "Wybierz urządzenie",
        "Tak",
        "Nie",
        "Anuluj",
        "Brak wybranego urządzenia przechowywania danych. Do zapisywania postępów w grze niezbędny jest wybór "
        "urządzenia przechowywania danych.",
        "Wczytywanie %1. Proszę nie wyłączać konsoli.",
        "%1 jest prawdopodobnie uszkodzony i nie może zostać użyty.",
        "Urządzenie przechowywania danych zostało odłączone podczas działania. Błąd wczytywania.",
        "%1 już istnieje na urządzeniu przechowywania danych %2. Ponowne zapisanie o tej nazwie zastąpi uprzednio "
        "zapisany stan gry. Na pewno chcesz kontynuować?",
        "Czy na pewno chcesz zastąpić %1 na urządzeniu przechowywania danych %2?",
        "Zapisywanie %1. Proszę nie wyłączać konsoli.",
        "Nie można zapisać %1.",
        "Urządzenie przechowywania danych zostało odłączone podczas działania. Błąd zapisywania.",
        "Czy na pewno chcesz usunąć %1 na urządzeniu przechowywania danych %2?",
        "Usuwanie %1. Proszę nie wyłączać konsoli.",
        "Nie można usunąć %1.",
        "Urządzenie przechowywania danych zostało odłączone podczas działania. Błąd usuwania.",
        "Nie ma dość wolnego miejsca na urządzeniu przechowywania danych %1.",
    };

    constexpr std::pair<uint32_t, std::string_view> kPolishBin[] = {
        // OLX_OFFLINE_WARNING
        { 0x66886589, "Jeżeli zdecydujesz się pojawić jako niedostępny, nie będziesz mógł wysyłać/odbierać "
                      "zaproszeń do gry ani czatu głosowego." },
        // OL_NoXBOX_Priv
        { 0xCAB5A537, "Twoje obecne przywileje na Xbox Live nie pozwalają ci na dostęp do tej funkcji." },
        // MC_CONFIRM_SAVE_THE_PROFILE_XENON
        { 0xAE269CC1, "Czy na pewno chcesz zapisać profil o nazwie %s na urządzeniu przechowywania danych?" },
        // MC_CARD_REMOVED
        { 0xEA2E0059, "Wybrane urządzenie przechowywania danych zostało odłączone lub zamienione. Do zapisania "
                      "postępów w grze i własnych ustawień niezbędne jest urządzenie przechowywania danych." },
        // NO_STORAGE_DEVICE
        { 0x777FBCFF, "Uprzednio wybrane urządzenie przechowywania danych nie jest już dostępne. Do zapisania "
                      "postępów w grze i własnych ustawień niezbędne jest urządzenie przechowywania danych." },
        // MC_AUTOSAVE_NO_CARD_XENON
        { 0x9F47FAB2, "Funkcja autozapisu nie może zapisać postępów w grze, ponieważ wybrane urządzenie przechowywania "
                      "danych zostało zmienione lub odłączone. Czy chcesz kontynuować bez autozapisu?" },
    };

    constexpr uint32_t kMemcardLocale = 0x3A00;  // r29 - this -> locale state
    constexpr uint32_t kLocaleBuffer = 0x20;     // state -> loaded .LOC buffer
    constexpr size_t kLocBuffer = 0x1000;        // its size

    bool Polish()
    {
        return s_text.load(std::memory_order_relaxed) == kPolish;
    }

    // The Polish text for a label hash in guest memory, made once (0: none).
    uint32_t PolishText(uint32_t hash)
    {
        static std::mutex lock;
        static std::unordered_map<uint32_t, uint32_t> made;
        std::string text;
        for (const auto& [h, utf8] : kPolishBin)
            if (h == hash)
                text = ToPolishFont(utf8);
        if (text.empty())
            return 0;
        std::lock_guard guard(lock);
        uint32_t& guest = made[hash];
        if (!guest)
        {
            void* p = g_userHeap.Alloc(text.size() + 1);
            std::memcpy(p, text.c_str(), text.size() + 1);
            guest = g_memory.MapVirtual(p);
        }
        return guest;
    }
}

// After the .LOC is read, the Polish one in place of
// the stub: (sub_82289E60, called by SetCurrentLanguage)
void MemcardLocalePolish(PPCRegister& r29)
{
    static const std::vector<uint8_t> loc = BuildLoc(kPolishLoc);
    uint8_t* base = g_memory.base;
    const uint32_t state = Polish() ? PPC_LOAD_U32(r29.u32 - kMemcardLocale) : 0;
    if (const uint32_t buffer = state ? PPC_LOAD_U32(state + kLocaleBuffer) : 0)
        std::memcpy(g_memory.Translate(buffer), loc.data(),
            std::min(loc.size(), kLocBuffer));
}

// The string lookup (label hash -> text; 0 when
// missing, which the callers handle themselves).
SB_HOOK(sub_822BCE58)
{
    const uint32_t hash = ctx.r3.u32;
    SB_ORIGINAL(sub_822BCE58)(ctx, base);
    if (const uint32_t fixed = ctx.r3.u32 && Polish() ? PolishText(hash) : 0)
        ctx.r3.u64 = fixed;
}
