#!/usr/bin/env python3
"""mkinstalltape.py -- build a bootable install tape (SIMH .tap): the
kernel, then a filesystem image for kernel/tapeinst.c to copy onto a
blank disk (FS_NOTES.md section 7).

usage: mkinstalltape.py kernel.ab disk.img out.tap

Tape layout:
  file 1: boot record + kernel records (eclipse-toolchain/tape/mktape.py's
          build_tape: the same bootable form eclipse-ab2tape writes)
  file 2: disk.img's blocks 0..N-1, one 512-byte record each
  two tape marks (logical end of tape)
N covers every metadata block (superblock, bitmap, inodes: these must be
written even when zero, or a fresh disk's garbage would become inodes)
and every nonzero block after that; trailing all-zero blocks are free
data blocks, which balloc zeroes when it allocates them anyway.
"""
import os, sys, tempfile
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(here, '..', '..', 'eclipse-toolchain', 'tape'))
sys.path.insert(0, here)
import mktape
from fslib import Image, FS, BSIZE

ab, img_path, out = sys.argv[1:4]
words_by_addr = mktape.parse_ab(ab)
lo, program_words = mktape.flatten(words_by_addr)
with tempfile.TemporaryDirectory() as d:
    boot_tap = os.path.join(d, 'boot.tap')
    mktape.build_tape(lo, program_words, boot_tap)
    boot_records = [r for r in mktape.parse_tap(boot_tap) if r is not None]

data = open(img_path, 'rb').read()
img = Image(data)
fs = FS(img)
nblk = len(data) // BSIZE
last = fs.dstart - 1
for b in range(nblk - 1, fs.dstart - 1, -1):
    if any(img.data[b * BSIZE:(b + 1) * BSIZE]):
        last = b
        break
# SIMH stores words little-endian; the tape carries them big-endian, the
# order the MTA DMA (and LDB/STB) uses -- i.e. exactly Image.bytes().
fs_records = [img.bytes(b) for b in range(last + 1)]
mktape.write_tap(out, boot_records + [None] + fs_records + [None, None])
print(f'mkinstalltape: {out}: kernel {len(program_words)} words in '
      f'{len(boot_records) - 1} records; filesystem blocks 0..{last} '
      f'({last + 1} records of {BSIZE} bytes)')
