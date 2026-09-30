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

#endif
