/* sort.c -- sort [-rnu] [file...]: lines in byte order (-n by leading
 * number, -r reversed, -u equal lines once). Everything is held in
 * memory: at most POOL bytes and MAXL lines in all. */
#include "uio.h"

#define POOL  16000
#define MAXL  1200
static char pool[POOL];
static unsigned int off[MAXL];
static unsigned int nl, used;
static int rev, numeric;

static int num(const char *s) {
    int v = 0, neg = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

static int cmp(unsigned int a, unsigned int b) {
    const char *x = pool + a, *y = pool + b;
    int c;
    if (numeric) {
        int p = num(x), q = num(y);
        c = p < q ? -1 : p > q;
        if (c == 0) c = ustrcmp(x, y);
    } else {
        c = ustrcmp(x, y);
    }
    return rev ? -c : c;
}

static int load(int fd) {
    int c, start;
    in_open(fd);
    for (;;) {
        c = in_getc();
        if (c < 0) return 0;
        if (nl >= MAXL) return -1;
        start = (int)used;
        while (c >= 0 && c != '\n') {
            if (used >= POOL - 1) return -1;
            pool[used++] = (char)c;
            c = in_getc();
        }
        pool[used++] = 0;
        off[nl++] = (unsigned int)start;
    }
}

int main(int argc, char **argv) {
    int i = 1, uniq = 0, fd;
    unsigned int gap, j, k, t;
    const char *f;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        for (f = argv[i] + 1; *f; f++) {
            if (*f == 'r') rev = 1;
            else if (*f == 'n') numeric = 1;
            else if (*f == 'u') uniq = 1;
            else { err2("usage: sort [-rnu] [file...]", ""); return 2; }
        }
    if (i >= argc && load(0) < 0) { err2("sort: input too big", ""); return 1; }
    for (; i < argc; i++) {
        fd = open(argv[i], O_RDONLY);
        if (fd < 0) { err2("sort: cannot open ", argv[i]); return 1; }
        if (load(fd) < 0) { err2("sort: input too big at ", argv[i]); return 1; }
        close(fd);
    }
    for (gap = nl / 2; gap > 0; gap /= 2)          /* Shell sort */
        for (j = gap; j < nl; j++)
            for (k = j; k >= gap && cmp(off[k - gap], off[k]) > 0; k -= gap) {
                t = off[k]; off[k] = off[k - gap]; off[k - gap] = t;
            }
    for (j = 0; j < nl; j++) {
        if (uniq && j > 0 && ustrcmp(pool + off[j], pool + off[j - 1]) == 0) continue;
        out_s(pool + off[j]);
        out_c('\n');
    }
    out_flush();
    return 0;
}
