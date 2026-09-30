/* ls.c -- ls [-l] [path...]: list directories (by reading their 16-byte
 * entries) or name files; -l adds type, inode, links and size via stat. */
#include "../usys.h"

static int lflag;

static void entry(const char *path, const char *name) {
    unsigned int st[4];
    if (lflag) {
        if (stat(path, st) < 0) { uputs("? "); }
        else {
            uputs(st[1] == 2 ? "d " : "- ");
            uputu(st[0]); uputs("\t"); uputu(st[2]); uputs("\t"); uputu(st[3]); uputs("\t");
        }
    }
    uputs(name); uputs("\n");
}

static char path[80];                           /* static, not local: SHELL_NOTES.md "large frames" */
static int ls(const char *dir) {
    unsigned int st[4];
    char de[16];
    int fd, i, k;
    if (stat(dir, st) < 0) { uputsfd(2, "ls: "); uputsfd(2, dir); uputsfd(2, ": not found\n"); return 1; }
    if (st[1] != 2) { entry(dir, dir); return 0; }
    fd = open(dir, O_RDONLY);
    while (read(fd, de, 16) == 16) {
        if (de[0] == 0 && de[1] == 0) continue;
        k = 0;
        for (i = 0; dir[i] && k < 60; i++) path[k++] = dir[i];
        if (k && path[k - 1] != '/') path[k++] = '/';
        for (i = 0; i < 14 && de[2 + i]; i++) path[k++] = de[2 + i];
        path[k] = 0;
        entry(path, path + k - i);
    }
    close(fd);
    return 0;
}

int main(int argc, char **argv) {
    int i = 1, st = 0, any = 0;
    if (i < argc && !ustrcmp(argv[i], "-l")) { lflag = 1; i++; }
    for (; i < argc; i++) {
        if (any || i + 1 < argc) { uputs(argv[i]); uputs(":\n"); }
        st |= ls(argv[i]);
        any = 1;
    }
    if (!any) st = ls(".");
    return st;
}
