#ifndef _TTY_H
#define _TTY_H
/* tty.h -- interrupt-driven console input (kernel/tty.c). */
void tty_intr(void);
void tty_start(void);
unsigned int tty_readline(char *k, unsigned int n);
unsigned int tty_getc(void);
extern unsigned int tty_nintr;
#endif
