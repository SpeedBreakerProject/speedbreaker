# Changelog

What changed in each SpeedBreaker release, newest first. Each release's notes on GitHub start from
its entry here, and the game shows the entry once, the first time a new version starts (offline:
it's built into the game).

## v0.1.9

### What's new

- **Nine more languages** (Settings > Game > Language): French, German, Italian, Spanish, Dutch,
  Swedish, Danish, Polish and Finnish, from the USA disc's own files. The menus and the race text
  switch (European languages show km/h) the next time the game starts; the movies and the speech
  stay in English, as the USA disc has them. English stays the default. Thanks to
  wendersonrodrigues, who found where the game picks its language (pull request #26).
- **Fill Screen renders the screen's own shape**, so it's sharper on screens taller than 16:9: a
  Steam Deck draws 1280x800 at 1x, pixel for pixel, instead of stretching 720 rows to 800. 16:9
  screens look exactly as before. Thanks to krittinen for the report and the screenshots
  (issue #34).

### Fixed

- Mac: a wired Xbox One or Series controller can't rumble (macOS takes the rumble and the
  controller does nothing). The game now says so once: pair it over Bluetooth for rumble.

### More about it

- The line under the Language row says what your installed files give the language you choose.
  The row isn't shown on iPhone and iPad.
- SteamOS and Linux: the log names a graphics card's PCIe link and warns when the card reaches
  the game's memory over PCIe, to help track down slow GPUs (issue #40).
- `NFSMW_NATIVE_SHAPE=0` keeps the old 16:9-shaped frame for a run, to compare.
- Updating keeps your saves, settings and the installed game, and needs no disc image: on a Mac,
  drag the new app into Applications and choose Replace, then Open Anyway once more; on SteamOS,
  unpack the new download over the old folder.

## v0.1.8

### What's new

- **Two-player split screen** (Quick Race > Split Screen): with two controllers, each player gets
  their own half of the screen. The second controller joins at "Player 2 press START". If either
  player's controller disconnects, the game pauses and says so until it's back (issue #28).
- **Smooth Motion** (Settings > Display), on a Mac with a 120 Hz screen (ProMotion or a 120 Hz
  monitor): up to 120 fps. The game still runs at 60 steps a second, as on the console, and draws
  one more picture between each two. It's off by default and takes effect the next time the game
  starts. When a scene is too heavy, it eases off, down to an even 60 for a few seconds.
- **USB steering wheels (experimental).** A Logitech G29 steers, the pedals are the throttle and
  the brake, the paddles shift gears, and the menus work from the wheel's buttons. Settings > Wheel
  (shown while a wheel is plugged in) has the steering range, deadzone, response, force feedback
  and Calibrate for other wheels. Tested with a G29 on a Mac; the steering feel is still being
  tuned, so feedback from wheel owners is very welcome. Thanks to byteofmydream, who wrote it
  (pull request #29).

### Fixed

- Now and then in a race, especially on a busy Mac, the engine (or another sound) dropped to a
  murmur until the next gear change. The game's own sound code took a moment where it didn't get to
  run for a console overload and cut sounds; it no longer does. Thanks to Omega3komma3 for the
  report and the video (issue #35).
- The fade to black before some cutscenes and loads (such as the career prologue's last race)
  now covers the whole screen, top and bottom, on screens taller than 16:9 (a MacBook, a Steam
  Deck), instead of leaving the scene showing above and below it.
- A preventive memory fix for Apple-silicon Macs, iPhones and iPads: handing memory back could
  clear a little more than it should. It wasn't seen to happen in play.

### More about it

- Split screen runs at 60 fps on a Mac and on a Steam Deck at its Auto resolution; at higher
  Internal Resolutions it can drop below.
- Smooth Motion isn't on SteamOS, Linux or iPhone and iPad yet. Internal Resolution's Auto picks
  for two pictures a step (1x on an M1 Pro); the controls answer a few milliseconds later.
- Wheels: other wheels than the G29 are used as best the game can work them out; Calibrate teaches
  it yours. Force feedback works on a Mac through SDL's own Logitech driver (the G29 in its PS3
  position, G27, G25, Driving Force GT and Pro; not a G29 in its PS4 position, a G920 or a G923).
  On SteamOS and Linux the wheel should steer as on a Mac; force feedback there isn't tested yet.
- The log now says when the game's sound code would have dropped sounds, at most once a minute.
- Updating keeps your saves, settings and the installed game, and needs no disc image: on a Mac,
  drag the new app into Applications and choose Replace, then Open Anyway once more; on SteamOS,
  unpack the new download over the old folder.

## v0.1.7

### What's new

- Smoother on 120 Hz screens on SteamOS and Linux, and on 180 and 240 Hz ones: each frame now
  stays on screen for exactly two refreshes (three at 180 Hz, four at 240 Hz), as a 60 Hz screen
  shows each for one. Until now some frames stayed for one refresh and the next for three,
  a small, uneven stutter that came and went. Macs are unchanged.
- A note, once, when your screen runs at a rate 60 frames a second can't share evenly (90 Hz on a
  Steam Deck OLED, 144 or 165 Hz monitors), which makes motion stutter: it says how to set the
  screen to 60 Hz or 120 Hz, or to turn on variable refresh. It waits for a menu or a pause rather
  than coming up mid-race, pauses the game while it's open, and doesn't come back for that rate
  once closed. It never shows with variable refresh, nor on a Mac's ProMotion screen.

### More about it

- Tested on simulated 120, 180 and 240 Hz screens (a nested gamescope session on a Steam Machine,
  whose own TV tops out at 60 Hz): 99.9% or more of frames shown evenly, against 77-88% at 120 and
  240 Hz before. Not yet on a real 120 Hz panel: a report from yours would help.
- Each frame reaches a 120 Hz screen about as soon as it did in v0.1.6, now evenly. While frames
  take longer (loading, a heavy scene), each is shown a refresh or two later, evenly, rather than
  some on time and some late.
- As at 60 Hz, this needs a graphics driver that reports when each frame reaches the screen
  (Vulkan's present wait); the log's "[vblank] locked to every 2nd refresh" line says it took.
  With variable refresh on, the game leaves the screen to follow its frames, as before.
  NFSMW_REFRESH_MULTIPLE=0 brings back v0.1.6's behaviour.
- The note's words are in the log too ("[video] refresh notice"), and NFSMW_REFRESH_NOTICE=0 turns
  it off. Where your screen's rate is recorded once the note is closed: settings.toml's [app]
  refresh_notices. On a Steam Deck it points to Quick Access > Performance; the names of Steam's
  settings in it haven't been checked on a Deck yet.
- Updating keeps your saves, settings and the installed game, and needs no disc image: on a Mac,
  drag the new app into Applications and choose Replace, then Open Anyway once more; on SteamOS,
  unpack the new download over the old folder.

## v0.1.6

### What's new

- No more lost sound followed by a freeze. If your audio output stopped taking sound for a while,
  the game's sound stopped for good, and the game later froze, for example on the save screen
  after an event. Every version so far had this. Now the game keeps running, unheard, and the
  sound comes back as soon as the output takes audio again. Thanks to Omega3komma3 for the report
  and the two bug reports (GitHub issue #20).
- No more rectangular borders in the Showcase (where you rotate and zoom around a car, in the car
  lot, the customize shop and on a Blacklist rival's car) or around a Blacklist rival's bio. The
  Showcase darkens the edges of the garage, and that darkening stopped short of the edges of the
  screen; the bio's black panel did the same. On a 16:9 screen that left a thin border, as on an
  Xbox 360, whose TV hid it past the edge of its screen; on a taller screen such as a MacBook or a
  Steam Deck the Showcase's border was wide at the top and bottom. Both now reach the edges of the
  screen, on every screen shape. Thanks to DoubleDips, who reported it, and m9cy884mv6-a11y, who
  confirmed it on the Steam Deck (GitHub issue #19).
- Mac: in a window, the picture now runs up under a transparent title bar, and the window's
  buttons show only while you move the mouse over it. Thanks to aleszczynskig, who wrote it
  (GitHub pull request #23).
- Mac: on a MacBook Pro's 120 Hz ProMotion screen, switching fullscreen on or off, or resizing the
  window, could close the game now and then (since v0.1.4; its crash report named "mutex lock
  failed"). It no longer does.
- A quit asked for from outside the game is now a normal quit: the stop signal a launcher sends,
  Ctrl+C, or closing the terminal the game was started from. Your settings are saved, as with
  Quit Game. Closing its terminal used to end it on the spot, and every session, even one ended
  with Quit Game, left a crash marker in its logs folder until the next start.

### More about it

- The sound: the log now says when the audio output stopped taking audio and when it came back,
  and a hang report includes the audio's state.
- The borders: the few black frames between two shots of the career's opening scene are now
  black to the edges of the screen too.
- The title bar: the window's saved size now takes in the title bar, so a window from an earlier
  version opens a title bar shorter, with the same picture.
- Quitting: a crash still saves a crash report, and the next start still offers it. A session
  that ended without quitting (stopped by force, or the power going) now leaves its marker, and
  the next start's log says so.
- Updating keeps your saves, settings and the installed game, and needs no disc image: on a Mac,
  drag the new app into Applications and choose Replace, then Open Anyway once more; on SteamOS,
  unpack the new download over the old folder.

## v0.1.5

### What's new

- Settings > Controls: every keyboard control can be reassigned, and each control has two keys.
  Choose a row, Left or Right for its first or second key, press A or Enter, then the new key; Y
  clears a key, X puts a row's defaults back, Reset page the whole table. The default keys are
  unchanged (the arrows steer as well as A and D), and the left stick click gets a key of its own,
  Z. Thanks to love-gamer, who wrote it (GitHub pull request #12).
- Settings > Graphics > Upscaling has a third choice, FSR 1 (AMD FidelityFX Super Resolution 1),
  for a window much larger than the Internal Resolution (1x on a 1440p, MacBook or 4K screen):
  curves, wires and diagonals stay smooth where bicubic shows steps, such as lamp posts against
  the sky or the speedometer's needle. It is used only when the picture is enlarged. Bicubic stays
  the default. Not on iPhone and iPad.
- The race results screen no longer shows a thin strip of the race above and below it. An Xbox 360
  draws that strip too, and a TV of the time hid it past the edge of its screen; today's screens
  show the whole picture, so it is now black, on every screen shape (Steam Deck, 16:9 and others).

### More about it

- The Controls page: the keys are saved by name in settings.toml's `[keys]` table (`w`,
  `left_shift`, `none`). F1, Esc (it cancels a wait) and the Cmd and Windows keys can't be
  assigned. If you go back to v0.1.4 and change a setting there, it forgets the Controls page's
  keys.
- FSR 1: Sharpening sets its sharpening pass too (0.3 of its full strength by default). Bicubic
  stays the default because FSR 1 makes foliage look a little painted, costs more GPU time (about
  1 ms a frame on an M1 Pro at 1080p, 0.1 ms on a Steam Machine), and with Internal Resolution on
  Auto most screens aren't enlarged at all.
- Updating keeps your saves, settings and the installed game, and needs no disc image: on a Mac,
  drag the new app into Applications and choose Replace, then Open Anyway once more; on SteamOS,
  unpack the new download over the old folder.

## v0.1.4

### What's new

- The picture now matches a real Xbox 360 on a TV: slightly deeper shadows and slightly brighter
  mid-tones. The game sets a gamma ramp for the console's TV output, and SpeedBreaker never
  applied it until now. The new Settings > Display > Xbox 360 Display Gamma is on by default, also
  after an update: to get the previous look back, turn it off.
- Settings > Display > Brightness: lighter or darker shadows and mid-tones, from -5 to +5, a
  gentle step at a time, like a TV's brightness. Black and white stay put, and only the game's
  picture changes, never the menus. At 0, the default, it leaves the picture as it is.
- The game's own Brightness slider (pause menu > Options > Video) now changes the picture as you
  move it, as on an Xbox 360: the game writes a new gamma ramp for each step, and SpeedBreaker
  applies it. It needs Settings > Display > Xbox 360 Display Gamma, which is on by default; with
  that off it does nothing, as before. If you moved the slider in an earlier version and the game
  has saved since, that brightness now takes effect: a slider left at maximum gives a much
  brighter, washed-out picture, as a 360 with that save would show. Set the slider back to the
  middle, or press Y (Defaults) and choose Yes. Thanks to m9cy884mv6-a11y for the report (GitHub
  issue #18).
- Keyboard: the right stick has keys, O and L for up and down, K and ; for left and right. The
  game shifts gears with the right stick, so you can now shift from the keyboard and finish a
  drag race. Thanks to love-gamer, who wrote it (GitHub pull request #11).
- Settings > Quit Game, at the bottom of the menu after Close: quits SpeedBreaker with a controller,
  the mouse or the keyboard, so a fullscreen game no longer needs a keyboard shortcut to leave. It
  asks first, with Cancel chosen, so no single press quits. Not on iPhone and iPad, where apps are
  closed from the app switcher.
- Settings > Display > Fullscreen now decides how the game starts, every time. On a Mac, the
  window's green button (or Ctrl+Cmd+F) switches fullscreen until you quit, so the game no longer
  sometimes starts fullscreen and sometimes not.
- Mac: the mouse pointer no longer sits over the game. It hides after two seconds without moving
  while you play, and comes back as soon as you move the mouse; in the settings menu and the
  installer it stays. Thanks to DoubleDips for the report (GitHub issue #15).
- Mac: smoother frame pacing. The game's 60 Hz clock now keeps time on macOS (it ran up to 9 ms
  late), and on a MacBook Pro's 120 Hz ProMotion screen each frame now follows the last by 1/60 s
  instead of arriving whenever the GPU finished it. Other screens show frames as before, and the
  timing turns itself off if a screen doesn't keep it.
- On slow GPUs, below 15 fps, the game no longer runs a few percent slow: its clock now keeps up
  with the screen (a laptop with Intel graphics at Internal Resolution 1.5x, about 12 fps, ran 4%
  slow).
- Changing the window's shape while a movie plays (for example the green button during the
  opening movie) no longer shows a stray "EVENT HANDLER" label above the picture.

### More about it

- The gamma ramp: the game sets it when it starts, and the console applies it to every frame on
  its way to the TV. Two real Xbox 360 recordings of the intro movie and the title screen match it
  to within half a level, against about two levels without it. With Xbox 360 Display Gamma off
  and Brightness at 0, the picture is exactly the one of earlier versions.
- The game's slider: each step writes a new ramp, which shows on that frame. At its lowest, its
  default and its highest, the picture's average brightness moves by about -13, +1 and +24 levels
  (of 255) against Xbox 360 Display Gamma off. The game keeps the slider in your alias's save the
  next time it saves the profile; without an alias it starts at the middle every time.
- The right-stick keys: holding both keys of a pair centres the stick. No other key changed.
- Fullscreen: a switch the game didn't ask for (on a Mac, the green button or Ctrl+Cmd+F) lasts
  until you quit and never changes the setting. While the window differs from the setting,
  Settings > Display > Fullscreen says NOW FULLSCREEN or NOW WINDOWED; choosing On or Off there
  saves it and switches the window.
- The pointer hides only over the game's window, so in a window it still shows over the desktop,
  and the mouse is never captured.
- The pacing: macOS let the thread that drives the game's 60 Hz clock wake up to 9 ms late; it
  now runs with real-time priority. And a ProMotion screen shows each frame as soon as it's ready,
  so frames that took a little longer to draw arrived unevenly: SpeedBreaker now asks for each
  frame to be shown at a set time after its tick. Measured on a MacBook Pro (M1 Pro) in
  fullscreen: 99.9% of frames 1/60 s apart in menus and about 94% while driving (99-100% on the
  open road, less in the heaviest scenes, where a frame takes longer than the time allowed),
  against about a quarter without it, for about 5 ms more latency.
- The slow clock: when a frame took longer than six screen refreshes, SpeedBreaker started its
  count over and dropped up to three refreshes of game time each time; below 15 fps that happened
  every second or so. That time is now given back over the next frames. Above 20 fps nothing
  changes. Checked by replaying frame timings recorded on that laptop: game time went from 96% to
  99.9% of real time.
- The label: it belongs to a menu the game keeps just above the picture. After a window change
  mid-movie the game kept the old window's layout for it, so the label slipped into view; it now
  gets the new layout at once.
- Updating keeps your saves, settings and the installed game, and needs no disc image: on a Mac,
  drag the new app into Applications and choose Replace, then Open Anyway once more; on SteamOS,
  unpack the new download over the old folder.

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
