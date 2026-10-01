/* bdev_dkp.c -- block-device backend for the DKP moving-head controller
 * (device 033), driving a DG 4231 (3330-class, about 92 MB): 23 sectors x
 * 19 surfaces x 411 cylinders of 256 words. SIMH: `set dkp0 4231`.
 * Interface: bdev.h. Register layout: nova_dkp.c ("new format" drives):
 *   DOC  unit<<14 | surface<<9 | sector<<4 | (16 - count)
 *   DOA  flags 0174000 (clear errors) | cmd<<9 | cylinder (9 bits)
 *        cmd 0 READ, 1 WRITE, 2 SEEK; NIOP starts a seek, NIOS a transfer
 *   DIA  status: 0100000 done, 040000 unit 0 seek done, low bits errors
 * The sequence follows SIMH's own DKP boot ROM (seek, wait seek done,
 * DOB memory address, start, wait done).
 *
 * Polled: the CPU waits (taking keystrokes via tty_poll), so this backend
 * masks its own completion interrupt (bdev_pimask). Linear block b maps
 * to cylinder b/437, surface (b%437)/23, sector b%23, matching SIMH's
 * GET_SA, so a flat image file is the disk in block order.
 *
 * The filesystem addresses at most 65535 blocks (16-bit block numbers),
 * so it occupies a 32 MB partition at the start of the 179,607-block
 * drive; the rest is unused for now (FS_NOTES.md). */
#include "bdev.h"
#include "tty.h"

#define DKP_SECT   23u
#define DKP_SURF   19u
#define DKP_CYL    411u
#define DKP_PERCYL (DKP_SECT * DKP_SURF)        /* 437 */

#define FCCY_CLR   0174000u
#define CMD_READ   0u
#define CMD_WRITE  01000u
#define CMD_SEEK   02000u
#define STA_DONE   0100000u
#define STA_SKDN0  040000u
#define STA_EFLGS  077u         /* err, data late, crc, unsafe, xcyl, nxcyl */

unsigned int bdev_pimask = 0400;    /* PI_DKP: polled */
unsigned int bdev_devcode = 0;
unsigned int bdev_volatile = 0;
unsigned int bdev_nintr, bdev_nsleep;

int bdev_init(void) { return 0; }
unsigned int bdev_nblocks(void) { return 65535u; }   /* partition 0 */
const char *bdev_name(void) { return "dkp (DG 4231, partition 0)"; }
void bdev_intr(void) { }

static unsigned int dkp_sta(void) {
    unsigned int s;
    asm volatile("DIA %0,033" : "=r"(s) : : "memory");
    return s;
}

static int dkp_xfer(unsigned int blk, unsigned int *buf, unsigned int cmd) {
    unsigned int cyl, rem, ussc, fccy, st;
    if (blk >= bdev_nblocks()) return -1;
    cyl = blk / DKP_PERCYL;
    rem = blk % DKP_PERCYL;
    ussc = ((rem / DKP_SECT) << 9) | ((rem % DKP_SECT) << 4) | 017; /* 1 sector */
    asm volatile("NIOC 033" ::: "memory");
    asm volatile("DOC %0,033" :: "r"(ussc) : "memory");
    fccy = FCCY_CLR | CMD_SEEK | cyl;
    asm volatile("DOA %0,033" :: "r"(fccy) : "memory");
    asm volatile("NIOP 033" ::: "memory");
    while ((dkp_sta() & STA_SKDN0) == 0) tty_poll();
    asm volatile("NIOC 033" ::: "memory");
    asm volatile("DOB %0,033" :: "r"((unsigned int)(unsigned long)buf >> 1) : "memory");
    asm volatile("DOC %0,033" :: "r"(ussc) : "memory");
    fccy = FCCY_CLR | cmd | cyl;
    asm volatile("DOA %0,033" :: "r"(fccy) : "memory");
    asm volatile("NIOS 033" ::: "memory");
    while (((st = dkp_sta()) & STA_DONE) == 0) tty_poll();
    asm volatile("NIOC 033" ::: "memory");
    return (st & STA_EFLGS) ? -1 : 0;
}

int bdev_read(unsigned int blk, unsigned int *buf)  { return dkp_xfer(blk, buf, CMD_READ); }
int bdev_write(unsigned int blk, unsigned int *buf) { return dkp_xfer(blk, buf, CMD_WRITE); }
