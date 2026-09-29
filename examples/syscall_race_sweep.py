# syscall_race_sweep.py -- SYSCALL_NOTES.md section 7's verification of
# the SYC-vs-scheduler race fix (commit 776fbe7).
#
# For k = 1..130: load syscall_sched_race_probe.s, step 46 (A mid-user_loop),
# inject a PIT interrupt (ordinary preemption A->B), step k more, inject a
# SECOND PIT interrupt wherever B now is (user_loop, SYC, __syscall_handler,
# the dispatch stand-in), then run to the probe's own 6th-syscall HALT and
# check both TCBs: ac2 == own step, ac1 == seed + step*iterations (one step
# behind if saved at 0113, between INC 3,3 and ADD 2,1), saved PC inside
# user_loop, saved MapStat == 1 (A) / 5 (B).
#
# usage: python3 syscall_race_sweep.py <probe.simh> <tcbA_ac0 addr, octal>
#   post-fix probe: tcbA_ac0 = 532; pre-fix (git show 73e8f7e:...) = 530.
# ULO/UHI (user_loop's span) are the same in both builds.
import subprocess, sys, re
simh, A = sys.argv[1], int(sys.argv[2], 8)
B = A + 6
ULO, UHI = 0o112, 0o130   # user_loop span (checked below)
def run(k):
    cmds = open(simh).read() + f"""dep PC 50
step 46
dep PIT DONE 1
dep PIT INT 1
step {k}
e PC
dep PIT DONE 1
dep PIT INT 1
step 5000
e {A:o}-{B+5:o}
quit
"""
    out = subprocess.run([sys.argv[3] if len(sys.argv)>3 else '/home/cully/dev/simh-src/BIN/eclipse'],
                         input=cmds, capture_output=True, text=True).stdout
    pc = re.findall(r'^PC:\s+(\d+)', out, re.M)[0]
    stop = re.findall(r'^(HALT[^\n]*|Step expired[^\n]*)', out, re.M)[-1]
    mem = {int(a,8): int(v,8) for a, v in re.findall(r'^([0-7]+):\s+([0-7]+)', out, re.M)}
    bad = []
    for name, base, seed, step, ms in (('A', A, 2000, 3, 1), ('B', B, 6000, 7, 5)):
        ac1, ac2, ac3, pcv, mst = (mem[base+i] for i in (1, 2, 3, 4, 5))
        if ac2 != step: bad.append(f'{name}.ac2={ac2}')
        n = ac3 - (1 if pcv == 0o113 else 0)  # INC 3,3 done, ADD 2,1 not yet
        if ac1 != (seed + step*n) & 0xffff: bad.append(f'{name}.ac1={ac1}!={seed}+{step}*{ac3}')
        if not (ULO <= pcv < UHI): bad.append(f'{name}.pc={pcv:o}')
        if mst != ms: bad.append(f'{name}.mapstat={mst:o}')
        if name=='B': biters=ac3
    return pc, stop.split(',')[0], mem[A+3], mem[B+3], bad
nbad = 0
for k in range(1, 131):
    pc, stop, ia, ib, bad = run(k)
    nbad += bool(bad)
    print(f'k={k:3d} inj@{pc} {stop:14s} itA={ia} itB={ib} {"OK" if not bad else "CORRUPT "+" ".join(bad)}')
print('corrupt runs:', nbad)
