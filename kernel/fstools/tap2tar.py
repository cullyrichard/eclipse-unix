#!/usr/bin/env python3
"""tap2tar.py -- SIMH .tap written by the Eclipse tar (kernel/user/tar.c)
-> ordinary .tar file: the records of the first tape file, concatenated.
usage: tap2tar.py in.tap out.tar   (then e.g. `tar -tvf out.tar`)"""
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'eclipse-toolchain', 'tape'))
import mktape
files, _eot, _rest = mktape.split_tape_files(mktape.parse_tap(sys.argv[1]))
recs = files[0][0] if files else []
open(sys.argv[2], 'wb').write(b''.join(recs))
print(f'tap2tar: {sys.argv[2]}: {len(recs)} records')
