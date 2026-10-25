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
