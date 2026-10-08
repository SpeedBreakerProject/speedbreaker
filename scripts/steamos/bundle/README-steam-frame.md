# SpeedBreaker for Steam Frame

An unofficial static recompilation of the Xbox 360 version of Need for Speed:
Most Wanted (2005): a native port, not an emulator. A fan project by
project(u), not affiliated with, endorsed by or sponsored by Electronic Arts
or Valve.

This is the arm64 SteamOS download, for the Steam Frame (the Steam Deck,
Steam Machine and Linux PCs have their own, the x86-64 one). In the headset
the game plays on a big flat screen: it is not a VR mode. This folder has the
game program, the libraries it needs and a launcher. It has **no game data**:
you need your own disc image of your own Xbox 360 disc of the game (an
`.iso`), and the game installs itself from it on the first run. `VERSION`
says which release this is.

It runs on the Frame's SteamOS as it is (tested on SteamOS 0.3.0) and needs
no developer tools. About 60 MB, plus 7.2 GB for the installed game.

## 1. Open the Frame's desktop

Start the **Desktop** app from your Steam library: it opens a desktop
window in the headset, with a file manager (Dolphin) and a terminal
(Konsole).

## 2. Unpack

Download `SpeedBreaker-steamos-arm64.tar.xz` (and its `.sha256` file) into
your Downloads folder. The Frame's file manager can't unpack it, so open
**Konsole** and run:

    mkdir -p ~/Games && tar -xf ~/Downloads/SpeedBreaker-steamos-arm64.tar.xz -C ~/Games

You get `~/Games/SpeedBreaker/`. To check the download first (optional):
`cd ~/Downloads && sha256sum -c SpeedBreaker-steamos-arm64.tar.xz.sha256`
should say OK.

Keep the folder on the Frame's internal storage, and don't use `:`, `;` or
`$` in the names of the folders it's in: the game can't start from there.

To update later, unpack a newer download over the same folder. Your saves,
settings and the installed game are kept elsewhere (below).

## 3. Add it to Steam

In the file manager, open `~/Games/SpeedBreaker`, right-click
`speedbreaker.sh` and choose **Add to Steam**. Or, in Konsole:

    steamos-add-to-steam ~/Games/SpeedBreaker/speedbreaker.sh

You can rename the shortcut (it's called `speedbreaker.sh`) in its
Properties. Leave its Compatibility setting (Proton) off: this is a Linux
program, and Proton can't run it.

## 4. Your disc image

Put your `.iso` in **Downloads** (`~/Downloads`). The first run also looks in
your home folder, Desktop, Documents, Games and on USB drives.

## 5. Install and play

Start SpeedBreaker from your Steam library (Non-Steam). The first run opens
the installer, with the image it found highlighted (marked "Ready to
install"): press A, then A again on Install. It checks the
image first (the right game and version, nothing missing), then copies and
checks every file: about 7 GB, a few minutes at most. Afterwards you can
delete the `.iso`.

From Konsole, the same install:
`~/Games/SpeedBreaker/speedbreaker.sh --install ~/Downloads/<your image>.iso`

## Controls and settings

- The Frame's controllers work as an Xbox 360 controller.
- **Settings menu**: press **Menu + View** together. The game pauses while
  it's open. D-pad picks, left/right changes, A toggles, L1/R1 turn pages,
  B closes.
- **Frame rate**: in the headset, the VR compositor shares the graphics
  chip, so set **Display > Frame Rate** to **30** for smooth play. The game
  keeps its full speed at 30.

## Reporting problems

Settings menu > Advanced > **Save Bug Report** puts the recent logs, any
crash reports, your settings, a system summary and a screenshot in one .zip
on your Desktop (open it from the Desktop app's file manager). Nothing is
uploaded: attach it to an issue at
https://github.com/SpeedBreakerProject/speedbreaker/issues and say roughly
when in the session it happened (in minutes). Please never attach game files
or disc images.

Each run also writes `last-run.log` in this folder (`~/Games/SpeedBreaker/`);
the run before is kept as `previous-run.log`.

## Where things are

- The installed game, saves and `settings.toml`: `~/.local/share/speedbreaker/`.
- Shader caches (safe to delete): `~/.cache/speedbreaker/`.
- To uninstall: delete this folder, `~/.local/share/speedbreaker/` (that
  deletes your saves too) and `~/.cache/speedbreaker/`, and remove the
  shortcut from Steam.

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
