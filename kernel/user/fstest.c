/* fstest.c -- exercises every file syscall path, including error cases,
 * and cleans up after itself; the free-block count must come back exactly
 * (FS_NOTES.md). Exit status = number of failed checks. */
#include "../usys.h"

static unsigned int nchk, nfail;

static void check(const char *what, int ok) {
    nchk++;
    if (!ok) {
        nfail++;
        uputs("fstest: FAIL "); uputs(what); uputs("\n");
    }
}

static unsigned int pat(unsigned int i) { return (i * 7 + 3) & 255; }

static int streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

#define BIG 20000
#define WCH 100
#define RCH 77

static char buf[WCH], rb[RCH];      /* static: SHELL_NOTES.md "large frames" */

int main(void) {
    static const char msg[] = "hello, filesystem\n";
    unsigned int free0, i, j, bad;
    int fd, fd2, n, got;

    /* leftovers from an interrupted earlier run */
    unlink("/ft/sub/f"); unlink("/ft/sub"); unlink("/ft/a");
    unlink("/ft/big"); unlink("/ft");
    free0 = (unsigned int)freeblocks();
    /* guards the "free blocks restored" check below against passing
     * vacuously when freeblocks() itself fails (-1 == -1): found when a
     * dispatch refactor dropped SYS_FREEBLK (SHELL_NOTES.md) */
    check("freeblocks() sane", free0 > 0 && free0 < 1024);

    check("open missing -> -1", open("/nope", O_RDONLY) == -1);
    check("mkdir /ft", mkdir("/ft") == 0);
    check("mkdir /ft again -> -1", mkdir("/ft") == -1);
    check("create in missing dir -> -1", open("/nodir/x", O_WRONLY | O_CREAT) == -1);
    check("15-char name -> -1", open("/ft/abcdefghijklmno", O_WRONLY | O_CREAT) == -1);

    /* small file round trip */
    fd = open("/ft/a", O_WRONLY | O_CREAT);
    check("create /ft/a", fd >= 0);
    check("write 18", write(fd, msg, 18) == 18);
    check("read on O_WRONLY -> -1", read(fd, buf, 1) == -1);
    close(fd);
    fd = open("/ft/a", O_RDONLY);
    n = read(fd, buf, WCH);
    bad = 0;
    for (i = 0; i < 18; i++) if (buf[i] != msg[i]) bad++;
    check("read back 18 bytes", n == 18 && bad == 0);
    check("read at EOF -> 0", read(fd, buf, 1) == 0);
    check("write on O_RDONLY -> -1", write(fd, msg, 1) == -1);
    check("unlink while open -> -1", unlink("/ft/a") == -1);
    close(fd);
    check("close twice -> -1", close(fd) == -1);

    /* append, size via lseek */
    fd = open("/ft/a", O_RDWR);
    check("lseek end = 18", lseek(fd, 0, 2) == 18);
    check("append 18", write(fd, msg, 18) == 18);
    check("lseek end = 36", lseek(fd, 0, 2) == 36);
    lseek(fd, 18, 0);
    n = read(fd, buf, WCH);
    bad = 0;
    for (i = 0; i < 18; i++) if (buf[i] != msg[i]) bad++;
    check("appended copy intact", n == 18 && bad == 0);
    close(fd);

    /* big file: 20000 bytes, crosses into the indirect block */
    fd = open("/ft/big", O_RDWR | O_CREAT);
    bad = 0;
    for (i = 0; i < BIG; i += WCH) {
        for (j = 0; j < WCH; j++) buf[j] = (char)pat(i + j);
        if (write(fd, buf, WCH) != WCH) bad++;
    }
    check("write 20000 bytes", bad == 0 && lseek(fd, 0, 2) == BIG);
    lseek(fd, 0, 0);
    bad = 0;
    for (i = 0; i < BIG; i += (unsigned int)got) {
        got = read(fd, rb, RCH);
        if (got <= 0) { bad += 1000; break; }
        for (j = 0; j < (unsigned int)got; j++)
            if (((unsigned int)rb[j] & 255) != pat(i + j)) bad++;
    }
    check("read back 20000 bytes (odd-sized chunks)", bad == 0);
    /* overwrite in the middle, across a block boundary */
    lseek(fd, 1020, 0);
    for (j = 0; j < 10; j++) buf[j] = 'X';
    check("overwrite 10 @1020", write(fd, buf, 10) == 10);
    lseek(fd, 1015, 0);
    got = read(fd, rb, 20);
    bad = 0;
    for (j = 0; j < 20; j++) {
        unsigned int want = (j >= 5 && j < 15) ? 'X' : pat(1015 + j);
        if (((unsigned int)rb[j] & 255) != want) bad++;
    }
    check("overwrite visible, neighbours intact", got == 20 && bad == 0);
    check("size unchanged by overwrite", lseek(fd, 0, 2) == BIG);
    close(fd);
    fd = open("/ft/big", O_RDWR | O_TRUNC);
    check("O_TRUNC -> size 0", fd >= 0 && lseek(fd, 0, 2) == 0);
    close(fd);

    /* bad user pointer: an unmapped page is refused, not a kernel fault */
    fd = open("/ft/a", O_RDONLY);
    check("read into unmapped page -> -1", read(fd, (char *)0160000, 10) == -1);
    check("open with unmapped path -> -1", open((char *)0160000, O_RDONLY) == -1);
    close(fd);

    /* directories */
    check("mkdir /ft/sub", mkdir("/ft/sub") == 0);
    fd = open("/ft/sub/f", O_WRONLY | O_CREAT);
    check("create /ft/sub/f", fd >= 0);
    close(fd);
    check("open dir for write -> -1", open("/ft", O_WRONLY) == -1);
    check("unlink non-empty dir -> -1", unlink("/ft/sub") == -1);
    check("unlink . -> -1", unlink("/ft/.") == -1);
    fd = open("/ft", O_RDONLY);
    n = 0;
    {
        char de[16];
        int sawa = 0, sawsub = 0;
        while (read(fd, de, 16) == 16) {
            if (de[0] == 0 && de[1] == 0) continue;
            n++;
            if (streq(de + 2, "a")) sawa = 1;
            if (streq(de + 2, "sub")) sawsub = 1;
        }
        check("dir /ft lists . .. a big sub", n == 5 && sawa && sawsub);
    }
    close(fd);
    check("unlink /ft/sub/f", unlink("/ft/sub/f") == 0);
    check("unlink /ft/sub", unlink("/ft/sub") == 0);
    check("unlink /ft/a", unlink("/ft/a") == 0);
    check("unlink /ft/big", unlink("/ft/big") == 0);
    check("unlink /ft", unlink("/ft") == 0);
    check("gone", open("/ft", O_RDONLY) == -1);
    check("free blocks restored", (unsigned int)freeblocks() == free0);

    /* descriptor table exhaustion: 8 per process, 3 are the console */
    fd = open("/etc/boots", O_RDONLY);
    for (n = 0; n < 10 && (fd2 = open("/etc/boots", O_RDONLY)) >= 0; n++) ;
    check("fd table full after 5", n == 4);
    for (fd2 = 3; fd2 < 8; fd2++) close(fd2);

    uputs("fstest: "); uputu(nchk); uputs(" checks, "); uputu(nfail);
    uputs(" failed\n");
    sys_exit((int)nfail);
    return 0;
}
