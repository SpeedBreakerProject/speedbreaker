# SpeedBreaker for Windows

This is a fork of [SpeedBreakerProject/speedbreaker](https://github.com/SpeedBreakerProject/speedbreaker)
(v0.1.7) with a Windows (x86-64) build on top. The upstream [README](README.md) still applies to
everything else, including what you need to play: **your own USA (NTSC-U) Xbox 360 disc**. No game
files are included or distributed here, and none ever should be: `.gitignore` blocks `*.xex`, `*.iso`
and friends.

## What this fork adds

| | Where |
|---|---|
| Windows port of the runtime (MinGW-w64, cross-compiled on Ubuntu) | `runtime/compat/win/` and small changes elsewhere |
| Scripts that build `SpeedBreaker.exe` | `win-build/` |
| **Audio fix**: the audio thread waits on a high-resolution timer and joins MMCSS | `runtime/apu/audio_wait.h`, `runtime/apu/audio.cpp` |
| **SpeedBreakerLauncher.exe**: edits `settings.toml` and a few launch options, then starts the game | `launcher/` |
| GitHub Actions: launcher, game, releases | `.github/workflows/` |
| Audio pacing test, and an exe hot-patch for when you can't rebuild | `win-build/audio-pacing-test/`, `win-build/exe-hotpatch/` |

## Playing

Download `SpeedBreaker.exe` and `SpeedBreakerLauncher.exe` from the
[Releases](../../releases) page, put them in **the same folder**, and start the launcher. On first
run the game's own installer asks for your disc image, as described upstream.

## Building

### The launcher (Windows, no extra tools)

```
launcher\build.bat
```

It uses the C# compiler that ships with Windows. GitHub Actions does the same on every push that
touches `launcher/` (workflow *Launcher and tools*).

### The game, on GitHub Actions

`SpeedBreaker.exe` is built from a translation of **your own `default.xex`**. That file can't be in
the repository, so the build fetches it from a private URL you control:

1. Put `default.xex` (USA NTSC-U, 3,936,256 bytes, SHA-256
   `aebdf3c14d1e4e4ed2c130d94bc1a0e97bd6d5bf39bdfdef33c3909c9cc4ba4c`) somewhere **private** that
   serves it over HTTPS, for example a release asset in a private repository.
2. Add the repository secret `XEX_URL` with that URL. If the server wants a header, add `XEX_AUTH`
   too, for example `Authorization: Bearer <token>`. (Settings, Secrets and variables, Actions.)
3. Run the workflow **Game (SpeedBreaker.exe)** from the Actions tab, or push a tag
   (`git tag v0.1.7-win.1 && git push origin v0.1.7-win.1`) to build everything and publish a
   release.

Without `XEX_URL` the game job does nothing, and a tag still publishes the launcher. The build takes
a few hours on a hosted runner. The workflow and `win-build/ci-build.sh` are new and have not been
run on GitHub yet, so expect to adjust them on the first run; the logs of a failed build are attached
as an artifact.

### The game, on your own machine

On Ubuntu 24.04 (or WSL2), with about 10 GB free:

```
JOBS=$(nproc) win-build/ci-build.sh /path/to/default.xex     # writes dist/SpeedBreaker.exe
```

`win-build/rebuild.sh` is the same build step by step. Its helper scripts use the fixed path
`/home/claude`; `ci-build.sh` links your checkout there for you.

## The audio fix

On some Windows PCs the game's sound was chopped: two frames of sound, then one of silence. The
silence was already in the game's own mix. The game's voices hold only a couple of decoded frames,
so the audio thread must render one frame per 5.33 ms tick, and `std::this_thread::sleep_until` on
Windows woke it a whole tick late on more than half the ticks (889 of 1500 on the PC where this was
found, and 957 with `timeBeginPeriod(1)`, which SDL requests). The thread then rendered a burst of
frames, and every frame after the first two in a burst was silent. A high-resolution waitable timer
woke it on time on 1500 of 1500 ticks (worst 0.6 ms).

To check a PC, run `AudioPacingTest.exe` (built by the *Launcher and tools* workflow, or from
`win-build/audio-pacing-test/`). After the fix the game log shows `[audio] pacing: high-resolution
timer, MMCSS` at start-up.

If you can't rebuild, `win-build/exe-hotpatch/make_patch.py` applies the timer part of the fix to an
existing exe. It refuses any exe other than the build it was written for.

## Following upstream

The work sits on a branch based on the `v0.1.7` tag, as a short series of commits. To move to a newer
upstream release:

```
git remote add upstream https://github.com/SpeedBreakerProject/speedbreaker.git
git fetch upstream --tags
git rebase --onto v0.1.8 v0.1.7 windows
```

## Licence

GPL-3.0-or-later, like upstream (see `COPYING`). Everything here builds on the SpeedBreaker project
and its dependencies; see `NOTICE` and `THIRD_PARTY_NOTICES.md`.
