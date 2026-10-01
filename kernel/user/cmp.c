/* cmp.c -- cmp file1 file2: the first difference, as "file1 file2
 * differ: char N, line M" (counting from 1), or "EOF on fileN". Status
 * 0 same, 1 different, 2 trouble. */
#include "uio.h"

static char b2[512];
static int p2, n2, fd2;

static int get2(void) {
    if (p2 >= n2) {
        n2 = read(fd2, b2, sizeof b2);
        p2 = 0;
        if (n2 <= 0) { n2 = 0; return -1; }
    }
    return (unsigned char)b2[p2++];
}

int main(int argc, char **argv) {
    int fd1, c1, c2;
    unsigned int ch = 1, ln = 1;
    if (argc != 3) { err2("usage: cmp file1 file2", ""); return 2; }
    fd1 = open(argv[1], O_RDONLY);
    if (fd1 < 0) { err2("cmp: cannot open ", argv[1]); return 2; }
    fd2 = open(argv[2], O_RDONLY);
    if (fd2 < 0) { err2("cmp: cannot open ", argv[2]); return 2; }
    in_open(fd1);
    for (;; ch++) {
        c1 = in_getc();
        c2 = get2();
        if (c1 != c2) break;
        if (c1 < 0) return 0;
        if (c1 == '\n') ln++;
    }
    if (c1 < 0 || c2 < 0) {
        out_s("cmp: EOF on ");
        out_s(c1 < 0 ? argv[1] : argv[2]);
    } else {
        out_s(argv[1]); out_c(' '); out_s(argv[2]);
        out_s(" differ: char "); out_u(ch);
        out_s(", line "); out_u(ln);
    }
    out_c('\n');
    out_flush();
    return 1;
}
