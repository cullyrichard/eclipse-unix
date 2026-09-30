#ifndef _FSYS_H
#define _FSYS_H

/* fsys.h -- descriptors and file syscalls (kernel/fsys.c). */

#define NOFILE     8                /* descriptors per process */
#define FS_MAXPATH 64

#define O_RDONLY   0
#define O_WRONLY   1
#define O_RDWR     2
#define O_CREAT    0100
#define O_TRUNC    01000

int uvalid(unsigned int b, unsigned int len);
int ucopystr(unsigned int ub, char *k, unsigned int max);

int fsys_isconsole(unsigned int fd);
void fsys_procinit(int n);
void fsys_procexit(int n);
void fsys_inherit(int child, int parent);
unsigned int fsys_cwd(int n);
int fsys_needslock(unsigned int no, unsigned int a1);

unsigned int sys_open(unsigned int upath, unsigned int flags);
unsigned int sys_close(unsigned int fd);
unsigned int sys_read(unsigned int fd, unsigned int ubuf, unsigned int n);
unsigned int sys_writefd(unsigned int fd, unsigned int ubuf, unsigned int n);
unsigned int sys_lseek(unsigned int fd, unsigned int off, unsigned int whence);
unsigned int sys_mkdir(unsigned int upath);
unsigned int sys_unlink(unsigned int upath);
unsigned int sys_chdir(unsigned int upath);
unsigned int sys_dup(unsigned int fd);
unsigned int sys_dup2(unsigned int fd, unsigned int nfd);
unsigned int sys_pipe(unsigned int ufds);
unsigned int sys_stat(unsigned int upath, unsigned int ust);
unsigned int fsys_syscall(unsigned int no, unsigned int a1, unsigned int a2,
                          unsigned int a3);

#endif
