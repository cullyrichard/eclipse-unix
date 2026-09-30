/* cp.c -- cp src dst */
#include "../usys.h"
static char buf[128];                           /* static, not local: SHELL_NOTES.md "large frames" */
int main(int argc, char **argv) {
    int in, out, n;
    if (argc != 3) { uputsfd(2, "usage: cp src dst\n"); return 2; }
    in = open(argv[1], O_RDONLY);
    if (in < 0) { uputsfd(2, "cp: cannot open "); uputsfd(2, argv[1]); uputsfd(2, "\n"); return 1; }
    out = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) { uputsfd(2, "cp: cannot create "); uputsfd(2, argv[2]); uputsfd(2, "\n"); return 1; }
    while ((n = read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, n) != n) { uputsfd(2, "cp: write failed\n"); return 1; }
    close(in); close(out);
    return 0;
}
