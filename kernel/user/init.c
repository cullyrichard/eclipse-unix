/* init.c -- /bin/init, the first process (FS_NOTES.md). Counts boots in
 * /etc/boots (persistence across reboots), then runs the filesystem tests
 * and a mixed concurrent load, and reports every child's exit status. */
#include "../usys.h"

static unsigned int nfail;

static int run(const char *path) {
    int pid = spawn(path), st;
    if (pid < 0) {
        uputs("init: spawn "); uputs(path); uputs(" FAILED\n");
        nfail++;
        return -1;
    }
    st = wait(pid);
    uputs("init: "); uputs(path); uputs(" exited "); uputi(st); uputs("\n");
    if (st != 0) nfail++;
    return st;
}

static unsigned int bootcount(void) {
    char buf[8];
    unsigned int n = 0;
    int fd, got, i;
    fd = open("/etc/boots", O_RDWR | O_CREAT);
    if (fd < 0) { uputs("init: cannot open /etc/boots\n"); nfail++; return 0; }
    got = read(fd, buf, 7);
    for (i = 0; i < got && buf[i] >= '0' && buf[i] <= '9'; i++)
        n = n * 10 + (unsigned int)(buf[i] - '0');
    n++;
    lseek(fd, 0, 0);
    i = 0;
    {
        char t[6];
        unsigned int v = n;
        int k = 0;
        do { t[k] = (char)('0' + v % 10); k++; v /= 10; } while (v);
        while (k > 0) { k--; buf[i] = t[k]; i++; }
    }
    buf[i] = '\n';
    write(fd, buf, i + 1);
    close(fd);
    return n;
}

int main(void) {
    int pids[8], np = 0, i, st;
    unsigned int free0;
    mkdir("/etc");                      /* may already exist */
    uputs("init: boot #"); uputu(bootcount()); uputs("\n");
    free0 = (unsigned int)freeblocks();
    uputs("init: free blocks "); uputu(free0); uputs("\n");

    run("/bin/fstest");

    /* concurrent: 3 file writers, a CPU-bound worker and a faulting
     * program all at once; each writer's file is checked by the host
     * afterwards (FS_NOTES.md) */
    uputs("init: concurrent phase\n");
    pids[np++] = spawn("/bin/fswriter");
    pids[np++] = spawn("/bin/fswriter");
    pids[np++] = spawn("/bin/worker");
    pids[np++] = spawn("/bin/fswriter");
    pids[np++] = spawn("/bin/badptr");
    for (i = 0; i < np; i++) {
        if (pids[i] < 0) { uputs("init: spawn FAILED\n"); nfail++; continue; }
        st = wait(pids[i]);
        uputs("init: pid "); uputi(pids[i]); uputs(" exited "); uputi(st);
        uputs("\n");
        /* badptr is expected to be killed (-2) */
        if (i == 4 ? st != -2 : st != 0) nfail++;
    }
    run("/bin/ls");
    uputs("init: free blocks "); uputu((unsigned int)freeblocks());
    uputs(", failures "); uputu(nfail); uputs("\n");
    sys_exit((int)nfail);
    return 0;
}
