#!/usr/bin/env bash
# build-realhw.sh -- rebuild every real-hardware (FPS array processor)
# program as a DG absolute binary for eclipse-run.sh's serial loader.
# --ieee: the default hardware-float mode gives wrong results on the real
# machine's FPU (DEBUGGING_NOTES.md #34). None of these can run in SIMH
# (no FPS model): built and assembled here, executed only on the machine.
# usage: examples/build-realhw.sh [outdir]   (default: examples/out-realhw)
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="${1:-$here/out-realhw}"
mkdir -p "$out"
export PATH="$HOME/dev/dgasm-src:$PATH"
for f in "$here"/mandel240_diag*.c "$here"/mandel240_view.c "$here"/fps_dma_test.c \
         "$here"/fps_dma_test2.c "$here"/sixel_fps_test_1.c "$here"/test_fps_add.c \
         "$here"/test_fps_md.c; do
  b="$(basename "$f" .c)"
  extra=()
  grep -q 'fps.h' "$f" && extra=("$here/fps.c")
  "$here/../eclipse-toolchain/eclipse-compile.sh" --ieee -o "$out/$b.ab" "$f" "${extra[@]}" >/dev/null
  echo "built $out/$b.ab"
done
