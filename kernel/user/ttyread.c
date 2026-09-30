/* ttyread.c -- blocks reading one line from the console (fd 0), then
 * echoes what it got, bracketed, so the transcript shows exactly when the
 * read returned relative to other processes' output (IO_NOTES.md). */
#include "../usys.h"
int main(void) {
    char line[40];
    int n;
    uputs("\n<ttyread: waiting>\n");
    n = read(0, line, 39);
    uputs("\n<ttyread: got ");
    uputi(n);
    uputs(" bytes: ");
    if (n > 0) write(1, line, n);
    uputs(">\n");
    sys_exit(n > 0 ? 0 : 1);
    return 0;
}
