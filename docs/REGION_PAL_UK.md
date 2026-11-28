# The UK (PAL, English) disc: how its tables were made

SpeedBreaker can run the UK disc (*Need for Speed - Most Wanted*, Europe,
English; `default.xex` SHA-256
`e2ea2ff11bf84cb30430d824d7bfe699a3acf3e7400d21b9a7c938e3d7a10474`, title
454107D9, media ID 36775B18) the way it runs the Spanish and German ones: its
own recompiled code (`ppc-pal-uk/`, namespace `pal_uk`) beside the USA's, with
the runtime's hooks moved to its addresses. Building it is README step 2. A
build without `ppc-pal-uk/` (`SB_PAL_UK`) refuses the UK disc up front and says
it needs a build with its code.

**Status:** the tables are checked against the UK and USA images (bytes at all
105 address pairs), and XenonRecomp generates the UK code from them with no
errors and no missing jump tables. Tested on Windows: it boots in English and
plays the English movies.

## The committed files

| File | Written by |
|---|---|
| `config/pal-uk/nfsmw.toml`, `config/pal-uk/switch_tables.toml` | `survey/region_map.py toml --region pal-uk` |
| `runtime/region/pal_uk_addresses.inc`, `runtime/region/pal_uk_names.h` | `survey/region_map.py table --region pal-uk` |
| `runtime/install/manifest_pal_uk.inc` | `scripts/gen_install_manifest.py` |

They hold addresses, names, sizes and hashes only.

## What's different about this disc

- Its `default.xex` is **the Spanish one's size** (3,940,352 bytes), with a
  different hash. The installer and the startup used to tell the regions apart
  by size alone, so `install::ManifestForXexFile` now hashes the file when two
  releases share a size.
- Against the USA build (`survey/func_map.py`): 22,679 functions, none missing,
  all but 19 the same code (modulo the addresses in it), in 16 pieces moved by
  -0x18..+0x240 (the German build has 19 changed functions too). Against the Spanish build it is **the same code moved in 4 places**:
  +0x10 from about 0x8225AE84, +0x28 from about 0x823A2400, +0x48 from about
  0x8251583C and +0x50 from about 0x82626A00 (Spanish addresses). `.data`,
  `.tls` and `.XEXID` are where the USA and Spanish builds have them. It has the
  PAL build's two extra leaf functions (the date and number formats), +0x28 from
  the Spanish ones (0x823AF040 and 0x823AF120).
- Its executable boots in **English** (`li r3, 0` before the boot's
  SetCurrentLanguage call), asks for `_pal` movies, and the disc holds 36
  `*_english_pal.wmv` movies. Saves go to `save-pal-uk/`.

## Two ways to the same tables

The tables were first made without a USA image, by `survey/sibling_map.py`:
it maps the Spanish committed tables to the UK layout and checks the result
against the UK image alone (anchors from the Spanish jump tables found at one
delta, monotonic pieces, `HAND_ANCHORS` for the addresses between pieces, each
with its evidence, and checks on function starts, leaf ends, compiler helpers
and `.rdata`). Later `survey/region_map.py`, with a USA image, wrote the same
tables the usual way: the same 105 address pairs, 44 names, functions and 531
jump tables, only the comments differ. The committed files are region_map.py's;
`sibling_map.py check` still passes on them (it compares content, not
comments), for a UK disc owner without a USA image.

## Redoing them

Needs both discs' `default.xex` (the USA one too: the UK addresses are found
against it) and the toolchain from README step 2. From the repository root:

```sh
mkdir -p build game/survey/mw_usa game/survey/mw_uk
clang++ -std=c++20 -O2 -include cstdlib -Itools/XenonRecomp/XenonUtils -Itools/XenonRecomp/thirdparty/simde \
  survey/xexsurvey.cpp tools/XenonRecomp/build/XenonUtils/libXenonUtils.a \
  tools/XenonRecomp/build/thirdparty/disasm/libdisasm.a -o build/xexsurvey
build/xexsurvey game/files/default.xex game/survey/mw_usa > game/survey/mw_usa/xexsurvey.txt
build/xexsurvey game/files-pal-uk/default.xex game/survey/mw_uk > game/survey/mw_uk/xexsurvey.txt
python3 -I survey/func_map.py game/survey/mw_usa game/survey/mw_uk > game/survey/mw_uk/funcmap.tsv

# Check the committed files against the images (writes nothing) ...
S=game/survey
python3 -I tests/region_addresses_check.py . $S/mw_usa $S/mw_uk $S/mw_uk/funcmap.tsv --region pal-uk

# ... or write them again (after a change to the runtime's hooks).
python3 -I survey/region_map.py toml  $S/mw_usa $S/mw_uk $S/mw_uk/funcmap.tsv --region pal-uk
python3 -I survey/region_map.py table $S/mw_usa $S/mw_uk $S/mw_uk/funcmap.tsv --region pal-uk
python3 scripts/gen_install_manifest.py game/files-pal-uk --config config/pal-uk/nfsmw.toml \
  -o runtime/install/manifest_pal_uk.inc
```

Without a USA image, XenonAnalyse's tables for the UK image and
`survey/sibling_map.py` check them (or write them, from `config/pal-es/`):

```sh
tools/XenonRecomp/build/XenonAnalyse/XenonAnalyse game/files-pal-uk/default.xex game/survey/mw_uk/xenonanalyse.toml
python3 -I tests/region_addresses_check.py . - $S/mw_uk - --region pal-uk
python3 -I survey/sibling_map.py write game/survey/mw_uk --region pal-uk --analyse game/survey/mw_uk/xenonanalyse.toml
```

`cmake -DSB_REGION_SURVEY=game/survey` with `mw_uk/` there makes ctest's
`region_addresses_check_pal_uk` run the check: against both images when
`mw_uk/funcmap.tsv` and `mw_usa/` are there, else with sibling_map.py.
