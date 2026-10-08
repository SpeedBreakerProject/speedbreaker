#!/usr/bin/env python3
# SpeedBreaker. GPL-3.0-or-later (see COPYING).
#
# make_app.py [build dir] [output dir] [--dmg]: SpeedBreaker.app for macOS
# (Apple silicon) from a build (default build/main), in build/app:
#   Contents/MacOS/SpeedBreaker      the game
#   Contents/Frameworks/             the libraries it links (Homebrew's SDL3,
#                                    Vulkan loader, glslang, SPIRV-Tools, and
#                                    what they link), named @rpath/<file>, and
#                                    MoltenVK, the Vulkan driver
#   Contents/Resources/vulkan/icd.d/ MoltenVK's manifest (the Vulkan loader
#                                    looks for drivers in the app's bundle)
#   Contents/Resources/AppIcon.icns  from scripts/macos/AppIcon.png
#   Contents/Resources/licenses/     COPYING, NOTICE, THIRD_PARTY_NOTICES.md,
#                                    LICENSES/, and each bundled library's
#                                    own license files (from its Homebrew keg)
#   Contents/Info.plist              a game (macOS Game Mode when fullscreen),
#                                    versioned from the build: CFBundleShort-
#                                    VersionString is project()'s version,
#                                    CFBundleVersion the commit's number in
#                                    the history, LSMinimumSystemVersion the
#                                    build's deployment target, which every
#                                    binary in the app must match (checked)
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
import os, plistlib, re, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ARGS = [a for a in sys.argv[1:] if not a.startswith('--')]
DMG = '--dmg' in sys.argv[1:]
BUILD = os.path.abspath(ARGS[0] if len(ARGS) > 0 else os.path.join(ROOT, 'build/main'))
OUT = os.path.abspath(ARGS[1] if len(ARGS) > 1 else os.path.join(ROOT, 'build/app'))
BIN = os.path.join(BUILD, 'runtime/SpeedBreaker')
MOLTENVK = os.environ.get('MOLTENVK', '/opt/homebrew/opt/molten-vk/lib/libMoltenVK.dylib')


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


def minos(path):
    """The oldest macOS a Mach-O file runs on (LC_BUILD_VERSION or the older
    LC_VERSION_MIN_MACOSX), as a tuple."""
    out = run('otool', '-l', path)
    m = re.search(r'cmd LC_BUILD_VERSION\n.*?\n\s+minos (\S+)', out, re.S) or \
        re.search(r'cmd LC_VERSION_MIN_MACOSX\n.*?\n\s+version (\S+)', out, re.S)
    return tuple(int(x) for x in m.group(1).split('.')) if m else (0,)


def keg(path):
    """The Homebrew keg a library file is in (/opt/homebrew/Cellar/<name>/<version>), or None."""
    m = re.match(r'(.*/Cellar/[^/]+/[^/]+)/', os.path.realpath(path))
    return m.group(1) if m else None


def copy_licenses(resources, libraries):
    """Contents/Resources/licenses: SpeedBreaker's notices and every bundled
    library's own license files, from its Homebrew keg."""
    licenses = os.path.join(resources, 'licenses')
    os.makedirs(licenses)
    for name in ('COPYING', 'NOTICE', 'THIRD_PARTY_NOTICES.md'):
        shutil.copy2(os.path.join(ROOT, name), licenses)
    shutil.copytree(os.path.join(ROOT, 'LICENSES'), os.path.join(licenses, 'LICENSES'))
    lines = ['SpeedBreaker is free software under the GPL-3.0-or-later (COPYING, NOTICE). What it is',
             'built from and with, and each license, is in THIRD_PARTY_NOTICES.md and LICENSES/.', '',
             'The libraries in Contents/Frameworks, with their own license files here:']
    missing = []
    for lib, src in sorted(libraries.items()):
        k = keg(src)
        found = []
        if k:
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
            lines.append(f'  {lib}: {package} {version} ({", ".join(sorted(set(found))) or "no license file found"})')
        else:
            lines.append(f'  {lib}: from {src} (not a Homebrew keg)')
        if not found:
            missing.append(lib)
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

    # Every binary here runs on the macOS the plist promises.
    def norm(v):
        v = tuple(v)
        return v + (0,) * (3 - len(v))
    need = {f: norm(minos(f)) for f in [exe] + [os.path.join(frameworks, n) for n in os.listdir(frameworks)]}
    floor = norm(int(x) for x in minimum.split('.'))
    newer = [f'{os.path.basename(f)} ({".".join(map(str, v))})' for f, v in need.items() if v > floor]
    if newer:
        raise SystemExit(f'make_app.py: these need a newer macOS than the build\'s {minimum}: ' + ', '.join(newer))
    if need[exe] != floor:
        raise SystemExit(f'make_app.py: the game was built for macOS {".".join(map(str, need[exe]))}, not {minimum}')

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

    leftover = [f'{os.path.basename(f)}: {r}' for f in [exe] + [os.path.join(frameworks, n) for n in os.listdir(frameworks)]
                for r in deps(f) if r.startswith('/opt/') or r.startswith('/usr/local/')]
    if leftover:
        raise SystemExit('make_app.py: still linked outside the app: ' + '; '.join(leftover))
    size = int(run('du', '-sk', app).split()[0]) // 1024
    print(f'{app} ({size} MB, v{version}, {commit}{"-dirty" if dirty else ""}, macOS {minimum}+): '
          f'{len(copied)} libraries and MoltenVK in Frameworks; symbols in {dsym}')
    if DMG:
        dmg = make_dmg(app, version)
        print(f'{dmg} ({os.path.getsize(dmg) // (1 << 20)} MB) and {os.path.basename(dmg)}.sha256')


if __name__ == '__main__':
    main()
