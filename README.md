<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/speedbreaker-banner-dark.png">
    <img alt="SpeedBreaker" src="assets/speedbreaker-banner-light.png" width="560">
  </picture>
</p>

<p align="center">
  <b>An unofficial static recompilation of the Xbox 360 version of <i>Need for Speed: Most Wanted</i> (2005):<br>
  a native port, not an emulator.<br>
  Play your own copy on Steam Deck, Steam Machine and Steam Frame (SteamOS), Linux and Mac,<br>
  or build it yourself for iPhone and iPad.</b>
</p>

<p align="center">
  <a href="https://github.com/SpeedBreakerProject/speedbreaker/releases/latest"><b>Download</b></a>
  &nbsp;·&nbsp;
  <a href="#quick-start">Quick start</a>
  &nbsp;·&nbsp;
  <a href="https://projectugarte.com/speedbreaker/">Website</a>
  &nbsp;·&nbsp;
  <a href="#reporting-a-bug">Report a bug</a>
</p>

<p align="center">
  <img src="assets/screenshots/speedbreaker-00-ultrawide-chase-cam-race.jpg" alt="A race on a 21:9 ultrawide screen, seen from the camera behind the player's car: an open expressway into a low sun, with the race HUD and the rear-view mirror" width="100%">
  <br>
  <sub>What you see in a race, on a 21:9 ultrawide: a wider view, not a stretched one, with the HUD and the mirror where they belong.</sub>
</p>

> [!IMPORTANT]
> **SpeedBreaker is an unofficial fan project by project(u). It is not affiliated with, endorsed
> by or sponsored by Electronic Arts Inc., the Need for Speed brand or Valve Corporation.**
>
> **No game files are included or distributed.** To play, you need your own legally obtained
> backup (a disc image) of your own Xbox 360 disc of *Need for Speed: Most Wanted* (2005). The
> in-app installer verifies it and installs it. Please don't ask for, or share, game files or
> disc images. See the full [legal notice](#legal-notice).

**Contents:** [What it is](#what-is-speedbreaker) · [Highlights](#highlights) ·
[Download](#download) · [Quick start](#quick-start) · [How to update](#how-to-update) ·
[Setup by platform](#setup-by-platform) ·
[Controls](#controls) · [Settings](#settings) · [Tips](#tips-for-smooth-play) ·
[FAQ](#faq-and-troubleshooting) · [What's next](#whats-next) ·
[For developers](#for-developers) · [Credits](#credits) · [License](#license) ·
[Legal notice](#legal-notice)

## What is SpeedBreaker?

SpeedBreaker is an unofficial static recompilation of the Xbox 360 version of *Need for Speed:
Most Wanted* (2005): a native port, not an emulator. It runs as an ordinary app on your device
(see [how it works](#how-it-works)).

It is built from the **Xbox 360 version** of the game, not the PC version: it runs the Xbox 360
release's own code, translated ahead of time, and it installs only from the Xbox 360 disc.

You bring the game: SpeedBreaker installs it from a backup of your own disc and checks every
file. Then you race, with things the Xbox 360 couldn't do: up to 4K, ultrawide and tall screens,
and a settings menu you can open mid-race.

## Highlights

- **Up to 4K.** The 3D scene renders at 1x, 2x or 3x the console's 720p (3x is 3840x2160).
  *Auto* picks a scale for your screen and device.
- **Ultrawide done right.** On 21:9 screens the view gets wider instead of stretched, and the
  HUD and the rear-view mirror stay in place. A 4:3 iPad, a 3:2 screen or the 16:10 screens of
  the Steam Deck and many Macs show more above and below instead, and fill the screen.
- **Up to 60 fps,** locked to your 60 Hz display. You can also pick a steady 30 at full game
  speed, or *Auto*, which drops to 30 only while a device is hot or can't hold 60.
- **The sun glare is back.** The low sun glows in open sky and fades behind buildings, as it did
  on the console.
- **The original look:** sunlit roads and shadows as on the console, correct colours on Apple
  devices, proper car paint and mirror reflections, and distant textures that don't shimmer. Anti-aliasing, sharpening and up to 16x
  anisotropic filtering are also available.
- **A guided installer.** It finds your disc image, checks that it is the right game, and
  verifies all 49 files as it copies them. An interrupted install never leaves a broken game.
- **Your controller, or the keyboard.** Controllers work as an Xbox 360 pad, with hot-plugging
  and rumble support.
- **A settings menu that pauses the game.** Open it with F1, Back + Start, or a three-finger
  tap. Every change applies at once.
- **Suspend and resume on iPhone and iPad.** Switch away and the game freezes; come back and it
  carries on where it stopped.
- **One-button bug reports.** Logs, settings and a screenshot go into one zip file. Nothing is
  uploaded, and you decide what to send.
- **Check for Updates.** One press in the settings asks GitHub whether a newer release is out,
  shows what changed and opens the download page (or shows it as a QR code for your phone).
  Nothing is checked online unless you press Check for Updates.

<table>
  <tr>
    <td width="50%"><img src="assets/screenshots/speedbreaker-x4-ultrawide-race-sun-sparks.jpg" alt="Racing at 21:9 into a low sun, sparks flying off the wall, HUD and rear-view mirror visible"><br><sub>21:9 in a race: the wider view and the low sun's glare.</sub></td>
    <td width="50%"><img src="assets/screenshots/speedbreaker-03-4x3-cinematic-flame-livery-closeup.jpg" alt="A close-up of a black car with flame livery, in a 4:3 frame"><br><sub>4:3, the shape of an iPad screen, with more picture above and below.</sub></td>
  </tr>
  <tr>
    <td width="50%"><img src="assets/screenshots/speedbreaker-01-ultrawide-cinematic-rivals-side-by-side.jpg" alt="The intro cinematic in 21:9: two cars side by side"><br><sub>The intro cinematic at 3440x1440.</sub></td>
    <td width="50%"><img src="assets/screenshots/speedbreaker-04-4k-cinematic-pursuit-crash.jpg" alt="Police cars crashing into a barrier in a shower of sparks"><br><sub>A pursuit cinematic at 3840x2160.</sub></td>
  </tr>
  <tr>
    <td width="50%"><img src="assets/screenshots/speedbreaker-10-4k-settings-internal-resolution.png" alt="The SpeedBreaker settings menu over the paused race, on the Graphics page"><br><sub>The settings menu over a paused race.</sub></td>
    <td width="50%"><img src="assets/screenshots/speedbreaker-11-installer-disc-image-found.png" alt="The first-run installer, showing a disc image it found, ready to install"><br><sub>First run: the installer has found the player's disc image.</sub></td>
  </tr>
</table>

<sub>Screenshots: SpeedBreaker on a Mac Studio (M4 Max). They are unedited frames from the game's
own output, except that the licence plate in the top image is blurred. The game content in them
belongs to its owners.</sub>

## Download

| Platform | How to get it | Tested on |
|---|---|---|
| **Steam Deck** and **Steam Machine** (SteamOS) | [Download][dl-steamos-x86_64]<br>`SpeedBreaker-steamos-x86_64.tar.xz` | Steam Deck OLED (SteamOS 3.9.2). Steam Machine (SteamOS 3.8) on a 3440x1440 ultrawide. The Steam Deck LCD hasn't been tested yet. |
| **Steam Frame** (SteamOS, arm64) | [Download][dl-steamos-arm64]<br>`SpeedBreaker-steamos-arm64.tar.xz` | Steam Frame (SteamOS 0.3.0). It plays as a big flat screen in the headset; it is not a VR mode. |
| **Linux** (x86-64) | [Download][dl-steamos-x86_64] the x86-64 SteamOS build, `SpeedBreaker-steamos-x86_64.tar.xz`, or [build from source](#building-from-source) | SteamOS only so far. Other distributions and NVIDIA or Intel GPUs are untested: reports welcome. |
| **Mac** (Apple silicon) | [Download][dl-macos]<br>`SpeedBreaker-macos.dmg` | MacBook Pro (M1 Pro) and Mac Studio (M4 Max), on macOS 26; macOS 15.6 (Sequoia) in a virtual machine. There is no Intel Mac build. |
| **iPhone** and **iPad** | [Build it yourself](#iphone-and-ipad) with Xcode | iPhone 15 Pro Max (iOS 27). iPad Pro 12.9-inch (M2, iPadOS 27) with a Backbone Pro. Not on the App Store. |
| **Windows** | Coming soon | |
| **Android** | Coming soon | |

Each link downloads the newest release. Every file and its `.sha256` checksum file are on the
[latest release page][releases]. To check a download, put its `.sha256` file next to it and run
`shasum -a 256 -c SpeedBreaker-macos.dmg.sha256` on a Mac, or
`sha256sum -c SpeedBreaker-steamos-x86_64.tar.xz.sha256` on SteamOS (with your file's name): it
should say OK. What changed in each release is in [CHANGELOG.md](CHANGELOG.md).

### System requirements

- **Everyone:** a disc image of your own **USA (NTSC-U) Xbox 360 disc** (see
  [Quick start](#quick-start)), about **7.2 GB** of free space, and a display running at about
  **60 Hz**.
- **Steam Deck, Steam Machine and Linux PCs** (the x86-64 SteamOS download): a 64-bit x86 CPU
  with AVX2 (x86-64-v3): Intel Haswell (2013) or newer, or AMD Excavator, Zen or newer. A
  Vulkan 1.2 graphics card.
- **Steam Frame** (the arm64 SteamOS download): SteamOS on the Steam Frame. The arm64 download
  is for the Frame only; it hasn't been tested on any other arm64 Linux device.
- **Mac:** Apple silicon and **macOS 15** (Sequoia) or later. The app won't open on older
  versions of macOS, and there is no Intel Mac build.
- **iPhone and iPad:** iOS or iPadOS 17 or later, on an A15-class chip or newer (iPhone 13 and
  later, M-series iPads), and a controller or keyboard. Devices with less memory than the tested
  ones are untested.

## Quick start

1. **Back up your own disc.** SpeedBreaker supports one version of the game: the **USA (NTSC-U,
   English) Xbox 360 disc**. The installer takes a full `.iso`, a trimmed XISO, or a folder with
   the extracted disc files. No title update is needed. SpeedBreaker doesn't provide game files
   or disc images, or link to them.
2. **Get SpeedBreaker.** [Download it](#download) for Steam Deck, Steam Machine and Steam Frame
   (SteamOS), Linux or Mac. On iPhone and iPad you build it yourself. Then follow the
   [setup for your platform](#setup-by-platform).
3. **Start it and install.** On first run, SpeedBreaker looks for your disc image, checks that
   it is the right game and version, then copies and verifies every file. This takes a few
   minutes at most. Afterwards you can delete the disc image.

   <img src="assets/screenshots/speedbreaker-12-installer-every-file-verified.png" alt="The installer's last page: every file matched the known-good disc" width="600">

4. **Race.** Your saves and settings are kept apart from the game, so they stay when you
   [update SpeedBreaker](#how-to-update) or reinstall the game. The settings open with **F1**, **Back + Start** on a
   controller, or a **three-finger tap** on iPhone and iPad.

### How to update

- **Mac:** drag the new SpeedBreaker into Applications, choose **Replace**, then allow it once more with **Open Anyway** (see [Mac setup](#mac)).
- **Steam Deck, Steam Machine, Steam Frame and Linux:** unpack the new download over the old folder (`~/Games/SpeedBreaker`); your Steam shortcut keeps working.
- **Everywhere:** your saves, settings and the installed game are kept, and no disc image is needed. Settings > Advanced > **Check for Updates** says when there's a new release, and so does GitHub's **Watch > Custom > Releases**.

## Setup by platform

### Steam Deck and Steam Machine

1. **Download and unpack** (in Desktop Mode). Download the SteamOS build,
   [`SpeedBreaker-steamos-x86_64.tar.xz`][dl-steamos-x86_64]. Make a `Games` folder in your home
   folder, move the file there, and choose *Extract archive here*. Or, in Konsole:

   ```sh
   mkdir -p ~/Games && tar -xf ~/Downloads/SpeedBreaker-steamos-x86_64.tar.xz -C ~/Games
   ```

   Keep it on internal storage or on an SD card formatted by SteamOS, and don't use `:`, `;` or
   `$` in the folder names. To update later, unpack the new download over the same folder
   ([How to update](#how-to-update)).

2. **Add it to Steam.** Steam > Games > *Add a Non-Steam Game to My Library* > Browse. Set the
   file type to *All Files* and pick `~/Games/SpeedBreaker/speedbreaker.sh`, the launcher (the
   program itself is in `lib/`). Leave *Compatibility* (Proton) off: this is a native Linux game.
3. **Put your disc image in Downloads,** or in the top folder of an SD card. SpeedBreaker also
   looks in your home folder, Desktop, Documents, Games, USB drives and EmuDeck's
   `Emulation/roms/xbox360`. Game Mode only sees SD cards and USB drives formatted by SteamOS;
   for a card formatted on a PC, copy the image into Downloads first.
4. **Install** (back in Game Mode). Start SpeedBreaker from your library. It highlights the image
   it found (*Ready to install*; with several, pick yours with Up and Down): press **A**, then
   **A** again on *Install*. Use the images it finds rather than *Browse for image...*, which may
   not open in Game Mode. The game goes to the internal storage; to put it on the SD card or in
   another folder, press Up and pick with Left and Right before *Install*. *Choose a folder...*
   opens a folder picker in Desktop Mode only. From Game Mode, switch to Desktop Mode and choose
   the folder there, or run `~/Games/SpeedBreaker/speedbreaker.sh --install <image> --dest <folder>`
   from Konsole in Desktop Mode (or over SSH). Use a folder on the internal storage or on a card
   formatted by SteamOS, or Game Mode won't find the game.
5. **Check the screen settings.**
   - **Steam Deck OLED: set the refresh rate to 60 Hz** (Quick Access > Performance). At 90 Hz
     the game stutters. On any Deck, leave the Framerate Limit off.
   - **Docked, or on a Steam Machine:** in the shortcut's Properties > General > Game
     Resolution, choose **Native**.
   - The picture fills the Deck's 16:10 screen, with a slightly taller view and the HUD kept at
     16:9. For the console's 16:9 picture with thin bars above and below, set Settings > Display
     > Aspect Ratio to 16:9.

**Controls:** the Deck's controls work through Steam Input (default Gamepad layout).
**Settings:** hold **View** (two squares, left of the screen) and press **Menu** (three lines,
right of the screen).

**Uninstall:** delete `~/Games/SpeedBreaker/`, `~/.local/share/speedbreaker/` (this deletes your
saves too) and `~/.cache/speedbreaker/`, and remove the shortcut from Steam.

### Steam Frame

The Steam Frame has its own download. You set it up on the Frame's desktop: open the
**Desktop** app in your Steam library.

1. **Download and unpack.** Download the Steam Frame build,
   [`SpeedBreaker-steamos-arm64.tar.xz`][dl-steamos-arm64], into Downloads (not
   `SpeedBreaker-steamos-x86_64.tar.xz`: that one is for the Steam Deck, Steam Machine and Linux
   PCs). The Frame's file manager can't unpack it, so open **Konsole** and run:

   ```sh
   mkdir -p ~/Games && tar -xf ~/Downloads/SpeedBreaker-steamos-arm64.tar.xz -C ~/Games
   ```

   This makes `~/Games/SpeedBreaker`. Keep it on the Frame's internal storage, and don't use `:`,
   `;` or `$` in the folder names. To update later, unpack the new download over the same folder
   ([How to update](#how-to-update)).

2. **Add it to Steam.** In the file manager, open `~/Games/SpeedBreaker`, right-click
   `speedbreaker.sh` and choose *Add to Steam*. Or, in Konsole:
   `steamos-add-to-steam ~/Games/SpeedBreaker/speedbreaker.sh`. Leave *Compatibility* (Proton)
   off: this is a native Linux game.
3. **Put your disc image in Downloads.** SpeedBreaker also looks in your home folder, Desktop,
   Documents and Games.
4. **Install.** Start SpeedBreaker from your Steam library. It highlights the image it found
   (*Ready to install*): press **A**, then **A** again on *Install*. Or install
   from Konsole: `~/Games/SpeedBreaker/speedbreaker.sh --install ~/Downloads/your-image.iso`
   (with your image's name; add `--dest <folder>` to put the game in a folder of your choice).

SpeedBreaker plays as a big flat screen in the headset. It is not a VR mode.

- **Settings:** press **Menu + View** together.
- **Frame rate:** set **Display > Frame Rate** to **30 fps** for smooth play. In the headset,
  the VR compositor shares the graphics chip. The game keeps its full speed at 30.
- **Uninstall:** as on the [Steam Deck](#steam-deck-and-steam-machine).

### Linux

There's no separate desktop-Linux package yet. Try the x86-64 SteamOS download,
[`SpeedBreaker-steamos-x86_64.tar.xz`][dl-steamos-x86_64], which brings its own runtime libraries
but hasn't been tested outside SteamOS, or [build from source](#building-from-source). Unpack it,
put your `.iso` in your home folder, Downloads, Desktop, Documents or Games (or on a USB drive),
and run `speedbreaker.sh`: always start the game with it, since it picks the libraries the
program in `lib/` needs (started on its own, the program fails on most systems with a message
such as `GLIBC_2.44 not found`). The game starts in a window; Display > Fullscreen switches to full
screen. The installer puts the game in `~/.local/share/speedbreaker/game`, or on a drive, or in
any folder you pick with *Choose a folder...* (it needs a file chooser portal or zenity; without
one, use `--dest`, below).

### Mac

1. **Download and install.** Download [`SpeedBreaker-macos.dmg`][dl-macos], open it, and drag
   SpeedBreaker to your Applications folder.
2. **Open it.** The app isn't notarized by Apple, so the first time you open it, macOS refuses.
   Close that message (don't move the app to the Trash), then go to **System Settings > Privacy &
   Security**, scroll down to the message about SpeedBreaker, click **Open Anyway** and confirm.
   You only need to do this the first time you open a new download. On current macOS,
   Control-click > Open no longer skips this step.
3. **Put your disc image in a `Games` folder in your home folder** (`~/Games`). That's the only
   place SpeedBreaker looks by itself, so macOS doesn't have to ask for access to your other
   folders. For an image anywhere else, use *Browse for image...*.
4. **Install.** The game goes to `~/Library/Application Support/SpeedBreaker/game`, or to an
   external drive or any other folder if you choose one (*Choose a folder...*, with the arrows on
   the *To* row). An empty folder is used as it is; a folder that already holds files gets a new
   `SpeedBreaker Game` folder inside it. For a folder in Desktop, Documents or Downloads, or on an
   external drive, macOS asks once whether SpeedBreaker may use it: choose **Allow**. iCloud Drive
   can't hold the game (nor Desktop and Documents, if your Mac keeps them in iCloud): macOS would
   upload it and can remove its files from the Mac to save space.

**Settings:** **F1** or **Cmd+,**, or Back + Start on a controller. The game starts in a window;
Display > Fullscreen switches to full screen. SpeedBreaker needs macOS 15 (Sequoia) or later.

### iPhone and iPad

There's no download for iPhone and iPad, and SpeedBreaker isn't on the App Store. You build it
yourself with Xcode on a Mac and install it on your own device, signed with your own Apple
developer account: [scripts/ios/README.md](scripts/ios/README.md) walks you through it.

Once SpeedBreaker is on your device:

1. **Get your disc image onto it.** Copy the `.iso` into **Files > On My iPhone (or iPad) >
   SpeedBreaker**, where it is found by itself. Or tap **Browse** and pick it from anywhere the
   Files app reaches, such as iCloud Drive or a USB drive. A browsed image is read where it is,
   not copied first.
2. **Install.** The game goes into the app's own storage, and is left out of iCloud and computer
   backups (your saves are still backed up). Afterwards you can delete the disc image.
3. **Connect a controller or a keyboard.** The game itself doesn't use touch. Pair a Bluetooth
   controller, such as an Xbox or PlayStation one, or use a Backbone. So far, a Backbone Pro on
   the iPad is the one tested.

**Settings:** tap with **three fingers**, or press Back + Start on the controller (View + Menu on
a Backbone). SpeedBreaker runs in landscape, full screen. If you switch away, the game freezes,
and it resumes exactly where it stopped. If 60 fps isn't steady on your iPhone, set Display >
Frame Rate to 30 fps or Auto.

## Controls

SpeedBreaker hands the game an Xbox 360 controller, so a race plays with the game's own Xbox 360
controls. Buttons are matched **by position**: on a PlayStation controller, Cross is A, Circle
is B, Square is X and Triangle is Y.

| Xbox 360 control | Controller | Keyboard |
|---|---|---|
| Left stick | Left stick | A / D steer (the arrow keys also move it) |
| Right trigger | Right trigger | W |
| Left trigger | Left trigger | S |
| A / B / X / Y | Bottom / right / left / top face button | Space / Backspace / Left Shift / Left Ctrl |
| LB / RB | Shoulder buttons | Q / E |
| D-pad | D-pad | Arrow keys |
| Start | Start / Menu / Options | Enter |
| Back | Back / View / Create | Esc |
| Right stick, stick clicks | Same on the controller | C (right stick click) |

- One player, one controller: the first one connected is used, and you can plug one in at any
  time. The keyboard always works as well.
- Because Back + Start opens the settings, the game sees Back a moment late: a tap arrives when
  you let go, a hold after a quarter of a second.
- There is no button remapping and no mouse control in game.

**To open the settings:**

| Device | Open or close the settings |
|---|---|
| Keyboard | **F1** (on a Mac also **Cmd+,**) |
| Xbox-style controller | **Back + Start** (View + Menu) together |
| Steam Deck | Hold **View**, press **Menu** |
| Steam Frame | **Menu + View** together |
| iPhone and iPad touchscreen | **Three-finger tap** |

## Settings

The game pauses while the settings are open. The D-pad (or arrow keys) chooses and changes a row,
LB and RB (or Q and E) turn pages, and B (or Esc) closes the menu. On a touchscreen, tap and
drag. The mouse works too. Every change applies at once and is saved when you close the menu.
Each page has a *Reset page* button.

<details>
<summary><b>All settings and their defaults</b></summary>

**Display**

| Option | Choices | Default |
|---|---|---|
| Fullscreen | On / Off | Off: a 1280x720 window you can resize. Always on in SteamOS Game Mode. Not shown on iPhone and iPad, which are always full screen. |
| V-Sync | On / Off | On. Off can tear, but shows frames sooner. |
| Frame Rate | Auto / 60 fps / 30 fps | 60 fps on most devices. 30 fps keeps the game at full speed. Auto runs at 60, or at a steady 30 while the device is hot or can't hold 60. |
| Screen Resolution | Full / 75% | Full. iPhone and iPad only: 75% is slightly softer and saves graphics work. |
| Aspect Ratio | Fill Screen / 16:9 | Fill Screen: wider screens get a wider view; 4:3, 3:2 and 16:10 screens (the Steam Deck, many Macs) a taller one. 16:9 always shows the classic picture with bars. |

**Graphics**

| Option | Choices | Default |
|---|---|---|
| Anti-Aliasing | On / Off | On |
| Upscaling | Bicubic / Bilinear | Bicubic |
| Sharpening | 0.00 to 1.00 | 0.30 (0 turns it off) |
| Anisotropic Filtering | Auto / Off / 2x / 4x / 8x / 16x / Original | Auto: 16x on dedicated graphics cards and M-series iPads, 4x elsewhere. Original uses each texture's own setting, as on the 360. |
| Mipmaps | On / Off | On, so distant textures don't shimmer |
| Internal Resolution | Auto / 1x (720p) / 2x (1440p) / 3x (2160p) | Auto: enough to cover your screen, within what the device can afford. On iPhone and iPad, Auto always picks 1x. |

**Audio:** Master Volume (100%), Mute (Off), and Mute in Background (Off), which silences the
game while its window isn't in front.

**Input:** Stick Deadzone (8%; raise it if the car steers by itself), Trigger Deadzone (2%), and
Vibration (On).

**Advanced:** Asynchronous Shaders (On: an object is skipped for a frame or two while its shader
compiles, instead of the game stuttering), Performance Overlay (Off: fps, frame times, and CPU
and GPU time per frame), **Save Bug Report**, and **Check for Updates** (the only thing that goes
online, and only when you press it). The page also shows which build you're running.

</details>

## Tips for smooth play

- **Run your display at 60 Hz.** On a Steam Deck OLED, set the refresh rate to 60 Hz and leave
  Steam's Framerate Limit off.
- **Start with Auto.** If busy scenes drop below 60, set Internal Resolution one step lower, or
  let Frame Rate *Auto* fall back to a steady 30.
- **30 fps is a real option.** It's a steady 30 at full game speed, and the smooth choice in the
  Steam Frame headset.
- **On iPhone and iPad,** keep Internal Resolution at Auto (1x). On the tested iPad Pro (M2), 2x
  is too slow for races. Screen Resolution 75% saves more.
- **Docked Deck or Steam Machine:** set the shortcut's Game Resolution to Native.
- **See what's happening:** Settings > Advanced > Performance Overlay.

## FAQ and troubleshooting

**Does SpeedBreaker include the game?**
No. No game files are included or distributed. You install the game from your own backup of
your own disc, and SpeedBreaker checks it file by file.

**Which version of the game do I need?**
The Xbox 360 **USA release (NTSC-U, English)**. The installer checks your image and tells you if
it's a different version. PAL and Japanese discs and re-releases haven't been tested, so expect
the installer to refuse them. Games on Demand packages aren't supported, and no title update is
used.

**Does it work with the PC version of the game?**
No. SpeedBreaker is built from the Xbox 360 version and installs only from the Xbox 360 USA disc.
PC copies of *Most Wanted*, including the Black Edition, aren't supported.

**Is this an emulator?**
No. It's a static recompilation: the game's Xbox 360 code was translated ahead of time into a
native app, so that code runs directly instead of being emulated. SpeedBreaker supplies what the
game expects from the Xbox 360: its system software, graphics, audio and controllers.

**Can it run above 60 fps?**
No. 60 fps is the game's own cap.

**Can I bring my Xbox 360 saves?**
No. Xbox 360 saves can't be imported, so your career starts fresh.

**The installer refused my image. Why?**
It always says what to do. The usual causes:

- *Another game, or an unsupported version:* it isn't the USA disc.
- *Truncated image:* most often a copy cut off at 4 GB by a FAT32 drive. Copy it again from your
  own backup, onto a drive that can hold files over 4 GB.
- *Not a disc image,* a Games on Demand package, or a video-only DVD image.
- *Missing or corrupt files:* make a new backup from your disc.

**My Mac says SpeedBreaker can't be opened.** Use **Open Anyway** in System Settings > Privacy &
Security (see [Mac setup](#mac)), again after each update. SpeedBreaker needs macOS 15 (Sequoia)
or later on Apple silicon.

**The game stutters on my Steam Deck OLED.** Set the screen to 60 Hz (see
[Steam Deck setup](#steam-deck-and-steam-machine)).

**The picture is small or stretched on my TV or monitor in Game Mode.** Set the shortcut's
Properties > General > Game Resolution to Native.

**Can I have the bars back on my Deck?** Yes. The game is 16:9 and the Deck's screen 16:10, and
since v0.1.1 the picture fills it with a slightly taller view. Set Settings > Display > Aspect
Ratio to 16:9 for the console's picture with thin bars above and below.

**My SD card or USB drive doesn't show up in Game Mode.** Game Mode only sees drives formatted by
SteamOS. Copy the image into Downloads from Desktop Mode.

**Can I install the game somewhere other than the default folder?** Yes. On the installer's last
page, choose where it goes on the *To* row: internal storage, a removable drive, or *Choose a
folder...* for any folder (SteamOS: in Desktop Mode). An empty folder is used as it is; a folder
that already holds files gets a new `SpeedBreaker Game` folder inside it, and the top of a drive
gets the drive's own `speedbreaker/game` (`SpeedBreaker/game` on a Mac). Or install from a
terminal with `--dest` (see [Command-line installer](#command-line-installer)). SpeedBreaker
remembers the place; if it's on a drive that isn't connected when the game starts, the installer
says so: connect it and start the game again.

**The car steers by itself.** Raise Settings > Input > Stick Deadzone.

**The game ignores my touches on iPhone or iPad.** The game needs a controller or a keyboard.
Touch only works in SpeedBreaker's own screens; a three-finger tap opens the settings.

**Where are my saves, settings and logs?**

| | Linux and SteamOS | Mac | iPhone and iPad |
|---|---|---|---|
| Saves, settings, logs | `~/.local/share/speedbreaker/` | `~/Library/Application Support/SpeedBreaker/` | Inside the app (not shown in the Files app) |
| Installed game | `game/` in that folder, `speedbreaker/game` on a drive, or the folder you chose | `game/` in that folder, `SpeedBreaker/game` on a drive, or the folder you chose | Inside the app |
| Shader caches (safe to delete) | `~/.cache/speedbreaker/` | `~/Library/Caches/SpeedBreaker/` | Inside the app |

Deleting that folder, or deleting the iPhone or iPad app, deletes your saves. A game you
installed in a folder of your own stays there until you delete that folder.

### Reporting a bug

1. Open the settings, go to **Advanced**, and choose **Save Bug Report**.
2. A notice says where the zip file went: your **Desktop** (on SteamOS, pick it up in Desktop
   Mode, or in the Desktop app on a Steam Frame), the **SpeedBreaker** folder in the Files app on
   iPhone and iPad, or the data folder above when there is no Desktop. Its name says which
   version you were running, for example `speedbreaker-bug-report-v0.1.1-<date>_<time>.zip`.
3. Open an [issue][issues], attach the zip, and say what you were doing, what you expected, what
   happened, and roughly how long into the session it happened.

The zip holds recent logs, any crash and hang reports, your settings, a summary of your system
and a screenshot of the last game frame. Paths in your home folder show as `~`. If the game
crashed, start it again and save a report: the crash report is included. **Never attach game
files or disc images to an issue.** [CONTRIBUTING.md](CONTRIBUTING.md) says how bug reports and
pull requests are handled.

## What's next

- **Windows:** coming soon.
- **Android:** coming soon.

**One more thing… SpeedBreaker: Carbon, coming soon.** It's planned, not available yet.

There are no dates and no promises: it's ready when it's ready.

## For developers

### How it works

SpeedBreaker is a **static recompilation**. [XenonRecomp](https://github.com/hedge-dev/XenonRecomp)
translates the game's PowerPC code into C++ ahead of time, and that C++ is compiled into a
native program for x86-64 or arm64. A runtime written for this project stands in for the Xbox
360: its kernel and system software, its GPU, its XMA audio and its controllers.

- **Graphics:** a Vulkan renderer that translates the Xenos GPU's commands and shaders. On Mac,
  iPhone and iPad it runs on Metal through MoltenVK.
- **Platform layer:** SDL3 for windows, input and audio. The installer, the settings menu and the
  overlay are drawn with Dear ImGui.
- **Audio:** XMA decoding done the way the hardware does it, frame by frame, with a patched,
  decoder-only FFmpeg.
- **Resolution:** the 3D scene renders at 1x, 2x or 3x; the bloom and haze stay at the original
  resolution, so the game keeps its colour grade.
- **Frame pacing:** the game's clock locks to the display's measured refresh (58.5 to 61.5 Hz),
  one game step per refresh. 30 fps runs two game steps per frame.
- **Sun glare:** the game measures the sun with occlusion queries; SpeedBreaker counts them on
  the GPU.

The game's data (cars, city, audio, movies) comes from the player's disc. SpeedBreaker contains
no game data or assets.

### Command-line installer

The installer also runs from a terminal. On SteamOS and Linux, run it through `speedbreaker.sh`
(`~/Games/SpeedBreaker/speedbreaker.sh --install ...`); on a Mac, the program is
`SpeedBreaker.app/Contents/MacOS/SpeedBreaker`.

```sh
SpeedBreaker --install <image or folder> [--dest <dir>]   # install (into exactly <dir>, if given)
SpeedBreaker --verify <image or folder>                   # check every file without copying
SpeedBreaker --find-images                                # list the disc images it can find
SpeedBreaker --where                                      # show where the game is installed
```

Without `--dest` the game goes to the default folder, as from the installer screen. `--dest`
installs into exactly that folder, which must be new, empty or hold a previous install (not a
temporary folder kept in memory, such as `/tmp` on SteamOS); the game remembers it.

### Building from source

The repository contains no game code: the build recompiles your own `default.xex`, from the USA
disc. The generated code (`ppc/`), the game files (`game/`) and the toolchain (`tools/`) are
ignored by git. **Never commit or share them.**

The supported executable is 3,936,256 bytes, with SHA-256
`aebdf3c14d1e4e4ed2c130d94bc1a0e97bd6d5bf39bdfdef33c3909c9cc4ba4c`.

#### 1. Dependencies

- **Mac:** Xcode's command-line tools, and from Homebrew: `brew install llvm lld cmake ninja
  molten-vk`. The libraries the app bundles (SDL3, the Vulkan loader, glslang, SPIRV-Tools and
  FFmpeg's XMA decoder) are built from their own source releases by
  `scripts/macos/build_deps.sh`, for macOS 15 like the release (step 3). Homebrew's bottles of
  them (`brew install sdl3 vulkan-loader vulkan-headers glslang spirv-tools`) also work, for an
  app that runs only on the macOS you build on (step 3).
- **SteamOS:** build inside an Arch Linux distrobox (podman and distrobox ship with SteamOS). The
  development launcher, `scripts/steamos/speedbreaker.sh`, expects the container to be called
  `nfsmw-build`:

  ```sh
  distrobox create --yes --name nfsmw-build --image docker.io/library/archlinux:latest \
    --additional-packages "clang lld cmake ninja git python sdl3 vulkan-headers \
      vulkan-icd-loader vulkan-tools glslang spirv-tools mesa vulkan-radeon pkgconf \
      make diffutils patch curl nasm libpulse pipewire pipewire-pulse alsa-lib libdecor"
  distrobox enter nfsmw-build
  ```

- **Other Linux distributions:** your distribution's equivalents of the package list above
  (Clang 18 or newer). Untested so far. The GPU needs Vulkan 1.2 and BC (DXT) texture support.
- `gh`, logged in, is optional: it fetches Xenia's instruction tests.

#### 2. Toolchain, your game files, recompilation

From the repository root:

```sh
scripts/setup_tools.sh    # XenonRecomp + SpeedBreaker's patches, extract-xiso, FFmpeg's XMA decoder

# Extract your own disc image into game/files and check the executable.
mkdir -p game/files
tools/extract-xiso/build/extract-xiso -d game/files -x /path/to/your-backup.iso
shasum -a 256 game/files/default.xex
# must print aebdf3c14d1e4e4ed2c130d94bc1a0e97bd6d5bf39bdfdef33c3909c9cc4ba4c

# Translate the game's code into C++ (writes ppc/).
mkdir -p ppc
tools/XenonRecomp/build/XenonRecomp/XenonRecomp config/nfsmw.toml tools/XenonRecomp/XenonUtils/ppc_context.h
```

#### 3. Build

```sh
# Linux and SteamOS (on SteamOS, inside the container)
cmake -S . -B build/main -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++

# Mac (Homebrew's LLVM): the libraries for macOS 15, as the release (a few minutes; it downloads
# their source archives and checks each one's SHA-256), then the game against them
scripts/macos/build_deps.sh build/deps
cmake -S . -B build/main -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm/bin/clang \
  -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ \
  -DCMAKE_PREFIX_PATH="$PWD/build/deps" -DFFMPEG_XMA="$PWD/build/deps/ffmpeg-xma"
# (or, with Homebrew's libraries instead of build_deps.sh, an app for your own macOS only: the
#  same cmake line with -DCMAKE_OSX_DEPLOYMENT_TARGET=<your macOS, e.g. 26.0> instead of the last
#  two options; make_app.py names the version if it's wrong)

ninja -C build/main SpeedBreaker
```

Run `build/main/runtime/SpeedBreaker` from the repository root. With the whole disc extracted in
`game/files`, it plays from there. Otherwise the first-run installer asks for your disc image.

#### 4. Package it (optional)

- **Mac app:** `python3 scripts/macos/make_app.py` makes `build/app/SpeedBreaker.app`, with its
  libraries, MoltenVK and the license notices inside. It refuses a library built for a newer
  macOS than the build (and says how to fix it); `python3 scripts/macos/check_app.py
  build/app/SpeedBreaker.app` runs the release's checks. Add `--dmg` for
  `build/app/SpeedBreaker-macos.dmg` and its `.sha256`, laid out like the download on Releases.
- **SteamOS bundle (x86-64):** inside the container, `scripts/steamos/make_bundle.sh` makes
  `build/bundle/SpeedBreaker-steamos-x86_64.tar.xz` and its `.sha256` (plus a copy named by the
  version, `SpeedBreaker-<version>-steamos-x86_64.tar.xz`): `speedbreaker.sh` at the top, the
  program and its libraries in `lib/`. Install it as described in
  [Steam Deck setup](#steam-deck-and-steam-machine).
- **Steam Frame bundle (arm64):** the Steam Frame download on Releases is built on a Frame, and
  so is your own: the Frame's SteamOS ships clang, CMake, Ninja, glslang and the Vulkan headers,
  so no container is needed. Build an SDL3 release into `~/deps/sdl3` (with
  `-DSDL_X11_XSCRNSAVER=OFF`), and do step 2 on the Frame (another machine's FFmpeg build won't
  link there). Then:

  ```sh
  cmake -S . -B build/main -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ -DSDL3_DIR=$HOME/deps/sdl3/lib/cmake/SDL3 \
    -DCMAKE_SKIP_BUILD_RPATH=ON
  ninja -C build/main
  scripts/steamos/make_bundle.sh build/main build/bundle
  ```

  This makes `build/bundle/SpeedBreaker-steamos-arm64.tar.xz` and its `.sha256` (plus a copy
  named by the version). `-DCMAKE_SKIP_BUILD_RPATH=ON` keeps your `~/deps` folder out of the
  program: the bundle brings SDL3 in its own `lib/`. Install it as described in [Steam Frame setup](#steam-frame).
- **iPhone and iPad:** `scripts/ios/build_deps.sh` builds the iOS libraries, and
  `scripts/ios/build.sh` builds, signs and installs the app. The steps, and signing with your own
  Apple developer account, are in [scripts/ios/README.md](scripts/ios/README.md).

#### Tests

The unit tests build with `-DSB_TESTS=ON` added to the `cmake` line, and
`ctest --test-dir build/main` runs them. `tests/run_instr_tests.sh` checks the instructions
SpeedBreaker's patches add to XenonRecomp. It needs LLVM's `llvm-mc` and `llvm-objdump` and
`ld.lld`: on a Mac, Homebrew's `llvm` and `lld`; elsewhere, your distribution's (on Arch, the
`llvm` and `lld` packages; the container above has only `lld`). `LLVM=<bin dir>` and
`LLD=<ld.lld>` pick others. So far it has been run on a Mac only. None of the tests needs game
files; `write_watch_test` needs `ppc/`.

## Credits

SpeedBreaker is made by **project(u)**. It stands on the shoulders of a lot of open-source work.
Thank you to the **hedge-dev** team, for XenonRecomp and for the approach Unleashed Recompiled
pioneered that this project follows, and to the **Xenia** project, for a decade of Xbox 360
research.

| Project | What SpeedBreaker uses it for | License |
|---|---|---|
| [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) (hedge-dev and contributors) | The PowerPC-to-C++ recompiler. Its XenonUtils (XEX loading) is built into the runtime. | MIT |
| [Unleashed Recompiled](https://github.com/hedge-dev/UnleashedRecomp) (hedge-dev and contributors) | The reference runtime. Parts of the kernel, XAM, memory and threading code are adapted from it. | GPL-3.0-or-later |
| [Xenia](https://github.com/xenia-project/xenia) (Ben Vanik and Xenia contributors) | GPU register, microcode and Xenos headers are ported from it, and its kernel, XMA and GPU behaviour is the reference throughout. | BSD-3-Clause |
| freedreno (Rob Clark) | Code inside the ported microcode header | MIT |
| [FFmpeg](https://ffmpeg.org/) 7.1.1 | XMA decoding: a decoder-only static build with a SpeedBreaker patch | LGPL-2.1-or-later |
| [SDL3](https://www.libsdl.org/) | Windows, input, audio, app lifecycle | zlib |
| [Dear ImGui](https://github.com/ocornut/imgui) (Omar Cornut) | The installer and settings UI | MIT |
| [Roboto](https://fonts.google.com/specimen/Roboto) (Google) | The UI font | Apache-2.0 |
| [o1heap](https://github.com/pavel-kirienko/o1heap) (Pavel Kirienko) | The guest heap allocator | MIT |
| [glslang](https://github.com/KhronosGroup/glslang) and [SPIRV-Tools](https://github.com/KhronosGroup/SPIRV-Tools) (Khronos) | Shader compilation at runtime | BSD-3-Clause and others; Apache-2.0 |
| [MoltenVK](https://github.com/KhronosGroup/MoltenVK) and the [Vulkan Loader](https://github.com/KhronosGroup/Vulkan-Loader) (Khronos) | Vulkan on Mac, iPhone and iPad | Apache-2.0 |
| [toml++](https://github.com/marzer/tomlplusplus) (Mark Gillard) | `settings.toml` | MIT |
| [SIMDe](https://github.com/simd-everywhere/simde) (Evan Nemerson and contributors) | SIMD in the recompiled code | MIT |
| [libmspack](https://github.com/kyz/libmspack) (Stuart Caie) | LZX decompression | LGPL-2.1 |
| [tiny-AES-c](https://github.com/kokke/tiny-AES-c) | XEX decryption | Unlicense |
| [TinySHA1](https://github.com/mohaps/TinySHA1) (Saurav Mohapatra) | SHA-1, via XenonUtils | ISC-style |
| [extract-xiso](https://github.com/XboxDev/extract-xiso) | Developer tool for unpacking disc images (not shipped) | Its own license |

The x86-64 SteamOS download also carries glibc and libstdc++ (LGPL-2.1+; GPL-3.0 with the GCC
Runtime Library Exception); the Steam Frame download uses the system's. Full license texts are in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and [LICENSES/](LICENSES/), and every download
carries them too: in `licenses/` in the SteamOS downloads, and in
`SpeedBreaker.app/Contents/Resources/licenses` in the Mac app, with each bundled library's own
license. The source code of the LGPL and GPL parts in the downloads (FFmpeg, libmspack, and the
x86-64 download's glibc and GCC runtime) is published next to the downloads of each release, as
`SpeedBreaker-<version>-third-party-source.tar.xz`.

## License

SpeedBreaker is free software: you can redistribute it and/or modify it under the terms of the
**GNU General Public License** as published by the Free Software Foundation, either version 3 of
the License, or (at your option) any later version (GPL-3.0-or-later). See [COPYING](COPYING)
and [NOTICE](NOTICE). Third-party components keep their own licenses, listed above.

Copyright (C) 2026 project(u) and SpeedBreaker contributors.

## Legal notice

SpeedBreaker is an unofficial, fan-made project by project(u). It is **not affiliated with,
endorsed by or sponsored by Electronic Arts Inc., the Need for Speed brand or Valve
Corporation.**

"Need for Speed", "Most Wanted" and "Carbon" are trademarks of Electronic Arts Inc. Steam,
Steam Deck, Steam Machine, Steam Frame, SteamOS and their logos and icons are trademarks of
Valve Corporation. All other trademarks, including Xbox, Xbox 360, PlayStation, Apple, Mac,
macOS, iPhone, iPad, iCloud, Xcode, App Store, Backbone, Vulkan, Linux, Windows and Android,
belong to their respective owners. They are used here only to describe what SpeedBreaker works
with. The game is named only to say which game SpeedBreaker runs; no logos, box art or key art
of the game are used. The game content visible in screenshots belongs to its owners.

**No game files are included or distributed.** SpeedBreaker contains no game data or assets: no
textures, models, audio, movies or disc files. To play, you need your own legally obtained
backup (a disc image) of your own Xbox 360 disc of *Need for Speed: Most Wanted* (2005). The
in-app installer verifies it and installs it.

SpeedBreaker is provided "as is", without warranty of any kind, as set out in the GNU General
Public License.

[releases]: https://github.com/SpeedBreakerProject/speedbreaker/releases/latest
[dl-steamos-x86_64]: https://github.com/SpeedBreakerProject/speedbreaker/releases/latest/download/SpeedBreaker-steamos-x86_64.tar.xz
[dl-steamos-arm64]: https://github.com/SpeedBreakerProject/speedbreaker/releases/latest/download/SpeedBreaker-steamos-arm64.tar.xz
[dl-macos]: https://github.com/SpeedBreakerProject/speedbreaker/releases/latest/download/SpeedBreaker-macos.dmg
[issues]: https://github.com/SpeedBreakerProject/speedbreaker/issues
