/* edit.c -- a nano-style screen editor: edit [-r rows] [-c cols] [file]
 * (SHELL_NOTES.md section 6). ANSI/VT100 terminal (24x80 unless -r/-c).
 *
 *   ^G help    ^O write out   ^W where is   ^K cut line   ^U paste
 *   ^X exit    ^_ go to line  ^C position   ^Y/^V page    ^L redraw
 *   arrows, ^F ^B ^P ^N, ^A ^E (line start/end), ^D delete, Backspace
 *
 * Text is a gap buffer in segment memory (SYS_SEGALLOC) mapped once into
 * the logical pages above the stack (SYS_MAPWIN), so it costs nothing in
 * the program image and is addressed as an ordinary char array. Console
 * input is raw (SYS_TTYRAW). The screen is drawn by comparing each row
 * with a copy of what the terminal already shows and sending only the
 * changed part: at 4800 baud a full 24x80 screen takes about 4 seconds.
 */
#include "uio.h"

#define MAXR 30
#define MAXC 100
#define CUTMAX 4096

static char *B;                     /* window: text [0,cap) + cut buffer */
static unsigned int cap, gs, ge;    /* gap [gs,ge); the cursor is at gs */
static char *cutb;
static unsigned int cutlen;
static int lastcut;

static int R = 24, C = 80, TR, W;   /* rows, cols, text rows, used width */
static unsigned int top;            /* first shown line's start */
static unsigned int wantcol, hoff;
static int modified;
static char fname[64], msg[100], lastsearch[40];
static char want[MAXR][MAXC], have[MAXR][MAXC];
static char wattr[MAXR], hattr[MAXR];

/* ---- gap buffer ---------------------------------------------------------- */

static unsigned int tlen(void) { return cap - (ge - gs); }
static int at(unsigned int i) { return (unsigned char)(i < gs ? B[i] : B[i + ge - gs]); }

static void movegap(unsigned int p) {
    while (gs > p) B[--ge] = B[--gs];
    while (gs < p) B[gs++] = B[ge++];
}

static int ins(int c) {
    if (gs == ge) return -1;
    B[gs++] = (char)c;
    modified = 1;
    return 0;
}

static unsigned int lstart(unsigned int i) {
    while (i > 0 && at(i - 1) != '\n') i--;
    return i;
}

static unsigned int lend(unsigned int i) {
    unsigned int n = tlen();
    while (i < n && at(i) != '\n') i++;
    return i;
}

static unsigned int cwidth(int c, unsigned int col) {
    if (c == '\t') return 8 - (col & 7);
    if (c < 32 || c == 127) return 2;
    return 1;
}

/* display column of position p in the line starting at ls */
static unsigned int colof(unsigned int ls, unsigned int p) {
    unsigned int col = 0;
    for (; ls < p; ls++) col += cwidth(at(ls), col);
    return col;
}

/* position in the line starting at ls closest to display column want */
static unsigned int posat(unsigned int ls, unsigned int wcol) {
    unsigned int e = lend(ls), col = 0, w;
    while (ls < e) {
        w = cwidth(at(ls), col);
        if (col + w > wcol) break;
        col += w;
        ls++;
    }
    return ls;
}

static unsigned int lineno(unsigned int p) {    /* 1-based */
    unsigned int i, n = 1;
    for (i = 0; i < p; i++) if (at(i) == '\n') n++;
    return n;
}

/* ---- terminal (ANSI) ----------------------------------------------------- */

static char obuf[1024];
static int olen;

static void oflush(void) { if (olen) write(1, obuf, olen); olen = 0; }
static void oc(int c) { obuf[olen++] = (char)c; if (olen == (int)sizeof obuf) oflush(); }
static void os(const char *s) { while (*s) oc(*s++); }
static void onum(unsigned int v) { if (v >= 10) onum(v / 10); oc('0' + (int)(v % 10)); }
static int tr = -1, tc = -1;       /* terminal cursor, -1 = unknown */
static void tgoto(int r, int c) {
    if (r == tr && c == tc) return;     /* already there: send nothing */
    oc(033); oc('['); onum((unsigned)r + 1); oc(';'); onum((unsigned)c + 1); oc('H');
    tr = r; tc = c;
}
static void tclreol(void) { os("\033[K"); }
static void tattr(int rev) { os(rev ? "\033[7m" : "\033[0m"); }

static void tclear(void) {
    int r, c;
    os("\033[0m\033[H\033[2J");
    tr = tc = 0;
    for (r = 0; r < R; r++) {
        for (c = 0; c < W; c++) have[r][c] = ' ';
        hattr[r] = 0;
    }
}

/* send rows that differ from what the terminal shows */
static void put(int r, int c) { oc(want[r][c]); tc = c + 1; }

static void flushscreen(int cr, int cc) {
    int r, c, f, l, wl, hl;
    for (r = 0; r < R; r++) {
        if (wattr[r] != hattr[r]) {
            tgoto(r, 0);
            tattr(wattr[r]);
            for (c = 0; c < W; c++) put(r, c);
            if (wattr[r]) tattr(0);
        } else {
            for (f = 0; f < W && want[r][f] == have[r][f]; f++)
                ;
            if (f == W) continue;
            for (l = W - 1; want[r][l] == have[r][l]; l--)
                ;
            for (wl = W; wl > 0 && want[r][wl - 1] == ' '; wl--)
                ;
            for (hl = W; hl > 0 && have[r][hl - 1] == ' '; hl--)
                ;
            tgoto(r, f);
            if (wattr[r]) tattr(1);
            if (l < wl || wattr[r]) {
                for (c = f; c <= l; c++) put(r, c);
            } else {
                for (c = f; c < wl; c++) put(r, c);
                if (hl > wl) tclreol();
            }
            if (wattr[r]) tattr(0);
        }
        for (c = 0; c < W; c++) have[r][c] = want[r][c];
        hattr[r] = wattr[r];
    }
    tgoto(cr, cc);
    oflush();
}

static void rowtext(int r, int c, const char *s) {
    while (*s && c < W) want[r][c++] = *s++;
}

static void rowclear(int r, int rev) {
    int c;
    for (c = 0; c < W; c++) want[r][c] = ' ';
    wattr[r] = (char)rev;
}

/* ---- keys ---------------------------------------------------------------- */

#define K_UP    0400
#define K_DOWN  0401
#define K_LEFT  0402
#define K_RIGHT 0403
#define K_HOME  0404
#define K_END   0405
#define K_PGUP  0406
#define K_PGDN  0407
#define K_DEL   0410
#define CTL(x)  ((x) & 037)

static char ibuf[32];
static int ipos, ilen;

static int rawc(void) {
    if (ipos >= ilen) {
        oflush();
        ilen = read(0, ibuf, sizeof ibuf);
        ipos = 0;
        if (ilen <= 0) { ilen = 0; return -1; }
    }
    return (unsigned char)ibuf[ipos++];
}

static int getkey(void) {
    int c = rawc(), d, n;
    if (c != 033) return c;
    c = rawc();
    if (c != '[' && c != 'O') return 033;
    c = rawc();
    if (c == 'A') return K_UP;
    if (c == 'B') return K_DOWN;
    if (c == 'C') return K_RIGHT;
    if (c == 'D') return K_LEFT;
    if (c == 'H') return K_HOME;
    if (c == 'F') return K_END;
    if (c >= '0' && c <= '9') {             /* ESC [ n ~ */
        n = c - '0';
        while ((d = rawc()) >= '0' && d <= '9') n = n * 10 + d - '0';
        if (n == 1 || n == 7) return K_HOME;
        if (n == 4 || n == 8) return K_END;
        if (n == 3) return K_DEL;
        if (n == 5) return K_PGUP;
        if (n == 6) return K_PGDN;
    }
    return 0;
}

/* ---- display ------------------------------------------------------------- */

static void setmsg(const char *a, const char *b, const char *c2) {
    int n = 0;
    while (*a && n < 95) msg[n++] = *a++;
    while (b && *b && n < 95) msg[n++] = *b++;
    while (c2 && *c2 && n < 95) msg[n++] = *c2++;
    msg[n] = 0;
}

static char *utoa(unsigned int v, char *e) {    /* writes backwards from e */
    *e = 0;
    do { *--e = (char)('0' + v % 10); v /= 10; } while (v);
    return e;
}

/* keep the cursor on screen: scroll by half a screen at a time */
static void scrollfix(void) {
    unsigned int cl = lstart(gs), p, n, col;
    if (cl < top) {
        top = cl;
        for (n = 0; n < (unsigned)TR / 2 && top > 0; n++) top = lstart(top - 1);
    } else {
        for (p = top, n = 0; p < cl; p++) if (at(p) == '\n') n++;
        if (n >= (unsigned)TR) {
            top = cl;
            for (n = 0; n < (unsigned)TR / 2 && top > 0; n++) top = lstart(top - 1);
        }
    }
    col = colof(cl, gs);
    if (col < hoff || col >= hoff + (unsigned)W - 1)
        hoff = col > (unsigned)W / 2 ? col - (unsigned)W / 2 : 0;
}

static const char *help1 = "^G Get Help  ^O Write Out ^W Where Is  ^K Cut Line  ^C Cur Pos";
static const char *help2 = "^X Exit      ^_ Go To Line ^Y Prev Page ^U Paste    ^V Next Page";

static void draw(void) {
    unsigned int p, n = tlen(), col, cl;
    int r, c, cr = 1, cc = 0, more = 1, ch, w, k;
    scrollfix();
    p = top;                        /* after scrollfix: it may move top */
    rowclear(0, 1);
    rowtext(0, 2, "EDIT 1.0");
    rowtext(0, W / 2 - 8, fname[0] ? fname : "New Buffer");
    if (modified) rowtext(0, W - 10, "Modified");
    cl = lstart(gs);
    for (r = 1; r <= TR; r++) {
        rowclear(r, 0);
        if (!more) continue;
        if (p == cl) { cr = r; cc = (int)(colof(cl, gs) - hoff); }
        col = 0;
        while (p < n && (ch = at(p)) != '\n') {
            w = (int)cwidth(ch, col);
            for (k = 0; k < w; k++) {
                c = (int)(col + (unsigned)k) - (int)hoff;
                if (c < 0) continue;
                if (c >= W - 1) { want[r][W - 1] = '$'; continue; }
                want[r][c] = ch == '\t' ? ' ' : (ch < 32 || ch == 127)
                    ? (k == 0 ? '^' : (char)(ch ^ 0100)) : (char)ch;
            }
            col += (unsigned)w;
            p++;
        }
        if (p < n) p++;             /* past the newline */
        else more = 0;
    }
    rowclear(TR + 1, 0);
    rowtext(TR + 1, 0, msg);
    rowclear(TR + 2, 0);
    rowtext(TR + 2, 0, help1);
    rowclear(TR + 3, 0);
    rowtext(TR + 3, 0, help2);
    flushscreen(cr, cc < 0 ? 0 : cc);
}

/* one-line prompt on the message row; 1 = accepted, 0 = cancelled */
static int prompt(const char *what, char *buf, int max) {
    int n = ustrlen(buf), k, pl;
    for (;;) {
        rowclear(TR + 1, 1);
        rowtext(TR + 1, 0, what);
        pl = ustrlen(what);
        rowtext(TR + 1, pl, ": ");
        rowtext(TR + 1, pl + 2, buf);
        rowclear(TR + 2, 0);
        rowtext(TR + 2, 0, "^C Cancel   Enter Accept");
        rowclear(TR + 3, 0);
        flushscreen(TR + 1, pl + 2 + n < W ? pl + 2 + n : W - 1);
        k = getkey();
        if (k == '\r' || k == '\n') return 1;
        if (k == CTL('C') || k < 0) return 0;
        if ((k == 010 || k == 0177) && n > 0) buf[--n] = 0;
        else if (k >= ' ' && k < 0177 && n < max - 1) { buf[n++] = (char)k; buf[n] = 0; }
    }
}

/* ---- file I/O ------------------------------------------------------------ */

static unsigned int countnl(void) {
    unsigned int i, n = tlen(), l = 0;
    for (i = 0; i < n; i++) if (at(i) == '\n') l++;
    if (n && at(n - 1) != '\n') l++;
    return l;
}

static int writefile(const char *name) {
    int fd = open(name, O_WRONLY | O_CREAT | O_TRUNC);
    char t[8];
    if (fd < 0) { setmsg("[ Cannot write ", name, " ]"); return -1; }
    if ((gs && write(fd, B, (int)gs) != (int)gs) ||
        (ge < cap && write(fd, B + ge, (int)(cap - ge)) != (int)(cap - ge))) {
        close(fd);
        setmsg("[ Error writing ", name, " ]");
        return -1;
    }
    close(fd);
    modified = 0;
    setmsg("[ Wrote ", utoa(countnl(), t + 7), " lines ]");
    return 0;
}

/* load name into the empty buffer: 0, -1 cannot open, -2 too big */
static int readfile(const char *name) {
    int fd = open(name, O_RDONLY), n;
    if (fd < 0) return -1;
    for (;;) {
        n = read(fd, B + gs, ge - gs > 512 ? 512 : (int)(ge - gs));
        if (n <= 0) break;
        gs += (unsigned int)n;
        if (gs == ge) {
            char c;
            n = read(fd, &c, 1);
            close(fd);
            return n > 0 ? -2 : 0;
        }
    }
    close(fd);
    movegap(0);
    return 0;
}

/* ---- commands ------------------------------------------------------------ */

static void up(void) {
    unsigned int ls = lstart(gs);
    if (ls == 0) return;
    movegap(posat(lstart(ls - 1), wantcol));
}

static void down(void) {
    unsigned int le = lend(gs);
    if (le >= tlen()) return;
    movegap(posat(le + 1, wantcol));
}

static void setwant(void) { wantcol = colof(lstart(gs), gs); }

static void cutline(void) {
    unsigned int ls = lstart(gs), le = lend(gs), n;
    if (ls == tlen()) { setmsg("[ Nothing to cut ]", 0, 0); return; }
    if (le < tlen()) le++;                  /* the newline too */
    if (!lastcut) cutlen = 0;
    movegap(ls);
    n = le - ls;
    while (n-- > 0) {
        if (cutlen < CUTMAX) cutb[cutlen++] = B[ge];
        ge++;
    }
    modified = 1;
    setwant();
}

static void paste(void) {
    unsigned int i;
    for (i = 0; i < cutlen; i++)
        if (ins(cutb[i]) < 0) { setmsg("[ Buffer full ]", 0, 0); break; }
    setwant();
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int matchat(unsigned int p, const char *s) {
    unsigned int n = tlen();
    for (; *s; s++, p++)
        if (p >= n || lower(at(p)) != lower(*s)) return 0;
    return 1;
}

static void search(void) {
    char s[40];
    unsigned int p, n = tlen(), start = gs;
    int i;
    for (i = 0; (s[i] = lastsearch[i]) != 0; i++)
        ;
    if (!prompt("Search", s, sizeof s) || !s[0]) { setmsg("[ Cancelled ]", 0, 0); return; }
    for (i = 0; (lastsearch[i] = s[i]) != 0; i++)
        ;
    for (p = start + 1; p < n; p++)
        if (matchat(p, s)) { movegap(p); setmsg("", 0, 0); setwant(); return; }
    for (p = 0; p <= start && p < n; p++)
        if (matchat(p, s)) {
            movegap(p);
            setmsg(p == start ? "[ This is the only occurrence ]" : "[ Search Wrapped ]", 0, 0);
            setwant();
            return;
        }
    setmsg("[ \"", s, "\" not found ]");
}

static void gotoline(void) {
    char s[8];
    unsigned int want_l, p = 0, n = tlen(), l = 1;
    s[0] = 0;
    if (!prompt("Enter line number", s, sizeof s) || atou(s) <= 0) { setmsg("[ Cancelled ]", 0, 0); return; }
    want_l = (unsigned int)atou(s);
    while (l < want_l && p < n) { if (at(p) == '\n') l++; p++; }
    movegap(p);
    wantcol = 0;
    setmsg("", 0, 0);
}

static void curpos(void) {
    char a[8], b[8], c[8], d[8];
    unsigned int ls = lstart(gs);
    setmsg("[ line ", utoa(lineno(gs), a + 7), "/");
    setmsg(msg, utoa(countnl(), b + 7), ", col ");
    setmsg(msg, utoa(colof(ls, gs) + 1, c + 7), ", char ");
    setmsg(msg, utoa(gs, d + 7), "/");
    setmsg(msg, utoa(tlen(), a + 7), " ]");
}

static const char *helptext[] = {
    "EDIT help",
    "",
    "Type to insert text. Enter splits the line; Backspace and ^D delete.",
    "",
    "^G  this help           ^X  exit (asks to save a modified buffer)",
    "^O  write the file      ^W  search (case-insensitive, wraps)",
    "^K  cut the line        ^U  paste the cut lines (^K again adds)",
    "^_  go to line number   ^C  show the cursor position",
    "^A  start of line       ^E  end of line",
    "^Y  previous page       ^V  next page",
    "^F ^B ^P ^N  or the arrow keys move the cursor",
    "^L  redraw the screen",
    "",
    "In a prompt: Enter accepts, ^C cancels.",
    "",
    "Press any key to return to the text.",
};

static void help(void) {
    int r, i;
    for (r = 0; r < R; r++) rowclear(r, 0);
    rowclear(0, 1);
    for (i = 0; i < (int)(sizeof helptext / sizeof helptext[0]) && i + 1 < R; i++)
        rowtext(i + (i ? 1 : 0), i ? 1 : 2, helptext[i]);
    flushscreen(R - 1, 0);
    getkey();
}

/* 1 = leave the editor */
static int quit(void) {
    int k;
    char s[64];
    if (!modified) return 1;
    for (;;) {
        rowclear(TR + 1, 1);
        rowtext(TR + 1, 0, "Save modified buffer? (Y/N, ^C cancels)");
        rowclear(TR + 2, 0);
        rowclear(TR + 3, 0);
        flushscreen(TR + 1, 40);
        k = getkey();
        if (k == 'n' || k == 'N') return 1;
        if (k == CTL('C') || k < 0) { setmsg("[ Cancelled ]", 0, 0); return 0; }
        if (k == 'y' || k == 'Y') break;
    }
    for (k = 0; (s[k] = fname[k]) != 0; k++)
        ;
    if (!prompt("File Name to Write", s, sizeof s) || !s[0]) { setmsg("[ Cancelled ]", 0, 0); return 0; }
    if (writefile(s) < 0) return 0;
    return 1;
}

static void writeout(void) {
    char s[64];
    int k;
    for (k = 0; (s[k] = fname[k]) != 0; k++)
        ;
    if (!prompt("File Name to Write", s, sizeof s) || !s[0]) { setmsg("[ Cancelled ]", 0, 0); return; }
    if (writefile(s) == 0)
        for (k = 0; (fname[k] = s[k]) != 0; k++)
            ;
}

/* ---- setup --------------------------------------------------------------- */

/* Map as many segment pages as fit between the stack and 32KW. The stack
 * is the 1024 words (build.sh) above the program; the kernel refuses a
 * window over any image or stack page, so walk up until one is taken. */
static int mapbuffer(void) {
    char here;
    int first = (int)((((unsigned int)&here >> 1) + 1024) >> 10) + 1, n;
    int segn = 0;
    for (; first < 31; first++) {
        n = 32 - first;
        if (segn < n) {                     /* grow the segment to n pages */
            segn = sys_segalloc(n - segn);
            if (segn < 0) return -1;
        }
        if (sys_mapwin(first, 0, n) == 0) {
            B = (char *)((unsigned int)first * 2048u);
            cap = (unsigned int)n * 2048u - CUTMAX;
            cutb = B + cap;
            gs = 0;
            ge = cap;
            return 0;
        }
    }
    return -1;
}

int main(int argc, char **argv) {
    int i = 1, k, r;
    char t[8];
    for (; i + 1 < argc && argv[i][0] == '-'; i += 2) {
        if (argv[i][1] == 'r') R = atou(argv[i + 1]);
        else if (argv[i][1] == 'c') C = atou(argv[i + 1]);
        else break;
    }
    if (R < 8 || R > MAXR || C < 40 || C > MAXC || i < argc - 1) {
        err2("usage: edit [-r rows 8-30] [-c cols 40-100] [file]", "");
        return 2;
    }
    TR = R - 4;
    W = C - 1;                      /* never write the last column: no wrap */
    if (mapbuffer() < 0) { err2("edit: no memory for the text", ""); return 1; }
    if (i < argc) {
        for (k = 0; k < 63 && (fname[k] = argv[i][k]) != 0; k++)
            ;
        fname[k] = 0;
        r = readfile(fname);
        if (r == -2) { err2("edit: file too big: ", fname); return 1; }
        if (r == -1) setmsg("[ New File ]", 0, 0);
        else setmsg("[ Read ", utoa(countnl(), t + 7), " lines ]");
    }
    ttyraw(1);
    tclear();
    for (;;) {
        draw();
        k = getkey();
        if (k != CTL('K')) lastcut = 0;
        if (k != CTL('C')) msg[0] = 0;
        if (k < 0) break;
        if (k == CTL('X')) { if (quit()) break; continue; }
        switch (k) {
        case CTL('G'): help(); break;
        case CTL('O'): writeout(); break;
        case CTL('W'): search(); break;
        case CTL('K'): cutline(); lastcut = 1; break;
        case CTL('U'): paste(); break;
        case CTL('C'): curpos(); break;
        case CTL('_'): gotoline(); break;
        case CTL('L'): tclear(); break;
        case CTL('A'): case K_HOME: movegap(lstart(gs)); setwant(); break;
        case CTL('E'): case K_END: movegap(lend(gs)); setwant(); break;
        case CTL('B'): case K_LEFT: if (gs > 0) movegap(gs - 1); setwant(); break;
        case CTL('F'): case K_RIGHT: if (gs < tlen()) movegap(gs + 1); setwant(); break;
        case CTL('P'): case K_UP: up(); break;
        case CTL('N'): case K_DOWN: down(); break;
        case CTL('Y'): case K_PGUP:
            for (r = 0; r < TR - 2; r++) { up(); if (top > 0) top = lstart(top - 1); }
            break;
        case CTL('V'): case K_PGDN:
            for (r = 0; r < TR - 2; r++) {
                down();
                if (lend(top) < tlen()) top = lend(top) + 1;
            }
            break;
        case CTL('D'): case K_DEL:
            if (ge < cap) { ge++; modified = 1; }
            break;
        case 010: case 0177:
            if (gs > 0) { gs--; modified = 1; }
            setwant();
            break;
        case '\r': case '\n':
            if (ins('\n') < 0) setmsg("[ Buffer full ]", 0, 0);
            setwant();
            break;
        default:
            if ((k >= ' ' && k < 0177) || k == '\t') {
                if (ins(k) < 0) setmsg("[ Buffer full ]", 0, 0);
                setwant();
            }
        }
    }
    os("\033[0m\033[H\033[2J");
    oflush();
    ttyraw(0);
    return 0;
}
