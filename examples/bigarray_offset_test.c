/* DEBUGGING_NOTES.md #43 regression test: constant indexes past 16383
 * words in a global array. Expected output: "1234 77 88". */
#include <stdio.h>
unsigned int big[17000];
int main(void) {
    big[16999] = 1234;      /* constant index past 16383 words */
    big[16383] = 77;
    big[16384] = 88;
    printf("%u %u %u\n", big[16999], big[16383], big[16384]);
    return 0;
}
