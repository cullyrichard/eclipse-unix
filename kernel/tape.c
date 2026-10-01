/* tape.c -- polled magtape (MTA, device 022) driver, used by the boot-time
 * installer (tapeinst.c) and by the SYS_TAPE syscall (user tar, SHELL/FS
 * notes). Programming per SIMH nova_mta.c:
 *   DOA  command<<3 | unit     (0 read, 1 rewind, 5 write, 6 write EOF)
 *   DOB  memory word address   (DMA; advances as words move)
 *   DOC  two's-complement word count (-256 = 0177400)
 *   NIOS start; Done when finished; DIA status 1, DIB memory address.
 * Status 1: 0100000 error, 0400 end of file (tape mark), 04 write lock,
 * 020000 rewinding, 01 ready.
 * Polled with tty_poll() in the waits (the kernel's MTA interrupt stays
 * masked, proc.c k_pimask). Records are at most 256 words (512 bytes). */
#include "tape.h"
#include "tty.h"
#include "kmem.h"
#include "fsys.h"

#define CMD_READ   0
#define CMD_REWIND 010
#define CMD_WRITE  050
#define CMD_WEOF   060
#define ST1_ERR    0100000
#define ST1_REW    020000
#define ST1_EOF    0400
#define ST1_WLK    04
#define ST1_RDY    01

unsigned int mta_buf[256];

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

static void mta_select(unsigned int cu) {
    asm volatile("DOA %0,022" :: "r"(cu));
}

/* wait until the selected unit is ready (not rewinding); 0 = ready, -1 =
 * not ready (no tape mounted / offline) after a long wait */
static int mta_ready(unsigned int unit) {
    unsigned int s, n;
    mta_select(unit & 7);
    for (n = 0; n < 60000; n++) {
        s = mta_st1();
        if (!(s & ST1_REW) && (s & ST1_RDY)) return 0;
        if ((n & 15) == 0) tty_poll();
    }
    return -1;
}

int mta_rewind(unsigned int unit) {
    if (mta_ready(unit) < 0) return -1;
    mta_select(CMD_REWIND | (unit & 7));
    asm volatile("NIOS 022");
    return mta_ready(unit);
}

/* one transfer of `words` words between mta_buf and the tape; returns the
 * words moved, 0 at a tape mark (read), -1 on error */
static int mta_xfer(unsigned int unit, unsigned int cmd, unsigned int words) {
    unsigned int wa, st, ma, cnt;
    if (mta_ready(unit) < 0) return -1;
    wa = (unsigned int)(unsigned long)mta_buf >> 1;
    cnt = (unsigned int)(0 - words);
    asm volatile("DOB %0,022" :: "r"(wa));
    asm volatile("DOC %0,022" :: "r"(cnt));
    mta_select(cmd | (unit & 7));
    asm volatile("NIOS 022");
    while (!mta_done()) tty_poll();
    st = mta_st1();
    asm volatile("DIB %0,022" : "=r"(ma));
    asm volatile("NIOC 022");
    if (cmd == CMD_READ && (st & ST1_EOF)) return 0;
    if (st & ST1_ERR) return -1;
    return (int)(ma - wa);
}

int mta_read(unsigned int unit) { return mta_xfer(unit, CMD_READ, 256); }
int mta_write(unsigned int unit, unsigned int words) {
    if (words == 0 || words > 256) return -1;
    return mta_xfer(unit, CMD_WRITE, words);
}

int mta_weof(unsigned int unit) {
    unsigned int st;
    if (mta_ready(unit) < 0) return -1;
    mta_select(CMD_WEOF | (unit & 7));
    asm volatile("NIOS 022");
    while (!mta_done()) tty_poll();
    st = mta_st1();
    asm volatile("NIOC 022");
    return (st & ST1_ERR) ? -1 : 0;
}

unsigned int mta_status(unsigned int unit) {
    mta_select(unit & 7);
    return mta_st1();
}

/* SYS_TAPE: a1 = op | unit << 8, a2 = user buffer (even byte address),
 * a3 = byte count (even, <= 512). */
unsigned int sys_tape(unsigned int a1, unsigned int ubuf, unsigned int n) {
    unsigned int op = a1 & 0377, unit = (a1 >> 8) & 7, i;
    int r;
    if (op == TP_REWIND) return mta_rewind(unit) < 0 ? 0177777 : 0;
    if (op == TP_WEOF) return mta_weof(unit) < 0 ? 0177777 : 0;
    if (op == TP_STATUS) return mta_status(unit);
    if (op != TP_READ && op != TP_WRITE) return 0177777;
    if ((ubuf & 1) || (n & 1) || n > 512 || !uvalid(ubuf, n ? n : 1))
        return 0177777;
    if (op == TP_WRITE) {
        if (n == 0) return 0177777;
        for (i = 0; i < n / 2; i++) mta_buf[i] = kcopyin_word((ubuf >> 1) + i);
        r = mta_write(unit, n / 2);
        return r < 0 ? 0177777 : (unsigned int)(2 * r);
    }
    r = mta_read(unit);
    if (r <= 0) return r < 0 ? 0177777 : 0;
    if ((unsigned int)(2 * r) > n) return 0177777;   /* record bigger than buf */
    for (i = 0; i < (unsigned int)r; i++) kcopyout_word((ubuf >> 1) + i, mta_buf[i]);
    return (unsigned int)(2 * r);
}
