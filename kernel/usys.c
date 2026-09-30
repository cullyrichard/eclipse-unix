#include "usys.h"
#include "ksyscall.h"

/* The one function that issues SYC -- examples/ulib.c's __syscall(),
 * unchanged in mechanism: every value crosses the asm boundary through a
 * named global, never an "r" operand, and AC2/AC3 (the live frame
 * pointer and the caller's AC3) are saved/restored around SYC because
 * staging arg2 in AC2 clobbers the frame pointer. See examples/ulib.c's
 * comments for the full reasoning. */
static int __sys_reason;
static int __sys_arg1;
static int __sys_arg2;
static int __sys_arg3;
static int __sys_ret;
static unsigned int __sys_save2;
static unsigned int __sys_save3;

/* arg3 travels in AC3 (FS_NOTES.md); AC3 is saved/restored anyway. */
static int __syscall3(int reason, int arg1, int arg2, int arg3) {
    __sys_reason = reason;
    __sys_arg1 = arg1;
    __sys_arg2 = arg2;
    __sys_arg3 = arg3;
    __sys_save2 = 0; /* keep storage alive through globaldce */
    __sys_save3 = 0;
    asm volatile(
        "ESTA 2,__sys_save2,0\n\t"
        "ESTA 3,__sys_save3,0\n\t"
        "ELDA 0,__sys_reason,0\n\t"
        "ELDA 1,__sys_arg1,0\n\t"
        "ELDA 2,__sys_arg2,0\n\t"
        "ELDA 3,__sys_arg3,0\n\t"
        "SYC 1,1\n\t"
        "ESTA 0,__sys_ret,0\n\t"
        "ELDA 2,__sys_save2,0\n\t"
        "ELDA 3,__sys_save3,0\n\t"
    );
    return __sys_ret;
}

static int __syscall(int reason, int arg1, int arg2) {
    return __syscall3(reason, arg1, arg2, 0);
}

int sys_putchar(int c)    { return __syscall(SYS_PUTCHAR, c, 0); }
int sys_getchar(void)     { return __syscall(SYS_GETCHAR, 0, 0); }
void sys_exit(int status) { __syscall(SYS_EXIT, status, 0); }
int sys_getpid(void)      { return __syscall(SYS_GETPID, 0, 0); }
int sys_write(const char *buf, int len) {
    return __syscall(SYS_WRITE, (int)buf, len);
}
int sys_segalloc(int npages) { return __syscall(SYS_SEGALLOC, npages, 0); }
int sys_mapwin(int winpage, int segpage, int npages) {
    return __syscall(SYS_MAPWIN, (winpage & 037) | (npages << 5), segpage);
}
int sys_unmapwin(int winpage, int npages) {
    return __syscall(SYS_MAPWIN, (winpage & 037) | (npages << 5), -1);
}

int open(const char *path, int flags) { return __syscall(SYS_OPEN, (int)path, flags); }
int close(int fd)                     { return __syscall(SYS_CLOSE, fd, 0); }
int read(int fd, char *buf, int n)    { return __syscall3(SYS_READ, fd, (int)buf, n); }
int write(int fd, const char *buf, int n) {
    return __syscall3(SYS_WRITEFD, fd, (int)buf, n);
}
int lseek(int fd, int off, int whence) { return __syscall3(SYS_LSEEK, fd, off, whence); }
int mkdir(const char *path)           { return __syscall(SYS_MKDIR, (int)path, 0); }
int unlink(const char *path)          { return __syscall(SYS_UNLINK, (int)path, 0); }
int spawn(const char *path)           { return __syscall(SYS_SPAWN, (int)path, 0); }
int wait(int pid)                     { return __syscall(SYS_WAIT, pid, 0); }
int freeblocks(void)                  { return __syscall(SYS_FREEBLK, 0, 0); }

/* console output helpers (the rt printf drives TTO directly, which faults
 * under I/O protection) */
int ustrlen(const char *s) {
    int n = 0;
    while (*s) { s++; n++; }
    return n;
}
void uputs(const char *s) { write(1, s, ustrlen(s)); }
void uputu(unsigned int v) {
    char b[6];
    int n = 0;
    do { b[5 - n] = (char)('0' + v % 10); n++; v /= 10; } while (v != 0);
    write(1, b + 6 - n, n);
}
void uputi(int v) {
    if (v < 0) { uputs("-"); uputu((unsigned int)(-v)); }
    else uputu((unsigned int)v);
}
