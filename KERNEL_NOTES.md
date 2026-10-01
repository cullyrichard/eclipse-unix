# KERNEL — N processes, each with its own 32KW address space, beyond the 2-map limit

Phase 5 (`PROCESS_NOTES.md`) ran exactly two processes, one permanently in
each hardware user map (A and B), with page 0 identity-mapped and shared.
This increment removes both limits. It is the first code in this project
that is a kernel rather than a probe:

- **Up to `NPROC` (8) processes**, each with its own 32-entry page table
  built from frames handed out by `examples/pmm.c`. So each process has its
  own 32KW logical address space, placed anywhere in the 1MW of physical
  memory.
- **Context switch = reload map A.** On a switch the kernel `LMP`s the next
  process's 32 PTEs into user map A. Map B is no longer a second process
  slot. It is the kernel's scratch map for reaching arbitrary physical
  frames (`kernel/kmem.c`).
- **Private page 0 per process.** Every process gets its own logical page
  0, including its own stack pointer at 040. This is safe because `SYC`,
  faults and interrupts never touch user page 0 (see "Why a private page 0
  is safe" below).
- **Real protection.** Processes run with MMPU I/O protection on. Any user
  I/O instruction (`HALT`, `NIOP MAP`, `DOA`...) faults to the kernel, and
  so does any access to an unmapped page. The kernel kills the process and
  frees its frames; everything else keeps running.
- **Syscalls with user buffers.** `SYS_WRITE(buf, len)` copies from the
  caller's address space, after checking that every word of the buffer is
  mapped.

**Bottom line**: verified in SIMH. Eight processes ran concurrently under
real PIT preemption: six workers and two deliberately bad programs. That
was 5554 timer preemptions and 5384 syscalls with zero corruption, both bad
processes were killed, and every frame was reclaimed. A mid-run physical
memory dump shows the same logical address holding six different
per-process values in six different physical frames, all above the
kernel's own 32KW.

Getting there turned up four toolchain bugs, all fixed or worked around
with evidence. Three of them silently miscompiled code that earlier phases
had relied on. See "Bugs found on the way" below.

## 1. Layout

```
kernel (supervisor, user map off): logical = physical 0..077777 (frames 0-31)
  page 0: vectors 0-3, kernel stack pointer 040/042, kernel code from 050
process n (user map A = its page table):
  page 0          image (org 050), its own 040/041/042 stack registers
  pages 1..k      rest of the image
  stack pages     _STACKTOP .. _STACKTOP+1024 (hardware stack grows UP)
  everything else INVALID -> any access faults
```

PTE words are `LMP` format (`MMPU_NOTES.md`): `slot<<10 | pfn`, and invalid
is `0101777 | slot<<10`. `eclipse_cpu.c` treats a stored `0101777` as
INVALID, so physical frame 1023 can never be mapped write-protected. That
doesn't matter yet, because nothing is mapped write-protected.

Every process runs with `MapStat = 041` (`P_MAPSTAT` in `kernel/proc.h`):
user mode on, map A, and **I/O protection on**. That last bit is required,
not optional. The manual's `NIOP` entry (`MMPU_NOTES.md`) says that from
user mode, with I/O protection off, `NIOP MAP` *turns the MMPU off*. So
every earlier probe in this project ran processes that could have taken
over the machine with one instruction. `kernel/user/escape.c` tries exactly
that and is killed.

## 2. Why a private page 0 is safe

Before this increment, user page 0 was always identity-mapped, so it never
mattered which page-0 a trap used. It matters now, because each process's
040 (hardware stack pointer) is private. Checked in `eclipse_cpu.c`:

- `SYC` does `Usermap = 0; MapStat &= ~1` **before** pushing its 5-word
  return block via `GetMap(040)`. The push therefore goes to the kernel's
  physical 040 stack, and the vector is read from physical location 2.
  TRAP_NOTES.md's manual citation for `SYC` says the same thing: "disables
  the current user map, pushes...".
- The protection-fault path also does `Usermap = 0; MapStat &= ~01` before
  its push, and reads vector 3 from `M[003]`.
- Interrupts write the return PC to `M[0]` and read the vector from `M[1]`,
  both physical.

So user page 0 is never touched by the trap machinery. It holds only the
process's own image and stack registers.

## 3. Kernel entry and exit (`kernel/kentry.s`)

Three vectors, one exit path:

| vector | entry | what it saves | cause |
|---|---|---|---|
| 1 | `k_int` | AC0-3, carry folded into PC bit 15 (PC from physical loc 0), `DIA` MapStat, `INTA` device | TICK (PIT) or DEV |
| 2 | `k_syc` | `NIOC 077` first (SYSCALL_NOTES.md §7), AC0-3, PC\|carry from the kernel-stack return block | SYS |
| 3 | `k_flt` | AC0-3, `DIA` MapStat (holds the fault code), PC from the return block | FLT |

All three reset the kernel stack to a fixed base and call
`k_trap(cause)`, using the one-argument hand-asm-to-C shape already
verified in `syscall_entry.s`. `k_trap` saves the context into the process
table if it's switching, picks what to run, and leaves the next context in
the `k_*` globals. `k_resume` then optionally `LMP`s map A (only when the
process whose page table is loaded changes), `DOA`s MapStat, restores
carry and ACs, executes `NIOS 077`, and `EJMP`s.

Invariants, relied on and checked:

- **The kernel always runs with ION off.** The only `NIOS 077` is the one
  immediately before the `EJMP` into user mode. So every tick interrupts
  user code, and the running process is always `curproc`, known from
  software without needing to decode MapStat. `k_trap` cross-checks
  MapStat bit 0 on every tick and would panic if an interrupt ever reached
  the kernel. It never did.
- **The kernel is not re-entrant, so one kernel stack suffices.** Each
  entry resets 040 to `k_stackbase`.
- **Only the PIT may interrupt.** `MSKO` (`DOB 0,077`) with `0103` masks
  TTI, TTO and DSK (`nova_defs.h` `PI_*`). The console and disk drivers
  poll their Done flags as before.
- **Map A always holds the current process's page table.** The kernel's own
  far accesses therefore go through map B. `examples/mmpu.c` loads its
  scratch PTE into map A slot 0, which would silently remap the *running
  process's* page 0, so the kernel doesn't use it.

## 4. Syscalls (`kernel/ksyscall.h`, user side `kernel/usys.c`)

Same ABI as `examples/syscall.h` (AC0 = number, AC1/AC2 = args, `SYC 1,1`,
result in AC0):

```
SYS_PUTCHAR 0   SYS_GETCHAR 1   SYS_EXIT 4   SYS_GETPID 5
SYS_WRITE   6   (buf, len): every word of buf..buf+len-1 must be in a
                mapped page (checked against the caller's page table),
                then each is read through map A with single-cycle mapping
                (kcopyin_word) and written to the console.
```

The mapped-page check isn't optional. A single-cycle access skips the
MMPU's validity check (`== INVALID && !SingleCycle` in `eclipse_cpu.c`),
so an unchecked copy from an unmapped page would silently read physical
frame 01777 instead of faulting.

## 5. Building and running

> **Update (FS_NOTES.md):** the kernel now boots from a filesystem on DSK
> and runs `/bin/init` from it. User programs are files in `/bin`
> (`kernel/build.sh` puts them there with `fstools/mkfs.py`), started with
> `spawn(path)` and reaped with `wait(pid)`, and `kernel/run.sh` boots it.
> `mkimage.py` and the compiled-in images described below are gone, and so
> is the `NWORKERS` knob (init decides what runs). The rest of this section
> is the historical record of how the numbers below were produced.

```
$ kernel/build.sh            # -> kernel/out/kernel.simh (+ user images)
$ { cat kernel/out/kernel.simh; echo 'dep PC 50'; echo 'go'; } | ~/dev/simh-src/BIN/eclipse
```

`build.sh` compiles each user program (`kernel/user/*.c` + `usys.c`) with
`eclipse-cc --ieee` into its own `.simh`. `kernel/mkimage.py` turns each
into a C header: a dense word array from 050, plus its `_STACKTOP`,
recovered from the program's own `LDA 0,_STACKTOP` at 050 and checked
rather than assumed. The kernel embeds those headers and copies each image
into fresh frames at spawn. Knobs: `NWORKERS=n`, `UCFLAGS=-DROUNDS=n`,
`KCFLAGS=-DKDEBUG` (panics with context if the kernel is ever about to
resume a process below 050).

`--ieee` for everything: the default hardware-float mode pulls the float
runtime into even a trivial program (about 6100 words vs 31), and is known
to misbehave on the real FPU (DEBUGGING_NOTES.md #34).

`eclipse-cc` gained three small features for this. All are optional, and
existing invocations are unchanged:
- `.s` sources are appended to llc's output before `reorder_asm.py`, the
  same mechanism `rt/eclipse_hwfloat.s` already used.
- `-DNAME[=V]` and `-Idir` are passed through to `clang -cc1`.
- `ECLIPSE_CC_KEEP_ASM=path` keeps the final assembly for mapping
  addresses back to code.

## 6. Verification

### Default run: 5 workers + 3 bad programs, real PIT, unedited (CRs stripped)

```
kernel: boot, free_frames=992
kernel: spawned worker pid=0 free_frames=988
  map: 0->40 1->41 5->42 6->43
...
kernel: spawned worker pid=4 free_frames=972
  map: 0->60 1->61 5->62 6->63
kernel: spawned badptr pid=5 free_frames=969
  map: 0->64 4->65 5->66
kernel: spawned escape pid=6 free_frames=966
  map: 0->67 4->70 5->71
kernel: spawned badread pid=7 free_frames=963
  map: 0->72 4->73 5->74
badptr: poking 070000

kernel: pid 5 killed: write to unmapped page pc=124 mapstat=10040
escape: NIOP MAP

kernel: pid 6 killed: I/O protection fault pc=115 mapstat=20040
badread: reading 070000

kernel: pid 7 killed: validity fault (read or fetch, unmapped page) pc=121 mapstat=0
[1] 0 [2] 0 [3] 0 [4] 0 [0] 0 [4] 1 [1] 1 [2] 1 [3] 1 [0] 1 [1] 2 ... [1] 5 [1]e=0
kernel: pid 1 exited, status 0
...
kernel: pid 0 exited, status 0
kernel: no runnable processes. ticks=1154 switches=1161 syscalls=128 faults=3 free_frames=992
```

- Every page table maps distinct frames (040-074 octal), all past the
  kernel's frames 0-31. Pages 2-4 of each worker are unmapped: the image
  ends in page 1 and the stack starts in page 5.
- All three protection paths fire and are identified correctly:
  - **write to an unmapped page**: code `010000`. The manual calls this
    code "write-protect", but processes run without the WP-enable bit, and
    SIMH's `PutMap` raises it for any write to an INVALID page. Real
    hardware may report validity instead; untested.
  - **user-mode I/O instruction**: `020000`, the `NIOP MAP` escape attempt.
  - **read from an unmapped page**: validity. The validity code isn't
    stored in MapStat, and SIMH also clears `0170`, hence `mapstat=0`.
- `free_frames` returns to exactly 992: every frame of every exited or
  killed process is freed.

### What each worker checks (`kernel/user/worker.c`)

Each fills `int marker[256]` with `pid*1000+i`. Then, for `ROUNDS` rounds
of 20 passes, it re-reads all 256 words (counting mismatches) and sums
them. A register-held sum that doesn't equal the precomputed total is also
counted, which catches a bad AC or carry restore mid-loop. It prints each
round through `SYS_WRITE` (a user-buffer copy) and exits with the error
count as its status.

### Soak: 8 processes, 200 rounds

`NWORKERS=6 UCFLAGS=-DROUNDS=200`: 6 workers plus 2 bad programs (before
`badread` was added):

```
kernel: pid 7 killed: I/O protection fault pc=115 mapstat=20040
kernel: pid 6 killed: write-protect fault pc=124 mapstat=10040   (old label)
kernel: pid 1 exited, status 0      ... all six workers: status 0, e=0
kernel: no runnable processes. ticks=5554 switches=5561 syscalls=5384 faults=2 free_frames=992
```

That is 6 × 200 × 20 × 256 = 6.1M checked marker reads across 5554 real
preemptions, with 0 errors.

### Isolation, directly in physical memory

`marker` is at logical 01536 in every worker (computed from the worker's
own assembly listing, using `reorder_asm.py`'s address model after its fix
below). The soak kernel was stopped mid-run (`step 20000000`), and that
word was read through each worker's own page-0 frame:

```
pid 0: frame 40 phys 101536: [0, 1, 2, 3]
pid 1: frame 44 phys 111536: [1000, 1001, 1002, 1003]
pid 2: frame 50 phys 121536: [2000, 2001, 2002, 2003]
pid 3: frame 54 phys 131536: [3000, 3001, 3002, 3003]
pid 4: frame 60 phys 141536: [4000, 4001, 4002, 4003]
pid 5: frame 64 phys 151536: [5000, 5001, 5002, 5003]
kernel physical 1536 (supervisor view): [16904, 452, 58424, 4329]  (kernel's own code)
```

This is the same logical address, with six different contents in six
physical frames above 32KW, and none of them visible at the kernel's own
physical 01536.

## 7. Bugs found on the way

Each is written up where it belongs. Summary:

1. **`examples/pmm.c`'s `unsigned char freemap[992]` wrote over the
   program's own code** (PMM_NOTES.md §6). A zero-initialized global `char`
   array indexed at runtime gets its *base address* halved. This isn't the
   kernel's bug; it's the compiler's (DEBUGGING_NOTES.md #37). Fixed by
   making `freemap` `unsigned int`. `pmm_test.c` re-run: output
   byte-identical to PMM_NOTES.md's transcript.
2. **`MULrr`/`UDIVrr` silently destroyed the other GPR**
   (DEBUGGING_NOTES.md #35). `(a+b-1)>>10` followed by `c>>10` returned `c`
   unchanged. Fixed in the backend. c-testsuite 199 → 201, no regressions.
3. **`JMP` displacement off by one in two tools** (DEBUGGING_NOTES.md #36).
   `dgasm` accepted +128 and encoded it as −128, and `reorder_asm.py`
   measured from the wrong base, so it never relaxed such a jump. A worker's
   loop back-edge landed 256 words away. Its AC2 then pointed at page 0,
   and the process eventually jumped to 0. Also: `SAVE` was sized as 1
   word. All fixed.
4. **`char` arrays are unreliable in general** (DEBUGGING_NOTES.md #37,
   open). Local arrays are packed two per word with colliding writes, and
   string-initialized globals misread at a runtime index. The kernel and
   user programs use `int` arrays for character buffers; string literals
   are fine.

## 8. What's open

- **Update: see IO_NOTES.md.** Console input and disk transfers now sleep
  (per-process kernel stacks, sleep/wakeup, idle loop); the kernel itself
  still runs with interrupts off.
- **No preemption during a syscall.** The handler runs ION-off (SYSCALL_NOTES.md §7),
  so `SYS_GETCHAR` stalls every process until a key arrives. It needs
  interrupt-driven console and disk I/O with sleep/wakeup.
- **No `fork`/`exec`, no loading from disk.** Images are embedded in the
  kernel. `exec` needs the filesystem (phase 8). `fork` needs a
  frame-copy loop (`kfar_read`/`kfar_write` or map B + `BLM`), which is
  straightforward on top of this.
- **Real hardware untested.** In particular:
  - the "write to invalid page → code 010000" behavior is emulator-derived;
  - the 1-instruction ION delay before `EJMP` is manual-backed
    (SYSCALL_NOTES.md §7), but has only been exercised in SIMH.
- **`char` arrays** must be fixed in the backend before the filesystem,
  which lives on byte strings.
- **Single programs larger than 32KW**: data is done (section 9); code
  overlays and transparent far pointers are not (see section 9's open
  list).
- Copying an image costs one `kfar_write` (an `LMP` plus a single-cycle
  store) per word. That's fine for a few KW; `BLM` through map B would be
  far faster.

## 9. One program, more than 32KW: segments and map windows

The second half of "beyond 32kW". A process's *logical* space stays 32KW
(15-bit addresses, `MMPU_NOTES.md`), but it can now own up to `MAXSEG` = 128
extra physical pages (128KW) of **segment** memory. It reaches them by
rebinding a range of its own logical pages, a **window**, to any run of
segment pages. The kernel does the rebinding by editing the process's page
table and reloading map A. That's the same mechanism as a context switch;
nothing new in hardware or the compiler.

### ABI (`kernel/ksyscall.h`, wrappers in `kernel/usys.c`)

```
SYS_SEGALLOC 7  arg1 = pages to add. -> new segment size, or -1.
                All-or-nothing: on pmm exhaustion every frame this call took
                is returned. Frames are zeroed. Limit 128 pages total.
SYS_MAPWIN   8  arg1 = window first page (bits 0-4) | npages << 5,
                arg2 = first segment page, or -1 to unmap the window.
                -> 0, or -1 (nothing changed) if npages = 0, the window runs
                past page 31, the segment range doesn't exist, or ANY window
                page is an image/stack page.
int sys_segalloc(int npages);
int sys_mapwin(int winpage, int segpage, int npages);
int sys_unmapwin(int winpage, int npages);
```

Implementation (`kernel/proc.c`):
- `p_fixed[]` marks the pages `proc_create` mapped (image + stack). These
  can never be part of a window, so a program can't unmap the code or
  stack it's running on.
- `p_seg[]`/`p_nseg[]` hold the segment's frames.
- `proc_free` frees the fixed pages' frames and the segment frames once
  each. Window PTEs point at segment frames, so they are not freed a
  second time through the page table.
- After a `MAPWIN`, `reload_cur()` copies the page table into `k_ptbuf`
  and sets `k_reload`, so `k_resume` `LMP`s the new map before returning
  to the caller.
- The mapping lives in `p_pt`, so it survives preemption like the rest of
  the page table.

A window is ordinary mapped memory. Plain C pointers work (`unsigned int
*w = (unsigned int *)040000; w[i] = ...`), and `SYS_WRITE` can copy from
it. Nothing in the compiler changed.

### Demo: `kernel/user/bigmem.c` (build with `KCFLAGS=-DBIGMEM`)

`bigmem` allocates 96 segment pages (96KW). Its own image and stack are 4
more, so **100KW for one process**. It walks the segment through an 8-page
window at logical pages 16-23 (`040000`-`057777`). It fills every word with
`segpage*257 + offset*3 + pid*4096`, then re-verifies all 96KW `BIGROUNDS`
times, visiting the 8KW chunks in a different order each round. That order
matters: a stale map (a missing reload) would read the wrong chunk.
`winfault` maps one segment page, writes it, unmaps it, and writes again.
The `-DBIGMEM` build spawns these two alongside the 5 workers, in place of
the three bad programs.

Soak, `KCFLAGS=-DBIGMEM UCFLAGS="-DROUNDS=400 -DBIGROUNDS=10"` (unedited
excerpts, CRs stripped):

```
kernel: pid 6 segalloc +1 -> 1 pages, frames 73..73 free_frames=964
kernel: pid 5 segalloc +96 -> 96 pages, frames 74..233 free_frames=868
kernel: pid 6 killed: write to unmapped page pc=201 mapstat=10040
bigmem: segalloc(96)=96
bigmem: bad args: -1 -1 -1 -1 -1 -1
[0] 46 [1] 46 [2] 46 ... [4] 89 [0] 89 bigmem: verified round 0 errs=0
[0] 90 [1] 90 ...                    bigmem: verified round 1 errs=0
...                                  bigmem: verified round 6 errs=0
kernel: pid 1 exited, status 0      ... all five workers status 0
bigmem: verified round 7 errs=0
bigmem: verified round 8 errs=0
bigmem: verified round 9 errs=0
kernel: pid 5 exited, status 0
kernel: no runnable processes. ticks=12316 switches=11843 syscalls=9705 faults=1 free_frames=992
```

- **Correctness under preemption.** 10 full passes over 96KW with 0
  errors, rounds 0-6 interleaved with the five workers' output (12,316
  timer ticks in the run). The workers also ran 400 rounds each with 0
  errors.
- **Bad arguments.** All six fail cleanly with −1: window over image page
  0, window past page 31, segment range past the end, zero pages,
  `segalloc(200)` over `MAXSEG`, and `segalloc(33)` giving 96+33 > 128.
  Both `segalloc` failures are caught by the limit check before any frame
  is taken. The *other* failure path, running out of physical frames
  partway through and rolling back, is implemented but was not exercised
  (it would need more than about 860 free frames' worth of requests).
- **Unmap really unmaps.** `winfault`'s first write through the window
  succeeds; its post-unmap write faults, and the process is killed.
- **No leaks.** `free_frames=992` at the end, including all 97 segment
  frames and the killed process's segment frame.

**Physical memory, checked independently of the program.** After the run
halted, SIMH `examine` dumped physical memory. The segment frames are
contiguous (`074`-`0233`), so segment page `sp` lives at physical
`(074+sp)<<10`.

```
examinable segment pages: 68 (frames 74..177), words 69632, mismatches 0
not examinable by SCP (frames >= 0200, above 128KW): 28 pages
  phys 0170000 = 20480  expected 20480       (sp 0, off 0)
  phys 0327000 = 34095  expected 34095       (sp 47, off 512)
```

68KW of this one process's data is confirmed word-for-word at physical
addresses between 60KW and 128KW, far outside its own 32KW logical space.
**The other 28 pages could not be checked this way.** SIMH's `cpu_dev`
declares a 17-bit address width (`eclipse_cpu.c:577`: `1, 8, 17, ...`), so
SCP `examine` rejects anything from `0400000` up ("Invalid address
specification"), even though the CPU and MMPU address the full 1MW (`show
cpu` → `1MW`). Those 28 pages are verified only by `bigmem` itself, reading
through the MMPU: 10 passes, 0 errors.

**Regression.** The default build (no `-DBIGMEM`) produces the same
spawns, kills, exits and `free_frames=992` as section 6. One observation,
checked and explained rather than hidden: with exactly the default
`ROUNDS=6`, the new kernel reported `ticks=32527` (121.9M instructions)
against the previous kernel's 1154 (13.3M), with identical program output.
Cause: SIMH's wall-clock-calibrated PIT (timer 8) recalibrated a second time
in that specific run (`show clocks`: 2 calibration opportunities, 113M
instructions since init, vs 1 and 4.9M), which inflates both the tick rate
and the console-output stall time (`sim_putchar_s` → `tto_svc` retry).
It's not a kernel cost. With `ROUNDS=12`, the new kernel gives 1258 ticks /
21.49M instructions against the old kernel's 1261 / 21.47M. Tick counts in
these notes are therefore evidence of preemption *happening*, not a
performance measure.

### Open

- **Code overlays: not done.** The mechanism supports them (map an
  overlay's frames at a window, then call into it), but the toolchain
  doesn't. `eclipse-cc` always emits `org 050`, and every call and global
  reference is an absolute `EJSR`/`ELDA`/`ESTA`, so an overlay has to be
  assembled at its window's address and has to resolve the root image's
  symbols (e.g. `sys_write`). dgasm has no linker or equate-import. A
  plausible route with no llc change: compile the overlay to assembly,
  rewrite its `org` to the window base, strip its `_start`, and emit
  `var`-free equates for root symbols taken from the root's
  `ECLIPSE_CC_KEEP_ASM` listing. Unverified; scoped here only.
- **Transparent far pointers** (a C pointer that reaches segment memory
  without an explicit `sys_mapwin`) need compiler support: an address-space
  qualifier plus runtime window management. That's the redesign-sized item
  in `MMPU_NOTES.md`; not attempted.
- Each `MAPWIN` costs a syscall plus a 32-word `LMP`. The window is 8KW
  here, so a 96KW sweep is 12 remaps per pass. Cheap, but a random-access
  far-memory workload would want a larger window or a smarter layout.
- `SEGALLOC` zeroes frames with one `kfar_write` per word, with interrupts
  off. 96 pages is about 98K calls, which stalls the other processes
  during that syscall (the same "no preemption in syscalls" limit as §8).
- Segments aren't shared between processes, and there is no
  `SEGFREE`/shrink; frames come back only at exit.
- Not tested on real hardware.


**Update:** `char` arrays work now: pointer values are byte addresses and chars pack two per word (DEBUGGING_NOTES.md #38). `SYS_WRITE` takes an ordinary `char` buffer, and absolute word addresses in user code are written as byte addresses (`(int *)0160000` is word 070000).

## 10. Merge of the interrupt-io and shell branches

`interrupt-io` (IO_NOTES.md: per-process kernel stacks, sleep/wakeup,
interrupt-driven TTI and DSK, idle loop, fs lock) and `shell`
(SHELL_NOTES.md: argv, cwd, dup/dup2, pipes, stat, /bin/sh and utilities)
were developed in parallel from `20c6bf5` and merged here. What the merge
had to decide:

- **Pipe blocking.** `shell` blocked a pipe reader/writer by rewinding the
  saved PC onto the `SYC` and restarting the call when woken (`P_PIPEW`,
  `sys_block_chan`). With per-process kernel stacks that machinery isn't
  needed: pipes now `while (empty/full) ksleep(CHAN_PIPE + pipe)` and
  re-test, and every transfer or close wakes the channel. `P_PIPEW`,
  `p_waitchan`, `proc_wakeup` and the restart code are gone. (`P_PIPEW`
  was also 5, the same value as `P_EMBRYO`.)
- **The fs lock vs pipes.** `interrupt-io` took the fs lock around every
  read/write except console reads. A pipe read can sleep indefinitely, so
  under that rule a reader would hold the lock its writer needs:
  deadlock. `fsys_needslock(no, fd)` now takes the lock only when the call
  can reach `fs.c`. Console and pipe I/O, `close`, `dup`, `dup2` and
  `pipe` never take it.
- **spawn/spawnv** run entirely under the fs lock. Loading an image can
  sleep on the disk, and the shared static `path` and argv staging buffers
  need the protection.
- **Liveness in `proc_exit`**: `shell`'s "parent is anything but
  FREE/ZOMBIE" test, which subsumes `interrupt-io`'s explicit
  RUN/WAIT/SLEEP fix.
- **Toolchain**: the merged kernel (30218 words) exposed two
  `reorder_asm.py` problems, both fixed (DEBUGGING_NOTES.md #39, #40): the
  stack wrapping past 32KW, and page-zero exhaustion from long-jump
  slots. The kernel builds with `ECLIPSE_STACK_WORDS=1536`. Kernel memory
  is now tight: about 2.5KW is left below 32KW. The 9 kernel stacks
  (9216 words) are the obvious place to recover space; the measured peak
  is 655 of 1024 words.

Verification of the merged kernel, re-running each branch's own evidence:

| check | result |
|---|---|
| shell full session (`tests/session_full.txt`), diffed against SHELL_NOTES.md | identical except the 2 extra `/bin` programs (inode numbers +2, `ls -l /bin \| wc` 22 lines vs 20) |
| heavy pipe session (`tests/session_keep.txt`) | 8380-byte file through a 3-stage pipe byte-identical (host `cmp`); Eclipse `wc` = host `wc` |
| autotest + rc, two boots of one disk | `fstest` 40/40, writers' files match the host pattern, `badptr` -2, free blocks stable at 868, `fsck` 0 problems both boots |
| `io_boundary.py` (inject TTI exactly at every ION-enable instruction) | 0 bad runs |
| console wait, disk overlap (`ioinit`, 4000-dot spinner) | see IO_NOTES.md merge addendum |
| `bigmem` as init | `errs=0` all rounds, 992 frames free |
| c-testsuite, both float modes | 206/220, same set as before |

## 11. Bug-fix pass: kernel stacks, time slice, keystroke latency

**Kernel stacks sized from a proof, not a guess.** `kernel/stackdepth.py`
computes the worst-case kernel stack depth from the call graph in
`out/kernel.s`: `SAVE` frames, return blocks and argument pushes, with
recursion rejected. The kernel has no recursion and no indirect calls.
The model predicts 656 words; the measured high-water mark was 655. The
deepest path is `open(O_CREAT)` → `fs_create` → path walk → `readi` →
`bmap` → disk. `KSTK` went from 1024 to 768, and `build.sh` now fails if
the computed worst case exceeds `KSTK - 16 (overflow guard) - 32`. That
frees 2304 words: the kernel image is 27,936 words, about 3.3KW below
32KW. The hardware overflow guard (vector 043, halt) is still there in
case the model is ever wrong.

**Time slice 0.8 ms → 16 ms.** `k_pitinit` was `0177770` (8 PIT counts),
inherited from PROCESS_NOTES.md's timer probe. SIMH's PIT counts at
10 kHz (`pit_tps`, matching DG's 100 us PIT). Under SIMH's deterministic
clock (`set clock nocalibrate`, 5M instructions per pseudo-second), a PC
profile of the I/O sweep workload put **two thirds of all samples in the
context switch (`load`)**: a preemption about every 1700 instructions.
Small layout shifts then swung total run time by 2x (3562 vs 7672 ticks
for code differing only in two counters), through resonance with that
tiny quantum. Now 160 counts (16 ms, about 60 Hz like V6): the same
workload takes 205 ticks and 555 switches (was 3562 and 3494), `load`
fell from 236 of 350 samples to 15, and user code is now the largest
share of samples before shutdown. (Earlier tick counts in these notes
are wall-clock calibrated and not comparable to these.)

**Context switch.** `load()` copied a process's 32-word page-table row
into `k_ptbuf` before `LMP`, about 2000 instructions with interrupts off.
`LMP` now reads the row directly (`k_ptaddr`).

**Keystroke latency (TTI overrun, IO_NOTES.md §5.4).** `tty_poll()`
checks TTI Done (`SKPDZ 010`) and takes a waiting character exactly as
the interrupt would. It is called from the kernel's long interrupts-off
loops: `kfar_write` (every 8), user byte copies (every 4), fs block
copies (every 8 bytes), block/inode scans (every 4), each console output
character, and `sched()`. A trace showed chains of in-kernel
sleep/wakeup switching kernel-to-kernel without ever enabling
interrupts. Measured with `io_sweep.py LAT=1` (instructions until the
kernel takes an injected keystroke, 60 points each):

```
                 before            after
user/ION         max 100           max 100
idle/ION         max 100           max 100
kernel/noION     median 4900,      median 450,
                 max 37000         max 1400
```

Functional test (type `x`, then CR GAP instructions later):
- GAP 2000: 0 of 60 runs lose a character.
- GAP 777: 8 of 60 still do (the I/O agent's sweep: 66 of 300).

On real hardware a 9600-baud character arrives about every 1 ms, so
back-to-back characters at full line speed can still occasionally
collide; human typing cannot. A complete fix needs the kernel to run with
interrupts enabled, which is a much larger change. Polling overhead:
12,323 polls for the whole sweep workload, about 300K instructions,
negligible.

## 12. Keystroke latency, measured from a full trace

Section 11's sweep *sampled* latency at 60-120 injection points, and each
sweep turned up a different slow spot. `kernel/gapprof.py` measures it
directly instead: from a complete SIMH instruction trace of the I/O
workload (deterministic clock, `d debug 100003`), it lists every kernel
stretch that runs with interrupts off and without calling `tty_poll()`,
longest first, with a per-function profile. The longest such stretch is
the worst-case delay before a typed character is taken.

| step | longest unpolled stretch (whole run, boot and shutdown included) |
|---|---|
| start of this pass | 177,014 (boot stack fill); steady state about 5,000 (path lookup) |
| polls in path lookup, `fs_iread`, dir scans, `args_one`, boot loops | 12,719 (kernel console messages) |
| poll per character in `kputs`/`kputbase` | 3,269 (`fs_iread` every 4th call) |
| poll every `fs_iread`/`fs_iwrite`, `proc_alloc` per page, pmm scans every 16 | **1,936** |

What's left at the top is the image loader's per-word `kfar_write`
sequence and a disk-buffer miss, at about 1,900 instructions. At the
4800 baud `eclipse-run.sh` uses for the console (about 2 ms per
character, roughly 1 us per S/140 instruction), that keeps a continuous
burst of input from overrunning TTI's one-character buffer. At 9600
baud it is borderline for back-to-back characters (a paste) during heavy
kernel work, and still fine for typing.

`examples/pmm.c` gained an optional `PMM_POLL` hook (the kernel passes
`-DPMM_POLL=tty_poll`); without it nothing changes.

**Cost**, exact instruction counts (`set clock nocalibrate`, then SIMH
`show clock`) for the same workload: 9,679,612 with every poll call site
present but `tty_poll` returning at once, 9,920,352 polling: +2.5%.
Measurement caveat: the kernel's "ticks" statistic is *not* an
instruction count. SIMH's idle detection advances simulated time while
the CPU sits in the idle loop, so ticks also depend on when scripted
input happens to arrive (405 vs 212 ticks for these two builds with
near-identical instruction counts).

## 13. rename, ps, kill and ^C; the kernel at -Os

New syscalls for the text and system tools (SHELL_NOTES.md section 6):

| # | call | notes |
|---|---|---|
| 37 | `SYS_RENAME from, to` | `fs_rename`: add the new entry, then remove the old. A file at `to` is unlinked first, but not while it is open. A directory only moves within its own parent, so `..` and link counts stay right. |
| 38 | `SYS_PSINFO pid, w[9]` | state, parent, pages (image + stack + segment), 12-byte name. The name is the last path component, saved at spawn. -1 past the last slot. |
| 39 | `SYS_KILL pid` | see below. Refused for a free/zombie/embryo slot and for a parentless process (init). |
| 18 | `SYS_FREEBLK what` | 0 free data blocks (the old call), 1 data blocks, 2 free inodes, 3 inodes. |

**Killing.** The process exits with status -9 (`KILLSTAT`).

- At the user boundary (preempted, `P_RUN` and not in the kernel, or
  blocked in `wait`): it exits at once.
- Inside the kernel (asleep, woken but not yet out, or the caller itself
  in a syscall): `p_killed` is set, and the process exits at the end of
  `do_syscall`. If it sleeps on the console (`CHAN_TTY`) or a pipe, it is
  woken; those loops check `kkilled()` and return early. Disk and fs-lock
  sleeps are short and run to completion.

**^C** in `tty_intr` drops the typed-ahead input, echoes `^C` and only
sets `tty_sigint`. `sched()` handles it on entry, because every caller of
`sched()` has already saved the current process's state, so any process
may exit there. `intr_fg()` kills every process its parent is
`wait()`ing for, except a child of a parentless process (the login
shell). It picks all targets before killing any, so killing a parent
can't orphan a target and hide it. `tty_intr` can also run from
`tty_poll()` deep inside kernel loops, which is why the kill can't
happen in `tty_intr` itself.

**The kernel now builds at -Os** (`KOPT`, default `-Os`). At -O0 these
syscalls (+1720 words) put it over 32KW. At -Os it is 30762 words (dsk),
against about 31900 at -O0. The code shrinks about 11%, but 6912 of the
words are the nine 768-word kernel stacks. What the optimizer needed
(DEBUGGING_NOTES.md #48):

- `"memory"` clobbers on all 66 inline asm statements;
- `volatile` + `used` on the statics that asm names only as text;
- `noinline` on the functions whose asm defines labels;
- a register-allocator fallback in `eclipse-cc` (`sys_pipe` needs the
  basic allocator).

`build.sh`'s first pass now sizes the boot stack at 768, not 1536
(`main` needs about 450; the second pass sizes it exactly), or the RAM
disk kernel's first pass didn't fit.

Verification, all on the -Os kernel:

| check | result |
|---|---|
| `tests/session_full.txt` (dsk) | same as the -O0 run, apart from inode numbers and the `/bin` count (11 new programs) |
| same, KDISK=dkp (DG 4231) and KDISK=ram (tape only, no disk) | same output; ram: 736 frames free at the end (256 hold the RAM disk) |
| autotest + rc, two boots | `fstest` 40/40, writers errs=0, `badptr` -2, free blocks 506 on both boots, `fsck` 0 problems after each |
| `tests/session_keep.txt` | `fsck` 0 problems |
| `tests/ed_session.txt`, `basic_session.txt`, `basic_deep.txt` | faults=0, 992 frames free. Both BASIC transcripts are identical to the -O0 kernel's apart from the final HALT address. |
| `tests/tools_session.txt` | see SHELL_NOTES.md section 6 |

**Keystroke latency on the -Os kernel.** I traced `tests/session_full.txt`
under `kernel/gapprof.py` (full instruction trace; note that the
Eclipse simulator writes it to `./trace.log`, whatever `set debug` says),
and ran the committed -O0 kernel on the same session for comparison.
Section 12's 1,936 came from a different workload.

| kernel | longest unpolled stretch | where |
|---|---|---|
| committed, -O0 | 6,482 | `SYS_WRITE` (`sys_write_user`): the whole string out to the polled console with no `tty_poll` |
| -Os | 5,338 | same path |
| -Os, plus a poll per character in `sys_write_user` (as `sys_writefd` already had) | **3,229** | a polled DSK read wait inside `bread` (about 3,000 at -O0 too) |

Session output is identical with and without the new poll. Open: at
about 1 us per instruction, 3.2 ms is more than one 4800-baud
character time (2.1 ms). So a continuous burst of typing during a
polled disk read can still lose a character. The fix would be to poll
inside the DSK wait loop.
