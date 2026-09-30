#ifndef _KSYSCALL_H
#define _KSYSCALL_H

/* ksyscall.h -- syscall numbers shared by the N-process kernel (kernel/)
 * and its user-side library (kernel/usys.c). See KERNEL_NOTES.md.
 *
 * ABI (unchanged from examples/syscall.h / SYSCALL_NOTES.md): AC0 =
 * number, AC1 = arg1, AC2 = arg2, `SYC 1,1`; result comes back in AC0.
 * 0-3 keep examples/syscall.h's numbering.
 *
 * Pointer values are BYTE addresses (DEBUGGING_NOTES.md #38): chars are
 * packed two per word, high byte first, exactly as LDB/STB address them.
 * SYS_WRITE's arg1 is such a byte address in the caller's address space,
 * so any ordinary `char` buffer or string literal can be passed.
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

/* Files and processes (FS_NOTES.md). arg3 is passed in AC3. */
#define SYS_OPEN      9   /* path, flags (O_* in fsys.h) -> fd or -1 */
#define SYS_CLOSE    10   /* fd */
#define SYS_READ     11   /* fd, buf, n -> bytes read (0 = EOF) or -1 */
#define SYS_WRITEFD  12   /* fd, buf, n -> bytes written or -1 */
#define SYS_LSEEK    13   /* fd, off, whence(0 set,1 cur,2 end) -> new off */
#define SYS_MKDIR    14   /* path */
#define SYS_UNLINK   15   /* path (not while open; dirs must be empty) */
#define SYS_SPAWN    16   /* path -> pid of a new child running that file */
#define SYS_WAIT     17   /* pid (a child) -> its exit status; blocks */
#define SYS_FREEBLK  18   /* -> free data blocks on the filesystem */

#endif
