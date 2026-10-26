"""Write runtime/install/manifest.inc: the file list the installer checks a
user's disc against (relative path, size, SHA-256 of every file).

    python3 scripts/gen_install_manifest.py game/files
    python3 scripts/gen_install_manifest.py --tsv files.tsv --xex default.xex \
        --config config/nfsmw-pal-es.toml -o runtime/install/manifest_pal_es.inc

Run it over a known-good extracted dump (extract-xiso output), or over a list
of the disc's files (--tsv: path, size, SHA-256 per line, tab-separated, as a
lister reading the disc image writes it; --xex is that disc's default.xex, for
its title ID, and must have the listed hash). It writes only names, sizes and
hashes, never game content, so the output is committed. default.xex must match
the hash the region's code was recompiled from (the "default.xex SHA-256" line
of --config, config/nfsmw.toml by default); --expect-xex overrides that for
test manifests. The installer checks every file against the manifest as it
copies, so a list that doesn't match the disc can't install.

The manifest version is derived from the entries, so installs made with an
older manifest stop counting as valid (FindGameInstall) whenever it changes.
"""
import argparse
import hashlib
import os
import pathlib
import re
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / 'runtime/install/manifest.inc'
FORMAT = 1                        # bump if the meaning of the entries changes
MARKER = '.nfsmw-install'         # written by the installer; not a disc file
CHUNK = 8 << 20


def expected_xex_hash(config):
    text = config.read_text()
    m = re.search(r'default\.xex SHA-256 ([0-9a-f]{64})', text)
    if not m:
        sys.exit(f'{config} has no "default.xex SHA-256 <hash>" comment; pass --expect-xex')
    return m.group(1)


def xex_title_id(path):
    # XEX2 header: optional header count at 0x14, then (key, value) pairs.
    # Key 0x00040006 is the execution info, whose title ID is at +12.
    data = path.read_bytes()[:0x10000]
    if data[:4] != b'XEX2':
        sys.exit(f'{path} is not an XEX2 executable')
    count = struct.unpack_from('>I', data, 0x14)[0]
    for i in range(min(count, 64)):
        key, value = struct.unpack_from('>II', data, 0x18 + 8 * i)
        if key == 0x00040006 and value + 16 <= len(data):
            return struct.unpack_from('>I', data, value + 12)[0]
    sys.exit(f'{path} has no execution info header')


def sha256_of(path, done, total):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        while chunk := f.read(CHUNK):
            h.update(chunk)
            done[0] += len(chunk)
            if sys.stderr.isatty():
                print(f'\r{done[0] * 100 // max(total, 1):3d}%  {path.name[:48]:<48}', end='', file=sys.stderr, flush=True)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('game_dir', type=pathlib.Path, nargs='?', help='extracted disc (default.xex, NFS/, Movies/)')
    ap.add_argument('-o', '--out', type=pathlib.Path, default=OUT)
    ap.add_argument('--tsv', type=pathlib.Path, help="the disc's file list (path, size, SHA-256) instead of game_dir")
    ap.add_argument('--xex', type=pathlib.Path, help="with --tsv: the disc's default.xex (for its title ID)")
    ap.add_argument('--config', type=pathlib.Path, default=ROOT / 'config/nfsmw.toml',
                    help='the XenonRecomp config naming the default.xex SHA-256 (default: config/nfsmw.toml)')
    ap.add_argument('--expect-xex', help='required default.xex SHA-256 (default: from --config)')
    args = ap.parse_args()
    if bool(args.tsv) == bool(args.game_dir):
        ap.error('give an extracted disc folder or --tsv, not both')
    if args.tsv and not args.xex:
        ap.error('--tsv needs --xex')

    if args.tsv:
        entries = []
        for line in args.tsv.read_text().splitlines():
            if not line.strip():
                continue
            rel, size, h = line.split('\t')
            if not re.fullmatch(r'[\x21-\x7e ]+', rel) or '"' in rel or '\\' in rel or rel.startswith('.'):
                sys.exit(f'unexpected characters in {rel!r}')
            if not re.fullmatch(r'[0-9a-f]{64}', h):
                sys.exit(f'{rel}: {h!r} is not a SHA-256')
            entries.append((rel, int(size), h))
        entries.sort()
        xex_file = args.xex
        if hashlib.sha256(xex_file.read_bytes()).hexdigest() != dict((r, x) for r, _, x in entries).get('default.xex'):
            sys.exit(f'{xex_file} is not the default.xex the list names')
        return write(args, entries, xex_file)

    game = args.game_dir.resolve()
    files = []
    for dirpath, dirnames, filenames in os.walk(game):
        # Hidden entries are host clutter (.DS_Store, AppleDouble ._ files on
        # exFAT drives, the install marker), never disc files; the installer
        # skips them the same way.
        dirnames[:] = sorted(d for d in dirnames if not d.startswith('.'))
        for name in filenames:
            if name.startswith('.'):
                continue
            path = pathlib.Path(dirpath) / name
            rel = path.relative_to(game).as_posix()
            if not re.fullmatch(r'[\x21-\x7e ]+', rel) or '"' in rel or '\\' in rel:
                sys.exit(f'unexpected characters in {rel!r}')
            files.append((rel, path))
    files.sort()
    by_lower = {}
    for rel, _ in files:
        if rel.lower() in by_lower:
            sys.exit(f'{rel} and {by_lower[rel.lower()]} differ only in case')
        by_lower[rel.lower()] = rel
    if 'default.xex' not in [rel for rel, _ in files]:
        sys.exit(f'{game} has no default.xex at its root')

    total = sum(p.stat().st_size for _, p in files)
    done = [0]
    entries = [(rel, p.stat().st_size, sha256_of(p, done, total)) for rel, p in files]
    if sys.stderr.isatty():
        print(file=sys.stderr)
    write(args, entries, game / 'default.xex')


def write(args, entries, xex_file):
    by_lower = {}
    for rel, _, _ in entries:
        if rel.lower() in by_lower:
            sys.exit(f'{rel} and {by_lower[rel.lower()]} differ only in case')
        by_lower[rel.lower()] = rel
    if 'default.xex' not in by_lower:
        sys.exit('the disc has no default.xex at its root')
    total = sum(size for _, size, _ in entries)
    xex_hash = next(h for rel, _, h in entries if rel == 'default.xex')
    expected = args.expect_xex or expected_xex_hash(args.config)
    if xex_hash != expected.lower():
        sys.exit(f'default.xex is {xex_hash}, expected {expected}: not the dump this build was recompiled from')

    canonical = f'nfsmw-install-manifest {FORMAT}\n' + ''.join(f'{rel}\t{size}\t{h}\n' for rel, size, h in entries)
    version = hashlib.sha256(canonical.encode()).hexdigest()[:16]
    title_id = xex_title_id(xex_file)

    lines = [
        '// Generated by scripts/gen_install_manifest.py from a known-good dump. Do not edit;',
        '// rerun the script. File names, sizes and SHA-256 hashes only: no game content.',
        '// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).',
        '//',
        '// MANIFEST_INFO(version, default.xex title ID, file count, total bytes)',
        '// MANIFEST_FILE(path relative to the disc root, size in bytes, SHA-256)',
        '',
        f'MANIFEST_INFO("{version}", 0x{title_id:08X}, {len(entries)}, {total}ull)',
    ]
    lines += [f'MANIFEST_FILE("{rel}", {size}ull, "{h}")' for rel, size, h in entries]
    args.out.write_text('\n'.join(lines) + '\n')
    shown = args.out.resolve()
    shown = shown.relative_to(ROOT) if shown.is_relative_to(ROOT) else shown
    print(f'{shown}: {len(entries)} files, {total:,} bytes, manifest {version}, title {title_id:08X}')


if __name__ == '__main__':
    main()
