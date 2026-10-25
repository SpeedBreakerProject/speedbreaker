# Play testing: what to send when something goes wrong

Thanks for testing. The game keeps its own logs and can pack everything we
need into one file, so a report takes one button press and a few words
from you.

## Installing

- **Steam Deck, Steam Machine, Steam Frame (SteamOS) and Mac:** the downloads
  on the Releases page, with the setup steps in the [README](../README.md)
  (and the README inside each SteamOS download). The first launch without
  game files offers to install them from your own disc image or extracted
  folder. In Game Mode, set the shortcut's Properties > General > Game
  Resolution to **Native**.
- **From source:** the README's "Building from source", or
  [scripts/steamos/README.md](../scripts/steamos/README.md) for SteamOS.
- **Windows (a beta build for testers):** unzip the kit somewhere (not
  inside the zip) and run `SpeedBreaker.exe`; its `README-TESTER.txt` has
  the rest. It is made with the `windows-clang-tester` CMake preset (no
  console window: everything goes to the session log; labelled "Windows
  beta") and packed with `scripts/windows/make_zip.py`, which checks that no
  game file, user name or user-folder path goes in.

## The menu

- Keyboard: **F1** (on a Mac also **Cmd+,**).
- Controller: **Back + Start** together (on a Steam Deck: the View and Menu
  buttons on either side of the screen).
- The D-pad picks a row, left/right changes it, A selects, LB/RB switch
  pages, B closes. The game pauses while the menu is open.
- The **Controls** page: choose a row, Left or Right for its first or second key, press A (or
  Enter), then the key you want; Esc or B cancels. Y (Backspace) clears a key, X (Delete) restores
  the row, *Reset page* the whole table.

## When something goes wrong

1. Open the menu, go to the **Advanced** page (LB/RB, or Q/E) and choose
   **Save Bug Report** (A, or Enter).
2. A notice at the bottom of the screen says where the file went:
   `speedbreaker-bug-report-v<version>-<date>_<time>.zip` (a Windows beta adds
   its label: `...-windows-beta-<build date>-<commit>.zip`) on your
   **Desktop** (on SteamOS, `/home/deck/Desktop`: switch to Desktop Mode to
   pick it up; on iPhone and iPad, the Files app's SpeedBreaker folder; on
   Windows, the Desktop Explorer shows, OneDrive's if it backs that up), or,
   if there is no Desktop (or it can't take the file), in the game's data
   folder (below).
3. Send that .zip with a few words: what you were doing, what you expected,
   what happened, and roughly how long into the session (the logs count
   seconds from launch).

With **Smooth Motion** on (Settings > Display, on a Mac's 120 Hz screen), say so, and for a picture
problem whether it showed in a race, a cutscene or a menu: the log records each step between full
in-between pictures, lighter ones and 60 ("[hfr]" lines), and when.

If the game **crashed**, just launch it again: it says it saved a crash
report, and the next Save Bug Report includes it.

If the game **froze** (no picture change for 20 seconds, outside loading),
it writes a hang report by itself and says so. Open the menu and save a bug
report right there if you can; if you can't, quit (Steam's Exit Game, or
close the window), launch again and save one then: the hang report is kept.

A bug report holds the last five sessions' logs, any crash and hang
reports, your settings.toml, a summary of the machine (OS, CPU, memory, GPU
and driver, display, the settings in effect) and a screenshot of the last
game frame without the menu over it. Paths in your home folder show as `~`
(on Windows, your user folder shows as `%USERPROFILE%`). Nothing is sent
anywhere by the game: you send the file.

## Where the logs live

Each launch writes its own log (the newest five are kept), named after the
date and time it started, next to any `crash-*.txt` and `hang-*.txt`
reports:

- Linux / SteamOS: `~/.local/share/speedbreaker/logs/`
  (`$XDG_DATA_HOME/speedbreaker/logs/` if that is set; in portable mode,
  `data/logs/` in the game's folder, next to `portable.txt`)
- macOS: `~/Library/Application Support/SpeedBreaker/logs/`
- Windows: `%LOCALAPPDATA%\SpeedBreaker\logs\` (paste it into Explorer's
  address bar; in portable mode, `data\logs\` next to `SpeedBreaker.exe`
  and `portable.txt`). Keep `SpeedBreaker.pdb` next to `SpeedBreaker.exe`:
  crash and hang reports take the names of the game's functions from it.

On SteamOS the launcher also leaves the latest session's output in
`last-run.log`, in the game's folder (`~/Games/SpeedBreaker/` for the
download).

The first lines of every log say which build it is
(`[build] SpeedBreaker v<version> (<commit>), built ...`, or for a Windows
beta `[build] SpeedBreaker v<version> Windows beta <date> (<commit>): built
...`, also shown at the bottom of Settings > Advanced), whether this version
is new since the last run (`[version] updated from v<a> to v<b>`, or `first
run`), and describe the machine (`[system] ...`).

## Graphics shader diagnostics

To identify a draw's pixel shader, launch with `NFSMW_LOG_DRAWS=<seconds>`:
the log records one frame's draws, pixel-shader hashes and sampled texture
addresses. To skip every draw using a particular pixel shader, relaunch with
`NFSMW_SKIP_PS=<hex-hash>` (the hash after `ps_` in a `[draws]` line). This is
a diagnostic override; a shader shared by multiple effects will be skipped
in all of them.

To time the GPU intervals of selected draws in one frame, launch with
`NFSMW_SHADER_PROFILE=<frame>,<selector>`. Select a guest pixel or vertex
shader with `ps:<hex-hash>` or `vs:<hex-hash>`, or use `shadow` to select draws
to the 1600-pitch sun shadow depth target. For example,
`NFSMW_SHADER_PROFILE=450,shadow` measures the shadow-map draws in frame 450.
Use `NFSMW_LOG_DRAWS=<seconds>` in a separate run to find a frame number and
shader hashes.

The `[shader-profile]` report gives per-shader-pair draw counts and the sum,
average, and maximum of per-draw timestamp intervals. Timestamp instrumentation
is intrusive (especially on MoltenVK), and intervals may include overlapped
pipeline work; these are comparative diagnostics, not exclusive shader-core
execution times. At most 2048 selected draws are timed in the frame.

For interactive shadow timing, set `NFSMW_SHADOW_BENCHMARK=1` before launch.
Press F9 to time the next frame's complete render passes to the 1600-pitch
shadow target; press F10 to start continuous capture and press it again to
stop. The report gives the total and per-frame GPU pass intervals, pass counts,
and query availability/errors. Query pools are recycled only after their
submission fences complete, so continuous captures are not limited by a
run-wide draw count. F9 and F10-stop reports are processed after the relevant
GPU work completes, without waiting on the swap thread. A new capture cannot
start until the previous report finishes. Pass intervals include the work
between their GPU timestamps and are not exclusive shader-core time; compare
them across runs with the same route, settings, and capture duration.

To verify shadow-resolution scaling, launch with `NFSMW_TRACE_SHADOW_SCALE=1`.
The `[shadow-scale]` lines report full-map resolve eligibility, direct
depth-target sampling, and the later generic texture-cache lookup. The
`texture lookup` line says whether the scaled cached texture or the 1x texture
was selected, its dimensions, and why scaling was unavailable when applicable.
`sample fallback` only means direct depth-target sampling was not used; it does
not by itself mean the selected cached texture is 1x. The trace is off by
default and reports distinct states as the scale changes. For a useful
comparison, start in a scene with visible shadows, then try 1x and 2x Internal
Resolution and attach `last-run.log`.

To trace writes to guest physical memory, launch with
`NFSMW_TRACE_GUEST_WRITES=<hex-address>[,<hex-size>]`. The default size is
`0x40000`. The runtime watches the range from startup and logs first CPU write
faults with the guest address and game-code offset, the first overlapping GPU
write, and file reads overlapping the range with the guest file path, file
offset, destination, and byte count.
For example, `NFSMW_TRACE_GUEST_WRITES=1A71E000` watches the 256x256 RGBA
texture at `1A71E000`. This is a diagnostic option and can add write-fault
overhead while the watched pages are initialized.
