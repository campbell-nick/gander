#!/bin/sh
# tests/watch.sh - the portable polling live-reload backend.
#
# Linux builds inotify by default; this builds a WATCH=poll binary out of tree
# and proves the scan-and-diff backend reports changes (and honours the commit
# scope). Skipped where curl or a C compiler is missing, or if the build fails.
set -u
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/.." && pwd)
. "$here/lib.sh"

if ! command -v curl >/dev/null 2>&1; then
  echo "curl not found; skipping the polling-backend tests"
  exit 0
fi
if ! command -v "${CC:-cc}" >/dev/null 2>&1; then
  echo "no C compiler; skipping the polling-backend tests"
  exit 0
fi

setup
cd "$root" || exit 1

# The Makefile knob must select each backend.
if make -n -B WATCH=poll all 2>/dev/null | grep -q -- -DGANDER_WATCH_POLL; then
  pass "WATCH=poll selects the polling backend"
else
  fail "WATCH=poll selects the polling backend"
fi
if make -n -B WATCH=inotify all 2>/dev/null | grep -q -- -DGANDER_WATCH_INOTIFY; then
  pass "WATCH=inotify selects the inotify backend"
else
  fail "WATCH=inotify selects the inotify backend"
fi

# Build a polling binary out of tree, leaving the default ./gander untouched.
poll_bin="$TMPROOT/gander-poll"
if ! "${CC:-cc}" -O2 -std=c11 -Wall -Wextra -D_GNU_SOURCE \
  -DGANDER_WATCH_POLL -o "$poll_bin" src/*.c >"$TMPROOT/build.log" 2>&1; then
  echo "could not build the polling backend; skipping"
  cat "$TMPROOT/build.log"
  exit 0
fi
GANDER_BIN="$poll_bin"

repo="$TMPROOT/repo"
make_repo "$repo"

# --- all scope: an edit wakes clients ----------------------------------------

if start_server_in "$repo" "$repo" --watch all; then
  assert_contains "the polling backend names itself in the banner" \
    "$(cat "$SERVER_OUT")" "watch:  poll (all files)"
else
  fail "the polling server starts" "$(cat "$SERVER_OUT")"
fi
base=$(server_base)

curl -s -N --max-time 4 "$base/api/events" >"$TMPROOT/sse1" 2>/dev/null &
sse=$!
sleep 1
printf 'edit\n' >>"$repo/README.md"
sleep 3
wait "$sse" 2>/dev/null
assert_contains "all scope: an edit wakes clients" "$(cat "$TMPROOT/sse1")" \
  "event: change"
stop_server

# --- commit scope: edits are ignored, commits wake clients -------------------

if start_server_in "$repo" "$repo" --watch commit; then
  assert_contains "the commit scope is named in the banner" \
    "$(cat "$SERVER_OUT")" "watch:  poll (commit)"
else
  fail "the commit-scope server starts" "$(cat "$SERVER_OUT")"
fi
base=$(server_base)

curl -s -N --max-time 3 "$base/api/events" >"$TMPROOT/sse2" 2>/dev/null &
sse=$!
sleep 1
printf 'ignored\n' >>"$repo/README.md"
sleep 2
wait "$sse" 2>/dev/null
assert_not_contains "commit scope: an edit does not wake clients" \
  "$(cat "$TMPROOT/sse2")" "event: change"

curl -s -N --max-time 4 "$base/api/events" >"$TMPROOT/sse3" 2>/dev/null &
sse=$!
sleep 1
git -C "$repo" add -A
git -C "$repo" commit -qm "second"
sleep 3
wait "$sse" 2>/dev/null
assert_contains "commit scope: a commit wakes clients" "$(cat "$TMPROOT/sse3")" \
  "event: change"
stop_server

report watch
