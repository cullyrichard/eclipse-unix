/* escape.c -- tries to turn the MMPU off from user mode with NIOP MAP.
 * With MapStat's I/O-protection bit clear this would really work (the
 * manual's NIOP entry; MMPU_NOTES.md) and hand the process the whole
 * machine. The kernel runs every process with I/O protection on, so this
 * must fault instead and the process must be killed. */
#include "../usys.h"

asm("dev MAP = 03");

int main(void) {
    sys_write("escape: NIOP MAP\r\n", 18);
    asm volatile("NIOP MAP\n\t");
    sys_write("escape: MMPU OFF (bug)\r\n", 24);
    sys_exit(99);
    return 0;
}
