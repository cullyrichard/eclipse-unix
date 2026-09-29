#ifndef _KIO_H
#define _KIO_H

/* Minimal kernel console output on top of examples/console.c (no printf:
 * it would pull most of eclipse_rt.c into the kernel image). */

void kputs(const char *s);
void kputu(unsigned int v);     /* decimal */
void kputo(unsigned int v);     /* octal */

#endif
