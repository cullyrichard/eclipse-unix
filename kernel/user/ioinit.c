/* ioinit.c -- test init for IO_NOTES.md. Runs /bin/spinner next to either
 * /bin/ttyread (IOTEST_TTY) or /bin/fswriter (disk), prints a marker when
 * the I/O process finishes, then waits for the spinner. Counting the dots
 * printed before the marker measures how much CPU other processes got
 * while one was blocked on I/O. */
#include "../usys.h"
int main(void) {
    int sp, io, st;
    mkdir("/tmp");
    sp = spawn("/bin/spinner");
#ifdef IOTEST_TTY
    io = spawn("/bin/ttyread");
#else
    io = spawn("/bin/fswriter");
#endif
    st = wait(io);
    uputs("\n<IO DONE status ");
    uputi(st);
    uputs(">\n");
    st = wait(sp);
    uputs("\n<spinner done ");
    uputi(st);
    uputs(">\n");
    sys_exit(0);
    return 0;
}
