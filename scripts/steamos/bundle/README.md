# SpeedBreaker for Steam Deck, Steam Machine and Linux PCs

An unofficial static recompilation of the Xbox 360 version of Need for Speed:
Most Wanted (2005): a native port, not an emulator. A fan project by
project(u), not affiliated with, endorsed by or sponsored by Electronic Arts
or Valve.

This is the x86-64 SteamOS download (the Steam Frame has its own, the arm64
one). This folder has the launcher, `speedbreaker.sh`, and in `lib/` the
game program and the libraries it needs. **Always start the game with
`speedbreaker.sh`** (your Steam shortcut does): the program in `lib/` can't
start on its own, since the launcher picks the libraries it runs with. It has
**no game data**: you need your own disc image of your own Xbox 360 disc of
the game (an `.iso`), and the game installs itself from it on the first run.
`VERSION` says which release this is.

It runs on SteamOS as it is and needs no distrobox or developer tools. Tested
on a Steam Deck OLED (SteamOS 3.9) and a Steam Machine (SteamOS 3.8). On other
Linux distributions it hasn't been tried yet; run `speedbreaker.sh` there too.

## 1. Unpack

In Desktop Mode (Steam button > Power > Switch to Desktop):

1. Make a `Games` folder in your home folder (Dolphin: Home, right-click >
   Create New > Folder), if there isn't one.
2. Move the `.tar.xz` file into it, right-click it > Extract > Extract
   archive here. You get `~/Games/SpeedBreaker/`.

Or in a terminal (Konsole):

    mkdir -p ~/Games && tar -xf ~/Downloads/SpeedBreaker-steamos-x86_64.tar.xz -C ~/Games

To check the download first (optional), with its `.sha256` file next to it:
`cd ~/Downloads && sha256sum -c SpeedBreaker-steamos-x86_64.tar.xz.sha256`
should say OK.

Keep the folder on the internal storage or on an SD card that SteamOS
formatted (ext4): SteamOS doesn't mount a card formatted for Windows (FAT32,
exFAT, NTFS) in Game Mode, and its files can lose the "executable" mark the
program needs. No `:`, `;` or `$`
in the names of the folders it's in (not "Need for Speed: Most Wanted"): the
game can't start from there.

To update later, unpack the newer download over the same folder (the same
command, or Extract archive here, letting it overwrite). Your Steam shortcut
keeps working, and your saves, settings and the installed game are kept
(they're elsewhere, below), so no disc image is needed. Over v0.1.0, which
kept the program at the top of this folder, the first start removes that old
copy. Settings menu > Advanced > **Check for Updates** says whether a newer
one is out. Nothing is checked online unless you press Check for Updates.

## 2. Add it to Steam

Still in Desktop Mode: Steam > Games > Add a Non-Steam Game to My Library >
Browse. Set the file type to "All Files", pick
`~/Games/SpeedBreaker/speedbreaker.sh`, then Add Selected Programs. You can rename
the shortcut (it's called `speedbreaker.sh`) in its Properties. Leave its
Compatibility setting off: this is a Linux program, and Proton can't run it.

Then go back to Game Mode and start it from your library (Non-Steam).

## 3. Your disc image

Put your `.iso` in **Downloads** (`~/Downloads`), or on the **SD card**
(its top folder, or one folder down). The first run looks there by itself
(also in your home folder, Desktop, Documents, Games, USB drives and
EmuDeck's `Emulation/roms/xbox360`), so you can pick it with the controller.
SteamOS only mounts an SD card or USB drive by itself when it's formatted the
Deck's way (ext4): one written on a PC (exFAT, FAT32, NTFS) isn't there in
Game Mode, so in Desktop Mode open it in Dolphin and copy the `.iso` into
Downloads.

The installer highlights the image it found (marked "Ready to install"; with
several, pick yours with Up and Down): press A, then A on Install. Use the
images it finds rather than "Browse for image...", whose file browser may not
show in Game Mode.
It checks the image first (the right game and version, nothing missing),
then copies and checks every file: about 7 GB, a few minutes at most. You
choose where it goes with left and right: the internal storage, the SD card,
or "Choose a folder...", which opens a folder picker (press A on it). An
empty folder is used as it is; a folder that already holds files gets a new
`SpeedBreaker Game` folder inside it (the top of a card or drive gets
`speedbreaker/game`, like the SD card's own entry). The picker opens in
Desktop Mode only: in Game Mode the installer says so. Pick a folder on the
internal storage or on a card or drive formatted by SteamOS, or Game Mode
won't find the game. Afterwards you can delete the `.iso`.

From a terminal, the same install: `~/Games/SpeedBreaker/speedbreaker.sh --install ~/Downloads/<your image>.iso`
(add `--dest <folder>` to install into exactly that folder: the game
remembers it, in Game Mode too).

## Controls

- The Deck's controls work as an Xbox 360 controller through Steam Input
  (leave it on, with the default Gamepad layout).
- **Settings menu**: hold View (the button left of the screen, two squares)
  and press Menu (right of the screen, three lines). The game pauses while
  it's open. D-pad picks, left/right changes, A toggles, L1/R1 turn pages,
  B closes. With a keyboard: F1.
- Stick deadzones and vibration are in the settings menu.

## Screen

- The Deck's screen is 16:10 and the game is 16:9. By default (Settings >
  Display > Aspect Ratio: Fill Screen) it fills the screen with a slightly
  taller view, the HUD kept at 16:9; choose 16:9 there for the console's
  picture with thin bars above and below.
- **Steam Deck OLED: set the refresh rate to 60 Hz** (Quick Access menu >
  Performance, the battery icon > Refresh Rate; turn on Advanced View if it
  isn't shown). The game runs at 60 frames a second and times itself to a
  screen at about 60 Hz. At 90 Hz it can't: its frames then stay on screen
  for one refresh, then two, which looks like stutter. On either model,
  keep the refresh rate at 60 (lowering it to save battery brings the same
  stutter) and leave the Framerate Limit off.
- Docked to a TV or monitor: in the shortcut's Properties > General > Game
  Resolution, choose **Native**, or Steam gives the game 1280x800 of the
  bigger screen.

## Performance

On a Steam Deck OLED it runs at 60 frames a second most of the time at its
own resolution; the busiest scenes can dip below. A Steam Machine holds 60 at
3440x1440, Internal Resolution 3x2 included. To see what yours does: Settings
menu > Advanced > Performance Overlay, or Steam's own performance overlay
(Quick Access > Performance).

## Reporting problems

Settings menu > Advanced > **Save Bug Report** puts the recent logs, any
crash reports, your settings, a system summary and a screenshot in one .zip
on your Desktop (`speedbreaker-bug-report-v<version>-<date>_<time>.zip`).
Nothing is uploaded: attach it to an issue at
https://github.com/SpeedBreakerProject/speedbreaker/issues and say roughly
when in the session it happened (in minutes). Please never attach game files
or disc images.

Each run also writes `last-run.log` in this folder (`~/Games/SpeedBreaker/`);
the run before is kept as `previous-run.log`. If the game crashes, the log
ends with where in the program it was.

## Where things are

- Saves and `settings.toml`: `~/.local/share/speedbreaker/`.
- The installed game: `game/` in that folder, or the SD card's
  `speedbreaker/game`, or the folder you chose. The game remembers where it
  is; if that card or drive isn't there when it starts, it says so.
- Shader caches (safe to delete): `~/.cache/speedbreaker/`.
- To uninstall: delete this folder, `~/.local/share/speedbreaker/` (that
  deletes your saves too), `~/.cache/speedbreaker/` and the game's folder
  if you chose one, and remove the shortcut from Steam.

## Licenses

SpeedBreaker is free software under the GNU General Public License, version
3 or later (GPL-3.0-or-later), Copyright (C) 2026 project(u) and SpeedBreaker
contributors. What it is built from and the bundled libraries keep their own
licenses: see `licenses/` (`THIRD_PARTY_NOTICES.md` lists them all). The
source is at https://github.com/SpeedBreakerProject/speedbreaker.
`bundle-info.txt` says what this build is and `SHA256SUMS` lists every file's
checksum.

"Need for Speed" and "Most Wanted" are trademarks of Electronic Arts Inc.
Steam, Steam Deck, Steam Machine, Steam Frame and SteamOS are trademarks of
Valve Corporation.
