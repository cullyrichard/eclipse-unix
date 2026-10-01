/* kill.c -- kill pid...: end processes (SYS_KILL; they exit with -9). */
#include "uio.h"

int main(int argc, char **argv) {
    int i, pid, st = 0;
    if (argc < 2) { err2("usage: kill pid...", ""); return 2; }
    for (i = 1; i < argc; i++) {
        pid = atou(argv[i]);
        if (pid < 0 || kill(pid) < 0) { err2("kill: no such process: ", argv[i]); st = 1; }
    }
    return st;
}
