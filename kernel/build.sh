#!/usr/bin/env bash
# build.sh -- build the kernel (out/kernel.simh), every user program in
# user/, and a fresh filesystem image (out/disk.img) with the programs in
# /bin. See KERNEL_NOTES.md and FS_NOTES.md.
#   UCFLAGS=..  extra flags for the user programs (e.g. -DROUNDS=200)
#   KCFLAGS=..  extra flags for the kernel (e.g. -DKDEBUG)
#   KEEPDISK=1  keep an existing out/disk.img (to test persistence)
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cc="$here/../eclipse-toolchain/eclipse-cc"
out="${OUT:-$here/out}"
mkdir -p "$out"
export PATH="$HOME/dev/dgasm-src:$PATH"

progs="init fstest fswriter ls worker badptr badread escape bigmem winfault spinner ttyread"
files=()
for p in $progs; do
  ECLIPSE_CC_KEEP_ASM="$out/$p.s" "$cc" --ieee ${UCFLAGS:-} -o "$out/$p.simh" \
    "$here/user/$p.c" "$here/usys.c" >/dev/null
  files+=("$out/$p.simh:/bin/$p")
done

ECLIPSE_CC_KEEP_ASM="$out/kernel.s" "$cc" --ieee -o "$out/kernel.simh" ${KCFLAGS:-} \
  "$here/kmain.c" "$here/proc.c" "$here/kmem.c" "$here/kio.c" \
  "$here/fs.c" "$here/fsys.c" "$here/tty.c" "$here/kdsk.c" \
  "$here/../examples/pmm.c" "$here/../examples/console.c" \
  "$here/../examples/dsk.c" \
  "$here/kentry.s"

if [ "${KEEPDISK:-0}" = 1 ] && [ -f "$out/disk.img" ]; then
  echo "build.sh: keeping $out/disk.img"
else
  python3 "$here/fstools/mkfs.py" "$out/disk.img" "${files[@]}"
fi
