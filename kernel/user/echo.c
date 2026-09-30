/* echo.c -- print the arguments separated by spaces */
#include "../usys.h"
int main(int argc, char **argv) {
    int i;
    for (i = 1; i < argc; i++) {
        uputs(argv[i]);
        if (i + 1 < argc) uputs(" ");
    }
    uputs("\n");
    return 0;
}
