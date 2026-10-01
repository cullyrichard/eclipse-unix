/* mv.c -- mv from to, or mv file... dir: rename within the filesystem
 * (SYS_RENAME). A file already at `to` is replaced; a directory can only
 * be renamed inside its own parent directory. */
#include "uio.h"

static char path[128];

static int isdir(const char *p) {
    unsigned int st[4];
    return stat(p, st) == 0 && st[1] == 2;
}

static int mv1(const char *from, const char *to) {
    if (rename(from, to) < 0) {
        err2("mv: cannot move ", from);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    int i, n, st = 0;
    const char *dir, *b, *s;
    if (argc < 3) { err2("usage: mv from to | mv file... dir", ""); return 2; }
    dir = argv[argc - 1];
    if (!isdir(dir)) {
        if (argc != 3) { err2("mv: not a directory: ", dir); return 2; }
        return mv1(argv[1], argv[2]);
    }
    for (i = 1; i < argc - 1; i++) {        /* into dir, same last name */
        b = argv[i];
        for (s = argv[i]; *s; s++)
            if (*s == '/' && s[1]) b = s + 1;
        n = 0;
        for (s = dir; *s && n < 100; s++) path[n++] = *s;
        if (n == 0 || path[n - 1] != '/') path[n++] = '/';
        for (s = b; *s && *s != '/' && n < 126; s++) path[n++] = *s;
        path[n] = 0;
        st |= mv1(argv[i], path);
    }
    return st;
}
