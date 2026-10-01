# FS — a V6-style filesystem on DSK, and booting `/bin/init` from it

> Continued in `SHELL_NOTES.md`: argv, cwd, dup/dup2, pipes, stat, `/bin/sh`
> and utilities; `init` now ends in an interactive shell.

Phase 8 of the OS roadmap. The kernel (`KERNEL_NOTES.md`) now mounts a
filesystem on DSK unit 0 at boot and runs `/bin/init` **from disk**. User
programs are ordinary files in `/bin`, loaded by a new `spawn(path)`
syscall, no longer images compiled into the kernel. Processes get Unix-style file
descriptors: `open`, `close`, `read`, `write`, `lseek`, `mkdir`,
`unlink`, and a blocking `wait(pid)`.

**Bottom line**: verified in SIMH, and cross-checked by an independent
implementation of the format:
- `fstest` passes all 39 checks, including every error path.
- Three writers running concurrently next to a CPU-bound worker and a
  faulting process produce files that a host-side Python reader confirms
  byte for byte.
- A host-side `fsck` (independent of the kernel code, and itself
  mutation-tested) finds 0 problems after every boot.
- State persists across three consecutive boots of the same disk.

## 1. On-disk format (`kernel/fs.h`; host copy `kernel/fstools/fslib.py`)

DSK is flat, with 256-word sectors (STORAGE_NOTES.md), so a block is 512
bytes, which is the same as V6. The default image is 1024 blocks (the
default DSK unit size) with 128 inodes:

| blocks | contents |
|---|---|
| 0 | boot (unused) |
| 1 | superblock: magic `0105106`, nblocks, ninodes, bitmap start, inode start, data start |
| 2 | free-block bitmap: bit `b&15` of word `b>>4`; 1 = in use; metadata blocks are marked in use |
| 3-10 | inodes: 16 words each, 16 per block; inode 0 unused, root = 1 |
| 11- | data |

- **Inode**: type (0 free, 1 file, 2 dir), nlink, size (16-bit byte count,
  so a file holds at most 65535 bytes), 8 direct blocks, 1 indirect block
  (256 more). The format could hold 135168 bytes; the 16-bit size is the
  real limit.
- **Directory**: an ordinary file of 16-byte entries: a 2-byte inode
  number, then a 14-byte NUL-padded name (exactly V6). Inode 0 marks a free
  slot. Every directory starts with `.` and `..`, and
  `nlink = number of entries naming it` (2 + subdirectories for a
  directory).
- **Byte order**: the Eclipse sees a word's bytes high-first (LDB/STB,
  DEBUGGING_NOTES.md #38), so file data is the big-endian packing of each
  block's words. SIMH's attach file stores each word little-endian, so
  host tools must go through words (`fslib.Image`), never raw file bytes.
  The kernel needs no conversion at all: a block buffer is a word array,
  viewed through a `char *`.
- **Executables**: `word 0 = 0407` (V6's magic), `nwords`, `base`,
  `stacktop`, then the image words. `mkfs.py` builds these from
  `eclipse-cc` output, recovering `_STACKTOP` from the program's own
  `LDA 0,_STACKTOP` at 050 and checking it rather than assuming it.

## 2. Kernel side

- `kernel/fs.c`: buffer cache (8 blocks, LRU, **write-through**, so the
  disk is consistent whenever no syscall is in progress),
  `balloc`/`bfree`, inode read/write/alloc, `bmap` (direct + indirect,
  allocate-on-write), `readi`/`writei`/`trunc`, directory
  lookup/link/unlink, path walk, `create`, `mkdir`, `unlink`. The kernel
  is single-threaded and runs with ION off, so nothing needs locking.
  Anything that may call `bread` again re-reads its buffer afterwards
  (e.g. `bmap` after `balloc`).
- `kernel/fsys.c`: a 16-entry open-file table and 8 descriptors per
  process. fds 0-2 start as the console: reads are line-at-a-time with
  echo, and writes do `\n`→`\r\n`. User buffers and paths are byte
  addresses in the caller's space, validated page by page against its
  page table before any byte moves. That matters because the kernel
  reaches user memory with single-cycle mapping, which skips the MMPU's
  own validity check (KERNEL_NOTES.md §4). A bad pointer returns -1; it
  never faults the kernel.
- `kernel/proc.c`: `proc_create` is split into `proc_alloc` (the address
  space) plus a loader. `proc_spawn(path)` streams an executable from the
  filesystem into the new frames. Processes gain a parent, an exit status,
  `P_ZOMBIE` (status kept for `wait`) and `P_WAIT` (blocked in `wait` for
  a specific child). Exit hands the status straight to a waiting parent,
  orphans the process's children, and closes its descriptors. A killed
  process exits with status -2. Process slots are now reused; previously
  zombies were never freed.
- **Syscall ABI**: a third argument travels in AC3. `SYC` already pushes
  and `POPB` restores all four ACs, and the kernel entry already captures
  AC3, so only the user stub changed.

```
9 open(path, flags)  10 close(fd)  11 read(fd,buf,n)  12 write(fd,buf,n)
13 lseek(fd,off,whence)  14 mkdir(path)  15 unlink(path)
16 spawn(path) -> pid  17 wait(pid) -> status  18 freeblocks()
flags: O_RDONLY 0, O_WRONLY 1, O_RDWR 2, O_CREAT 0100, O_TRUNC 01000
```

Paths are absolute (a leading `/` is optional); there is no current
directory yet. `unlink` refuses an open file and a non-empty directory.

## 3. Tools

```
kernel/build.sh            # user programs -> kernel -> mkfs out/disk.img (/bin/*)
KEEPDISK=1 kernel/build.sh # rebuild but keep the existing disk
kernel/run.sh              # attach out/disk.img as DSK 0, boot, console to stdout
kernel/fstools/mkfs.py image [-n blocks] [-i inodes] src:dest ...
kernel/fstools/fsck.py image [-l] [-x path]
```

`fsck.py` checks:
- the superblock;
- every inode's blocks are in range, owned by exactly one inode, and none
  lies beyond the file size;
- the bitmap equals exactly metadata plus owned blocks;
- every directory's `.` and `..` are correct;
- every entry names an allocated inode;
- every link count equals the number of entries naming it;
- every allocated inode is reachable from the root.

`kernel/mkimage.py` and the images compiled into the kernel are gone:
`mkfs.py` replaces them.

## 4. Verification

### `fsck` itself, mutation-tested

Six deliberate corruptions of a kernel-written image, each caught with
exit status 1:

```
bitmap: leaked block         fsck: bitmap: block 1023 is 1, should be 0
wrong link count             fsck: inode 14: nlink 2, 1 entries
block owned twice            fsck: block 80: owned by inodes 16 and 17
in-use block marked free     fsck: bitmap: block 56 is 0, should be 1
unreachable inode            fsck: inode 19: allocated but unreachable
corrupt .. entry             fsck: /tmp: bad . / .. entries
```

### Three boots of one disk (`build.sh` once, then `run.sh` three times)

```
=== boot 1
init: boot #1
init: free blocks 945
fstest: 39 checks, 0 failed
init: /bin/fstest exited 0
init: concurrent phase
init: pid 1 exited 0         (fswriter)
init: pid 2 exited 0         (fswriter)
init: pid 3 exited 0         (worker)
init: pid 4 exited 0         (fswriter)
init: pid 5 exited -2        (badptr, killed as intended)
init: /bin/ls exited 0
init: free blocks 905, failures 0
fsck: 17 names, 108 data blocks in use, 905 free, 0 problems
=== boot 2: init: boot #2 ... identical results, fsck 0 problems, 905 free
=== boot 3: init: boot #3 ... identical results, fsck 0 problems, 905 free
```

- The boot counter in `/etc/boots` (read, incremented, rewritten in place
  with `lseek`) survives each reboot, confirmed from the host by
  `fsck.py -x /etc/boots` → `1`, `2`, `3`.
- The free count is identical after every boot. Each boot recreates the
  writers' files with `O_TRUNC` and `fstest` deletes everything it
  creates, so any leak or double-free in `trunc`/`unlink`/`balloc` would
  show here. `fstest` also checks internally that the count comes back
  exactly.
- `fsck` reports 0 problems after every boot, on the image as the kernel
  left it.

### `fstest` (39 checks)

- Error paths: missing file, existing directory, create in a missing
  directory, a 15-byte name, read on a write-only fd, write on a
  read-only fd, unlink while open, closing twice.
- Small-file round trip, append, and size via `lseek`.
- A 20000-byte file, which uses the indirect block: written in 100-byte
  chunks and read back in 77-byte chunks, with bytes ≥ 0200 included, so
  the `char` sign handling is exercised.
- An overwrite straddling a block boundary (1020-1029) with its neighbours
  checked, and the size unchanged afterwards.
- `O_TRUNC`.
- Unmapped user buffers for `read` and for a path: both return -1.
- Directories: create and link, a non-empty directory can't be removed,
  `.` can't be unlinked, and a directory is listed by reading its entries.
- Full cleanup, with the free-block count restored exactly.
- Descriptor-table exhaustion at 8.

### Concurrent writers, checked from the host

Each `fswriter` writes `/tmp/w<pid>` (6000 bytes, byte `i = (i*pid +
7*pid) & 255`) in 50-byte writes while the others, a CPU-bound `worker`
and the faulting `badptr` run. It then reads the file back and verifies
it itself (all `errs=0`). The files are left on disk, and the host
extracts them with `fsck.py -x` and compares them against the same
formula computed in Python:

```
/tmp/w1: 6000 bytes, host-computed pattern match: True
/tmp/w2: 6000 bytes, host-computed pattern match: True
/tmp/w4: 6000 bytes, host-computed pattern match: True
```

### Loading programs from disk

Every process in the runs above came from `/bin` via `spawn`, `init`
included. `bigmem` (KERNEL_NOTES.md §9), installed as `/bin/init` on its
own image, still maps and verifies its 96KW segment with `errs=0` and
exits cleanly, with all 992 frames free. So the `proc_alloc` refactor
didn't regress the >32KW path.

## 5. What's open

- **Update: done, see IO_NOTES.md** -- disk transfers are interrupt-driven
  and the caller sleeps; the filesystem is one sleep-lock. Original note:
- **Every disk operation blocks the whole machine.** The kernel polls DSK
  with ION off (the same limitation as console input, KERNEL_NOTES.md §8).
  Interrupt-driven I/O with sleep/wakeup is the next structural step, and
  now matters more.
- **Size limits**: 16-bit sizes and offsets (files up to 65535 bytes), one
  disk, 1024 blocks, 128 inodes. `lseek` doesn't reject seeking past EOF,
  but `write` there returns -1 (no holes).
- No current directory, no `argv`, no `exec` that replaces a process (only
  `spawn` + `wait`), no `fork`, no pipes, no permissions or owners, no
  timestamps, no `stat` (`ls` gets sizes with `lseek(fd,0,2)`).
- **Crash consistency**: the cache is write-through, but a multi-block
  update such as `mkdir` (inode + directory block + parent) is not atomic.
  A kill in the middle of one can't happen (syscalls run ION-off), but a
  power loss could leave something `fsck` would flag. It only checks,
  never repairs.
- **Real hardware**: DSK is the SIMH-verifiable backend. The DSKP-family
  drivers (Zebra/Vulcan/Kismet) still aren't behind `blockdev.h`, and
  `fs.c` calls `dsk_read_block`/`dsk_write_block` directly. Moving it onto
  `blockdev_t` is the step that lets this filesystem run on your real
  drives.

## 6. Block-device layer and real-drive backends (`kernel/bdev.h`)

`fs.c` now talks only to `bdev_read`/`bdev_write`. One backend is linked
per kernel, chosen with `KDISK` at build time (`kernel/build.sh`):

| KDISK | backend | device | how | verified |
|---|---|---|---|---|
| `dsk` (default) | `bdev_dsk.c` | DG 4019 fixed-head, 020 | interrupt-driven, caller sleeps | SIMH |
| `dkp` | `bdev_dkp.c` | DG 4231 (3330-class, about 92 MB) on the DKP controller, 033 | polled; seek, then CHS transfer | SIMH (`set dkp0 4231`) |
| `zebra` | `bdev_dskp.c` + `examples/zebra.c` | DG 6060/6061/6067, 027 | polled | builds only |
| `vulcan` | `bdev_dskp.c` + `examples/vulcan.c` | DG 6122, 027 | polled | builds only |
| `kismet` | `bdev_dskp.c` + `examples/kismet.c` | DG 6160/6161/6214, 027 | polled | builds only |

- **Why the 4231, not the D31.** The first DKP backend targeted the 4047
  (Diablo 31) because STORAGE_NOTES.md had probed it. But a 2.4 MB pack is
  Nova-era: by the S/140's years (1979-82) a system would have had a
  3330-class drive or a DG/Disc subsystem. The 4231 is the largest drive
  SIMH's DKP models, and it uses the controller's new register format
  (9-bit cylinder, 5-bit surface and sector).
- **32 MB partition.** Block numbers are 16 bits, so a filesystem holds
  at most 65535 blocks. On every big-disk backend it occupies the first
  65535 blocks; the rest of the drive is unused for now. Going past that
  means 32-bit block numbers, which would depend on the backend's `long`
  arithmetic (DEBUGGING_NOTES.md #12 still lists an open 32-bit multiply
  bug).
- **Polled backends** mask their own completion interrupt
  (`bdev_pimask`) and call `tty_poll()` in their wait loops. Unlike DSK,
  they hold the CPU during the transfer.
- **Stacks sized per backend.** The DG/Disc drivers are deeper:
  `build.sh` now builds twice, sizing the kernel stacks (worst + 48,
  rounded up to 64) and the boot stack (main's worst + 64) from
  `stackdepth.py`. Zebra and Vulcan are built with `DSKP_BLOCK16`, a new
  16-bit-only translation path, so the 32-bit runtime divide (and its
  118-word frame) isn't linked into the kernel.

```
KDISK   kernel stacks  static worst  boot stack  image (words)
dsk     768            658           576         28357
dkp     832            735           640         29089
zebra   896            811           704         30274
vulcan  832            769           640         29469
kismet  832            755           640         29306
```

**Verified on `KDISK=dkp`**: a 65535-block filesystem with 1024 inodes
(16 bitmap blocks, data from block 82). Two boots of autotest:
`fstest` 40/40, writers' files match the host pattern, `fsck` clean,
65308 blocks free both times. The full shell session ran with every
feature and error case behaving as expected. `fstest`'s
"freeblocks() sane" check used to assume a 1024-block disk; it now only
rejects -1. `balloc` and `freeblocks` skip whole bitmap words, since
65535 bits is a lot of per-bit work. DSK re-verified unchanged.

**Not verified**: the Zebra, Vulcan and Kismet kernels have never
executed. SIMH models none of those controllers, so they need your
drives. To build one: `KDISK=vulcan kernel/build.sh` (or `zebra`,
`kismet`; for Kismet add `KCFLAGS=-DKISMET_HEADS=KISMET_HEADS_6161` or
`_6214` for the larger models).
