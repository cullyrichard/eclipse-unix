# SHELL — /bin/sh, argv, cwd, redirection, pipes, and a small userland

Phase 9 of the OS roadmap, on top of the filesystem (`FS_NOTES.md`). Boot
now ends at an interactive shell. Branch `shell` (worktree
`../eclipse-package-shell`), built on `20c6bf5`.

**Bottom line**: verified in SIMH with scripted keystroke sessions (SIMH
`expect "$ " send "..."`, `kernel/session.sh`). Every feature and every
error case below produced the expected output. Files created through the
shell match byte for byte when extracted on the host. `fsck` finds 0
problems after every session. The earlier tests still pass: `fstest`
40/40, three concurrent `fswriter` files match the host-computed pattern,
and `bigmem` verifies its 96KW with `errs=0`.

## 1. Kernel additions (syscalls 30-35; 19-29 are left to the interrupt-I/O work)

```
30 spawnv(path, argv)   argv: NULL-terminated char *[] (byte addresses); 0 = {path}
31 chdir(path)
32 dup(fd)              lowest free fd, sharing the file slot (and offset)
33 dup2(fd, newfd)      closes newfd first
34 pipe(int fds[2])     fds[0] read end, fds[1] write end
35 stat(path, unsigned st[4])  -> {inode, type (1 file, 2 dir), nlink, size}
```

- **argv.** `proc_spawn` lays the strings and the `char *argv[]` vector
  out at the top of the child's mapped stack area. It then stores `argc`
  at `_STACKTOP` and `argv` at `_STACKTOP-1`. That is exactly where
  `main(int argc, char **argv)` reads them: eclipse-cc's `_start` sets
  040 = `_STACKTOP` and does `EJSR main`, and the calling convention
  reads the first argument at FP-5 (= `_STACKTOP`) and the second at FP-6.
  This was checked against a compiled 2-argument function, not assumed.
  `_STACKTOP-1` is safe to write: `reorder_asm.py` puts `_STACKTOP` at
  least 2 words past the image (DEBUGGING_NOTES.md #47), and the stack mapping now starts at the page holding
  `_STACKTOP-1`. Plain `spawn(path)` gives `argv = {path}`.
- **main() may return.** `_start` executes `HALT` after `EJSR main`,
  which I/O-faults under protection. The fault handler recognizes exactly
  that case (fault PC 057, word 056 = `HALT`) and treats it as
  `exit(AC0)`, i.e. main's return value. Utilities just `return`.
- **Current directory.** Per process, inherited by spawned children. The
  `fs.c` path walk starts relative paths at `fs_cwd`, which `do_syscall`
  sets from the caller before every syscall. `unlink` refuses a directory
  that is anyone's cwd.
- **Descriptor inheritance.** A child gets the parent's fds **0-2 only**,
  sharing the file slots. With spawn instead of fork there's no point at
  which the child could close extra fds, and a pipeline writer inheriting
  its own pipe's read end would never see EOF.
- **Pipes.** 4 pipes, 256-byte ring buffers, one file slot per end.
  - `read` on an empty pipe with writers blocks. With no writers left it
    returns 0 (EOF).
  - `write` on a full pipe blocks. With no readers left it returns -1.
    Partial writes are allowed, and the user `write()` loops over them.
- **Blocking seam, for the interrupt-I/O merge.** A syscall that can't
  make progress sets `sys_block_chan = <pipe>` and returns having
  transferred nothing. `do_syscall` then rewinds the caller's PC by one
  word, onto its `SYC`, marks it `P_PIPEW`, and schedules. `proc_wakeup(chan)`
  makes waiters runnable, and they simply re-execute the syscall. This is
  a restart model with no kernel stacks per process, fitting a kernel
  that runs ION-off on one stack. The interrupt-I/O branch's sleep/wakeup
  should replace `P_PIPEW`/`proc_wakeup`, and pipes need no other change.
- **Exit messages.** The `kernel: pid N exited` line is now opt-in
  (`-DKTRACE_EXIT`), so it doesn't interleave with shell output. Kills are
  still reported.
- **`do_syscall` refactor.** The descriptor/file syscalls moved into
  `fsys_syscall()` (fsys.c). Adding the new syscalls pushed the kernel's
  long-jump relaxation slots past the 256-word page zero (dgasm: "Address
  out of range ... 0 - 255"), and one fewer long function fixed it.

## 2. /bin/sh and utilities

`sh` (interactive) or `sh FILE` (script):
- `;` and newlines, pipelines of up to 4 stages with `|`, `<`, `>`, `>>`;
- `'...'`/`"..."` quoting, `#` comments, `$?`;
- builtins `cd [dir]` and `exit [n]`;
- other words run `/bin/WORD`, or a path if the word contains `/`;
- reports `not found` (status 127) and `killed` (status -2).

Redirection without fork: the shell `dup2`s its own fd 0/1 to the file or
pipe, spawns the child (which inherits 0-2), and restores them. After
each stage it drops its own copy of that stage's pipe write end, so the
next stage sees EOF.

Utilities (`kernel/user/`): `echo`, `cat`, `ls [-l]` (type, inode,
links, size via `stat`), `mkdir`, `rm`, `wc`, `cp`, and `pwd`, which walks
`..` in user space the way V6 did.

`init`:
1. counts boots;
2. runs the automated suite (fstest, the concurrent writers) if
   `/etc/autotest` exists;
3. runs `/etc/rc` through `sh` if it exists;
4. starts an interactive `sh`, and halts when that shell exits.

## 3. Evidence

Tools: `kernel/session.sh CMDS` types each line of `CMDS` at the next
`$ ` prompt; the scripts are in `kernel/tests/`. Build extras with
`EXTRA="host:/fs/path ..." kernel/build.sh`.

### Full feature and error session (`tests/session_full.txt`), unedited

```
init: boot #1
$ pwd
/
$ mkdir /home
$ cd /home
$ pwd
/home
$ echo hello from the shell > greet
$ cat greet
hello from the shell
$ echo second line >> greet
$ cat < greet
hello from the shell
second line
$ wc greet
2 6 33 greet
$ cp greet copy ; ls -l
d 23	2	64	.
d 1	5	80	..
- 24	1	33	greet
- 25	1	33	copy
$ cat /bin/../home/greet | wc
2 6 33
$ ls -l /bin | wc
20 100 326
$ echo a b c | cat | cat | wc
1 3 6
$ mkdir sub ; cd sub ; pwd ; cd .. ; rm sub ; ls
/home/sub
.
..
greet
copy
$ echo 'script ran' > /home/s1
$ echo echo from a script > script
$ echo pwd >> script
$ echo exit 3 >> script
$ sh script
from a script
/home
$ echo status $?
status 3
$ nosuchcmd
sh: nosuchcmd: not found
$ echo status $?
status 127
$ cat /nope
cat: cannot open /nope
$ echo status $?
status 1
$ cat < /nope
sh: cannot open /nope
$ echo >
sh: syntax error near >
$ echo hi > /nodir/x
sh: cannot create /nodir/x
$ cd greet
sh: cd: cannot enter greet
$ ls /nope
ls: /nope: not found
$ rm /nope
rm: cannot remove /nope
$ badptr
badptr: poking 070000

kernel: pid 2 killed: write to unmapped page pc=127 mapstat=10040
sh: badptr: killed
$ echo status $?
status -2
$ /bin/echo absolute path
absolute path
$ cd
$ pwd
/
$ rm /home/greet /home/copy /home/script /home/s1
$ rm /home
$ ls
.
..
bin
etc
$ exit 0
init: shell exited 0, halting
```

Everything here checks out:
- `ls -l /bin | wc` gives 20 lines (18 programs plus `.` and `..`).
- `cd sub` then `pwd` gives `/home/sub`, and `rm sub` works once we've
  left it.
- The script sets `$?` to 3.
- Error statuses are 127 (not found), 1 (cat: missing file), and -2
  (killed).
- Every redirection error is reported by the shell, and the shell carries
  on.

### Files left on disk, checked from the host (`tests/session_keep.txt`)

```
init: boot #2
$ mkdir /keep
$ echo 'quoted  text | not a pipe' > /keep/q
$ cat /keep/q
quoted  text | not a pipe
$ cat /bin/fstest | cat | cat > /keep/fstest.copy
$ wc /bin/fstest /keep/fstest.copy
97 1522 8378 /bin/fstest
97 1522 8378 /keep/fstest.copy
194 3044 16756 total
$ cat /bin/fstest | wc
97 1522 8378
$ ls -l /keep
d 23	2	64	.
d 1	5	80	..
- 24	1	26	q
- 25	1	8378	fstest.copy
$ echo one > /keep/log ; echo two >> /keep/log ; echo three >> /keep/log
$ wc < /keep/log
3 3 14
$ exit 5
init: shell exited 5, halting
```

Host side:
```
$ fsck.py out/disk.img          -> fsck: 25 names, 119 data blocks in use, 894 free, 0 problems
$ fsck.py -x /keep/fstest.copy | cmp - <(fsck.py -x /bin/fstest)   -> identical
$ fsck.py -x /bin/fstest | wc   ->  97  1522  8378   (same as the Eclipse wc)
$ fsck.py -x /keep/q | od -c    -> "quoted  text | not a pipe\n"
$ fsck.py -x /keep/log          -> one / two / three
```

8378 bytes went through a 3-stage pipeline with 256-byte pipe buffers,
blocking and waking both ends dozens of times, and came out identical.

### Regressions

- **Autotest + rc, two boots of one disk** (`/etc/autotest`, `/etc/rc`
  from `tests/rc`): both boots report `fstest: 40 checks, 0 failed`, all
  three `fswriter` `errs=0`, and `badptr` exits -2.
  - Free blocks are 873 before and after.
  - `fsck` finds 0 problems after each boot.
  - Host extraction of `/tmp/w1,2,4` matches the formula (6000 bytes
    each, True).
- **`bigmem` as `/bin/init`**: `segalloc(96)=96`, 6 bad-argument calls
  return -1, verify rounds 0-2 `errs=0`, and all 992 frames are free at
  the end.

## 4. Bugs found

- **`fstest`'s "free blocks restored" check could pass vacuously.** My
  dispatch refactor briefly dropped `SYS_FREEBLK`, so `freeblocks()`
  returned -1 before and after, and the check passed. It was caught
  because `init` printed `free blocks 65535`. The syscall is restored and
  `fstest` now also checks that `freeblocks()` is sane (the 40th check).
- **Compiler: large stack frames corrupt locals (NOT fixed here;
  llvm-project is out of this branch's scope).** A function whose frame
  needs offsets beyond the 8-bit displacement gets frame-index
  elimination through `_scratch`. That expansion lands *inside* the
  backend's own `STA x,_scratch / LDA y,@_scratch` indirect load/store
  sequences, which use the same `_scratch`, so values are silently lost.
  Minimal repro (`--ieee`), with the expected value in parentheses:

  ```c
  int E(int k) { int c[4]; int pad[60]; c[2] = 3; pad[0] = 1; c[2] = c[2] + 1; return c[2] + pad[0]; }
  /* E(2) = 4 (5). With pad[10]: correct. c[j]++ with a runtime j: 0 (2). */
  ```

  In the generated code, `STA 1,_scratch` for an indirect load is followed
  by an inserted large-offset address computation that rewrites
  `_scratch`, and then `LDA 1,@_scratch` goes through the wrong address.
  This made the shell treat every command as empty (`runpipe`'s frame was
  350 words). **Workaround used**: large arrays in `sh`, `cat`, `cp`,
  `wc`, `ls`, `pwd` and `fstest` are `static`, and the kernel's
  `do_syscall` path buffer is static. After that, **no function in the
  kernel or any utility uses large-offset frame accesses**, except
  `fstest`'s `main` (frame 172, which still passes all 40 checks). This
  was checked by scanning every `out/*.s` for the `ADD 2,x` frame-address
  pattern. It needs a real fix in `EclipseRegisterInfo::eliminateFrameIndex`
  (use a scratch word distinct from `_scratch`, or preserve it).

## 5. Open

- **Blocking I/O**: console input still polls ION-off, so while the
  shell waits for a line nothing else runs. Pipes block properly via the
  restart seam. The interrupt-I/O branch should take over both.
- No `fork`/`exec` (spawn + wait only), no job control or `&`, no
  environment or `PATH` (only `/bin`), no globbing. Quotes can't nest or
  escape.
- `pwd` handles at most 8 levels. `ls` has no sorting. Paths are limited
  to 64 bytes, and argv to 16 arguments and 256 bytes total.
- Real hardware: untested, like the rest of the kernel.

## 6. Userland additions

- **`tar`**: magtape archives, POSIX ustar (FS_NOTES.md section 10).
- **`ed`**: a V6-style line editor, `ed [-p prompt] [file]`.
  - Addresses: `n . $ +n -n /re/ ?re? 'x a,b , ;`.
  - Commands: `a i c d p n l = s/re/rep/[g][p|n] g/re/cmd v/re/cmd m t j k w W r e E f q Q u h`.
  - Regular expressions: `^ $ . * [..] [^..] \( \) \1-\9`, with `&` and
    `\1-\9` in replacements.
  - Errors print `?`; `h` explains.
  - Limits: 16 KB of text and 1000 lines, so that the program plus its
    buffer fit in one 32KW process (the first version, with 20 KB and
    1500 lines, came to 34497 words and didn't). Move is an in-place
    rotation of the line index; copy is line by line.
  - Tested by `kernel/edtest.py tests/ed_session.txt`, a scripted session
    mixing shell and ed commands. Every command above behaved as
    expected; `cat`/`wc` in the shell confirm the written files. The test
    found an undo bug (`u` saved its own undo point first, so it undid
    nothing); fixed. Building `ed` also turned up a compiler bug
    (DEBUGGING_NOTES.md #45).
- **`basic`**: line-numbered BASIC, `basic` (interactive, `READY`
  prompt) or `basic file` (load, run, exit with 1 on an error).
  - Commands: `RUN LIST [n] NEW LOAD "f" SAVE "f" BYE`.
  - Statements: `LET PRINT (; , TAB) INPUT ["prompt";] IF..THEN
    GOTO GOSUB RETURN FOR..TO..STEP NEXT ON..GOTO DIM READ DATA RESTORE
    REM END STOP RANDOMIZE`, with `:` to separate statements.
  - Expressions: `^ * / + -`, relations (numeric and string),
    `NOT AND OR`.
  - Functions: `ABS INT SGN SQR SIN COS TAN ATN EXP LOG RND LEN ASC VAL
    CHR$ STR$ LEFT$ RIGHT$ MID$`.
  - Variables: `A`-`Z` plus `A0`-`Z9` numerics (IEEE single), `A$`-`Z$`
    strings up to 80 characters, and numeric arrays of 1 or 2
    dimensions (DIM, or 0-10 by default).
  - Errors print `?<WHAT> ERROR`.
  - Limits: program text 5000 bytes and 250 lines; 500 array cells in
    total. The program runs from its text and isn't tokenized.
  - Size: even at -O0 with tiny buffers the code didn't fit in 32KW.
    It's built with `-Oz` (BASIC is the only program that is; see
    DEBUGGING_NOTES.md #47 for what -O needed). Code plus runtime is
    about 26K words, of which the soft-float runtime is about 8K.
  - Stack: 2560 words. Expressions recurse, and a stack overflow wraps
    into page zero and hangs the machine without a fault, so `nexpr` and
    `sexpr` compare a local's address with `main`'s. Past 1800 words in
    use they report `?FORMULA TOO COMPLEX`. String temporaries live in
    non-inlined helpers (`strfn`, `strrel`) so they don't add to every
    nesting level: one level of parentheses went from about 400 words to
    about 150.
  - Tested by `edtest.py tests/basic_session.txt` (`> ` lines go at
    `READY`, `? ` lines answer INPUT). The session covers arithmetic and
    formatting, every function, string comparison, a prime sieve,
    2-D arrays, STEP -3, GOSUB, READ/DATA/RESTORE, ON GOTO, INPUT,
    errors, SAVE then NEW then LOAD, the saved file read with `cat`, and
    `basic file` exit status 0. Everything matched by hand. The test
    found two bugs, both fixed: a value printed after an error inside
    PRINT (`?DIVISION BY ZERO ERROR` then ` 0 `), and `PRINT "A"<"B"`
    printed as a string item. `tests/basic_deep.txt` nests
    `LEN(STR$(...))`, `LEFT$(...)`, `SIN(...)` and parentheses 8 to 20
    deep: every case either gives the answer or `?FORMULA TOO COMPLEX`,
    and the process exits normally. With the guard disabled, the same
    file hangs the machine.
- **Text and system tools** (`kernel/user/uio.h` is their shared
  buffered input/output):
  - `grep [-vcni] pattern [file...]`: `c . [..] [^..] [a-z] x* ^ $ \c`.
    Status 0 if a line matched, 1 if none, 2 on error.
  - `head [-n N|-N]`, `tail [-n N|-N]`. `tail` keeps the last 8 KB, so it
    works on pipes.
  - `sort [-rnu] [file...]`, in memory: 16000 bytes and 1200 lines.
  - `od [-bcw] [file]`: octal 16-bit words (the default) or bytes or
    characters, with octal offsets, and `*` for repeated lines.
  - `cmp file1 file2`: `differ: char N, line M` or `EOF on f`. Status
    0/1/2.
  - `mv from to`, or `mv file... dir`: SYS_RENAME. A file already at the
    target is replaced. A directory moves only within its own parent.
  - `more [file...]`: 22 lines, then `--More--`. Return continues and
    `q` Return stops. Keys come from the console (SYS_GETCHAR), so
    `... | more` works.
  - `ps`: PID, PPID, state, pages and name, from SYS_PSINFO.
  - `kill pid...`: SYS_KILL. The process exits with status -9.
  - `df`: data blocks and inodes, total/used/free, from SYS_FREEBLK(what).
  - **^C** kills the foreground job, meaning every process its parent is
    `wait()`ing for, except the login shell (KERNEL_NOTES.md section 13).
  - Tested by `edtest.py tests/tools_session.txt`, which checks every
    tool and option above against the expected output by hand.
    Highlights:
    - `sort -n` puts non-numbers (value 0) first, ties in byte order.
    - `grep 'an*a$'` matches `banana`; `grep xyz` gives status 1.
    - `od` words agree with `od -b`.
    - `cmp` reports EOF and the first difference (char 2, line 1).
    - `mv`: renames, moves into a directory, replaces a file, renames a
      directory, and refuses a directory to another parent and a missing
      source.
    - `df` is the same before and after the session.
    - `cat | ps` then ^C: `cat`, asleep on the console, is killed and
      the shell returns.
    - `cat | kill 2` kills that `cat`; `kill 0` (init) and `kill 99`
      are refused.
    - `ls -l /bin | more` stops once at `--More--`.
    - BASIC running `10 X=X+1 : 20 GOTO 10` stops at ^C with status -9.
    - At shutdown: faults=0 and all 992 frames free.
- **`edit [-r rows] [-c cols] [file]`**: a nano-style screen editor for
  an ANSI/VT100 terminal, 24x80 unless told otherwise.
  - Keys: `^G` help, `^O` write, `^X` exit (asks to save a modified
    buffer), `^W` search (case-insensitive, wraps), `^K` cut a line
    (repeat to collect several), `^U` paste, `^_` go to line, `^C`
    cursor position, `^Y`/`^V` page, `^A`/`^E` line start/end, `^D`
    delete, Backspace, `^L` redraw. The arrow keys and Home, End, PgUp,
    PgDn and Delete also work.
  - Text lives in a gap buffer in segment memory (SYS_SEGALLOC),
    mapped once into the logical pages above the stack (SYS_MAPWIN). It
    adds nothing to the program image, and holds about 32 KB plus a 4 KB
    cut buffer. A larger file is refused, not truncated.
  - Console input is raw (SYS_TTYRAW, KERNEL_NOTES.md section 14).
  - The screen is drawn by comparing every row with a copy of what the
    terminal already shows, and sending only the changed span. Measured
    in SIMH: one cursor move is 16 bytes, typing a character is about
    10, and a 24x80 screen is about 1,900. At 4800 baud that's 33 ms,
    21 ms and 4 s.
  - The last column is never written (no auto-wrap surprises). A long
    line scrolls the whole view sideways to keep the cursor visible,
    and a `$` marks text cut off on the right.
  - Not supported:
    - DG Dasher terminals: their control codes would have to come from
      a manual; ANSI is isolated in `tgoto`/`tclreol`/`tattr`/`tclear`.
    - undo, search-and-replace, and reading a second file into the
      buffer.
  - Under SIMH, `kernel/console.sh` moves SIMH's own break key from ^E
    to ^], because the editor uses ^E.
  - Tested two ways:
    - On the host (the same source with `malloc` in place of the
      segment): cut/paste order, Backspace joining lines, `^D`,
      wrapped case-insensitive search, sideways scroll, quit-discard,
      quit-save under a new name, and cancel. Files were compared byte
      for byte.
    - In SIMH through a real pty, by `kernel/edittest.py`, with the
      terminal screen rebuilt by `kernel/vt.py` (a small VT100 model).
      Covered: open, page down, go to line, end of a 110-character
      line, the Home key, cut 2 lines / paste, search, `^C`, help,
      write, and exit. Back in the shell, `head` and `wc` confirm the
      saved file. At halt: faults=0, 992 frames free.
  - Bug the pty test found: after a jump (go to line), the screen showed
    the new position one keystroke late. `draw()` copied `top` before
    `scrollfix()` moved it.
  - Pasting: input that's already waiting is applied before the screen
    is redrawn, so a paste is drawn once at the end.
    `kernel/pastetest.py`: a 20 KB, 400-line paste arrives byte-exact
    (`cmp` status 0, `tty_drop=0`). Before this fix only 160 of 1400
    characters survived (DEBUGGING_NOTES.md #49).
