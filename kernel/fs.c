/* fs.c -- the kernel's filesystem: buffer cache, block/inode allocation,
 * file read/write, directories and path lookup. On-disk format: fs.h and
 * FS_NOTES.md. The kernel is single-threaded and runs ION-off, so nothing
 * here needs locking.
 *
 * The buffer cache is write-through: every modified block goes to disk
 * before the syscall returns, so the disk image is consistent whenever
 * the kernel is not inside a syscall. Callers may hold at most a couple
 * of buffer pointers at once; anything that can call bread() again (e.g.
 * balloc) may evict them, so such code re-reads afterwards. */
#include "fs.h"
#include "../examples/dsk.h"

#define NBUF 8

static unsigned int bdata[NBUF * BWORDS];
static unsigned int bblk[NBUF];
static unsigned int bage[NBUF];
static unsigned int bvalid[NBUF];
static unsigned int bclock;

static unsigned int s_nblocks, s_ninodes, s_bmap, s_istart, s_dstart;

static const unsigned int bitmask[16] = {
    01, 02, 04, 010, 020, 040, 0100, 0200,
    0400, 01000, 02000, 04000, 010000, 020000, 040000, 0100000
};

static unsigned int *bread(unsigned int blk) {
    int i, v;
    unsigned int oldest;
    for (i = 0; i < NBUF; i++) {
        if (bvalid[i] && bblk[i] == blk) {
            bage[i] = ++bclock;
            return &bdata[i * BWORDS];
        }
    }
    v = 0;
    oldest = 0177777;
    for (i = 0; i < NBUF; i++) {
        if (!bvalid[i]) { v = i; break; }
        if (bage[i] < oldest) { oldest = bage[i]; v = i; }
    }
    dsk_read_block(blk, &bdata[v * BWORDS]);
    bblk[v] = blk;
    bvalid[v] = 1;
    bage[v] = ++bclock;
    return &bdata[v * BWORDS];
}

static void bwrite(unsigned int blk, unsigned int *b) {
    dsk_write_block(blk, b);
}

int fs_init(void) {
    unsigned int *sb;
    int i;
    for (i = 0; i < NBUF; i++) bvalid[i] = 0;
    sb = bread(1);
    if (sb[SB_MAGIC] != FS_MAGIC) return -1;
    s_nblocks = sb[SB_NBLOCKS];
    s_ninodes = sb[SB_NINODES];
    s_bmap = sb[SB_BMAP];
    s_istart = sb[SB_ISTART];
    s_dstart = sb[SB_DSTART];
    if (s_nblocks == 0 || s_nblocks > DSK_MAX_BLOCK + 1 || s_dstart >= s_nblocks)
        return -1;
    return 0;
}

/* ---- blocks ------------------------------------------------------------ */

static unsigned int balloc(void) {
    unsigned int blk, i, *b;
    for (blk = s_dstart; blk < s_nblocks; blk++) {
        b = bread(s_bmap + (blk >> 12));
        if ((b[(blk >> 4) & 0377] & bitmask[blk & 15]) == 0) {
            b[(blk >> 4) & 0377] |= bitmask[blk & 15];
            bwrite(s_bmap + (blk >> 12), b);
            b = bread(blk);
            for (i = 0; i < BWORDS; i++) b[i] = 0;
            bwrite(blk, b);
            return blk;
        }
    }
    return 0;
}

static void bfree(unsigned int blk) {
    unsigned int *b;
    if (blk < s_dstart || blk >= s_nblocks) return;
    b = bread(s_bmap + (blk >> 12));
    b[(blk >> 4) & 0377] &= ~bitmask[blk & 15];
    bwrite(s_bmap + (blk >> 12), b);
}

unsigned int fs_freeblocks(void) {
    unsigned int blk, n = 0, *b;
    for (blk = s_dstart; blk < s_nblocks; blk++) {
        b = bread(s_bmap + (blk >> 12));
        if ((b[(blk >> 4) & 0377] & bitmask[blk & 15]) == 0) n++;
    }
    return n;
}

/* ---- inodes ------------------------------------------------------------ */

int fs_iread(unsigned int inum, unsigned int *ip) {
    unsigned int *b, i, o;
    if (inum == 0 || inum >= s_ninodes) return -1;
    b = bread(s_istart + inum / IPB);
    o = (inum % IPB) * IWORDS;
    for (i = 0; i < IWORDS; i++) ip[i] = b[o + i];
    return 0;
}

void fs_iwrite(unsigned int inum, const unsigned int *ip) {
    unsigned int *b, i, o;
    if (inum == 0 || inum >= s_ninodes) return;
    b = bread(s_istart + inum / IPB);
    o = (inum % IPB) * IWORDS;
    for (i = 0; i < IWORDS; i++) b[o + i] = ip[i];
    bwrite(s_istart + inum / IPB, b);
}

unsigned int fs_ialloc(unsigned int type) {
    unsigned int inum, i, ip[IWORDS];
    for (inum = ROOTINO + 1; inum < s_ninodes; inum++) {
        fs_iread(inum, ip);
        if (ip[I_TYPE] == T_FREE) {
            for (i = 0; i < IWORDS; i++) ip[i] = 0;
            ip[I_TYPE] = type;
            fs_iwrite(inum, ip);
            return inum;
        }
    }
    return 0;
}

/* Disk block holding file block fbn of inode ip; with alloc, allocate it
 * (and the indirect block) if missing -- the caller writes ip back. */
static unsigned int bmap(unsigned int *ip, unsigned int fbn, int alloc) {
    unsigned int a, ind, *b;
    if (fbn < NDIRECT) {
        a = ip[I_ADDR + fbn];
        if (a == 0 && alloc) {
            a = balloc();
            ip[I_ADDR + fbn] = a;
        }
        return a;
    }
    fbn -= NDIRECT;
    if (fbn >= NINDIRECT) return 0;
    ind = ip[I_INDIR];
    if (ind == 0) {
        if (!alloc) return 0;
        ind = balloc();
        if (ind == 0) return 0;
        ip[I_INDIR] = ind;
    }
    b = bread(ind);
    a = b[fbn];
    if (a == 0 && alloc) {
        a = balloc();
        if (a == 0) return 0;
        b = bread(ind);             /* balloc may have evicted it */
        b[fbn] = a;
        bwrite(ind, b);
    }
    return a;
}

int fs_readi(unsigned int inum, unsigned int off, char *dst, unsigned int n) {
    unsigned int ip[IWORDS], done, m, boff, blk, j;
    char *cb;
    if (fs_iread(inum, ip) < 0) return -1;
    if (off >= ip[I_SIZE]) return 0;
    if (n > ip[I_SIZE] - off) n = ip[I_SIZE] - off;
    done = 0;
    while (done < n) {
        boff = off & (BSIZE - 1);
        m = BSIZE - boff;
        if (m > n - done) m = n - done;
        blk = bmap(ip, off >> 9, 0);
        if (blk == 0) {
            for (j = 0; j < m; j++) dst[done + j] = 0;
        } else {
            cb = (char *)bread(blk);
            for (j = 0; j < m; j++) dst[done + j] = cb[boff + j];
        }
        done += m;
        off += m;
    }
    return (int)done;
}

int fs_writei(unsigned int inum, unsigned int off, const char *src, unsigned int n) {
    unsigned int ip[IWORDS], done, m, boff, blk, j, *b;
    char *cb;
    if (fs_iread(inum, ip) < 0) return -1;
    if (off > ip[I_SIZE]) return -1;
    if (n > MAXFILE - off) n = MAXFILE - off;
    done = 0;
    while (done < n) {
        boff = off & (BSIZE - 1);
        m = BSIZE - boff;
        if (m > n - done) m = n - done;
        blk = bmap(ip, off >> 9, 1);
        if (blk == 0) break;                    /* disk full */
        b = bread(blk);
        cb = (char *)b;
        for (j = 0; j < m; j++) cb[boff + j] = src[done + j];
        bwrite(blk, b);
        done += m;
        off += m;
    }
    if (off > ip[I_SIZE]) ip[I_SIZE] = off;
    fs_iwrite(inum, ip);
    return (int)done;
}

void fs_trunc(unsigned int inum) {
    unsigned int ip[IWORDS], i, *b, a;
    if (fs_iread(inum, ip) < 0) return;
    for (i = 0; i < NDIRECT; i++) {
        if (ip[I_ADDR + i]) bfree(ip[I_ADDR + i]);
        ip[I_ADDR + i] = 0;
    }
    if (ip[I_INDIR]) {
        for (i = 0; i < NINDIRECT; i++) {
            b = bread(ip[I_INDIR]);
            a = b[i];
            if (a) bfree(a);
        }
        bfree(ip[I_INDIR]);
        ip[I_INDIR] = 0;
    }
    ip[I_SIZE] = 0;
    fs_iwrite(inum, ip);
}

/* ---- directories ------------------------------------------------------- */

static int namematch(const char *name, const char *ent) {
    int i;
    for (i = 0; i < DIRSIZ; i++) {
        if (name[i] != ent[i]) return 0;
        if (name[i] == 0) return 1;
    }
    return 1;
}

static unsigned int dent_inum(const char *de) {
    return (((unsigned int)de[0] & 0377) << 8) | ((unsigned int)de[1] & 0377);
}

unsigned int fs_dirlookup(unsigned int dinum, const char *name) {
    unsigned int ip[IWORDS], off, inum;
    char de[DENTSZ];
    if (fs_iread(dinum, ip) < 0 || ip[I_TYPE] != T_DIR) return 0;
    for (off = 0; off + DENTSZ <= ip[I_SIZE]; off += DENTSZ) {
        if (fs_readi(dinum, off, de, DENTSZ) != DENTSZ) return 0;
        inum = dent_inum(de);
        if (inum != 0 && namematch(name, de + 2)) return inum;
    }
    return 0;
}

int fs_dirlink(unsigned int dinum, const char *name, unsigned int inum) {
    unsigned int ip[IWORDS], off;
    char de[DENTSZ];
    int i, end;
    if (name[0] == 0 || fs_dirlookup(dinum, name) != 0) return -1;
    if (fs_iread(dinum, ip) < 0 || ip[I_TYPE] != T_DIR) return -1;
    for (off = 0; off + DENTSZ <= ip[I_SIZE]; off += DENTSZ) {
        fs_readi(dinum, off, de, DENTSZ);
        if (dent_inum(de) == 0) break;          /* reuse a free slot */
    }
    de[0] = (char)(inum >> 8);
    de[1] = (char)(inum & 0377);
    end = 0;
    for (i = 0; i < DIRSIZ; i++) {
        if (name[i] == 0) end = 1;
        de[2 + i] = end ? 0 : name[i];
    }
    return fs_writei(dinum, off, de, DENTSZ) == DENTSZ ? 0 : -1;
}

int fs_dirunlink(unsigned int dinum, const char *name) {
    unsigned int ip[IWORDS], off;
    char de[DENTSZ];
    int i;
    if (fs_iread(dinum, ip) < 0) return -1;
    for (off = 0; off + DENTSZ <= ip[I_SIZE]; off += DENTSZ) {
        fs_readi(dinum, off, de, DENTSZ);
        if (dent_inum(de) != 0 && namematch(name, de + 2)) {
            for (i = 0; i < DENTSZ; i++) de[i] = 0;
            fs_writei(dinum, off, de, DENTSZ);
            return 0;
        }
    }
    return -1;
}

/* Copy the next path component into name (NUL-padded to DIRSIZ+1). Returns
 * the rest of the path, or 0 if the component is longer than DIRSIZ. */
static const char *skipelem(const char *path, char *name) {
    int n = 0, i;
    while (*path == '/') path++;
    while (*path != 0 && *path != '/') {
        if (n >= DIRSIZ) return 0;
        name[n] = *path;
        n++;
        path++;
    }
    for (i = n; i <= DIRSIZ; i++) name[i] = 0;
    return path;
}

static unsigned int walk(const char *path, int parent, char *name) {
    unsigned int cur = ROOTINO, next;
    const char *rest;
    char elem[DIRSIZ + 1];
    for (;;) {
        rest = skipelem(path, elem);
        if (rest == 0) return 0;                /* component too long */
        if (elem[0] == 0) return parent ? 0 : cur;
        if (parent) {
            const char *p = rest;
            while (*p == '/') p++;
            if (*p == 0) {                      /* elem is the last one */
                int i;
                for (i = 0; i <= DIRSIZ; i++) name[i] = elem[i];
                return cur;
            }
        }
        next = fs_dirlookup(cur, elem);
        if (next == 0) return 0;
        cur = next;
        path = rest;
    }
}

unsigned int fs_namei(const char *path) {
    return walk(path, 0, 0);
}

unsigned int fs_nameiparent(const char *path, char *name) {
    unsigned int ip[IWORDS], p;
    p = walk(path, 1, name);
    if (p == 0 || fs_iread(p, ip) < 0 || ip[I_TYPE] != T_DIR) return 0;
    return p;
}

unsigned int fs_create(const char *path) {
    unsigned int p, i, ip[IWORDS];
    char name[DIRSIZ + 1];
    p = fs_nameiparent(path, name);
    if (p == 0) return 0;
    i = fs_dirlookup(p, name);
    if (i != 0) {
        fs_iread(i, ip);
        return ip[I_TYPE] == T_FILE ? i : 0;
    }
    i = fs_ialloc(T_FILE);
    if (i == 0) return 0;
    fs_iread(i, ip);
    ip[I_NLINK] = 1;
    fs_iwrite(i, ip);
    if (fs_dirlink(p, name, i) < 0) {
        ip[I_TYPE] = T_FREE;
        ip[I_NLINK] = 0;
        fs_iwrite(i, ip);
        return 0;
    }
    return i;
}

int fs_mkdir(const char *path) {
    unsigned int p, i, ip[IWORDS];
    char name[DIRSIZ + 1];
    p = fs_nameiparent(path, name);
    if (p == 0 || fs_dirlookup(p, name) != 0) return -1;
    i = fs_ialloc(T_DIR);
    if (i == 0) return -1;
    fs_iread(i, ip);
    ip[I_NLINK] = 2;                            /* its entry + "." */
    fs_iwrite(i, ip);
    if (fs_dirlink(i, ".", i) < 0 || fs_dirlink(i, "..", p) < 0 ||
        fs_dirlink(p, name, i) < 0) {
        fs_trunc(i);
        fs_iread(i, ip);
        ip[I_TYPE] = T_FREE;
        ip[I_NLINK] = 0;
        fs_iwrite(i, ip);
        return -1;
    }
    fs_iread(p, ip);
    ip[I_NLINK]++;                              /* the child's ".." */
    fs_iwrite(p, ip);
    return 0;
}

static int dir_empty(unsigned int dinum) {
    unsigned int ip[IWORDS], off;
    char de[DENTSZ];
    fs_iread(dinum, ip);
    for (off = 2 * DENTSZ; off + DENTSZ <= ip[I_SIZE]; off += DENTSZ) {
        fs_readi(dinum, off, de, DENTSZ);
        if (dent_inum(de) != 0) return 0;
    }
    return 1;
}

int fs_unlink(const char *path) {
    unsigned int p, i, ip[IWORDS];
    char name[DIRSIZ + 1];
    p = fs_nameiparent(path, name);
    if (p == 0) return -1;
    if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0)))
        return -1;
    i = fs_dirlookup(p, name);
    if (i == 0) return -1;
    fs_iread(i, ip);
    if (ip[I_TYPE] == T_DIR && !dir_empty(i)) return -1;
    if (fs_dirunlink(p, name) < 0) return -1;
    if (ip[I_TYPE] == T_DIR) {
        fs_iread(p, ip);
        ip[I_NLINK]--;                          /* the child's ".." */
        fs_iwrite(p, ip);
        fs_trunc(i);
        fs_iread(i, ip);
        ip[I_TYPE] = T_FREE;
        ip[I_NLINK] = 0;
        fs_iwrite(i, ip);
        return 0;
    }
    ip[I_NLINK]--;
    fs_iwrite(i, ip);
    if (ip[I_NLINK] == 0) {
        fs_trunc(i);
        fs_iread(i, ip);
        ip[I_TYPE] = T_FREE;
        fs_iwrite(i, ip);
    }
    return 0;
}
