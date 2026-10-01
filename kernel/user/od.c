/* od.c -- od [-bcw] [file]: dump in octal. -w (the default) 16-bit words,
 * high byte first, the order the Eclipse stores them; -b bytes; -c
 * characters (\0 \n \t \r, other non-printing ones as 3-digit octal).
 * The address column is the octal byte offset. Lines of 16 bytes; a run
 * of identical lines prints once and then "*". */
#include "uio.h"

static char cur[16], prev[16];

int main(int argc, char **argv) {
    int i = 1, mode = 'w', fd = 0, n, k, c, same = 0;
    unsigned int addr = 0;
    if (i < argc && argv[i][0] == '-' && argv[i][1]) {
        mode = argv[i][1];
        if ((mode != 'b' && mode != 'c' && mode != 'w') || argv[i][2]) {
            err2("usage: od [-bcw] [file]", "");
            return 2;
        }
        i++;
    }
    if (i < argc) {
        fd = open(argv[i], O_RDONLY);
        if (fd < 0) { err2("od: cannot open ", argv[i]); return 1; }
    }
    in_open(fd);
    for (;;) {
        for (n = 0; n < 16 && (c = in_getc()) >= 0; n++) cur[n] = (char)c;
        if (n == 0) break;
        if (n == 16 && addr > 0) {
            for (k = 0; k < 16 && cur[k] == prev[k]; k++)
                ;
            if (k == 16) {
                if (!same) out_s("*\n");
                same = 1;
                addr += 16;
                continue;
            }
        }
        same = 0;
        out_o(addr, 7);
        for (k = 0; k < n; k++) {
            c = (unsigned char)cur[k];
            if (mode == 'w') {
                if (k & 1) continue;
                out_c(' ');
                out_o((unsigned int)c << 8 | (k + 1 < n ? (unsigned char)cur[k + 1] : 0), 6);
            } else if (mode == 'b') {
                out_c(' ');
                out_o((unsigned int)c, 3);
            } else {
                if (c >= ' ' && c < 0177) { out_s("   "); out_c(c); }  /* 4 wide */
                else if (c == 0) out_s("  \\0");
                else if (c == '\n') out_s("  \\n");
                else if (c == '\t') out_s("  \\t");
                else if (c == '\r') out_s("  \\r");
                else { out_c(' '); out_o((unsigned int)c, 3); }
            }
            prev[k] = cur[k];
        }
        out_c('\n');
        addr += (unsigned int)n;
        if (n < 16) break;
    }
    out_o(addr, 7);
    out_c('\n');
    out_flush();
    return 0;
}
