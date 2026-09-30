/* ls.c -- list /, /bin and /tmp by reading directory files directly
 * (16-byte entries: 2-byte inode number, 14-byte name). */
#include "../usys.h"

static void lsdir(const char *dir) {
    char de[16], path[40];
    int fd, fd2, i, k, sz;
    unsigned int inum;
    fd = open(dir, O_RDONLY);
    if (fd < 0) { uputs("ls: cannot open "); uputs(dir); uputs("\n"); return; }
    uputs(dir); uputs(":\n");
    while (read(fd, de, 16) == 16) {
        inum = (((unsigned int)de[0] & 255) << 8) | ((unsigned int)de[1] & 255);
        if (inum == 0) continue;
        k = 0;
        for (i = 0; dir[i]; i++) path[k++] = dir[i];
        if (k > 1) path[k++] = '/';
        for (i = 0; i < 14 && de[2 + i]; i++) path[k++] = de[2 + i];
        path[k] = 0;
        fd2 = open(path, O_RDONLY);
        sz = fd2 < 0 ? -1 : lseek(fd2, 0, 2);
        if (fd2 >= 0) close(fd2);
        uputs("  "); uputu(inum); uputs("\t"); uputu((unsigned int)sz);
        uputs("\t"); uputs(path + (k - i)); uputs("\n");
    }
    close(fd);
}

int main(void) {
    lsdir("/");
    lsdir("/bin");
    lsdir("/tmp");
    sys_exit(0);
    return 0;
}
