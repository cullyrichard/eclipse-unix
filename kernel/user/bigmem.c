/* bigmem.c -- one process using far more than 32KW (KERNEL_NOTES.md
 * section 9). Allocates SEGPAGES pages (96KW) of segment memory, then
 * walks it through an 8-page (8KW) window at logical pages 16-23
 * (040000-057777) with SYS_MAPWIN: fill every word with a pattern that
 * depends on its segment page, offset and pid, then re-verify the whole
 * 96KW ROUNDS times while other processes preempt it. Also checks that
 * bad SYS_SEGALLOC/SYS_MAPWIN arguments fail cleanly. */
#include "../usys.h"

#ifndef BIGROUNDS
#define BIGROUNDS 3
#endif
#define WIN 16
#define WPAGES 8
#define SEGPAGES 96

static void put_u(unsigned int v) {
    int buf[6];         /* int, not char: KERNEL_NOTES.md */
    int n = 0;
    do { buf[n] = '0' + v % 10; n++; v = v / 10; } while (v != 0);
    while (n > 0) { n--; sys_putchar(buf[n]); }
}

static void put_i(int v) {
    if (v < 0) { sys_putchar('-'); v = -v; }
    put_u((unsigned int)v);
}

static unsigned int pat(unsigned int sp, unsigned int off, unsigned int pid) {
    return sp * 257 + off * 3 + pid * 4096;
}

int main(void) {
    unsigned int pid = (unsigned int)sys_getpid();
    unsigned int *w = (unsigned int *)0100000; /* byte address of word 040000 (page WIN) */
    unsigned int chunk, i, round, errs = 0, base;
    int r;

    r = sys_segalloc(SEGPAGES);
    sys_write("bigmem: segalloc(96)=", 21); put_i(r); sys_write("\r\n", 2);
    if (r != SEGPAGES) { sys_exit(1000); }

    /* bad arguments: each must return -1 and change nothing */
    sys_write("bigmem: bad args:", 17);
    sys_putchar(' '); put_i(sys_mapwin(0, 0, 1));            /* page 0 is image */
    sys_putchar(' '); put_i(sys_mapwin(30, 0, 4));           /* past page 31 */
    sys_putchar(' '); put_i(sys_mapwin(WIN, SEGPAGES - 2, 4)); /* past segment end */
    sys_putchar(' '); put_i(sys_mapwin(WIN, 0, 0));          /* zero pages */
    sys_putchar(' '); put_i(sys_segalloc(200));              /* over MAXSEG */
    sys_putchar(' '); put_i(sys_segalloc(33));               /* 96+33 > 128 */
    sys_write("\r\n", 2);

    for (chunk = 0; chunk < SEGPAGES / WPAGES; chunk++) {
        base = chunk * WPAGES;
        if (sys_mapwin(WIN, (int)base, WPAGES) != 0) errs += 10000;
        for (i = 0; i < WPAGES * 1024; i++)
            w[i] = pat(base + (i >> 10), i & 1023, pid);
    }
    sys_write("bigmem: filled 96KW\r\n", 21);

    for (round = 0; round < BIGROUNDS; round++) {
        /* walk chunks in a different order each round */
        for (chunk = 0; chunk < SEGPAGES / WPAGES; chunk++) {
            base = ((chunk * 5 + round) % (SEGPAGES / WPAGES)) * WPAGES;
            if (sys_mapwin(WIN, (int)base, WPAGES) != 0) errs += 10000;
            for (i = 0; i < WPAGES * 1024; i++)
                if (w[i] != pat(base + (i >> 10), i & 1023, pid)) errs++;
        }
        sys_write("bigmem: verified round ", 23); put_u(round);
        sys_write(" errs=", 6); put_u(errs); sys_write("\r\n", 2);
    }
    sys_exit((int)errs);
    return 0;
}
