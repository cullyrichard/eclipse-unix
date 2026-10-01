/* ed.c -- a V6-style line editor (SHELL_NOTES.md, "Userland").
 *
 *   ed [-p prompt] [file]
 *
 * Addresses: n  .  $  +n  -n  /re/  ?re?  'x  a,b  ,  ;
 * Commands:  a i c d p n l =  s/re/rep/[g][n|p]  g/re/cmd  v/re/cmd
 *            m t j k w W r e E f q Q u  (and a bare address prints it)
 * Regular expressions: ^ $ . * [..] [^..] \( \) \1-\9, \x quotes x;
 * in a replacement, & is the match and \1-\9 the groups.
 * Errors print "?"; "h" explains the last one. Text lives in a 16 KB
 * pool with a line index, compacted in place when full (files up to
 * about 16 KB / 1000 lines; the whole program must fit in 32KW). */
#include "../usys.h"

#define POOL   16000
#define MAXL   1000
#define LBUF   256

static char pool[POOL];
static unsigned int ptop;                    /* next free byte in pool */
static unsigned int loff[MAXL + 1];          /* line i (1..nl) at pool+loff[i] */
static unsigned int llen[MAXL + 1];
static int nl, cur, dirty;
static char fname[64];
static const char *errmsg = "";
static char lastre[LBUF];
static char ibuf[LBUF];                      /* the current command line */
static const char *ip;
static char tbuf[LBUF * 2];
static int mark[26];
static char *prompt;

/* undo: one level, a full copy of the line index */
static unsigned int uoff[MAXL + 1], ulen[MAXL + 1];
static int unl, ucur, uvalidx;

static int err(const char *m) { errmsg = m; return -1; }

/* ---- I/O ---------------------------------------------------------------- */

/* Separate read-ahead for standard input (slot 0) and for the file being
 * read by r/e (slot 1): switching between them must not drop buffered
 * input, e.g. "ed file < script" reads the script in 128-byte chunks. */
static char rb[2][128];
static int rn[2], rp[2];

static void reader_reset(int slot) { rn[slot] = rp[slot] = 0; }

static int getline_fd(int fd, char *buf, int max) {
    /* one line (without '\n') from fd; -1 at EOF with nothing read */
    int n = 0, sl = fd == 0 ? 0 : 1;
    char c;
    for (;;) {
        if (rp[sl] >= rn[sl]) {
            rn[sl] = read(fd, rb[sl], 128);
            rp[sl] = 0;
            if (rn[sl] <= 0) { rn[sl] = 0; return n ? n : -1; }
        }
        c = rb[sl][rp[sl]++];
        if (c == '\n') return n;
        if (n < max - 1) buf[n++] = c;
    }
}

static void putl(const char *s, int n) { write(1, s, n); write(1, "\n", 1); }

/* ---- the line pool -------------------------------------------------------- */

static int cmpoff_less(int a, int b) { return loff[a] < loff[b]; }

static void compact(void) {
    /* slide live lines down in pool order (in place: each moves lower) */
    static int ord[MAXL + 1];
    int i, j, k, t;
    unsigned int top = 0, n;
    for (i = 1; i <= nl; i++) ord[i] = i;
    for (i = 2; i <= nl; i++) {                /* insertion sort by offset */
        t = ord[i];
        for (j = i - 1; j >= 1 && cmpoff_less(t, ord[j]); j--) ord[j + 1] = ord[j];
        ord[j + 1] = t;
    }
    for (i = 1; i <= nl; i++) {
        k = ord[i];
        n = llen[k];
        if (loff[k] != top) {
            for (j = 0; j < (int)n; j++) pool[top + j] = pool[loff[k] + j];
            loff[k] = top;
        }
        top += n;
    }
    ptop = top;
    uvalidx = 0;                               /* undo copy points at old offsets */
}

static int store(const char *s, int n, unsigned int *off) {
    if (ptop + (unsigned int)n > POOL) {
        compact();
        if (ptop + (unsigned int)n > POOL) return err("buffer full");
    }
    *off = ptop;
    while (n-- > 0) pool[ptop++] = *s++;
    return 0;
}

static void save_undo(void) {
    int i;
    for (i = 1; i <= nl; i++) { uoff[i] = loff[i]; ulen[i] = llen[i]; }
    unl = nl; ucur = cur; uvalidx = 1;
}

/* insert a line after line `after` */
static int addline(int after, const char *s, int n) {
    unsigned int off;
    int i;
    if (nl >= MAXL) return err("too many lines");
    if (store(s, n, &off) < 0) return -1;
    for (i = nl; i > after; i--) { loff[i + 1] = loff[i]; llen[i + 1] = llen[i]; }
    loff[after + 1] = off;
    llen[after + 1] = (unsigned int)n;
    nl++;
    dirty = 1;
    return 0;
}

static void delete(int a, int b) {
    int i, n = b - a + 1;
    for (i = b + 1; i <= nl; i++) { loff[i - n] = loff[i]; llen[i - n] = llen[i]; }
    nl -= n;
    dirty = 1;
}

static int getl(int i, char *buf) {          /* copy line i, NUL-terminate */
    int n = (int)llen[i], k;
    for (k = 0; k < n; k++) buf[k] = pool[loff[i] + k];
    buf[n] = 0;
    return n;
}

/* ---- regular expressions -------------------------------------------------- */

static const char *gstart[10], *gend[10];
static const char *bol;

static const char *matchhere(const char *re, const char *t);

static int matchone(const char **rep, char c) {
    /* does the single item at *rep match c? advances *rep past it */
    const char *re = *rep;
    int ok, neg;
    if (*re == '.') { *rep = re + 1; return c != 0; }
    if (*re == '\\' && re[1]) { *rep = re + 2; return c == re[1]; }
    if (*re == '[') {
        re++;
        neg = (*re == '^');
        if (neg) re++;
        ok = 0;
        do {
            if (re[1] == '-' && re[2] && re[2] != ']') {
                if (c >= re[0] && c <= re[2]) ok = 1;
                re += 3;
            } else {
                if (c == *re) ok = 1;
                re++;
            }
        } while (*re && *re != ']');
        if (*re == ']') re++;
        *rep = re;
        return c != 0 && (neg ? !ok : ok);
    }
    *rep = re + 1;
    return c == *re;
}

static const char *itemend(const char *re) {
    const char *p = re;
    char c = 'x';
    matchone(&p, c);                           /* advances past one item */
    return p;
}

static const char *matchstar(const char *item, const char *rest, const char *t) {
    const char *s = t, *e;
    const char *p;
    while (*s) { p = item; if (!matchone(&p, *s)) break; s++; }
    for (; s >= t; s--) {
        e = matchhere(rest, s);
        if (e) return e;
    }
    return 0;
}

static const char *matchhere(const char *re, const char *t) {
    const char *next;
    int g;
    for (;;) {
        if (*re == 0) return t;
        if (re[0] == '\\' && re[1] == '(') {
            for (g = 1; g < 10 && gstart[g]; g++) ;
            if (g < 10) gstart[g] = t;
            re += 2;
            continue;
        }
        if (re[0] == '\\' && re[1] == ')') {
            for (g = 9; g >= 1 && !(gstart[g] && !gend[g]); g--) ;
            if (g >= 1) gend[g] = t;
            re += 2;
            continue;
        }
        if (re[0] == '\\' && re[1] >= '1' && re[1] <= '9') {
            g = re[1] - '0';
            if (gstart[g] && gend[g]) {
                const char *q = gstart[g];
                while (q < gend[g]) { if (*t != *q) return 0; t++; q++; }
            }
            re += 2;
            continue;
        }
        if (re[0] == '$' && re[1] == 0) return *t == 0 ? t : 0;
        next = itemend(re);
        if (*next == '*') return matchstar(re, next + 1, t);
        if (!matchone(&re, *t)) return 0;
        t++;
    }
}

/* first match of re in t: returns 1 and sets *ms and *me */
static int rematch(const char *re, const char *t, const char **ms, const char **me) {
    const char *e;
    int g;
    bol = t;
    do {
        for (g = 0; g < 10; g++) gstart[g] = gend[g] = 0;
        if (re[0] == '^') {
            if (t != bol) return 0;
            e = matchhere(re + 1, t);
        } else {
            e = matchhere(re, t);
        }
        if (e) { *ms = t; *me = e; return 1; }
    } while (*t++ && re[0] != '^');
    return 0;
}

/* read a delimited pattern from ip into dst; empty -> last pattern */
static int getre(char delim, char *dst) {
    int n = 0;
    while (*ip && *ip != delim) {
        if (*ip == '\\' && ip[1]) { if (n < LBUF - 2) { dst[n++] = *ip++; } }
        if (n < LBUF - 1) dst[n++] = *ip;
        ip++;
    }
    dst[n] = 0;
    if (*ip == delim) ip++;
    if (n == 0) {
        if (!lastre[0]) return err("no previous pattern");
        for (n = 0; lastre[n]; n++) dst[n] = lastre[n];
        dst[n] = 0;
    } else {
        for (n = 0; dst[n]; n++) lastre[n] = dst[n];
        lastre[n] = 0;
    }
    return 0;
}

static int search(const char *re, int dir) {
    static char lb[LBUF];
    const char *s, *e;
    int i = cur, k;
    for (k = 0; k < nl; k++) {
        i += dir;
        if (i > nl) i = 1;
        if (i < 1) i = nl;
        getl(i, lb);
        if (rematch(re, lb, &s, &e)) return i;
    }
    return err("no match");
}

/* ---- addresses -------------------------------------------------------------- */

static int getnum(int *v) {
    int n = 0, any = 0;
    while (*ip >= '0' && *ip <= '9') { n = n * 10 + (*ip++ - '0'); any = 1; }
    *v = n;
    return any;
}

/* one address, or -2 if none; -1 error */
static int address(void) {
    static char re[LBUF];
    int a = -2, n, sign;
    for (;;) {
        if (*ip >= '0' && *ip <= '9') { getnum(&a); }
        else if (*ip == '.') { ip++; a = cur; }
        else if (*ip == '$') { ip++; a = nl; }
        else if (*ip == '\'') {
            ip++;
            if (*ip < 'a' || *ip > 'z') return err("bad mark");
            a = mark[*ip++ - 'a'];
            if (a == 0) return err("mark not set");
        } else if (*ip == '/' || *ip == '?') {
            char d = *ip++;
            if (getre(d, re) < 0) return -1;
            a = search(re, d == '/' ? 1 : -1);
            if (a < 0) return -1;
        } else if (*ip == '+' || *ip == '-') {
            sign = (*ip++ == '+') ? 1 : -1;
            if (a == -2) a = cur;
            if (!getnum(&n)) n = 1;
            a += sign * n;
        } else break;
    }
    if (a != -2 && (a < 0 || a > nl)) return err("address out of range");
    return a;
}

/* ---- commands ---------------------------------------------------------------- */

static int readfile(const char *name, int after) {
    static char lb[LBUF];
    int fd, n, cnt = 0;
    unsigned int bytes = 0;
    fd = open(name, O_RDONLY);
    if (fd < 0) return err("cannot open file");
    reader_reset(1);
    while ((n = getline_fd(fd, lb, LBUF)) >= 0) {
        if (addline(after + cnt, lb, n) < 0) { close(fd); return -1; }
        cnt++;
        bytes += (unsigned int)n + 1;
    }
    close(fd);
    uputu(bytes); uputs("\n");
    cur = after + cnt;
    return cnt;
}

static int writefile(const char *name, int a, int b, int append) {
    static char lb[LBUF];
    int fd, i, n;
    unsigned int bytes = 0;
    fd = open(name, append ? O_WRONLY | O_CREAT : O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return err("cannot create file");
    if (append) lseek(fd, 0, 2);
    for (i = a; i <= b && nl > 0; i++) {
        n = getl(i, lb);
        lb[n] = '\n';
        if (write(fd, lb, n + 1) != n + 1) { close(fd); return err("write error"); }
        bytes += (unsigned int)n + 1;
    }
    close(fd);
    uputu(bytes); uputs("\n");
    return 0;
}

static int input(int after) {
    /* text lines until a lone "." */
    static char lb[LBUF];
    int n;
    for (;;) {
        n = getline_fd(0, lb, LBUF);
        if (n < 0) break;
        if (n == 1 && lb[0] == '.') break;
        if (addline(after, lb, n) < 0) return -1;
        after++;
        cur = after;
    }
    return 0;
}

static void printl(int i, int numbered, int list) {
    static char lb[LBUF];
    int n = getl(i, lb), k;
    if (numbered) { uputu((unsigned int)i); uputs("\t"); }
    if (!list) { putl(lb, n); return; }
    for (k = 0; k < n; k++) {
        char c = lb[k];
        if (c == '\t') uputs("\\t");
        else if (c == '\\') uputs("\\\\");
        else if ((unsigned char)c < 32 || (unsigned char)c > 126) {
            char o[4];
            o[0] = '\\'; o[1] = (char)('0' + ((c >> 6) & 3));
            o[2] = (char)('0' + ((c >> 3) & 7)); o[3] = (char)('0' + (c & 7));
            write(1, o, 4);
        } else write(1, &c, 1);
    }
    uputs("$\n");
}

static int subst(int a, int b, int *count) {
    static char re[LBUF], rep[LBUF], lb[LBUF];
    const char *ms, *me, *t;
    int i, n, k, g = 0, pflag = 0, any;
    char d = *ip++;
    if (d == 0 || d == ' ' || d == '\n') return err("bad substitute");
    if (getre(d, re) < 0) return -1;
    n = 0;
    while (*ip && *ip != d) {
        if (*ip == '\\' && ip[1]) { rep[n++] = *ip++; }
        rep[n++] = *ip++;
    }
    rep[n] = 0;
    if (*ip == d) ip++;
    while (*ip == 'g' || *ip == 'p' || *ip == 'n') {
        if (*ip == 'g') g = 1; else pflag = *ip;
        ip++;
    }
    *count = 0;
    for (i = a; i <= b; i++) {
        getl(i, lb);
        t = lb;
        k = 0;
        any = 0;
        while (rematch(re, t, &ms, &me)) {
            const char *q;
            int j;
            if (re[0] == '^' && t != lb) break;
            for (q = t; q < ms; q++) if (k < (int)sizeof tbuf - 1) tbuf[k++] = *q;
            for (j = 0; rep[j]; j++) {
                if (rep[j] == '&') {
                    for (q = ms; q < me; q++) if (k < (int)sizeof tbuf - 1) tbuf[k++] = *q;
                } else if (rep[j] == '\\' && rep[j + 1] >= '1' && rep[j + 1] <= '9') {
                    int gn = rep[++j] - '0';
                    if (gstart[gn] && gend[gn])
                        for (q = gstart[gn]; q < gend[gn]; q++) if (k < (int)sizeof tbuf - 1) tbuf[k++] = *q;
                } else {
                    if (rep[j] == '\\' && rep[j + 1]) j++;
                    if (k < (int)sizeof tbuf - 1) tbuf[k++] = rep[j];
                }
            }
            any = 1;
            if (me == ms) {                          /* empty match: step on */
                if (*me && k < (int)sizeof tbuf - 1) tbuf[k++] = *me;
                t = *me ? me + 1 : me;
                if (!*me) break;
            } else t = me;
            if (!g) break;
        }
        if (!any) continue;
        while (*t && k < (int)sizeof tbuf - 1) tbuf[k++] = *t++;
        if (k >= LBUF) return err("line too long");
        if (store(tbuf, k, &loff[i]) < 0) return -1;
        llen[i] = (unsigned int)k;
        dirty = 1;
        cur = i;
        (*count)++;
    }
    if (*count == 0) return err("no match");
    if (pflag) printl(cur, pflag == 'n', 0);
    return 0;
}

static int docmd(int global);

static int globalcmd(int a, int b, int want) {
    /* mark matching lines, then run the rest of the command on each */
    static char re[LBUF], lb[LBUF], cmd[LBUF];
    static char gmark[MAXL + 1];
    const char *ms, *me;
    int i, n, k;
    char d = *ip++;
    if (getre(d, re) < 0) return -1;
    for (n = 0; *ip && n < LBUF - 1; n++) cmd[n] = *ip++;
    cmd[n] = 0;
    if (n == 0) { cmd[0] = 'p'; cmd[1] = 0; }
    for (i = 1; i <= nl; i++) {
        getl(i, lb);
        gmark[i] = (i >= a && i <= b) && (rematch(re, lb, &ms, &me) == want);
    }
    for (i = 1; i <= nl; i++) {
        if (!gmark[i]) continue;
        gmark[i] = 0;
        cur = i;
        for (k = 0; cmd[k]; k++) ibuf[k] = cmd[k];
        ibuf[k] = 0;
        ip = ibuf;
        if (docmd(1) < 0) return -1;
        /* lines may have moved: rescan from the start for remaining marks */
        if (nl < i) break;
    }
    return 0;
}

static int getfname(char *dst) {
    int n = 0;
    while (*ip == ' ') ip++;
    while (*ip && n < 63) dst[n++] = *ip++;
    dst[n] = 0;
    if (n == 0) {
        if (!fname[0]) return err("no file name");
        for (n = 0; fname[n]; n++) dst[n] = fname[n];
        dst[n] = 0;
    } else if (!fname[0]) {
        for (n = 0; dst[n]; n++) fname[n] = dst[n];
        fname[n] = 0;
    }
    return 0;
}

static void rev(int i, int j) {
    unsigned int t;
    for (; i < j; i++, j--) {
        t = loff[i]; loff[i] = loff[j]; loff[j] = t;
        t = llen[i]; llen[i] = llen[j]; llen[j] = t;
    }
}

/* lines [a..b] and [b+1..c] swap places */
static void rotate(int a, int b, int c) {
    rev(a, b); rev(b + 1, c); rev(a, c);
}

static int docmd(int global) {
    static char fn[64];
    int a, b, c, n, i, two = 0;
    char cmd;
    a = address();
    if (a == -1) return -1;
    b = a;
    if (*ip == ',' || *ip == ';') {
        char sep = *ip++;
        if (a == -2) a = (sep == ',') ? 1 : cur;
        if (sep == ';') cur = a;
        b = address();
        if (b == -1) return -1;
        if (b == -2) b = nl;
        two = 1;
    }
    cmd = *ip ? *ip++ : 0;
    if (a == -2) { a = b = cur; }
    else if (!two) b = a;
    if (cmd != 'a' && cmd != 'i' && cmd != 'r' && cmd != 'e' && cmd != 'E' &&
        cmd != 'f' && cmd != 'q' && cmd != 'Q' && cmd != '=' && cmd != 'w' &&
        cmd != 'W' && cmd != 'u' && cmd != 'h' && cmd != 0 && cmd != 'k' &&
        (a < 1 || b > nl || a > b))
        return err(nl ? "address out of range" : "buffer empty");
    if (cmd != 'p' && cmd != 'n' && cmd != 'l' && cmd != '=' && cmd != 'f' &&
        cmd != 'h' && cmd != 'w' && cmd != 'W' && cmd != 'u' && cmd != 0 &&
        cmd != 'q' && cmd != 'Q' && !global)
        save_undo();                    /* not for 'u' itself: it would
                                           overwrite what it undoes */
    switch (cmd) {
    case 0:                                         /* bare address: print */
        if (b < 1 || b > nl) return err("address out of range");
        cur = b; printl(cur, 0, 0); return 0;
    case 'a': return input(b < 0 ? 0 : b);
    case 'i': return input(b < 1 ? 0 : b - 1);
    case 'c': delete(a, b); cur = a - 1; return input(a - 1);
    case 'd':
        delete(a, b);
        cur = a > nl ? nl : a;
        return 0;
    case 'p': case 'n': case 'l':
        for (i = a; i <= b; i++) printl(i, cmd == 'n', cmd == 'l');
        cur = b; return 0;
    case '=': uputu((unsigned int)(two ? b : (a == cur && !two ? nl : a))); uputs("\n"); return 0;
    case 'k':
        if (*ip < 'a' || *ip > 'z') return err("bad mark");
        mark[*ip++ - 'a'] = b; return 0;
    case 's': return subst(a, b, &n);
    case 'g': return globalcmd(two ? a : 1, two ? b : nl, 1);
    case 'v': return globalcmd(two ? a : 1, two ? b : nl, 0);
    case 'j':
        if (a == b) b = a + 1;
        if (b > nl) return err("nothing to join");
        n = 0;
        for (i = a; i <= b; i++) {
            if (n + (int)llen[i] >= LBUF) return err("line too long");
            n += getl(i, tbuf + n);
        }
        delete(a, b);
        if (addline(a - 1, tbuf, n) < 0) return -1;
        cur = a; return 0;
    case 'm': case 't':
        c = address();
        if (c == -1) return -1;
        if (c == -2) return err("destination expected");
        n = b - a + 1;
        if (cmd == 'm') {
            if (c >= a - 1 && c <= b) return err("bad destination");
            /* move = rotate the index range, in place (three reversals) */
            if (c > b) { rotate(a, b, c); cur = c; }
            else { rotate(c + 1, a - 1, b); cur = c + n; }
        } else {
            if (nl + n > MAXL) return err("too many lines");
            for (i = 0; i < n; i++) {                /* copy line by line */
                /* each copy inserted after c shifts every line past c by
                 * one, so source line a+i has moved by i iff it lies past c */
                int src = a + i + ((a + i > c) ? i : 0);
                int len = getl(src, tbuf);
                if (addline(c + i, tbuf, len) < 0) return -1;
            }
            cur = c + n;
        }
        dirty = 1; return 0;
    case 'u':
        if (!uvalidx) return err("nothing to undo");
        {
            int t;
            for (i = 1; i <= unl || i <= nl; i++) {
                unsigned int o = loff[i], l = llen[i];
                loff[i] = uoff[i]; llen[i] = ulen[i]; uoff[i] = o; ulen[i] = l;
            }
            t = nl; nl = unl; unl = t;
            t = cur; cur = ucur; ucur = t;
        }
        return 0;
    case 'w': case 'W':
        if (getfname(fn) < 0) return -1;
        if (!two) { a = 1; b = nl; }
        if (writefile(fn, a, b, cmd == 'W') < 0) return -1;
        if (a == 1 && b == nl) dirty = 0;
        return 0;
    case 'r':
        if (getfname(fn) < 0) return -1;
        return readfile(fn, b < 0 ? nl : b) < 0 ? -1 : 0;
    case 'e': case 'E':
        if (cmd == 'e' && dirty) { dirty = 0; return err("warning: buffer modified"); }
        if (getfname(fn) < 0) return -1;
        for (i = 0; fn[i]; i++) fname[i] = fn[i];
        fname[i] = 0;
        nl = 0; cur = 0; ptop = 0; uvalidx = 0;
        if (readfile(fname, 0) < 0) return -1;
        dirty = 0; return 0;
    case 'f':
        if (getfname(fn) == 0 && fn[0]) { for (i = 0; fn[i]; i++) fname[i] = fn[i]; fname[i] = 0; }
        uputs(fname); uputs("\n"); return 0;
    case 'h': uputs(errmsg); uputs("\n"); return 0;
    case 'q':
        if (dirty) { dirty = 0; return err("warning: buffer modified"); }
        /* fall through */
    case 'Q':
        sys_exit(0);
        return 0;
    }
    return err("unknown command");
}

int main(int argc, char **argv) {
    int i = 1, n;
    if (argc > 2 && argv[1][0] == '-' && argv[1][1] == 'p') { prompt = argv[2]; i = 3; }
    if (i < argc) {
        for (n = 0; argv[i][n] && n < 63; n++) fname[n] = argv[i][n];
        fname[n] = 0;
        if (readfile(fname, 0) < 0) uputs("?\n");
        dirty = 0;
    }
    for (;;) {
        if (prompt) uputs(prompt);
        n = getline_fd(0, ibuf, LBUF);
        if (n < 0) break;
        ibuf[n] = 0;
        ip = ibuf;
        if (docmd(0) < 0) uputs("?\n");
    }
    return 0;
}
