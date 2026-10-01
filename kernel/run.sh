#!/usr/bin/env bash
# run.sh -- boot out/kernel.simh in SIMH with out/disk.img attached as DSK
# unit 0; console output on stdout. TIMEOUT (seconds, default 600).
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="${OUT:-$here/out}"
{ case "$(cat "$out/kdisk" 2>/dev/null || echo dsk)" in
    dkp) echo "set dkp0 4231"; echo "attach dkp0 $out/disk.img" ;;
    *)   echo "attach dsk $out/disk.img" ;;
  esac; cat "$out/kernel.simh"; echo 'dep PC 50'; echo 'go'; } \
  | timeout "${TIMEOUT:-600}" "$HOME/dev/simh-src/BIN/eclipse" 2>&1 | tr -d '\r'
