/* fswriter.c -- writes /tmp/wN (N = pid) in many small writes while other
 * processes run, reads it back and verifies it, and leaves it on disk for
 * the host-side check (FS_NOTES.md). Content: byte i = (i*N + 7*N) & 255,
 * WLEN bytes. Exit status = mismatches (or 100+ on I/O errors). */
#include "../usys.h"

#define WLEN 6000
#define CH 50

static char name[] = "/tmp/w0";

static unsigned int pat(unsigned int i, unsigned int n) {
    return (i * n + 7 * n) & 255;
}

int main(void) {
    unsigned int n = (unsigned int)sys_getpid(), i, j, errs = 0;
    char buf[CH];
    int fd, got;
    name[6] = (char)('0' + n);
    mkdir("/tmp");                      /* may already exist */
    fd = open(name, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) sys_exit(100);
    for (i = 0; i < WLEN; i += CH) {
        for (j = 0; j < CH; j++) buf[j] = (char)pat(i + j, n);
        if (write(fd, buf, CH) != CH) sys_exit(101);
    }
    close(fd);
    fd = open(name, O_RDONLY);
    if (fd < 0) sys_exit(102);
    for (i = 0; i < WLEN; i += (unsigned int)got) {
        got = read(fd, buf, 37);
        if (got <= 0) sys_exit(103);
        for (j = 0; j < (unsigned int)got; j++)
            if (((unsigned int)buf[j] & 255) != pat(i + j, n)) errs++;
    }
    if (read(fd, buf, 1) != 0) errs++;  /* exactly WLEN bytes */
    close(fd);
    uputs("fswriter: "); uputs(name); uputs(" errs="); uputu(errs); uputs("\n");
    sys_exit((int)errs);
    return 0;
}
