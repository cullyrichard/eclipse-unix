#!/usr/bin/env bash
# run-c-testsuite.sh -- run ~/dev/c-testsuite's single-exec tests through
# eclipse-cc + SIMH and print one "PASS|FAIL|CCFAIL|TIMEOUT <test>" line
# per test (sorted), so two runs can be diffed test-by-test.
#
# usage: run-c-testsuite.sh [eclipse-cc flags...] > results.txt
#   env: SUITE (default ~/dev/c-testsuite), JOBS (default 8),
#        LLVM_BUILD (passed through to eclipse-cc)
#
# A test passes when the program's console output (SIMH banner and the
# final HALT line stripped) matches NNNNN.c.expected and the run reached
# a HALT (the compiled _start HALTs after main returns). Replaces the
# earlier, no-longer-present CC_SCRIPT runner the DEBUGGING_NOTES.md
# "197/220" figures came from; its exact pass criteria are not known, so
# compare runs of THIS script against each other, not against 197.
set -uo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
suite="${SUITE:-$HOME/dev/c-testsuite}"
export ECC="$here/eclipse-cc" ECC_FLAGS="$*"
export PATH="$HOME/dev/dgasm-src:$PATH"

one() {
  t="$1"; n="$(basename "$t" .c)"
  w="$(mktemp -d)"
  # shellcheck disable=SC2086
  if ! "$ECC" $ECC_FLAGS -o "$w/a.simh" "$t" >"$w/cc.log" 2>&1; then
    echo "CCFAIL $n"; rm -rf "$w"; return
  fi
  { cat "$w/a.simh"; echo 'dep PC 50'; echo 'go'; } \
    | timeout 60 "$HOME/dev/simh-src/BIN/eclipse" >"$w/run.log" 2>&1
  if ! grep -q '^HALT instruction' "$w/run.log"; then
    echo "TIMEOUT $n"; rm -rf "$w"; return
  fi
  sed -e '1,/^ECLIPSE simulator/d' -e '/^HALT instruction/,$d' "$w/run.log" \
    | sed -e '1{/^$/d}' >"$w/out.txt"
  # the console echoes \r\n; expected files use \n
  tr -d '\r' <"$w/out.txt" | sed -e '${/^$/d}' >"$w/out2.txt"
  if diff -q "$w/out2.txt" "$t.expected" >/dev/null 2>&1; then
    echo "PASS $n"
  else
    echo "FAIL $n"
  fi
  rm -rf "$w"
}
export -f one
ls "$suite"/tests/single-exec/*.c | xargs -P "${JOBS:-8}" -I{} bash -c 'one {}' | sort -k2
