#!/usr/bin/env bash
# run.sh -- boot out/kernel.simh in SIMH with out/disk.img attached as DSK
# unit 0; console output on stdout. TIMEOUT (seconds, default 600).
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="${OUT:-$here/out}"
{ echo "attach dsk $out/disk.img"; cat "$out/kernel.simh"; echo 'dep PC 50'; echo 'go'; } \
  | timeout "${TIMEOUT:-600}" "$HOME/dev/simh-src/BIN/eclipse" 2>&1 | tr -d '\r'
