#!/usr/bin/env python3
"""fsck.py -- check (and list / extract from) a kernel filesystem image,
independently of the kernel's own code (FS_NOTES.md).

usage: fsck.py image [-l] [-x FSPATH]
  -l        list every file (ls -lR style)
  -x PATH   print the contents of PATH to stdout (bytes)

Checks: superblock; every allocated inode's blocks are in range and owned
by exactly one inode; no blocks beyond the file size; the bitmap matches
exactly the metadata + owned blocks; every directory has correct '.' and
'..'; every directory entry names an allocated inode; every link count
equals the number of entries naming it; every allocated inode is
reachable from the root. Exit status 1 on any problem.
"""
import argparse, sys
from fslib import *


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('image')
    ap.add_argument('-l', action='store_true')
    ap.add_argument('-x')
    a = ap.parse_args()
    img = Image(open(a.image, 'rb').read())
    fs = FS(img)
    if a.x:
        inum = fs.namei(a.x)
        if not inum:
            sys.exit(f'{a.x}: not found')
        sys.stdout.buffer.write(fs.read(inum))
        return
    errs = []
    if fs.magic != FS_MAGIC:
        sys.exit(f'bad magic {fs.magic:o}')
    owner = {}
    for inum in range(1, fs.ninodes):
        ip = fs.iread(inum)
        if ip[I_TYPE] == T_FREE:
            continue
        if ip[I_TYPE] not in (T_FILE, T_DIR):
            errs.append(f'inode {inum}: bad type {ip[I_TYPE]}')
            continue
        n = (ip[I_SIZE] + BSIZE - 1) // BSIZE
        mine = [ip[I_ADDR + i] for i in range(NDIRECT)]
        for i, b in enumerate(mine):
            if b and i >= n:
                errs.append(f'inode {inum}: direct block {i} beyond size')
        if ip[I_INDIR]:
            if n <= NDIRECT:
                errs.append(f'inode {inum}: indirect block but size {ip[I_SIZE]}')
            ind = img.words(ip[I_INDIR])
            for i, b in enumerate(ind):
                if b and NDIRECT + i >= n:
                    errs.append(f'inode {inum}: indirect entry {i} beyond size')
            mine += [ip[I_INDIR]] + ind
        for b in mine:
            if not b:
                continue
            if not (fs.dstart <= b < fs.nblocks):
                errs.append(f'inode {inum}: block {b} out of range')
            elif b in owner:
                errs.append(f'block {b}: owned by inodes {owner[b]} and {inum}')
            else:
                owner[b] = inum
    for b in range(fs.nblocks):
        want = 1 if (b < fs.dstart or b in owner) else 0
        if fs.bit(b) != want:
            errs.append(f'bitmap: block {b} is {fs.bit(b)}, should be {want}')
    refs, seen, listing = {}, set(), []

    def walk(dinum, parent, path):
        seen.add(dinum)
        ents = fs.entries(dinum)
        if len(ents) < 2 or ents[0][2] != '.' or ents[0][1] != dinum \
                or ents[1][2] != '..' or ents[1][1] != parent:
            errs.append(f'{path or "/"}: bad . / .. entries')
        for off, inum, name in ents:
            if not inum:
                continue
            refs[inum] = refs.get(inum, 0) + 1
            if name in ('.', '..'):
                continue
            ip = fs.iread(inum)
            p = f'{path}/{name}'
            if ip[I_TYPE] == T_FREE:
                errs.append(f'{p}: names free inode {inum}')
                continue
            listing.append((p, inum, ip))
            if ip[I_TYPE] == T_DIR:
                if inum in seen:
                    errs.append(f'{p}: directory reached twice')
                else:
                    walk(inum, dinum, p)
            else:
                seen.add(inum)

    walk(ROOTINO, ROOTINO, '')
    for inum in range(1, fs.ninodes):
        ip = fs.iread(inum)
        if ip[I_TYPE] == T_FREE:
            continue
        if inum not in seen:
            errs.append(f'inode {inum}: allocated but unreachable')
        if refs.get(inum, 0) != ip[I_NLINK]:
            errs.append(f'inode {inum}: nlink {ip[I_NLINK]}, {refs.get(inum, 0)} entries')
    free = sum(1 for b in range(fs.dstart, fs.nblocks) if not fs.bit(b))
    if a.l:
        for p, inum, ip in listing:
            kind = 'd' if ip[I_TYPE] == T_DIR else '-'
            print(f'{kind} {inum:4d} {ip[I_NLINK]:2d} {ip[I_SIZE]:6d} {p}')
    for e in errs:
        print('fsck:', e)
    print(f'fsck: {len(listing)} names, {len(owner)} data blocks in use, '
          f'{free} free, {len(errs)} problems')
    sys.exit(1 if errs else 0)


if __name__ == '__main__':
    main()
