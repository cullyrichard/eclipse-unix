/* nova_fpsx.c -- logging stand-in for the FPS-100 at devices 054/055.
 * Not an FPS-100 model: every DOx/NIOx/DIx to 054 or 055 is written to
 * fpsx.log ("<op> <dev> <octal value>"), and every DIx returns the next
 * value of a fixed 16-bit LFSR (DIC, the busy/status read, returns 0).
 * Two builds of the same program that drive the FPS identically produce
 * identical logs and see identical answers. */
#include "nova_defs.h"
static FILE *fpsx_log;
static uint16 fpsx_lfsr = 0xACE1;
static uint16 fpsx_next(void) {
    uint16 b = ((fpsx_lfsr >> 0) ^ (fpsx_lfsr >> 2) ^ (fpsx_lfsr >> 3) ^ (fpsx_lfsr >> 5)) & 1;
    fpsx_lfsr = (fpsx_lfsr >> 1) | (b << 15);
    return fpsx_lfsr;
}
static int32 fpsx_io(int dev, int32 pulse, int32 code, int32 AC) {
    static const char *names[] = { "NIO", "DIA", "DOA", "DIB", "DOB", "DIC", "DOC", "SKP" };
    static const char *pnames[] = { "", "S", "C", "P" };
    int32 r = AC;
    if (!fpsx_log) fpsx_log = fopen("fpsx.log", "w");
    if (code == ioDIA || code == ioDIB) r = fpsx_next();
    else if (code == ioDIC) r = 0;
    if (code != ioSKP)
        fprintf(fpsx_log, "%s%s %03o %06o\n", names[code], pnames[pulse], dev,
                (code == ioDIA || code == ioDIB || code == ioDIC) ? r : (code == ioNIO ? 0 : AC & 0177777));
    fflush(fpsx_log);
    return r;
}
static int32 fpsa(int32 pulse, int32 code, int32 AC) { return fpsx_io(054, pulse, code, AC); }
static int32 fpsb(int32 pulse, int32 code, int32 AC) { return fpsx_io(055, pulse, code, AC); }
DIB fpsa_dib = { 054, 0, 0, &fpsa };
DIB fpsb_dib = { 055, 0, 0, &fpsb };
UNIT fpsa_unit = { UDATA (NULL, 0, 0) };
UNIT fpsb_unit = { UDATA (NULL, 0, 0) };
DEVICE fpsa_dev = { "FPSA", &fpsa_unit, NULL, NULL, 1, 8, 16, 1, 8, 16,
    NULL, NULL, NULL, NULL, NULL, NULL, &fpsa_dib, 0 };
DEVICE fpsb_dev = { "FPSB", &fpsb_unit, NULL, NULL, 1, 8, 16, 1, 8, 16,
    NULL, NULL, NULL, NULL, NULL, NULL, &fpsb_dib, 0 };
