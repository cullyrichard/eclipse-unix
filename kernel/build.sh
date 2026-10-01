#!/usr/bin/env bash
# build.sh -- build the kernel (out/kernel.simh), every user program in
# user/, and a fresh filesystem image (out/disk.img) with the programs in
# /bin. See KERNEL_NOTES.md and FS_NOTES.md.
#   UCFLAGS=..  extra flags for the user programs (e.g. -DROUNDS=200)
#   KCFLAGS=..  extra flags for the kernel (e.g. -DKDEBUG)
#   KOPT=..     kernel optimization (default -Os; at -O0, KOPT=, the kernel
#               no longer fits in 32KW -- KERNEL_NOTES.md section 13)
#   KEEPDISK=1  keep an existing out/disk.img (to test persistence)
#   KDISK=dsk|dkp|zebra|vulcan|kismet|ram   block-device backend (bdev.h;
#               default dsk). dkp = DG 4231 on SIMH DKP; zebra/vulcan/
#               kismet = the real DG/Disc drives, not runnable in SIMH.
#   EXTRA="host:/fs/path ..."  more files for the image (e.g. an /etc/rc)
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cc="$here/../eclipse-toolchain/eclipse-cc"
out="${OUT:-$here/out}"
mkdir -p "$out"
export PATH="$HOME/dev/dgasm-src:$PATH"

kdisk="${KDISK:-dsk}"
case "$kdisk" in
  dsk)    bdev_src=("$here/bdev_dsk.c" "$here/../examples/dsk.c"); mkfs_args=(-n 1024 -i 128) ;;
  dkp)    bdev_src=("$here/bdev_dkp.c"); mkfs_args=(-n 65535 -i 1024) ;;
  zebra)  bdev_src=("-DDSKP_VARIANT_ZEBRA" "-DDSKP_BLOCK16" "$here/bdev_dskp.c" "$here/../examples/zebra.c" "$here/../examples/dskp_common.c"); mkfs_args=(-n 65535 -i 1024) ;;
  vulcan) bdev_src=("-DDSKP_VARIANT_VULCAN" "-DDSKP_BLOCK16" "$here/bdev_dskp.c" "$here/../examples/vulcan.c" "$here/../examples/dskp_common.c"); mkfs_args=(-n 65535 -i 1024) ;;
  kismet) bdev_src=("-DDSKP_VARIANT_KISMET" "$here/bdev_dskp.c" "$here/../examples/kismet.c" "$here/../examples/dskp_common.c"); mkfs_args=(-n 65535 -i 1024) ;;
  ram)    bdev_src=("$here/bdev_ram.c"); mkfs_args=(-n 1024 -i 128) ;;
  *) echo "build.sh: unknown KDISK=$kdisk" >&2; exit 1 ;;
esac
echo "$kdisk" > "$out/kdisk"

progs="init sh tar ed basic echo cat ls mkdir rm wc cp pwd grep head tail sort od cmp mv more ps kill df fstest fswriter worker badptr badread escape bigmem winfault spinner ttyread"
files=()
# Per-program stack (words above _STACKTOP; the kernel maps at least
# STACKWORDS=1024 of it, more if the image's header asks) and -O level.
# basic needs -Oz to fit under 32KW at all (SHELL_NOTES.md section 6).
for p in $progs; do
  pstk=1024; popt=
  case "$p" in
    basic) pstk=2560; popt=-Oz ;;
  esac
  ECLIPSE_STACK_WORDS=$pstk ECLIPSE_CC_KEEP_ASM="$out/$p.s" "$cc" --ieee $popt ${UCFLAGS:-} -o "$out/$p.simh" \
    "$here/user/$p.c" "$here/usys.c" >/dev/null
  files+=("$out/$p.simh:/bin/$p")
done

build_kernel() {
# boot stack: main() only runs until k_start(); the kernel is ~30KW, so the
# default 4096-word margin would pass 32KW (reorder_asm.py LOGICAL_TOP).
# Pass 1 uses 768 (main needs ~450); pass 2 uses main's static worst case + 64.
# KOPT: the kernel's -O level, -Os unless set. At -O0 it no longer fits
# with the text/system-tool syscalls (KERNEL_NOTES.md section 13).
ECLIPSE_STACK_WORDS=${bootstk:-768} ECLIPSE_CC_KEEP_ASM="$out/kernel.s" "$cc" --ieee ${KOPT--Os} -o "$out/kernel.simh" -DPMM_POLL=tty_poll ${KCFLAGS:-} "$@" \
  "$here/kmain.c" "$here/proc.c" "$here/kmem.c" "$here/kio.c" \
  "$here/fs.c" "$here/fsys.c" "$here/tty.c" "$here/tapeinst.c" "$here/tape.c" "${bdev_src[@]}" \
  "$here/../examples/pmm.c" "$here/../examples/console.c" \
  "$here/kentry.s"

}

# Kernel stacks sized from the static worst case (stackdepth.py: call
# graph + SAVE frames, no recursion): build, measure, and rebuild with
# KSTK = worst + 48 (16-word overflow guard + 32 for the trap-entry
# argument push and MUL/DIV/indirect temporaries), rounded up to 64.
build_kernel
worst=$(python3 "$here/stackdepth.py" "$out/kernel.s" k_trap main | sed -n 's/.*worst case \([0-9]*\) words/\1/p' | sort -n | tail -1)
mainw=$(python3 "$here/stackdepth.py" "$out/kernel.s" main | sed -n 's/.*worst case \([0-9]*\) words/\1/p')
kstk=$(( (worst + 48 + 63) / 64 * 64 ))
bootstk=$(( (mainw + 64 + 63) / 64 * 64 ))
build_kernel -DKSTK=$kstk
worst2=$(python3 "$here/stackdepth.py" "$out/kernel.s" k_trap main | sed -n 's/.*worst case \([0-9]*\) words/\1/p' | sort -n | tail -1)
if [ "$worst2" -gt $((kstk - 48)) ]; then
  echo "build.sh: kernel stack worst case $worst2 exceeds KSTK=$kstk - 48" >&2
  exit 1
fi
echo "build.sh: kernel stacks $kstk words (static worst case $worst2), boot stack $bootstk"

if [ "${KEEPDISK:-0}" = 1 ] && [ -f "$out/disk.img" ]; then
  echo "build.sh: keeping $out/disk.img"
else
  python3 "$here/fstools/mkfs.py" "$out/disk.img" "${mkfs_args[@]}" "${files[@]}" ${EXTRA:-}
fi

# Real-hardware media (FS_NOTES.md section 7): the kernel as a DG absolute
# binary (serial load via eclipse-run.sh's APL path), as a boot tape, and
# an install tape that also carries the filesystem for tapeinst.c.
dgasm -t eclipse_s140 -f ab -o "$out/kernel.ab" "$out/kernel.s"
python3 "$here/../eclipse-toolchain/eclipse-ab2tape" -o "$out/kernel.tap" "$out/kernel.ab" >/dev/null
python3 "$here/fstools/mkinstalltape.py" "$out/kernel.ab" "$out/disk.img" "$out/install.tap"

