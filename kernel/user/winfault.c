/* winfault.c -- a window page is really gone after SYS_MAPWIN(-1):
 * map one segment page, write it, unmap it, write again. Expected: the
 * first write works, the second faults and the kernel kills this
 * process (and frees its segment frame). */
#include "../usys.h"

int main(void) {
    volatile unsigned int *w = (volatile unsigned int *)060000; /* page 24 */
    sys_segalloc(1);
    sys_mapwin(24, 0, 1);
    *w = 4321;
    sys_write("winfault: mapped write ok, unmapping\r\n", 38);
    sys_unmapwin(24, 1);
    *w = 1234;
    sys_write("winfault: STILL ALIVE (bug)\r\n", 29);
    sys_exit(99);
    return 0;
}
