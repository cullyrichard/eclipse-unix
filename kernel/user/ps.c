/* ps.c -- list processes: PID PPID STATE PAGES NAME (SYS_PSINFO). */
#include "uio.h"

static const char *states[] = { "free", "run", "zombie", "wait", "sleep", "new" };

int main(void) {
    unsigned int w[9];
    int pid, j, c;
    out_s("  PID PPID STATE  PAGES NAME\n");
    for (pid = 0; psinfo(pid, w) == 0; pid++) {
        if (w[0] == 0) continue;
        out_uw((unsigned int)pid, 5);
        out_c(' ');
        if ((int)w[1] < 0) out_s("   -");
        else out_uw(w[1], 4);
        out_c(' ');
        out_s(w[0] < 6 ? states[w[0]] : "?");
        for (j = ustrlen(w[0] < 6 ? states[w[0]] : "?"); j < 6; j++) out_c(' ');
        out_uw(w[2], 6);
        out_c(' ');
        for (j = 0; j < 12; j++) {
            c = (int)((w[3 + j / 2] >> ((j & 1) ? 0 : 8)) & 0377);
            if (c == 0) break;
            out_c(c);
        }
        out_c('\n');
    }
    out_flush();
    return 0;
}
