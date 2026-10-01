#!/usr/bin/env python3
"""tar2tap.py -- an ordinary .tar file -> SIMH .tap image for the Eclipse
tar (kernel/user/tar.c): one 512-byte record per tar block, then two tape
marks. usage: tar2tap.py in.tar out.tap   (make in.tar with
`tar --format=ustar -cf in.tar ...`; files must be <= 65535 bytes)"""
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'eclipse-toolchain', 'tape'))
import mktape
data = open(sys.argv[1], 'rb').read()
data += b'\0' * (-len(data) % 512)
recs = [data[i:i + 512] for i in range(0, len(data), 512)]
mktape.write_tap(sys.argv[2], recs + [None, None])
print(f'tar2tap: {sys.argv[2]}: {len(recs)} records')
