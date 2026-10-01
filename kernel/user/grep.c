/* grep.c -- grep [-vcni] pattern [file...]
 * Patterns: c . [abc] [a-z] [^..] x* ^ $ and \c for a literal c (the
 * Kernighan-Pike matcher with character classes). -v non-matching lines,
 * -c count only, -n line numbers, -i ignore case. Status 0 if any line
 * matched, 1 if none, 2 on error. */
#include "uio.h"

static char line[256];
static int icase;

static int lower(int c) { return (icase && c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* length of the pattern item at p: c, \c, . or [...] */
static int itemlen(const char *p) {
    const char *q = p;
    if (*q == '\\' && q[1]) return 2;
    if (*q != '[') return 1;
    q++;
    if (*q == '^') q++;
    if (*q == ']') q++;                 /* "[]...]": ']' is a member */
    while (*q && *q != ']') q++;
    return *q ? (int)(q - p) + 1 : 1;   /* unclosed '[': a literal */
}

/* does the item at p match character c? */
static int item(const char *p, int c) {
    int neg = 0, hit = 0, lo, hi;
    const char *q;
    if (*p == '\\' && p[1]) return lower(p[1]) == lower(c);
    if (*p == '.') return 1;
    if (*p != '[' || itemlen(p) == 1) return lower(*p) == lower(c);
    q = p + 1;
    if (*q == '^') { neg = 1; q++; }
    do {
        lo = *q++;
        hi = lo;
        if (*q == '-' && q[1] && q[1] != ']') { hi = q[1]; q += 2; }
        if (lower(c) >= lower(lo) && lower(c) <= lower(hi)) hit = 1;
    } while (*q != ']');
    return hit != neg;
}

static int matchhere(const char *p, const char *s);

static int matchstar(const char *p, int n, const char *s) {
    do {                                /* zero or more, longest last */
        if (matchhere(p + n + 1, s)) return 1;
    } while (*s && item(p, *s++));
    return 0;
}

static int matchhere(const char *p, const char *s) {
    int n;
    if (*p == 0) return 1;
    if (*p == '$' && p[1] == 0) return *s == 0;
    n = itemlen(p);
    if (p[n] == '*') return matchstar(p, n, s);
    if (*s && item(p, *s)) return matchhere(p + n, s + 1);
    return 0;
}

static int match(const char *p, const char *s) {
    if (*p == '^') return matchhere(p + 1, s);
    do {
        if (matchhere(p, s)) return 1;
    } while (*s++);
    return 0;
}

int main(int argc, char **argv) {
    int i = 1, v = 0, cnt = 0, num = 0, fd, n, any = 0, named, st = 0;
    unsigned int lno, hits;
    const char *pat, *f;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (f = argv[i] + 1; *f; f++) {
            if (*f == 'v') v = 1;
            else if (*f == 'c') cnt = 1;
            else if (*f == 'n') num = 1;
            else if (*f == 'i') icase = 1;
            else { err2("usage: grep [-vcni] pattern [file...]", ""); return 2; }
        }
    }
    if (i >= argc) { err2("usage: grep [-vcni] pattern [file...]", ""); return 2; }
    pat = argv[i++];
    named = argc - i > 1;
    do {
        fd = 0;
        if (i < argc) {
            fd = open(argv[i], O_RDONLY);
            if (fd < 0) { err2("grep: cannot open ", argv[i]); st = 2; continue; }
        }
        in_open(fd);
        lno = hits = 0;
        while ((n = in_line(line, sizeof line)) >= 0) {
            lno++;
            if (match(pat, line) == v) continue;
            hits++;
            if (cnt) continue;
            if (named) { out_s(argv[i]); out_c(':'); }
            if (num) { out_u(lno); out_c(':'); }
            out_s(line);
            out_c('\n');
        }
        if (cnt) {
            if (named) { out_s(argv[i]); out_c(':'); }
            out_u(hits);
            out_c('\n');
        }
        if (hits) any = 1;
        if (fd) close(fd);
    } while (++i < argc);
    out_flush();
    return st ? st : !any;
}
