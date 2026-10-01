/* bdev_dskp.c -- block-device backend for the DG/Disc ("DSKP", device 027)
 * family: exactly one of DSKP_VARIANT_ZEBRA / _VULCAN / _KISMET is defined
 * by kernel/build.sh (KDISK=zebra|vulcan|kismet), selecting the driver in
 * examples/{zebra,vulcan,kismet}.c (DSKP_FAMILY_NOTES.md). Interface:
 * bdev.h.
 *
 * NOT EXECUTED ANYWHERE YET: SIMH models none of these controllers, so
 * this compiles and links but has only been run on nothing. It needs a
 * test on the real drives before it is trusted (FS_NOTES.md).
 *
 * Polled, unit 0, 512-byte sectors; the filesystem uses a 32 MB
 * partition at the start of the drive (16-bit block numbers). For
 * Kismet, KISMET_HEADS selects the model (default 6160; pass
 * -DKISMET_HEADS=KISMET_HEADS_6161 or _6214 via KCFLAGS). */
#include "bdev.h"
#if defined(DSKP_VARIANT_ZEBRA)
#include "../examples/zebra.h"
#define DRV_READ(b, p)  zebra_read_block(0, (unsigned long)(b), (p))
#define DRV_WRITE(b, p) zebra_write_block(0, (unsigned long)(b), (p))
#define DRV_NAME "zebra (DG 6060/6061/6067, partition 0)"
#elif defined(DSKP_VARIANT_VULCAN)
#include "../examples/vulcan.h"
#define DRV_READ(b, p)  vulcan_read_block(0, (unsigned long)(b), (p))
#define DRV_WRITE(b, p) vulcan_write_block(0, (unsigned long)(b), (p))
#define DRV_NAME "vulcan (DG 6122, partition 0)"
#elif defined(DSKP_VARIANT_KISMET)
#include "../examples/kismet.h"
#ifndef KISMET_HEADS
#define KISMET_HEADS KISMET_HEADS_6160
#endif
#define DRV_READ(b, p)  kismet_read_block(0, KISMET_HEADS, (b), (p))
#define DRV_WRITE(b, p) kismet_write_block(0, KISMET_HEADS, (b), (p))
#define DRV_NAME "kismet (DG 6160/6161/6214, partition 0)"
#else
#error "bdev_dskp.c: build with exactly one DSKP_VARIANT_* (kernel/build.sh KDISK)"
#endif

unsigned int bdev_pimask = 0400;    /* DSKP priority mask bit 7: polled */
unsigned int bdev_devcode = 0;
unsigned int bdev_volatile = 0;
unsigned int bdev_nintr, bdev_nsleep;

int bdev_init(void) { return 0; }
unsigned int bdev_nblocks(void) { return 65535u; }
const char *bdev_name(void) { return DRV_NAME; }
void bdev_intr(void) { }

int bdev_read(unsigned int blk, unsigned int *buf) {
    return DRV_READ(blk, buf) == 0 ? 0 : -1;
}
int bdev_write(unsigned int blk, unsigned int *buf) {
    return DRV_WRITE(blk, buf) == 0 ? 0 : -1;
}
