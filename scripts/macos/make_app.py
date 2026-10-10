#!/usr/bin/env python3
# SpeedBreaker. GPL-3.0-or-later (see COPYING).
#
# make_app.py [build dir] [output dir] [--dmg] [--min-macos=<version>]:
# SpeedBreaker.app for macOS (Apple silicon) from a build (default
# build/main), in build/app:
#   Contents/MacOS/SpeedBreaker      the game
#   Contents/Frameworks/             the libraries it links (SDL3, the Vulkan
#                                    loader, glslang, SPIRV-Tools, and what
#                                    they link), named @rpath/<file>, and
#                                    MoltenVK, the Vulkan driver (Homebrew's
#                                    1.4.2: it runs on macOS 12+; another
#                                    version only with MOLTENVK_VERSION=<it>)
#   Contents/Resources/vulkan/icd.d/ MoltenVK's manifest (the Vulkan loader
#                                    looks for drivers in the app's bundle)
#   Contents/Resources/AppIcon.icns  from scripts/macos/AppIcon.png
#   Contents/Resources/licenses/     COPYING, NOTICE, THIRD_PARTY_NOTICES.md,
#                                    LICENSES/, and each bundled library's
#                                    own license files (from build_deps.sh's
#                                    prefix, with what each was built from, or
#                                    from the library's Homebrew keg)
#   Contents/Info.plist              a game (macOS Game Mode when fullscreen),
#                                    versioned from the build: CFBundleShort-
#                                    VersionString is project()'s version,
#                                    CFBundleVersion the commit's number in
#                                    the history, LSMinimumSystemVersion the
#                                    build's deployment target, which every
#                                    binary in the app must match (checked)
#
# The macOS the app runs on: the build's deployment target (CMakeLists.txt's
# default, 15.0, unless the build was configured with another). Every library
# in the app, and the FFmpeg the game links statically, must be built for it
# or an older macOS; a newer one is refused, with the two ways out:
#   - the libraries built for it by scripts/macos/build_deps.sh <prefix>, the
#     build configured with -DCMAKE_PREFIX_PATH=<prefix>
#     -DFFMPEG_XMA=<prefix>/ffmpeg-xma (how the release is built);
#   - or Homebrew's libraries, which are built for the macOS they were made
#     on (26.0 on macOS 26), with the build configured for that macOS:
#     -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0. Fine for an app of your own; it
#     won't open on an older macOS, and this script says so.
# --min-macos=<version> (release scripts): refuse an app for any other macOS.
# scripts/macos/check_app.py checks the app or dmg it makes (the release's
# gate: the floor, every binary's, what it loads and imports).
# signed ad hoc (Apple silicon runs nothing unsigned, and rewriting a
# library's names breaks its signature). Nothing of Homebrew is needed to
# run it. No game data: the game installs from the player's disc image on
# the first run, into ~/Library/Application Support/SpeedBreaker/game.
#
# The game's binary has its debug map stripped (the paths of the objects it
# was linked from); its symbol table stays, so crash reports name host
# functions, and they name the recompiled ones from the game's own table
# (report/crash.cpp) either way. The full symbols go to
# <output>/symbols/SpeedBreaker-<version>-<commit>-macos.dSYM, for us: never
# published, they map a player's report back to the exact build.
#
# --dmg: also <output>/SpeedBreaker-macos.dmg (the app and a shortcut to
# Applications, to drag it onto) with its .sha256, the release download, and
# a copy named by the version (SpeedBreaker-<version>-macos.dmg, a hard link).
import json, os, plistlib, re, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ARGS = [a for a in sys.argv[1:] if not a.startswith('--')]
OPTIONS = [a for a in sys.argv[1:] if a.startswith('--')]
DMG = '--dmg' in OPTIONS
MIN_MACOS = next((a.split('=', 1)[1] for a in OPTIONS if a.startswith('--min-macos=')), None)
for a in OPTIONS:
    if a != '--dmg' and not a.startswith('--min-macos='):
        raise SystemExit(f'make_app.py: unknown option {a} (usage: make_app.py [build dir] [output dir] [--dmg] [--min-macos=<version>])')
BUILD = os.path.abspath(ARGS[0] if len(ARGS) > 0 else os.path.join(ROOT, 'build/main'))
OUT = os.path.abspath(ARGS[1] if len(ARGS) > 1 else os.path.join(ROOT, 'build/app'))
BIN = os.path.join(BUILD, 'runtime/SpeedBreaker')
MOLTENVK = os.environ.get('MOLTENVK', '/opt/homebrew/opt/molten-vk/lib/libMoltenVK.dylib')
# The MoltenVK the releases ship (v0.1.0 and on): the Vulkan driver, so another
# version can change how the game runs and how fast. A `brew upgrade` must not
# change it silently: another is taken only when MOLTENVK_VERSION names it.
MOLTENVK_PINNED = '1.4.2'
# Projects compiled into a bundled library that come with their own license:
# their files go next to the library's (LICENSES/ has them).
EMBEDDED = {'molten-vk': [('cereal, BSD-3-Clause', 'LICENSES/cereal-BSD-3-Clause.txt', 'cereal-LICENSE.txt')]}


def run(*args):
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


def deps(path):
    """The libraries `path` links (not the system's), as written in it."""
    lines = run('otool', '-L', path).splitlines()[1:]
    names = [l.strip().split(' (')[0] for l in lines]
    own = run('otool', '-D', path).splitlines()[1:]  # a library lists itself first
    return [n for n in names if n not in own and not n.startswith(('/usr/lib/', '/System/'))]


def rpaths(path):
    out = run('otool', '-l', path)
    return re.findall(r'cmd LC_RPATH\n\s+cmdsize \d+\n\s+path (.+?) \(offset', out)


def resolve(ref, referrer):
    """A dependency's file: an absolute path, or @rpath/@loader_path ones."""
    if ref.startswith('@rpath/'):
        for rp in rpaths(referrer):
            rp = rp.replace('@loader_path', os.path.dirname(referrer)).replace('@executable_path', os.path.dirname(BIN))
            cand = os.path.join(rp, ref[len('@rpath/'):])
            if os.path.exists(cand):
                return cand
        raise SystemExit(f'make_app.py: {ref} (from {referrer}) not found in its rpaths')
    if ref.startswith('@loader_path/'):
        return os.path.join(os.path.dirname(referrer), ref[len('@loader_path/'):])
    return ref


def cache_value(key):
    """A value from the build's CMakeCache.txt, or ''."""
    try:
        with open(os.path.join(BUILD, 'CMakeCache.txt')) as f:
            for line in f:
                if line.startswith(key + ':'):
                    return line.split('=', 1)[1].strip()
    except OSError:
        pass
    return ''


def build_identity():
    """(version, commit, dirty) as the build compiled them in (build_info.inc)."""
    info = {}
    try:
        with open(os.path.join(BUILD, 'runtime/generated/build_info.inc')) as f:
            for m in re.finditer(r'#define NFSMW_BUILD_(\w+) "?([^"\n]*)"?', f.read()):
                info[m.group(1)] = m.group(2)
    except OSError:
        pass
    version = info.get('VERSION') or cache_value('CMAKE_PROJECT_VERSION')
    if not re.fullmatch(r'\d+\.\d+\.\d+', version or ''):
        raise SystemExit(f'make_app.py: no version in {BUILD} (project() in CMakeLists.txt; build first)')
    return version, info.get('COMMIT', 'unknown'), info.get('DIRTY', '0') == '1'


def build_number(commit):
    """CFBundleVersion: the commit's number in its history (an integer that
    grows from one release to the next), 0 when git can't tell."""
    out = subprocess.run(['git', '-C', cache_value('CMAKE_HOME_DIRECTORY') or ROOT, 'rev-list', '--count', commit],
                         capture_output=True, text=True).stdout.strip()
    return out if out.isdigit() else '0'


def version_tuple(v):
    """'15.0' -> (15, 0, 0), comparable."""
    v = tuple(int(x) for x in str(v).split('.'))
    return v + (0,) * (3 - len(v))


def minos_all(path):
    """The oldest macOS each Mach-O in a file runs on (LC_BUILD_VERSION or the
    older LC_VERSION_MIN_MACOSX): one for a binary or library, one per member
    of a static archive."""
    found, field = [], None
    for line in run('otool', '-l', path).splitlines():
        line = line.strip()
        if line.startswith('cmd '):
            field = {'cmd LC_BUILD_VERSION': 'minos', 'cmd LC_VERSION_MIN_MACOSX': 'version'}.get(line)
        elif field and line.startswith(field + ' '):
            found.append(version_tuple(line.split()[1]))
            field = None
    return found


def minos(path):
    """The oldest macOS a Mach-O file runs on, as a tuple ((0, 0, 0) if it
    doesn't say)."""
    found = minos_all(path)
    return max(found) if found else (0, 0, 0)


def project_default_target():
    """CMakeLists.txt's default deployment target (SB_OSX_DEFAULT_TARGET), or None."""
    try:
        with open(os.path.join(cache_value('CMAKE_HOME_DIRECTORY') or ROOT, 'CMakeLists.txt')) as f:
            m = re.search(r'set\(SB_OSX_DEFAULT_TARGET "([0-9.]+)"\)', f.read())
            return m.group(1) if m else None
    except OSError:
        return None


def moltenvk_version():
    """The version of the MoltenVK to bundle (its Homebrew keg's, or
    MOLTENVK_VERSION's), refused unless it's the pinned one or
    MOLTENVK_VERSION names it."""
    k = keg(MOLTENVK)
    found = k.split('/')[-1] if k else None
    wanted = os.environ.get('MOLTENVK_VERSION')
    if found and wanted and found != wanted:
        raise SystemExit(f'make_app.py: {MOLTENVK} is MoltenVK {found}, not MOLTENVK_VERSION={wanted}')
    version = found or wanted
    if version != MOLTENVK_PINNED and not wanted:
        raise SystemExit(f'make_app.py: {MOLTENVK} is MoltenVK {version or "of an unknown version (not a Homebrew keg)"}, '
                         f'not {MOLTENVK_PINNED}, the one the releases ship (the Vulkan driver: another can change how the '
                         f'game runs). To bundle it anyway, set MOLTENVK_VERSION=<its version>.')
    return version


def keg(path):
    """The Homebrew keg a library file is in (/opt/homebrew/Cellar/<name>/<version>), or None."""
    m = re.match(r'(.*/Cellar/[^/]+/[^/]+)/', os.path.realpath(path))
    return m.group(1) if m else None


DEPS_RECORD = 'share/speedbreaker-deps.json'   # written by scripts/macos/build_deps.sh


def deps_prefix(path):
    """The scripts/macos/build_deps.sh prefix a library file is in (<prefix>/lib/<file>, or
    <prefix>/ffmpeg-xma/lib/<file>), or None."""
    d = os.path.dirname(os.path.realpath(path))
    for prefix in (os.path.dirname(d), os.path.dirname(os.path.dirname(d))):
        if os.path.isfile(os.path.join(prefix, DEPS_RECORD)):
            return prefix
    return None


def copy_licenses(resources, libraries):
    """Contents/Resources/licenses: SpeedBreaker's notices and every bundled
    library's own license files: from the build_deps.sh prefix it was built
    in (with the project, version, source and SHA-256 it was built from), or
    from its Homebrew keg. A prefix's projects that the game and the
    libraries are built with but that aren't a library of their own here
    (the Vulkan and SPIR-V headers, the FFmpeg the game links statically)
    get their license files too."""
    licenses = os.path.join(resources, 'licenses')
    os.makedirs(licenses)
    for name in ('COPYING', 'NOTICE', 'THIRD_PARTY_NOTICES.md'):
        shutil.copy2(os.path.join(ROOT, name), licenses)
    shutil.copytree(os.path.join(ROOT, 'LICENSES'), os.path.join(licenses, 'LICENSES'))
    lines = ['SpeedBreaker is free software under the GPL-3.0-or-later (COPYING, NOTICE). What it is',
             'built from and with, and each license, is in THIRD_PARTY_NOTICES.md and LICENSES/.', '',
             'The libraries in Contents/Frameworks, with their own license files here:']
    records = {}   # prefix -> its build_deps.sh record
    used = set()   # (prefix, project) whose license files are here

    def record(prefix):
        if prefix not in records:
            with open(os.path.join(prefix, DEPS_RECORD)) as f:
                records[prefix] = json.load(f)
        return records[prefix]

    def copy_project(prefix, project):
        src = os.path.join(prefix, 'share/licenses', project)
        dest = os.path.join(licenses, project)
        files = record(prefix)['projects'][project]['licenses']
        if (prefix, project) not in used:
            if os.path.exists(dest):
                raise SystemExit(f'make_app.py: two sources of license files for {project}')
            for f in files:
                os.makedirs(os.path.dirname(os.path.join(dest, f)), exist_ok=True)
                shutil.copy2(os.path.join(src, f), os.path.join(dest, f))
            used.add((prefix, project))
        return files

    def describe(prefix, project, built=True):
        r = record(prefix); j = r['projects'][project]
        built = f', built for macOS {r["deployment_target"]}+ ({r["architecture"]})' if built else ''
        return f'{project} {j["version"]}{built}, from {j["url"]} (SHA-256 {j["sha256"]})'

    missing = []
    for lib, src in sorted(libraries.items()):
        found = []
        prefix = deps_prefix(src)
        k = keg(src)
        if prefix:
            projects = record(prefix)['projects']
            project = next((n for n, j in projects.items()
                            if lib in j['libraries'] or os.path.basename(os.path.realpath(src)) in j['libraries']), None)
            if project:
                found = copy_project(prefix, project)
                lines.append(f'  {lib}: {describe(prefix, project)}; license files in {project}/: {", ".join(found)}')
            else:
                lines.append(f'  {lib}: from {src} (not in {os.path.join(prefix, DEPS_RECORD)})')
        elif k:
            package, version = k.split('/')[-2:]
            dest = os.path.join(licenses, package)
            candidates = [os.path.join(k, f) for f in os.listdir(k)
                          if re.fullmatch(r'(LICENSE|COPYING|NOTICE)([.-](txt|md|rst|LIB|L?GPL\S*))?', f, re.I)]
            share = os.path.join(k, 'share/licenses')
            if os.path.isdir(share):
                for d, _, files in os.walk(share):
                    candidates += [os.path.join(d, f) for f in files]
            for c in candidates:
                if os.path.isfile(c):
                    os.makedirs(dest, exist_ok=True)
                    target = os.path.join(dest, os.path.basename(c))
                    if not os.path.exists(target):
                        shutil.copy2(c, target)
                    found.append(os.path.basename(c))
            embedded = []
            for name, text, file in EMBEDDED.get(package, []) if found else []:
                shutil.copy2(os.path.join(ROOT, text), os.path.join(dest, file))
                embedded.append(f'{name} ({file})')
            lines.append(f'  {lib}: {package} {version}, from Homebrew '
                         f'({", ".join(sorted(set(found))) or "no license file found"})'
                         + (f'; it includes {", ".join(embedded)}' if embedded else ''))
        else:
            lines.append(f'  {lib}: from {src} (neither a build_deps.sh prefix nor a Homebrew keg)')
        if not found:
            missing.append(lib)

    # What the game and those libraries are built with from the same prefixes
    # (the headers of a prefix that a bundled library came from), and the
    # FFmpeg the game links statically when it comes from one. A prefix only
    # FFmpeg came from, as when the libraries are Homebrew's, gives FFmpeg
    # alone: the game was built with Homebrew's headers then.
    library_prefixes = {prefix for prefix, _ in used}
    ffmpeg = cache_value('FFMPEG_XMA')
    ffmpeg_prefix = deps_prefix(os.path.join(ffmpeg, 'lib/libavcodec.a')) if ffmpeg else None
    if ffmpeg_prefix:
        record(ffmpeg_prefix)
    # (A project with libraries of its own that aren't in the app isn't in it at all.)
    others = [(prefix, project) for prefix in sorted(records) for project, j in records[prefix]['projects'].items()
              if (prefix, project) not in used
              and (prefix == ffmpeg_prefix if project == 'FFmpeg'
                   else not j['libraries'] and prefix in library_prefixes)]
    if others:
        lines += ['', 'Built into the game and the libraries above, with their license files here:']
        for prefix, project in others:
            files = copy_project(prefix, project)
            ffmpeg_here = project == 'FFmpeg'
            what = 'linked into the game, statically, with SpeedBreaker\'s patch' if ffmpeg_here else 'headers'
            lines.append(f'  {describe(prefix, project, built=ffmpeg_here)}: {what}; license files in {project}/: {", ".join(files)}')
    with open(os.path.join(licenses, 'README.txt'), 'w') as f:
        f.write('\n'.join(lines) + '\n')
    if missing:
        raise SystemExit('make_app.py: no license file found for ' + ', '.join(missing))


def make_dmg(app, version):
    """SpeedBreaker-macos.dmg: the app and a shortcut to Applications."""
    dmg = os.path.join(OUT, 'SpeedBreaker-macos.dmg')
    versioned = os.path.join(OUT, f'SpeedBreaker-{version}-macos.dmg')
    with tempfile.TemporaryDirectory() as tmp:
        stage = os.path.join(tmp, 'SpeedBreaker')
        os.makedirs(stage)
        run('ditto', app, os.path.join(stage, 'SpeedBreaker.app'))
        os.symlink('/Applications', os.path.join(stage, 'Applications'))
        for f in (dmg, versioned):
            if os.path.exists(f):
                os.remove(f)
        run('hdiutil', 'create', '-quiet', '-volname', 'SpeedBreaker', '-srcfolder', stage, '-fs', 'HFS+', '-format', 'UDZO',
            '-imagekey', 'zlib-level=9', '-ov', dmg)
    os.link(dmg, versioned)
    for f in (dmg, versioned):
        digest = run('shasum', '-a', '256', f).split()[0]
        with open(f + '.sha256', 'w') as out:
            out.write(f'{digest}  {os.path.basename(f)}\n')  # sha256sum -c / shasum -a 256 -c
    run('hdiutil', 'verify', '-quiet', dmg)
    return dmg


def main():
    if sys.platform != 'darwin':
        raise SystemExit('make_app.py: macOS only')
    if not os.access(BIN, os.X_OK):
        raise SystemExit(f'make_app.py: no {BIN}; build first')
    version, commit, dirty = build_identity()
    if dirty:
        print(f'make_app.py: warning: the build is {commit}-dirty (uncommitted changes): not for release', file=sys.stderr)
    minimum = cache_value('CMAKE_OSX_DEPLOYMENT_TARGET')
    if not re.fullmatch(r'\d+(\.\d+)*', minimum):
        raise SystemExit(f'make_app.py: no CMAKE_OSX_DEPLOYMENT_TARGET in {BUILD} (the top-level CMakeLists.txt sets one)')
    if MIN_MACOS and version_tuple(MIN_MACOS) != version_tuple(minimum):
        raise SystemExit(f'make_app.py: {BUILD} is built for macOS {minimum}, not {MIN_MACOS} (--min-macos); '
                         f'configure it with -DCMAKE_OSX_DEPLOYMENT_TARGET={MIN_MACOS}, or in a new build folder')
    default = project_default_target()
    if default and version_tuple(default) != version_tuple(minimum):
        print(f'make_app.py: warning: {BUILD} is configured for macOS {minimum}, not the project\'s {default} '
              f'(CMakeLists.txt): not for release', file=sys.stderr)
    mvk_version = moltenvk_version()
    app = os.path.join(OUT, 'SpeedBreaker.app')
    shutil.rmtree(app, ignore_errors=True)
    contents = os.path.join(app, 'Contents')
    macos, frameworks = os.path.join(contents, 'MacOS'), os.path.join(contents, 'Frameworks')
    resources = os.path.join(contents, 'Resources')
    icd = os.path.join(resources, 'vulkan/icd.d')
    for d in (macos, frameworks, icd):
        os.makedirs(d)
    exe = os.path.join(macos, 'SpeedBreaker')
    shutil.copy2(BIN, exe)
    # The full symbols for us, then the debug map out of the app's copy.
    symbols = os.path.join(OUT, 'symbols')
    os.makedirs(symbols, exist_ok=True)
    dsym = os.path.join(symbols, f'SpeedBreaker-{version}-{commit}{"-dirty" if dirty else ""}-macos.dSYM')
    shutil.rmtree(dsym, ignore_errors=True)
    subprocess.run(['dsymutil', BIN, '-o', dsym], check=True, capture_output=True)
    run('strip', '-S', exe)

    # The closure of non-system libraries, each copied once by the name it is
    # linked as (libSDL3.0.dylib), from the file behind it. The originals are
    # resolved against the build's own binary, so the copies can be rewritten.
    origin = {exe: BIN}
    copied = {}  # name in Frameworks -> original path
    queue = [exe]
    while queue:
        f = queue.pop()
        for ref in deps(f):
            name = os.path.basename(ref)
            if name in copied:
                continue
            src = os.path.realpath(resolve(ref, origin[f]))
            dst = os.path.join(frameworks, name)
            shutil.copy2(src, dst)
            os.chmod(dst, 0o755)
            copied[name] = src
            origin[dst] = src
            queue.append(dst)
    shutil.copy2(MOLTENVK, os.path.join(frameworks, 'libMoltenVK.dylib'))
    os.chmod(os.path.join(frameworks, 'libMoltenVK.dylib'), 0o755)
    origin[os.path.join(frameworks, 'libMoltenVK.dylib')] = MOLTENVK

    # Names: every reference to a copied library becomes @rpath/<name>; the
    # game finds them in ../Frameworks, the libraries next to themselves.
    # Homebrew's rpaths go, so nothing outside the app is ever loaded.
    for f in [exe] + [os.path.join(frameworks, n) for n in list(copied) + ['libMoltenVK.dylib']]:
        args = []
        if f != exe:
            args += ['-id', '@rpath/' + os.path.basename(f)]
        for ref in deps(f):
            if os.path.basename(ref) in copied:
                args += ['-change', ref, '@rpath/' + os.path.basename(ref)]
        for rp in rpaths(f):
            args += ['-delete_rpath', rp]
        args += ['-add_rpath', '@executable_path/../Frameworks' if f == exe else '@loader_path']
        run('install_name_tool', *args, f)

    with open(os.path.join(icd, 'MoltenVK_icd.json'), 'w') as j:
        j.write('{\n    "file_format_version": "1.0.0",\n    "ICD": {\n'
                '        "library_path": "../../../Frameworks/libMoltenVK.dylib",\n'
                '        "api_version": "1.4.0",\n        "is_portability_driver": true\n    }\n}\n')

    # The icon, every size macOS asks for.
    with tempfile.TemporaryDirectory() as tmp:
        iconset = os.path.join(tmp, 'AppIcon.iconset')
        os.makedirs(iconset)
        src = os.path.join(ROOT, 'scripts/macos/AppIcon.png')
        for size in (16, 32, 128, 256, 512):
            for scale in (1, 2):
                px = size * scale
                name = f'icon_{size}x{size}' + ('@2x' if scale == 2 else '') + '.png'
                run('sips', '-z', str(px), str(px), src, '--out', os.path.join(iconset, name))
        run('iconutil', '-c', 'icns', iconset, '-o', os.path.join(resources, 'AppIcon.icns'))

    # Every binary here runs on the macOS the plist promises, and so does the
    # FFmpeg linked into the game (an archive built for a newer macOS links
    # with only an ld warning).
    def show(v):
        return '.'.join(map(str, v[:2] if v[2] == 0 else v))
    floor = version_tuple(minimum)
    need = {f: minos(f) for f in [exe] + [os.path.join(frameworks, n) for n in os.listdir(frameworks)]}
    newer = [f'{os.path.basename(f)} ({show(v)}, from {origin[f]})' for f, v in sorted(need.items()) if v > floor]
    newest = max(need.values())
    ffmpeg = cache_value('FFMPEG_XMA')
    for a in ('lib/libavcodec.a', 'lib/libavutil.a') if ffmpeg else ():
        path = os.path.join(ffmpeg, a)
        found = minos_all(path) if os.path.isfile(path) else []
        if any(v > floor for v in found):
            newer.append(f'{os.path.basename(a)} ({show(max(found))}, from {path}, linked into the game)')
            newest = max(newest, max(found))
    if newer:
        brew = any('/Cellar/' in n or '/opt/homebrew/' in n for n in newer)
        raise SystemExit(
            f'make_app.py: these need a newer macOS than the build\'s {minimum}, so the app would not open on macOS '
            f'{minimum}:\n  ' + '\n  '.join(newer) + '\n'
            + ('Homebrew\'s bottles are built for the macOS they were made on. ' if brew else '')
            + f'Either build the libraries for {minimum} (how the release is built):\n'
            f'  scripts/macos/build_deps.sh <prefix>, then configure the build with -DCMAKE_PREFIX_PATH=<prefix> '
            f'-DFFMPEG_XMA=<prefix>/ffmpeg-xma and rebuild;\n'
            f'or make an app for this Mac\'s macOS and newer only: configure the build with '
            f'-DCMAKE_OSX_DEPLOYMENT_TARGET={show(newest)} and rebuild.')
    if need[exe] != floor:
        raise SystemExit(f'make_app.py: the game was built for macOS {show(need[exe])}, not {minimum}')

    copy_licenses(resources, {**copied, 'libMoltenVK.dylib': MOLTENVK})

    with open(os.path.join(contents, 'Info.plist'), 'wb') as p:
        plistlib.dump({
            'CFBundleDevelopmentRegion': 'en',
            'CFBundleExecutable': 'SpeedBreaker',
            'CFBundleIconFile': 'AppIcon',
            'CFBundleIdentifier': 'local.speedbreaker.SpeedBreaker',
            'CFBundleInfoDictionaryVersion': '6.0',
            'CFBundleName': 'SpeedBreaker',
            'CFBundleDisplayName': 'SpeedBreaker',
            'CFBundlePackageType': 'APPL',
            'CFBundleShortVersionString': version,
            'CFBundleVersion': build_number(commit),
            'SpeedBreakerCommit': commit + ('-dirty' if dirty else ''),
            'LSMinimumSystemVersion': minimum,
            # A game: macOS Game Mode (CPU/GPU priority, lower Bluetooth
            # latency for controllers) when it runs fullscreen.
            'LSApplicationCategoryType': 'public.app-category.racing-games',
            'GCSupportsControllerUserInteraction': True,
            'NSHighResolutionCapable': True,
            'NSHumanReadableCopyright': 'Copyright (C) 2026 project(u) and SpeedBreaker contributors. Free software '
                                        '(GPL-3.0-or-later). Need for Speed and Most Wanted are trademarks of Electronic Arts; '
                                        'SpeedBreaker is not affiliated with Electronic Arts or Valve. It needs your own '
                                        'copy of the Xbox 360 game.',
        }, p)

    # Ad hoc signatures, the libraries before the app that seals them.
    for n in sorted(os.listdir(frameworks)):
        run('codesign', '--force', '--sign', '-', os.path.join(frameworks, n))
    run('codesign', '--force', '--sign', '-', app)
    run('codesign', '--verify', '--deep', '--strict', app)

    # Nothing but the system's and the app's own: no library or search path
    # outside it (Homebrew's, or the prefix the libraries were built in).
    binaries = [exe] + [os.path.join(frameworks, n) for n in os.listdir(frameworks)]
    leftover = [f'{os.path.basename(f)}: {r}' for f in binaries for r in deps(f) if not r.startswith('@rpath/')]
    leftover += [f'{os.path.basename(f)}: rpath {r}' for f in binaries for r in rpaths(f) if not r.startswith('@')]
    if leftover:
        raise SystemExit('make_app.py: still linked outside the app: ' + '; '.join(leftover))
    size = int(run('du', '-sk', app).split()[0]) // 1024
    print(f'{app} ({size} MB, v{version}, {commit}{"-dirty" if dirty else ""}, macOS {minimum}+): '
          f'{len(copied)} libraries and MoltenVK {mvk_version} in Frameworks; symbols in {dsym}')
    if DMG:
        dmg = make_dmg(app, version)
        print(f'{dmg} ({os.path.getsize(dmg) // (1 << 20)} MB) and {os.path.basename(dmg)}.sha256')


if __name__ == '__main__':
    main()
