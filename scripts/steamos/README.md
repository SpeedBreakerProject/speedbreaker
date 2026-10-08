# Building and running on SteamOS (Steam Machine / Steam Deck)

SteamOS's root filesystem is read-only and has no compiler, so the build
lives in an Arch Linux distrobox (podman and distrobox ship with SteamOS).
Tested on SteamOS 3.8 (Steam Machine, RADV NAVI33, clang 22, SDL 3.4).

1. Build container, once:

       distrobox create --yes --name nfsmw-build --image docker.io/library/archlinux:latest \
         --additional-packages "clang lld cmake ninja git python sdl3 vulkan-headers \
           vulkan-icd-loader vulkan-tools glslang spirv-tools mesa vulkan-radeon pkgconf \
           make diffutils patch curl nasm libpulse pipewire pipewire-pulse alsa-lib libdecor"

2. The repo anywhere in your home folder (`~/speedbreaker` below), with your
   own game files in `game/files`
   (`default.xex` must match the SHA-256 in `config/nfsmw.toml`).

3. Tools, recompile, build (inside the container, from the repo root):

       distrobox enter nfsmw-build
       scripts/setup_tools.sh
       tools/XenonRecomp/build/XenonRecomp/XenonRecomp config/nfsmw.toml tools/XenonRecomp/XenonUtils/ppc_context.h
       cmake -S . -B build/main -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
       ninja -C build/main SpeedBreaker

4. Play from Game Mode: in Desktop Mode, Steam > Games > Add a Non-Steam
   Game > Browse > `~/speedbreaker/scripts/steamos/speedbreaker.sh`.
   Then, in Game Mode, open the shortcut's Properties (gear icon) > General >
   Game Resolution and choose **Native**: on "Default", Steam gives the game
   1280x720 (or 1280x800) of a larger monitor and gamescope stretches it. The
   game runs fullscreen in Game Mode by itself; `[video] screen ... monitor
   ...` in last-run.log shows what Steam gave it. Controllers go
   through Steam Input. The run logs to `last-run.log` in the repo
   (including `[perf]` frame rate lines, once a second the left stick and
   the triggers raw and as the game receives them, and each change made in
   the settings menu).

5. Logs and bug reports: the game also keeps its last five sessions' logs,
   and any crash or hang reports, in `~/.local/share/speedbreaker/logs/`,
   and Settings > Advanced > Save Bug Report packs them (with the settings,
   a system summary and a screenshot) into one .zip on the Desktop. What
   play testers should send: [docs/TESTING.md](../../docs/TESTING.md).

Headless check over SSH (no window; frames can be captured with
NFSMW_CHECK_FRONT_SEC):

    distrobox enter nfsmw-build -- env SDL_VIDEODRIVER=offscreen NFSMW_MUTE=1 build/main/runtime/SpeedBreaker

Every option of the settings menu, changed from the menu and measured
(scripts/verify_settings.py, about 30 minutes; a profile of its own under
--out, never the player's; don't run it while the game is being played):

    distrobox enter nfsmw-build -- python3 scripts/verify_settings.py --out ~/verify-settings

## A self-contained build for a Steam Deck (no distrobox)

A Steam Deck (or any SteamOS machine) can run the game without the distrobox
or a compiler. After building (step 3), inside the container:

    scripts/steamos/make_bundle.sh    # [build dir (build/main)] [output dir (build/bundle)]

This makes `build/bundle/SpeedBreaker/` and the release download
`build/bundle/SpeedBreaker-steamos-x86_64.tar.xz` (+ `.sha256`; about 16 MB,
64 MB unpacked), with a copy named by the version
(`SpeedBreaker-<version>-steamos-x86_64.tar.xz`): the game (its debug
information stripped; the full symbols go to `build/bundle/symbols/`, never
published), SDL3, glslang, SPIRV-Tools, the C and C++ runtimes it was built
against, the launcher `bundle/speedbreaker.sh`, the player's instructions
`bundle/README.md` and licenses (`COPYING`, `NOTICE`,
`THIRD_PARTY_NOTICES.md`, `LICENSES/` and each bundled library's package
licenses). The version is `project()`'s in the top-level `CMakeLists.txt`. No
game data. Copy the
tarball to the Deck (scp, a USB stick) and follow its README: unpack into
`~/Games`, add `speedbreaker.sh` to Steam as a non-Steam game, put the disc image in
Downloads or on the SD card, and the first run installs from it. The game logs
to `last-run.log` in the unpacked folder (`previous-run.log` keeps the run
before).

The Vulkan loader, GPU driver, X11, PipeWire and the rest come from SteamOS.
The distrobox's glibc and libstdc++ are newer than SteamOS 3.8's (2.44 vs
2.41, GLIBCXX_3.4.36 vs 3.4.34) and the build needs them, so the launcher
runs the game through the bundled glibc loader while the system's is older,
and on the system's own once it catches up.

## Steam Frame (arm64)

The Frame's SteamOS (arm64) ships clang, gcc, CMake, Ninja, glslang and the
Vulkan headers, so the game builds on the Frame itself, no distrobox. It lacks
SDL3 (build a release into `~/deps/sdl3`, with `-DSDL_X11_XSCRNSAVER=OFF`) and
the XMA FFmpeg (`tools/ffmpeg-xma` must be built there: the steps in
`scripts/setup_tools.sh`; another machine's static libraries won't link). Then:

    cmake -S . -B build/main -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang \
        -DCMAKE_CXX_COMPILER=clang++ -DSDL3_DIR=$HOME/deps/sdl3/lib/cmake/SDL3 \
        -DCMAKE_SKIP_BUILD_RPATH=ON
    ninja -C build/main
    scripts/steamos/make_bundle.sh build/main build/bundle

which makes `SpeedBreaker-steamos-arm64.tar.xz` (and the versioned copy).
`-DCMAKE_SKIP_BUILD_RPATH=ON` keeps `~/deps/sdl3/lib` out of the program
(make_bundle.sh refuses a binary that names the home folder); to run
`build/main/runtime/SpeedBreaker` itself, set `LD_LIBRARY_PATH=~/deps/sdl3/lib`.
It carries no C or C++ runtime (the Frame's own are the ones it was built
against; make_bundle.sh checks the binary needs no newer ones) and the
Frame's own README (`bundle/README-steam-frame.md`: the Desktop app, Konsole,
Add to Steam). SDL3's license comes from `~/deps/sdl3/share/licenses`. The Frame's Adreno 750 (Turnip) binds at most 128 MB of a storage
buffer, so the renderer binds guest memory in four parts there (the log's
`[renderer] guest memory bound as 4 parts` line); in the headset the GPU is
shared with the compositor, where Display > Frame Rate 30 can play smoother.
