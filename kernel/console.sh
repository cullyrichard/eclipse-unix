#!/usr/bin/env bash
# console.sh -- boot the built system in SIMH interactively: your terminal
# is the Eclipse console. (run.sh and session.sh pipe their commands into
# SIMH, so they can't take keyboard input.)
#   Type at the "$ " prompt; "exit" in the login shell halts the machine.
#   Ctrl-E drops to the sim> prompt ("cont" resumes, "quit" leaves).
# The disk image keeps its contents between runs; ./build.sh makes a
# fresh one.
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="${OUT:-$here/out}"
ini="$(mktemp --suffix=.ini)"
trap 'rm -f "$ini"' EXIT
{ case "$(cat "$out/kdisk" 2>/dev/null || echo dsk)" in
    dkp) echo "set dkp0 4231"; echo "attach dkp0 $out/disk.img" ;;
    ram) echo "attach mta0 $out/install.tap" ;;    # no disk: FS comes from tape
    *)   echo "attach dsk $out/disk.img" ;;
  esac
  cat "$out/kernel.simh"
  echo 'dep PC 50'
  echo 'go'
  echo 'quit'                                       # after the kernel halts
} > "$ini"
"$HOME/dev/simh-src/BIN/eclipse" "$ini"
