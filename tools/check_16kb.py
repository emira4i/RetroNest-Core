#!/usr/bin/env python3
"""Reports whether arm64 shared libraries are 16 KB page-size compatible.

A library passes when every PT_LOAD segment is aligned to at least 16 KB (p_align >= 0x4000).
Usage: python tools/check_16kb.py [file or folder ...]   (default: corehost/libs/arm64-v8a)
Exit status 1 when at least one library fails.
"""
import glob
import os
import struct
import sys

PT_LOAD = 1
PAGE_16K = 0x4000


def load_alignments(path):
    with open(path, 'rb') as f:
        header = f.read(64)
        if header[:4] != b'\x7fELF' or header[4] != 2:
            raise ValueError('not a 64-bit ELF')
        phoff = struct.unpack_from('<Q', header, 32)[0]
        phentsize, phnum = struct.unpack_from('<HH', header, 54)
        aligns = []
        for i in range(phnum):
            f.seek(phoff + i * phentsize)
            entry = f.read(phentsize)
            if struct.unpack_from('<I', entry, 0)[0] == PT_LOAD:
                aligns.append(struct.unpack_from('<Q', entry, 48)[0])
    return aligns


def main():
    targets = sys.argv[1:] or ['corehost/libs/arm64-v8a']
    files = []
    for target in targets:
        files += sorted(glob.glob(os.path.join(target, '*.so'))) if os.path.isdir(target) else [target]
    failed = 0
    for path in files:
        try:
            aligns = load_alignments(path)
        except ValueError:
            print(f'SKIP {os.path.basename(path):48} not a 64-bit ELF')
            continue
        ok = bool(aligns) and min(aligns) >= PAGE_16K
        failed += not ok
        print(f"{'OK  ' if ok else 'FAIL'} {os.path.basename(path):48} min align {hex(min(aligns)) if aligns else '-'}")
    print(f'{len(files) - failed} of {len(files)} compatible')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
