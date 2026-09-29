#include "kio.h"
#include "../examples/console.h"

void kputs(const char *s) {
    while (*s) {
        console_putchar(*s);
        s++;
    }
}

static void kputbase(unsigned int v, unsigned int base) {
    int buf[8];         /* int, not char: char arrays miscompile (KERNEL_NOTES.md) */
    int n = 0;
    do {
        buf[n] = ('0' + v % base);
        n++;
        v = v / base;
    } while (v != 0);
    while (n > 0) {
        n--;
        console_putchar(buf[n]);
    }
}

void kputu(unsigned int v) { kputbase(v, 10); }
void kputo(unsigned int v) { kputbase(v, 8); }
