# The German (PAL) disc: how its tables were made

SpeedBreaker runs the German disc (*Need for Speed - Most Wanted (Germany)*,
`default.xex` SHA-256 `23ece29d18724ea28971d52f936964d127148bd2359ff24795da7442b9069bf4`)
the way it runs the Spanish one: its own recompiled code (`ppc-pal-de/`,
namespace `pal_de`) beside the USA's, with the runtime's hooks moved to its
addresses. Building it is README step 2. This page says where its committed
files come from, how to check them, and how to redo them, for a change to the
runtime's hooks or for another region.

## The committed files

| File | Written by |
|---|---|
| `config/pal-de/nfsmw.toml`, `config/pal-de/switch_tables.toml` | `survey/region_map.py toml --region pal-de` |
| `runtime/region/pal_de_addresses.inc`, `runtime/region/pal_de_names.h` | `survey/region_map.py table --region pal-de` |
| `runtime/install/manifest_pal_de.inc` | `scripts/gen_install_manifest.py` |

They hold addresses, names, sizes and hashes only, as their Spanish
counterparts do. The German manifest is a game manifest only in a build that
links `ppc-pal-de/` (`SB_PAL_DE`); a build without it, like the downloads for
now, refuses the German disc up front and says it needs a build with its code.
Without the images, `tests/region_addresses_check.py . --region pal-de` checks
that `config/pal-de/` is still `config/` translated: a hook added to
`config/nfsmw.toml` and missing here fails it.

## What the survey found

The German `default.xex` is laid out like the Spanish one: 22,679 functions,
none missing, all but 19 the same code as the USA's (modulo the addresses in
it), in 17 pieces moved by -0x28..+0x200; `.data` and `.bss` where the USA's
are; the same two extra leaf functions (the date and number formats, at
0x823AF018 and 0x823AF0F8). Its executable boots in German (language 2), asks
for `_pal` movies, and the disc holds German movies and the German police radio
(`copspeech_ge`). Its `default.xex` has a size of its own (3,944,448 bytes).

Tested on Linux (RADV): it boots, plays the German movies and the police radio
in German, and saves to `save-pal-de/`.

## Redoing them

Needs both discs' `default.xex` (the USA one too: the German addresses are
found against it) and the toolchain from README step 2. From the repository
root:

```sh
# Each executable decrypted, with its section table.
mkdir -p build game/survey/mw_usa game/survey/mw_de
clang++ -std=c++20 -O2 -include cstdlib -Itools/XenonRecomp/XenonUtils -Itools/XenonRecomp/thirdparty/simde \
  survey/xexsurvey.cpp tools/XenonRecomp/build/XenonUtils/libXenonUtils.a \
  tools/XenonRecomp/build/thirdparty/disasm/libdisasm.a -o build/xexsurvey
build/xexsurvey game/files/default.xex game/survey/mw_usa > game/survey/mw_usa/xexsurvey.txt
build/xexsurvey game/files-pal-de/default.xex game/survey/mw_de > game/survey/mw_de/xexsurvey.txt

# Each USA function's counterpart in the German build.
python3 -I survey/func_map.py game/survey/mw_usa game/survey/mw_de > game/survey/mw_de/funcmap.tsv

# Check the committed files against the images (writes nothing) ...
S=game/survey
python3 -I tests/region_addresses_check.py . $S/mw_usa $S/mw_de $S/mw_de/funcmap.tsv --region pal-de

# ... or write them again (after a change to the runtime's hooks).
python3 -I survey/region_map.py toml  $S/mw_usa $S/mw_de $S/mw_de/funcmap.tsv --region pal-de
python3 -I survey/region_map.py table $S/mw_usa $S/mw_de $S/mw_de/funcmap.tsv --region pal-de
python3 scripts/gen_install_manifest.py game/files-pal-de --config config/pal-de/nfsmw.toml \
  -o runtime/install/manifest_pal_de.inc
```

`cmake -DSB_REGION_SURVEY=game/survey` with `mw_de/` beside `mw_usa/` makes
ctest's `region_addresses_check_pal_de` do the same check.

## Another region

The same tools take another PAL disc, given a region entry in
`survey/region_map.py` (`REGIONS`), `runtime/region/region.h`/`.cpp`,
`install/manifest.cpp` and `game/language.h` (`DiscLanguage`). What to look
at on the way:

- `func_map.py`'s summary: how many functions are the same, changed or
  missing, and in how many pieces the layout moved. Far more than the 19
  changed functions here means more work below.
- `region_map.py` stops with every address it can't map, each with the reason
  (code the other build changed or lacks, an `.rdata` item it can't find,
  `.data moved`). `region_addresses_check` also fails for a runtime address
  inside a changed function: review those hooks.
- `region_map.py` lists the jump tables only the other build has, with the
  bounds it found; `NOT a bl target` marks a start to check by hand.
- XenonRecomp's `Found a switch jump table ... with no switch table entry` or
  `Switch case ... is trying to jump outside function` means a table or bounds
  the config still lacks.
