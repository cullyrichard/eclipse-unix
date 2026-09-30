# IO — interrupt-driven console and disk, sleep/wakeup, idle

Before this, every console read and every disk transfer was polled with
interrupts off, so one process waiting on I/O stopped the whole machine
(KERNEL_NOTES.md §8, FS_NOTES.md §5). Now a process waiting for a typed
line or a disk transfer **sleeps inside the kernel**, other processes run,
and a device interrupt wakes it. When nothing is runnable but something is
asleep, the CPU idles with interrupts on.

**Bottom line** (SIMH, measured against the pre-change kernel built from
`20c6bf5`):
- While a process waited for a line of console input, a CPU-bound process
  printed **55** progress dots; on the old kernel, **1** (the machine was
  stuck in the polled read).
- While a process wrote a 6000-byte file, a CPU-bound process got
  **303-344** dots of progress vs **234-241** before (two runs each), and
  the whole job finished in ~9% fewer timer ticks.
- `fstest` 39/39, concurrent writers host-verified byte for byte, `fsck`
  clean over consecutive boots, `bigmem` unchanged.
- Deterministic interrupt-injection sweeps: 6300 runs across user mode,
  kernel and idle, plus 25 targeted at the exact ION-enable instructions
  (`k_idle`, `k_resume`, `k_kswitch`), **0 failures**. One real limitation found and measured: console
  input overrun when two characters arrive during a long interrupts-off
  kernel stretch (§5).

## 1. The design decision: per-process kernel stacks, not restartable syscalls

Blocking in the middle of a syscall needs somewhere to keep the half-done
kernel work. The two options:

- **Restartable syscalls** (rewind the saved PC to re-execute `SYC` after
  wakeup) keep one kernel stack, but every blocking syscall must be
  idempotent up to the point where it sleeps. Filesystem operations are
  not: `fs_writei` may allocate a block, write it, sleep on the next
  block's disk write, and a restart would write the first chunk again (or,
  with `O_CREAT`, create twice). A disk read deep inside `bmap` → `bread`
  would unwind and redo the whole path lookup. Making every fs operation
  restartable would mean rewriting `fs.c` around explicit state machines.
- **Per-process kernel stacks** let a process sleep *anywhere* in the
  kernel with its C frames intact, and let the existing `fs.c` code stay
  as it is (it just calls `kdsk_read`/`kdsk_write`, which may sleep).

Chosen: **per-process kernel stacks**, plus one filesystem sleep-lock.

- `kstk[(NPROC+1) * KSTK]`: one 1024-word stack per process slot plus one
  for the idle loop (`proc.c`). The first build used 512 words: it hit the
  new stack-overflow vector (location 043 → `k_stkflt`, a `HALT`) on the
  first boot, `040=031155 > 042` — caught, not silent. Measured high
  water at 1024: **658 words** (printed at shutdown from a fill pattern).
- `ksleep(chan)` (`proc.c`) saves 040/041/042, AC2/AC3 and a resume label
  (`ksleep_back`) into the process slot, marks it `P_SLEEP`, and calls
  `sched()`, which **never returns**. `swtch_to(n)` either `EJMP`s to
  `k_resume` (user context) or, for a process with a saved kernel
  context, restores it through `k_kswitch` (`kentry.s`): 040/041/042,
  AC2/AC3, `EJMP @k_ks_pc`. The code after `ksleep_back` sees
  `k_ks_resumed = 1` and returns normally — its frame, addressed through
  the restored 041, is intact because nothing else ran on that stack.
- `wakeup(chan)` marks every `P_SLEEP` process on `chan` `P_RUN`.
- The filesystem is one sleep-lock (`fs_lock`/`fs_unlock` around every
  syscall that can reach `fs.c`). A process may sleep on the disk with a
  buffer-cache slot in hand; no other process can enter `fs.c`, so that
  slot can't be evicted or reused while its DMA is outstanding, and
  multi-block operations (e.g. `mkdir`: inode, directory block, parent)
  stay atomic with respect to other processes. Console reads and writes,
  `close`, `wait`, `getpid` etc. don't take it.
- `do_syscall` copies `k_ac1..3` into locals before anything that can
  sleep: `k_ac*` belong to whichever process trapped last. When a sleeper
  resumes, `swtch_to` → `load()` has already restored `k_ac*`/`k_pc` from
  its slot (saved by `ksleep` → `save_cur()`), so finishing the syscall
  and returning through `k_resume` resumes the right user context.
- `load()` now `LMP`s map A immediately instead of deferring it to
  `k_resume`: a process woken inside the kernel copies to/from its user
  memory (`kcopyin_word`/`kcopyout_word`, through map A) before it ever
  reaches `k_resume`.

## 2. Invariants: what changed and what replaces it

| before (KERNEL_NOTES.md §3) | now |
|---|---|
| Kernel always runs ION-off. | **Unchanged.** ION goes on in exactly two places: `k_resume` (`NIOS 077` one instruction before `EJMP` into user mode, as before) and `k_idle` (`NIOS 077` then a `JMP` to itself). |
| Interrupts arrive only from user mode. | Interrupts arrive from user mode **or from the idle loop**, never from kernel code that has live state. `k_trap` panics if user mode was off and `curproc >= 0` (i.e. anything other than idle); it never fired. |
| One kernel stack, reset to one base on every entry. | Every entry resets 040/042 to the base/limit of the **current context's own** stack (`k_kbase`/`k_klim`): the running process's, or the idle stack. Correct because the interrupted context (user mode or idle) has no live frames there; a sleeping process's frames are on its own slice, which nothing else touches. `k_resume` also resets 040/042, so `SYC` and faults push their return block onto the process's own empty kernel stack. |
| — | **No lost wakeups**: "test condition, then `ksleep`" runs ION-off, and interrupt handlers only run when ION is on (user mode or idle), so a wakeup can't slip in between the test and the sleep. Every sleeper re-tests in a loop. |
| SYC race (SYSCALL_NOTES.md §7): `NIOC 077` as the handler's first instruction. | **Unchanged** (`k_syc`), and still needed: the handler must not be interrupted before it has captured the context. |
| MapIntMode: `DIA 0,MAP` first MAP access on interrupt entry. | Unchanged. From idle, `DIA` shows user mode off, which is how `k_trap` recognizes idle. A device interrupt that returns to the same user code now resumes with `k_mapstat = P_MAPSTAT` explicitly rather than the readback. |
| MSKO `0103`: only the PIT may interrupt. | MSKO `0001`: PIT, **TTI** and **DSK** interrupt; TTO stays masked (polled, §3). |
| Stack-overflow vector (043) unset. | `k_stkflt`: `HALT`. A kernel stack overflow stops with the evidence. |

`sched()` never returns now: every caller has already saved what it
needs. `k_start` jumps to the first process through `sched()` directly.

Process states gain `P_SLEEP` (asleep in the kernel, with a kernel
context) and `P_EMBRYO` (slot reserved while `spawn` builds the address
space, since `spawn` can now sleep on the disk in the middle). One bug
this exposed and fixed: a child that exited while its parent was asleep
in the kernel (in its next `spawn`, on the disk) was freed instead of
kept as a zombie, because `proc_exit` only counted `P_RUN`/`P_WAIT`
parents as alive; its slot was reused and the parent's later `wait`
returned -1. Now `P_SLEEP` parents count too.

## 3. Console (`kernel/tty.c`)

- **Input** is interrupt-driven: each TTI interrupt reads the character
  with `DIAC` (read + clear Done), echoes it, and appends it to a
  128-byte ring buffer. A newline completes a line and wakes `CHAN_TTY`.
  Backspace/DEL erase within the unfinished line. `read(0, ...)` sleeps
  until a whole line is available, then returns at most one line
  (canonical mode); `SYS_GETCHAR` is line-buffered the same way.
- **Output stays polled** (`console_putchar`: `DOAS` + `SKPDN` loop),
  with TTO masked. Reason: an interrupt-driven TTO needs an output queue
  and a writer that sleeps when it's full, and the payoff is small here —
  SIMH's TTO completes in a few instructions, and on real hardware the
  cost is bounded per character. Echo from the TTI interrupt handler uses
  the same polled output. Worth revisiting on real hardware at low baud.
- Characters typed before the kernel first enables TTI (during boot, while
  `fs_init`/`/bin/init` load with polled disk I/O) are lost: `tty_start`'s
  `NIOS 010` clears TTI Done.

## 4. Disk (`kernel/kdsk.c`)

Same programming sequence as `examples/dsk.c` (STORAGE_NOTES.md): `DOA`
block, `DOB` word address (the buffer pointer `>> 1`, DEBUGGING_NOTES.md
#38), `NIOS` read / `NIOP` write. Instead of spinning on `SKPDN`, the
caller sleeps on `CHAN_DSK`; `kdsk_intr` reads status (`DIA`), clears
Done (`NIOC`), and wakes it. The controller does one transfer at a time:
a second requester sleeps on `CHAN_DSKFREE` (in practice the fs lock
already serializes them). At boot, before any process exists
(`kcansleep()` false), it falls back to `examples/dsk.c`'s polling; the
Done flag those polled transfers leave set produces one harmless
interrupt when interrupts are first enabled (visible as
`dsk_intr = dsk_sleeps + 1` in every shutdown line).

`fs.c` changes only in calling `kdsk_read`/`kdsk_write` instead of
`dsk_read_block`/`dsk_write_block`. Its buffer cache is correct across
sleeps because of the fs lock (§1): the only process that can be inside
`fs.c` is the one waiting on the transfer.

## 5. Verification

Tools, in `kernel/`: `user/spinner.c` (CPU-bound, prints a dot every 400
loop iterations), `user/ttyread.c` (blocks on one console line and
reports it), `user/ioinit.c` (runs spinner next to ttyread, or next to
fswriter with `-DIOTEST_TTY` off, and prints `<IO DONE>` when the I/O
process exits), `user/sweepinit.c` + `io_sweep.py` (§5.4). The baseline
kernel is `20c6bf5` built unchanged (`git archive` into scratch).

SIMH harness note: the console must be driven with `run 50` (or `reset`
before `go`). `dep PC 50` + `go` never activates the TTI unit's polling,
so `send` input is never delivered — both kernels "hung" that way until
this was found.

### 5.1 Console wait no longer stops the machine

`send after=6000000,"hello\r"` (input typed long after ttyread is
blocked), same disk image, both kernels:

```
new: init is pid 0 | .. | <ttyread: waiting> | [52 dots]he.ll.o | . |
     <ttyread: got 6 bytes: hello> | ... | <spinner done 0>
     dots while ttyread was blocked: 55
old: init is pid 0 | . | <ttyread: waiting> | hello | . |
     <ttyread: got 6 bytes: hello> | ... | <spinner done 0>
     dots while ttyread was blocked: 1
```

On the new kernel the echo `h e l l o` is interleaved with the spinner's
dots — characters handled by interrupt while another process runs.

### 5.2 Disk I/O overlaps computation

`ioinit` (disk variant): a 4000-dot spinner next to one fswriter (6000
bytes in 50-byte writes, then read back). Dots printed before the writer
finished:

```
new run 1: 344   run 2: 303   (ticks 3744, 3675)   fswriter errs=0
old run 1: 234   run 2: 241   (ticks 4101, 4091)   fswriter errs=0
```

The gain is modest because SIMH's DSK transfer time is short relative to
the kernel's per-byte copy work, which still runs with interrupts off.
Every run: `dsk_sleeps = 334`, i.e. every one of the writer's disk
transfers put it to sleep.

### 5.3 Regression

Default `init` (fstest, three concurrent fswriters + worker + faulting
badptr, ls), two consecutive boots of one disk:

```
boot 1: fstest: 39 checks, 0 failed | children 0 0 0 0, badptr -2 | failures 0
        io: sleeps=2603 idle=1983 tty_intr=0 dsk_intr=1849 dsk_sleeps=1848
        fsck: 17 names, 108 data blocks in use, 905 free, 0 problems
boot 2: fstest: 39 checks, 0 failed | ... | failures 0
        fsck: ... 905 free, 0 problems
/tmp/w1, /tmp/w2, /tmp/w4: 6000 bytes each, host pattern match: True
```

`bigmem` as init (KERNEL_NOTES.md §9): `segalloc(96)=96`, bad args all
-1, rounds 0-2 `errs=0`, 992 frames free at shutdown; kernel stack high
water 201 words; no sleeps (it never waits on I/O).

### 5.4 Interrupt-injection sweeps (`kernel/io_sweep.py`)

Workload: worker (register/marker checker), fswriter, ttyread at once.
For offset k: `reset`, step k instructions, inject a TTI character `x`
(`dep TTI BUF/DONE/INT`, the SCP technique of
`examples/syscall_race_sweep.py`), step GAP, inject CR, run to the end.
Checked per run: ttyread got exactly `x\n`, every child exited 0,
`<SWEEP bad=0>`, no PANIC, clean shutdown, `fsck` 0 problems.

```
coarse: k = 6.6M..21.6M, 300 offsets, GAP 200000     300/300 OK
        landed: user/ION 45, kernel/noION 76, idle/ION 179
fine:   k = 6998500..7000499, every instruction      2000/2000 OK
        landed: user 1675, kernel 325; PCs inside k_int, k_syc and
        k_resume (027037, 027047, 027056, 027061) among them
idle:   k = 20000000..20003999, every instruction   4000/4000 OK
        (worker and fswriter done, ttyread asleep): idle 3994, kernel 6
        (PIT ticks taken from the idle loop)
```

The sweeps only hit the ION-enable instructions themselves by chance, so
`kernel/io_boundary.py` targets them: run to a SIMH breakpoint on the
instruction, inject the TTI character there, for the 1st..5th execution
of each:

```
k_idle NIOS 077     @027117  5/5 OK     k_idle_spin JMP  @027120  5/5 OK
k_resume NIOS 077   @027067  5/5 OK     k_resume EJMP    @027070  5/5 OK
k_kswitch EJMP      @027107  5/5 OK
```

(A first version of `io_boundary.py` matched "EJMP" inside a comment and
targeted `k_resume`'s `DOA 0,MAP` at 027044 instead: also 5/5 OK, but not
the instruction intended; the pattern now matches only the opcode.)

Total: 6300 sweep runs + 25 targeted, 0 failures with GAP 200000.

**Finding — input overrun.** With GAP = 777 the coarse sweep had 66
failures, all where the `x` landed while the kernel was running
(ION off): the kernel didn't read TTI before the CR arrived, the CR
overwrote the one-character hardware buffer, and ttyread got `"\n"`
(1 byte) instead of `"x\n"`. Everything else in those runs was correct
(`SWEEP bad=0`, fsck clean). Measured input latency (instructions until
TTI Done clears, `LAT=1`, 100-instruction resolution):

```
user/ION   n=45   max 100
idle/ION   n=179  max 100
kernel     n=76   median 5700   max 31300
```

So characters arriving closer together than ~31000 instructions can be
lost during heavy filesystem activity: the kernel's longest
interrupts-off stretches are byte-at-a-time user copies
(`kcopyin_word` per byte), bitmap scans and page zeroing. At 9600 baud
(~1 ms per character) that's a real risk on real hardware. Fixes, not
done here: shorter kernel critical sections (copy words, not bytes;
allow interrupts in long loops), or a deeper hardware FIFO if the
controller has one.

## 6. What's open

- **Input overrun** under heavy fs load (§5.4).
- The kernel's C code still runs with interrupts off throughout, so
  other processes only run while someone is *asleep*; a long
  CPU-bound syscall (e.g. `spawn` zeroing and copying pages) still holds
  the machine.
- One fs lock: two processes doing file I/O don't overlap each other's
  disk waits, only computation.
- TTO output is polled (§3).
- Memory: nine 1024-word kernel stacks (9216 words). The kernel image
  now ends at 064305 (26821 words) of its 32768-word window.
- Real hardware: TTI/DSK interrupt behavior is emulator-verified only.
  The DSKP-family drivers are still not behind this path.
