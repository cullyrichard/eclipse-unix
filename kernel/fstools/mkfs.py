#!/usr/bin/env python3
"""mkfs.py -- build a filesystem image for the kernel (FS_NOTES.md).

usage: mkfs.py image [-n NBLOCKS] [-i NINODES] [SRC:DEST ...]

SRC is a host file; DEST is its absolute path in the new filesystem
(parent directories are created). A SRC ending in .simh is converted to an
executable (simh_to_aout); anything else is copied byte for byte.
"""
import argparse, sys
from fslib import *


def mkdir(fs, parent, name):
    inum = fs.ialloc(T_DIR)
    fs.write_new(inum, dentry(inum, '.') + dentry(parent, '..'))
    ip = fs.iread(inum); ip[I_NLINK] = 2; fs.iwrite(inum, ip)
    link(fs, parent, name, inum)
    pp = fs.iread(parent); pp[I_NLINK] += 1; fs.iwrite(parent, pp)
    return inum


def link(fs, dinum, name, inum):
    old = fs.read(dinum)
    ip = fs.iread(dinum)
    # rewrite the directory (simple: free its blocks, write again)
    for b in fs.blocks(ip):
        if b: fs.setbit(b, 0)
    if ip[I_INDIR]: fs.setbit(ip[I_INDIR], 0)
    for i in range(NDIRECT): ip[I_ADDR + i] = 0
    ip[I_INDIR] = 0; ip[I_SIZE] = 0
    fs.iwrite(dinum, ip)
    fs.write_new(dinum, old + dentry(inum, name))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('image')
    ap.add_argument('-n', type=int, default=1024)
    ap.add_argument('-i', type=int, default=128)
    ap.add_argument('files', nargs='*')
    a = ap.parse_args()
    assert a.i % IPB == 0 and a.n <= 65535   # 16-bit block numbers
    bmap_blocks = (a.n + 4095) // 4096
    istart = 2 + bmap_blocks
    dstart = istart + a.i // IPB
    img = Image(bytes(a.n * BSIZE))
    sb = [0] * BWORDS
    sb[:6] = [FS_MAGIC, a.n, a.i, 2, istart, dstart]
    img.set_words(1, sb)
    fs = FS(img)
    for blk in range(dstart):
        fs.setbit(blk, 1)                   # metadata blocks are in use
    root = [0] * IWORDS
    root[I_TYPE] = T_DIR
    fs.iwrite(ROOTINO, root)
    fs.write_new(ROOTINO, dentry(ROOTINO, '.') + dentry(ROOTINO, '..'))
    ip = fs.iread(ROOTINO); ip[I_NLINK] = 2; fs.iwrite(ROOTINO, ip)
    for spec in a.files:
        src, dest = spec.rsplit(':', 1)
        parts = [p for p in dest.split('/') if p]
        cur = ROOTINO
        for d in parts[:-1]:
            cur = fs.lookup(cur, d) or mkdir(fs, cur, d)
        data = simh_to_aout(src) if src.endswith('.simh') else open(src, 'rb').read()
        inum = fs.ialloc(T_FILE)
        fs.write_new(inum, data)
        ip = fs.iread(inum); ip[I_NLINK] = 1; fs.iwrite(inum, ip)
        link(fs, cur, parts[-1], inum)
    open(a.image, 'wb').write(img.data)
    free = sum(1 for b in range(dstart, a.n) if not fs.bit(b))
    print(f'mkfs: {a.image}: {a.n} blocks, {a.i} inodes, data from {dstart}, '
          f'{len(a.files)} files, {free} blocks free')


if __name__ == '__main__':
    main()
