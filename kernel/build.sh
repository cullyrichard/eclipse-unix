#!/usr/bin/env bash
# build.sh -- build the kernel (out/kernel.simh), every user program in
# user/, and a fresh filesystem image (out/disk.img) with the programs in
# /bin. See KERNEL_NOTES.md and FS_NOTES.md.
#   UCFLAGS=..  extra flags for the user programs (e.g. -DROUNDS=200)
#   KCFLAGS=..  extra flags for the kernel (e.g. -DKDEBUG)
#   KEEPDISK=1  keep an existing out/disk.img (to test persistence)
#   EXTRA="host:/fs/path ..."  more files for the image (e.g. an /etc/rc)
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cc="$here/../eclipse-toolchain/eclipse-cc"
out="${OUT:-$here/out}"
mkdir -p "$out"
export PATH="$HOME/dev/dgasm-src:$PATH"

progs="init sh echo cat ls mkdir rm wc cp pwd fstest fswriter worker badptr badread escape bigmem winfault spinner ttyread"
files=()
for p in $progs; do
  ECLIPSE_CC_KEEP_ASM="$out/$p.s" "$cc" --ieee ${UCFLAGS:-} -o "$out/$p.simh" \
    "$here/user/$p.c" "$here/usys.c" >/dev/null
  files+=("$out/$p.simh:/bin/$p")
done

# boot stack: main() only runs until k_start(); the kernel is ~30KW, so the
# default 4096-word margin would pass 32KW (reorder_asm.py LOGICAL_TOP)
ECLIPSE_STACK_WORDS=1536 ECLIPSE_CC_KEEP_ASM="$out/kernel.s" "$cc" --ieee -o "$out/kernel.simh" ${KCFLAGS:-} \
  "$here/kmain.c" "$here/proc.c" "$here/kmem.c" "$here/kio.c" \
  "$here/fs.c" "$here/fsys.c" "$here/tty.c" "$here/kdsk.c" \
  "$here/../examples/pmm.c" "$here/../examples/console.c" \
  "$here/../examples/dsk.c" \
  "$here/kentry.s"

if [ "${KEEPDISK:-0}" = 1 ] && [ -f "$out/disk.img" ]; then
  echo "build.sh: keeping $out/disk.img"
else
  python3 "$here/fstools/mkfs.py" "$out/disk.img" "${files[@]}" ${EXTRA:-}
fi

# Kernel stack budget: the static worst-case depth (stackdepth.py, call
# graph + SAVE frames; no recursion) must fit in a kernel stack with the
# 16-word overflow guard (k_klim) and a 32-word margin for the trap-entry
# argument push and the MUL/DIV/indirect-access temporaries.
kstk=$(sed -n 's/^#define KSTK *\([0-9]*\).*/\1/p' "$here/proc.h")
worst=$(python3 "$here/stackdepth.py" "$out/kernel.s" k_trap main | sed -n 's/.*worst case \([0-9]*\) words/\1/p' | sort -n | tail -1)
if [ "$worst" -gt $((kstk - 16 - 32)) ]; then
  echo "build.sh: kernel stack worst case $worst words exceeds KSTK=$kstk - 48" >&2
  python3 "$here/stackdepth.py" "$out/kernel.s" k_trap >&2
  exit 1
fi
echo "build.sh: kernel stack worst case $worst of $kstk words"
