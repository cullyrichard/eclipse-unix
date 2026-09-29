#!/usr/bin/env bash
# build.sh -- build the N-process kernel image (kernel.simh) and its
# embedded user programs. See KERNEL_NOTES.md.
#   NWORKERS=n  number of worker processes (default 5)
#   UCFLAGS=..  extra flags for the user programs (e.g. -DROUNDS=200)
#   KCFLAGS=..  extra flags for the kernel (e.g. -DKDEBUG)
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cc="$here/../eclipse-toolchain/eclipse-cc"
out="${OUT:-$here/out}"
mkdir -p "$out"
export PATH="$HOME/dev/dgasm-src:$PATH"

for p in worker badptr escape badread; do
  ECLIPSE_CC_KEEP_ASM="$out/$p.s" "$cc" --ieee ${UCFLAGS:-} -o "$out/$p.simh" "$here/user/$p.c" "$here/usys.c" >/dev/null
  python3 "$here/mkimage.py" "$out/$p.simh" "img_$p" > "$out/img_$p.h"
done

ECLIPSE_CC_KEEP_ASM="$out/kernel.s" "$cc" --ieee -o "$out/kernel.simh" -I"$out" \
  -DNWORKERS="${NWORKERS:-5}" ${KCFLAGS:-} \
  "$here/kmain.c" "$here/proc.c" "$here/kmem.c" "$here/kio.c" \
  "$here/../examples/pmm.c" "$here/../examples/console.c" \
  "$here/kentry.s"
