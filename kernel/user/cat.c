/* cat.c -- concatenate files (or fd 0 if none) to fd 1 */
#include "../usys.h"
static char buf[128];                           /* static, not local: SHELL_NOTES.md "large frames" */
static int copy(int fd) {
    int n;
    while ((n = read(fd, buf, sizeof buf)) > 0)
        if (write(1, buf, n) != n) return 1;
    return n < 0;
}
int main(int argc, char **argv) {
    int i, fd, st = 0;
    if (argc < 2) return copy(0);
    for (i = 1; i < argc; i++) {
        fd = open(argv[i], O_RDONLY);
        if (fd < 0) { uputsfd(2, "cat: cannot open "); uputsfd(2, argv[i]); uputsfd(2, "\n"); st = 1; continue; }
        st |= copy(fd);
        close(fd);
    }
    return st;
}
