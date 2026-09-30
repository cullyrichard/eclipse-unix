/* mkdir.c -- make directories */
#include "../usys.h"
int main(int argc, char **argv) {
    int i, st = 0;
    for (i = 1; i < argc; i++)
        if (mkdir(argv[i]) < 0) { uputsfd(2, "mkdir: cannot create "); uputsfd(2, argv[i]); uputsfd(2, "\n"); st = 1; }
    return st;
}
