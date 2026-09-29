/* badptr.c -- writes through a pointer into a page that is not mapped in
 * this process's address space. Expected: MMPU validity fault -> kernel
 * kills this process; every other process keeps running. */
#include "../usys.h"

int main(void) {
    volatile int *p = (volatile int *)0160000; /* byte address of word 070000: logical page 28 */
    sys_write("badptr: poking 070000\r\n", 23);
    *p = 1234;
    sys_write("badptr: STILL ALIVE (bug)\r\n", 27);
    sys_exit(99);
    return 0;
}
