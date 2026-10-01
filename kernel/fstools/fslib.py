"""fslib.py -- host-side implementation of the kernel's filesystem format
(kernel/fs.h, FS_NOTES.md), shared by mkfs.py and fsck.py.

The image is a SIMH DSK attach file: 16-bit words, each stored
little-endian (STORAGE_NOTES.md). The Eclipse sees a word's two bytes
high-first (LDB/STB, DEBUGGING_NOTES.md #38), so a block's *byte* view --
file data, directory entries -- is the big-endian packing of its words.
"""
import re, struct

BSIZE, BWORDS = 512, 256
FS_MAGIC = 0o105106
ROOTINO = 1
IWORDS, IPB = 16, 16
NDIRECT, NINDIRECT = 8, 256
DIRSIZ, DENTSZ = 14, 16
MAXFILE = 65535
T_FREE, T_FILE, T_DIR = 0, 1, 2
I_TYPE, I_NLINK, I_SIZE, I_ADDR, I_INDIR = 0, 1, 2, 4, 12
AOUT_MAGIC = 0o407     # header: magic, nwords, base, stacktop
AOUT_MAGIC2 = 0o410    # ... + stack words (reorder_asm.py _STACKWDS)


class Image:
    def __init__(self, data):
        self.data = bytearray(data)

    def words(self, blk):
        return list(struct.unpack_from('<256H', self.data, blk * BSIZE))

    def set_words(self, blk, w):
        struct.pack_into('<256H', self.data, blk * BSIZE, *w)

    def bytes(self, blk):
        return struct.pack('>256H', *self.words(blk))

    def set_bytes(self, blk, b):
        b = bytes(b).ljust(BSIZE, b'\0')
        self.set_words(blk, list(struct.unpack('>256H', b)))


class FS:
    def __init__(self, img):
        self.img = img
        sb = img.words(1)
        self.magic, self.nblocks, self.ninodes, self.bmap, self.istart, self.dstart = sb[:6]

    # inodes
    def iread(self, inum):
        w = self.img.words(self.istart + inum // IPB)
        o = (inum % IPB) * IWORDS
        return w[o:o + IWORDS]

    def iwrite(self, inum, ip):
        blk = self.istart + inum // IPB
        w = self.img.words(blk)
        o = (inum % IPB) * IWORDS
        w[o:o + IWORDS] = ip
        self.img.set_words(blk, w)

    # bitmap
    def bit(self, blk):
        w = self.img.words(self.bmap + (blk >> 12))
        return (w[(blk >> 4) & 0o377] >> (blk & 15)) & 1

    def setbit(self, blk, v):
        b = self.bmap + (blk >> 12)
        w = self.img.words(b)
        i = (blk >> 4) & 0o377
        w[i] = (w[i] | (1 << (blk & 15))) if v else (w[i] & ~(1 << (blk & 15)))
        self.img.set_words(b, w)

    def balloc(self):
        for blk in range(self.dstart, self.nblocks):
            if not self.bit(blk):
                self.setbit(blk, 1)
                self.img.set_words(blk, [0] * BWORDS)
                return blk
        raise RuntimeError('disk full')

    def ialloc(self, typ):
        for inum in range(ROOTINO + 1, self.ninodes):
            if self.iread(inum)[I_TYPE] == T_FREE:
                ip = [0] * IWORDS
                ip[I_TYPE] = typ
                self.iwrite(inum, ip)
                return inum
        raise RuntimeError('no free inodes')

    # file blocks
    def blocks(self, ip, alloc_upto=0):
        """Disk blocks of a file, in order (0 = hole)."""
        n = (ip[I_SIZE] + BSIZE - 1) // BSIZE
        out = [ip[I_ADDR + i] for i in range(min(n, NDIRECT))]
        if n > NDIRECT:
            ind = self.img.words(ip[I_INDIR]) if ip[I_INDIR] else [0] * BWORDS
            out += ind[:n - NDIRECT]
        return out

    def read(self, inum):
        ip = self.iread(inum)
        data = b''.join(self.img.bytes(b) if b else b'\0' * BSIZE
                        for b in self.blocks(ip))
        return data[:ip[I_SIZE]]

    def write_new(self, inum, data):
        """Write data into a freshly allocated (empty) inode."""
        assert len(data) <= MAXFILE
        ip = self.iread(inum)
        nb = (len(data) + BSIZE - 1) // BSIZE
        ind = None
        for i in range(nb):
            blk = self.balloc()
            self.img.set_bytes(blk, data[i * BSIZE:(i + 1) * BSIZE])
            if i < NDIRECT:
                ip[I_ADDR + i] = blk
            else:
                if ind is None:
                    ip[I_INDIR] = self.balloc()
                    ind = [0] * BWORDS
                ind[i - NDIRECT] = blk
        if ind is not None:
            self.img.set_words(ip[I_INDIR], ind)
        ip[I_SIZE] = len(data)
        self.iwrite(inum, ip)

    # directories
    def entries(self, dinum):
        d = self.read(dinum)
        out = []
        for off in range(0, len(d) - DENTSZ + 1, DENTSZ):
            inum = (d[off] << 8) | d[off + 1]
            name = d[off + 2:off + DENTSZ].split(b'\0', 1)[0].decode('latin-1')
            out.append((off, inum, name))
        return out

    def lookup(self, dinum, name):
        for _, inum, n in self.entries(dinum):
            if inum and n == name:
                return inum
        return 0

    def namei(self, path):
        cur = ROOTINO
        for part in [p for p in path.split('/') if p]:
            cur = self.lookup(cur, part)
            if not cur:
                return 0
        return cur


def dentry(inum, name):
    nb = name.encode('latin-1')
    assert 0 < len(nb) <= DIRSIZ, name
    return bytes([inum >> 8, inum & 0o377]) + nb.ljust(DIRSIZ, b'\0')


def simh_to_aout(path):
    """eclipse-cc .simh -> executable file bytes: header (magic, nwords,
    base, stacktop) + words, big-endian (the kernel reads it as words).
    _STACKTOP is recovered from the program's first instruction, LDA
    0,_STACKTOP at 050 -- the same check kernel/mkimage.py makes."""
    mem = {}
    for line in open(path):
        m = re.match(r'^dep\s+([0-7]+)\s+([0-7]+)\s*$', line)
        if m:
            mem[int(m.group(1), 8)] = int(m.group(2), 8)
    base, end = min(mem), max(mem)
    if base != 0o50:
        raise ValueError(f'{path}: origin {base:o}, expected 050')
    lda = mem[0o50]
    if lda & 0o177400 != 0o020000:
        raise ValueError(f'{path}: word at 050 is {lda:o}, not LDA 0,<pz>')
    stacktop = mem[lda & 0o377]
    if stacktop <= end + 1:     # _STACKTOP-1 holds main's argv
        raise ValueError(f'{path}: _STACKTOP {stacktop:o} <= end+1 {end + 1:o}')
    words = [mem.get(a, 0) for a in range(base, end + 1)]
    # _start: LDA 0,_STACKTOP / STA 0,040 / LDA 0,_STACKLIM; _STACKWDS
    # follows _STACKLIM (0177700). Older builds have no _STACKWDS.
    lim = mem.get(0o52, 0)
    if lim & 0o177400 == 0o020000 and mem.get(lim & 0o377) == 0o177700:
        swords = mem.get((lim & 0o377) + 1, 0)
        if 0 < swords and stacktop + swords <= 0o100000:
            return struct.pack(f'>5H{len(words)}H', AOUT_MAGIC2, len(words),
                               base, stacktop, swords, *words)
    return struct.pack(f'>4H{len(words)}H', AOUT_MAGIC, len(words), base,
                       stacktop, *words)
