/* worker.c -- user program for the N-process kernel's isolation test
 * (KERNEL_NOTES.md). Every copy of this program is loaded into its own
 * address space, so `marker` sits at the SAME logical address in every
 * process but in DIFFERENT physical frames. Each process fills it with a
 * pid-specific pattern, then keeps re-checking it while the scheduler
 * preempts it; any cross-process leakage or a bad context restore shows
 * up as a nonzero error count. */
#include "../usys.h"

#ifndef ROUNDS
#define ROUNDS 6
#endif
#define NMARK 256

int marker[NMARK];

static void put_u(unsigned int v) {
    char buf[6];
    int n = 0;
    do { buf[n] = '0' + v % 10; n++; v = v / 10; } while (v != 0);
    while (n > 0) { n--; sys_putchar(buf[n]); }
}

int main(void) {
    int pid = sys_getpid();
    unsigned int i, round, pass, errs = 0;
    unsigned int sum, expect;
    char tag[4];

    expect = 0;
    for (i = 0; i < NMARK; i++) {
        marker[i] = pid * 1000 + i;
        expect += pid * 1000 + i;
    }

    tag[0] = '['; tag[1] = '0' + pid; tag[2] = ']'; tag[3] = ' ';
    for (round = 0; round < ROUNDS; round++) {
        for (pass = 0; pass < 20; pass++) {
            sum = 0;
            for (i = 0; i < NMARK; i++) {
                if (marker[i] != pid * 1000 + i) errs++;
                sum += marker[i];
            }
            /* sum lives in a register across preemptions: a bad context
             * restore (registers or carry) shows up here */
            if (sum != expect) errs++;
        }
        sys_write(tag, 4);          /* user buffer copied in by the kernel */
        put_u(round);
        sys_putchar(' ');
    }
    sys_write(tag, 3);
    sys_putchar('e'); sys_putchar('=');
    put_u(errs);
    sys_putchar(' ');
    sys_exit(errs);
    return 0;
}
