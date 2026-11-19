#!/bin/sh
# tests/run.sh - build gander if needed, then run every suite.
set -u
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/.." && pwd)
cd "$root" || exit 1

if [ ! -x ./gander ]; then
  echo "building gander..."
  make -s || {
    echo "build failed"
    exit 1
  }
fi
GANDER_BIN="$root/gander"
export GANDER_BIN

failed=0
for suite in cli http md watch install; do
  printf '\n== %s ==\n' "$suite"
  if sh "$here/$suite.sh"; then
    :
  else
    failed=1
  fi
done

if [ "$failed" -eq 0 ]; then
  printf '\nAll tests passed.\n'
else
  printf '\nSome tests FAILED.\n'
fi
exit "$failed"
