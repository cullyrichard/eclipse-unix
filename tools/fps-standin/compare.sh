#!/usr/bin/env bash
# compare.sh BASELINE_LLVM_BUILD -- check that the current compiler drives
# the FPS-100 exactly as a baseline compiler does, for every real-hardware
# program in examples/build-realhw.sh. Each program is built twice through
# eclipse-compile.sh (the build-realhw.sh path), with --ieee, by the
# baseline LLVM and the current one, then run under eclipse-fpsx. Compared:
# console output, and every FPS-100 operation (DOA/DOB/DIx/NIOx on 054/055,
# value included). A differing value is accepted only if it is the address
# of the same symbol+offset in each build's own listing, or the same
# offset from each build's _STACKTOP (a buffer in main's frame).
# Needs: tools/fps-standin/eclipse-fpsx (build-sim.sh), dgasm on PATH.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; ex="$here/../../examples"
base="${1:?usage: compare.sh BASELINE_LLVM_BUILD}"
w="$(mktemp -d)"; trap 'rm -rf "$w"' EXIT
export PATH="$HOME/dev/dgasm-src:$PATH"
sed 's|  dgasm -t eclipse_s140 -f ab -o "$out" "$work/prog_r.s"|  cp "$work/prog_r.s" "$out.s"\n  dgasm -t eclipse_s140 -f ab -o "$out" "$work/prog_r.s"|' \
  "$here/../../eclipse-toolchain/eclipse-compile.sh" > "$w/compile-keep.sh"; chmod +x "$w/compile-keep.sh"
cd "$ex"
for f in mandel240_diag*.c mandel240_view.c fps_dma_test.c fps_dma_test2.c sixel_fps_test_1.c test_fps_add.c test_fps_md.c; do
  b="${f%.c}"; extra=(); grep -q 'fps.h' "$f" && extra=(fps.c)
  for v in B C; do
    mkdir -p "$w/$v/$b"
    if [ $v = B ]; then LLVM_BUILD="$base" "$w/compile-keep.sh" --ieee -o "$w/$v/$b.ab" "$f" "${extra[@]}" >/dev/null
    else "$w/compile-keep.sh" --ieee -o "$w/$v/$b.ab" "$f" "${extra[@]}" >/dev/null; fi
    (cd "$w/$v/$b" && { echo "load $w/$v/$b.ab"; echo 'dep PC 50'; echo go; } \
      | timeout 120 "$here/eclipse-fpsx" 2>&1 | tr -d '\r' | sed -e '1,/^ECLIPSE simulator/d' -e '/^HALT/,$d' > console.txt)
  done
done
python3 - "$w" "$here" <<'PY'
import importlib.util, os, re, subprocess, sys
w, here = sys.argv[1], sys.argv[2]
spec = importlib.util.spec_from_file_location('r', os.path.join(here, '../../eclipse-toolchain/reorder_asm.py'))
r = importlib.util.module_from_spec(spec); spec.loader.exec_module(r)
def stacktop(ab):
    run = lambda c: subprocess.run([here + '/eclipse-fpsx'], input=f'load {ab}\n{c}\n', capture_output=True, text=True).stdout
    pz = int(re.search(r'\b50:\s+([0-7]+)', run('ex 50')).group(1), 8) & 0o377
    return int(re.search(r'\b%o:\s+([0-7]+)' % pz, run('ex %o' % pz)).group(1), 8)
def syms(path):
    la, _, end = r.compute_addresses(open(path).read().splitlines())
    return sorted((a, n) for n, a in la.items() if not n.startswith(('LBB', 'CPI', 'Lrx_')) and '_w' not in n), end
def sym(t, v):
    items, end = t; best = None
    for a, n in items:
        if a <= v: best = (a, n)
        else: break
    return f'{best[1]}+{v - best[0]}' if best and v < end else None
bad_total = 0
for b in sorted(d for d in os.listdir(w + '/B') if os.path.isdir(f'{w}/B/{d}')):
    con = open(f'{w}/B/{b}/console.txt').read() == open(f'{w}/C/{b}/console.txt').read()
    lb = open(f'{w}/B/{b}/fpsx.log').read().split('\n'); lc = open(f'{w}/C/{b}/fpsx.log').read().split('\n')
    tb, tc = syms(f'{w}/B/{b}.ab.s'), syms(f'{w}/C/{b}.ab.s')
    sb, sc = stacktop(f'{w}/B/{b}.ab'), stacktop(f'{w}/C/{b}.ab')
    same = moved = bad = 0
    if len(lb) != len(lc): bad = abs(len(lb) - len(lc)) or 1
    for x, y in zip(lb, lc):
        if x == y: same += 1; continue
        px, py = x.split(), y.split(); vx, vy = int(px[2], 8), int(py[2], 8)
        if px[:2] == py[:2] and ((sym(tb, vx) and sym(tb, vx) == sym(tc, vy)) or vx - sb == vy - sc): moved += 1
        else: bad += 1
    bad_total += bad + (0 if con else 1)
    print(f'{b:18s} console {"same" if con else "DIFFERENT"}  fps ops: {same} identical, {moved} same buffer at a moved address, {bad} different')
print('RESULT:', 'all programs drive the FPS-100 identically' if bad_total == 0 else f'{bad_total} differences')
sys.exit(1 if bad_total else 0)
PY
