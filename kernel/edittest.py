#!/usr/bin/env python3
"""edittest.py -- the screen editor (user/edit.c) in SIMH, through a real
pty, with the terminal's screen reconstructed by vt.py. Prints the
screen after each step; run after ./build.sh (it boots out/)."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vt import Session
os.chdir(os.path.dirname(os.path.abspath(__file__)))
s = Session(['./console.sh'])
s.until(b'$ ', 60)
def show(t, rows=None):
    print('=== ' + t); d = s.vt.dump().split('\n')
    print('\n'.join(d if rows is None else [d[r] for r in rows] + [d[-1]]))
s.type('ls -l /bin > big\r', 4)    # one command per line: the console holds 127 chars
s.type('echo ' + 'x' * 50 + 'END-OF-LONG-LINE-' + 'y' * 40 + ' >> big\r', 4)
s.type('wc big\r', 4)
s.type('edit big\r', 5); show('open')
n = s.vt.bytes; s.type('\x0e', 1.0); print('bytes, one cursor-down:', s.vt.bytes - n)
n = s.vt.bytes; s.type('Z', 1.0); print('bytes, first char (title gets Modified):', s.vt.bytes - n)
n = s.vt.bytes; s.type('Z', 1.0); print('bytes, next char:', s.vt.bytes - n)
s.type('\x16', 3); show('^V page down', rows=[1, 2, 20])
s.type('\x1f999\r\x10\x05', 3); show('^_ 999, ^P, ^E: last (long) line, scrolled sideways')
s.type('\x1b[H', 2); show('Home key', rows=[1, 19])
s.type('\x1f1\r\x0b\x0b\x1b[B\x1b[B\x15', 3); show('cut 2, down 2, paste', rows=[0, 1, 2, 3, 4, 5])
s.type('\x17WC\r', 3); show('^W WC', rows=[21])
s.type('\x03', 2); show('^C', rows=[21])
s.type('\x07', 3); show('^G help', rows=[0, 2, 3, 4]); s.type(' ', 3)
s.type('\x0f\r', 3); show('^O write', rows=[0, 21])
s.type('\x18', 3); show('^X exit -> shell', rows=[0, 1])
s.type('head -6 big ; wc big\r', 6)
print(s.raw.decode('latin-1').replace('\r', '').split('head -6 big')[-1][:400])
s.type('exit 0\r', 4)
print(s.raw.decode('latin-1').replace('\r', '')[-300:])
