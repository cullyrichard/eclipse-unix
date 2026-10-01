#include <stdio.h>
/* DEBUGGING_NOTES #41 repro: big frame, values lost via _scratch */
int f(int k) {
    volatile int pad[200];
    int c[4];
    int i;
    for (i = 0; i < 200; i++) pad[i] = i;
    c[2] = 3;
    c[2] = c[2] + 1;
    c[k] = c[k] + 10;      /* runtime index too */
    return c[2] + pad[0] + pad[k * 50];
}
int g(void) {
    char buf[300];
    int n = 0, i;
    for (i = 0; i < 300; i++) buf[i] = (char)(i & 63);
    for (i = 0; i < 300; i++) n += buf[i];
    buf[299] = buf[298] + 1;
    return n + buf[299];
}
int E(int k);
int Ej(int j);
int main(void) {
    printf("f=%d (expect %d)\n", f(2), 4 + 10 + 0 + 100);
    printf("g=%d (expect %d)\n", g(), 9010 + 43);
    printf("E=%d Ej=%d\n", E(2), Ej(1));
    return 0;
}
/* DEBUGGING_NOTES.md #41 regression test, plus the shell's original repro:
 * expected output "f=114 (expect 114)" and "E=5 Ej=2". g (a large
 * char frame) is 9053 with or without the fix. */
int E(int k) { int c[4]; int pad[60]; c[2] = 3; pad[0] = 1; c[2] = c[2] + 1; return c[2] + pad[0]; }
int Ej(int j) { int c[4]; int pad[60]; c[j] = 1; pad[0] = 1; c[j]++; return c[j]; }
