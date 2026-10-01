/* bdev_ram.c -- block-device backend: a RAM disk in physical memory above
 * the kernel's own 32KW, for running with no disk at all (FS_NOTES.md
 * section 9). Interface: bdev.h.
 *
 * RAM_BLOCKS 512-byte blocks (default 1024 = 256 frames = 256KW, a
 * quarter of a 1MW S/140) are taken from the page allocator at boot and
 * reached through user map B with single-cycle mapped accesses
 * (kmem.c kfar_bget/kfar_bput: the map is loaded once per 64-word chunk).
 * Four blocks per 1KW frame.
 *
 * Volatile: real memory powers up with random contents, so the superblock
 * block is zeroed at init (fs_init then finds no filesystem) and the
 * kernel loads the filesystem from the boot/install tape without asking
 * (tapeinst.c: there is nothing on a RAM disk to overwrite). Everything is
 * lost at power-off. */
#include "bdev.h"
#include "kmem.h"
#include "tty.h"
#include "../examples/pmm.h"

#ifndef RAM_BLOCKS
#define RAM_BLOCKS 1024
#endif
#define RAM_FRAMES ((RAM_BLOCKS + 3) / 4)

unsigned int bdev_pimask = 0;
unsigned int bdev_devcode = 0;
unsigned int bdev_volatile = 1;
unsigned int bdev_nintr, bdev_nsleep;

static unsigned int ram_pfn[RAM_FRAMES];
static unsigned int zero64[64];

int bdev_init(void) {
    unsigned int i;
    int pfn;
    for (i = 0; i < RAM_FRAMES; i++) {
        pfn = pmm_alloc_page();
        if (pfn == PMM_NONE) return -1;          /* not enough memory */
        ram_pfn[i] = (unsigned int)pfn;
    }
    for (i = 0; i < 256; i += 64)                /* block 1: superblock */
        kfar_bput(ram_pfn[0], 256 + i, zero64, 64);
    return 0;
}

unsigned int bdev_nblocks(void) { return RAM_BLOCKS; }
const char *bdev_name(void) { return "ram (physical memory, volatile)"; }
void bdev_intr(void) { }

int bdev_read(unsigned int blk, unsigned int *buf) {
    unsigned int pfn, off, c;
    if (blk >= RAM_BLOCKS) return -1;
    pfn = ram_pfn[blk >> 2];
    off = (blk & 3) * 256;
    for (c = 0; c < 256; c += 64) {
        kfar_bget(pfn, off + c, buf + c, 64);
        tty_poll();
    }
    return 0;
}

int bdev_write(unsigned int blk, unsigned int *buf) {
    unsigned int pfn, off, c;
    if (blk >= RAM_BLOCKS) return -1;
    pfn = ram_pfn[blk >> 2];
    off = (blk & 3) * 256;
    for (c = 0; c < 256; c += 64) {
        kfar_bput(pfn, off + c, buf + c, 64);
        tty_poll();
    }
    return 0;
}
