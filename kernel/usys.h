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

#endif
