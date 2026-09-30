/* sweepinit.c -- init for IO_NOTES.md's interrupt-injection sweep: a
 * CPU-bound register checker (worker), a disk writer (fswriter) and a
 * console reader (ttyread) at once. Exit status = number of children that
 * did not exit 0. */
#include "../usys.h"
int main(void) {
    int p[3], i, bad = 0, st;
    mkdir("/tmp");
    p[0] = spawn("/bin/worker");
    p[1] = spawn("/bin/fswriter");
    p[2] = spawn("/bin/ttyread");
    for (i = 0; i < 3; i++) {
        st = wait(p[i]);
        if (st != 0) bad++;
    }
    uputs("\n<SWEEP bad="); uputi(bad); uputs(">\n");
    sys_exit(bad);
    return 0;
}
