#!/usr/bin/env bash
set -uo pipefail
cd "$(dirname "$0")/.."
fail=0
for t in tests/test_*.py; do
  printf '%-36s ' "$(basename "$t")"
  out=$(python3 "$t" 2>&1)
  rc=$?
  echo "$out" | tail -1
  if [ $rc -ne 0 ]; then
    fail=1
    echo "$out" | grep -E '^\s*(FAIL|FAIL:)' | head -20
  fi
done
for t in tests/test_*.cjs; do
  [ -e "$t" ] || continue
  printf '%-36s ' "$(basename "$t")"
  out=$(node "$t" 2>&1); rc=$?
  echo "$out" | tail -1
  [ $rc -ne 0 ] && fail=1
done
echo
[ $fail -eq 0 ] && echo "ALL SUITES PASS" || echo "SOME SUITES FAILED"
exit $fail
