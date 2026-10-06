"""vt.py -- drive kernel/console.sh through a pty and keep a 24x80 VT100
screen model of the output (CUP, EL, ED, SGR, CR, LF, BS)."""
import os, pty, select, time, re, sys
class VT:
    def __init__(s, R=24, C=80):
        s.R, s.C = R, C; s.scr = [[' ']*C for _ in range(R)]; s.rev = [[0]*C for _ in range(R)]
        s.r = s.c = 0; s.a = 0; s.st = ''; s.esc = None; s.bytes = 0
    def feed(s, data):
        for ch in data.decode('latin-1'):
            s.bytes += 1
            if s.esc is not None:
                s.esc += ch
                if s.esc == '[' : continue
                if s.esc[0] != '[': s.esc = None; continue
                if ch.isalpha() or ch == '~':
                    s.csi(s.esc[1:-1], ch); s.esc = None
                continue
            if ch == '\033': s.esc = ''; continue
            if ch == '\r': s.c = 0
            elif ch == '\n':
                s.r += 1
                if s.r >= s.R: s.scr.pop(0); s.scr.append([' ']*s.C); s.rev.pop(0); s.rev.append([0]*s.C); s.r = s.R-1
            elif ch == '\b': s.c = max(0, s.c-1)
            elif ch >= ' ':
                if s.c < s.C: s.scr[s.r][s.c] = ch; s.rev[s.r][s.c] = s.a
                s.c = min(s.c+1, s.C-1)
    def csi(s, args, f):
        p = [int(x) if x else 0 for x in args.split(';')] if args else []
        if f == 'H': s.r = (p[0] if p else 1)-1; s.c = (p[1] if len(p)>1 else 1)-1
        elif f == 'K':
            for c in range(s.c, s.C): s.scr[s.r][c] = ' '; s.rev[s.r][c] = 0
        elif f == 'J' and p and p[0] == 2: s.scr = [[' ']*s.C for _ in range(s.R)]; s.rev = [[0]*s.C for _ in range(s.R)]
        elif f == 'm': s.a = 1 if p and p[0] == 7 else 0
    def dump(s):
        out = []
        for r in range(s.R):
            line = ''.join(s.scr[r]).rstrip()
            out.append(f'{r:2d}|{line}' + ('   <rev>' if any(s.rev[r]) else ''))
        out.append(f'cursor row {s.r} col {s.c}')
        return '\n'.join(out)
class Session:
    def __init__(s, cmd):
        s.pid, s.fd = pty.fork()
        if s.pid == 0: os.execv(cmd[0], cmd)
        s.vt = VT(); s.raw = b''
    def pump(s, t):
        end = time.time()+t
        while time.time() < end:
            r,_,_ = select.select([s.fd],[],[],0.2)
            if r:
                try: d = os.read(s.fd, 4096)
                except OSError: return
                s.raw += d; s.vt.feed(d)
    def until(s, tok, t=60):
        end = time.time()+t
        while time.time() < end:
            if tok in s.raw: return True
            s.pump(0.3)
        return False
    def type(s, text, settle=1.5):
        for ch in text: os.write(s.fd, ch.encode('latin-1')); time.sleep(0.03)
        s.pump(settle)
