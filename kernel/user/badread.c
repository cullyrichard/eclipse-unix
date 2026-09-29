/* badread.c -- reads through a pointer into a page that is not mapped in
 * this process's address space. Expected: MMPU validity fault (the read
 * path, unlike badptr.c's write) -> kernel kills this process. */
#include "../usys.h"

int main(void) {
    volatile int *p = (volatile int *)0160000; /* byte address of word 070000: logical page 28 */
    int v;
    sys_write("badread: reading 070000\r\n", 25);
    v = *p;
    sys_write("badread: STILL ALIVE (bug)\r\n", 28);
    sys_exit(v);
    return 0;
}
