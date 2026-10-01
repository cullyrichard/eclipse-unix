#!/usr/bin/env python3
"""edtest.py -- drive an interactive session that mixes shell commands
("$ " prompt) and ed commands (ed started with -p 'ed* '), via SIMH
expect/send. Lines in the script: '$ cmd' = shell command, ': cmd' = ed
command (sent at the "ed* " prompt -- a string the kernel never prints;
SIMH keeps every expect rule armed, so ": " fired on "kernel: boot"),
'> cmd' = BASIC line sent at "READY", '? text' = answer to a BASIC
INPUT prompt ("? "), '= EXPECT<tab>SEND' = raw SIMH strings (escapes
like \003 for ^C, \r) for anything else, e.g. --More-- or a delayed ^C,
'| text' = a line of ed input text (sent together with the
following lines up to the next non-'|' line, which is typically '.').
usage: edtest.py script [OUT dir]   -- console output on stdout"""
import os, subprocess, sys
here = os.path.dirname(os.path.abspath(__file__))
out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(here, 'out')
lines = open(sys.argv[1]).read().splitlines()
cmds = [f'attach dsk {out}/disk.img'] + open(f'{out}/kernel.simh').read().splitlines() + ['dep PC 50']
def esc(t): return t.replace('\\', '\\\\').replace('"', '\\"')
i = 0
while i < len(lines):
    l = lines[i]
    prompts = {'$ ': '$ ', ': ': 'ed* ', '> ': 'READY', '? ': '? '}
    if l[:2] in prompts:
        prompt = prompts[l[:2]]
        text = esc(l[2:]) + '\\r'
        i += 1
        while i < len(lines) and lines[i].startswith('| '):
            text += esc(lines[i][2:]) + '\\r'
            i += 1
        # after=: let the program finish printing before "typing" (the
        # console echo would otherwise interleave with its output)
        cmds.append(f'expect "{prompt}" send after=150000,"{text}"; continue')
    elif l.startswith('= '):
        exp, snd = l[2:].split('\t', 1)
        cmds.append(f'expect "{exp}" send after=1000000,"{snd}"; continue')
        i += 1
    else:
        i += 1
cmds.append('go')
p = subprocess.Popen([os.path.expanduser('~/dev/simh-src/BIN/eclipse')], stdin=subprocess.PIPE,
                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
try:
    o, _ = p.communicate('\n'.join(cmds) + '\n', timeout=int(os.environ.get('TIMEOUT', '900')))
except subprocess.TimeoutExpired:
    p.kill()
    o, _ = p.communicate()
    o += '\n[edtest: TIMEOUT]\n'
sys.stdout.write(o.replace('\r', ''))
