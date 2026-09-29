#ifndef _KMEM_H
#define _KMEM_H

/* Kernel access to physical memory beyond the kernel's own 32KW, and to
 * the current process's address space. See kernel/kmem.c. */

void kfar_write(unsigned int pfn, unsigned int off, unsigned int val);
unsigned int kfar_read(unsigned int pfn, unsigned int off);
unsigned int kcopyin_word(unsigned int uaddr);

#endif
