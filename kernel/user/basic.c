/* basic.c -- a classic line-numbered BASIC for the Eclipse (SHELL_NOTES.md,
 * "Userland"). Executes the stored program text directly (no tokenizer).
 *
 * Commands:   RUN  LIST [n[-m]]  NEW  LOAD "file"  SAVE "file"  BYE
 *             (a line starting with a number edits the program; a bare
 *             number deletes that line; anything else runs immediately)
 * Statements: LET  PRINT (;  ,  TAB(n))  INPUT ["prompt";] v,...  IF c THEN
 *             stmt|line  GOTO  GOSUB  RETURN  FOR v=a TO b [STEP s]  NEXT [v]
 *             ON e GOTO l,...  DIM a(n[,m])  READ  DATA  RESTORE  REM  END
 *             STOP  RANDOMIZE; several statements per line with ':'
 * Values:     floats A-Z, A0-Z9; strings A$-Z$ (up to 80 chars); numeric
 *             arrays A(..)-Z(..), 1 or 2 dimensions, default size 10
 * Operators:  + - * / ^  = <> < > <= >=  AND OR NOT; strings: + and compare
 * Functions:  ABS INT SGN SQR SIN COS TAN ATN EXP LOG RND LEN ASC VAL
 *             CHR$ STR$ LEFT$ RIGHT$ MID$
 * Floats are 32-bit IEEE (software); SIN/COS/ATN/EXP/LOG and number
 * printing are implemented here (the runtime has neither). */
#include "../usys.h"
#include <math.h>

/* Own floor/abs: clang turns floorf()/fabsf() into the ffloor/fabs
 * intrinsics, and in software-float mode the backend has no library call
 * registered for FFLOOR ("unsupported library call operation",
 * DEBUGGING_NOTES.md #46). Same method as the runtime's floorf. */
static float bfloor(float f) {
    long i = (long)f;
    float t = (float)i;
    return f < t ? t - 1.0f : t;
}
static float babs(float f) { return f < 0.0f ? -f : f; }

#ifndef PROG
#define PROG     5000           /* program text bytes */
#endif
#ifndef MAXLINES
#define MAXLINES 250
#endif
#define SLEN     80
#ifndef ASIZE
#define ASIZE    500            /* numeric array cells, all arrays */
#endif
#define NFOR     12
#ifndef STACKUSE
#define STACKUSE 1800           /* words of stack nexpr/sexpr may start at;
                                   build.sh gives basic 2560 (see deep()) */
#endif
#define NGOSUB   24

/* ---- program store ----------------------------------------------------- */
static char prog[PROG];
static unsigned int ptop;
static unsigned int lnum[MAXLINES], loff[MAXLINES];   /* sorted by number */
static int nlines;

/* ---- run state ------------------------------------------------------------ */
static const char *tp;           /* text pointer */
static int curl;                 /* index of the line executing, -1 = immediate */
static int running, stopped;
static char ibuf[160];
static float nv[26 * 11];
static char sv[26][SLEN + 1];
static float arr[ASIZE];
static unsigned int abase[26], adim1[26], adim2[26], aused;
static int fvar[NFOR], fline[NFOR];
static const char *fpos[NFOR];
static float flim[NFOR], fstep[NFOR];
static int nfor;
static int gline[NGOSUB];
static const char *gpos[NGOSUB];
static int ngosub;
static int dline;                /* READ position: line index and pointer */
static const char *dpos;
static unsigned int seed = 12345;
static int col;                  /* output column, for , and TAB */
static int errflag;
static int jumped;               /* control moved: tp/curl were set */

/* ---- output ------------------------------------------------------------------ */
static void outc(char c) {
    write(1, &c, 1);
    if (c == '\n') col = 0; else col++;
}
static void outs(const char *s) { while (*s) outc(*s++); }

static void error(const char *m) {
    if (errflag) return;
    errflag = 1;
    if (col) outc('\n');
    outs("?"); outs(m); outs(" ERROR");
    if (curl >= 0) { outs(" IN "); { char b[8]; int n = 0; unsigned int v = lnum[curl];
        do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v); while (n) outc(b[--n]); } }
    outc('\n');
}

/* ---- floats: formatting and math ----------------------------------------------- */
static float pow10i(int e) {
    float r = 1.0f, b = 10.0f;
    int neg = e < 0;
    if (neg) e = -e;
    while (e) { if (e & 1) r *= b; b *= b; e >>= 1; }
    return neg ? 1.0f / r : r;
}

static void fmtnum(float v, char *out) {
    /* BASIC style: " 12", "-3.5", " 1.23457E+08"; 6 significant digits */
    char d[8];
    int n = 0, e = 0, i, k, neg = 0;
    unsigned long m;
    if (v < 0) { neg = 1; v = -v; }
    out[n++] = neg ? '-' : ' ';
    if (v == 0.0f) { out[n++] = '0'; out[n] = 0; return; }
    if (v == bfloor(v) && v < 1000000.0f) {             /* integer */
        unsigned long iv = (unsigned long)v;
        char t[8];
        k = 0;
        do { t[k++] = (char)('0' + iv % 10); iv /= 10; } while (iv);
        while (k) out[n++] = t[--k];
        out[n] = 0;
        return;
    }
    while (v >= 10.0f) { v /= 10.0f; e++; }
    while (v < 1.0f) { v *= 10.0f; e--; }
    m = (unsigned long)(v * 100000.0f + 0.5f);          /* 6 digits */
    if (m >= 1000000UL) { m /= 10; e++; }
    for (i = 5; i >= 0; i--) { d[i] = (char)('0' + m % 10); m /= 10; }
    k = 6;
    while (k > 1 && d[k - 1] == '0') k--;               /* drop trailing zeros */
    if (e >= -1 && e < 6) {
        if (e < 0) { out[n++] = '.'; for (i = 0; i < k; i++) out[n++] = d[i]; }
        else {
            for (i = 0; i <= e; i++) out[n++] = i < k ? d[i] : '0';
            if (k > e + 1) { out[n++] = '.'; for (i = e + 1; i < k; i++) out[n++] = d[i]; }
        }
    } else {
        out[n++] = d[0];
        if (k > 1) { out[n++] = '.'; for (i = 1; i < k; i++) out[n++] = d[i]; }
        out[n++] = 'E';
        out[n++] = e < 0 ? '-' : '+';
        if (e < 0) e = -e;
        out[n++] = (char)('0' + e / 10);
        out[n++] = (char)('0' + e % 10);
    }
    out[n] = 0;
}

#define PI 3.14159265f

static float bsin(float x) {
    float x2, r;
    int neg = 0;
    x = x - 2.0f * PI * bfloor(x / (2.0f * PI));        /* [0, 2pi) */
    if (x > PI) { x -= PI; neg = 1; }                    /* [0, pi] */
    if (x > PI / 2) x = PI - x;                          /* [0, pi/2] */
    x2 = x * x;
    r = x * (1.0f - x2 / 6.0f * (1.0f - x2 / 20.0f * (1.0f - x2 / 42.0f * (1.0f - x2 / 72.0f))));
    return neg ? -r : r;
}
static float bcos(float x) { return bsin(x + PI / 2); }

static float batn(float x) {
    float r, x2;
    int neg = 0, inv = 0, red = 0;
    if (x < 0) { x = -x; neg = 1; }
    if (x > 1.0f) { x = 1.0f / x; inv = 1; }
    if (x > 0.2679492f) { x = (x * 1.7320508f - 1.0f) / (x + 1.7320508f); red = 1; }  /* tan(pi/12) */
    x2 = x * x;
    r = x * (1.0f - x2 * (0.3333333f - x2 * (0.2f - x2 * 0.1428571f)));
    if (red) r += PI / 6;
    if (inv) r = PI / 2 - r;
    return neg ? -r : r;
}

static float bexp(float x) {
    /* e^x = 2^k * e^r, r in [-ln2/2, ln2/2] */
    float r, s;
    int k;
    if (x > 88.0f) { error("OVERFLOW"); return 0.0f; }
    if (x < -88.0f) return 0.0f;
    k = (int)bfloor(x / 0.6931472f + 0.5f);
    r = x - (float)k * 0.6931472f;
    s = 1.0f + r * (1.0f + r / 2.0f * (1.0f + r / 3.0f * (1.0f + r / 4.0f * (1.0f + r / 5.0f * (1.0f + r / 6.0f)))));
    while (k > 0) { s *= 2.0f; k--; }
    while (k < 0) { s *= 0.5f; k++; }
    return s;
}

static float blog(float x) {
    /* x = m * 2^e with m in [sqrt(.5), sqrt(2)); ln m by atanh series */
    float m, t, t2, r;
    int e = 0;
    if (x <= 0.0f) { error("ILLEGAL QUANTITY"); return 0.0f; }
    m = x;
    while (m >= 1.4142136f) { m *= 0.5f; e++; }
    while (m < 0.7071068f) { m *= 2.0f; e--; }
    t = (m - 1.0f) / (m + 1.0f);
    t2 = t * t;
    r = 2.0f * t * (1.0f + t2 * (0.3333333f + t2 * (0.2f + t2 * (0.1428571f + t2 * 0.1111111f))));
    return r + (float)e * 0.6931472f;
}

static float bpow(float a, float b) {
    float r = 1.0f;
    int n, neg;
    if (b == bfloor(b) && b > -64.0f && b < 64.0f) {    /* integer power */
        n = (int)b;
        neg = n < 0;
        if (neg) n = -n;
        while (n--) r *= a;
        return neg ? 1.0f / r : r;
    }
    if (a <= 0.0f) { error("ILLEGAL QUANTITY"); return 0.0f; }
    return bexp(b * blog(a));
}

static float parsenum(const char **pp) {
    const char *p = *pp;
    float v = 0.0f, f;
    int e = 0, es = 1, any = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10.0f + (float)(*p++ - '0'); any = 1; }
    if (*p == '.') {
        p++;
        f = 0.1f;
        while (*p >= '0' && *p <= '9') { v += f * (float)(*p++ - '0'); f *= 0.1f; any = 1; }
    }
    if (any && (*p == 'E' || *p == 'e')) {
        const char *q = p + 1;
        if (*q == '+' || *q == '-') { es = (*q == '-') ? -1 : 1; q++; }
        if (*q >= '0' && *q <= '9') {
            p = q;
            while (*p >= '0' && *p <= '9') e = e * 10 + (*p++ - '0');
            v *= pow10i(es * e);
        }
    }
    *pp = p;
    return v;
}

/* ---- scanning ---------------------------------------------------------------------- */
static void skip(void) { while (*tp == ' ' || *tp == '\t') tp++; }

static int upc(char c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

/* match keyword kw (upper case) at tp, advance past it */
static int kw(const char *k) {
    const char *p;
    skip();
    p = tp;
    while (*k) { if (upc(*p) != *k) return 0; p++; k++; }
    tp = p;
    return 1;
}

static int accept(char c) { skip(); if (*tp == c) { tp++; return 1; } return 0; }

/* end of statement */
static int eos(void) { skip(); return *tp == 0 || *tp == ':'; }

/* a variable name at tp: letter [digit] [$]; returns 0..285 numeric index,
 * 1000+letter for a string, -1 if none. *isarr set if followed by '(' */
static int varname(int *isarr) {
    int l, d = 0, v;
    skip();
    if (upc(*tp) < 'A' || upc(*tp) > 'Z') return -1;
    l = upc(*tp) - 'A';
    {   /* a keyword starting here is not a variable */
        const char *p = tp;
        int n = 0;
        while (upc(p[n]) >= 'A' && upc(p[n]) <= 'Z') n++;
        if (n > 1) return -1;
    }
    tp++;
    if (*tp >= '0' && *tp <= '9') d = *tp++ - '0' + 1;
    if (*tp == '$') { tp++; v = 1000 + l; }
    else v = l * 11 + d;
    skip();
    *isarr = (*tp == '(' && v < 1000);
    return v;
}

/* ---- expressions ---------------------------------------------------------------------- */
static float nexpr(void);
static void sexpr(char *out);

static int isstrstart(void) {
    const char *p;
    skip();
    if (*tp == '"') return 1;
    if (upc(tp[0]) >= 'A' && upc(tp[0]) <= 'Z') {
        p = tp + 1;
        if (*p >= '0' && *p <= '9') p++;
        if (*p == '$') return 1;
        p = tp;
        while (upc(*p) >= 'A' && upc(*p) <= 'Z') p++;
        if (*p == '$' && p - tp > 1) return 1;          /* CHR$ STR$ ... */
    }
    return 0;
}

static unsigned int acell(int l) {
    /* array element index for array l at tp ('(' already next) */
    int i, j = 0;
    unsigned int d1, d2;
    if (!abase[l] && !adim1[l]) {                        /* auto-DIM 10 */
        if (aused + 11 > ASIZE) { error("OUT OF MEMORY"); return 0; }
        abase[l] = aused + 1; adim1[l] = 11; adim2[l] = 1; aused += 11;
    }
    accept('(');
    i = (int)nexpr();
    if (accept(',')) j = (int)nexpr();
    if (!accept(')')) { error("SYNTAX"); return 0; }
    d1 = adim1[l]; d2 = adim2[l];
    if (i < 0 || (unsigned int)i >= d1 || j < 0 || (unsigned int)j >= d2) { error("SUBSCRIPT"); return 0; }
    return abase[l] - 1 + (unsigned int)i * d2 + (unsigned int)j;
}

static float fn1(float (*f)(float)) {
    float v;
    if (!accept('(')) { error("SYNTAX"); return 0.0f; }
    v = nexpr();
    if (!accept(')')) error("SYNTAX");
    return f(v);
}

/* String-argument functions and string comparison keep their SLEN-byte
 * temporaries in their own non-inlined frames: inlined into nprimary/nrel
 * they cost every level of parenthesis nesting ~200 words of stack. */
__attribute__((noinline)) static float strfn(int which) {
    char s[SLEN + 1];
    const char *p;
    accept('('); sexpr(s); accept(')');
    if (which == 0) return (float)ustrlen(s);
    if (which == 1) return (float)((unsigned int)s[0] & 255);
    p = s;
    while (*p == ' ') p++;
    if (*p == '-') { p++; return -parsenum(&p); }
    return parsenum(&p);
}

/* Expressions nest through nexpr and sexpr; a word address of a local
 * against main's measures the stack actually in use (it grows up), so
 * any mix of parentheses and string functions is bounded, not just one
 * kind. STACKUSE leaves room under the 2560-word stack for the deepest
 * call chain below one level (fmtnum, soft float, syscalls). */
static unsigned int sbase;

static int deep(void) {
    char here;
#ifndef __ECLIPSE__                     /* host test build: stacks grow down */
    return 0;
#endif
    if (((unsigned int)&here >> 1) - sbase > STACKUSE) {
        error("FORMULA TOO COMPLEX");
        return 1;
    }
    return 0;
}

static float nprimary(void) {
    float v;
    int va, isarr;
    skip();
    if ((*tp >= '0' && *tp <= '9') || *tp == '.') return parsenum(&tp);
    if (accept('(')) { v = nexpr(); if (!accept(')')) error("SYNTAX"); return v; }
    if (accept('-')) return -nprimary();
    if (accept('+')) return nprimary();
    if (kw("ABS")) { v = fn1(babs); return v; }
    if (kw("INT")) return fn1(bfloor);
    if (kw("SQR")) { if (!accept('(')) { error("SYNTAX"); return 0; } v = nexpr(); accept(')');
                     if (v < 0) { error("ILLEGAL QUANTITY"); return 0; } return sqrtf(v); }
    if (kw("SGN")) { if (!accept('(')) { error("SYNTAX"); return 0; } v = nexpr(); accept(')');
                     return v > 0 ? 1.0f : (v < 0 ? -1.0f : 0.0f); }
    if (kw("SIN")) return fn1(bsin);
    if (kw("COS")) return fn1(bcos);
    if (kw("TAN")) { if (!accept('(')) { error("SYNTAX"); return 0; } v = nexpr(); accept(')');
                     return bsin(v) / bcos(v); }
    if (kw("ATN")) return fn1(batn);
    if (kw("EXP")) return fn1(bexp);
    if (kw("LOG")) return fn1(blog);
    if (kw("RND")) {
        if (accept('(')) { nexpr(); accept(')'); }
        seed = seed * 25173u + 13849u;
        return (float)(seed >> 1) / 32768.0f;
    }
    if (kw("LEN")) return strfn(0);
    if (kw("ASC")) return strfn(1);
    if (kw("VAL")) return strfn(2);
    va = varname(&isarr);
    if (va < 0 || va >= 1000) { error("SYNTAX"); return 0.0f; }
    if (isarr) return arr[acell(va / 11)];
    return nv[va];
}

static float npower(void) {
    float v = nprimary();
    while (accept('^')) v = bpow(v, nprimary());
    return v;
}

static float nterm(void) {
    float v = npower(), r;
    for (;;) {
        if (accept('*')) v *= npower();
        else if (accept('/')) {
            r = npower();
            if (r == 0.0f) { error("DIVISION BY ZERO"); return 0.0f; }
            v /= r;
        } else return v;
    }
}

static float nsum(void) {
    float v = nterm();
    for (;;) {
        if (accept('+')) v += nterm();
        else if (accept('-')) v -= nterm();
        else return v;
    }
}

static int relop(void) {
    /* 1 = 2 <> 3 < 4 > 5 <= 6 >= ; 0 none */
    skip();
    if (tp[0] == '=') { tp++; return 1; }
    if (tp[0] == '<' && tp[1] == '>') { tp += 2; return 2; }
    if (tp[0] == '<' && tp[1] == '=') { tp += 2; return 5; }
    if (tp[0] == '>' && tp[1] == '=') { tp += 2; return 6; }
    if (tp[0] == '<') { tp++; return 3; }
    if (tp[0] == '>') { tp++; return 4; }
    return 0;
}

static int cmpres(int op, int c) {
    return op == 1 ? c == 0 : op == 2 ? c != 0 : op == 3 ? c < 0 :
           op == 4 ? c > 0 : op == 5 ? c <= 0 : c >= 0;
}

__attribute__((noinline)) static float strrel(void) {
    char s1[SLEN + 1], s2[SLEN + 1];
    int op;
    sexpr(s1);
    op = relop();
    if (!op) { error("TYPE MISMATCH"); return 0.0f; }
    sexpr(s2);
    return cmpres(op, ustrcmp(s1, s2)) ? -1.0f : 0.0f;
}

static float nrel(void) {
    float a, b;
    int op;
    if (isstrstart()) return strrel();                  /* string comparison */
    a = nsum();
    op = relop();
    if (!op) return a;
    b = nsum();
    return cmpres(op, a < b ? -1 : (a > b ? 1 : 0)) ? -1.0f : 0.0f;
}

static float nnot(void) {
    if (kw("NOT")) return nnot() == 0.0f ? -1.0f : 0.0f;
    return nrel();
}

static float nand(void) {
    float v = nnot();
    while (kw("AND")) { float w = nnot(); v = (v != 0.0f && w != 0.0f) ? -1.0f : 0.0f; }
    return v;
}

static float nexpr(void) {
    float v;
    if (deep()) return 0.0f;
    v = nand();
    while (kw("OR")) { float w = nand(); v = (v != 0.0f || w != 0.0f) ? -1.0f : 0.0f; }
    return v;
}

static void scopy(char *d, const char *s, int n) {
    int i;
    for (i = 0; i < n && s[i] && i < SLEN; i++) d[i] = s[i];
    d[i] = 0;
}

static void sprimary(char *out) {
    char t[SLEN + 1];
    int va, isarr, n, m, len;
    skip();
    out[0] = 0;
    if (*tp == '"') {
        tp++;
        n = 0;
        while (*tp && *tp != '"') { if (n < SLEN) out[n++] = *tp; tp++; }
        out[n] = 0;
        if (*tp == '"') tp++;
        return;
    }
    if (kw("CHR$")) { accept('('); out[0] = (char)(int)nexpr(); out[1] = 0; accept(')'); return; }
    if (kw("STR$")) { accept('('); fmtnum(nexpr(), out); accept(')'); return; }
    if (kw("LEFT$")) { accept('('); sexpr(t); accept(','); n = (int)nexpr(); accept(')');
                       scopy(out, t, n < 0 ? 0 : n); return; }
    if (kw("RIGHT$")) { accept('('); sexpr(t); accept(','); n = (int)nexpr(); accept(')');
                        len = ustrlen(t); if (n > len) n = len; if (n < 0) n = 0;
                        scopy(out, t + len - n, n); return; }
    if (kw("MID$")) { accept('('); sexpr(t); accept(','); n = (int)nexpr(); m = SLEN;
                      if (accept(',')) m = (int)nexpr(); accept(')');
                      len = ustrlen(t); if (n < 1) n = 1; if (n > len + 1) n = len + 1;
                      scopy(out, t + n - 1, m < 0 ? 0 : m); return; }
    va = varname(&isarr);
    if (va < 1000) { error("TYPE MISMATCH"); return; }
    scopy(out, sv[va - 1000], SLEN);
}

static void sexpr(char *out) {
    char t[SLEN + 1];
    int n, i;
    out[0] = 0;
    if (deep()) return;
    sprimary(out);
    while (accept('+')) {
        sprimary(t);
        n = ustrlen(out);
        for (i = 0; t[i] && n < SLEN; i++) out[n++] = t[i];
        out[n] = 0;
    }
}

/* ---- program store -------------------------------------------------------------------- */
static int findline(unsigned int n, int *exact) {
    int lo = 0, hi = nlines;                           /* first index >= n */
    while (lo < hi) { int mid = (lo + hi) / 2; if (lnum[mid] < n) lo = mid + 1; else hi = mid; }
    *exact = lo < nlines && lnum[lo] == n;
    return lo;
}

static void delline(int i) {
    unsigned int off = loff[i], len = (unsigned int)ustrlen(prog + off) + 1, k;
    int j;
    for (k = off; k + len < ptop; k++) prog[k] = prog[k + len];
    ptop -= len;
    for (j = i; j < nlines - 1; j++) { lnum[j] = lnum[j + 1]; loff[j] = loff[j + 1]; }
    nlines--;
    for (j = 0; j < nlines; j++) if (loff[j] > off) loff[j] -= len;
}

static void putline(unsigned int n, const char *text) {
    int i, ex, len, k;
    i = findline(n, &ex);
    if (ex) { delline(i); i = findline(n, &ex); }
    while (*text == ' ') text++;
    if (*text == 0) return;                             /* bare number: delete */
    len = ustrlen(text);
    if (nlines >= MAXLINES || ptop + (unsigned int)len + 1 > PROG) { error("OUT OF MEMORY"); return; }
    for (k = nlines; k > i; k--) { lnum[k] = lnum[k - 1]; loff[k] = loff[k - 1]; }
    lnum[i] = n;
    loff[i] = ptop;
    for (k = 0; k <= len; k++) prog[ptop++] = text[k];
    nlines++;
}

/* ---- statements ------------------------------------------------------------------------- */
static void clearvars(void) {
    int i;
    for (i = 0; i < 26 * 11; i++) nv[i] = 0.0f;
    for (i = 0; i < 26; i++) { sv[i][0] = 0; abase[i] = adim1[i] = adim2[i] = 0; }
    aused = 0; nfor = 0; ngosub = 0;
    dline = 0; dpos = 0;
}

static void gotoline(unsigned int n) {
    int ex, i = findline(n, &ex);
    if (!ex) { error("UNDEFINED LINE"); return; }
    curl = i;
    tp = prog + loff[i];
    running = 1;
    jumped = 1;
}

static void doprint(void) {
    char s[SLEN + 1], nb[20];
    int nl = 1, n;
    float v;
    for (;;) {
        if (eos()) break;
        nl = 1;
        if (accept(';')) { nl = 0; continue; }
        if (accept(',')) { do outc(' '); while (col % 14); nl = 0; continue; }
        if (kw("TAB")) {
            accept('('); n = (int)nexpr(); accept(')');
            while (col < n) outc(' ');
            nl = 1;
            continue;
        }
        if (isstrstart()) {
            const char *st = tp;
            sexpr(s);
            skip();
            if (errflag) return;
            if (*tp != '<' && *tp != '>' && *tp != '=') { outs(s); continue; }
            tp = st;                    /* "A$ < B$": a numeric (truth) item */
        }
        v = nexpr();
        if (errflag) return;            /* no half-printed value after ?ERROR */
        fmtnum(v, nb); outs(nb); outc(' ');
    }
    if (nl) outc('\n');
}

static int readline(char *b, int max) {
    int n = read(0, b, max - 1);
    if (n <= 0) return -1;
    if (b[n - 1] == '\n') n--;
    b[n] = 0;
    return n;
}

static void assignfrom(int va, int isarr, const char **pp) {
    /* store the next input/data item at *pp into variable va */
    const char *p = *pp;
    while (*p == ' ') p++;
    if (va >= 1000) {
        char *d = sv[va - 1000];
        int n = 0;
        if (*p == '"') { p++; while (*p && *p != '"') { if (n < SLEN) d[n++] = *p; p++; } if (*p) p++; }
        else while (*p && *p != ',') { if (n < SLEN) d[n++] = *p; p++; }
        d[n] = 0;
    } else {
        float v;
        int neg = 0;
        if (*p == '-') { neg = 1; p++; } else if (*p == '+') p++;
        if (!((*p >= '0' && *p <= '9') || *p == '.')) { error("BAD DATA"); return; }
        v = parsenum(&p);
        if (neg) v = -v;
        (void)isarr;
        nv[va] = v;
    }
    while (*p == ' ') p++;
    *pp = p;
}

static void doinput(void) {
    char prompt[SLEN + 1];
    const char *p;
    int va, isarr;
    unsigned int cell = 0;
    prompt[0] = 0;
    if (*tp == '"' || (skip(), *tp == '"')) { sexpr(prompt); accept(';'); }
again:
    outs(prompt[0] ? prompt : ""); outs("? ");
    if (readline(ibuf, sizeof ibuf) < 0) { stopped = 1; return; }
    col = 0;
    p = ibuf;
    for (;;) {
        va = varname(&isarr);
        if (va < 0) { error("SYNTAX"); return; }
        if (isarr) cell = acell(va / 11);
        if (*p == 0) { outs("??"); if (readline(ibuf, sizeof ibuf) < 0) return; p = ibuf; }
        if (isarr) {
            float v; int neg = 0;
            while (*p == ' ') p++;
            if (*p == '-') { neg = 1; p++; }
            if (!((*p >= '0' && *p <= '9') || *p == '.')) { outs("?REDO\n"); goto again; }
            v = parsenum(&p); arr[cell] = neg ? -v : v;
        } else {
            const char *q = p;
            assignfrom(va, 0, &q);
            if (errflag) { errflag = 0; outs("?REDO\n"); goto again; }
            p = q;
        }
        while (*p == ' ') p++;
        if (*p == ',') p++;
        if (!accept(',')) break;
    }
}

/* advance the DATA cursor to the next item; 0 = out of data */
static int nextdata(void) {
    for (;;) {
        if (dpos) {
            while (*dpos == ' ') dpos++;
            if (*dpos == ',') { dpos++; return 1; }
            if (*dpos && *dpos != ':') return 1;
            dpos = 0;
            dline++;
        }
        for (; dline < nlines; dline++) {
            const char *p = prog + loff[dline];
            while (*p == ' ') p++;
            if (upc(p[0]) == 'D' && upc(p[1]) == 'A' && upc(p[2]) == 'T' && upc(p[3]) == 'A') {
                dpos = p + 4;
                while (*dpos == ' ') dpos++;
                return 1;
            }
        }
        return 0;
    }
}

static void doread(void) {
    int va, isarr;
    unsigned int cell = 0;
    for (;;) {
        va = varname(&isarr);
        if (va < 0) { error("SYNTAX"); return; }
        if (isarr) cell = acell(va / 11);
        if (!nextdata()) { error("OUT OF DATA"); return; }
        if (isarr) { float v; int neg = 0; while (*dpos == ' ') dpos++;
            if (*dpos == '-') { neg = 1; dpos++; } v = parsenum(&dpos); arr[cell] = neg ? -v : v; }
        else assignfrom(va, 0, &dpos);
        if (errflag) return;
        if (!accept(',')) break;
    }
}

static void statement(void);

static void dofor(void) {
    int va, isarr, i;
    float start, lim, step = 1.0f;
    va = varname(&isarr);
    if (va < 0 || va >= 1000 || isarr) { error("SYNTAX"); return; }
    if (!accept('=')) { error("SYNTAX"); return; }
    start = nexpr();
    if (!kw("TO")) { error("SYNTAX"); return; }
    lim = nexpr();
    if (kw("STEP")) step = nexpr();
    nv[va] = start;
    for (i = 0; i < nfor; i++) if (fvar[i] == va) { nfor = i; break; }   /* reuse */
    if (nfor >= NFOR) { error("FOR NESTING"); return; }
    fvar[nfor] = va; flim[nfor] = lim; fstep[nfor] = step;
    fline[nfor] = curl; fpos[nfor] = tp;
    nfor++;
}

static void donext(void) {
    int va = -1, isarr, i;
    if (!eos()) va = varname(&isarr);
    i = nfor - 1;
    if (va >= 0) while (i >= 0 && fvar[i] != va) i--;
    if (i < 0) { error("NEXT WITHOUT FOR"); return; }
    nfor = i + 1;
    nv[fvar[i]] += fstep[i];
    if ((fstep[i] >= 0 && nv[fvar[i]] <= flim[i]) || (fstep[i] < 0 && nv[fvar[i]] >= flim[i])) {
        curl = fline[i];
        tp = fpos[i];
        jumped = 1;
    } else nfor = i;
}

static void dolist(void) {
    unsigned int a = 0, b = 65535u;
    int i;
    char nb[8];
    skip();
    if (*tp >= '0' && *tp <= '9') { a = (unsigned int)parsenum(&tp); b = a; }
    if (accept('-')) { skip(); b = (*tp >= '0' && *tp <= '9') ? (unsigned int)parsenum(&tp) : 65535u; }
    for (i = 0; i < nlines; i++) {
        if (lnum[i] < a || lnum[i] > b) continue;
        fmtnum((float)lnum[i], nb);
        outs(nb + 1); outc(' '); outs(prog + loff[i]); outc('\n');
    }
}

static void getfile(char *name) {
    char s[SLEN + 1];
    sexpr(s);
    scopy(name, s, 63);
}

static void doload(void) {
    char name[64], line[160];
    int fd, n = 0, r;
    char c;
    getfile(name);
    fd = open(name, O_RDONLY);
    if (fd < 0) { error("FILE NOT FOUND"); return; }
    nlines = 0; ptop = 0;
    for (;;) {
        r = read(fd, &c, 1);
        if (r <= 0 || c == '\n') {
            line[n] = 0;
            if (n > 0) {
                const char *p = line;
                unsigned int ln;
                while (*p == ' ') p++;
                ln = (unsigned int)parsenum(&p);
                putline(ln, p);
            }
            n = 0;
            if (r <= 0) break;
        } else if (n < 159) line[n++] = c;
    }
    close(fd);
}

static void dosave(void) {
    char name[64], nb[8];
    int fd, i;
    getfile(name);
    fd = open(name, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) { error("CANNOT CREATE"); return; }
    for (i = 0; i < nlines; i++) {
        fmtnum((float)lnum[i], nb);
        write(fd, nb + 1, ustrlen(nb + 1));
        write(fd, " ", 1);
        write(fd, prog + loff[i], ustrlen(prog + loff[i]));
        write(fd, "\n", 1);
    }
    close(fd);
}

static void doif(void) {
    float c = nexpr();
    if (errflag) return;
    if (!kw("THEN")) { if (!kw("GOTO")) { error("SYNTAX"); return; } }
    if (c == 0.0f) { while (*tp) tp++; return; }        /* skip rest of line */
    skip();
    if (*tp >= '0' && *tp <= '9') { gotoline((unsigned int)parsenum(&tp)); return; }
    statement();
}

static void statement(void) {
    int va, isarr;
    unsigned int cell;
    skip();
    if (*tp == 0 || *tp == ':') return;
    if (kw("REM") || *tp == '\'') { while (*tp) tp++; return; }
    if (kw("DATA")) { while (*tp && *tp != ':') tp++; return; }
    if (kw("PRINT") || accept('?')) { doprint(); return; }
    if (kw("INPUT")) { doinput(); return; }
    if (kw("IF")) { doif(); return; }
    if (kw("GOTO")) { gotoline((unsigned int)nexpr()); return; }
    if (kw("GOSUB")) {
        unsigned int n = (unsigned int)nexpr();
        if (ngosub >= NGOSUB) { error("GOSUB NESTING"); return; }
        gline[ngosub] = curl; gpos[ngosub] = tp; ngosub++;
        gotoline(n);
        return;
    }
    if (kw("RETURN")) {
        if (!ngosub) { error("RETURN WITHOUT GOSUB"); return; }
        ngosub--;
        curl = gline[ngosub]; tp = gpos[ngosub];
        jumped = 1;
        return;
    }
    if (kw("FOR")) { dofor(); return; }
    if (kw("NEXT")) { donext(); return; }
    if (kw("ON")) {
        int k = (int)nexpr(), i;
        unsigned int n = 0;
        if (!kw("GOTO")) { error("SYNTAX"); return; }
        for (i = 1; ; i++) {
            n = (unsigned int)nexpr();
            if (i == k) { gotoline(n); return; }
            if (!accept(',')) return;                    /* out of range: next */
        }
    }
    if (kw("DIM")) {
        do {
            unsigned int d1, d2 = 1;
            va = varname(&isarr);
            if (va < 0 || va >= 1000 || !isarr) { error("SYNTAX"); return; }
            accept('(');
            d1 = (unsigned int)nexpr() + 1;
            if (accept(',')) d2 = (unsigned int)nexpr() + 1;
            accept(')');
            if (aused + d1 * d2 > ASIZE) { error("OUT OF MEMORY"); return; }
            abase[va / 11] = aused + 1; adim1[va / 11] = d1; adim2[va / 11] = d2;
            { unsigned int k; for (k = 0; k < d1 * d2; k++) arr[aused + k] = 0.0f; }
            aused += d1 * d2;
        } while (accept(','));
        return;
    }
    if (kw("READ")) { doread(); return; }
    if (kw("RESTORE")) { dline = 0; dpos = 0; return; }
    if (kw("END")) { running = 0; stopped = 1; return; }
    if (kw("STOP")) { running = 0; stopped = 1; outs("BREAK"); if (curl >= 0) { char b[8]; fmtnum((float)lnum[curl], b); outs(" IN"); outs(b); } outc('\n'); return; }
    if (kw("RANDOMIZE")) { seed = seed * 31u + 7u; return; }
    if (kw("LIST")) { dolist(); return; }
    if (kw("RUN")) { clearvars(); if (nlines) { curl = 0; tp = prog + loff[0]; running = 1; jumped = 1; } return; }
    if (kw("NEW")) { nlines = 0; ptop = 0; clearvars(); return; }
    if (kw("LOAD")) { doload(); return; }
    if (kw("SAVE")) { dosave(); return; }
    if (kw("BYE") || kw("SYSTEM")) sys_exit(0);
    kw("LET");
    va = varname(&isarr);
    if (va < 0) { error("SYNTAX"); return; }
    if (va >= 1000) {
        char s[SLEN + 1];
        if (!accept('=')) { error("SYNTAX"); return; }
        sexpr(s);
        if (!errflag) scopy(sv[va - 1000], s, SLEN);
        return;
    }
    cell = 0;
    if (isarr) cell = acell(va / 11);
    if (!accept('=')) { error("SYNTAX"); return; }
    {
        float v = nexpr();
        if (errflag) return;
        if (isarr) arr[cell] = v; else nv[va] = v;
    }
}

/* run statements from tp until the line ends, following the program */
static void execute(void) {
    while (!errflag) {
        jumped = 0;
        statement();
        if (errflag || stopped) break;
        if (jumped) {                                   /* GOTO/NEXT/RETURN/RUN */
            if (*tp != 0) continue;                     /* resume mid-line */
        } else {
            skip();
            if (*tp == ':') { tp++; continue; }
            if (*tp != 0) { error("SYNTAX"); break; }   /* junk after statement */
        }
        if (!running || curl < 0) break;
        if (curl + 1 >= nlines) break;
        curl++;
        tp = prog + loff[curl];
    }
    running = 0;
}

int main(int argc, char **argv) {
    const char *p;
    char base;
    sbase = (unsigned int)&base >> 1;
    clearvars();
    outs("ECLIPSE BASIC\n");
    if (argc > 1) {                                     /* basic file: load + run */
        char q[70];
        int i, n = 0;
        q[n++] = '"';
        for (i = 0; argv[1][i] && n < 66; i++) q[n++] = argv[1][i];
        q[n++] = '"'; q[n] = 0;
        tp = q; curl = -1; errflag = 0; doload();
        if (!errflag && nlines) { clearvars(); curl = 0; tp = prog + loff[0]; running = 1; stopped = 0; execute(); }
        return errflag;
    }
    for (;;) {
        outs("READY\n");
        do {
            if (readline(ibuf, sizeof ibuf) < 0) return 0;
            col = 0;
            p = ibuf;
            while (*p == ' ') p++;
        } while (*p == 0);
        if (*p >= '0' && *p <= '9') {
            for (;;) {                                  /* program line(s) */
                unsigned int n = (unsigned int)parsenum(&p);
                putline(n, p);
                if (readline(ibuf, sizeof ibuf) < 0) return 0;
                p = ibuf;
                while (*p == ' ') p++;
                if (!(*p >= '0' && *p <= '9')) break;
            }
            if (*p == 0) continue;
        }
        tp = p; curl = -1; errflag = 0; stopped = 0; running = 0;
        execute();
        errflag = 0;
    }
}
