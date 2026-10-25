#!/usr/bin/env python3
# make_zip.py: the Windows tester kit, one zip to hand to testers who play
# their own copy of the game. GPL-3.0-or-later (see COPYING).
#
#   python scripts/windows/make_zip.py --build <build dir> --docs <dir> --deps-src <dir> --out <dir>
#       [--forbid <word>]... [--allow-dirty] [--checklist-name CHECKLIST.md] [--public]
#
# --build     a labelled build's folder (CMake preset windows-clang-tester:
#             SB_WINDOWS_GUI=ON, SB_BUILD_LABEL="Windows beta"), with
#             runtime/SpeedBreaker.exe, its .pdb and generated/build_info.inc
# --docs      README-TESTER.txt, CHECKLIST.md and REPORT-TEMPLATE.txt, and
#             TESTER-NOTES.txt (a short note for this round) if it is there
# --deps-src  the dependencies' source trees the build used: SDL3-*,
#             glslang-* and ffmpeg-* (their license texts)
# --out       where the zip goes, with <zip name>.txt beside it (hashes and
#             the checks' findings, for us: not in the zip)
# --public    the download for everyone (a release asset) instead of a tester
#             kit: SpeedBreaker-<version>-windows-x64-<label>.zip with a
#             .sha256 beside it; README.txt (--docs) is the only document;
#             SpeedBreaker.pdb is checked against the exe but NOT put in the
#             zip (the symbols stay with the project, for crash reports); and
#             licenses\README.txt points to the public repository's commit and
#             the release's third-party source archives for the source code.
# --forbid    a word that must appear nowhere: the zip's text files, the
#             exe's strings, the PDB's (a name, a handle, an email address);
#             as a whole word (not inside a longer one: "alex" doesn't match
#             "scaleX"), in any case, any number of times
#
# The zip, SpeedBreaker-<version>-windows-x64-<label>-<date>-<commit>.zip
# (the version from project(), "beta" for the label "Windows beta": the
# label's words but "windows" and "build"; the build's own date and commit),
# holds at its root (Explorer's Extract All makes the folder):
#   SpeedBreaker.exe, SpeedBreaker.pdb (the same build: names in crash and
#   hang reports), the three tester documents, and licenses\ with every
#   third-party notice the exe needs and a README.txt listing them, which
#   says the source code is in <zip name>-source.zip beside it (made
#   separately, from the commit).
#
# It refuses (exit 1), naming why, when:
#   - the build isn't a labelled one, is dirty (--allow-dirty), or the exe
#     doesn't carry the version, label and commit build_info.inc names;
#   - the exe isn't x64, isn't linked for the Windows subsystem, or doesn't
#     export NvOptimusEnablement and AmdPowerXpressRequestHighPerformance;
#   - the PDB isn't the exe's (the GUID in the exe's debug directory);
#   - a file in the zip isn't one of the expected kinds, has a game-data
#     extension (.xex .iso .bin .wmv ...), or has the SHA-256 of a game file
#     (runtime/install/manifest.inc);
#   - a --forbid word, or a user-profile path (C:\Users\<name>, /Users/<name>,
#     /home/<name>), is in any text file, the exe or the PDB;
#   - an email address is in the tester documents, or one at a personal mail
#     service (gmail.com, outlook.com, ...) is in the exe (FFmpeg's
#     ffmpeg-devel@ffmpeg.org is listed only; third-party license texts name
#     their authors' addresses: listed only).
# Build paths (C:\sb\...: assert messages, the PDB's sources) are allowed and
# counted in the .txt.
import argparse
import datetime
import hashlib
import io
import os
import re
import struct
import sys
import textwrap
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DOCS = ("README-TESTER.txt", "CHECKLIST.md", "REPORT-TEMPLATE.txt")
PUBLIC_DOCS = ("README.txt",)
REPOSITORY = "https://github.com/SpeedBreakerProject/speedbreaker"
OPTIONAL_DOCS = ("TESTER-NOTES.txt",)
GAME_EXTENSIONS = {".xex", ".iso", ".xiso", ".god", ".bin", ".wmv", ".xmv", ".big", ".lzc", ".xma", ".xpr", ".xtl", ".stfs"}

problems = []
notes = []


def fail(text):
    problems.append(text)
    print("PROBLEM: " + text, file=sys.stderr)


def read(path, mode="rb"):
    with open(path, mode, **({} if "b" in mode else {"encoding": "utf-8"})) as f:
        return f.read()


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def crlf(text):
    return text.replace("\r\n", "\n").replace("\n", "\r\n")


# --- The build -------------------------------------------------------------

def build_info(build):
    path = os.path.join(build, "runtime", "generated", "build_info.inc")
    text = read(path, "r")
    info = dict(re.findall(r'#define NFSMW_BUILD_(\w+) "?([^"\n]*)"?', text))
    return info


def pe_info(data):
    """Machine, subsystem, exported names and the CodeView record (GUID, age, PDB name) of a PE file."""
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("not a PE file")
    machine, sections = struct.unpack_from("<HH", data, pe + 4)
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    subsystem = struct.unpack_from("<H", data, opt + 68)[0]
    dirs = opt + (112 if magic == 0x20B else 96)
    table = []
    for i in range(sections):
        s = opt + optional_size + 40 * i
        vsize, va, rawsize, raw = struct.unpack_from("<IIII", data, s + 8)
        table.append((va, max(vsize, rawsize), raw))

    def offset(rva):
        for va, size, raw in table:
            if va <= rva < va + size:
                return raw + rva - va
        raise ValueError("RVA %x outside the sections" % rva)

    def cstring(off):
        return data[off:data.index(b"\0", off)].decode("ascii", "replace")

    exports = []
    export_rva, export_size = struct.unpack_from("<II", data, dirs)
    if export_rva:
        e = offset(export_rva)
        count, names = struct.unpack_from("<I", data, e + 24)[0], struct.unpack_from("<I", data, e + 32)[0]
        for i in range(count):
            exports.append(cstring(offset(struct.unpack_from("<I", data, offset(names) + 4 * i)[0])))
    codeview = None
    debug_rva, debug_size = struct.unpack_from("<II", data, dirs + 6 * 8)
    if debug_rva:
        d = offset(debug_rva)
        for i in range(debug_size // 28):
            kind, size, rva, raw = struct.unpack_from("<IIII", data, d + 28 * i + 12)
            if kind == 2 and data[raw:raw + 4] == b"RSDS":
                codeview = (data[raw + 4:raw + 20], struct.unpack_from("<I", data, raw + 20)[0], cstring(raw + 24))
    return machine, subsystem, exports, codeview


def pdb_guid(path):
    """The GUID and age in a PDB's info stream (MSF 7.00, stream 1)."""
    data = read(path)
    if not data.startswith(b"Microsoft C/C++ MSF 7.00\r\n\x1aDS\0\0\0"):
        raise ValueError("not an MSF 7.00 PDB")
    block, _, _, directory_size, _, map_block = struct.unpack_from("<6I", data, 32)
    blocks = -(-directory_size // block)
    directory_blocks = struct.unpack_from("<%dI" % blocks, data, map_block * block)
    directory = b"".join(data[b * block:(b + 1) * block] for b in directory_blocks)[:directory_size]
    streams = struct.unpack_from("<I", directory, 0)[0]
    sizes = struct.unpack_from("<%dI" % streams, directory, 4)
    at = 4 + 4 * streams
    stream_blocks = []
    for size in sizes:
        n = 0 if size == 0xFFFFFFFF else -(-size // block)
        stream_blocks.append(struct.unpack_from("<%dI" % n, directory, at))
        at += 4 * n
    info = b"".join(data[b * block:(b + 1) * block] for b in stream_blocks[1])[:sizes[1]]
    _, _, age = struct.unpack_from("<III", info, 0)
    return info[12:28], age


def guid_text(raw):
    a, b, c = struct.unpack_from("<IHH", raw, 0)
    return "%08X-%04X-%04X-%s-%s" % (a, b, c, raw[8:10].hex().upper(), raw[10:16].hex().upper())


# --- Licenses ---------------------------------------------------------------

def find_dir(parent, prefix):
    for name in sorted(os.listdir(parent)):
        if name.lower().startswith(prefix.lower()) and os.path.isdir(os.path.join(parent, name)):
            return os.path.join(parent, name)
    raise FileNotFoundError("no %s* folder in %s" % (prefix, parent))


def between(text, start, end, include_end=True):
    i = text.index(start)
    j = text.index(end, i + len(start))
    return text[i:j + (len(end) if include_end else 0)]


def comment_text(block):
    """A C comment block's text without its ' * ' margins."""
    lines = []
    for line in block.splitlines():
        line = line.strip()
        if line in ("/*", "*/", "/**"):
            continue
        line = re.sub(r"^\*+ ?", "", line)
        lines.append(line.rstrip(" *"))
    return "\n".join(lines).strip() + "\n"


def licenses(deps):
    """[(file name in licenses\\, text)] and the README's lines, from the trees on disk."""
    sdl = find_dir(deps, "SDL3-")
    glslang = find_dir(deps, "glslang-")
    ffmpeg = find_dir(deps, "ffmpeg-")
    xr = os.path.join(ROOT, "tools", "XenonRecomp")
    imgui = os.path.join(ROOT, "runtime", "thirdparty", "imgui")
    t = lambda *p: read(os.path.join(*p), "r")
    files = []

    def add(name, text):
        files.append((name, text))
        return name

    readme = []

    def entry(what, license, names):
        wrap = lambda text, first, rest: "\n".join(textwrap.wrap(text, 78, initial_indent=first, subsequent_indent=rest,
                                                                  break_on_hyphens=False, break_long_words=False))
        readme.append("%s\n%s\n%s" % (wrap(what, "- ", "  "), wrap(license, "    License: ", "             "),
                                        wrap(", ".join(names), "    Text:    ", "             ")))

    entry("SpeedBreaker, this build: an unofficial native port of Need for Speed: Most Wanted (2005, Xbox 360)",
          "GPL-3.0-or-later", [add("SpeedBreaker-COPYING.txt", t(ROOT, "COPYING")),
                               add("SpeedBreaker-NOTICE.txt", t(ROOT, "NOTICE"))])
    entry("Unleashed Recompiled (hedge-dev and contributors): parts of the kernel, XAM, memory and threading code "
          "are adapted from it", "GPL-3.0-or-later, the same license", ["SpeedBreaker-COPYING.txt"])
    entry("XenonRecomp and its XenonUtils (hedge-dev and contributors): the recompiler, and the XEX loader in the exe",
          "MIT", [add("XenonRecomp-LICENSE.txt", t(xr, "LICENSE.md"))])
    # Xenia's own LICENSE isn't in these trees: its files' headers name the
    # holder and "the BSD license", the 3-clause one (SPDX's text from
    # glslang's LICENSES), with that holder in place of the template's line.
    holders = set()
    for folder in (os.path.join(ROOT, "runtime"), os.path.join(xr, "XenonUtils")):
        for parent, _, names in os.walk(folder):
            for name in names:
                if name.endswith((".h", ".cpp", ".inc")):
                    holders.update(re.findall(rb"Copyright (\d{4}) Ben Vanik", read(os.path.join(parent, name))))
    years = sorted(y.decode() for y in holders)
    bsd = t(glslang, "LICENSES", "BSD-3-Clause.txt")
    bsd = bsd[bsd.index("\n") + 1:].lstrip("\n")
    entry("Xenia (Ben Vanik and the Xenia contributors): GPU register, microcode and Xenos headers, and XEX patching",
          "BSD 3-Clause", [add("Xenia-LICENSE.txt",
                               "Xenia - Xbox 360 Emulator Research Project, https://github.com/xenia-project/xenia\n"
                               "Copyright %s Ben Vanik. All rights reserved.\n"
                               "Released under the BSD license (3-clause):\n\n" %
                               ("-".join([years[0], years[-1]]) if len(years) > 1 else years[0]) + bsd)])
    ucode = t(ROOT, "runtime", "gpu", "xenos", "ucode.h")
    entry("freedreno (Rob Clark): the Xenos shader instruction formats, through Xenia", "MIT",
          [add("freedreno-LICENSE.txt", comment_text(between(ucode, "/*\n * Copyright (c) 2012 Rob Clark", "*/")))])
    patch = t(ROOT, "patches", "ffmpeg", "0001-xma-single-stream-no-holdback.patch")
    entry("FFmpeg %s: its XMA and WMA Pro audio decoders, linked in statically, with one change of "
          "SpeedBreaker's (FFmpeg-patch.txt): single-stream XMA (one stream per decoder, as on the Xbox 360) gives "
          "out every decoded sample at once instead of holding the last 4096 back" % os.path.basename(ffmpeg).split("-", 1)[1],
          "LGPL-2.1-or-later", [add("FFmpeg-COPYING.LGPLv2.1.txt", t(ffmpeg, "COPYING.LGPLv2.1")),
                                add("FFmpeg-patch.txt", patch)])
    entry("libmspack (Stuart Caie): LZX decompression of the XEX", "LGPL-2.1",
          [add("libmspack-COPYING.LIB.txt", t(xr, "thirdparty", "libmspack", "libmspack", "COPYING.LIB"))])
    entry("SDL %s: the window, input and audio, with HIDAPI (controllers) and yuv2rgb inside it" %
          os.path.basename(sdl).split("-", 1)[1], "zlib; HIDAPI used under its BSD-style license; yuv2rgb BSD 3-Clause",
          [add("SDL3-LICENSE.txt", t(sdl, "LICENSE.txt")),
           add("SDL3-hidapi-LICENSE.txt", t(sdl, "src", "hidapi", "LICENSE.txt")),
           add("SDL3-hidapi-LICENSE-bsd.txt", t(sdl, "src", "hidapi", "LICENSE-bsd.txt")),
           add("SDL3-yuv2rgb-LICENSE.txt", t(sdl, "src", "video", "yuv2rgb", "LICENSE"))])
    entry("glslang %s (Khronos Group): compiles the game's shaders at run time" % os.path.basename(glslang).split("-", 1)[1],
          "BSD 3-Clause and others, as listed in its LICENSE", [add("glslang-LICENSE.txt", t(glslang, "LICENSE.txt"))])
    entry("Dear ImGui (Omar Cornut): the installer and settings screens", "MIT",
          [add("DearImGui-LICENSE.txt", t(imgui, "LICENSE.txt"))])
    # Dear ImGui's two built-in fonts stay in the exe (IMGUI_DISABLE_DEFAULT_FONT
    # isn't defined) though only Roboto is used. Their holders, from the
    # notices in imgui_draw.cpp, over the MIT permission text of Dear
    # ImGui's own LICENSE.txt.
    draw = t(imgui, "imgui_draw.cpp")
    clean = re.search(r"// MIT License / (Copyright \(c\) [^\n]*Tristan Grimmer)\n", draw).group(1)
    forever = re.search(r"// MIT license / (Copyright \(c\) [^\n]*Tristan Grimmer)\n", draw).group(1)
    mit = t(imgui, "LICENSE.txt")
    mit = mit[mit.index("Permission is hereby granted"):]
    entry("ProggyClean and ProggyForever (Tristan Grimmer; ProggyForever also Disco Hello): Dear ImGui's built-in "
          "fonts, inside it", "MIT",
          [add("Proggy-fonts-LICENSE.txt",
               "ProggyClean.ttf, https://github.com/bluescan/proggyfonts\n%s\n\n"
               "ProggyForever (its minimal Regular), https://github.com/ocornut/proggyforever\n%s\n\n"
               "Both under the MIT License:\n\n%s" % (clean, forever.replace(", Copyright", "\nCopyright"), mit))])
    entry("stb_truetype, stb_rect_pack and stb_textedit (Sean Barrett), inside Dear ImGui",
          "MIT, or public domain", [add("stb-LICENSE.txt", between(t(imgui, "imstb_truetype.h"),
                                                                     "This software is available under 2 licenses", "\n*/",
                                                                     include_end=False) + "\n")])
    entry("Roboto (Google): the user interface's font", "Apache-2.0",
          [add("Roboto-LICENSE-Apache-2.0.txt", t(glslang, "LICENSES", "Apache-2.0.txt"))])
    entry("o1heap (Pavel Kirienko): the game's memory allocator", "MIT",
          [add("o1heap-LICENSE.txt", t(ROOT, "runtime", "thirdparty", "o1heap", "LICENSE"))])
    entry("QR Code generator library (Project Nayuki): the download page's QR code in Settings > Advanced > "
          "Check for Updates", "MIT",
          [add("qrcodegen-LICENSE.txt", t(ROOT, "runtime", "thirdparty", "qrcodegen", "LICENSE"))])
    entry("AMD FidelityFX Super Resolution 1 (Advanced Micro Devices): Settings > Graphics > Upscaling's FSR 1 "
          "(its shader code and constant setup, unmodified)", "MIT",
          [add("FidelityFX-FSR-LICENSE.txt", t(ROOT, "runtime", "thirdparty", "fsr1", "LICENSE.txt"))])
    entry("Vulkan-Headers and SPIRV-Headers (The Khronos Group): headers the game and glslang are built with",
          "Apache-2.0 or MIT (Vulkan-Headers); MIT (SPIRV-Headers)",
          [add("Vulkan-Headers-LICENSE.txt", t(ROOT, "LICENSES", "Vulkan-Headers-MIT.txt")),
           add("SPIRV-Headers-LICENSE.txt", t(ROOT, "LICENSES", "SPIRV-Headers.txt"))])
    entry("tiny-AES-c: XEX decryption", "The Unlicense (public domain)",
          [add("tiny-AES-c-unlicense.txt", t(xr, "thirdparty", "tiny-AES-c", "unlicense.txt"))])
    entry("toml++ (Mark Gillard): settings.toml", "MIT",
          [add("tomlplusplus-LICENSE.txt", t(xr, "thirdparty", "tomlplusplus", "LICENSE"))])
    entry("SIMDe (Evan Nemerson and contributors): SIMD in the recompiled code", "MIT",
          [add("SIMDe-COPYING.txt", t(xr, "thirdparty", "simde", "COPYING"))])
    sha1 = t(xr, "thirdparty", "TinySHA1", "TinySHA1.hpp")
    entry("TinySHA1 (Saurav Mohapatra): SHA-1 in the XEX loader", "ISC-style",
          [add("TinySHA1-LICENSE.txt", comment_text(between(sha1, "/*", "*/")))])
    return files, readme


def licenses_readme(build_line, readme, source_zip, public=None):
    # The source goes next to the build (GPL-3.0 section 6), as <zip name>-
    # source.zip: kit 1 was repacked by hand to say so; this says it itself.
    # public: (commit, version) for the release download, whose source is the
    # public repository at that commit plus the release's third-party source
    # archives (as NOTICE says for every download).
    if public:
        commit, version = public
        head = ("LICENSES\n"
                "========\n\n"
                "%s\n\n"
                "SpeedBreaker is free software under the GNU General Public License, version 3\n"
                "or (at your option) any later version (SpeedBreaker-COPYING.txt and\n"
                "SpeedBreaker-NOTICE.txt), provided \"as is\", without warranty of any kind.\n"
                "Its complete source code is at %s, commit\n"
                "%s. The complete source code of the LGPL code by others in this\n"
                "build (FFmpeg, with SpeedBreaker's change to it and the way this build\n"
                "configures it, and libmspack) is published next to this download on the\n"
                "project's Releases page, as SpeedBreaker-%s-windows-third-party-source.tar.xz.\n"
                "If it is ever missing, ask through the project's issue tracker.\n\n"
                "This build contains code from the projects below, under the licenses named,\n"
                "whose texts are in this folder.\n\n" % (build_line, REPOSITORY, commit, version))
        return head + "\n\n".join(readme) + licenses_tail()
    head = ("LICENSES\n"
            "========\n\n"
            "%s\n\n"
            "SpeedBreaker is free software under the GNU General Public License, version 3\n"
            "or (at your option) any later version (SpeedBreaker-COPYING.txt and\n"
            "SpeedBreaker-NOTICE.txt), provided \"as is\", without warranty of any kind.\n"
            "The complete source code of this build, with the parts of other projects it\n"
            "contains and the change it makes to FFmpeg, is in\n"
            "%s, shared in the\n"
            "same place as this build. Ask the person who gave you this build if it's missing.\n\n"
            "This build contains code from the projects below, under the licenses named,\n"
            "whose texts are in this folder.\n\n" % (build_line, source_zip))
    return head + "\n\n".join(readme) + licenses_tail()


def licenses_tail():
    return ("\n\nAlso linked in: Microsoft's C and C++ runtime libraries (statically, from the\n"
            "Visual Studio Build Tools). The Vulkan driver (vulkan-1.dll) is your graphics\n"
            "driver's and isn't part of this build. No game files are included.\n")


# --- Checks -----------------------------------------------------------------

USER_PATH = re.compile(rb"(?i)(?:[a-z]:[\\/]+users[\\/]+|/users/|/home/)([a-z0-9._ -]{1,64})")
EMAIL = re.compile(rb"[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)*\.[A-Za-z]{2,}")
BUILD_PATH = re.compile(rb"(?i)c:[\\/]+sb[\\/]+[a-z0-9._-]+(?:[\\/]+[a-z0-9._-]+)?")
GENERIC_PROFILES = {b"public", b"default", b"all users", b"default user", b"shared"}
PERSONAL_MAIL = re.compile(rb"(?i)@(gmail|googlemail|outlook|hotmail|live|msn|icloud|me|mac|yahoo|proton|protonmail|aol|gmx)\.")


def strings(data):
    """Printable runs of 6 or more: ASCII, and UTF-16LE turned into ASCII."""
    out = [m.group() for m in re.finditer(rb"[\x20-\x7e]{6,}", data)]
    out += [m.group().replace(b"\0", b"") for m in re.finditer(rb"(?:[\x20-\x7e]\x00){6,}", data)]
    return out


def scan(label, data, forbid, *, binary, emails_fail, report):
    """Forbidden words, user-profile paths and emails in `data` (bytes); build paths counted.
    emails_fail: True, any email fails; "personal", one at a personal mail service does."""
    runs = strings(data) if binary else [data]
    found_paths, found_emails, builds = set(), set(), {}
    words = [(w, re.compile(rb"(?i)(?<![a-z0-9])" + re.escape(w.encode()) + rb"(?![a-z0-9])")) for w in forbid]
    for run in runs:
        for word, pattern in words:
            m = pattern.search(run)
            if m:
                i = m.start()
                fail("%s: contains the forbidden word %r: ...%s..." % (label, word, run[max(0, i - 40):i + 40].decode("ascii", "replace")))
        for m in USER_PATH.finditer(run):
            name = m.group(1).strip().lower()
            if name.split(b"\\")[0] not in GENERIC_PROFILES:
                found_paths.add(m.group().decode("ascii", "replace"))
        for m in EMAIL.finditer(run):
            found_emails.add(m.group().decode("ascii", "replace"))
        for m in BUILD_PATH.finditer(run):
            key = re.sub(r"[\\/]+", r"\\", m.group().decode("ascii", "replace"))
            builds[key] = builds.get(key, 0) + 1
    for p in sorted(found_paths):
        fail("%s: a user-profile path: %s" % (label, p))
    if found_emails:
        personal = sorted(e for e in found_emails if PERSONAL_MAIL.search(e.encode()))
        if emails_fail is True or (emails_fail == "personal" and personal):
            fail("%s: email addresses: %s" % (label, ", ".join(sorted(found_emails) if emails_fail is True else personal)))
        report("%s: email addresses (listed): %s" % (label, ", ".join(sorted(found_emails))))
    if builds:
        report("%s: build paths (allowed): %s" % (label, ", ".join("%s (%d)" % kv for kv in sorted(builds.items()))))


def game_hashes():
    text = read(os.path.join(ROOT, "runtime", "install", "manifest.inc"), "r")
    return set(re.findall(r'MANIFEST_FILE\("[^"]+", \d+ull, "([0-9a-f]{64})"\)', text))


# --- Main -------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description="The Windows tester kit (see the header).")
    ap.add_argument("--build", required=True)
    ap.add_argument("--docs", required=True)
    ap.add_argument("--deps-src", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--forbid", action="append", default=[])
    ap.add_argument("--allow-dirty", action="store_true")
    ap.add_argument("--checklist-name", default="CHECKLIST.md")
    ap.add_argument("--public", action="store_true")
    args = ap.parse_args()

    info = build_info(args.build)
    commit, release, version = info.get("COMMIT", "unknown"), info.get("RELEASE", ""), info.get("VERSION", "")
    dirty = info.get("DIRTY", "0") == "1"
    m = re.fullmatch(r"([A-Za-z0-9 ]+) (\d{4}-\d{2}-\d{2})", release)
    if not m:
        sys.exit("make_zip.py: %s isn't a labelled build (SB_BUILD_LABEL; preset windows-clang-tester)" % args.build)
    label, day = m.group(1), m.group(2)
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        sys.exit("make_zip.py: %s's build_info.inc has no version (%r): a build from before v0.1.0's project() version" %
                 (args.build, version))
    build_id = commit + ("-dirty" if dirty else "")
    if dirty and not args.allow_dirty:
        sys.exit("make_zip.py: the build is dirty (%s): commit first, or --allow-dirty" % build_id)
    name_line = "SpeedBreaker v%s %s (%s)" % (version, release, build_id)
    kind = "-".join(w for w in label.lower().split() if w not in ("build", "windows")) or "build"
    stem = "SpeedBreaker-%s-windows-x64-%s-%s-%s" % (version, kind, day, build_id)
    if args.public:
        if dirty:
            sys.exit("make_zip.py: --public needs a clean build (%s)" % build_id)
        stem = "SpeedBreaker-%s-windows-x64-%s" % (version, kind)
    print("build: " + name_line)

    exe_path = os.path.join(args.build, "runtime", "SpeedBreaker.exe")
    pdb_path = os.path.join(args.build, "runtime", "SpeedBreaker.pdb")
    exe, pdb = read(exe_path), read(pdb_path)

    # The exe is that build, for testers (the line is put together at run
    # time from the version, the label and the commit, each a string in the
    # exe).
    for part in (version, release, commit):
        if part.encode() not in exe:
            fail("the exe doesn't carry %r of its build line %r (another build than build_info.inc's?)" % (part, name_line))
    machine, subsystem, exports, codeview = pe_info(exe)
    if machine != 0x8664:
        fail("the exe isn't x64 (machine 0x%04X)" % machine)
    if subsystem != 2:
        fail("the exe isn't linked for the Windows subsystem (subsystem %d; SB_WINDOWS_GUI=ON)" % subsystem)
    for wanted in ("NvOptimusEnablement", "AmdPowerXpressRequestHighPerformance"):
        if wanted not in exports:
            fail("the exe doesn't export %s (exports: %s)" % (wanted, ", ".join(exports) or "none"))
    pdb_id, pdb_age = pdb_guid(pdb_path)
    if not codeview or codeview[0] != pdb_id:
        fail("SpeedBreaker.pdb isn't the exe's (exe %s, pdb %s)" % (guid_text(codeview[0]) if codeview else "no debug record",
                                                                    guid_text(pdb_id)))
    notes.append("exe: x64, subsystem %d (Windows), exports %s; debug record %s age %d, %s" %
                 (subsystem, ", ".join(exports), guid_text(codeview[0]) if codeview else "-", codeview[1] if codeview else 0,
                  codeview[2] if codeview else "-"))
    notes.append("pdb: GUID %s, age %d (matches the exe)" % (guid_text(pdb_id), pdb_age))

    # The zip's members.
    members = [("SpeedBreaker.exe", exe)] + ([] if args.public else [("SpeedBreaker.pdb", pdb)])
    docs = PUBLIC_DOCS if args.public else DOCS + tuple(d for d in OPTIONAL_DOCS if os.path.exists(os.path.join(args.docs, d)))
    for doc in docs:
        text = read(os.path.join(args.docs, doc))
        members.append((args.checklist_name if doc == "CHECKLIST.md" else doc, text))
    license_files, readme = licenses(args.deps_src)
    members.append(("licenses/README.txt", crlf(licenses_readme(name_line, readme, stem + "-source.zip",
                                                                (commit, version) if args.public else None)).encode("utf-8")))
    for name, text in license_files:
        members.append(("licenses/" + name, crlf(text).encode("utf-8")))

    # Checks.
    hashes = game_hashes()
    allowed = re.compile(r"(SpeedBreaker\.(exe|pdb)|[A-Z-]+\.(txt|md)|licenses/[A-Za-z0-9.+_-]+\.txt)")
    for name, data in members:
        ext = os.path.splitext(name)[1].lower()
        if not allowed.fullmatch(name):
            fail("%s: not one of the kit's kinds of file" % name)
        if ext in GAME_EXTENSIONS:
            fail("%s: a game-data extension" % name)
        if sha256(data) in hashes:
            fail("%s: has the SHA-256 of a game file" % name)
    for name, data in members:
        if name.endswith((".txt", ".md")):
            third_party = name.startswith("licenses/") and name != "licenses/README.txt"
            scan(name, data, args.forbid, binary=False, emails_fail=not third_party, report=notes.append)
            if not third_party:
                try:
                    data.decode("ascii")
                except UnicodeDecodeError:
                    notes.append("%s: not plain ASCII" % name)
    scan("SpeedBreaker.exe", exe, args.forbid, binary=True, emails_fail="personal", report=notes.append)
    scan("SpeedBreaker.pdb", pdb, args.forbid, binary=True, emails_fail=False, report=notes.append)

    if problems:
        print("make_zip.py: %d problem(s); no zip made" % len(problems), file=sys.stderr)
        return 1

    os.makedirs(args.out, exist_ok=True)
    zip_path = os.path.join(args.out, stem + ".zip")
    stamp = datetime.datetime.now().timetuple()[:6]
    with zipfile.ZipFile(zip_path + ".partial", "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name, data in members:
            entry = zipfile.ZipInfo(name, stamp)
            entry.compress_type = zipfile.ZIP_DEFLATED
            entry.external_attr = 0o644 << 16
            z.writestr(entry, data)
    os.replace(zip_path + ".partial", zip_path)
    with zipfile.ZipFile(zip_path) as z:
        bad = z.testzip()
        if bad:
            sys.exit("make_zip.py: %s fails its own CRC check at %s" % (zip_path, bad))
        listing = [(i.filename, i.file_size, i.compress_size) for i in z.infolist()]

    zip_sha = sha256(read(zip_path))
    if args.public:
        # As the other downloads' .sha256: "<hash>  <name>" and a newline.
        with open(zip_path + ".sha256", "w", encoding="ascii", newline="\n") as f:
            f.write("%s  %s\n" % (zip_sha, os.path.basename(zip_path)))
    lines = [name_line, "zip: %s (%d bytes), SHA-256 %s" % (os.path.basename(zip_path), os.path.getsize(zip_path), zip_sha),
              "exe: SHA-256 %s (%d bytes)" % (sha256(exe), len(exe)),
              "pdb: SHA-256 %s (%d bytes)%s" % (sha256(pdb), len(pdb), " (not in the zip: --public)" if args.public else ""), ""]
    lines += ["%-46s %10d -> %10d" % entry for entry in listing]
    lines += ["", "checks: no game-data extension or game file's SHA-256; no forbidden word (%d given) or user-profile path"
              % len(args.forbid)] + notes
    with open(os.path.join(args.out, stem + ".txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
