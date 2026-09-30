#!/usr/bin/env python3
"""io_boundary.py -- IO_NOTES.md §5.4: inject a TTI interrupt exactly at the
ION-enable boundaries the sweeps can only hit by chance. For each label in
LABELS (k_idle's NIOS, the spin JMP after it, k_resume's NIOS and the EJMP
after it, k_kswitch's EJMP) and for its 1st..Nth execution: run to a SIMH
breakpoint there, inject 'x', remove the breakpoint, step 200000, inject
CR, run to the end; then the same checks as io_sweep.py."""
import importlib.util, os, re, shutil, subprocess, sys, tempfile
here = os.path.dirname(os.path.abspath(__file__)); out = os.path.join(here, 'out')
spec = importlib.util.spec_from_file_location('r', os.path.join(here, '..', 'eclipse-toolchain', 'reorder_asm.py'))
r = importlib.util.module_from_spec(spec); spec.loader.exec_module(r)
lines = open(os.path.join(out, 'kernel.s')).read().splitlines()
la, li, _ = r.compute_addresses(lines)
def after_label(lab, pat):
    """address of the first instruction matching pat after label lab"""
    i = next(j for j, l in enumerate(lines) if l.startswith(lab + ':'))
    for j in range(i + 1, len(lines)):
        if li[j] is not None and re.search(pat, lines[j]): return li[j]
targets = {
    'k_idle NIOS': after_label('k_idle', r'^\s*NIOS 077'),
    'k_idle_spin JMP': la['k_idle_spin'],
    'k_resume NIOS': after_label('k_noload', r'^\s*NIOS 077'),
    'k_resume EJMP': after_label('k_noload', r'^\s*EJMP'),
    'k_kswitch EJMP': after_label('k_kswitch', r'^\s*EJMP'),
}
kern = open(os.path.join(out, 'kernel.simh')).read()
inj = lambda ch: f'dep TTI BUF {ch:o}\ndep TTI DONE 1\ndep TTI INT 1\n'
N = int(sys.argv[1]) if len(sys.argv) > 1 else 5
bad = 0
for name, a in targets.items():
    for nth in range(1, N + 1):
        d = tempfile.mkdtemp(); img = os.path.join(d, 'd.img')
        shutil.copy(os.path.join(out, 'sweep.img'), img)
        cmds = (f'attach dsk {img}\n' + kern + 'reset\ndep PC 50\n'
                # 'x' only after ttyread is waiting (IO_NOTES.md §5.4)
                'step 6600000\n' + f'break {a:o}\n' + 'go\n' * nth +
                f'e PC\nnobreak {a:o}\n' + inj(ord('x')) +
                'step 200000\n' + inj(13) + 'go\nquit\n')
        o = subprocess.run([os.path.expanduser('~/dev/simh-src/BIN/eclipse')], input=cmds,
                           capture_output=True, text=True, timeout=600).stdout.replace('\r', '')
        pcs = re.findall(r'^PC:\s+([0-7]+)', o, re.M)
        fk = subprocess.run(['python3', os.path.join(here, 'fstools', 'fsck.py'), img],
                            capture_output=True, text=True)
        probs = [n for n, bad_ in (('not-at-target', not pcs or int(pcs[-1], 8) != a),
                                   ('ttyread', '<ttyread: got 2 bytes: x' not in o),
                                   ('children', '<SWEEP bad=0>' not in o),
                                   ('PANIC', 'PANIC' in o),
                                   ('no-shutdown', 'no runnable processes' not in o),
                                   ('fsck', fk.returncode != 0)) if bad_]
        bad += bool(probs)
        print(f'{name:16s} @{a:06o} hit #{nth}: {"OK" if not probs else "BAD " + ",".join(probs)}', flush=True)
        shutil.rmtree(d)
print('bad runs:', bad)
