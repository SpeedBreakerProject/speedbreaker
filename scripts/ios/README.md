# Building SpeedBreaker for iPhone and iPad

There's no download for iPhone and iPad, and SpeedBreaker isn't on the App Store. You build it on
a Mac and install it on your own device, signed with your own Apple developer account. Nothing
here is distributed: the app you build is yours.

## What you need

- **A Mac** with Apple silicon and **Xcode** (built with Xcode 26.4), plus everything the Mac
  build needs: see [Building from source](../../README.md#building-from-source), step 1.
- **Your own disc image** of the USA Xbox 360 disc, for step 2 of that guide (the game's code is
  translated from it on your Mac) and later for the installer on the device.
- **An Apple developer account.** SpeedBreaker was built and tested with a paid Apple Developer
  Program membership. It asks for two memory capabilities, Extended Virtual Addressing and
  Increased Memory Limit, which a free Apple Account's Personal Team may not be able to sign;
  apps signed by a free account also stop opening after 7 days.
- **An iPhone or iPad** with iOS or iPadOS 17 or later and an A15-class chip or newer (iPhone 13
  and later, M-series iPads), and a controller or keyboard. Tested on an iPhone 15 Pro Max and an
  iPad Pro 12.9-inch (M2), both on version 27.

## 1. The game's code

On the Mac, follow [Building from source](../../README.md#building-from-source) steps 1 and 2:
`scripts/setup_tools.sh`, your disc's `default.xex` in `game/files`, and XenonRecomp, which
writes `ppc/`. You don't need the Mac build itself (step 3).

## 2. The iOS libraries

```sh
scripts/ios/build_deps.sh ~/speedbreaker-ios-deps
```

This downloads and builds SDL3, glslang and FFmpeg's XMA decoders for iOS, and unpacks MoltenVK's
static library, into the folder you name (a few minutes; the sources, about 65 MB, are checked
against their SHA-256). Nothing is installed anywhere else.

## 3. Signing

Find your **team ID**: in Xcode, Settings > Accounts, select your account and team, or on
developer.apple.com under Membership details. It's 10 characters, like `A1B2C3D4E5`. Pick a
**bundle identifier** of your own, such as `com.yourname.SpeedBreaker`: it must be unique to your
team.

On the device, turn on **Developer Mode** (Settings > Privacy & Security > Developer Mode; it
restarts), then connect it to the Mac with a cable and tap **Trust**.

## 4. Build and install

```sh
export IOS_DEPS=~/speedbreaker-ios-deps
export DEVELOPMENT_TEAM=A1B2C3D4E5            # your team ID
export IOS_BUNDLE_ID=com.yourname.SpeedBreaker
scripts/ios/build.sh install
```

`build.sh` compiles the game into `build/ios` (about 6 minutes the first time), links and signs the
app with Xcode's automatic signing, which makes the provisioning profile itself, and installs it on
the connected device. The device must be registered with your team: if signing fails because the
team has no devices, add it on developer.apple.com under Devices, with the UDID Finder shows for it
(select the device in Finder's sidebar, then click the text under its name). Without `install` it
only builds: `build/ios/xcode/Build/Products/Release-iphoneos/SpeedBreaker.app`. With more than one
device connected, set `IOS_DEVICE` to the one you want (`xcrun devicectl list devices`).

If the app doesn't open and iOS says the developer isn't trusted, go to Settings > General > VPN &
Device Management, choose your developer account and trust it.

## 5. Play

Copy your `.iso` into **Files > On My iPhone (or iPad) > SpeedBreaker** and start the app: the
installer finds it, checks it and installs the game. See
[iPhone and iPad](../../README.md#iphone-and-ipad) in the main README for the rest: controllers,
the settings (a three-finger tap) and frame rates.

To update, pull the new version, repeat step 1's XenonRecomp line if `config/` changed, and run
`scripts/ios/build.sh install` again: your installed game and saves stay.
