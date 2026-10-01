#ifndef _BDEV_H
#define _BDEV_H

/* bdev.h -- the kernel's block device (BDEV_NOTES.md section in
 * FS_NOTES.md). fs.c talks only to this interface; exactly one backend
 * is linked into a given kernel, chosen by kernel/build.sh's KDISK:
 *
 *   dsk     bdev_dsk.c   DG 4019 fixed-head (SIMH DSK, device 020).
 *                        Interrupt-driven: the caller sleeps.
 *   dkp     bdev_dkp.c   DG 4047 / Diablo 31 moving-head (SIMH DKP,
 *                        device 033). Polled, with seek + CHS translation.
 *   zebra   bdev_dskp.c  DG 6060/6061/6067 DG/Disc (device 027). Polled.
 *   vulcan  bdev_dskp.c  DG 6122 DG/Disc (device 027). Polled.
 *   kismet  bdev_dskp.c  DG 6160/6161/6214 DG/Disk (device 027). Polled.
 *
 * Compile-time selection, not an ops table: DSKP_FAMILY_NOTES.md allows
 * one DSKP generation per build, and a struct of function pointers would
 * reach the open struct-field defect (DEBUGGING_NOTES.md #15).
 *
 * A block is 512 bytes (256 words) on every backend: DSK and DKP sectors
 * are 256 words, and the DG/Disc subsystems use 512-byte sectors
 * (VULCAN_NOTES.md capacity check). Block numbers are linear. */

int bdev_init(void);                    /* 0 = ready */
int bdev_read(unsigned int blk, unsigned int *buf);   /* 0 = ok */
int bdev_write(unsigned int blk, unsigned int *buf);
unsigned int bdev_nblocks(void);        /* blocks this backend addresses */
const char *bdev_name(void);
void bdev_intr(void);                   /* BDEV_DEVCODE interrupt */

/* MSKO bits this backend needs masked (polled backends mask their own
 * completion interrupt), and the device code bdev_intr() serves (0: none). */
extern unsigned int bdev_pimask, bdev_devcode;
extern unsigned int bdev_nintr, bdev_nsleep;   /* statistics */

#endif
