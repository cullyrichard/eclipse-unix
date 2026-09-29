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
- **Single programs larger than 32KW** (overlays or far data) are the
  second half of the "beyond 32kW" request, and not started. The
  per-process map this increment built is the natural place to hang it:
  a process can already have its map A rewritten by the kernel, so a
  "map this overlay/data window" syscall is a small step. Transparent far
  pointers in the compiler are the redesign-sized option `MMPU_NOTES.md`
  already scoped.
- Copying an image costs one `kfar_write` (an `LMP` plus a single-cycle
  store) per word. That's fine for a few KW; `BLM` through map B would be
  far faster.
