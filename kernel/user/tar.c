/* tar.c -- POSIX ustar archives on magtape, one 512-byte tar block per
 * tape record (FS_NOTES.md section 10). The host side converts between
 * these tapes and ordinary .tar files (fstools/tap2tar.py, tar2tap.py),
 * so archives move both ways with a standard tar.
 *
 *   tar c [-u unit] path...   archive files/directories (recursively)
 *   tar t [-u unit]           list
 *   tar x [-u unit]           extract into the current directory
 * Default unit 1 (unit 0 usually holds the boot tape). Leading '/' is
 * stripped from names, as standard tar does. Files are at most 65535
 * bytes (the filesystem's limit). */
#include "../usys.h"

static unsigned int blkw[256];          /* word array: even address */
static char *blk = (char *)blkw;
static int unit = 1, nerr;
static char path[128];

static void say(const char *a, const char *b) { uputs(a); uputs(b); uputs("\n"); }

static void zero(void) { int i; for (i = 0; i < 256; i++) blkw[i] = 0; }

static void octal(char *f, int width, unsigned int v) {
    int i;
    for (i = width - 2; i >= 0; i--) { f[i] = (char)('0' + (v & 7)); v >>= 3; }
    f[width - 1] = 0;
}

static unsigned int unoctal(const char *f, int width) {
    unsigned int v = 0;
    int i;
    for (i = 0; i < width && f[i] == ' '; i++) ;
    for (; i < width && f[i] >= '0' && f[i] <= '7'; i++) v = v * 8 + (unsigned int)(f[i] - '0');
    return v;
}

static unsigned int checksum(void) {
    unsigned int s = 0;
    int i;
    for (i = 0; i < 512; i++)
        s += (i >= 148 && i < 156) ? ' ' : ((unsigned int)blk[i] & 255);
    return s;
}

static int put(void) {
    if (tape(TP_WRITE, unit, blk, 512) != 512) { uputs("tar: tape write error\n"); nerr++; return -1; }
    return 0;
}

static int header(const char *name, unsigned int size, int isdir) {
    int i, n;
    zero();
    while (*name == '/') name++;
    n = ustrlen(name);
    if (n + isdir > 99) { say("tar: name too long: ", name); nerr++; return -1; }
    for (i = 0; i < n; i++) blk[i] = name[i];
    if (isdir && (n == 0 || name[n - 1] != '/')) blk[n] = '/';
    octal(blk + 100, 8, isdir ? 0755 : 0644);
    octal(blk + 108, 8, 0);
    octal(blk + 116, 8, 0);
    octal(blk + 124, 12, size);
    octal(blk + 136, 12, 0);
    blk[156] = isdir ? '5' : '0';
    blk[257] = 'u'; blk[258] = 's'; blk[259] = 't'; blk[260] = 'a'; blk[261] = 'r';
    blk[263] = '0'; blk[264] = '0';
    octal(blk + 148, 7, checksum());   /* 6 digits + NUL, then a space */
    blk[155] = ' ';
    return put();
}

static void addfile(const char *p, unsigned int size) {
    int fd, got, i;
    unsigned int done = 0;
    fd = open(p, O_RDONLY);
    if (fd < 0) { say("tar: cannot open ", p); nerr++; return; }
    if (header(p, size, 0) < 0) { close(fd); return; }
    while (done < size) {
        zero();
        for (i = 0; i < 512 && done < size; i += got, done += (unsigned int)got) {
            got = read(fd, blk + i, 512 - i);
            if (got <= 0) break;
        }
        if (put() < 0) break;
        if (got <= 0 && done < size) { say("tar: short read ", p); nerr++; break; }
    }
    close(fd);
    say("a ", p);
}

static void add(int len) {          /* path[0..len) names a file or dir */
    unsigned int st[4];
    char de[16];
    int fd, i, k;
    path[len] = 0;
    if (stat(path, st) < 0) { say("tar: no such file: ", path); nerr++; return; }
    if (st[1] != 2) { addfile(path, st[3]); return; }
    if (header(path, 0, 1) < 0) return;
    say("a ", path);
    fd = open(path, O_RDONLY);
    if (fd < 0) return;
    while (read(fd, de, 16) == 16) {
        if (de[0] == 0 && de[1] == 0) continue;
        if (de[2] == '.' && (de[3] == 0 || (de[3] == '.' && de[4] == 0))) continue;
        k = len;
        if (k > 0 && path[k - 1] != '/') path[k++] = '/';
        for (i = 0; i < 14 && de[2 + i] && k < 126; i++) path[k++] = de[2 + i];
        path[k] = 0;
        add(k);
        path[len] = 0;
    }
    close(fd);
}

static int get(void) {                      /* next block: 1, 0 = end */
    int n = tape(TP_READ, unit, blk, 512);
    if (n == 512) return 1;
    if (n != 0) { uputs("tar: tape read error\n"); nerr++; }
    return 0;
}

static void mkparents(char *p) {
    int i;
    for (i = 1; p[i]; i++)
        if (p[i] == '/') { p[i] = 0; mkdir(p); p[i] = '/'; }
}

static void list_or_extract(int extract) {
    unsigned int size, left, sum;
    int fd, i, isdir, n;
    char name[101];
    while (get()) {
        if (blk[0] == 0) break;                         /* end-of-archive block */
        sum = unoctal(blk + 148, 8);
        if (sum != checksum()) { uputs("tar: bad header checksum\n"); nerr++; return; }
        for (i = 0; i < 100 && blk[i]; i++) name[i] = blk[i];
        name[i] = 0;
        size = unoctal(blk + 124, 12);
        isdir = blk[156] == '5' || (i > 0 && name[i - 1] == '/');
        if (isdir && i > 1 && name[i - 1] == '/') name[i - 1] = 0;
        if (!extract) {
            uputs(isdir ? "d " : "- "); uputu(size); uputs("\t"); uputs(name); uputs("\n");
        }
        fd = -1;
        if (extract) {
            mkparents(name);
            if (isdir) { mkdir(name); say("x ", name); }
            else if (blk[156] == '0' || blk[156] == 0) {
                fd = open(name, O_WRONLY | O_CREAT | O_TRUNC);
                if (fd < 0) { say("tar: cannot create ", name); nerr++; }
                else say("x ", name);
            }
        }
        for (left = size; left > 0; left -= (unsigned int)n) {
            if (!get()) { uputs("tar: archive ends inside a file\n"); nerr++; return; }
            n = left < 512 ? (int)left : 512;
            if (fd >= 0 && write(fd, blk, n) != n) { say("tar: write error ", name); nerr++; }
        }
        if (fd >= 0) close(fd);
    }
}

int main(int argc, char **argv) {
    int i = 2, len;
    char mode;
    if (argc < 2) { uputs("usage: tar c|t|x [-u unit] [path...]\n"); return 2; }
    mode = argv[1][0];
    if (argc > 3 && argv[2][0] == '-' && argv[2][1] == 'u') {
        unit = argv[3][0] - '0';
        i = 4;
    }
    if (tape(TP_REWIND, unit, blk, 0) < 0) { uputs("tar: tape not ready\n"); return 1; }
    if (mode == 'c') {
        for (; i < argc; i++) {
            for (len = 0; argv[i][len] && len < 120; len++) path[len] = argv[i][len];
            add(len);
        }
        zero();
        put(); put();                               /* end of archive */
        tape(TP_WEOF, unit, blk, 0);
        tape(TP_WEOF, unit, blk, 0);
        tape(TP_REWIND, unit, blk, 0);
    } else if (mode == 't' || mode == 'x') {
        list_or_extract(mode == 'x');
        tape(TP_REWIND, unit, blk, 0);
    } else {
        uputs("usage: tar c|t|x [-u unit] [path...]\n");
        return 2;
    }
    return nerr ? 1 : 0;
}
