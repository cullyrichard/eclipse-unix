/* tail.c -- tail [-n N | -N] [file]: the last N (10) lines.
 * Keeps the last TBUF bytes of the input in a ring, so it works on pipes
 * too; lines that begin more than TBUF bytes from the end are cut. */
#include "uio.h"

#define TBUF 8192
static char ring[TBUF];

int main(int argc, char **argv) {
    int i = 1, n = 10, fd = 0, c, k;
    unsigned int w = 0, cnt = 0, pos;       /* write index, bytes kept */
    if (i < argc && argv[i][0] == '-' && argv[i][1] == 'n' && i + 1 < argc) {
        n = atou(argv[i + 1]);
        i += 2;
    } else if (i < argc && argv[i][0] == '-' && argv[i][1]) {
        n = atou(argv[i] + 1);
        i++;
    }
    if (n < 0 || argc - i > 1) { err2("usage: tail [-n N] [file]", ""); return 2; }
    if (i < argc) {
        fd = open(argv[i], O_RDONLY);
        if (fd < 0) { err2("tail: cannot open ", argv[i]); return 1; }
    }
    in_open(fd);
    while ((c = in_getc()) >= 0) {
        ring[w] = (char)c;
        w = (w + 1) % TBUF;
        if (cnt < TBUF) cnt++;
    }
#define AT(j) ring[(w + TBUF - cnt + (j)) % TBUF]   /* j-th kept byte */
    /* walk back over n line ends; a last line without '\n' counts too */
    pos = cnt;
    if (pos > 0 && AT(pos - 1) == '\n') pos--;
    for (k = 0; pos > 0; pos--)
        if (AT(pos - 1) == '\n' && ++k == n) break;
    if (n == 0) pos = cnt;
    for (; pos < cnt; pos++) out_c(AT(pos));
    out_flush();
    return 0;
}
