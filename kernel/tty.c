/* tty.c -- interrupt-driven console input (IO_NOTES.md).
 *
 * TTI (device 010) interrupts once per character. tty_intr() reads it
 * with DIAC (read + clear Done, nova_tt.c), echoes it (polled TTO), and
 * appends it to a line buffer; a newline, or a full buffer, completes a
 * line and wakes readers. Canonical mode: a reader sleeps until a whole
 * line is available, then gets at most one line. Backspace/DEL erase the
 * last character of the unfinished line. Output (TTO) stays polled: see
 * IO_NOTES.md for why. */
#include "tty.h"
#include "ksync.h"
#include "../examples/console.h"

#define TTYBUF 128

static char tbuf[TTYBUF];
static unsigned int thead, ttail, tcount;   /* ring buffer */
static unsigned int tlines;                 /* complete lines buffered */
unsigned int tty_nintr;                     /* statistics */

static void tput(unsigned int c) {
    tbuf[ttail] = (char)c;
    ttail = (ttail + 1) % TTYBUF;
    tcount++;
}

static unsigned int tget(void) {
    unsigned int c = (unsigned int)tbuf[thead] & 0377;
    thead = (thead + 1) % TTYBUF;
    tcount--;
    if (c == '\n') tlines--;
    return c;
}

void tty_intr(void) {
    unsigned int c;
    tty_nintr++;
    asm volatile("DIAC %0,010" : "=r"(c));
    c &= 0177;
    if (c == '\r') c = '\n';
    if (c == 010 || c == 0177) {                /* erase */
        if (tcount > 0) {
            unsigned int last = (ttail + TTYBUF - 1) % TTYBUF;
            if (tbuf[last] != '\n') {
                ttail = last;
                tcount--;
                console_putchar(010);
                console_putchar(' ');
                console_putchar(010);
            }
        }
        return;
    }
    if (tcount >= TTYBUF - 1) {                 /* full: drop it, let */
        wakeup(CHAN_TTY);                       /* readers drain */
        return;
    }
    if (c == '\n') console_putchar('\r');
    console_putchar((int)c);
    tput(c);
    if (c == '\n') {
        tlines++;
        wakeup(CHAN_TTY);
    }
}

static void tty_wait_line(void) {
    while (tlines == 0 && tcount < TTYBUF - 1)
        ksleep(CHAN_TTY);
}

/* One line (up to n bytes, including its '\n') into k; returns length. */
unsigned int tty_readline(char *k, unsigned int n) {
    unsigned int done = 0, c;
    tty_wait_line();
    while (done < n && tcount > 0) {
        c = tget();
        k[done] = (char)c;
        done++;
        if (c == '\n') break;
    }
    return done;
}

/* getchar semantics: line-buffered, one character. */
unsigned int tty_getc(void) {
    tty_wait_line();
    return tcount > 0 ? tget() : '\n';
}

void tty_start(void) {
    asm volatile("NIOS 010");                    /* TTI busy (DG convention) */
}
