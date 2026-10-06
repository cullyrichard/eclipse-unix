#!/usr/bin/env python3
"""pastetest.py -- paste a 20 KB, 400-line text into `edit` in one burst
through a pty (as a terminal paste would arrive), save it, and have the
Eclipse's own cmp compare it with a reference copy put on the disk at
build time. Also prints the kernel's dropped-character count (tty_drop).
usage: pastetest.py   (builds out/ itself, with /ref.txt added)"""
import os, subprocess, sys, tempfile
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
from vt import Session
os.chdir(here)
ref = ''.join('%03d: pack my box with five dozen liquor jugs %s\n'
              % (i, 'abcdefghij'[:i % 11]) for i in range(1, 401))
refpath = os.path.join(tempfile.mkdtemp(), 'ref.txt')
open(refpath, 'w').write(ref)
subprocess.run(['./build.sh'], env=dict(os.environ, EXTRA=refpath + ':/ref.txt'),
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
s = Session(['./console.sh'])
s.until(b'$ ', 60)
s.type('edit p.txt\r', 5)
os.write(s.fd, ref.replace('\n', '\r').encode())   # a terminal sends CR per line
s.pump(120)
s.type('\x0f\r', 6); s.type('\x18', 4)
s.type('wc p.txt /ref.txt ; cmp p.txt /ref.txt ; echo cmp status $?\r', 10)
print(s.raw.decode('latin-1').replace('\r', '').split('wc p.txt /ref.txt ;')[-1][:300])
s.type('exit 0\r', 4)
print([l for l in s.raw.decode('latin-1').replace('\r', '').split('\n') if 'tty_drop' in l])
