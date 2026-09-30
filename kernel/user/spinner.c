/* spinner.c -- CPU-bound progress meter for IO_NOTES.md's measurements:
 * prints '.' every SPINK iterations of a busy loop, SPINDOTS times. */
#include "../usys.h"
#ifndef SPINDOTS
#define SPINDOTS 60
#endif
#define SPINK 400
int main(void) {
    volatile unsigned int x = 0;
    unsigned int d, i;
    for (d = 0; d < SPINDOTS; d++) {
        for (i = 0; i < SPINK; i++) x = x + i;
        write(1, ".", 1);
    }
    sys_exit(0);
    return 0;
}
