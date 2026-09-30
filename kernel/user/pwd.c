/* pwd.c -- print the current directory by walking ".." (V6 style): at
 * each level find the entry in ".." whose inode is ours. No kernel help
 * beyond stat and reading directories. */
#include "../usys.h"
static char names[8][15], up[48];               /* static, not local: SHELL_NOTES.md "large frames" */
int main(void) {
    char de[16];
    unsigned int st[4], me, root;
    int depth = 0, fd, i, k, found;
    stat("/", st); root = st[0];
    up[0] = '.'; up[1] = 0; k = 1;
    for (;;) {
        if (stat(up, st) < 0) return 1;
        me = st[0];
        if (me == root || depth == 8) break;
        up[k++] = '/'; up[k++] = '.'; up[k++] = '.'; up[k] = 0;
        if (k == 4) { up[0] = '.'; up[1] = '.'; up[2] = 0; k = 2; }
        fd = open(up, O_RDONLY);
        found = 0;
        while (read(fd, de, 16) == 16) {
            if ((((unsigned int)de[0] & 255) << 8 | ((unsigned int)de[1] & 255)) == me
                && de[2] != '.') {
                for (i = 0; i < 14; i++) names[depth][i] = de[2 + i];
                names[depth][14] = 0;
                found = 1;
                break;
            }
        }
        close(fd);
        if (!found) return 1;
        depth++;
    }
    if (depth == 0) uputs("/");
    while (depth > 0) { depth--; uputs("/"); uputs(names[depth]); }
    uputs("\n");
    return 0;
}
