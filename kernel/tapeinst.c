/* tapeinst.c -- install a filesystem from magtape when the disk has none
 * (FS_NOTES.md section 7). This is how a real machine's disk gets its
 * first filesystem: boot an install tape built by
 * fstools/mkinstalltape.py, laid out as
 *   file 1: boot record + kernel records    (read by the tape bootstrap)
 *   file 2: the filesystem, one 256-word (512-byte) record per block,
 *           from block 0
 *   two tape marks
 * The kernel finds no valid superblock, asks on the console, then copies
 * file 2 to the disk block by block (bdev_write) and mounts it.
 *
 * Polled MTA (device 022) on unit 0, before k_start (interrupts off).
 * Programming per SIMH nova_mta.c: DOA command<<3 | unit, DOB memory
 * address, DOC two's-complement word count, NIOS start; DIA status 1
 * (0400 = end of file / tape mark, 1 = ready, 020000 = rewinding).
 * DIB reads back the memory address, so a record's length is how far the
 * DMA advanced it. */
#include "tapeinst.h"
#include "bdev.h"
#include "kio.h"
#include "fs.h"
#include "../examples/console.h"

#include "tape.h"

int tape_install(void) {
    int n, c;
    unsigned int blk, skipped;
    if (bdev_volatile) {
        /* RAM disk: nothing to overwrite, no question to ask */
        kputs("kernel: loading the filesystem into memory from tape unit 0\r\n");
    } else {
        kputs("kernel: no filesystem on the disk. Install one from tape unit 0? [y/N] ");
        c = console_getchar() & 0177;
        console_putchar(c);
        kputs("\r\n");
        if (c != 'y' && c != 'Y') return -1;
    }
    mta_rewind(0);
    skipped = 0;                             /* file 1: bootstrap + kernel */
    for (;;) {
        n = mta_read(0);
        if (n == 0) break;
        if (n < 0) { kputs("kernel: tape read error in file 1\r\n"); return -1; }
        skipped++;
    }
    kputs("kernel: skipped ");
    kputu(skipped);
    kputs(" boot records; copying filesystem");
    for (blk = 0;; blk++) {
        n = mta_read(0);
        if (n == 0) break;
        if (n != 256) {
            kputs("\r\nkernel: bad filesystem record ");
            kputu(blk);
            kputs("\r\n");
            return -1;
        }
        if (blk >= bdev_nblocks() || bdev_write(blk, mta_buf) != 0) {
            kputs("\r\nkernel: disk write failed at block ");
            kputu(blk);
            kputs("\r\n");
            return -1;
        }
        if ((blk & 63) == 0) console_putchar('.');
    }
    kputs("\r\nkernel: installed ");
    kputu(blk);
    kputs(" blocks\r\n");
    return 0;
}
