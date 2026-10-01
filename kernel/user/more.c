/* more.c -- more [file...]: show PAGE lines at a time. At "--More--"
 * type Return for the next page, or q then Return to stop. The answer is
 * read from the console itself (SYS_GETCHAR), so `... | more` works too.
 * The console is line-buffered, so every answer ends with Return. */
#include "uio.h"

#define PAGE 22
static char line[256];
static int shown;

/* 1 to go on, 0 to quit */
static int ask(void) {
    int c, q;
    out_s("--More--");
    out_flush();
    c = sys_getchar();
    q = c == 'q' || c == 'Q';
    while (c != '\n') c = sys_getchar();
    shown = 0;
    return !q;
}

static int more(int fd) {
    in_open(fd);
    while (in_line(line, sizeof line) >= 0) {
        if (shown == PAGE && !ask()) return 0;
        out_s(line);
        out_c('\n');
        shown++;
    }
    return 1;
}

int main(int argc, char **argv) {
    int i, fd;
    if (argc < 2) more(0);
    for (i = 1; i < argc; i++) {
        fd = open(argv[i], O_RDONLY);
        if (fd < 0) { err2("more: cannot open ", argv[i]); continue; }
        if (argc > 2) {
            if (shown && i > 1 && !ask()) break;
            out_s(":::::::::::::: "); out_s(argv[i]); out_s(" ::::::::::::::\n");
            shown++;
        }
        if (!more(fd)) break;
        close(fd);
    }
    out_flush();
    return 0;
}
