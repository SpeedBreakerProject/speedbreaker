# Downloads

Downloads one big file at a time on its own thread. Anything that hands over a
`download::Task` can use it; right now that's `mods/`. `ui/download_dialog.cpp`
runs the job and shows its progress.

## Phases

- **Contacting**: `task.resolve` says what to fetch (`Target`). Then the job
  gets the SHA-256 (either given, or read from `<url>.sha256`) and the size
  from a HEAD request.
- **Ready**: offers Download, or Resume if part of the file is already there.
- **Downloading**: writes to `<folder>/<name>.part`, picking up from whatever
  is already there. The expected hash is saved next to it as
  `<folder>/<name>.sha256`.
- **Checking**: hashes the whole file.
- **Installing**: `task.finish` puts the verified file to use (unpacks it, etc).
- **Done**, or **Failed** with one line for the player and one for the log
  (`[download] ...`).

## What stays on disk

- A part that got cut short (network drop, Stop, quitting) is kept, so the next
  try can resume it.
- A part is deleted if its hash changed, if the download turned out damaged or
  got rejected, or if it ran into a full or read-only disk. The file is also
  deleted once `finish` has used it, and so is `<folder>` if that leaves it
  empty.
- If the game exits while a job is running, the job is stopped (takes about two
  seconds) and the part is kept.

## Unpacking (unpack.h)

Handles what downloads bring along: sha256sum lines and stored zips (JSON
manifests go through `update/json.h`). It's plain logic on bytes, apart from `OpenZip` and `ExtractZip`,
which read the zip file and write out its contents. Tests are in
`tests/download_unpack_test.cpp`.

A zip is only accepted if:

- every entry is stored (method 0) and not encrypted
- every entry is under `<root>/` (or anywhere, if `root` is empty)
- no path has `..`, `.`, empty, absolute or drive parts, backslashes or
  control characters
- there are no symlinks and no duplicates
- local headers come before the directory
- there's at least one file

Anything else is Rejected. If the records themselves don't add up, it's
Damaged. Zip64, multi-disk archives and oversized central directories are
Rejected too.
