#include "kio.h"
#include "tty.h"
#include "../examples/console.h"

void kputs(const char *s) {
    while (*s) {
        console_putchar(*s);
        tty_poll();             /* polled TTO output is slow (section 11) */
        s++;
    }
}

static void kputbase(unsigned int v, unsigned int base) {
    char buf[8];
    int n = 0;
    do {
        buf[n] = ('0' + v % base);
        n++;
        v = v / base;
    } while (v != 0);
    while (n > 0) {
        n--;
        console_putchar(buf[n]);
        tty_poll();
    }
}

void kputu(unsigned int v) { kputbase(v, 10); }
void kputo(unsigned int v) { kputbase(v, 8); }
