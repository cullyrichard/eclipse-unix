/* fsys.c -- file descriptors and the file syscalls on top of fs.c.
 *
 * Global open-file table (f_*), per-process descriptor table (p_ofile).
 * fds 0-2 of every process start out as the console. User buffers are
 * byte addresses in the calling process's space (DEBUGGING_NOTES.md #38)
 * and are validated page by page before any byte is touched. */
#include "fsys.h"
#include "fs.h"
#include "proc.h"
#include "kmem.h"
#include "../examples/console.h"

extern int curproc;
extern unsigned int p_pt[];

#define NFILE      16
#define FD_CONSOLE 0100             /* p_ofile value: the console */
#define FD_NONE    0177777
#define KCHUNK     128

static unsigned int f_ref[NFILE], f_inum[NFILE], f_off[NFILE], f_mode[NFILE];
static unsigned int p_ofile[NPROC * NOFILE];
static char kbuf[KCHUNK];

/* ---- user memory --------------------------------------------------------- */

int uvalid(unsigned int b, unsigned int len) {
    unsigned int pg, last, pt0;
    if (len == 0) return 1;
    if (b + (len - 1) < b) return 0;            /* wraps */
    pt0 = (unsigned int)curproc * NPAGES;
    last = ((b + (len - 1)) >> 1) >> 10;
    for (pg = (b >> 1) >> 10; pg <= last; pg++)
        if ((p_pt[pt0 + pg] & PTE_INVALID) == PTE_INVALID) return 0;
    return 1;
}

static unsigned int ugetb(unsigned int b) {
    unsigned int w = kcopyin_word(b >> 1);
    return (b & 1) ? (w & 0377) : ((w >> 8) & 0377);
}

static void uputb(unsigned int b, unsigned int v) {
    unsigned int w = kcopyin_word(b >> 1);
    if (b & 1) w = (w & 0177400) | (v & 0377);
    else w = (w & 0377) | ((v & 0377) << 8);
    kcopyout_word(b >> 1, w);
}

/* NUL-terminated user string -> k (at most max-1 chars). -1 if invalid or
 * too long. */
int ucopystr(unsigned int ub, char *k, unsigned int max) {
    unsigned int i, c;
    for (i = 0; i < max; i++) {
        if (!uvalid(ub + i, 1)) return -1;
        c = ugetb(ub + i);
        k[i] = (char)c;
        if (c == 0) return 0;
    }
    return -1;
}

/* ---- descriptors --------------------------------------------------------- */

void fsys_procinit(int n) {
    int i;
    for (i = 0; i < NOFILE; i++)
        p_ofile[n * NOFILE + i] = i < 3 ? FD_CONSOLE : FD_NONE;
}

static void fclose_slot(unsigned int f) {
    if (f < NFILE && f_ref[f] > 0) f_ref[f]--;
}

void fsys_procexit(int n) {
    int i;
    for (i = 0; i < NOFILE; i++) {
        fclose_slot(p_ofile[n * NOFILE + i]);
        p_ofile[n * NOFILE + i] = FD_NONE;
    }
}

static int inum_open(unsigned int inum) {
    int f;
    for (f = 0; f < NFILE; f++)
        if (f_ref[f] > 0 && f_inum[f] == inum) return 1;
    return 0;
}

/* p_ofile entry for fd, or FD_NONE */
static unsigned int fdget(unsigned int fd) {
    if (fd >= NOFILE) return FD_NONE;
    return p_ofile[curproc * NOFILE + fd];
}

unsigned int sys_open(unsigned int upath, unsigned int flags) {
    char path[FS_MAXPATH];
    unsigned int inum, ip[IWORDS], acc;
    int f, fd;
    if (ucopystr(upath, path, FS_MAXPATH) < 0) return 0177777;
    acc = flags & 3;
    if (acc == 3) return 0177777;
    inum = (flags & O_CREAT) ? fs_create(path) : fs_namei(path);
    if (inum == 0 || fs_iread(inum, ip) < 0) return 0177777;
    if (ip[I_TYPE] == T_DIR && acc != O_RDONLY) return 0177777;
    fd = -1;
    for (f = 0; f < NOFILE; f++)
        if (p_ofile[curproc * NOFILE + f] == FD_NONE) { fd = f; break; }
    if (fd < 0) return 0177777;
    for (f = 0; f < NFILE; f++)
        if (f_ref[f] == 0) break;
    if (f == NFILE) return 0177777;
    if ((flags & O_TRUNC) && acc != O_RDONLY && ip[I_TYPE] == T_FILE)
        fs_trunc(inum);
    f_ref[f] = 1;
    f_inum[f] = inum;
    f_off[f] = 0;
    f_mode[f] = acc;
    p_ofile[curproc * NOFILE + fd] = (unsigned int)f;
    return (unsigned int)fd;
}

unsigned int sys_close(unsigned int fd) {
    unsigned int f = fdget(fd);
    if (f == FD_NONE) return 0177777;
    fclose_slot(f);
    p_ofile[curproc * NOFILE + fd] = FD_NONE;
    return 0;
}

unsigned int sys_read(unsigned int fd, unsigned int ubuf, unsigned int n) {
    unsigned int f = fdget(fd), done, m, i;
    int got, c;
    if (f == FD_NONE || !uvalid(ubuf, n)) return 0177777;
    if (f == FD_CONSOLE) {
        /* one line (or n chars), echoed; CR is returned as '\n' */
        for (done = 0; done < n; ) {
            c = console_getchar() & 0177;
            if (c == '\r') c = '\n';
            console_putchar(c == '\n' ? '\r' : c);
            if (c == '\n') console_putchar('\n');
            uputb(ubuf + done, (unsigned int)c);
            done++;
            if (c == '\n') break;
        }
        return done;
    }
    if (f_mode[f] == O_WRONLY) return 0177777;
    for (done = 0; done < n; done += (unsigned int)got) {
        m = n - done;
        if (m > KCHUNK) m = KCHUNK;
        got = fs_readi(f_inum[f], f_off[f], kbuf, m);
        if (got <= 0) break;
        for (i = 0; i < (unsigned int)got; i++)
            uputb(ubuf + done + i, (unsigned int)kbuf[i]);
        f_off[f] += (unsigned int)got;
    }
    return done;
}

unsigned int sys_writefd(unsigned int fd, unsigned int ubuf, unsigned int n) {
    unsigned int f = fdget(fd), done, m, i;
    int put, c;
    if (f == FD_NONE || !uvalid(ubuf, n)) return 0177777;
    if (f == FD_CONSOLE) {
        for (i = 0; i < n; i++) {
            c = (int)ugetb(ubuf + i);
            if (c == '\n') console_putchar('\r');     /* ONLCR */
            console_putchar(c);
        }
        return n;
    }
    if (f_mode[f] == O_RDONLY) return 0177777;
    for (done = 0; done < n; done += (unsigned int)put) {
        m = n - done;
        if (m > KCHUNK) m = KCHUNK;
        for (i = 0; i < m; i++) kbuf[i] = (char)ugetb(ubuf + done + i);
        put = fs_writei(f_inum[f], f_off[f], kbuf, m);
        if (put <= 0) break;
        f_off[f] += (unsigned int)put;
        if ((unsigned int)put < m) { done += (unsigned int)put; break; }
    }
    return done;
}

unsigned int sys_lseek(unsigned int fd, unsigned int off, unsigned int whence) {
    unsigned int f = fdget(fd), ip[IWORDS], base;
    if (f == FD_NONE || f == FD_CONSOLE) return 0177777;
    if (whence == 0) base = 0;
    else if (whence == 1) base = f_off[f];
    else if (whence == 2) { fs_iread(f_inum[f], ip); base = ip[I_SIZE]; }
    else return 0177777;
    /* off is a signed displacement for whence 1/2 */
    f_off[f] = base + off;
    return f_off[f];
}

unsigned int sys_mkdir(unsigned int upath) {
    char path[FS_MAXPATH];
    if (ucopystr(upath, path, FS_MAXPATH) < 0) return 0177777;
    return fs_mkdir(path) < 0 ? 0177777 : 0;
}

unsigned int sys_unlink(unsigned int upath) {
    char path[FS_MAXPATH];
    unsigned int inum;
    if (ucopystr(upath, path, FS_MAXPATH) < 0) return 0177777;
    inum = fs_namei(path);
    if (inum == 0 || inum_open(inum)) return 0177777;
    return fs_unlink(path) < 0 ? 0177777 : 0;
}
