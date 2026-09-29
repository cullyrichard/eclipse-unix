#ifndef _KPROC_H
#define _KPROC_H

/* N-process table for the kernel in kernel/. See KERNEL_NOTES.md.
 *
 * Parallel global arrays, not a struct: this backend still has an open
 * defect where a struct field reached through a pointer, other than the
 * first or last, gets different load vs store addresses
 * (BLOCKDEV_NOTES.md, DEBUGGING_NOTES.md #15). */

#define NPROC   8
#define NPAGES  32          /* logical pages per address space (32 x 1KW) */
#define MAXSEG  128         /* max extra segment pages per process (128KW) */

#define P_FREE     0
#define P_RUN      1        /* runnable (round-robin candidate) */
#define P_ZOMBIE   2        /* exited or killed; frames already freed */

/* Map-status word every process runs with (DOA MAP, MMPU_NOTES.md):
 *   0001 user mode on, A/B = A (process always lives in user map A)
 *   0040 I/O protection: any user I/O instruction (HALT, NIOP MAP, DOA...)
 *        faults to vector 3. REQUIRED for isolation: without it, NIOP MAP
 *        from user mode silently turns the MMPU off (manual's NIOP entry,
 *        MMPU_NOTES.md) -- i.e. any process could drop to supervisor. */
#define P_MAPSTAT  041

/* A PTE word as LMP loads it: bits 10-14 = logical slot, 0-9 = physical
 * page, 0100000 = write-protect. eclipse_cpu.c treats the stored value
 * 0101777 (WP + physical page 01777) as INVALID: any mapped (non-single-
 * cycle) access to such a page takes a validity fault. Consequence:
 * physical page 1023 can never be mapped write-protected. */
#define PTE_INVALID  0101777

/* A loadable user image: words[i] belongs at logical address base+i;
 * stacktop is the program's _STACKTOP (the hardware stack grows UP from
 * it); stackwords of stack are mapped above it. */
int proc_create(const unsigned int *words, unsigned int nwords,
                unsigned int base, unsigned int stacktop,
                unsigned int stackwords);
void proc_dump(int n);      /* print logical page -> physical frame map */
void k_start(void);         /* never returns */

#endif
