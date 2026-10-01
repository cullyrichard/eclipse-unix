/* bdev_dsk.c -- block-device backend: interrupt-driven DSK transfers
 * (IO_NOTES.md; interface in bdev.h).
 *
 * Inside a process's syscall, a transfer is started (DOA block, DOB word
 * address, NIOS = read / NIOP = write -- the same programming sequence as
 * examples/dsk.c, STORAGE_NOTES.md) and the process sleeps until DSK's
 * completion interrupt; other processes run meanwhile. The controller
 * does one transfer at a time, so a second requester sleeps until it is
 * free. Outside a syscall (boot: fs_init, spawning /bin/init) there is no
 * process to put to sleep, so it falls back to examples/dsk.c's polling.
 *
 * The buffer handed to the controller is owned by the sleeping caller
 * until the interrupt: fs.c only calls this with the filesystem lock
 * held (kernel/proc.c), so no other process can pick that buffer-cache
 * slot while the DMA is outstanding. */
#include "bdev.h"
#include "ksync.h"
#include "../examples/dsk.h"

static unsigned int dsk_busy, dsk_done, dsk_status;
unsigned int bdev_nintr, bdev_nsleep;        /* statistics */
unsigned int bdev_pimask = 0;                /* DSK interrupts are used */
unsigned int bdev_devcode = 020;

int bdev_init(void) { return 0; }
unsigned int bdev_nblocks(void) { return DSK_MAX_BLOCK + 1; }
const char *bdev_name(void) { return "dsk (DG 4019)"; }

void bdev_intr(void) {
    unsigned int st;
    bdev_nintr++;
    asm volatile("DIA %0,020" : "=r"(st));
    asm volatile("NIOC 020");                /* clear Done: drops the request */
    if (dsk_busy) {
        dsk_status = st;
        dsk_done = 1;
        wakeup(CHAN_DSK);
    }
}

static int kdsk_io(unsigned int blk, unsigned int *buf, int wr) {
    unsigned int wa;
    if (blk > DSK_MAX_BLOCK) return -1;
    if (!kcansleep())
        return wr ? dsk_write_block(blk, buf) : dsk_read_block(blk, buf);
    while (dsk_busy)
        ksleep(CHAN_DSKFREE);
    dsk_busy = 1;
    dsk_done = 0;
    wa = (unsigned int)(unsigned long)buf >> 1;  /* DMA wants a word address */
    asm volatile("DOA %0,020" :: "r"(blk));
    asm volatile("DOB %0,020" :: "r"(wa));
    if (wr) asm volatile("NIOP 020");
    else asm volatile("NIOS 020");
    while (!dsk_done) {
        bdev_nsleep++;
        ksleep(CHAN_DSK);
    }
    dsk_busy = 0;
    wakeup(CHAN_DSKFREE);
    return (int)dsk_status;
}

int bdev_read(unsigned int blk, unsigned int *buf)  { return kdsk_io(blk, buf, 0); }
int bdev_write(unsigned int blk, unsigned int *buf) { return kdsk_io(blk, buf, 1); }
