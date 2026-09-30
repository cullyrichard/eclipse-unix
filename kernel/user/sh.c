/* sh.c -- /bin/sh (SHELL_NOTES.md).
 *
 *   sh            interactive: prompt "$ ", read commands from fd 0
 *   sh FILE       run the commands in FILE, no prompt
 *
 * Syntax: commands separated by ';' or newlines; a command is a pipeline
 * of up to MAXSTAGE stages separated by '|'; each stage is words (quote
 * spaces/operators with '...' or "...") plus
 * redirections '<' file, '>' file, '>>' file. '#' starts a comment. The
 * word "$?" is replaced by the last exit status. Builtins: cd [dir],
 * exit [n]. Other commands: a word containing '/' is a path, otherwise
 * /bin/WORD. There is no fork: the shell points its own fds 0/1 at the
 * right place with dup2, spawns the child (which inherits fds 0-2), and
 * restores them. */
#include "../usys.h"

#define LINEMAX  128
#define MAXWORDS 16
#define MAXSTAGE 4

static int infd = 0;                 /* where commands come from */
static char rbuf[64];
static int rpos, rlen;
static int status;                   /* $? */
static char stbuf[8];

/* one line (without '\n') into line; -1 at end of input */
static int readline(char *line) {
    int n = 0, c;
    for (;;) {
        if (rpos == rlen) {
            rlen = read(infd, rbuf, sizeof rbuf);
            rpos = 0;
            if (rlen <= 0) {
                rlen = 0;
                if (n == 0) return -1;
                break;
            }
        }
        c = rbuf[rpos++];
        if (c == '\n' || c == '\r') break;
        if (n < LINEMAX - 1) line[n++] = (char)c;
    }
    line[n] = 0;
    return n;
}

static int isspace_(int c) { return c == ' ' || c == '\t'; }
static int special(int c) { return c == '<' || c == '>' || c == '|' || c == ';'; }

/* Split line in place into tokens; operators become their own tokens.
 * ">>" is returned as the token ">>". */
static char tokspace[LINEMAX * 2];
static int tokenize(char *line, char **tok, int max) {
    int n = 0, i = 0, o = 0;
    while (line[i] && n < max) {
        while (isspace_(line[i])) i++;
        if (!line[i] || line[i] == '#') break;
        tok[n++] = tokspace + o;
        if (special(line[i])) {
            tokspace[o++] = line[i];
            if (line[i] == '>' && line[i + 1] == '>') { tokspace[o++] = '>'; i++; }
            i++;
        } else {
            /* a word; '...' and "..." quote spaces and operators */
            while (line[i] && !isspace_(line[i]) && !special(line[i]) && line[i] != '#') {
                if (line[i] == '\'' || line[i] == '"') {
                    int q = line[i++];
                    while (line[i] && line[i] != q) tokspace[o++] = line[i++];
                    if (line[i]) i++;
                } else {
                    tokspace[o++] = line[i++];
                }
            }
        }
        tokspace[o++] = 0;
    }
    return n;
}

static void err(const char *a, const char *b) {
    uputsfd(2, "sh: "); uputsfd(2, a); uputsfd(2, b); uputsfd(2, "\n");
}

static char *itoa_(int v) {
    int k = 7, neg = v < 0;
    unsigned int u = neg ? (unsigned int)(-v) : (unsigned int)v;
    stbuf[k] = 0;
    do { stbuf[--k] = (char)('0' + u % 10); u /= 10; } while (u);
    if (neg) stbuf[--k] = '-';
    return stbuf + k;
}

/* One pipeline: tok[0..n). Returns its status (the last stage's). */
static char *argv[MAXSTAGE][MAXWORDS + 1];   /* static, not local: SHELL_NOTES.md "large frames" */
static char *in[MAXSTAGE], *out[MAXSTAGE];
static int app[MAXSTAGE], argc[MAXSTAGE], pid[MAXSTAGE];
static char path[64];

static int runpipe(char **tok, int n) {
    int ns = 0, i, j, k, fd, save0, save1, pfd[2], nextin = -1, st = 0;

    if (n == 0) return status;
    argc[0] = 0; in[0] = 0; out[0] = 0; app[0] = 0;
    for (i = 0; i < n; i++) {
        if (tok[i][0] == '|' ) {
            if (argc[ns] == 0 || ns + 1 == MAXSTAGE) { err("syntax error near ", "|"); return 2; }
            ns++;
            argc[ns] = 0; in[ns] = 0; out[ns] = 0; app[ns] = 0;
        } else if (tok[i][0] == '<' || tok[i][0] == '>') {
            if (i + 1 >= n || special(tok[i + 1][0])) {
                err("syntax error near ", tok[i]); return 2;
            }
            if (tok[i][0] == '<') in[ns] = tok[i + 1];
            else { out[ns] = tok[i + 1]; app[ns] = tok[i][1] == '>'; }
            i++;
        } else if (argc[ns] < MAXWORDS) {
            argv[ns][argc[ns]++] = (tok[i][0] == '$' && tok[i][1] == '?' && !tok[i][2])
                                   ? itoa_(status) : tok[i];
        }
    }
    if (argc[ns] == 0) { err("syntax error: ", "empty command"); return 2; }
    ns++;
    for (i = 0; i < ns; i++) argv[i][argc[i]] = 0;

    /* builtins (only as a single-stage command) */
    if (ns == 1 && !ustrcmp(argv[0][0], "cd")) {
        if (chdir(argc[0] > 1 ? argv[0][1] : "/") < 0) {
            err("cd: cannot enter ", argc[0] > 1 ? argv[0][1] : "/"); return 1;
        }
        return 0;
    }
    if (ns == 1 && !ustrcmp(argv[0][0], "exit")) {
        k = 0;
        if (argc[0] > 1)
            for (j = 0; argv[0][1][j] >= '0' && argv[0][1][j] <= '9'; j++)
                k = k * 10 + (argv[0][1][j] - '0');
        sys_exit(k);
    }

    save0 = dup(0);
    save1 = dup(1);
    for (i = 0; i < ns; i++) pid[i] = -1;
    for (i = 0; i < ns; i++) {
        /* stdin: previous pipe, or '<', or the shell's own */
        if (nextin >= 0) { dup2(nextin, 0); close(nextin); nextin = -1; }
        else dup2(save0, 0);
        if (in[i]) {
            fd = open(in[i], O_RDONLY);
            if (fd < 0) { err("cannot open ", in[i]); st = 1; break; }
            dup2(fd, 0); close(fd);
        }
        /* stdout: next pipe, or '>'/'>>', or the shell's own */
        dup2(save1, 1);
        if (i + 1 < ns) {
            if (pipe(pfd) < 0) { err("pipe: ", "no pipes left"); st = 1; break; }
            dup2(pfd[1], 1); close(pfd[1]);
            nextin = pfd[0];
        }
        if (out[i]) {
            fd = open(out[i], O_WRONLY | O_CREAT | (app[i] ? 0 : O_TRUNC));
            if (fd < 0) { err("cannot create ", out[i]); st = 1; break; }
            if (app[i]) lseek(fd, 0, 2);
            dup2(fd, 1); close(fd);
        }
        k = 0;
        for (j = 0; argv[i][0][j]; j++) if (argv[i][0][j] == '/') k = 1;
        if (k) {
            for (j = 0; argv[i][0][j] && j < 63; j++) path[j] = argv[i][0][j];
            path[j] = 0;
        } else {
            const char *b = "/bin/";
            for (j = 0; b[j]; j++) path[j] = b[j];
            for (k = 0; argv[i][0][k] && j < 63; k++) path[j++] = argv[i][0][k];
            path[j] = 0;
        }
        pid[i] = spawnv(path, argv[i]);
        if (pid[i] < 0) { err(argv[i][0], ": not found"); st = 127; }
        /* our copy of this stage's stdout (maybe a pipe's write end) must
         * go now, or the next stage never sees EOF */
        dup2(save1, 1);
    }
    if (nextin >= 0) close(nextin);
    dup2(save0, 0); dup2(save1, 1);
    close(save0); close(save1);
    for (i = 0; i < ns; i++) {
        if (pid[i] < 0) continue;
        k = wait(pid[i]);
        if (k == -2) { err(argv[i][0], ": killed"); }
        if (i == ns - 1) st = k;
    }
    return st;
}

static char *tok[MAXWORDS * 2];                 /* static, not local: SHELL_NOTES.md "large frames" */
static void runline(char *line) {
    int n = tokenize(line, tok, MAXWORDS * 2), i, s = 0;
    for (i = 0; i <= n; i++) {
        if (i == n || tok[i][0] == ';') {
            if (i > s) status = runpipe(tok + s, i - s);
            s = i + 1;
        }
    }
}

static char line[LINEMAX];
int main(int ac, char **av) {
    int interactive = 1;
    if (ac > 1) {
        infd = open(av[1], O_RDONLY);
        if (infd < 0) { err("cannot open ", av[1]); return 1; }
        interactive = 0;
    }
    for (;;) {
        if (interactive) uputs("$ ");
        if (readline(line) < 0) break;
        runline(line);
    }
    return status;
}
