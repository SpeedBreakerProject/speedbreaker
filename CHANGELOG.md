# Changelog

What changed in each SpeedBreaker release, newest first. Each release's notes on GitHub start from
its entry here, and the game shows the entry once, the first time a new version starts (offline:
it's built into the game).

## Unreleased

### What's new

- The right stick has keys: O / L push it up and down, K / ; left and right. The game shifts
  gears with the right stick, so drag races can now be driven from the keyboard.

## v0.1.3

### What's new

- Smoother driving: no more small hitch every few seconds (the car jumped ahead, then stood
  still for a frame or two), which frame-time graphs didn't show (GitHub issue #9). Thanks to
  m9cy884mv6-a11y for the careful report.
- Sun shadows of cars, walls and tunnel roofs: cars cast their hard shadow in sunlight again, and
  walls, barriers and tunnel roofs shade the road and the cars, as on the Xbox 360. Inside tunnels
  the car no longer flickers between sunlight and tree shadows (GitHub issue #3). Thanks to the
  players who reported the flicker on GitHub and the missing car shadows on Reddit.
- Nitrous light trails are as long as on the Xbox 360 again: at 60 fps they were half as long, and
  looked brighter (GitHub issue #10). Thanks to jaswinprakash for the report, and to victorgbd,
  whose Android port of another Most Wanted recompilation found the cause.
- Pause menu > Performance Tuning: each slider has its own name again (Steering, Handling, Brakes,
  Ride Height, Aerodynamics, Nitrous, Turbo or Supercharger) instead of "Steering" on every row
  (GitHub issue #8).

### More about it

- The hitch: the game's main loop runs a couple of frames ahead of the screen. When one of its
  frames took longer, SpeedBreaker gave it two or three steps of game time and the next ones
  none, while the screen still showed one frame per refresh. A frame the screen shows for one
  refresh now runs one step. The game's speed is unchanged.
- The shadows: the game draws cars, walls and tunnel roofs into its sun shadow map as plain solid
  shapes, which on the Xbox 360 run no pixel shader. SpeedBreaker ran the trees' leaf-cutting
  shader on them too, and it threw them all away. Checked against real Xbox 360 footage of the
  same race. Sunlit roads are as bright as in v0.1.1 and v0.1.2.
- The light trails: the game draws them from where the car was over its last two frames, and the
  Xbox 360 races at 30 fps, so at 60 fps they covered half the distance. They now span the same
  time as on the 360. At Frame Rate 30 nothing changes.
- The tuning labels: the recompiled code looked up each row's name in a table that had lost all
  but its first entry.
- Updating keeps your saves, settings and the installed game, and needs no disc image: on a Mac,
  drag the new app into Applications and choose Replace, then Open Anyway once more; on SteamOS,
  unpack the new download over the old folder.

## v0.1.2

### What's new

- Internal Resolution 1.5x (1080p), in Settings > Graphics: a much sharper picture than 1x, with
  a little over half the pixels of 2x: for a GPU with room above 1x but not for 2x. Thanks to the
  Reddit player who asked for it.
- Laptops with two GPUs: the game now runs on the dedicated graphics card (NVIDIA or AMD), not on
  the one built into the processor. On Linux with NVIDIA's driver, the launcher also turns on
  NVIDIA's PRIME render offload: thanks to lydacious for the idea and the first version (GitHub
  pull request #6).
- Portable mode on Steam Deck, Steam Machine and Linux: put a file named `portable.txt` next to
  `speedbreaker.sh`, and SpeedBreaker keeps your settings, saves, logs and caches in that folder,
  and installs the game there too. Move or copy the whole folder to another machine and play on
  (GitHub issue #1).
- Taller and wider screens: the sun's glare is round again (it was stretched up to 1.33 times
  taller on the Steam Deck, 4:3 and 3:2, and wider on ultrawides); no faint line of a letterboxed
  movie shows in the black past the 16:9 picture; and the race results screen is black past its
  16:9 picture, as on a 16:9 TV, instead of showing the road behind it.
- iPhone: after a screenshot, the picture no longer shrinks into a corner of the screen with the
  settings menu blown up.
- Experimental: an IPA for iPhone and iPad, to sideload with your own Apple Account.

### More about it

- 1.5x renders 2.25 times the pixels of 1x, where 2x renders 4 times. Auto still chooses between
  whole scales (1x, 2x or 3x) as before.
- The game picks a dedicated (discrete) GPU over a built-in (integrated) one, and of two alike the
  one with more video memory. The log's `[video] Vulkan devices:` line lists every GPU it found,
  the one it uses, and why. To use another, put `NFSMW_GPU=<its number or part of its name>` in
  the launch options, e.g. `NFSMW_GPU=intel %command%`. On the Steam Deck, Steam Machine, Steam
  Frame, a Mac, an iPhone or an iPad (one GPU each) nothing changes. The launcher sets
  `__NV_PRIME_RENDER_OFFLOAD=1` only on a PC where NVIDIA's driver runs one GPU beside another and
  NVIDIA's Vulkan driver is installed, and keeps a value you set yourself.
- `speedbreaker.sh --portable on` copies what you have now (settings, saves and the installed game)
  into the folder and turns portable mode on; `--portable off` copies them back. Both copy and
  never delete: your old files stay until you remove them. Not on the Mac, where an app can't keep
  its data inside itself.
- The IPA: the game asks iOS for two memory entitlements, and a free Apple Account can give at most
  one of them, with some tools only: with a free account it may not start, or iOS may close it in
  busy scenes. scripts/ios/README.md says which tools keep what. When iOS refuses the memory, the
  game now says why on screen instead of closing.
- iPhone and iPad: on iOS 27 the screenshot screen could leave the game laid out as if the phone
  were held upright, until the game was closed. The game now checks its picture against the
  screen after coming back from the screenshot screen, the app switcher or Control Center, and
  puts it right if they disagree; an iPhone, which plays only in landscape, never draws upright.
- iPhone and iPad: Internal Resolution's Auto now decides chip by chip, and the log says what it
  chose and why. Every chip still gets 1x (720p) under Auto, the iPhone 18 Pro Max included: in a
  long race on a charger it got hot even at 1x. 1.5x and 2x are in Settings > Graphics > Internal
  Resolution. The log also records the battery level and charging state.
- After a macOS, SteamOS or graphics driver update, the first drive can stutter for a moment while
  the game rebuilds its shaders for the new system. It happens once, and the game's log now says
  so.
- Updating keeps your saves, settings and the installed game, and needs no disc image: on a Mac,
  drag the new app into Applications and choose Replace, then Open Anyway once more; on SteamOS,
  unpack the new download over the old folder.

## v0.1.1

### What's new

- The Steam Deck and 16:10 Macs fill the screen: a slightly taller view, with the HUD kept at
  16:9. Settings > Display > Aspect Ratio > 16:9 brings the bars back.
- Sunlit roads and correct shadows. A bug in the shadow map left every road in shade, on every
  device: the roads are now as bright as on the Xbox 360, and the line between shade and sun that
  followed the car is gone (GitHub issue #3). Thanks to the Reddit tester who spotted it.
- Cutscene bars: the police video camera in the career intro, and the intro's black frames, now
  cover the whole screen on taller and wider screens.
- macOS 15 Sequoia support: the Mac app runs on macOS 15 or later (v0.1.0 needed macOS 26). On
  macOS 26 it runs exactly as before.
- Linux startup crash fixes: no crash at startup on graphics cards without Resizable BAR (NVIDIA),
  and none expected on Intel's Linux driver (GitHub issue #2).
- Choose a folder... in the installer: put the game in any folder you like (GitHub issue #1).
- Check for Updates, in Settings > Advanced.
- A cleaner Linux folder: speedbreaker.sh is the only thing at the top to run, and the program
  moved into lib/.

### More about it

- Choose a folder... is on the installer's last page. An empty folder is used as it is; one that
  already holds files gets a new SpeedBreaker Game folder inside. The game remembers the folder,
  and says so if its drive isn't connected. On SteamOS the folder picker opens only in Desktop
  Mode: in Game Mode, switch to Desktop Mode and choose the folder there, or run
  `speedbreaker.sh --install <image> --dest <folder>` from Konsole in Desktop Mode (or over SSH).
- Check for Updates asks GitHub whether a newer SpeedBreaker is out. If one is, it shows what
  changed, opens the download page, and shows that page as a QR code to scan with a phone (for
  Steam Deck Game Mode and the Steam Frame, where no browser may open). Nothing is checked online
  unless you press Check for Updates.
- Steam Deck, Steam Machine, Steam Frame and Linux: always start the game with speedbreaker.sh, as
  your Steam shortcut does. The program in lib/ can't pick its libraries on its own. Unpacked over
  v0.1.0, the first start removes v0.1.0's program from the top of the folder.
- Updating keeps your saves, settings and the installed game, and needs no disc image: on a Mac,
  drag the new app into Applications and choose Replace, then Open Anyway once more; on SteamOS,
  unpack the new download over the old folder.

## v0.1.0

The first public release of SpeedBreaker: an unofficial static recompilation of the Xbox 360
version of Need for Speed: Most Wanted (2005), running natively. Bring a backup of your own USA
Xbox 360 disc: the installer checks it and installs the game.

### Where it runs

- Steam Deck, Steam Machine and Linux PCs (x86-64).
- Steam Frame, as a big flat screen in the headset (not VR).
- Mac with Apple silicon, on macOS 26 or later.
- iPhone and iPad: there's no download, you build it yourself with Xcode.
- Windows: coming soon.

### What you get

- Up to 4K: the 3D scene renders at 1x, 2x or 3x the console's 720p, and Auto picks one for your
  screen and device.
- Ultrawide screens get a wider view instead of a stretched one, and 4:3 and 3:2 screens a taller
  one, with the HUD and the rear-view mirror where they belong.
- Up to 60 fps on a 60 Hz display, a steady 30 at full game speed, or Auto, which drops to 30 only
  while the device is hot or can't hold 60.
- The original look: the low sun's glare, correct colours on Apple devices, proper car paint and
  mirror reflections, and distant textures that don't shimmer.
- A guided installer: it finds your disc image, checks that it's the right game and verifies all
  49 files as it copies them.
- Controllers work as an Xbox 360 pad, with hot-plugging and rumble. The keyboard works too.
- A settings menu that pauses the game: F1, Back + Start (View + Menu on the Steam Deck and Steam
  Frame), or a three-finger tap on iPhone and iPad. Every change applies at once.
- On iPhone and iPad, switch away and the game freezes; come back and it carries on.
- Settings > Advanced > Save Bug Report puts the logs, your settings and a screenshot in one zip
  file. Nothing is uploaded: you decide what to send.
- From this version on, the first start after an update shows what changed, once. Nothing is
  checked online.

### Good to know

- Mac: the app isn't notarized by Apple, so the first time, allow it in System Settings > Privacy
  & Security > Open Anyway.
- Steam Deck OLED: set the screen to 60 Hz.
- Steam Frame: Display > Frame Rate 30 fps is the smooth choice in the headset.

SpeedBreaker is an unofficial fan project, not affiliated with Electronic Arts or Valve. No game
files are included.
