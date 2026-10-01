#ifndef _TAPE_H
#define _TAPE_H
/* tape.h -- polled magtape driver and SYS_TAPE (kernel/tape.c). */
#define TP_REWIND 1
#define TP_READ   2     /* -> bytes read, 0 at a tape mark */
#define TP_WRITE  3     /* -> bytes written */
#define TP_WEOF   4     /* write a tape mark */
#define TP_STATUS 5     /* -> DIA status 1 */
extern unsigned int mta_buf[256];
int mta_rewind(unsigned int unit);
int mta_read(unsigned int unit);            /* into mta_buf: words, 0 = mark */
int mta_write(unsigned int unit, unsigned int words);
int mta_weof(unsigned int unit);
unsigned int mta_status(unsigned int unit);
unsigned int sys_tape(unsigned int a1, unsigned int ubuf, unsigned int n);
#endif
