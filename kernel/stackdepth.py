#!/usr/bin/env python3
"""stackdepth.py -- worst-case kernel stack depth from the call graph in
out/kernel.s (KERNEL_NOTES.md). Each compiled function's cost is its SAVE
frame + 5 (SAVE's own push) + the words it pushes for its calls'
arguments; the MUL/DIV expansions and indirect-access sequences push at
most 2 more words. Depth = max over call paths from k_trap (every kernel
entry calls k_trap on a fresh stack), with no recursion allowed (checked).
usage: stackdepth.py [kernel.s] [root ...]"""
import re, sys
src = sys.argv[1] if len(sys.argv) > 1 else 'out/kernel.s'
roots = sys.argv[2:] or ['k_trap', 'main']
L = open(src).read().splitlines()
frame, calls, pushes, cur = {}, {}, {}, None
for l in L:
    s = l.split('//')[0].strip()
    m = re.match(r'^([A-Za-z_][A-Za-z0-9_]*):', s)
    if m and not re.match(r'(LBB|CPI|L\d)', m.group(1)):
        cur = m.group(1); calls.setdefault(cur, set()); pushes.setdefault(cur, 0); continue
    if cur is None: continue
    m = re.match(r'SAVE\s+(\d+)', s)
    if m: frame[cur] = int(m.group(1))
    m = re.match(r'EJSR\s+([A-Za-z_][A-Za-z0-9_]*)', s)
    if m: calls[cur].add(m.group(1))
    if re.match(r'(JSR|JMP)\s+@', s) and 'SLOT' not in s:
        print(f'warning: indirect call/jump in {cur}: {s}', file=sys.stderr)
pushcount = {}
cur = None; run = 0
for l in L:
    s = l.split('//')[0].strip()
    m = re.match(r'^([A-Za-z_][A-Za-z0-9_]*):', s)
    if m and not re.match(r'(LBB|CPI|L\d)', m.group(1)):
        cur = m.group(1); run = 0; pushcount[cur] = 0; continue
    if cur is None: continue
    if re.match(r'ISZ\s+040', s): run += 1; pushcount[cur] = max(pushcount[cur], run)
    if re.match(r'DSZ\s+040', s): run = max(0, run - 1)
memo, onpath = {}, set()
def depth(f):
    if f in memo: return memo[f]
    if f in onpath: raise SystemExit(f'recursion through {f}')
    onpath.add(f)
    own = (frame.get(f, 0) + 5 if f in frame else 0) + pushcount.get(f, 0)
    best, via = 0, []
    for c in calls.get(f, ()):
        d, p = depth(c)
        if d > best: best, via = d, p
    onpath.discard(f)
    memo[f] = (own + best, [f] + via)
    return memo[f]
for r in roots:
    d, p = depth(r)
    print(f'{r}: worst case {d} words')
    print('  ' + ' -> '.join(f'{x}({frame.get(x, 0)})' for x in p))
