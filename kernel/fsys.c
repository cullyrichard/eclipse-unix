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
#include "tty.h"
#include "ksyscall.h"
#include "ksync.h"

extern int curproc;
extern unsigned int p_pt[];

#define NFILE      16
#define FD_CONSOLE 0100             /* p_ofile value: the console */
#define FD_NONE    0177777
#define KCHUNK     128

static unsigned int f_ref[NFILE], f_inum[NFILE], f_off[NFILE], f_mode[NFILE];
static unsigned int p_ofile[NPROC * NOFILE];
static char kbuf[KCHUNK];

/* SHELL_NOTES.md: current directory per process (inode; 0 = slot unused) */
static unsigned int p_cwd[NPROC];

/* Pipes (SHELL_NOTES.md). A file slot with f_pipe != 0 is one end of pipe
 * f_pipe-1: f_mode O_RDONLY = read end, O_WRONLY = write end. pr/pw count
 * open read/write *file slots* (a slot shared by dup or inheritance counts
 * once; f_ref tracks the descriptors). */
#define NPIPE  4
#define PIPESZ 256                  /* power of two */
static unsigned int f_pipe[NFILE];
static char pbuf[NPIPE * PIPESZ];
static unsigned int ph[NPIPE], pc[NPIPE], pr[NPIPE], pw[NPIPE], pu[NPIPE];

/* Blocking: a pipe read with no data (and a writer still open), or a pipe
 * write with no space (and a reader still open), ksleep()s on
 * CHAN_PIPE+pipe and re-tests when woken (IO_NOTES.md sleep/wakeup). Each
 * transfer, and closing either end, wakes the channel. Pipe calls never
 * hold the fs lock (fsys_needslock), so a sleeping reader can't stop its
 * writer. */

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

static unsigned int upoll;

static unsigned int ugetb(unsigned int b) {
    TTY_POLL_EVERY(upoll, 3);  /* byte-at-a-time user copies */
    unsigned int w = kcopyin_word(b >> 1);
    return (b & 1) ? (w & 0377) : ((w >> 8) & 0377);
}

static void uputb(unsigned int b, unsigned int v) {
    TTY_POLL_EVERY(upoll, 3);
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
    p_cwd[n] = ROOTINO;
}

/* child starts with the parent's descriptors 0-2 (shared file slots, so a
 * shared offset, as after a Unix fork) and current directory */
void fsys_inherit(int child, int parent) {
    int i;
    unsigned int f;
    /* only 0-2: with spawn instead of fork there is no place to close
     * the rest, and a pipeline writer holding its own pipe's read end
     * would block forever once the real reader exits (SHELL_NOTES.md) */
    for (i = 0; i < 3; i++) {
        f = p_ofile[parent * NOFILE + i];
        p_ofile[child * NOFILE + i] = f;
        if (f < NFILE) f_ref[f]++;
    }
    p_cwd[child] = p_cwd[parent];
}

unsigned int fsys_cwd(int n) {
    return p_cwd[n] ? p_cwd[n] : ROOTINO;
}

static void fclose_slot(unsigned int f) {
    unsigned int pi;
    if (f < NFILE && f_ref[f] > 0) {
        f_ref[f]--;
        if (f_ref[f] == 0 && f_pipe[f]) {
            pi = f_pipe[f] - 1;
            if (f_mode[f] == O_RDONLY) pr[pi]--; else pw[pi]--;
            if (pr[pi] == 0 && pw[pi] == 0) pu[pi] = 0;
            f_pipe[f] = 0;
            wakeup(CHAN_PIPE + pi);  /* EOF for readers, EPIPE for writers */
        }
    }
}

void fsys_procexit(int n) {
    int i;
    for (i = 0; i < NOFILE; i++) {
        fclose_slot(p_ofile[n * NOFILE + i]);
        p_ofile[n * NOFILE + i] = FD_NONE;
    }
    p_cwd[n] = 0;
}

static int inum_open(unsigned int inum) {
    int f;
    for (f = 0; f < NFILE; f++)
        if (f_ref[f] > 0 && !f_pipe[f] && f_inum[f] == inum) return 1;
    for (f = 0; f < NPROC; f++)
        if (p_cwd[f] == inum) return 1;         /* someone's cwd */
    return 0;
}

int fsys_isconsole(unsigned int fd) {
    return fd < NOFILE && p_ofile[curproc * NOFILE + fd] == FD_CONSOLE;
}

/* 1 if syscall `no` with first argument a1 can reach fs.c and so must run
 * under the fs lock (proc.c do_syscall). Console and pipe I/O, close, dup
 * and pipe never touch the disk; a console or pipe read may sleep for a
 * long time and must not hold the lock. */
int fsys_needslock(unsigned int no, unsigned int a1) {
    unsigned int f;
    if (no == SYS_CLOSE || no == SYS_DUP || no == SYS_DUP2 || no == SYS_PIPE)
        return 0;
    if (no == SYS_READ || no == SYS_WRITEFD) {
        if (a1 >= NOFILE) return 0;
        f = p_ofile[curproc * NOFILE + a1];
        return f < NFILE && !f_pipe[f];
    }
    return 1;
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
        /* One line (or n bytes of it) from the interrupt-driven tty
         * (tty.c); sleeps until a line is typed. Uses its own buffer, not
         * kbuf: a process asleep on the disk in the middle of a file
         * read/write owns kbuf's contents (IO_NOTES.md). */
        char lb[32];
        unsigned int got;
        (void)c;
        for (done = 0; done < n; done += got) {
            m = n - done;
            if (m > sizeof lb) m = sizeof lb;
            got = tty_readline(lb, m);
            for (i = 0; i < got; i++) uputb(ubuf + done + i, (unsigned int)lb[i]);
            if (got == 0 || lb[got - 1] == '\n') { done += got; break; }
        }
        return done;
    }
    if (f_mode[f] == O_WRONLY) return 0177777;
    if (f_pipe[f]) {
        unsigned int pi = f_pipe[f] - 1;
        if (n == 0) return 0;
        while (pc[pi] == 0) {
            if (pw[pi] == 0) return 0;          /* no writers: EOF */
            ksleep(CHAN_PIPE + pi);
        }
        m = n < pc[pi] ? n : pc[pi];
        for (i = 0; i < m; i++)
            uputb(ubuf + i, (unsigned int)pbuf[pi * PIPESZ + ((ph[pi] + i) & (PIPESZ - 1))]);
        ph[pi] = (ph[pi] + m) & (PIPESZ - 1);
        pc[pi] -= m;
        wakeup(CHAN_PIPE + pi);
        return m;
    }
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
            tty_poll();     /* polled TTO output is slow: check per char */
        }
        return n;
    }
    if (f_mode[f] == O_RDONLY) return 0177777;
    if (f_pipe[f]) {
        unsigned int pi = f_pipe[f] - 1, space;
        if (n == 0) return 0;
        while (pc[pi] == PIPESZ) {
            if (pr[pi] == 0) return 0177777;   /* no readers: EPIPE */
            ksleep(CHAN_PIPE + pi);
        }
        if (pr[pi] == 0) return 0177777;
        space = PIPESZ - pc[pi];
        m = n < space ? n : space;
        for (i = 0; i < m; i++)
            pbuf[pi * PIPESZ + ((ph[pi] + pc[pi] + i) & (PIPESZ - 1))] = (char)ugetb(ubuf + i);
        pc[pi] += m;
        wakeup(CHAN_PIPE + pi);
        return m;                               /* may be partial */
    }
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
    if (f == FD_NONE || f == FD_CONSOLE || f_pipe[f]) return 0177777;
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

/* ---- SHELL_NOTES.md: cwd, dup, pipes -------------------------------------- */

unsigned int sys_chdir(unsigned int upath) {
    char path[FS_MAXPATH];
    unsigned int inum, ip[IWORDS];
    if (ucopystr(upath, path, FS_MAXPATH) < 0) return 0177777;
    inum = fs_namei(path);
    if (inum == 0 || fs_iread(inum, ip) < 0 || ip[I_TYPE] != T_DIR)
        return 0177777;
    p_cwd[curproc] = inum;
    return 0;
}

static int fdfree(int from) {
    int fd;
    for (fd = from; fd < NOFILE; fd++)
        if (p_ofile[curproc * NOFILE + fd] == FD_NONE) return fd;
    return -1;
}

unsigned int sys_dup(unsigned int fd) {
    unsigned int f = fdget(fd);
    int nfd;
    if (f == FD_NONE) return 0177777;
    nfd = fdfree(0);
    if (nfd < 0) return 0177777;
    p_ofile[curproc * NOFILE + nfd] = f;
    if (f < NFILE) f_ref[f]++;
    return (unsigned int)nfd;
}

unsigned int sys_dup2(unsigned int fd, unsigned int nfd) {
    unsigned int f = fdget(fd);
    if (f == FD_NONE || nfd >= NOFILE) return 0177777;
    if (nfd == fd) return nfd;
    fclose_slot(p_ofile[curproc * NOFILE + nfd]);
    p_ofile[curproc * NOFILE + nfd] = f;
    if (f < NFILE) f_ref[f]++;
    return nfd;
}

/* pipe(int fds[2]): fds[0] = read end, fds[1] = write end */
unsigned int sys_pipe(unsigned int ufds) {
    int pi, fr, fw, dr, dw;
    if ((ufds & 1) || !uvalid(ufds, 4)) return 0177777;
    for (pi = 0; pi < NPIPE; pi++) if (!pu[pi]) break;
    if (pi == NPIPE) return 0177777;
    for (fr = 0; fr < NFILE; fr++) if (f_ref[fr] == 0) break;
    for (fw = fr + 1; fw < NFILE; fw++) if (f_ref[fw] == 0) break;
    if (fw >= NFILE) return 0177777;
    dr = fdfree(0);
    if (dr < 0) return 0177777;
    dw = fdfree(dr + 1);
    if (dw < 0) return 0177777;
    pu[pi] = 1; ph[pi] = 0; pc[pi] = 0; pr[pi] = 1; pw[pi] = 1;
    f_ref[fr] = 1; f_pipe[fr] = (unsigned int)pi + 1; f_mode[fr] = O_RDONLY;
    f_inum[fr] = 0; f_off[fr] = 0;
    f_ref[fw] = 1; f_pipe[fw] = (unsigned int)pi + 1; f_mode[fw] = O_WRONLY;
    f_inum[fw] = 0; f_off[fw] = 0;
    p_ofile[curproc * NOFILE + dr] = (unsigned int)fr;
    p_ofile[curproc * NOFILE + dw] = (unsigned int)fw;
    kcopyout_word(ufds >> 1, (unsigned int)dr);
    kcopyout_word((ufds >> 1) + 1, (unsigned int)dw);
    return 0;
}

/* stat(path, unsigned st[4]): inode number, type (T_FILE/T_DIR), nlink,
 * size in bytes */
unsigned int sys_stat(unsigned int upath, unsigned int ust) {
    char path[FS_MAXPATH];
    unsigned int inum, ip[IWORDS];
    if (ucopystr(upath, path, FS_MAXPATH) < 0) return 0177777;
    if ((ust & 1) || !uvalid(ust, 8)) return 0177777;
    inum = fs_namei(path);
    if (inum == 0 || fs_iread(inum, ip) < 0) return 0177777;
    kcopyout_word(ust >> 1, inum);
    kcopyout_word((ust >> 1) + 1, ip[I_TYPE]);
    kcopyout_word((ust >> 1) + 2, ip[I_NLINK]);
    kcopyout_word((ust >> 1) + 3, ip[I_SIZE]);
    return 0;
}

unsigned int fsys_syscall(unsigned int no, unsigned int a1, unsigned int a2,
                          unsigned int a3) {
    if (no == SYS_OPEN) return sys_open(a1, a2);
    if (no == SYS_CLOSE) return sys_close(a1);
    if (no == SYS_READ) return sys_read(a1, a2, a3);
    if (no == SYS_WRITEFD) return sys_writefd(a1, a2, a3);
    if (no == SYS_LSEEK) return sys_lseek(a1, a2, a3);
    if (no == SYS_MKDIR) return sys_mkdir(a1);
    if (no == SYS_UNLINK) return sys_unlink(a1);
    if (no == SYS_CHDIR) return sys_chdir(a1);
    if (no == SYS_DUP) return sys_dup(a1);
    if (no == SYS_DUP2) return sys_dup2(a1, a2);
    if (no == SYS_PIPE) return sys_pipe(a1);
    if (no == SYS_STAT) return sys_stat(a1, a2);
    return 0177777;
}
