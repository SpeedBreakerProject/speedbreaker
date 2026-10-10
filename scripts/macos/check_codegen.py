#!/usr/bin/env python3
# SpeedBreaker. GPL-3.0-or-later (see COPYING).
#
# check_codegen.py <build folder> [--reference-target=26.0] [--against=<build folder>]
#
# The release's check that building for the oldest macOS the game runs on (the build folder's
# CMAKE_OSX_DEPLOYMENT_TARGET, CMakeLists.txt's default 15.0) costs nobody any speed: that its game code is
# the code the same sources give for macOS 26.0 (v0.1.0's target, what Macs on macOS 26 ran), function by
# function, but for differences read and found cold (ALLOWED below). The deployment target doesn't choose
# the CPU code (that's -mcpu, the compiler's default for arm64 Macs, apple-m1), but it does choose which
# libc++ features the system's libc++.dylib is assumed to have, and which inline code Apple's headers give:
# targeting 15.0, libc++ compiles in its own copy of what macOS 15.0's libc++.dylib lacks (std::
# bad_function_call's destructor, which the dylib has from macOS 15.4, for one). Any of that can land in
# new code. So this is run on the release candidate's own commit, never on an earlier build's results: an
# A/B speed run measured one build, and this proves the candidate's code is that build's code at 26.0.
#
# The build folder is the release's (configured and built with the README's cmake line). Unless --against
# names a build folder of the same sources for the reference macOS, this configures <the build folder's
# parent>/codegen-<reference> with the same compilers, libraries and options but the reference target, and
# builds the SpeedBreaker target in both (ninja) once steps 1 and 2 and
# the controls have passed:
#   1. Every compile and link command that builds SpeedBreaker is the same in both but for
#      -mmacosx-version-min (and the build folders' own paths).
#   2. The compiler gets the same CPU, target features and optimisation for each: for each distinct set of
#      flags, the cc1 command clang -### makes from it for each target. Only the target triple's OS version
#      differs, and the defaults that follow it and change no code (the Objective-C runtime's version, two
#      debug-information options, with no -g).
#   3. Every object: its code (every section of instructions: __text, __StaticInit, ...) function by
#      function, the bytes compared exactly but for the fields a relocation fills in and the nop padding to
#      the next function's alignment, and each relocation's kind and target compared too (a symbol by name;
#      an assembler-local label, which is numbered through the file, by the constant it labels; a C string
#      only as a string). A function that differs, or is only in one build, must match ALLOWED, or the check
#      fails, naming it. Data sections that differ are listed (the build date's string is one).
#   4. For the record, the linked game: its code's size, how far the functions moved, and how many changed
#      their place in a 128-byte cache line (Apple silicon's line), and whether the data's layout is the
#      same (the writable __DATA, where hot arrays sit, and __DATA_CONST).
# First, the controls (in a temporary folder, with the build's own C++ compiler and flags): a file calling an
# empty std::function, built for both targets, must show libc++'s own copy of std::bad_function_call as the
# (allowed) difference; and a function built with a different constant, once in an instruction and once in a
# constant pool (the same instructions), must be caught as a difference that is not allowed. If any fails,
# the check itself is broken.
# Exit status 0 only when every step passes. Needs Apple's command line tools, cmake and ninja.
import collections, hashlib, os, re, shlex, struct, subprocess, sys, tempfile

ARGS = [a for a in sys.argv[1:] if not a.startswith('--')]
OPTS = dict(a[2:].split('=', 1) if '=' in a else (a[2:], '') for a in sys.argv[1:] if a.startswith('--'))

# Code that differs between the targets, each read and found to run only on a cold path:
# {regex matching the whole mangled name: why}. Add one only after reading the difference (llvm-objdump -d -r
# of the object in both builds); every match is listed in the output.
ALLOWED = {
    r'__ZNSt3__117bad_function_callD[012]Ev':
        "std::bad_function_call's destructor: libc++ assumes macOS 15.4's libc++.dylib has it "
        "(_LIBCPP_AVAILABILITY_HAS_BAD_FUNCTION_CALL_KEY_FUNCTION), so a 15.0 build carries its own copy. "
        "Runs only after an empty std::function was called, which throws (a crash path).",
    r'__ZNSt3__125__throw_bad_function_call\w*':
        "throws std::bad_function_call when an empty std::function is called (a crash path); at 15.0 it "
        "takes the class's type information and destructor from the build's own copy (adrp/add) instead of "
        "libc++.dylib's (through the GOT): the same instructions otherwise.",
}

# arm64 relocation kinds (mach-o/arm64/reloc.h) and the instruction bits each one fills in.
RELOC_NAMES = {0: 'UNSIGNED', 1: 'SUBTRACTOR', 2: 'BRANCH26', 3: 'PAGE21', 4: 'PAGEOFF12', 5: 'GOT_LOAD_PAGE21',
               6: 'GOT_LOAD_PAGEOFF12', 7: 'POINTER_TO_GOT', 8: 'TLVP_LOAD_PAGE21', 9: 'TLVP_LOAD_PAGEOFF12',
               10: 'ADDEND', 11: 'AUTHENTICATED_POINTER'}
INSN_FIELD = {2: 0x03FFFFFF, 3: 0x60FFFFE0, 5: 0x60FFFFE0, 8: 0x60FFFFE0, 4: 0x003FFC00, 6: 0x003FFC00, 9: 0x003FFC00}
CODE_FLAGS = 0x80000000 | 0x00000400  # S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS
ZEROFILL = {0x1, 0xc, 0x12}           # S_ZEROFILL, S_GB_ZEROFILL, S_THREAD_LOCAL_ZEROFILL
NOP = struct.pack('<I', 0xd503201f)  # arm64 nop
LINE = 128                            # bytes in a cache line on Apple silicon (hw.cachelinesize)


def run(*args, cwd=None, check=True):
    r = subprocess.run(args, cwd=cwd, capture_output=True, text=True)
    if check and r.returncode != 0:
        raise SystemExit(f'check_codegen.py: {" ".join(args[:4])} ... failed ({r.returncode}):\n'
                         f'{(r.stdout + r.stderr)[-3000:]}')
    return r.stdout


def cache(build):
    """{name: value} of a build folder's CMakeCache.txt."""
    values = {}
    with open(os.path.join(build, 'CMakeCache.txt')) as f:
        for line in f:
            m = re.match(r'^([A-Za-z0-9_.+-]+):[A-Z]+=(.*)$', line.rstrip('\n'))
            if m:
                values[m.group(1)] = m.group(2)
    return values


# --- Mach-O objects -------------------------------------------------------------------------------------
class MachO:
    """A thin 64-bit Mach-O: its sections, symbols and each section's relocations."""

    def __init__(self, path):
        with open(path, 'rb') as f:
            self.data = d = f.read()
        if len(d) < 32 or struct.unpack_from('<I', d, 0)[0] != 0xfeedfacf:
            raise ValueError(f'{path}: not a thin 64-bit Mach-O')
        self.filetype, ncmds = struct.unpack_from('<I', d, 12)[0], struct.unpack_from('<I', d, 16)[0]
        self.sections, self.symbols, off = [], [], 32
        for _ in range(ncmds):
            cmd, size = struct.unpack_from('<II', d, off)
            if cmd == 0x19:  # LC_SEGMENT_64
                for i in range(struct.unpack_from('<I', d, off + 64)[0]):
                    s = off + 72 + 80 * i
                    sect, seg = d[s:s + 16].rstrip(b'\0').decode(), d[s + 16:s + 32].rstrip(b'\0').decode()
                    addr, size_, offset, _, reloff, nreloc, flags = struct.unpack_from('<QQIIIII', d, s + 32)
                    zerofill = (flags & 0xff) in ZEROFILL
                    self.sections.append({'name': f'{seg},{sect}', 'addr': addr, 'size': size_, 'flags': flags,
                                          'code': bool(flags & CODE_FLAGS),
                                          'bytes': b'' if zerofill else d[offset:offset + size_],
                                          'relocs': self._relocs(reloff, nreloc)})
            elif cmd == 0x2:  # LC_SYMTAB
                symoff, nsyms, stroff, _ = struct.unpack_from('<IIII', d, off + 8)
                for i in range(nsyms):
                    strx, ntype, nsect, _, value = struct.unpack_from('<IBBHQ', d, symoff + 16 * i)
                    end = d.index(b'\0', stroff + strx)
                    self.symbols.append((d[stroff + strx:end].decode(errors='replace'), ntype, nsect, value))
            off += size

    def _relocs(self, reloff, nreloc):
        out = []
        for i in range(nreloc):
            address, info = struct.unpack_from('<iI', self.data, reloff + 8 * i)
            out.append((address, info & 0xffffff, (info >> 24) & 1, (info >> 25) & 3, (info >> 27) & 1, info >> 28))
        return out

    def symbol_name(self, index):
        return self.symbols[index][0] if 0 <= index < len(self.symbols) else f'<symbol {index}>'


def functions(obj, sec_index):
    """[(name(s), start, end)] of a code section (sec_index counts from 1, as n_sect does), split at its
    symbols; assembler temporaries (l..., L...) don't start one."""
    sec = obj.sections[sec_index - 1]
    starts = collections.defaultdict(list)
    for name, ntype, nsect, value in obj.symbols:
        if ntype & 0xe0 or (ntype & 0x0e) != 0x0e or nsect != sec_index or name.startswith(('l', 'L')):
            continue
        starts[value - sec['addr']].append(name)
    offsets = sorted(starts)
    if not offsets or offsets[0] != 0:
        offsets.insert(0, 0)
        starts[0] = starts.get(0) or [f'<start of {sec["name"]}>']
    out = []
    for i, start in enumerate(offsets):
        end = offsets[i + 1] if i + 1 < len(offsets) else sec['size']
        if end > start or sec['size'] == 0:
            out.append((' = '.join(sorted(starts[start])), start, end))
    return out


def resolved_relocs(obj, sec):
    """A section's relocations with their targets named: [(offset, kind, pc-relative, length, target, addend)].
    The target is the symbol's name for an external relocation, else the section's (whose address sits in
    the relocated field); an ARM64_RELOC_ADDEND entry becomes the next relocation's addend."""
    out, addend = [], 0
    for address, symnum, pcrel, length, extern, rtype in sec['relocs']:
        if rtype == 10:  # ARM64_RELOC_ADDEND: 24 bits, signed
            addend = symnum - (1 << 24) if symnum & 0x800000 else symnum
            continue
        if address & 0x80000000:  # a scattered relocation (none on arm64): kept whole
            target = f'scattered {address:#x}'
        elif extern:
            target = obj.symbol_name(symnum)
            if target.startswith(('l', 'L')):  # an assembler-local label: compared by what it labels (Local)
                target = local_content(obj, symnum)
        else:
            target = f'section {obj.sections[symnum - 1]["name"]}' if 0 < symnum <= len(obj.sections) else f'section {symnum}'
        out.append((address, RELOC_NAMES.get(rtype, rtype), pcrel, length, target, addend))
        addend = 0
    return out


class Local:
    """What an assembler-local label (lCPI3_0, l_.str.12, l_switch.table..., LJTI5_0) labels, as a relocation's
    target. The labels are numbered through the file (a constant pool's by its function's place in it), so one
    function more anywhere renumbers the rest without changing them: they are compared by their bytes instead,
    to the next label, but for the zero padding before it (which follows the next one's alignment). A C string
    is compared by its section only: a different string changes no code (the build's date is one), and the
    strings that differ are listed with the data."""

    def __init__(self, section, data, cstring):
        self.section, self.data, self.cstring = section, data, cstring

    def __eq__(self, other):
        if not isinstance(other, Local) or self.section != other.section or self.cstring != other.cstring:
            return False
        if self.cstring:
            return True
        short, long_ = sorted((self.data, other.data), key=len)
        return long_.startswith(short) and not long_[len(short):].strip(b'\0')

    def __hash__(self):
        return hash(self.section)

    def __repr__(self):
        return f'local label in {self.section} ({len(self.data)} bytes, {hashlib.sha256(self.data).hexdigest()[:12]})'


def local_content(obj, symnum):
    """The Local an assembler-local label's relocation targets."""
    name, _, nsect, value = obj.symbols[symnum]
    if not 0 < nsect <= len(obj.sections):
        return name
    if not hasattr(obj, 'starts'):
        obj.starts = collections.defaultdict(list)
        for _, ntype, n, v in obj.symbols:
            if not ntype & 0xe0 and (ntype & 0x0e) == 0x0e:
                obj.starts[n].append(v)
        for v in obj.starts.values():
            v.sort()
    sec = obj.sections[nsect - 1]
    later = [v for v in obj.starts[nsect] if v > value]
    end = (later[0] if later else sec['addr'] + sec['size']) - sec['addr']
    data = sec['bytes'][value - sec['addr']:end]
    cstring = (sec['flags'] & 0xff) == 0x2  # S_CSTRING_LITERALS
    return Local(sec['name'], data.split(b'\0', 1)[0] if cstring else data, cstring)


def section_symbols(obj, sec_index):
    sec = obj.sections[sec_index - 1]
    return sorted((value - sec['addr'], name) for name, ntype, nsect, value in obj.symbols
                  if not ntype & 0xe0 and (ntype & 0x0e) == 0x0e and nsect == sec_index)


def masked_code(obj, sec_index):
    """{function name: (its bytes with every relocated field zeroed, its relocations, offsets from the
    function's start)} of a code section."""
    sec = obj.sections[sec_index - 1]
    code = bytearray(sec['bytes'])
    relocs = resolved_relocs(obj, sec)
    for address, kind, _, length, target, _ in relocs:
        rtype = next((k for k, v in RELOC_NAMES.items() if v == kind), None)
        if isinstance(target, str) and target.startswith('scattered'):
            continue
        if rtype in INSN_FIELD and address + 4 <= len(code):
            word = struct.unpack_from('<I', code, address)[0] & ~INSN_FIELD[rtype]
            struct.pack_into('<I', code, address, word)
        elif address + (1 << length) <= len(code):
            code[address:address + (1 << length)] = bytes(1 << length)
    out, seen = {}, collections.Counter()
    for name, start, end in functions(obj, sec_index):
        seen[name] += 1
        key = name if seen[name] == 1 else f'{name} #{seen[name]}'
        own = tuple((r[0] - start,) + r[1:] for r in relocs if start <= r[0] < end)
        body = bytes(code[start:end])
        while len(body) >= 4 and body[-4:] == NOP and not any(r[0] >= len(body) - 4 for r in own):
            body = body[:-4]  # the padding to the next function's alignment, which moves with it
        out[key] = (body, own)
    return out


def compare_objects(path_a, path_b):
    """Two objects' code. -> (status, {function: how it differs}, [data sections that differ]); status is
    'identical' (every code section the same bytes, relocations and symbols), 'same' (the same functions
    once the relocated fields are masked) or 'differs'."""
    a, b = MachO(path_a), MachO(path_b)
    code_a = {s['name']: i + 1 for i, s in enumerate(a.sections) if s['code']}
    code_b = {s['name']: i + 1 for i, s in enumerate(b.sections) if s['code']}
    differ, identical = {}, set(code_a) == set(code_b)
    for name in sorted(set(code_a) | set(code_b)):
        sa = a.sections[code_a[name] - 1] if name in code_a else None
        sb = b.sections[code_b[name] - 1] if name in code_b else None
        if sa and sb and sa['bytes'] == sb['bytes'] and resolved_relocs(a, sa) == resolved_relocs(b, sb) \
                and section_symbols(a, code_a[name]) == section_symbols(b, code_b[name]):
            continue  # the same code, byte for byte, with the same relocations and symbols
        identical = False
        fa = masked_code(a, code_a[name]) if sa else {}
        fb = masked_code(b, code_b[name]) if sb else {}
        for f in sorted(set(fa) | set(fb)):
            if f not in fb:
                differ[f] = f'only in the first build ({len(fa[f][0])} bytes, {name})'
            elif f not in fa:
                differ[f] = f'only in the second build ({len(fb[f][0])} bytes, {name})'
            elif fa[f] != fb[f]:
                what = 'instructions' if fa[f][0] != fb[f][0] else 'relocations'
                differ[f] = f'{what} differ ({len(fa[f][0])} vs {len(fb[f][0])} bytes, {name})'
    data_a = {s['name']: s for s in a.sections if not s['code']}
    data_b = {s['name']: s for s in b.sections if not s['code']}
    data = [n for n in sorted(set(data_a) | set(data_b))
            if n not in data_a or n not in data_b or data_a[n]['bytes'] != data_b[n]['bytes']
            or data_a[n]['size'] != data_b[n]['size']
            or resolved_relocs(a, data_a[n]) != resolved_relocs(b, data_b[n])]
    return ('identical' if identical else 'same' if not differ else 'differs'), differ, data


def allowed(function):
    """The ALLOWED pattern a function (or one of its names) matches, or None."""
    for name in function.split(' #')[0].split(' = '):
        for pattern in ALLOWED:
            if re.fullmatch(pattern, name):
                return pattern
    return None


# --- the build's commands -------------------------------------------------------------------------------
def commands(build):
    """({object path relative to the build: its compile command}, the link command) of the SpeedBreaker
    target, from ninja."""
    compiles, link = {}, None
    for line in run('ninja', '-C', build, '-t', 'commands', 'SpeedBreaker').splitlines():
        words = shlex.split(line)
        if '-c' in words and '-o' in words:
            compiles[words[words.index('-o') + 1]] = words
        elif '-o' in words and words[words.index('-o') + 1].endswith('SpeedBreaker'):
            link = words
    return compiles, link


def normalized(words, build, other_build):
    """A command with the build folders' paths and the deployment target replaced by placeholders."""
    paths = sorted({build, os.path.realpath(build), other_build, os.path.realpath(other_build)}, key=len, reverse=True)
    out = []
    for w in words:
        for path in paths:
            w = w.replace(path, '<build>')
        out.append(re.sub(r'^(-mmacosx-version-min=|-mmacos-version-min=)[0-9.]+$', r'\1<target>', w))
    return out


def cc1(compiler, language, flags, target):
    """The cc1 arguments clang makes from `flags` for `target`."""
    flags = [re.sub(r'^(-mmacosx-version-min=|-mmacos-version-min=)[0-9.]+$', rf'\g<1>{target}', f) for f in flags]
    out = subprocess.run([compiler, '-###', '-x', language, '-c', '/dev/null', '-o', '/dev/null'] + flags,
                         capture_output=True, text=True).stderr
    for line in out.splitlines():
        if '"-cc1"' in line:
            return shlex.split(line)
    raise SystemExit(f'check_codegen.py: no cc1 command from {compiler} -### for {language}:\n{out[-2000:]}')


def flag_set(words):
    """(compiler, language, the flags that reach code generation) of a compile command: without the
    source, the output, the dependency file, include paths, macros and forced includes (which pick the
    source, not how it is compiled; step 1 compares them)."""
    source = words[words.index('-c') + 1]
    lang = {'.mm': 'objective-c++', '.m': 'objective-c', '.c': 'c'}.get(os.path.splitext(source)[1], 'c++')
    flags, i = [], 1
    while i < len(words):
        w = words[i]
        if w in ('-o', '-c', '-MF', '-MT', '-MQ', '-include', '-isystem', '-iquote', '-idirafter', '-I', '-D', '-U'):
            i += 2
            continue
        if w == '-x':
            lang = words[i + 1]
            i += 2
            continue
        if w not in ('-MD', '-MMD') and not w.startswith(('-I', '-D', '-U')):
            flags.append(w)
        i += 1
    return words[0], lang, tuple(flags)


# Where cc1's arguments may differ between the two targets, and why that changes no code.
CC1_EXPECTED = [
    (r'-triple', 'the target triple names the OS version (the CPU part, arm64-apple-macosx, is the same)'),
    (r'-fobjc-runtime=macosx-[0-9.]+', 'the Objective-C runtime version follows the target (Objective-C++ only)'),
    (r'-gsimple-template-names=simple', 'debug information only (the build has no -g)'),
    (r'-debug-forward-template-params', 'debug information only (the build has no -g)'),
    (r'-target-sdk-version=[0-9.]+', 'the SDK, the same in both'),
]


def cc1_differences(args_a, args_b):
    """(differences that may change code, the expected ones) between two cc1 commands."""
    def keyed(args):
        out, i = [], 0
        while i < len(args):
            if args[i] in ('-triple', '-target-cpu', '-target-feature', '-target-abi', '-mllvm', '-tune-cpu') \
                    and i + 1 < len(args):
                out.append(f'{args[i]} {args[i + 1]}')
                i += 2
            else:
                out.append(args[i])
                i += 1
        return out
    ka, kb = keyed(args_a[1:]), keyed(args_b[1:])
    only_a = [x for x in ka if x not in kb and not x.startswith(('/dev/null', '-main-file-name'))]
    only_b = [x for x in kb if x not in ka and not x.startswith(('/dev/null', '-main-file-name'))]
    bad, expected = [], []
    for side, items in (('first', only_a), ('second', only_b)):
        for x in items:
            why = next((w for p, w in CC1_EXPECTED if re.match(p, x)), None)
            if x.startswith('-triple '):
                why = why if re.sub(r'[0-9.]+$', '', x.split()[1]) == 'arm64-apple-macosx' else None
            (expected if why else bad).append(f'{side} build only: {x}' + (f' ({why})' if why else ''))
    return bad, expected


# --- the controls -----------------------------------------------------------------------------------------
CONTROL_FUNCTION = '#include <functional>\nstd::function<void()> sb_control_target;\n' \
                   'void sb_control_call() { sb_control_target(); }\n'
CONTROL_CONSTANT = 'extern "C" int sb_control_constant(int x) { return x + SB_CONTROL_VALUE; }\n'
CONTROL_POOL = 'typedef float sb_v4 __attribute__((ext_vector_type(4)));\n' \
               'extern "C" sb_v4 sb_control_pool(sb_v4 x) { return x * (sb_v4){1.5f, 2.5f, 3.5f, SB_CONTROL_VALUE}; }\n'


def controls(compiler, flags, target, reference):
    """The two controls, with the build's C++ compiler and flags. -> (ok, lines)."""
    lines, ok = [], True
    with tempfile.TemporaryDirectory() as tmp:
        def build(side, name, src, version, extra=()):
            # the same file name on both sides: a static initializer is named after its file
            os.makedirs(os.path.join(tmp, side), exist_ok=True)
            path = os.path.join(tmp, side, name)
            with open(path + '.cpp', 'w') as f:
                f.write(src)
            fl = [re.sub(r'^-mmacosx-version-min=[0-9.]+$', f'-mmacosx-version-min={version}', x) for x in flags]
            run(compiler, *fl, *extra, '-c', path + '.cpp', '-o', path + '.o')
            return path + '.o'
        _, differ, _ = compare_objects(build('a', 'function', CONTROL_FUNCTION, target),
                                       build('b', 'function', CONTROL_FUNCTION, reference))
        good = bool(differ) and all(allowed(f) for f in differ)
        lines.append(f'  control 1, an empty std::function called, {target} vs {reference}: '
                     f'{len(differ)} functions differ, {"all allowed: ok" if good else "FAIL"} '
                     f'({", ".join(sorted(differ)) or "none: the comparison sees no target-dependent code"})')
        ok &= good
        _, differ, _ = compare_objects(build('a', 'constant', CONTROL_CONSTANT, target, ['-DSB_CONTROL_VALUE=1']),
                                       build('b', 'constant', CONTROL_CONSTANT, target, ['-DSB_CONTROL_VALUE=2']))
        good = list(differ) == ['_sb_control_constant'] and not allowed('_sb_control_constant')
        lines.append(f'  control 2, a function built with x + 1 and with x + 2: '
                     f'{"caught, not allowed: ok" if good else "FAIL (found " + repr(differ) + ")"}')
        ok &= good
        _, differ, _ = compare_objects(build('a', 'pool', CONTROL_POOL, target, ['-DSB_CONTROL_VALUE=4.5f']),
                                       build('b', 'pool', CONTROL_POOL, target, ['-DSB_CONTROL_VALUE=5.5f']))
        good = list(differ) == ['_sb_control_pool'] and not allowed('_sb_control_pool')
        lines.append(f'  control 3, a function whose constant (in a constant pool, the same instructions) is 4.5 and '
                     f'5.5: {"caught, not allowed: ok" if good else "FAIL (found " + repr(differ) + ")"}')
        ok &= good
    return ok, lines


# --- the linked game ------------------------------------------------------------------------------------
def linked_report(exe_a, exe_b):
    lines = []
    a, b = MachO(exe_a), MachO(exe_b)
    ta = next(s for s in a.sections if s['name'] == '__TEXT,__text')
    tb = next(s for s in b.sections if s['name'] == '__TEXT,__text')
    lines.append(f'  __TEXT,__text: {ta["size"]:,} bytes at {ta["addr"]:#x} vs {tb["size"]:,} at {tb["addr"]:#x} '
                 f'({tb["size"] - ta["size"]:+,} bytes)')

    def addresses(m):
        out, seen = {}, collections.Counter()
        text = next(i + 1 for i, s in enumerate(m.sections) if s['name'] == '__TEXT,__text')
        for name, ntype, nsect, value in m.symbols:
            if not ntype & 0xe0 and (ntype & 0x0e) == 0x0e and nsect == text:
                seen[name] += 1
                out[name if seen[name] == 1 else f'{name} #{seen[name]}'] = value
        return out
    fa, fb = addresses(a), addresses(b)
    both = [n for n in fa if n in fb]
    shifts = collections.Counter(fb[n] - fa[n] for n in both)
    moved_line = sum(1 for n in both if (fa[n] - ta['addr']) % LINE != (fb[n] - tb['addr']) % LINE)
    lines.append(f'  functions: {len(fa):,} vs {len(fb):,} ({len(both):,} in both); moved by '
                 + ', '.join(f'{s:+} bytes: {c:,}' for s, c in shifts.most_common(6))
                 + (f', {len(shifts) - 6} other distances' if len(shifts) > 6 else ''))
    lines.append(f'  place in a {LINE}-byte cache line changed: {moved_line:,} of {len(both):,} functions')
    for seg, what in (('__DATA,', 'writable data (__DATA)'), ('__DATA_CONST,', 'data read-only once loaded (__DATA_CONST)')):
        lines += layout(a, b, seg, what)
    return lines


def layout(a, b, seg, what):
    lines = []
    wa = [(s['name'], s['addr'], s['size']) for s in a.sections if s['name'].startswith(seg)]
    wb = [(s['name'], s['addr'], s['size']) for s in b.sections if s['name'].startswith(seg)]
    if wa == wb:
        lines.append(f'  {what}, {len(wa)} sections: the same addresses and sizes')
    else:
        lines.append(f'  {what}: the layout differs (section, address, size):')
        for n in sorted({x[0] for x in wa} | {y[0] for y in wb}):
            x, y = next((v for v in wa if v[0] == n), None), next((v for v in wb if v[0] == n), None)
            if x != y:
                show = lambda v: f'{v[1]:#x} {v[2]:,}' if v else '-'
                lines.append(f'    {n}: {show(x)} vs {show(y)}')
    return lines


# --- main -----------------------------------------------------------------------------------------------
def configure_reference(build, values, reference, out):
    """Configures `out` like `build` but for macOS `reference`."""
    args = ['cmake', '-S', values['CMAKE_HOME_DIRECTORY'], '-B', out, '-G', values.get('CMAKE_GENERATOR', 'Ninja'),
            f'-DCMAKE_OSX_DEPLOYMENT_TARGET={reference}', '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON']
    for k in ('CMAKE_BUILD_TYPE', 'CMAKE_C_COMPILER', 'CMAKE_CXX_COMPILER', 'CMAKE_OBJCXX_COMPILER', 'CMAKE_PREFIX_PATH',
              'FFMPEG_XMA', 'CMAKE_C_FLAGS', 'CMAKE_CXX_FLAGS', 'CMAKE_OBJCXX_FLAGS', 'CMAKE_EXE_LINKER_FLAGS'):
        if values.get(k):
            args.append(f'-D{k}={values[k]}')
    for k, v in values.items():
        if k.startswith('SB_') and k not in ('SB_TESTS', 'SB_OSX_TARGET_FROM') and not k.endswith('-ADVANCED'):
            args.append(f'-D{k}={v}')
    run(*args)


def main():
    if len(ARGS) != 1:
        raise SystemExit('usage: check_codegen.py <build folder> [--reference-target=26.0] [--against=<build folder>]')
    build = os.path.abspath(ARGS[0])
    values = cache(build)
    target = values.get('CMAKE_OSX_DEPLOYMENT_TARGET', '')
    reference = OPTS.get('reference-target') or '26.0'
    if not target or target == reference:
        raise SystemExit(f'check_codegen.py: {build} is built for macOS {target or "(no target)"}: nothing to '
                         f'compare with {reference}')
    other = os.path.abspath(OPTS.get('against') or os.path.join(os.path.dirname(build), f'codegen-{reference}'))
    if not OPTS.get('against') and not os.path.exists(os.path.join(other, 'CMakeCache.txt')):
        print(f'configuring {other} for macOS {reference} (like {build})')
        configure_reference(build, values, reference, other)
    got = cache(other).get('CMAKE_OSX_DEPLOYMENT_TARGET')
    if got != reference:
        raise SystemExit(f'check_codegen.py: {other} is built for macOS {got}, not {reference}')
    ok = True
    print(f'{build}: macOS {target}\n{other}: macOS {reference}')

    # 1. the commands
    ca, la = commands(build)
    cb, lb = commands(other)
    print(f'1. compile and link commands of SpeedBreaker ({len(ca)} compiles):')
    bad = sorted(set(ca) ^ set(cb))
    for o in bad:
        print(f'   FAIL: {o} is built only in {"the first" if o in ca else "the second"} build')
    diff = [o for o in sorted(set(ca) & set(cb)) if normalized(ca[o], build, other) != normalized(cb[o], other, build)]
    for o in diff[:20]:
        x, y = normalized(ca[o], build, other), normalized(cb[o], other, build)
        print(f'   FAIL: {o}: {" ".join(w for w in x if w not in y)} | {" ".join(w for w in y if w not in x)}')
    if len(diff) > 20:
        print(f'   ... {len(diff) - 20} more')
    link_same = la is not None and lb is not None and normalized(la, build, other) == normalized(lb, other, build)
    if not link_same:
        print('   FAIL: the link commands differ (beyond the target): '
              f'{[w for w in normalized(la or [], build, other) if w not in normalized(lb or [], other, build)]} | '
              f'{[w for w in normalized(lb or [], other, build) if w not in normalized(la or [], build, other)]}')
    good = not bad and not diff and link_same
    print(f'   {"ok: the same commands but for -mmacosx-version-min" if good else "FAIL"}')
    ok &= good

    # 2. what the compiler makes of them
    groups = collections.OrderedDict()
    for o in sorted(ca):
        groups.setdefault(flag_set(ca[o]), []).append(o)
    print(f'2. code generation options: {len(groups)} distinct sets of flags')
    good, notes = True, set()
    for (compiler, lang, flags), objs in groups.items():
        a1, b1 = cc1(compiler, lang, list(flags), target), cc1(compiler, lang, list(flags), reference)
        diffs, expected = cc1_differences(a1, b1)
        cpu = a1[a1.index('-target-cpu') + 1] if '-target-cpu' in a1 else '?'
        opt = next((x for x in a1 if re.fullmatch(r'-O[0-9sz]', x)), '-O0')
        nfeat = sum(1 for x in a1 if x == '-target-feature')
        print(f'   {len(objs):>4} {lang:<14} {os.path.basename(compiler)}: {opt}, -target-cpu {cpu}, {nfeat} target '
              f'features: {"the same for both targets" if not diffs else "FAIL"}')
        for d in diffs:
            print(f'        {d}')
        notes.update(expected)
        good &= not diffs
    for n in sorted(notes):
        print(f'        expected: {n}')
    ok &= good

    # the controls, with the runtime's C++ flags (the set with the most C++ objects)
    cxx = max(((k, v) for k, v in groups.items() if k[1] == 'c++'), key=lambda kv: len(kv[1]))[0]
    print('controls:')
    good, lines = controls(cxx[0], [f for f in cxx[2] if f not in ('-x', 'c++')], target, reference)
    print('\n'.join(lines))
    if not good:
        print('   FAIL: the comparison itself is broken: fix check_codegen.py')
    ok &= good

    if not ok:
        print('FAILED: the two builds are not the same build but for the target (or the check is broken), so '
              'their code is not compared')
        sys.exit(1)
    for b in (build, other):
        print(f'ninja -C {b} SpeedBreaker')
        run('ninja', '-C', b, 'SpeedBreaker')

    # 3. the objects
    print('3. the code of every object, function by function:')
    status = collections.Counter()
    used, unexplained, data = collections.defaultdict(list), [], []
    for o in sorted(set(ca) & set(cb)):
        st, differ, dsec = compare_objects(os.path.join(build, o), os.path.join(other, o))
        status[st] += 1
        for f, how in sorted(differ.items()):
            p = allowed(f)
            (used[p].append(f'{o}: {f}: {how}') if p else unexplained.append(f'{o}: {f}: {how}'))
        data += [f'{o}: {s}' for s in dsec]
    print(f'   {len(set(ca) & set(cb))} objects: {status["identical"]} the same code byte for byte; {status["same"]} '
          f'the same functions once relocated fields are masked; {status["differs"]} with functions that differ')
    for p, items in used.items():
        print(f'   allowed ({p}): {ALLOWED[p]}')
        for i in items:
            print(f'      {i}')
    for u in unexplained:
        print(f'   FAIL: {u}')
    if unexplained:
        print(f'   {len(unexplained)} functions differ that ALLOWED doesn\'t name: read each (llvm-objdump -d -r of '
              f'the object in both builds); if it runs only on a cold path, add it to ALLOWED with why; if not, '
              f'the code for macOS {target} isn\'t the code that was measured')
    print(f'   data sections that differ (not code; for the record): {len(data)}')
    for d in data[:40]:
        print(f'      {d}')
    ok &= not unexplained

    # 4. the linked game
    print('4. the linked game (for the record):')
    print('\n'.join(linked_report(os.path.join(build, 'runtime/SpeedBreaker'), os.path.join(other, 'runtime/SpeedBreaker'))))
    print('PASS' if ok else 'FAILED')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
