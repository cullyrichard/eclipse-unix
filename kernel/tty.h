#ifndef _TTY_H
#define _TTY_H
/* tty.h -- interrupt-driven console input (kernel/tty.c). */
void tty_intr(void);
void tty_start(void);
unsigned int tty_readline(char *k, unsigned int n);
unsigned int tty_getc(void);
extern unsigned int tty_nintr, tty_npoll, tty_ncalls;
extern unsigned int tty_sigint;     /* ^C typed; proc.c sched() acts on it */
/* Take a waiting TTI character now, if there is one (IO_NOTES.md overrun
 * fix). Called from the kernel's long interrupts-off loops so a second
 * keystroke can't overwrite the first in TTI's one-character buffer. */
void tty_poll(void);
/* Same, but only every 2^k calls: for per-word/per-byte loops. */
#define TTY_POLL_EVERY(ctr, mask) do { if ((++(ctr) & (mask)) == 0) tty_poll(); } while (0)
#endif
