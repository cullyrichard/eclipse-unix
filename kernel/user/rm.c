/* rm.c -- remove files and empty directories */
#include "../usys.h"
int main(int argc, char **argv) {
    int i, st = 0;
    for (i = 1; i < argc; i++)
        if (unlink(argv[i]) < 0) { uputsfd(2, "rm: cannot remove "); uputsfd(2, argv[i]); uputsfd(2, "\n"); st = 1; }
    return st;
}
