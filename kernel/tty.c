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
unsigned int tty_nintr, tty_npoll, tty_ncalls;  /* statistics */
unsigned int tty_sigint;
int tty_raw, tty_rawpid = -1;

static void tput(unsigned int c) {
    tbuf[ttail] = (char)c;
    ttail = (ttail + 1) % TTYBUF;
    tcount++;
}

static unsigned int tget(void) {
    unsigned int c = (unsigned int)tbuf[thead] & 0377;
    thead = (thead + 1) % TTYBUF;
    tcount--;
    if (c == '\n' && tlines) tlines--;
    return c;
}

/* SYS_TTYRAW. Switching mode drops anything already typed: half a cooked
 * line would otherwise reach a raw reader, or the reverse. */
unsigned int tty_setraw(unsigned int on, int pid) {
    unsigned int old = (unsigned int)tty_raw;
    tty_raw = on ? 1 : 0;
    tty_rawpid = on ? pid : -1;
    thead = ttail = tcount = tlines = 0;
    return old;
}

void tty_intr(void) {
    unsigned int c;
    tty_nintr++;
    asm volatile("DIAC %0,010" : "=r"(c) : : "memory");
    c &= 0177;
    if (tty_raw) {                              /* SYS_TTYRAW: as typed */
        if (tcount < TTYBUF - 1) {
            tput(c);
            wakeup(CHAN_TTY);
        }
        return;
    }
    if (c == '\r') c = '\n';
    if (c == 003) {                             /* ^C: drop typed-ahead */
        thead = ttail = tcount = tlines = 0;    /* input, and let sched() */
        tty_sigint = 1;                         /* kill the foreground */
        console_putchar('^');
        console_putchar('C');
        console_putchar('\r');
        console_putchar('\n');
        wakeup(CHAN_TTY);
        return;
    }
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
    while ((tty_raw ? tcount == 0 : tlines == 0 && tcount < TTYBUF - 1) &&
           !kkilled())
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
        if (c == '\n' && !tty_raw) break;
    }
    return done;
}

/* getchar semantics: line-buffered, one character. */
unsigned int tty_getc(void) {
    tty_wait_line();
    return tcount > 0 ? tget() : '\n';
}

/* Software poll of TTI Done. SKPDZ skips the INC when Done is clear, so r
 * is 1 exactly when a character is waiting; tty_intr() then takes it
 * exactly as the interrupt would (DIAC clears Done and the request). */
void tty_poll(void) {
    unsigned int r;
    tty_ncalls++;
    asm volatile("SUB %0,%0\n\tSKPDZ 010\n\tINC %0,%0" : "=r"(r) : : "memory");
    if (r) {
        tty_npoll++;
        tty_intr();
        tty_nintr--;            /* counted as a poll, not an interrupt */
    }
}

void tty_start(void) {
    asm volatile("NIOS 010" ::: "memory");                    /* TTI busy (DG convention) */
}
