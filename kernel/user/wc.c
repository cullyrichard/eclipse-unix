/* wc.c -- lines, words, bytes of each file (or fd 0) */
#include "../usys.h"
static unsigned int tl, tw, tc;
static char buf[128];                           /* static, not local: SHELL_NOTES.md "large frames" */
static void count(int fd, const char *name) {
    int n, i, inw = 0;
    unsigned int l = 0, w = 0, c = 0;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        for (i = 0; i < n; i++) {
            c++;
            if (buf[i] == '\n') l++;
            if (buf[i] == ' ' || (buf[i] >= '\t' && buf[i] <= '\r')) inw = 0;   /* isspace */
            else if (!inw) { inw = 1; w++; }
        }
    }
    uputu(l); uputs(" "); uputu(w); uputs(" "); uputu(c);
    if (name) { uputs(" "); uputs(name); }
    uputs("\n");
    tl += l; tw += w; tc += c;
}
int main(int argc, char **argv) {
    int i, fd, st = 0;
    if (argc < 2) { count(0, 0); return 0; }
    for (i = 1; i < argc; i++) {
        fd = open(argv[i], O_RDONLY);
        if (fd < 0) { uputsfd(2, "wc: cannot open "); uputsfd(2, argv[i]); uputsfd(2, "\n"); st = 1; continue; }
        count(fd, argv[i]);
        close(fd);
    }
    if (argc > 2) { uputu(tl); uputs(" "); uputu(tw); uputs(" "); uputu(tc); uputs(" total\n"); }
    return st;
}
