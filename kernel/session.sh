#!/usr/bin/env bash
# session.sh -- boot out/disk.img and type the lines of a command file at
# the shell, one per "$ " prompt, via SIMH expect/send (SHELL_NOTES.md).
# usage: session.sh cmds.txt    (console output on stdout)
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="${OUT:-$here/out}"
{
  case "$(cat "$out/kdisk" 2>/dev/null || echo dsk)" in
    dkp) echo "set dkp0 4231"; echo "attach dkp0 $out/disk.img" ;;
    *)   echo "attach dsk $out/disk.img" ;;
  esac
  cat "$out/kernel.simh"
  echo 'dep PC 50'
  while IFS= read -r line; do
    esc=${line//\\/\\\\}; esc=${esc//\"/\\\"}
    echo "expect \"\$ \" send \"$esc\\r\"; continue"
  done < "$1"
  echo 'go'
} | timeout "${TIMEOUT:-600}" "$HOME/dev/simh-src/BIN/eclipse" 2>&1 | tr -d '\r'
