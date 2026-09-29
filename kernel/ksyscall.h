#ifndef _KSYSCALL_H
#define _KSYSCALL_H

/* ksyscall.h -- syscall numbers shared by the N-process kernel (kernel/)
 * and its user-side library (kernel/usys.c). See KERNEL_NOTES.md.
 *
 * ABI (unchanged from examples/syscall.h / SYSCALL_NOTES.md): AC0 =
 * number, AC1 = arg1, AC2 = arg2, `SYC 1,1`; result comes back in AC0.
 * 0-3 keep examples/syscall.h's numbering.
 *
 * Pointers are plain word addresses on this backend. SYS_WRITE's buffer is
 * "one character per word, starting at logical word address arg1" in the
 * calling process's own address space. A string literal is laid out that
 * way (dgasm stores one character per word); an `int` array filled with
 * characters is too. Local and zero-initialized `char` arrays are NOT
 * reliable on this backend yet -- see KERNEL_NOTES.md, "char arrays".
 */

#define SYS_PUTCHAR   0   /* arg1 = character. returns 0. */
#define SYS_GETCHAR   1   /* returns the character read. */
#define SYS_EXIT      4   /* arg1 = status. does not return. */
#define SYS_GETPID    5   /* returns the caller's pid. */
#define SYS_WRITE     6   /* arg1 = user buffer, arg2 = length. returns the
                             count written, or -1 if any word of the
                             buffer is outside the caller's mapped pages. */

/* Big memory beyond a process's 32KW (KERNEL_NOTES.md section 9). */
#define SYS_SEGALLOC  7   /* arg1 = pages to add to the caller's segment.
                             returns the new segment size in pages, or -1
                             (limit MAXSEG=128 pages, or out of frames;
                             nothing is allocated on failure). */
#define SYS_MAPWIN    8   /* arg1 = window first logical page (bits 0-4)
                             | page count << 5; arg2 = first segment page,
                             or -1 to unmap. returns 0, or -1 if any window
                             page is an image/stack page, the window runs
                             past page 31, or the segment range doesn't
                             exist (nothing is changed on failure). */

#endif
