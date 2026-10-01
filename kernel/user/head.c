/* head.c -- head [-n N | -N] [file...]: the first N (10) lines */
#include "uio.h"

static char line[256];

static void head(int fd, int n) {
    int i;
    in_open(fd);
    for (i = 0; i < n && in_line(line, sizeof line) >= 0; i++) {
        out_s(line);
        out_c('\n');
    }
}

int main(int argc, char **argv) {
    int i = 1, n = 10, fd, st = 0, first;
    if (i < argc && argv[i][0] == '-' && argv[i][1] == 'n' && i + 1 < argc) {
        n = atou(argv[i + 1]);
        i += 2;
    } else if (i < argc && argv[i][0] == '-' && argv[i][1]) {
        n = atou(argv[i] + 1);
        i++;
    }
    if (n < 0) { err2("usage: head [-n N] [file...]", ""); return 2; }
    if (i >= argc) head(0, n);
    first = i;
    for (; i < argc; i++) {
        fd = open(argv[i], O_RDONLY);
        if (fd < 0) { err2("head: cannot open ", argv[i]); st = 1; continue; }
        if (argc - first > 1) {                 /* several files: name each */
            if (i > first) out_c('\n');
            out_s("==> "); out_s(argv[i]); out_s(" <==\n");
        }
        head(fd, n);
        close(fd);
    }
    out_flush();
    return st;
}
