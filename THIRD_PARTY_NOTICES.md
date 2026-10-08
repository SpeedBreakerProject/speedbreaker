# Third-party notices

SpeedBreaker is Copyright (C) 2026 project(u) and SpeedBreaker contributors, and is free software
under the GNU General Public License, version 3 or (at your option) any later version
(GPL-3.0-or-later; [`COPYING`](COPYING), [`NOTICE`](NOTICE)). It is built from, and its downloads
include, software by others under their own licenses, listed here. The license texts are in
[`LICENSES/`](LICENSES).

The downloads carry these notices too: the Mac app in `SpeedBreaker.app/Contents/Resources/licenses`,
each SteamOS download in its `licenses/` folder (with the license of every library it bundles,
collected when the bundle is made).

## Compiled into SpeedBreaker (every platform)

| Project | What it does here | License | Text |
|---|---|---|---|
| **Unleashed Recompiled**: hedge-dev and contributors | Parts of the kernel, XAM, memory, heap and threading code are adapted from it (each such file says so at the top). | GPL-3.0-or-later | [`LICENSES/GPL-3.0.txt`](LICENSES/GPL-3.0.txt) |
| **Xenia**: Copyright Ben Vanik and Xenia contributors | The Xenos GPU register, microcode and format headers are ported from it (`runtime/gpu/xenos/`), as is the NT status table (`runtime/kernel/imports_error.cpp`). Its kernel, XMA and GPU behaviour is the reference throughout. | BSD-3-Clause | [`LICENSES/Xenia-BSD-3-Clause.txt`](LICENSES/Xenia-BSD-3-Clause.txt) |
| **freedreno**: Copyright (c) 2012 Rob Clark | Shader instruction formats, through Xenia's `ucode.h`. | MIT | [`LICENSES/freedreno-MIT.txt`](LICENSES/freedreno-MIT.txt) |
| **XenonRecomp** (XenonUtils): Copyright (c) 2025 hedge-dev and contributors | Recompiles the game's PowerPC code into C++ (with SpeedBreaker's patches, `patches/xenonrecomp/`); its XEX loader is compiled into the runtime. | MIT | [`LICENSES/XenonRecomp-MIT.md`](LICENSES/XenonRecomp-MIT.md) |
| **libmspack**: (C) 2003-2023 Stuart Caie | LZX decompression of the XEX (`lzxd.c`, through XenonRecomp). | LGPL-2.1 | [`LICENSES/libmspack-LGPL-2.1.txt`](LICENSES/libmspack-LGPL-2.1.txt) |
| **tiny-AES-c** | XEX decryption (through XenonRecomp). | Unlicense | [`LICENSES/tiny-AES-c-Unlicense.txt`](LICENSES/tiny-AES-c-Unlicense.txt) |
| **TinySHA1**: Copyright (c) 2012-22 Saurav Mohapatra | SHA-1, in XenonRecomp's XEX patcher. | ISC-style | [`LICENSES/TinySHA1-ISC.txt`](LICENSES/TinySHA1-ISC.txt) |
| **toml++**: Copyright (c) Mark Gillard | Reads and writes `settings.toml` (through XenonRecomp). | MIT | [`LICENSES/tomlplusplus-MIT.txt`](LICENSES/tomlplusplus-MIT.txt) |
| **SIMDe**: Copyright (c) 2017 Evan Nemerson and contributors | Vector instructions in the recompiled code (through XenonRecomp). | MIT | [`LICENSES/SIMDe-MIT.txt`](LICENSES/SIMDe-MIT.txt) |
| **Dear ImGui** 1.92.9: Copyright (c) 2014-2026 Omar Cornut | The installer, the settings menu and the overlays (`runtime/thirdparty/imgui`). | MIT | [`LICENSES/DearImGui-MIT.txt`](LICENSES/DearImGui-MIT.txt) |
| **stb_truetype, stb_rect_pack, stb_textedit**: Copyright (c) 2017 Sean Barrett | Font rasterising and text editing, inside Dear ImGui. | MIT (or public domain) | [`LICENSES/stb-MIT.txt`](LICENSES/stb-MIT.txt) |
| **Roboto** (Medium): Copyright 2011 Google Inc. | The menus' font, embedded in the program. Roboto is a trademark of Google. | Apache-2.0 | [`LICENSES/Apache-2.0.txt`](LICENSES/Apache-2.0.txt) |
| **o1heap**: Copyright (c) 2020 Pavel Kirienko | The game's heap allocator (`runtime/thirdparty/o1heap`). | MIT | [`LICENSES/o1heap-MIT.txt`](LICENSES/o1heap-MIT.txt) |
| **FFmpeg** 7.1.1: the FFmpeg developers | The XMA1, XMA2 and WMA Pro audio decoders, linked statically. Built without any GPL parts, with one SpeedBreaker change: [`patches/ffmpeg/0001-xma-single-stream-no-holdback.patch`](patches/ffmpeg/0001-xma-single-stream-no-holdback.patch) (no sample hold-back for single-stream XMA, which the game's audio mixer depends on). Source: `https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz` plus that patch; [`scripts/build_ffmpeg_xma.sh`](scripts/build_ffmpeg_xma.sh) builds it. | LGPL-2.1-or-later | [`LICENSES/FFmpeg-LGPL-2.1.txt`](LICENSES/FFmpeg-LGPL-2.1.txt) |

## Libraries in the downloads

| Project | Where | License | Text |
|---|---|---|---|
| **SDL3**: Copyright (C) 1997-2026 Sam Lantinga | Windows, input, audio and the app's lifecycle. In the Mac app and both SteamOS downloads. | zlib | [`LICENSES/SDL3-Zlib.txt`](LICENSES/SDL3-Zlib.txt) |
| **glslang**: The Khronos Group Inc. and others | Compiles the game's shaders while it runs. In the Mac app and both SteamOS downloads. | BSD-3-Clause and others (its own list) | [`LICENSES/glslang.txt`](LICENSES/glslang.txt) |
| **SPIRV-Tools**: The Khronos Group Inc. and others | Used by glslang. In the Mac app and both SteamOS downloads. | Apache-2.0 | [`LICENSES/Apache-2.0.txt`](LICENSES/Apache-2.0.txt) |
| **MoltenVK** (with SPIRV-Cross): Copyright (c) 2015-2026 The Brenwill Workshop Ltd., The Khronos Group Inc. | Vulkan on Apple's Metal. In the Mac app (and an iPhone or iPad build). | Apache-2.0 | [`LICENSES/Apache-2.0.txt`](LICENSES/Apache-2.0.txt) |
| **Vulkan Loader**: The Khronos Group Inc., Valve Corporation, LunarG, Inc. | In the Mac app (SteamOS has its own). | Apache-2.0 | [`LICENSES/Apache-2.0.txt`](LICENSES/Apache-2.0.txt) |
| **GNU C Library** (glibc) and **GCC's runtime libraries** (libstdc++, libgcc_s) | In the SteamOS x86-64 download only, from the Arch Linux packages it was built with (the download's `licenses/README.txt` names each package and version). The Steam Frame download uses SteamOS's own. | glibc: LGPL-2.1-or-later. GCC's runtime: GPL-3.0 with the GCC Runtime Library Exception | in the download's `licenses/` |

Every SteamOS download's `licenses/` folder also holds the license files of the exact packages its
libraries came from.

## Source code of the LGPL and GPL parts

FFmpeg and libmspack (LGPL-2.1) are compiled into every download, and the SteamOS x86-64 download
carries the GNU C Library (LGPL-2.1) and GCC's runtime libraries (GPL-3.0 with the GCC Runtime
Library Exception). Their complete source code, at the versions in the downloads, is published
next to the downloads of each release on the [Releases](https://github.com/SpeedBreakerProject/speedbreaker/releases)
page, as `SpeedBreaker-<version>-third-party-source.tar.xz`:

- FFmpeg 7.1.1 as released (`ffmpeg-7.1.1.tar.xz`), SpeedBreaker's patch and the build script
  with its configure options;
- libmspack, as XenonRecomp's pinned submodule has it;
- the GNU C Library and GCC, at the Arch Linux package versions the x86-64 download's
  `licenses/README.txt` names, with Arch's build files and patches for them.

If it is ever missing, ask through the project's [issue tracker](https://github.com/SpeedBreakerProject/speedbreaker/issues):
for at least three years after a release, its copy is provided at no charge.

## Tools used to build it, not shipped

- **XenonRecomp** (above) and **XenosRecomp**: hedge-dev and contributors, MIT.
- **extract-xiso**: for developers unpacking their own disc image.
- **Xenia's PowerPC instruction tests**: the recompiler's instruction tests (`tests/`).

## Thanks

The hedge-dev team, for XenonRecomp and the approach of Unleashed Recompiled that SpeedBreaker
follows, and the Xenia project, for years of Xbox 360 research.
