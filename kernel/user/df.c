/* df.c -- filesystem usage: 512-byte data blocks and inodes. */
#include "uio.h"

int main(void) {
    unsigned int bt = (unsigned int)fsinfo(1), bf = (unsigned int)fsinfo(0);
    unsigned int it = (unsigned int)fsinfo(3), ifr = (unsigned int)fsinfo(2);
    out_s("blocks  used  free inodes  used  free\n");
    out_uw(bt, 6); out_uw(bt - bf, 6); out_uw(bf, 6);
    out_uw(it, 7); out_uw(it - ifr, 6); out_uw(ifr, 6);
    out_c('\n');
    out_flush();
    return 0;
}
