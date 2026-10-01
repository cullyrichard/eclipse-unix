/* uio.h -- buffered line input and buffered output for the text tools
 * (SHELL_NOTES.md section 6). Header-only: every function is static and
 * eclipse-cc's globaldce drops what a program doesn't call. One input
 * stream at a time (the tools read their files one after another). */
#ifndef _UIO_H
#define _UIO_H
#include "../usys.h"

static char in_buf[512];
static int in_fd, in_pos, in_len;

static void in_open(int fd) { in_fd = fd; in_pos = in_len = 0; }

/* next byte, or -1 at end of input */
static int in_getc(void) {
    if (in_pos >= in_len) {
        in_len = read(in_fd, in_buf, sizeof in_buf);
        in_pos = 0;
        if (in_len <= 0) { in_len = 0; return -1; }
    }
    return (unsigned char)in_buf[in_pos++];
}

/* one line without its '\n' into b (at most max-1 bytes kept, the rest of
 * a longer line dropped); its length, or -1 at end of input */
static int in_line(char *b, int max) {
    int c, n = 0;
    c = in_getc();
    if (c < 0) return -1;
    while (c >= 0 && c != '\n') {
        if (n < max - 1) b[n++] = (char)c;
        c = in_getc();
    }
    b[n] = 0;
    return n;
}

static char out_buf[256];
static int out_len;

static void out_flush(void) {
    if (out_len > 0) write(1, out_buf, out_len);
    out_len = 0;
}

static void out_c(int c) {
    out_buf[out_len++] = (char)c;
    if (out_len == (int)sizeof out_buf) out_flush();
}

static void out_s(const char *s) { while (*s) out_c(*s++); }

static void out_u(unsigned int v) {
    char t[6];
    int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) out_c(t[--n]);
}

/* v right-aligned in w columns */
static void out_uw(unsigned int v, int w) {
    unsigned int t = v;
    int n = 1;
    while (t >= 10) { t /= 10; n++; }
    while (n++ < w) out_c(' ');
    out_u(v);
}

static void out_o(unsigned int v, int digits) {
    int i;
    for (i = digits - 1; i >= 0; i--) out_c('0' + ((v >> (3 * i)) & 7));
}

static void err2(const char *a, const char *b) {
    out_flush();
    uputsfd(2, a);
    uputsfd(2, b);
    uputsfd(2, "\n");
}

/* decimal digits at s; -1 if s isn't a number */
static int atou(const char *s) {
    int v = 0;
    if (*s == 0) return -1;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return *s ? -1 : v;
}

#endif
