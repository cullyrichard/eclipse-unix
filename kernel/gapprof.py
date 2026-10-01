#!/usr/bin/env python3
"""gapprof.py -- longest kernel stretches with interrupts off and no
tty_poll() call (KERNEL_NOTES.md section 11): the worst-case keystroke
latency, measured from a full SIMH instruction trace instead of sampled.
usage: gapprof.py trace.log kernel.s [top]"""
import re, sys, collections, importlib.util, os
here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('r', os.path.join(here, '..', 'eclipse-toolchain', 'reorder_asm.py'))
r = importlib.util.module_from_spec(spec); spec.loader.exec_module(r)
la, _, _ = r.compute_addresses(open(sys.argv[2]).read().splitlines())
top = int(sys.argv[3]) if len(sys.argv) > 3 else 8
funcs = sorted((a, n) for n, a in la.items() if not re.match(r'(LBB|CPI|L\d|Lrx_|var)', n) and (n[0].islower() or n.startswith('k_')))
starts = [a for a, n in funcs]
import bisect
def fn(pc):
    i = bisect.bisect_right(starts, pc) - 1
    return funcs[i][1] if i >= 0 else '?'
poll = la['tty_poll']
gaps = []          # (length, Counter of functions, first fn, last fn)
cur = 0; prof = collections.Counter(); first = None; last = None
pat = re.compile(r'^([IAB ]{0,2})\s*([0-7]{6}) acs:')
for line in open(sys.argv[1], errors='replace'):
    m = pat.match(line)
    if not m:
        continue
    flags, pc = m.group(1), int(m.group(2), 8)
    if 'I' in flags or 'A' in flags or 'B' in flags or pc == poll:
        if cur:
            gaps.append((cur, prof, first, last))
        cur = 0; prof = collections.Counter(); first = None
        continue
    f = fn(pc)
    if first is None: first = f
    last = f
    cur += 1; prof[f] += 1
gaps.sort(key=lambda g: -g[0])
print(f'kernel ION-off stretches: {len(gaps)}, longest {gaps[0][0] if gaps else 0} instructions')
for n, p, a, b in gaps[:top]:
    print(f'{n:6d}  {a} .. {b}   ' + ', '.join(f'{f} {c}' for f, c in p.most_common(4)))
