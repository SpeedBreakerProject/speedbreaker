#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Hot-patch a prebuilt SpeedBreaker.exe so the audio thread waits on a high-resolution timer.

For when you have an exe but can't rebuild it. Building from this tree already includes the same
fix at source level (runtime/apu/audio_wait.h); you don't need this then.

It redirects the one nanosleep call inside AudioThread (inlined sleep_until, VA 0x1400020e8) to
~350 bytes of new code in a new section. That code waits on a high-resolution waitable timer and
falls back to the original nanosleep if anything is unavailable. The addresses are those of one
specific build, so the input's SHA-256 is checked and anything else is refused.

Needs: Python 3, `pip install pefile`, and `nasm` on PATH.
usage: make_patch.py SpeedBreaker.exe SpeedBreaker-hrtimer.exe
"""
import hashlib, os, struct, subprocess, sys, tempfile
import pefile

BASE_SHA256 = 'fa3add6f5f7c8d04ecaa7ea8019789dbdfd705bd7cf4b5bb93a7f85174fda6e2'  # the v0.1.7 Windows port build
CALL_VA = 0x1400020E8          # call nanosleep, in AudioThread()
CALL_BYTES_OFFSET = 0x14E8     # file offset of that call
CALL_OLD = bytes.fromhex('e8a37dd501')
ORIG_NANOSLEEP = 0x141D59E90
HERE = os.path.dirname(os.path.abspath(__file__))


def main(src, dst):
    data = bytearray(open(src, 'rb').read())
    sha = hashlib.sha256(data).hexdigest()
    if sha != BASE_SHA256:
        sys.exit('refusing: %s has SHA-256 %s\nthis patch is for the build with %s\n(the addresses are specific to it)' % (src, sha, BASE_SHA256))
    if data[CALL_BYTES_OFFSET:CALL_BYTES_OFFSET + 5] != CALL_OLD:
        sys.exit('the call site does not look as expected')

    pe = pefile.PE(data=bytes(data))
    base = pe.OPTIONAL_HEADER.ImageBase
    iat = {}
    for d in pe.DIRECTORY_ENTRY_IMPORT:
        for imp in d.imports:
            if imp.name and imp.name.decode() in ('GetModuleHandleW', 'GetProcAddress', 'WaitForSingleObject', 'CloseHandle'):
                iat[imp.name.decode()] = imp.address
    if len(iat) != 4:
        sys.exit('missing kernel32 imports: %r' % iat)
    secalign = pe.OPTIONAL_HEADER.SectionAlignment
    falign = pe.OPTIONAL_HEADER.FileAlignment
    first_raw = pe.sections[0].PointerToRawData
    cave_rva = (pe.OPTIONAL_HEADER.SizeOfImage + secalign - 1) // secalign * secalign
    cave_va = base + cave_rva

    subs = {'CAVE_VA': hex(cave_va), 'ORIG_NANOSLEEP': hex(ORIG_NANOSLEEP)}
    subs.update({'IAT_' + k: hex(v) for k, v in iat.items()})
    with tempfile.TemporaryDirectory() as tmp:
        asm, out = os.path.join(tmp, 'cave.asm'), os.path.join(tmp, 'cave.bin')
        open(asm, 'w').write(open(os.path.join(HERE, 'cave.asm.in')).read() % subs)
        subprocess.check_call(['nasm', '-Werror', '-f', 'bin', asm, '-o', out])   # -Werror: NASM must never fall back to absolute addressing
        code = open(out, 'rb').read()

    raw_size = (len(code) + falign - 1) // falign * falign
    raw_off = len(data)
    if raw_off % falign:
        sys.exit('file has trailing data (overlay); not handled')
    data += code + b'\0' * (raw_size - len(code))

    pe_off = struct.unpack_from('<I', data, 0x3C)[0]
    nsec = struct.unpack_from('<H', data, pe_off + 6)[0]
    optsz = struct.unpack_from('<H', data, pe_off + 20)[0]
    new_hdr = pe_off + 24 + optsz + nsec * 40
    if any(data[new_hdr:new_hdr + 40]) or new_hdr + 40 > first_raw:
        sys.exit('no room for another section header')
    data[new_hdr:new_hdr + 40] = struct.pack('<8sIIIIIIHHI', b'.hrwait', len(code), cave_rva, raw_size, raw_off, 0, 0, 0, 0, 0x60000020)  # code, execute, read
    struct.pack_into('<H', data, pe_off + 6, nsec + 1)
    opt = pe_off + 24
    struct.pack_into('<I', data, opt + 4, struct.unpack_from('<I', data, opt + 4)[0] + raw_size)   # SizeOfCode
    struct.pack_into('<I', data, opt + 56, cave_rva + secalign)                                    # SizeOfImage
    rel = cave_va - (CALL_VA + 5)
    data[CALL_BYTES_OFFSET:CALL_BYTES_OFFSET + 5] = b'\xe8' + struct.pack('<i', rel)
    checksum = pefile.PE(data=bytes(data)).generate_checksum()
    struct.pack_into('<I', data, opt + 64, checksum)
    open(dst, 'wb').write(data)
    print('wrote %s (%d bytes, new code %d bytes at %#x)' % (dst, len(data), len(code), cave_va))
    print('sha256', hashlib.sha256(data).hexdigest())


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
