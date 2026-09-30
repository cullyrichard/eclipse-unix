#ifndef _KDSK_H
#define _KDSK_H
/* kdsk.h -- interrupt-driven DSK transfers (kernel/kdsk.c). */
int kdsk_read(unsigned int blk, unsigned int *buf);
int kdsk_write(unsigned int blk, unsigned int *buf);
void kdsk_intr(void);
extern unsigned int dsk_nintr, dsk_nsleep;
#endif
