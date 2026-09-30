#ifndef _KSYNC_H
#define _KSYNC_H

/* ksync.h -- sleep/wakeup for the kernel (IO_NOTES.md).
 *
 * ksleep() may only be called from a process's syscall path (curproc >= 0,
 * running on its own kernel stack, ION off). It saves the kernel context
 * and runs something else; it returns once wakeup(chan) has made the
 * process runnable and the scheduler has switched back to it. Because
 * all kernel code runs with ION off, "test a condition, then ksleep" is
 * atomic with respect to interrupt handlers: no wakeup can be lost
 * between the test and the sleep. Always re-test after ksleep returns. */

#define CHAN_TTY     1      /* a line (or buffer-full) is ready */
#define CHAN_DSK     2      /* the disk transfer in flight finished */
#define CHAN_DSKFREE 3      /* the disk controller became free */
#define CHAN_FSLOCK  4      /* the filesystem lock was released */

void ksleep(unsigned int chan);
void wakeup(unsigned int chan);
int kcansleep(void);        /* 1 if inside a process's syscall */

#endif
