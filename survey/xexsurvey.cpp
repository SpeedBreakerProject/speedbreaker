// xexsurvey: Phase 1 static survey of a retail XEX using XenonRecomp's loader.
//
//   xexsurvey <default.xex> <out-dir>
//
// Writes <out-dir>/image.bin (the decrypted, decompressed image, loadable in
// Ghidra as raw big-endian PowerPC at the printed base) and prints sections,
// the entry point, every kernel/XAM import, and the addresses of the
// compiler helpers XenonRecomp's TOML config needs.

#include <xex.h>
#include <image.h>
#include <file.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

extern std::unordered_map<size_t, const char*> XamExports;
extern std::unordered_map<size_t, const char*> XboxKernelExports;

static uint32_t ReadBE32(const uint8_t* p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// Optional-header lookup on the raw file (headers are never encrypted).
static const uint8_t* FindOptHeader(const std::vector<uint8_t>& file, uint32_t key)
{
    uint32_t count = ReadBE32(&file[0x14]);
    for (uint32_t i = 0; i < count; i++)
    {
        const uint8_t* entry = &file[0x18 + i * 8];
        if (ReadBE32(entry) == key)
        {
            uint32_t value = ReadBE32(entry + 4);
            return (key & 0xFF) == 0 || (key & 0xFF) == 1 ? entry + 4 : &file[value];
        }
    }
    return nullptr;
}

struct Pattern
{
    const char* key;
    std::vector<uint8_t> bytes;
};

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: xexsurvey <default.xex> <out-dir>\n");
        return 1;
    }

    std::vector<uint8_t> file = LoadFile(argv[1]);
    if (file.empty())
    {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }

    Image image = Xex2LoadImage(file.data(), file.size());

    printf("## Image\n\n");
    printf("base 0x%08zX, size 0x%08X, entry 0x%08zX\n\n", image.base, image.size, image.entry_point);
    printf("| Section | Start | End | Size | Kind |\n|---|---|---|---|---|\n");
    for (auto& s : image.sections)
    {
        printf("| %s | 0x%08zX | 0x%08zX | 0x%X | %s |\n", s.name.c_str(), s.base, s.base + s.size, s.size,
            (s.flags & SectionFlags_Code) ? "code" : "data");
    }

    std::string outPath = std::string(argv[2]) + "/image.bin";
    if (FILE* f = fopen(outPath.c_str(), "wb"))
    {
        fwrite(image.data.get(), 1, image.size, f);
        fclose(f);
        printf("\nwrote %s (load as PowerPC:BE:64:A2ALT-32addr or VLE-less PPC BE at base 0x%08zX)\n", outPath.c_str(), image.base);
    }

    // Imports: walk the import-library header on the raw file; read each
    // thunk from the loaded image. The loader overwrites function thunks with
    // a nop/blr stub and names them in the symbol table; variable records keep
    // their ordinal.
    printf("\n## Imports\n\n");
    const uint8_t* importHeader = FindOptHeader(file, 0x000103FF);
    std::map<std::string, std::set<std::string>> byLibrary;
    std::map<std::string, std::set<std::string>> variablesByLibrary;
    if (importHeader != nullptr)
    {
        uint32_t stringTableSize = ReadBE32(importHeader + 4);
        uint32_t numLibraries = ReadBE32(importHeader + 8);
        std::vector<std::string> names;
        const char* strings = reinterpret_cast<const char*>(importHeader + 12);
        size_t off = 0;
        for (uint32_t i = 0; i < numLibraries; i++)
        {
            names.emplace_back(strings + off);
            off += ((names.back().size() + 1) + 3) & ~size_t(3);
        }

        const uint8_t* lib = importHeader + 12 + stringTableSize;
        for (uint32_t i = 0; i < numLibraries; i++)
        {
            uint32_t libSize = ReadBE32(lib);
            uint16_t nameIndex = (lib[0x24] << 8) | lib[0x25];
            uint16_t count = (lib[0x26] << 8) | lib[0x27];
            const std::string& libName = names[nameIndex];
            const auto* table = libName == "xam.xex" ? &XamExports
                : libName == "xboxkrnl.exe" ? &XboxKernelExports : nullptr;

            for (uint16_t im = 0; im < count; im++)
            {
                uint32_t thunkAddr = ReadBE32(lib + 0x28 + im * 4);
                auto sym = image.symbols.find(thunkAddr);
                if (sym != image.symbols.end() && sym->address == thunkAddr)
                {
                    byLibrary[libName].insert(sym->name);
                    continue;
                }

                auto* thunk = static_cast<const uint8_t*>(image.Find(thunkAddr));
                if (thunk == nullptr)
                    continue;

                // Loader byteswapped this record to host order.
                uint32_t data;
                memcpy(&data, thunk, 4);
                uint32_t ordinal = data & 0xFFFF;
                uint32_t type = data >> 24;
                const char* name = nullptr;
                if (table != nullptr)
                {
                    auto it = table->find(ordinal);
                    if (it != table->end())
                        name = it->second;
                }

                char buf[64];
                if (name == nullptr)
                {
                    snprintf(buf, sizeof(buf), "ordinal_%u", ordinal);
                    name = buf;
                }

                if (type == 0)
                    variablesByLibrary[libName].insert(name);
                else
                    byLibrary[libName].insert(name);
            }
            lib += libSize;
        }
    }

    size_t total = 0;
    for (auto& [lib, fns] : byLibrary)
    {
        // A variable record that pairs a named function is just its IAT slot.
        std::set<std::string> dataOnly;
        for (auto& v : variablesByLibrary[lib])
            if (!fns.count(v))
                dataOnly.insert(v);

        printf("### %s: %zu functions, %zu data imports\n\n", lib.c_str(), fns.size(), dataOnly.size());
        for (auto& fn : fns)
            printf("- %s\n", fn.c_str());
        for (auto& v : dataOnly)
            printf("- %s (data)\n", v.c_str());
        printf("\n");
        total += fns.size() + dataOnly.size();
    }
    printf("total distinct imports: %zu\n", total);

    // Compiler helpers: byte patterns from the XenonRecomp README.
    const Pattern patterns[] = {
        { "restgprlr_14_address", { 0xE9, 0xC1, 0xFF, 0x68 } },
        { "savegprlr_14_address", { 0xF9, 0xC1, 0xFF, 0x68 } },
        { "restfpr_14_address", { 0xC9, 0xCC, 0xFF, 0x70 } },
        { "savefpr_14_address", { 0xD9, 0xCC, 0xFF, 0x70 } },
        { "restvmx_14_address", { 0x39, 0x60, 0xFE, 0xE0, 0x7D, 0xCB, 0x60, 0xCE } },
        { "savevmx_14_address", { 0x39, 0x60, 0xFE, 0xE0, 0x7D, 0xCB, 0x61, 0xCE } },
        { "restvmx_64_address", { 0x39, 0x60, 0xFC, 0x00, 0x10, 0x0B, 0x60, 0xCB } },
        { "savevmx_64_address", { 0x39, 0x60, 0xFC, 0x00, 0x10, 0x0B, 0x61, 0xCB } },
    };

    printf("\n## Compiler helpers\n\n");
    for (auto& p : patterns)
    {
        std::vector<size_t> hits;
        for (auto& s : image.sections)
        {
            if (!(s.flags & SectionFlags_Code))
                continue;
            for (size_t i = 0; i + p.bytes.size() <= s.size; i += 4)
            {
                if (memcmp(s.data + i, p.bytes.data(), p.bytes.size()) == 0)
                    hits.push_back(s.base + i);
            }
        }

        printf("%s =", p.key);
        for (size_t h : hits)
            printf(" 0x%08zX", h);
        printf("%s\n", hits.empty() ? " (not found)" : hits.size() > 1 ? "   <-- multiple hits, check by hand" : "");
    }

    printf("\n## Import stubs\n\n");
    for (auto& sym : image.symbols)
    {
        if (sym.name.rfind("__imp__", 0) == 0)
            printf("0x%08zX %s\n", sym.address, sym.name.c_str());
    }

    return 0;
}
