# Texture mods

Replacement textures live in folders under `<data>/mods/<key>`. Each mod gets a
row in Settings and can be downloaded from the asset repository. The list of
mods is `mods::kMods` in `mods.h`.

## The asset repository

`manifest.json` sits on the main branch of the repository:

```json
{"mods":{"<key>":{"tag":"<key>-v1","file":"<key>-v1.zip"}, ...}}
```

- Every key is a folder in `mods/` (`[a-z0-9_-]`, 1-64 characters).
- Every tag `<key>-v<N>` is a release with `<file>` (a stored zip with `<key>/`
  at the root) and `<file>.sha256` (a line from sha256sum).
- Entries are shown in the order they appear in the manifest. Any other
  members are ignored.
- The installed set keeps its tag in `<key>/VERSION`.

## Inside the data folder

```
mods/<key>/                                the installed set, VERSION is written last
mods_update/<file>.part, <file>.sha256     a download and the hash it should have
mods_update/<key>/                         an unpacked set waiting to be moved into mods/
```

## Download and install

`download.cpp` runs a download job (see `download/download.h`). Its resolve
step reads the manifest to find the mod's release, and its finish step unpacks
the zip to `mods_update/<key>/` (VERSION last) and places the set. The download
dialog (`ui.h`) shows the progress and then offers to turn the mod on.

If `mods/<key>` has no textures, nothing from it is in use, so the new set
moves in right away. Otherwise it waits for the next start and moves in before
the game indexes mods. The move is a single rename, or a copy with VERSION
written last if `mods/` is on another drive. If the game crashes halfway, the
waiting set is still whole and the next start just does it again.

On start, every waiting set newer than the installed one moves in, and the rest
(older ones, ones without VERSION, unknown names) get deleted. Files sitting
directly in `mods_update/` (downloads) are left alone, and the folder goes away
once it's empty. Download parts and hash files that no manifest entry names
anymore are deleted.

## Mod state

For each mod we keep track, for the session, of what's on disk (rescanned when
a download places a set), the version in the manifest, and whether its
Settings row needs a restart (`settings::SetModWaiting`). A mod is active this
session if its setting was on at start and `mods/<key>` had textures at that
point (`PresentAtBoot`). Files that show up later wait for a restart.

The manifest version is offered as an update when it's newer than both the
installed set and the one waiting to be swapped in.

## Update check

`download.cpp` also handles our side of the update check (`update/updater.h`):
newer versions in the manifest, and the tags the startup toast has already
announced (`<data>/updates/notified_mods.txt`, one per line).

## Texture files

- `<TEX>.dds`
- `<TEX>.dds.zst`: packed with zstd
- `<TEX>.dds.sblink`: a single UTF-8 line with the path of a `.dds` (or
  `.dds.zst`) in the mod folder, used for duplicates

Names and extensions match regardless of case. If one texture comes in several
forms, plain `.dds` wins over `.zst`, and `.zst` wins over a link. Other files
(`NOTICE`, `LICENSE`) are skipped, as are top level folders starting with `_`
(`_overrides`). The game
side (hooks, and files replacing textures through `gpu/texture_override.h`)
lives in `game/mods.cpp`.

## Apple

Mods are turned off with `SB_NO_MODS` (`runtime/CMakeLists.txt`) on macOS and
iOS for now, since they haven't been tested there. That means no Mods page, no
manifest check, no downloads and no texture mods: `StartSession` does nothing,
and `game/mods.cpp`, `gpu/texture_override.cpp` and zstd aren't built. To
enable them on Apple, drop the `if(APPLE)` in CMake, implement `update::Head`
and `update::Download` in `platform/apple/update_fetch.mm`, and remove their
stubs from `update/fetch.h`.

## Tests

`tests/mods_test.cpp` runs `mods.cpp` against a scratch folder.
