#!/usr/bin/env python3
"""io_sweep.py -- IO_NOTES.md's deterministic interrupt-injection sweep.

Boots out/kernel.simh on a copy of out/sweep.img (init = sweepinit: worker
+ fswriter + ttyread) and, for each offset k, steps k instructions, injects
a TTI character 'x' (BUF/DONE/INT deposited directly, the same SCP
technique as examples/syscall_race_sweep.py), steps GAP more, injects CR,
then runs to the end. Records where each injection landed (user mode /
kernel / idle loop, ION on/off) and checks:
  - ttyread got exactly "x\\n" (2 bytes)
  - every child exited 0 (worker's register/marker check, fswriter's
    read-back check), SWEEP bad=0, no PANIC, clean shutdown
  - fsck.py reports 0 problems on the disk the kernel left.
usage: io_sweep.py START STOP STEP [GAP]
With LAT=1 in the environment, also measures how many instructions the
injected 'x' waits before the kernel reads it (TTI Done cleared), in
steps of 100: the input latency. GAP shorter than that latency makes the
CR overwrite the 'x' in TTI's one-character buffer (overrun).
"""
import os, re, shutil, subprocess, sys, tempfile
here = os.path.dirname(os.path.abspath(__file__))
out = os.path.join(here, 'out')
start, stop, stride = (int(a) for a in sys.argv[1:4])
gap = int(sys.argv[4]) if len(sys.argv) > 4 else 777
kern = open(os.path.join(out, 'kernel.simh')).read()
idle_lo = idle_hi = None
import importlib.util
spec = importlib.util.spec_from_file_location('r', os.path.join(here, '..', 'eclipse-toolchain', 'reorder_asm.py'))
r = importlib.util.module_from_spec(spec); spec.loader.exec_module(r)
la, _, _ = r.compute_addresses(open(os.path.join(out, 'kernel.s')).read().splitlines())
idle_lo, idle_hi = la['k_idle'], la['k_stkflt']
nbad = 0
where = {}
for k in range(start, stop, stride):
    d = tempfile.mkdtemp()
    img = os.path.join(d, 'd.img')
    shutil.copy(os.path.join(out, 'sweep.img'), img)
    inj = lambda ch: f'dep TTI BUF {ch:o}\ndep TTI DONE 1\ndep TTI INT 1\n'
    cmds = (f'attach dsk {img}\n' + kern + 'reset\ndep PC 50\n'
            f'step {k}\ne PC\ne ACTIVE\ne ION\n' + inj(ord('x')) +
            (''.join('step 100\nex tti done\n' for _ in range(gap // 100))
             if os.environ.get('LAT') else f'step {gap}\n') +
            inj(13) + 'go\nquit\n')
    o = subprocess.run([os.path.expanduser('~/dev/simh-src/BIN/eclipse')], input=cmds,
                       capture_output=True, text=True, timeout=600).stdout.replace('\r', '')
    pc = int(re.search(r'^PC:\s+([0-7]+)', o, re.M).group(1), 8)
    act = int(re.search(r'^ACTIVE:\s+([0-7]+)', o, re.M).group(1), 8)
    ion = int(re.search(r'^ION:\s+([0-7]+)', o, re.M).group(1), 8)
    loc = 'user' if act else ('idle' if idle_lo <= pc < idle_hi else 'kernel')
    loc += '/ION' if ion else '/noION'
    lat = ''
    if os.environ.get('LAT'):
        dn = re.findall(r'^DONE:\s+([01])', o, re.M)
        lat = ' lat=' + (str(100 * (dn.index('0') + 1)) if '0' in dn else f'>{gap}')
    where[loc] = where.get(loc, 0) + 1
    fk = subprocess.run(['python3', os.path.join(here, 'fstools', 'fsck.py'), img],
                        capture_output=True, text=True)
    probs = []
    if '<ttyread: got 2 bytes: x' not in o: probs.append('ttyread')
    if '<SWEEP bad=0>' not in o: probs.append('children')
    if 'PANIC' in o: probs.append('PANIC')
    if 'no runnable processes' not in o: probs.append('no-shutdown')
    if fk.returncode != 0: probs.append('fsck')
    nbad += bool(probs)
    print(f'k={k:7d} pc={pc:06o} {loc:12s} {"OK" if not probs else "BAD " + ",".join(probs)}{lat}', flush=True)
    shutil.rmtree(d)
print('where:', where)
print('bad runs:', nbad)
