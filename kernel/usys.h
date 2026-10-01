#ifndef _USYS_H
#define _USYS_H

/* usys.h -- user-mode syscall wrappers for the N-process kernel. Same
 * SYC-issuing technique as examples/ulib.c; see kernel/ksyscall.h for
 * the ABI. */

int sys_putchar(int c);
int sys_getchar(void);
void sys_exit(int status);
int sys_getpid(void);
int sys_write(const char *buf, int len);
int sys_segalloc(int npages);
int sys_mapwin(int winpage, int segpage, int npages);
int sys_unmapwin(int winpage, int npages);

/* files and processes (FS_NOTES.md) */
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0100
#define O_TRUNC  01000
int open(const char *path, int flags);
int close(int fd);
int read(int fd, char *buf, int n);
int write(int fd, const char *buf, int n);
int lseek(int fd, int off, int whence);
int mkdir(const char *path);
int unlink(const char *path);
int spawn(const char *path);
int wait(int pid);
int freeblocks(void);
int ustrlen(const char *s);
void uputs(const char *s);
void uputu(unsigned int v);
void uputi(int v);

/* shell support (SHELL_NOTES.md) */
int spawnv(const char *path, char **argv);
int chdir(const char *path);
int dup(int fd);
int dup2(int fd, int nfd);
int pipe(int *fds);
int stat(const char *path, unsigned int *st);  /* inum, type (1 file, 2 dir), nlink, size */
int ustrcmp(const char *a, const char *b);
void uputsfd(int fd, const char *s);

/* magtape: tape(TP_*, unit, buf, nbytes) -- one record of at most 512
 * bytes per call; TP_READ returns 0 at a tape mark */
#define TP_REWIND 1
#define TP_READ   2
#define TP_WRITE  3
#define TP_WEOF   4
#define TP_STATUS 5
int tape(int op, int unit, char *buf, int n);

/* text and system tools (ksyscall.h SYS_RENAME..SYS_KILL) */
int rename(const char *from, const char *to);
int psinfo(int pid, unsigned int *w);   /* w[9]: state parent pages name[6] */
int kill(int pid);
int fsinfo(int what);   /* 0 free blocks, 1 data blocks, 2 free inodes, 3 inodes */

#endif
