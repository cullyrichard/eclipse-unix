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

#define MTA_REWIND 010          /* CU_REWIND << 3, unit 0 */
#define MTA_READ   0            /* CU_READ << 3, unit 0 */
#define ST1_EOF    0400
#define ST1_RDY    01
#define ST1_REW    020000

static unsigned int tbuf[256];

static unsigned int mta_st1(void) {
    unsigned int s;
    asm volatile("DIA %0,022" : "=r"(s));
    return s;
}

static unsigned int mta_done(void) {
    unsigned int r;
    asm volatile("SUB %0,%0\n\tSKPDZ 022\n\tINC %0,%0" : "=r"(r));
    return r;
}

static void mta_rewind(void) {
    asm volatile("DOA %0,022" :: "r"((unsigned int)MTA_REWIND));
    asm volatile("NIOS 022");
    while ((mta_st1() & ST1_REW) || !(mta_st1() & ST1_RDY))
        ;
}

/* Read the next record into tbuf (at most 256 words). Returns the number
 * of words transferred, 0 for a tape mark, -1 for an error. */
static int mta_read(void) {
    unsigned int wa, st, ma;
    wa = (unsigned int)(unsigned long)tbuf >> 1;
    asm volatile("DOB %0,022" :: "r"(wa));
    asm volatile("DOC %0,022" :: "r"((unsigned int)0177400));   /* -256 */
    asm volatile("DOA %0,022" :: "r"((unsigned int)MTA_READ));
    asm volatile("NIOS 022");
    while (!mta_done())
        ;
    st = mta_st1();
    asm volatile("DIB %0,022" : "=r"(ma));
    asm volatile("NIOC 022");
    if (st & ST1_EOF) return 0;
    if (st & 0100000) return -1;            /* error summary bit */
    return (int)(ma - wa);
}

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
    mta_rewind();
    skipped = 0;                             /* file 1: bootstrap + kernel */
    for (;;) {
        n = mta_read();
        if (n == 0) break;
        if (n < 0) { kputs("kernel: tape read error in file 1\r\n"); return -1; }
        skipped++;
    }
    kputs("kernel: skipped ");
    kputu(skipped);
    kputs(" boot records; copying filesystem");
    for (blk = 0;; blk++) {
        n = mta_read();
        if (n == 0) break;
        if (n != 256) {
            kputs("\r\nkernel: bad filesystem record ");
            kputu(blk);
            kputs("\r\n");
            return -1;
        }
        if (blk >= bdev_nblocks() || bdev_write(blk, tbuf) != 0) {
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
